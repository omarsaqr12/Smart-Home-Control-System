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
#include "esp_tls.h"
#include "esp_http_client.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_rom_sys.h"
#include "soc/gpio_reg.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"
#include "secrets.h"

// ── IR AC Control ─────────────────────────────────────────────────────────────
#define IR_TX_GPIO  GPIO_NUM_19
#define IR_TX_CHANNEL RMT_TX_CHANNEL_0
static rmt_channel_handle_t ir_tx_handle = NULL;
static uint32_t ac_ir_codes[16] = {0};  // Store up to 16 different AC codes

// ── IR Code Capture (temp for learning remote codes) ──────────────────────────
#define IR_CAPTURE_ENABLED 1   // Set to 0 to disable code capture and resume normal operation
#define IR_CAPTURE_PIN IR_OUTER_PIN  // Temporarily use outer receiver to capture codes
#define IR_RX_CHANNEL RMT_RX_CHANNEL_0
static rmt_channel_handle_t ir_rx_handle = NULL;

// ── WiFi / Firebase config ────────────────────────────────────────────────────
#define WIFI_MAX_RETRY  10

#define FIREBASE_BASE                "/smarthome/room001"
static char       fb_id_token[1200] = {0};
static TickType_t fb_token_obtained_at = 0;
#define TOKEN_REFRESH_INTERVAL_MS    (55 * 60 * 1000)

#define PATH_TEMP       FIREBASE_BASE "/temperature.json"
#define PATH_HUM        FIREBASE_BASE "/humidity.json"
#define PATH_ROOM       FIREBASE_BASE "/room.json"
#define PATH_COUNT      FIREBASE_BASE "/count.json"
#define PATH_LIGHT      FIREBASE_BASE "/light.json"
#define PATH_ATOMIZER   FIREBASE_BASE "/atomizer.json"
#define PATH_MUSIC      FIREBASE_BASE "/music.json"
#define PATH_COMMAND    FIREBASE_BASE "/command.json"

// ── Pin definitions ───────────────────────────────────────────────────────────
#define DHT_PIN         GPIO_NUM_26
#define PIR_PIN         GPIO_NUM_25
#define IR_OUTER_PIN    GPIO_NUM_13   // outer receiver (outside the door)
#define IR_INNER_PIN    GPIO_NUM_14   // inner receiver (inside the door)
#define RELAY_PIN       GPIO_NUM_23   // relay IN1 — active LOW
#define ATOMIZER_PIN    GPIO_NUM_21   // ultrasonic atomizer control (HIGH = on)
#define DFPLAYER_TX_PIN GPIO_NUM_17   // ESP32 TX → DFPlayer RX
#define DFPLAYER_UART   UART_NUM_2
#define DFPLAYER_BAUD   9600

// ── Timing ────────────────────────────────────────────────────────────────────
#define POLL_PERIOD_MS          10
#define DEBOUNCE_SAMPLES        5
#define SEQUENCE_TIMEOUT_MS     3000
#define COOLDOWN_MS             2000
#define DHT_INTERVAL_MS         5000
#define COMMAND_POLL_MS         3000    // how often ESP32 checks Firebase for a new command

static const char *TAG = "SmartHome";


// ── Forward declarations ──────────────────────────────────────────────────────
static void dfplayer_play(uint16_t track);
static void dfplayer_stop(void);
static void relay_set(bool on);
static void atomizer_press(void);
static void atomizer_set(bool on);
static void ir_tx_init(void);
static void send_ir_nec(uint32_t addr, uint32_t cmd);
static void handle_ac_command(int temp);
static void ir_rx_init(void);
static bool ir_rx_capture(void);

// ── Global state ──────────────────────────────────────────────────────────────
static volatile bool room_occupied = false;
static int           people_count  = 0;
static bool          light_on      = false;
static bool          atomizer_on   = false;
static bool          music_on      = false;
static bool          fb_ready      = false;   // set true after successful Firebase sign-in

