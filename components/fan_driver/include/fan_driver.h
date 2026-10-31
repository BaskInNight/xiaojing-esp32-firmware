#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * fan_driver.h — Fan PWM driver (4-wire, 25kHz)
 * Simple percent-based control. 0% = off.
 * ================================================================ */

typedef struct fan_ctx fan_handle_t;

/* Initialize fan driver. Configures LEDC timer 0 + channel 0. */
fan_handle_t *fan_driver_init(void);

/* Destroy driver. Sets duty to 0 first. NULL-safe. */
void fan_driver_destroy(fan_handle_t *handle);

/* Set fan speed (0-100%). 0 = off. Idempotent. */
esp_err_t fan_driver_set_percent(fan_handle_t *handle, uint8_t percent);

/* Stop fan (0%). Idempotent. */
esp_err_t fan_driver_stop(fan_handle_t *handle);

/* Get current percent. */
uint8_t fan_driver_get_percent(fan_handle_t *handle);

#ifdef __cplusplus
}
#endif
