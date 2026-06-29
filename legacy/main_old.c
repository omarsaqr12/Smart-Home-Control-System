#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_rom_sys.h"
#include "lwip/sockets.h"
#include "soc/gpio_reg.h"

// ── WiFi credentials ──────────────────────────────
#define WIFI_SSID       "your-network-name"
#define WIFI_PASS       "your-wifi-password"
#define WIFI_MAX_RETRY  10

// ── MQTT broker ───────────────────────────────────
// Configure these for your local Mosquitto broker.
#define MQTT_HOST       "192.168.1.10"     // broker IP on your LAN
#define MQTT_PORT       1883
#define MQTT_USER       "your-mqtt-user"
#define MQTT_PASS       "your-mqtt-password"
#define MQTT_CLIENT_ID  "ESP32-SmartHome"
#define MQTT_KEEPALIVE  60

// ── MQTT topics ───────────────────────────────────
#define TOPIC_TEMP      "smarthome/room001/temperature"
#define TOPIC_HUM       "smarthome/room001/humidity"
#define TOPIC_ROOM      "smarthome/room001/room"
#define TOPIC_COMMAND   "smarthome/room001/command"

// ── Pin definitions ───────────────────────────────
#define DHT_PIN         GPIO_NUM_17
#define PIR_PIN         GPIO_NUM_20
#define IR_TX_PIN       GPIO_NUM_25
#define IR_RX_PIN       GPIO_NUM_22

// ── IR carrier ────────────────────────────────────
#define IR_CARRIER_HZ    38000
#define LEDC_TIMER       LEDC_TIMER_0
#define LEDC_MODE        LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL     LEDC_CHANNEL_0
#define LEDC_DUTY_RES    LEDC_TIMER_10_BIT
#define LEDC_DUTY        512

// ── IR beam debounce ──────────────────────────────
#define POLL_PERIOD_MS   10
#define DEBOUNCE_SAMPLES 5

static const char *TAG = "SmartHome";

// ── WiFi ──────────────────────────────────────────
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int wifi_retry_count = 0;

// ── MQTT state ────────────────────────────────────
static int               mqtt_sock = -1;
static SemaphoreHandle_t mqtt_tx_mutex = NULL;
static volatile bool     mqtt_connected = false;
static volatile bool     room_occupied  = false;

// ── IR beam state ─────────────────────────────────
static bool ir_beam_broken = false;
static int  ir_high_run    = 0;
static int  ir_low_run     = 0;
static int  ir_crossings   = 0;

// ─────────────────────────────────────────────────
// DHT22 bit-bang driver
// Uses direct register access (same as Arduino) to avoid ESP-IDF HAL overhead
// that would break µs-level timing.  Pin must be 0-31.
// Pull-up is configured once in app_main; direction is toggled here via OE bit.
// ─────────────────────────────────────────────────
#define _DHT_BIT(p)   (1UL << (p))
#define DHT_READ(p)   (((REG_READ(GPIO_IN_REG))  >> (p)) & 1U)
#define DHT_HIGH(p)   REG_WRITE(GPIO_OUT_W1TS_REG,  _DHT_BIT(p))
#define DHT_LOW(p)    REG_WRITE(GPIO_OUT_W1TC_REG,  _DHT_BIT(p))
#define DHT_OUTPUT(p) REG_WRITE(GPIO_ENABLE_W1TS_REG, _DHT_BIT(p))
#define DHT_INPUT(p)  REG_WRITE(GPIO_ENABLE_W1TC_REG, _DHT_BIT(p))

typedef struct { float temperature; float humidity; bool valid; } dht_data_t;