// ── WiFi state ───────────────────────────────────────────────────────────────
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
static int wifi_retry_count = 0;

// ── Directional detection state machine ───────────────────────────────────────
typedef enum {
    DETECT_IDLE,
    DETECT_OUTER_FIRST,
    DETECT_INNER_FIRST,
    DETECT_AWAIT_PIR,
} detect_state_t;

static detect_state_t detect_state      = DETECT_IDLE;
static TickType_t     state_entered_at  = 0;
static TickType_t     last_detection_at = 0;

static bool outer_broken = false, inner_broken = false;
static int  outer_hi = 0, outer_lo = 0;
static int  inner_hi = 0, inner_lo = 0;

// ─────────────────────────────────────────────────────────────────────────────
// IR receivers
// ─────────────────────────────────────────────────────────────────────────────
static void ir_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << IR_OUTER_PIN) | (1ULL << IR_INNER_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
}

static bool debounce_beam(int gpio, int *hi, int *lo, bool *broken)
{
    int level = gpio_get_level(gpio);
    if (level) { (*hi)++; *lo = 0; }
    else        { (*lo)++; *hi = 0; }
    bool prev = *broken;
    if (!*broken && *hi >= DEBOUNCE_SAMPLES) *broken = true;
    if ( *broken && *lo >= DEBOUNCE_SAMPLES) *broken = false;
    return (!prev && *broken);
}

// ─────────────────────────────────────────────────────────────────────────────
// Firebase HTTP helpers
// ─────────────────────────────────────────────────────────────────────────────
static char http_response_buf[8192];
static int  http_response_len = 0;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int copy = evt->data_len;
        if (http_response_len + copy >= (int)sizeof(http_response_buf) - 1)
            copy = sizeof(http_response_buf) - 1 - http_response_len;
        if (copy > 0) {
            memcpy(http_response_buf + http_response_len, evt->data, copy);
            http_response_len += copy;
            http_response_buf[http_response_len] = '\0';
        }
    }
    return ESP_OK;
}

static esp_err_t http_discard_handler(esp_http_client_event_t *evt)
{
    return ESP_OK;
}

static bool firebase_signin_anonymous(void)
{
    char url[256];
    snprintf(url, sizeof(url),
        "https://identitytoolkit.googleapis.com/v1/accounts:signUp?key=%s",
        FIREBASE_API_KEY);

    const char *body = "{\"returnSecureToken\":true}";

    http_response_len = 0;
    http_response_buf[0] = '\0';

    esp_http_client_config_t cfg = {
        .url            = url,
        .method         = HTTP_METHOD_POST,
        .event_handler  = http_event_handler,
        .timeout_ms     = 20000,
        .buffer_size_tx = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_post_field(client, body, strlen(body));
    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "Anonymous sign-in failed: %s (HTTP %d) buf='%.80s'",
                 esp_err_to_name(err), status, http_response_buf);
        return false;
    }

    char *p = strstr(http_response_buf, "\"idToken\":");
    if (!p) {
        ESP_LOGE(TAG, "Sign-in: idToken field not found (len=%d buf='%.80s')",
                 http_response_len, http_response_buf);
        return false;
    }
    p += strlen("\"idToken\":");
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') {
        ESP_LOGE(TAG, "Sign-in: unexpected char after idToken colon: 0x%02x", (unsigned char)*p);
        return false;
    }
    p++;
    char *end = strchr(p, '"');
    if (!end) {
        ESP_LOGE(TAG, "Sign-in: idToken closing quote missing — response truncated?");
        return false;
    }
    int len = end - p;
    if (len >= (int)sizeof(fb_id_token)) {
        ESP_LOGE(TAG, "Sign-in: idToken too long (%d bytes, max %d)", len, (int)sizeof(fb_id_token) - 1);
        return false;
    }
    strncpy(fb_id_token, p, len);
    fb_id_token[len] = '\0';
    fb_token_obtained_at = xTaskGetTickCount();
    ESP_LOGI(TAG, "Firebase: anonymous sign-in OK (token %d bytes).", len);
    return true;
}

