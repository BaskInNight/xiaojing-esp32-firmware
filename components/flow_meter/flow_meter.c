/*
 * flow_meter.c — Flow meter driver using PCNT new API
 * ESP-IDF 5.5.4 pulse_cnt.h.
 * GPIO18 rising edge counting from NPN open-collector flow sensor.
 */

#include <stdlib.h>
#include "flow_meter.h"
#include "board_config.h"
#include "esp_log.h"
#include "driver/pulse_cnt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "flow";

struct flow_ctx {
    pcnt_unit_handle_t pcnt_unit;
    pcnt_channel_handle_t pcnt_chan;
    SemaphoreHandle_t mutex;
    uint32_t last_count;     /* For delta tracking */
    uint32_t total_delta;    /* Accumulated delta since init/reset */
};

flow_handle_t *flow_meter_init(void)
{
    flow_handle_t *handle = calloc(1, sizeof(flow_handle_t));
    if (!handle) return NULL;

    handle->mutex = xSemaphoreCreateMutex();
    if (!handle->mutex) {
        free(handle);
        return NULL;
    }

    /* Create PCNT unit */
    pcnt_unit_config_t unit_config = {
        .high_limit = INT16_MAX,
        /* ESP-IDF PCNT requires low_limit < 0 < high_limit. The channel only
         * increments, so the negative range is never used by normal input. */
        .low_limit = INT16_MIN,
    };
    esp_err_t err = pcnt_new_unit(&unit_config, &handle->pcnt_unit);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCNT unit creation failed: 0x%x", err);
        goto fail;
    }

    /* Set glitch filter */
    pcnt_glitch_filter_config_t filter_config = {
        .max_glitch_ns = 1000,
    };
    err = pcnt_unit_set_glitch_filter(handle->pcnt_unit, &filter_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Glitch filter config failed: 0x%x", err);
        /* Non-fatal, continue without filter */
    }

    /* Create PCNT channel for GPIO18 */
    pcnt_chan_config_t chan_config = {
        .edge_gpio_num = XIAOJING_GPIO_FLOW_PCNT,
        .level_gpio_num = -1,
    };
    err = pcnt_new_channel(handle->pcnt_unit, &chan_config, &handle->pcnt_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCNT channel creation failed: 0x%x", err);
        goto fail;
    }

    /* Configure edge action: count on rising edge */
    err = pcnt_channel_set_edge_action(handle->pcnt_chan,
                                       PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                       PCNT_CHANNEL_EDGE_ACTION_HOLD);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCNT edge action config failed: 0x%x", err);
        goto fail;
    }

    /* Enable and start */
    err = pcnt_unit_enable(handle->pcnt_unit);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCNT unit enable failed: 0x%x", err);
        goto fail;
    }

    err = pcnt_unit_start(handle->pcnt_unit);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCNT unit start failed: 0x%x", err);
        goto fail;
    }

    handle->last_count = 0;
    handle->total_delta = 0;

    ESP_LOGI(TAG, "Flow meter initialized (GPIO%d, PCNT unit)",
             XIAOJING_GPIO_FLOW_PCNT);
    return handle;

fail:
    flow_meter_destroy(handle);
    return NULL;
}

void flow_meter_destroy(flow_handle_t *handle)
{
    if (!handle) return;

    if (handle->pcnt_unit) {
        pcnt_unit_stop(handle->pcnt_unit);
        pcnt_unit_disable(handle->pcnt_unit);
    }
    if (handle->pcnt_chan) {
        pcnt_del_channel(handle->pcnt_chan);
    }
    if (handle->pcnt_unit) {
        pcnt_del_unit(handle->pcnt_unit);
    }
    if (handle->mutex) {
        vSemaphoreDelete(handle->mutex);
    }
    free(handle);
}

esp_err_t flow_meter_read(flow_handle_t *handle, flow_snapshot_t *out)
{
    if (!handle || !out) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(handle->mutex, portMAX_DELAY);

    /* Read PCNT INSIDE mutex to prevent race with concurrent reset */
    int count = 0;
    esp_err_t err = pcnt_unit_get_count(handle->pcnt_unit, &count);
    if (err != ESP_OK) {
        xSemaphoreGive(handle->mutex);
        return err;
    }

    if (count < 0) count = 0;

    uint32_t current = (uint32_t)count;
    uint32_t delta = current - handle->last_count;
    handle->last_count = current;
    handle->total_delta += delta;

    out->pulses = current;
    out->delta_pulses = delta;
    out->timestamp_ms = (int64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;

    xSemaphoreGive(handle->mutex);
    return ESP_OK;
}

esp_err_t flow_meter_reset(flow_handle_t *handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(handle->mutex, portMAX_DELAY);

    esp_err_t err = pcnt_unit_clear_count(handle->pcnt_unit);
    if (err == ESP_OK) {
        handle->last_count = 0;
        handle->total_delta = 0;
    }

    xSemaphoreGive(handle->mutex);
    return err;
}

esp_err_t flow_meter_get_count(flow_handle_t *handle, uint32_t *count)
{
    if (!handle || !count) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(handle->mutex, portMAX_DELAY);

    int c = 0;
    esp_err_t err = pcnt_unit_get_count(handle->pcnt_unit, &c);
    if (err == ESP_OK) {
        *count = (c < 0) ? 0 : (uint32_t)c;
    }

    xSemaphoreGive(handle->mutex);
    return err;
}