static dht_data_t dht_read(int pin)
{
    dht_data_t result = {0.0f, 0.0f, false};
    uint8_t data[5] = {0};

    DHT_OUTPUT(pin);
    DHT_LOW(pin);
    vTaskDelay(pdMS_TO_TICKS(20));

    portDISABLE_INTERRUPTS();
    DHT_HIGH(pin);
    esp_rom_delay_us(40);
    DHT_INPUT(pin);

    int t = 0;
    while (DHT_READ(pin) == 1) { esp_rom_delay_us(1); if (++t > 200) goto done; }
    t = 0;
    while (DHT_READ(pin) == 0) { esp_rom_delay_us(1); if (++t > 200) goto done; }
    t = 0;
    while (DHT_READ(pin) == 1) { esp_rom_delay_us(1); if (++t > 200) goto done; }

    for (int i = 0; i < 40; i++) {
        t = 0;
        while (DHT_READ(pin) == 0) { esp_rom_delay_us(1); if (++t > 100) goto done; }
        esp_rom_delay_us(35);
        if (DHT_READ(pin) == 1) data[i / 8] |= (1U << (7 - (i % 8)));
        t = 0;
        while (DHT_READ(pin) == 1) { esp_rom_delay_us(1); if (++t > 150) goto done; }
    }

    if ((uint8_t)(data[0] + data[1] + data[2] + data[3]) != data[4]) goto done;
    result.humidity    = ((data[0] << 8) | data[1]) * 0.1f;
    result.temperature = (((data[2] & 0x7F) << 8) | data[3]) * 0.1f;
    if (data[2] & 0x80) result.temperature = -result.temperature;
    result.valid = true;

done:
    portENABLE_INTERRUPTS();
    DHT_OUTPUT(pin);
    DHT_HIGH(pin);
    return result;
}

// ─────────────────────────────────────────────────
// IR break beam
// ─────────────────────────────────────────────────
static void ir_carrier_start(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_MODE,
        .timer_num       = LEDC_TIMER,
        .duty_resolution = LEDC_DUTY_RES,
        .freq_hz         = IR_CARRIER_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t channel = {
        .speed_mode = LEDC_MODE,
        .channel    = LEDC_CHANNEL,
        .timer_sel  = LEDC_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = IR_TX_PIN,
        .duty       = LEDC_DUTY,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
}

static void ir_rx_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << IR_RX_PIN,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
}

// Poll the IR beam; updates ir_beam_broken and ir_crossings.
// Returns true if the beam state changed this call.
static bool ir_poll(void)
{
    int level = gpio_get_level(IR_RX_PIN);

    if (level) { ir_high_run++; ir_low_run  = 0; }
    else        { ir_low_run++;  ir_high_run = 0; }

    if (!ir_beam_broken && ir_high_run >= DEBOUNCE_SAMPLES) {
        ir_beam_broken = true;
        ir_crossings++;
        ESP_LOGW(TAG, "IR: BEAM BROKEN (crossing #%d)", ir_crossings);
        return true;
    }
    if (ir_beam_broken && ir_low_run >= DEBOUNCE_SAMPLES) {
        ir_beam_broken = false;
        ESP_LOGI(TAG, "IR: Beam restored");
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────
// WiFi
// ─────────────────────────────────────────────────
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *event_data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        mqtt_connected = false;
        if (wifi_retry_count < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            wifi_retry_count++;
            ESP_LOGW(TAG, "WiFi retry %d/%d", wifi_retry_count, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi connected. IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        wifi_retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_scan_and_log(void)
{
    wifi_scan_config_t scan_cfg = { .show_hidden = false };
    esp_wifi_scan_start(&scan_cfg, true);

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count == 0) {
        ESP_LOGI(TAG, "WiFi scan: no networks found");
        return;
    }

    wifi_ap_record_t *records = malloc(count * sizeof(wifi_ap_record_t));
    if (!records) return;

    esp_wifi_scan_get_ap_records(&count, records);
    ESP_LOGI(TAG, "WiFi scan: %d network(s) found:", count);
    for (int i = 0; i < count; i++) {
        ESP_LOGI(TAG, "  [%2d] RSSI %4d  %s", i + 1, records[i].rssi, records[i].ssid);
    }
    free(records);
}

static void wifi_init(void)
{
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_scan_and_log();

    esp_event_handler_instance_t h_any, h_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event_handler, NULL, &h_any));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        wifi_event_handler, NULL, &h_ip));

    wifi_config_t wifi_cfg = { .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS } };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_disconnect());
    ESP_ERROR_CHECK(esp_wifi_connect());
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                        pdFALSE, pdFALSE, portMAX_DELAY);
}