static void firebase_put(const char *path, const char *json)
{
    static char url[512];
    snprintf(url, sizeof(url), "https://%s%s?key=%s", FIREBASE_HOST, path, FIREBASE_API_KEY);

    esp_http_client_config_t cfg = {
        .url            = url,
        .method         = HTTP_METHOD_PUT,
        .event_handler  = http_discard_handler,
        .timeout_ms     = 15000,
        .buffer_size_tx = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json, strlen(json));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK && status == 200) {
        ESP_LOGI(TAG, "firebase_put(%s) = %s ✓", path, json);
    } else {
        ESP_LOGW(TAG, "firebase_put(%s) failed: %s (HTTP %d)", path, esp_err_to_name(err), status);
    }
}

static bool firebase_get(const char *path, char *out_buf, int out_size)
{
    static char url[512];
    snprintf(url, sizeof(url), "https://%s%s?key=%s", FIREBASE_HOST, path, FIREBASE_API_KEY);

    http_response_len = 0;
    http_response_buf[0] = '\0';

    esp_http_client_config_t cfg = {
        .url            = url,
        .method         = HTTP_METHOD_GET,
        .event_handler  = http_event_handler,
        .timeout_ms     = 15000,
        .buffer_size_tx = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "firebase_get(%s) failed: %s (HTTP %d)", path, esp_err_to_name(err), status);
        return false;
    }

    strncpy(out_buf, http_response_buf, out_size - 1);
    out_buf[out_size - 1] = '\0';
    if (strlen(out_buf) > 0 && strcmp(out_buf, "null") != 0) {
        ESP_LOGI(TAG, "firebase_get(%s) = %s", path, out_buf);
    }
    return true;
}

static void firebase_delete(const char *path)
{
    static char url[512];
    snprintf(url, sizeof(url), "https://%s%s?key=%s", FIREBASE_HOST, path, FIREBASE_API_KEY);

    esp_http_client_config_t cfg = {
        .url            = url,
        .method         = HTTP_METHOD_DELETE,
        .event_handler  = http_discard_handler,
        .timeout_ms     = 15000,
        .buffer_size_tx = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_perform(client);
    esp_http_client_cleanup(client);
}

// ─────────────────────────────────────────────────────────────────────────────
// Firebase publish helpers — all guard against pre-sign-in calls
// ─────────────────────────────────────────────────────────────────────────────
static void pub_room(const char *status)
{
    if (!fb_ready) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "\"%s\"", status);
    firebase_put(PATH_ROOM, buf);
}

static void pub_count(int count)
{
    if (!fb_ready) return;
    char buf[12];
    snprintf(buf, sizeof(buf), "%d", count);
    firebase_put(PATH_COUNT, buf);
}

static void pub_light(bool on)
{
    if (!fb_ready) return;
    firebase_put(PATH_LIGHT, on ? "\"ON\"" : "\"OFF\"");
}

static void pub_atomizer(bool on)
{
    if (!fb_ready) return;
    firebase_put(PATH_ATOMIZER, on ? "\"ON\"" : "\"OFF\"");
}

static void pub_music(bool on)
{
    if (!fb_ready) return;
    firebase_put(PATH_MUSIC, on ? "\"PLAYING\"" : "\"STOPPED\"");
}

// ─────────────────────────────────────────────────────────────────────────────
// Hardware control — defined after pub_* so they can publish state changes
// ─────────────────────────────────────────────────────────────────────────────
static void relay_set(bool on)
{
    // Active LOW relay: LOW = energised = lamp ON
    gpio_set_level(RELAY_PIN, on ? 0 : 1);
    light_on = on;
    pub_light(on);
    ESP_LOGI("relay", "Lamp %s", on ? "ON" : "OFF");
}

