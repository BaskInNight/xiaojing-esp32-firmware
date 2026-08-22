/*
 * boot_smoke_main.c — minimal boot test
 * Prints markers at each startup phase. No business logic.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_psram.h"

static const char *TAG = "smoke";

void app_main(void)
{
    ESP_LOGI(TAG, "=== BOOT_SMOKE_START ===");
    ESP_LOGI(TAG, "Free heap: %"PRIu32, esp_get_free_heap_size());
    ESP_LOGI(TAG, "PSRAM size: %zu", esp_psram_get_size());
    ESP_LOGI(TAG, "=== BOOT_SMOKE_OK ===");

    /* Heartbeat every 5 seconds */
    int beat = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "heartbeat %d, heap=%"PRIu32, ++beat, esp_get_free_heap_size());
    }
}
