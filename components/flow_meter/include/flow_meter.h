#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "xiaojing_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * flow_meter.h — Flow meter driver using PCNT (ESP-IDF 5.5.4)
 * Pulse counting from NPN open-collector flow sensor.
 * Thread-safe delta tracking.
 * ================================================================ */

typedef struct flow_ctx flow_handle_t;

/* Initialize flow meter on PCNT unit 0, GPIO18. */
flow_handle_t *flow_meter_init(void);

/* Destroy flow meter. NULL-safe. */
void flow_meter_destroy(flow_handle_t *handle);

/* Read flow snapshot (cumulative pulses + delta since last read).
 * Thread-safe. */
esp_err_t flow_meter_read(flow_handle_t *handle, flow_snapshot_t *out);

/* Reset pulse counter and delta accumulator. */
esp_err_t flow_meter_reset(flow_handle_t *handle);

/* Get cumulative pulse count (no delta update). */
esp_err_t flow_meter_get_count(flow_handle_t *handle, uint32_t *count);

#ifdef __cplusplus
}
#endif
