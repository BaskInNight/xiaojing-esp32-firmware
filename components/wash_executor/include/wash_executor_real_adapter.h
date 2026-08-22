/*
 * wash_executor_real_adapter.h — Real service adapter public API
 *
 * Bridges wash_executor's terminal_token_t protocol with
 * services' machine_event_sink_t protocol.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declare opaque types */
struct wash_executor_adapter;
typedef struct wash_executor_adapter wash_executor_adapter_t;
struct real_adapter_ctx;
typedef struct real_adapter_ctx real_adapter_ctx_t;
struct wash_executor;
typedef struct wash_executor wash_executor_t;

typedef struct {
    bool active_in_flight;
    bool cancel_in_flight;
    bool reset_in_flight;
    uint32_t active_request_id;
    uint32_t cancel_request_id;
    uint32_t reset_request_id;
    uint32_t active_step_type;
    uint32_t dispatch_count;
    uint32_t dispatch_reject_count;
    uint32_t cancel_count;
    uint32_t cancel_reject_count;
    uint32_t reset_count;
    uint32_t stale_terminal_drops;
    uint32_t wrong_service_drops;
    uint32_t events_consumed;
} real_adapter_snapshot_t;

/**
 * Initialize a real adapter. Caller provides adapter struct and context.
 * machine_event_queue is the shared queue that services publish to
 * (obtain via app_bootstrap_get_event_queue()).
 */
esp_err_t real_adapter_init(wash_executor_adapter_t *adapter,
                            real_adapter_ctx_t *ctx,
                            void *machine_event_queue);

/** Set executor pointer (called after executor creation). */
void real_adapter_set_executor(real_adapter_ctx_t *ctx, wash_executor_t *exec);

/** Override BL50 action-specific PWM defaults (0 = keep default). */
void real_adapter_set_bl50_defaults(real_adapter_ctx_t *ctx,
                                     uint8_t pulsator, uint8_t drum,
                                     uint8_t spin, uint8_t home);

/** Start the adapter's event consumer task. */
esp_err_t real_adapter_start_consumer(real_adapter_ctx_t *ctx);

/** Stop the adapter's event consumer task. */
esp_err_t real_adapter_stop_consumer(real_adapter_ctx_t *ctx);

/** Destroy consumer resources. Returns INVALID_STATE while task is live. */
esp_err_t real_adapter_destroy_consumer(real_adapter_ctx_t *ctx);

/* Diagnostics */
uint32_t real_adapter_dispatch_count(const real_adapter_ctx_t *ctx);
uint32_t real_adapter_cancel_count(const real_adapter_ctx_t *ctx);
uint32_t real_adapter_reset_count(const real_adapter_ctx_t *ctx);
uint32_t real_adapter_stale_drops(const real_adapter_ctx_t *ctx);
uint32_t real_adapter_wrong_service_drops(const real_adapter_ctx_t *ctx);
uint32_t real_adapter_events_consumed(const real_adapter_ctx_t *ctx);
bool real_adapter_active_in_flight(const real_adapter_ctx_t *ctx);
bool real_adapter_cancel_in_flight(const real_adapter_ctx_t *ctx);
bool real_adapter_reset_in_flight(const real_adapter_ctx_t *ctx);
esp_err_t real_adapter_get_snapshot(const real_adapter_ctx_t *ctx,
                                    real_adapter_snapshot_t *out);

#ifdef __cplusplus
}
#endif
