#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * turbidity_sensor.h — Turbidity sensor ADC driver
 * ADC1_CH4 on GPIO5 with line-fitting calibration.
 * Calibration failure is non-fatal: raw values still available.
 * ================================================================ */

typedef struct turbidity_ctx turbidity_handle_t;

typedef struct {
    int raw;            /* 12-bit ADC value (0-4095) */
    int voltage_mv;     /* Calibrated millivolts (valid only if calibrated=true) */
    bool calibrated;    /* true if calibration succeeded */
} turbidity_reading_t;

/* Initialize turbidity sensor (ADC1_CH4, GPIO5). */
turbidity_handle_t *turbidity_sensor_init(void);

/* Destroy sensor. NULL-safe. */
void turbidity_sensor_destroy(turbidity_handle_t *handle);

/* Read turbidity sensor (raw + calibrated if available). */
esp_err_t turbidity_sensor_read(turbidity_handle_t *handle,
                                turbidity_reading_t *out);

/* Read raw ADC value only (always works). */
esp_err_t turbidity_sensor_read_raw(turbidity_handle_t *handle, int *raw);

#ifdef __cplusplus
}
#endif
