/*
 * turbidity_sensor.c — Turbidity sensor ADC driver
 * ADC1_CH4 on GPIO5 using ADC oneshot API.
 * Line-fitting calibration (ESP32-S3 supported).
 * Calibration failure is non-fatal.
 */

#include <stdlib.h>
#include "turbidity_sensor.h"
#include "board_config.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "turbidity";

struct turbidity_ctx {
    adc_oneshot_unit_handle_t adc_handle;
    adc_cali_handle_t cali_handle;
    bool calibrated;
    adc_channel_t adc_channel;
};

turbidity_handle_t *turbidity_sensor_init(void)
{
    turbidity_handle_t *handle = calloc(1, sizeof(turbidity_handle_t));
    if (!handle) return NULL;

    /* Create ADC oneshot unit for ADC1 */
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    esp_err_t err = adc_oneshot_new_unit(&init_cfg, &handle->adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit creation failed: 0x%x", err);
        free(handle);
        return NULL;
    }

    /* Configure ADC1_CH4 (GPIO5) */
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };

    /* Map GPIO5 to ADC channel */
    adc_unit_t mapped_unit = ADC_UNIT_1;
    err = adc_oneshot_io_to_channel(XIAOJING_GPIO_TURBIDITY_ADC,
                                    &mapped_unit,
                                    &handle->adc_channel);
    if (err != ESP_OK || mapped_unit != ADC_UNIT_1) {
        ESP_LOGE(TAG, "GPIO%d not valid ADC pin: 0x%x",
                 XIAOJING_GPIO_TURBIDITY_ADC, err);
        goto fail;
    }

    err = adc_oneshot_config_channel(handle->adc_handle,
                                     handle->adc_channel,
                                     &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed: 0x%x", err);
        goto fail;
    }

    /* Attempt calibration (non-fatal if fails).
     * Try line-fitting first, fall back to curve-fitting. */
#ifdef CONFIG_ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    {
        adc_cali_line_fitting_config_t cali_config = {
            .unit_id = ADC_UNIT_1,
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        err = adc_cali_create_scheme_line_fitting(&cali_config,
                                                   &handle->cali_handle);
        if (err == ESP_OK) {
            handle->calibrated = true;
            ESP_LOGI(TAG, "ADC calibration OK (line-fitting)");
        }
    }
#endif
#ifdef CONFIG_ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (!handle->calibrated) {
        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = ADC_UNIT_1,
            .chan = handle->adc_channel,
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        err = adc_cali_create_scheme_curve_fitting(&cali_config,
                                                    &handle->cali_handle);
        if (err == ESP_OK) {
            handle->calibrated = true;
            ESP_LOGI(TAG, "ADC calibration OK (curve-fitting)");
        }
    }
#endif
    if (!handle->calibrated) {
        ESP_LOGW(TAG, "ADC calibration unavailable (raw only)");
    }

    ESP_LOGI(TAG, "Turbidity sensor initialized (GPIO%d, ADC1_CH%u)",
             XIAOJING_GPIO_TURBIDITY_ADC, handle->adc_channel);
    return handle;

fail:
    turbidity_sensor_destroy(handle);
    return NULL;
}

void turbidity_sensor_destroy(turbidity_handle_t *handle)
{
    if (!handle) return;
    if (handle->cali_handle) {
#ifdef CONFIG_ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        adc_cali_delete_scheme_line_fitting(handle->cali_handle);
#elif defined(CONFIG_ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED)
        adc_cali_delete_scheme_curve_fitting(handle->cali_handle);
#endif
    }
    if (handle->adc_handle) {
        adc_oneshot_del_unit(handle->adc_handle);
    }
    free(handle);
}

esp_err_t turbidity_sensor_read(turbidity_handle_t *handle,
                                turbidity_reading_t *out)
{
    if (!handle || !out) return ESP_ERR_INVALID_ARG;

    out->calibrated = false;
    out->voltage_mv = 0;

    esp_err_t err = adc_oneshot_read(handle->adc_handle,
                                     handle->adc_channel,
                                     &out->raw);
    if (err != ESP_OK) return err;

    if (handle->calibrated && handle->cali_handle) {
        int mv = 0;
        if (adc_cali_raw_to_voltage(handle->cali_handle, out->raw, &mv)
            == ESP_OK) {
            out->voltage_mv = mv;
            out->calibrated = true;
        }
    }

    return ESP_OK;
}

esp_err_t turbidity_sensor_read_raw(turbidity_handle_t *handle, int *raw)
{
    if (!handle || !raw) return ESP_ERR_INVALID_ARG;
    return adc_oneshot_read(handle->adc_handle, handle->adc_channel, raw);
}
