#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * ibt2_driver.h — IBT-2 H-bridge motor driver
 * HARD CONSTRAINT: RPWM and LPWM can NEVER both be non-zero.
 * GPIO48 (EN) not driven until hardware confirmed.
 * No auto-motion without calibrated direction.
 * ================================================================ */

typedef enum {
    IBT2_STATE_IDLE = 0,
    IBT2_STATE_RUNNING_CW,
    IBT2_STATE_RUNNING_CCW,
} ibt2_state_t;

typedef struct {
    bool direction_calibrated;   /* If false, CW/CCW refused */
    bool rpwm_is_cw;            /* Direction mapping (from machine_config) */
} ibt2_driver_config_t;

#define IBT2_DEFAULT_CONFIG() { \
    .direction_calibrated = false, \
    .rpwm_is_cw = true, \
}

typedef struct ibt2_ctx ibt2_handle_t;

/* Initialize IBT-2 driver. Configures LEDC timer 2 + channels 2/3. */
ibt2_handle_t *ibt2_driver_init(const ibt2_driver_config_t *config);

/* Destroy driver. Stops motor first. NULL-safe. */
void ibt2_driver_destroy(ibt2_handle_t *handle);

/* Run motor clockwise at given PWM percent (0-100).
 * Returns ESP_ERR_INVALID_STATE if direction not calibrated.
 * Returns ESP_ERR_INVALID_ARG if pwm_percent > 100. */
esp_err_t ibt2_driver_run_cw(ibt2_handle_t *handle, uint8_t pwm_percent);

/* Run motor counter-clockwise at given PWM percent (0-100). */
esp_err_t ibt2_driver_run_ccw(ibt2_handle_t *handle, uint8_t pwm_percent);

/* Stop motor (both RPWM and LPWM = 0). Idempotent. */
esp_err_t ibt2_driver_stop(ibt2_handle_t *handle);

/* Emergency stop (same as stop, guaranteed idempotent). */
esp_err_t ibt2_driver_emergency_stop(ibt2_handle_t *handle);

/* Get current state. */
ibt2_state_t ibt2_driver_get_state(ibt2_handle_t *handle);

/* Get current PWM percent. */
uint8_t ibt2_driver_get_pwm(ibt2_handle_t *handle);

/* Update direction calibration (runtime). */
esp_err_t ibt2_driver_set_calibration(ibt2_handle_t *handle,
                                      bool calibrated,
                                      bool rpwm_is_cw);

#ifdef __cplusplus
}
#endif
