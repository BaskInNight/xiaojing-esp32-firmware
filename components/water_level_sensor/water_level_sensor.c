/*
 * water_level_sensor.c — Washing drum water level sensor
 * XKC-Y25-V high/low output on GPIO4 through the board's 5 V-to-3.3 V divider.
 * Two-read debounce: both reads must agree.
 */

#include <stdlib.h>
#include "water_level_sensor.h"
#include "board_config.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "water_lvl";

#define DEBOUNCE_MS  50
/* Verified on the fitted XKC module: output is LOW while water is detected. */
#define WATER_LEVEL_ACTIVE_HIGH 0

struct water_level_ctx {
    int gpio_pin;
    bool last_full;            /* Last confirmed reading */
    int64_t last_change_ms;    /* Timestamp of last signal change */
    int last_raw;              /* Last raw GPIO level */
};

water_level_handle_t *water_level_sensor_init(void)
{
    water_level_handle_t *handle = calloc(1, sizeof(water_level_handle_t));
    if (!handle) return NULL;

    handle->gpio_pin = XIAOJING_GPIO_WATER_LEVEL;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << handle->gpio_pin),
        .mode = GPIO_MODE_INPUT,
        /* XKC_OUT is driven through the external divider, so do not bias it
         * with an ESP32 internal pull resistor. */
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: 0x%x", err);
        free(handle);
        return NULL;
    }

    const int initial_raw = gpio_get_level(handle->gpio_pin);
    handle->last_raw = initial_raw;
    handle->last_full = WATER_LEVEL_ACTIVE_HIGH ? (initial_raw != 0)
                                                 : (initial_raw == 0);
    handle->last_change_ms = (int64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
    ESP_LOGI(TAG, "Water level sensor initialized (GPIO%d raw=%d level=%s)",
             handle->gpio_pin, initial_raw,
             handle->last_full ? "FULL" : "OK");
    return handle;
}

void water_level_sensor_destroy(water_level_handle_t *handle)
{
    free(handle);
}

esp_err_t water_level_sensor_read(water_level_handle_t *handle, bool *full)
{
    if (!handle || !full) return ESP_ERR_INVALID_ARG;

    int now_ms = (int)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    int raw = gpio_get_level(handle->gpio_pin);

    if (raw != handle->last_raw) {
        /* Signal changed — record timestamp, but don't update yet */
        handle->last_raw = raw;
        handle->last_change_ms = now_ms;
        ESP_LOGI(TAG, "GPIO%d raw edge=%d", handle->gpio_pin, raw);
    }

    /* Only accept new value after debounce window */
    bool detected = WATER_LEVEL_ACTIVE_HIGH ? (raw != 0) : (raw == 0);
    if (detected != handle->last_full) {
        if ((now_ms - handle->last_change_ms) >= DEBOUNCE_MS) {
            handle->last_full = detected;
            ESP_LOGI(TAG, "GPIO%d confirmed %s (raw=%d)",
                     handle->gpio_pin, detected ? "FULL" : "OK", raw);
        }
    }

    *full = handle->last_full;
    return ESP_OK;
}
