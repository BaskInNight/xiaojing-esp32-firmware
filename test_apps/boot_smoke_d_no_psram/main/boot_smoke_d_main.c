/*
 * boot_smoke_d_main.c — NO PSRAM baseline
 * Prints alive marker every second. No PSRAM, no business logic.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"

static const char *TAG = "smoke_d";

void app_main(void)
{
    ESP_LOGI(TAG, "=== BOOT_SMOKE_D_NO_PSRAM_START ===");
    ESP_LOGI(TAG, "Free heap: %"PRIu32, esp_get_free_heap_size());

    int tick = 0;
    while (1) {
        ESP_LOGI(TAG, "BOOT_SMOKE_NO_PSRAM_ALIVE tick=%d heap=%"PRIu32,
                 ++tick, esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
