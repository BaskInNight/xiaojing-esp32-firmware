#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * water_level_sensor.h — Washing drum water level sensor
 * XKC-Y25-V NPN open-collector digital output on GPIO4.
 * ================================================================ */

typedef struct water_level_ctx water_level_handle_t;

/* Initialize water level sensor (GPIO4 input). */
water_level_handle_t *water_level_sensor_init(void);

/* Destroy sensor. NULL-safe. */
void water_level_sensor_destroy(water_level_handle_t *handle);

/* Read water level with debounce.
 * Two consecutive reads must agree (default 50ms debounce).
 * Returns ESP_OK and sets *full to true if drum is full. */
esp_err_t water_level_sensor_read(water_level_handle_t *handle, bool *full);

#ifdef __cplusplus
}
#endif
