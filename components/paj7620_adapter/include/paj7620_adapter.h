#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "i2c_bus_manager.h"
#include "mcp23017_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * paj7620_adapter.h — PAJ7620 gesture sensor interrupt adapter
 * Interrupt path: PAJ7620 INT → MCP GPB6 → MCP INTB → GPIO13
 * ISR does NOT do I2C. Gesture register read happens in task context.
 * ================================================================ */

#define PAJ7620_ADDR_DEFAULT  0x73

/* Gesture types from PAJ7620 register 0x43 */
typedef enum {
    PAJ_GESTURE_NONE         = 0x00,
    PAJ_GESTURE_RIGHT        = 0x01,
    PAJ_GESTURE_LEFT         = 0x02,
    PAJ_GESTURE_UP           = 0x04,
    PAJ_GESTURE_DOWN         = 0x08,
    PAJ_GESTURE_FORWARD      = 0x10,
    PAJ_GESTURE_BACKWARD     = 0x20,
    PAJ_GESTURE_CLOCKWISE    = 0x40,
    PAJ_GESTURE_COUNTER_CW   = 0x80,
    PAJ_GESTURE_WAVE         = 0x100,
} paj_gesture_t;

typedef struct {
    paj_gesture_t gesture;
    int64_t timestamp_ms;
    bool valid;
} paj_gesture_event_t;

typedef struct paj7620_ctx paj7620_handle_t;

/* Callback for gesture events (called from task context). */
typedef void (*paj7620_gesture_callback_t)(const paj_gesture_event_t *event,
                                           void *user_data);

typedef struct {
    uint16_t i2c_addr;       /* default 0x73 */
    uint32_t i2c_speed_hz;   /* board default 100kHz */
    int mcp_gpb_pin;         /* MCP GPB pin for gesture interrupt (default 6) */
} paj7620_config_t;

#define PAJ7620_DEFAULT_CONFIG() { \
    .i2c_addr = PAJ7620_ADDR_DEFAULT, \
    .i2c_speed_hz = 100000, \
    .mcp_gpb_pin = XIAOJING_MCP_GPB6_GESTURE_INT, \
}

/* Probe, verify part ID and configure PAJ7620 gesture mode. */
paj7620_handle_t *paj7620_init(i2c_bus_ctx_t *bus,
                               const paj7620_config_t *config);

/* Destroy adapter. NULL-safe. */
void paj7620_destroy(paj7620_handle_t *handle);

/* Register gesture callback. */
esp_err_t paj7620_register_callback(paj7620_handle_t *handle,
                                    paj7620_gesture_callback_t callback,
                                    void *user_data);

/* Check if GPB6 is active (gesture pending) by reading MCP GPIOB.
 * Returns true if gesture interrupt is active. */
esp_err_t paj7620_is_gesture_pending(paj7620_handle_t *handle,
                                     bool *pending);

/* Read gesture register from PAJ7620 via I2C.
 * Clears the gesture data. */
esp_err_t paj7620_read_gesture(paj7620_handle_t *handle,
                               paj_gesture_event_t *out);

#ifdef __cplusplus
}
#endif