// Low-level toggle pulse — call atomizer_set() for state-aware control
static void atomizer_press(void)
{
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(ATOMIZER_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(ATOMIZER_PIN, 0);
    ESP_LOGI("atomizer", "button pressed");
}

// State-aware toggle: only pulses if actual state differs from desired state
static void atomizer_set(bool on)
{
    if (on == atomizer_on) return;
    atomizer_press();
    atomizer_on = on;
    pub_atomizer(on);
}

// ─────────────────────────────────────────────────────────────────────────────
// Command polling — called from the main loop every COMMAND_POLL_MS
// ─────────────────────────────────────────────────────────────────────────────
static void poll_command(void)
{
    if (!fb_ready) return;

    char raw[128];
    if (!firebase_get(PATH_COMMAND, raw, sizeof(raw))) return;

    // Firebase returns JSON: "LIGHTS_ON" (with quotes) or null
    if (strcmp(raw, "null") == 0 || strlen(raw) < 3) return;

    // Strip surrounding quotes: "LIGHTS_ON" → LIGHTS_ON
    char cmd[64] = {0};
    int len = strlen(raw);
    if (raw[0] == '"' && raw[len - 1] == '"') {
        strncpy(cmd, raw + 1, len - 2);
        cmd[len - 2] = '\0';
    } else {
        strncpy(cmd, raw, sizeof(cmd) - 1);
    }

    ESP_LOGI(TAG, "Command received: %s", cmd);

    if (strcmp(cmd, "LIGHTS_ON") == 0) {
        relay_set(true);
    } else if (strcmp(cmd, "LIGHTS_OFF") == 0) {
        relay_set(false);
    } else if (strcmp(cmd, "ATOMIZER_ON") == 0) {
        atomizer_set(true);
    } else if (strcmp(cmd, "ATOMIZER_OFF") == 0) {
        atomizer_set(false);
    } else if (strcmp(cmd, "MUSIC_ON") == 0) {
        dfplayer_play(1);
        music_on = true;
        pub_music(true);
    } else if (strcmp(cmd, "MUSIC_OFF") == 0) {
        dfplayer_stop();
        music_on = false;
        pub_music(false);
    } else if (strcmp(cmd, "STATUS") == 0) {
        pub_room(room_occupied ? "OCCUPIED" : "EMPTY");
        pub_count(people_count);
        pub_light(light_on);
        pub_atomizer(atomizer_on);
        pub_music(music_on);
    } else if (strncmp(cmd, "AC_SET_TEMP:", 12) == 0) {
        int temp = atoi(cmd + 12);
        handle_ac_command(temp);
    }

    // Clear the command node so it is not processed again
    firebase_delete(PATH_COMMAND);
}

// ─────────────────────────────────────────────────────────────────────────────
// Directional detection (IR + PIR)
// ─────────────────────────────────────────────────────────────────────────────
static void detection_poll(void)
{
    static TickType_t last_debug = 0;
    TickType_t now = xTaskGetTickCount();

    // Debug: log sensor states every 2 seconds
    if ((now - last_debug) >= pdMS_TO_TICKS(2000)) {
        int outer_level = gpio_get_level(IR_OUTER_PIN);
        int inner_level = gpio_get_level(IR_INNER_PIN);
        int pir_level = gpio_get_level(PIR_PIN);
        ESP_LOGI(TAG, "SENSORS: outer=%d inner=%d pir=%d | state=%d",
                 outer_level, inner_level, pir_level, detect_state);
        last_debug = now;
    }

    if ((now - last_detection_at) < pdMS_TO_TICKS(COOLDOWN_MS)) return;

    bool outer_just_broke = debounce_beam(IR_OUTER_PIN, &outer_hi, &outer_lo, &outer_broken);
    bool inner_just_broke = debounce_beam(IR_INNER_PIN, &inner_hi, &inner_lo, &inner_broken);

    switch (detect_state) {

        case DETECT_IDLE:
            if (outer_just_broke) {
                detect_state = DETECT_OUTER_FIRST;
                state_entered_at = now;
                ESP_LOGI(TAG, "Outer beam broke — watching for inner");
            } else if (inner_just_broke) {
                detect_state = DETECT_INNER_FIRST;
                state_entered_at = now;
                ESP_LOGI(TAG, "Inner beam broke — watching for outer");
            }
            break;

        case DETECT_OUTER_FIRST:
            if ((now - state_entered_at) >= pdMS_TO_TICKS(SEQUENCE_TIMEOUT_MS)) {
                ESP_LOGI(TAG, "Outer-first timeout — sequence abandoned");
                detect_state = DETECT_IDLE;
            } else if (inner_just_broke) {
                detect_state = DETECT_AWAIT_PIR;
                state_entered_at = now;
                ESP_LOGI(TAG, "outer→inner — awaiting PIR confirmation");
            }
            break;

        case DETECT_INNER_FIRST:
            if ((now - state_entered_at) >= pdMS_TO_TICKS(SEQUENCE_TIMEOUT_MS)) {
                ESP_LOGI(TAG, "Inner-first timeout — sequence abandoned");
                detect_state = DETECT_IDLE;
            } else if (outer_just_broke) {
                // EXIT confirmed
                people_count = (people_count > 0) ? people_count - 1 : 0;
                pub_count(people_count);
                if (people_count == 0 && room_occupied) {
                    room_occupied = false;
                    pub_room("EMPTY");
                    relay_set(false);
                    dfplayer_stop();
                    music_on = false;
                    pub_music(false);
                    atomizer_set(false);
                }
                ESP_LOGI(TAG, "<<< EXIT (people: %d)", people_count);
                last_detection_at = now;
                detect_state = DETECT_IDLE;
            }
            break;

        case DETECT_AWAIT_PIR:
            if ((now - state_entered_at) >= pdMS_TO_TICKS(SEQUENCE_TIMEOUT_MS)) {
                ESP_LOGI(TAG, "PIR timeout — entrance not confirmed");
                detect_state = DETECT_IDLE;
            } else if (gpio_get_level(PIR_PIN) == 1) {
                // ENTRANCE confirmed
                people_count++;
                pub_count(people_count);
                if (!room_occupied) {
                    room_occupied = true;
                    pub_room("OCCUPIED");
                    relay_set(true);
                    atomizer_set(true);
                    dfplayer_play(1);
                    music_on = true;
                    pub_music(true);
                }
                ESP_LOGI(TAG, ">>> ENTRANCE confirmed (People: %d)", people_count);
                last_detection_at = now;
                detect_state = DETECT_IDLE;
            }
            break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// WiFi
// ─────────────────────────────────────────────────────────────────────────────
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *event_data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
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

static void wifi_init(void)
{
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t h_any, h_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event_handler, NULL, &h_any));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        wifi_event_handler, NULL, &h_ip));

    wifi_config_t wifi_cfg = { .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS } };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected successfully.");
    } else {
        ESP_LOGE(TAG, "WiFi failed to connect after %d retries.", WIFI_MAX_RETRY);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// DFPlayer Mini — one-way UART control (ESP32 TX only)
// Protocol: 10-byte frame  7E FF 06 CMD 00 paramH paramL checkH checkL EF
// Checksum = 0 - sum(bytes 1..6) as a 16-bit value.
// ─────────────────────────────────────────────────────────────────────────────
static void dfplayer_send_cmd(uint8_t cmd, uint16_t param)
{
    uint8_t f[10];
    f[0] = 0x7E;
    f[1] = 0xFF;
    f[2] = 0x06;
    f[3] = cmd;
    f[4] = 0x00;
    f[5] = (param >> 8) & 0xFF;
    f[6] = param & 0xFF;
    uint16_t sum = 0;
    for (int i = 1; i <= 6; i++) sum += f[i];
    uint16_t chk = 0 - sum;
    f[7] = (chk >> 8) & 0xFF;
    f[8] = chk & 0xFF;
    f[9] = 0xEF;
    uart_write_bytes(DFPLAYER_UART, (const char *)f, sizeof(f));
}

static void dfplayer_init(void)
{
    uart_config_t cfg = {
        .baud_rate  = DFPLAYER_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(DFPLAYER_UART, 256, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(DFPLAYER_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(DFPLAYER_UART,
                                 DFPLAYER_TX_PIN,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));

    // Module needs ~1.5–2 s after power-up before it accepts commands
    vTaskDelay(pdMS_TO_TICKS(2000));
    dfplayer_send_cmd(0x06, 20);       // volume 0..30
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI("dfplayer", "init done (UART%d, TX=GPIO%d)",
             DFPLAYER_UART, DFPLAYER_TX_PIN);
}

static void dfplayer_play(uint16_t track)
{
    dfplayer_send_cmd(0x03, track);
    ESP_LOGI("dfplayer", "play track %u", track);
}

static void dfplayer_stop(void)
{
    dfplayer_send_cmd(0x16, 0);
    ESP_LOGI("dfplayer", "stop");
}

// ─────────────────────────────────────────────────────────────────────────────
// IR AC Control
// ─────────────────────────────────────────────────────────────────────────────
static void ir_tx_init(void)
{
    rmt_tx_channel_config_t tx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = IR_TX_GPIO,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_cfg, &ir_tx_handle));
    ESP_ERROR_CHECK(rmt_enable(ir_tx_handle));
    ESP_LOGI(TAG, "IR TX initialized on GPIO%d", IR_TX_GPIO);
}

static void send_ir_nec(uint32_t addr, uint32_t cmd)
{
    ir_nec_scan_code_t scan_code = {
        .address = addr,
        .command = cmd,
    };

    rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,
    };

    rmt_encoder_handle_t nec_encoder;
    ir_nec_encoder_config_t nec_cfg = {
        .flags.msb_first = 1,
    };

    ESP_ERROR_CHECK(rmt_new_ir_nec_encoder(&nec_cfg, &nec_encoder));
    ESP_ERROR_CHECK(rmt_transmit(ir_tx_handle, nec_encoder, &scan_code, &tx_cfg));
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK(rmt_encoder_reset(nec_encoder));
    ESP_LOGI(TAG, "IR sent: addr=0x%02X cmd=0x%02X", addr, cmd);
}