// ─────────────────────────────────────────────────
// Minimal MQTT 3.1.1 over TCP socket
// ─────────────────────────────────────────────────
static int mqtt_encode_len(uint8_t *buf, int len)
{
    int n = 0;
    do {
        uint8_t b = len & 0x7F;
        len >>= 7;
        if (len) b |= 0x80;
        buf[n++] = b;
    } while (len);
    return n;
}

static int sock_write_all(const uint8_t *data, int len)
{
    int sent = 0;
    while (sent < len) {
        int n = send(mqtt_sock, data + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return sent;
}

static int sock_read_all(uint8_t *buf, int len)
{
    int got = 0;
    while (got < len) {
        int n = recv(mqtt_sock, buf + got, len - got, 0);
        if (n <= 0) return -1;
        got += n;
    }
    return got;
}

static int mqtt_recv_packet(uint8_t *buf, int buf_size, int *out_len)
{
    uint8_t hdr;
    if (sock_read_all(&hdr, 1) < 0) return -1;

    int remaining = 0, shift = 0;
    uint8_t b;
    do {
        if (sock_read_all(&b, 1) < 0) return -1;
        remaining |= (b & 0x7F) << shift;
        shift += 7;
    } while (b & 0x80);

    *out_len = remaining;
    if (remaining > 0) {
        if (remaining > buf_size) {
            uint8_t trash[64];
            int left = remaining;
            while (left > 0) {
                int chunk = left < 64 ? left : 64;
                if (sock_read_all(trash, chunk) < 0) return -1;
                left -= chunk;
            }
        } else {
            if (sock_read_all(buf, remaining) < 0) return -1;
        }
    }
    return hdr & 0xF0;
}

static bool mqtt_send_connect_pkt(void)
{
    uint8_t pl[200];
    int plen = 0;

    pl[plen++] = 0x00; pl[plen++] = 0x04;
    pl[plen++] = 'M'; pl[plen++] = 'Q'; pl[plen++] = 'T'; pl[plen++] = 'T';
    pl[plen++] = 0x04;
    pl[plen++] = 0xC2;
    pl[plen++] = (MQTT_KEEPALIVE >> 8) & 0xFF;
    pl[plen++] = MQTT_KEEPALIVE & 0xFF;

    uint16_t id_len = strlen(MQTT_CLIENT_ID);
    pl[plen++] = id_len >> 8; pl[plen++] = id_len & 0xFF;
    memcpy(pl + plen, MQTT_CLIENT_ID, id_len); plen += id_len;

    uint16_t u_len = strlen(MQTT_USER);
    pl[plen++] = u_len >> 8; pl[plen++] = u_len & 0xFF;
    memcpy(pl + plen, MQTT_USER, u_len); plen += u_len;

    uint16_t pw_len = strlen(MQTT_PASS);
    pl[plen++] = pw_len >> 8; pl[plen++] = pw_len & 0xFF;
    memcpy(pl + plen, MQTT_PASS, pw_len); plen += pw_len;

    uint8_t pkt[210];
    int pos = 0;
    pkt[pos++] = 0x10;
    pos += mqtt_encode_len(pkt + pos, plen);
    memcpy(pkt + pos, pl, plen); pos += plen;
    return sock_write_all(pkt, pos) == pos;
}

static bool mqtt_send_subscribe_pkt(const char *topic)
{
    int tlen = strlen(topic);
    uint8_t pkt[128];
    int pos = 0;
    pkt[pos++] = 0x82;
    pos += mqtt_encode_len(pkt + pos, 2 + 2 + tlen + 1);
    pkt[pos++] = 0x00; pkt[pos++] = 0x01;
    pkt[pos++] = tlen >> 8; pkt[pos++] = tlen & 0xFF;
    memcpy(pkt + pos, topic, tlen); pos += tlen;
    pkt[pos++] = 0x00;
    return sock_write_all(pkt, pos) == pos;
}

static void mqtt_send_pingreq(void)
{
    uint8_t pkt[] = {0xC0, 0x00};
    sock_write_all(pkt, 2);
}

static void mqtt_pub(const char *topic, const char *data)
{
    if (!mqtt_connected) return;
    int tlen = strlen(topic);
    int dlen = strlen(data);
    uint8_t pkt[256];
    int pos = 0;
    pkt[pos++] = 0x30;
    pos += mqtt_encode_len(pkt + pos, 2 + tlen + dlen);
    pkt[pos++] = tlen >> 8; pkt[pos++] = tlen & 0xFF;
    memcpy(pkt + pos, topic, tlen); pos += tlen;
    memcpy(pkt + pos, data,  dlen); pos += dlen;

    xSemaphoreTake(mqtt_tx_mutex, portMAX_DELAY);
    sock_write_all(pkt, pos);
    xSemaphoreGive(mqtt_tx_mutex);
}

static void on_command(const char *cmd)
{
    ESP_LOGI(TAG, "Command received: %s", cmd);
    if (strcmp(cmd, "LIGHTS_ON") == 0) {
        ESP_LOGI(TAG, ">>> Lights ON");
    } else if (strcmp(cmd, "LIGHTS_OFF") == 0) {
        ESP_LOGI(TAG, ">>> Lights OFF");
    } else if (strcmp(cmd, "STATUS") == 0) {
        mqtt_pub(TOPIC_ROOM, room_occupied ? "OCCUPIED" : "EMPTY");
    }
}

static void mqtt_recv_task(void *arg)
{
    uint8_t buf[256];
    int pkt_len;

    while (1) {
        int type = mqtt_recv_packet(buf, sizeof(buf), &pkt_len);
        if (type < 0) {
            ESP_LOGW(TAG, "MQTT recv error — will reconnect");
            break;
        }
        if (type == 0x30 && pkt_len >= 2) {
            int tlen = (buf[0] << 8) | buf[1];
            int dlen = pkt_len - 2 - tlen;
            if (dlen > 0 && (2 + tlen + dlen) <= pkt_len) {
                char cmd[128] = {0};
                int copy = dlen < (int)(sizeof(cmd) - 1) ? dlen : (int)(sizeof(cmd) - 1);
                memcpy(cmd, buf + 2 + tlen, copy);
                on_command(cmd);
            }
        }
    }

    mqtt_connected = false;
    close(mqtt_sock);
    mqtt_sock = -1;
    vTaskDelete(NULL);
}

static bool mqtt_connect(void)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(MQTT_PORT),
    };
    inet_aton(MQTT_HOST, &addr.sin_addr);

    mqtt_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (mqtt_sock < 0) { ESP_LOGE(TAG, "socket() failed"); return false; }

    if (connect(mqtt_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "TCP connect failed");
        close(mqtt_sock); mqtt_sock = -1; return false;
    }

    if (!mqtt_send_connect_pkt()) {
        close(mqtt_sock); mqtt_sock = -1; return false;
    }

    uint8_t ack[4] = {0};
    if (sock_read_all(ack, 4) < 0 || ack[0] != 0x20 || ack[3] != 0x00) {
        ESP_LOGE(TAG, "CONNACK failed (rc=0x%02x)", ack[3]);
        close(mqtt_sock); mqtt_sock = -1; return false;
    }

    mqtt_connected = true;
    ESP_LOGI(TAG, "MQTT connected.");

    xSemaphoreTake(mqtt_tx_mutex, portMAX_DELAY);
    mqtt_send_subscribe_pkt(TOPIC_COMMAND);
    xSemaphoreGive(mqtt_tx_mutex);
    ESP_LOGI(TAG, "Subscribed to: %s", TOPIC_COMMAND);

    xTaskCreate(mqtt_recv_task, "mqtt_recv", 4096, NULL, 5, NULL);
    return true;
}

