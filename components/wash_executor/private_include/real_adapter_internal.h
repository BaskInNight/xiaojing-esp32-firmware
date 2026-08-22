/*
 * real_adapter_internal.h — Real adapter context definition
 *
 * Shared between wash_executor_real_adapter.c and wash_executor_bootstrap.c
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include "wash_executor_core.h"
#include "machine_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Active dispatch tracking ---- */

typedef struct {
    terminal_token_t token;
    wash_step_type_t step_type;
    machine_request_id_t machine_request_id;
    bool in_flight;
} active_dispatch_t;

/* ---- Internal context ---- */

struct real_adapter_ctx {
    struct wash_executor *exec;

    /* Event queue — shared with bootstrap, services publish here */
    QueueHandle_t event_queue;

    /* Consumer task */
    TaskHandle_t consumer_task;
    SemaphoreHandle_t consumer_ready_sem;
    SemaphoreHandle_t consumer_done_sem;
    atomic_bool consumer_stop;

    /* Serializes dispatch slots and diagnostics across executor/consumer tasks. */
    SemaphoreHandle_t state_mutex;

    /* Active dispatch state (written by dispatch, read by consumer) */
    active_dispatch_t active;
    active_dispatch_t cancel;
    active_dispatch_t reset;

    /* BL50 defaults */
    uint8_t bl50_pwm_pulsator;
    uint8_t bl50_pwm_drum;
    uint8_t bl50_pwm_spin;
    uint8_t bl50_pwm_home;

    /* Diagnostics */
    uint32_t dispatch_count;
    uint32_t dispatch_reject_count;
    uint32_t cancel_count;
    uint32_t cancel_reject_count;
    uint32_t reset_count;
    uint32_t stale_terminal_drops;
    uint32_t wrong_service_drops;
    uint32_t total_events_consumed;
};

#ifdef __cplusplus
}
#endif
