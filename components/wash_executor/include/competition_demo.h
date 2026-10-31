#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "wash_contract.h"
#include "wash_executor.h"

/* Trusted built-in program, not an arbitrary BLE/AI script interpreter. */
#ifndef COMPETITION_DEMO_SHAFT_HOME_REQUIRED
/* GPB5 shaft alignment is spare hardware in this prototype. Bucket halls remain required. */
#define COMPETITION_DEMO_SHAFT_HOME_REQUIRED 0
#endif
typedef struct {
    bool active;
    bool completed;
    uint32_t program_id;
    uint32_t generation;
    uint8_t stage;
    uint8_t total_stages;
    uint32_t elapsed_ms;
    uint32_t estimated_total_ms;
    wash_phase_t phase;
    drum_position_t target;
    uint32_t error_code;
    uint32_t fault_detail;
} competition_demo_snapshot_t;

/* Preserve service identity when reporting a composite failure. */
static inline uint32_t competition_demo_event_fault(const machine_event_t *event)
{
    if (event->fault.code > FAULT_NONE && event->fault.code <= FAULT_INTERNAL)
        return (uint32_t)event->fault.code;
    return event->result == SERVICE_RESULT_TIMEOUT ? FAULT_PROGRAM_TIMEOUT : FAULT_INTERNAL;
}

typedef void (*competition_demo_terminal_fn)(const void *token, bool success,
                                             uint32_t error, void *context);
esp_err_t competition_demo_submit(const void *token, size_t token_bytes,
                                  competition_demo_terminal_fn terminal, void *context);
esp_err_t competition_demo_submit_plan(const void *token, size_t token_bytes,
    const wash_recipe_t *recipe, competition_demo_terminal_fn terminal, void *context);
esp_err_t competition_demo_cancel(const void *token, size_t token_bytes);
void competition_demo_abort(void);
esp_err_t competition_demo_shutdown(void);
void competition_demo_observe_event(const machine_event_t *event);
bool competition_demo_get_snapshot(competition_demo_snapshot_t *out);