// ─────────────────────────────────────────────────
// Entry point
// ─────────────────────────────────────────────────
void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    mqtt_tx_mutex = xSemaphoreCreateMutex();

    // PIR
    gpio_config_t pir_cfg = {
        .pin_bit_mask = (1ULL << PIR_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&pir_cfg);

    // DHT22
    gpio_set_pull_mode(DHT_PIN, GPIO_PULLUP_ONLY);
    DHT_OUTPUT(DHT_PIN);
    DHT_HIGH(DHT_PIN);

    // IR break beam
    ir_carrier_start();
    ir_rx_init();
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "IR beam armed — TX GPIO%d @ %d Hz, RX GPIO%d",
             IR_TX_PIN, IR_CARRIER_HZ, IR_RX_PIN);

    ESP_LOGI(TAG, "=== Smart Home System ===");
    wifi_init();
    mqtt_connect();

    TickType_t last_dht_tick  = xTaskGetTickCount();
    TickType_t last_ping_tick = xTaskGetTickCount();
    TickType_t last_reconnect = xTaskGetTickCount() - pdMS_TO_TICKS(6000);

    ESP_LOGI(TAG, "System ready.");

    while (1) {
        TickType_t now = xTaskGetTickCount();

        // ── Reconnect if dropped ─────────────────────────────────────────────
        if (!mqtt_connected && (now - last_reconnect) >= pdMS_TO_TICKS(5000)) {
            last_reconnect = now;
            ESP_LOGI(TAG, "Reconnecting MQTT...");
            mqtt_connect();
        }

        // ── Keepalive ping ───────────────────────────────────────────────────
        if (mqtt_connected && (now - last_ping_tick) >= pdMS_TO_TICKS(30000)) {
            last_ping_tick = now;
            xSemaphoreTake(mqtt_tx_mutex, portMAX_DELAY);
            mqtt_send_pingreq();
            xSemaphoreGive(mqtt_tx_mutex);
        }

        // ── IR beam poll ─────────────────────────────────────────────────────
        ir_poll();

        // ── PIR + IR combined occupancy ──────────────────────────────────────
        bool pir_active  = (gpio_get_level(PIR_PIN) == 1);
        bool new_occupied = ir_beam_broken || pir_active;

        if (new_occupied != room_occupied) {
            room_occupied = new_occupied;
            if (room_occupied) {
                const char *src = (ir_beam_broken && pir_active) ? "IR+PIR" :
                                   ir_beam_broken                ? "IR"     : "PIR";
                ESP_LOGI(TAG, ">>> OCCUPIED (via %s)", src);
                mqtt_pub(TOPIC_ROOM, "OCCUPIED");
            } else {
                ESP_LOGI(TAG, "<<< EMPTY");
                mqtt_pub(TOPIC_ROOM, "EMPTY");
            }
        }

        // ── DHT22 every 5 s ──────────────────────────────────────────────────
        if ((now - last_dht_tick) >= pdMS_TO_TICKS(5000)) {
            last_dht_tick = now;
            dht_data_t dht = dht_read(DHT_PIN);
            if (dht.valid) {
                char temp_str[10], hum_str[10];
                snprintf(temp_str, sizeof(temp_str), "%.1f", dht.temperature);
                snprintf(hum_str,  sizeof(hum_str),  "%.1f", dht.humidity);
                mqtt_pub(TOPIC_TEMP, temp_str);
                mqtt_pub(TOPIC_HUM,  hum_str);
                ESP_LOGI(TAG, "Temp: %s C | Hum: %s %% | Room: %s",
                         temp_str, hum_str, room_occupied ? "OCCUPIED" : "EMPTY");
            } else {
                ESP_LOGW(TAG, "DHT22 read failed.");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}
