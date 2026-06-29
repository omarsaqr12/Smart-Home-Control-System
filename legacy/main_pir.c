#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define PIR_PIN GPIO_NUM_23

static const char *TAG = "pir";

void app_main(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << PIR_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "Waiting 30 seconds for PIR to warm up...");
    ESP_LOGI(TAG, "Do not move in front of it during this time.");

    for (int i = 30; i > 0; i--) {
        ESP_LOGI(TAG, "Ready in: %d seconds", i);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "PIR is ready!");

    while (1) {
        int pir_state = gpio_get_level(PIR_PIN);
        if (pir_state == 1) {
            ESP_LOGI(TAG, "MOTION DETECTED -- Room: OCCUPIED");
        } else {
            ESP_LOGI(TAG, "No motion       -- Room: EMPTY");
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
