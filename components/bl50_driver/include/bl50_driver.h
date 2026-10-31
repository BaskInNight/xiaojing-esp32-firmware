#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * bl50_driver.h — ZS-X11B Hall BLDC pulsator driver
 * Business API: run_cw/run_ccw/brake/stop.
 * The legacy BL50 API name is retained so the service/protocol contract does
 * not change. Physical outputs are DIR plus an open-drain, inverted PWM.
 * ================================================================ */

typedef enum {
    BL50_STATE_IDLE = 0,
    BL50_STATE_RUNNING_CW,
    BL50_STATE_RUNNING_CCW,
    BL50_STATE_BRAKING,
} bl50_state_t;

typedef struct bl50_ctx bl50_handle_t;

/* Initialize ZS-X11B driver. Starts in PWM=0 (open-drain output held low). */
bl50_handle_t *bl50_driver_init(void);

/* Destroy driver. Stops motor first. NULL-safe. */
void bl50_driver_destroy(bl50_handle_t *handle);

/* Run clockwise at given PWM percent (0-100). */
esp_err_t bl50_driver_run_cw(bl50_handle_t *handle, uint8_t pwm_percent);

/* Run counter-clockwise at given PWM percent (0-100). */
esp_err_t bl50_driver_run_ccw(bl50_handle_t *handle, uint8_t pwm_percent);

/* ZS-X11B has no brake input; this performs a PWM=0 coast stop. */
esp_err_t bl50_driver_brake(bl50_handle_t *handle);

/* Stop by commanding PWM=0. Idempotent. */
esp_err_t bl50_driver_stop(bl50_handle_t *handle);

/* Emergency stop. Idempotent. */
esp_err_t bl50_driver_emergency_stop(bl50_handle_t *handle);

/* Get current state. */
bl50_state_t bl50_driver_get_state(bl50_handle_t *handle);

/* Get current PWM percent. */
uint8_t bl50_driver_get_pwm(bl50_handle_t *handle);

#ifdef __cplusplus
}
#endif