static void handle_ac_command(int temp)
{
    if (temp < 16 || temp > 30) {
        ESP_LOGW(TAG, "AC temp out of range: %d°C", temp);
        return;
    }

    // Map temperature to command code
    // For your RG56V2/BGEF remote, you'll need to capture actual IR codes
    // For now, using standard NEC: addr=0x01, cmd=temperature
    uint32_t cmd = (uint32_t)temp;

    ESP_LOGI(TAG, "AC: setting temperature to %d°C", temp);
    send_ir_nec(0x01, cmd);
}

// ─────────────────────────────────────────────────────────────────────────────
// IR RX — Temporary code capture (for learning remote codes)
// ─────────────────────────────────────────────────────────────────────────────
static bool ir_rx_ready = false;
static ir_nec_scan_code_t captured_code = {0, 0};
static rmt_decoder_handle_t nec_decoder = NULL;

static bool ir_rx_callback(rmt_rx_done_event_data_t *edata, void *user_ctx)
{
    rmt_nec_code_t *nec_code = (rmt_nec_code_t *)edata->received_symbols;
    if (edata->num_symbols > 0) {
        captured_code.address = nec_code->address;
        captured_code.command = nec_code->command;
        ir_rx_ready = true;
    }
    return true;
}

static void ir_rx_init(void)
{
    if (!IR_CAPTURE_ENABLED) return;

    rmt_rx_channel_config_t rx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = IR_CAPTURE_PIN,
        .mem_block_symbols = 64,
    };
    ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_cfg, &ir_rx_handle));

    ir_nec_decoder_config_t nec_cfg = {.flags.msb_first = 1};
    ESP_ERROR_CHECK(rmt_new_ir_nec_decoder(&nec_cfg, &nec_decoder));

    rmt_rx_event_callbacks_t cbs = {.on_done = ir_rx_callback};
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(ir_rx_handle, &cbs, NULL));
    ESP_ERROR_CHECK(rmt_enable(ir_rx_handle));
    ESP_ERROR_CHECK(rmt_receive(ir_rx_handle, nec_decoder, NULL, -1));
    ESP_LOGI(TAG, "IR RX (code capture) initialized on GPIO%d", IR_CAPTURE_PIN);
}

static bool ir_rx_capture(void)
{
    if (!IR_CAPTURE_ENABLED || !ir_rx_handle) return false;

    if (ir_rx_ready) {
        ir_rx_ready = false;
        ESP_LOGI(TAG, "IR CODE: addr=0x%02X cmd=0x%02X",
                 captured_code.address, captured_code.command);
        return true;
    }

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Entry point
// ─────────────────────────────────────────────────────────────────────────────
void app_main(void)
{
    // NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Relay — lamp OFF at boot (active LOW: HIGH = OFF)
    gpio_config_t relay_cfg = {
        .pin_bit_mask = (1ULL << RELAY_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&relay_cfg);
    gpio_set_level(RELAY_PIN, 1);   // active LOW — HIGH = OFF

    // Atomizer — OFF at boot
    gpio_config_t atomizer_cfg = {
        .pin_bit_mask = (1ULL << ATOMIZER_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&atomizer_cfg);
    gpio_set_level(ATOMIZER_PIN, 0);

    // PIR
    gpio_config_t pir_cfg = {
        .pin_bit_mask = (1ULL << PIR_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&pir_cfg);

    // IR (receivers for detection) — skip outer if capture is enabled
    if (!IR_CAPTURE_ENABLED) {
        ir_init();
    } else {
        // Only init inner receiver when capturing with outer
        gpio_config_t io = {
            .pin_bit_mask = (1ULL << IR_INNER_PIN),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);
        ESP_LOGI(TAG, "IR: outer receiver in capture mode, inner for detection");
    }

    // IR (transmitter for AC control)
    ir_tx_init();

    // IR (receiver code capture for learning remote)
    if (IR_CAPTURE_ENABLED) {
        ir_rx_init();
        ESP_LOGI(TAG, "=== IR CODE CAPTURE MODE ===");
        ESP_LOGI(TAG, "Press buttons on your remote — codes will be logged to serial");
    }

    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "=== Smart Home — WiFi/Firebase build ===");

    // DFPlayer (needs ~2 s startup before accepting commands)
    dfplayer_init();

    // WiFi (blocks until connected or WIFI_MAX_RETRY exhausted)
    wifi_init();
    esp_wifi_set_ps(WIFI_PS_NONE);

    // Firebase anonymous sign-in
    if (firebase_signin_anonymous()) {
        fb_ready = true;
        // Publish boot state so the dashboard shows real values immediately
        pub_room("EMPTY");
        pub_count(0);
        pub_light(false);
        pub_atomizer(false);
        pub_music(false);
        ESP_LOGI(TAG, "Firebase ready — publishing enabled.");
    } else {
        ESP_LOGE(TAG, "Firebase auth failed — publishing and commands disabled.");
    }

    ESP_LOGI(TAG, "System ready.");

    TickType_t last_cmd_poll = 0;
    while (1) {
        if (IR_CAPTURE_ENABLED) {
            // Code capture mode: listen for IR signals
            ir_rx_capture();
        } else {
            // Normal operation: detection + Firebase commands
            detection_poll();

            TickType_t now = xTaskGetTickCount();
            if ((now - last_cmd_poll) >= pdMS_TO_TICKS(COMMAND_POLL_MS)) {
                poll_command();
                last_cmd_poll = now;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}
