#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "wash_contract.h"
#include "machine_types.h"
#include "wash_executor.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * wash_executor_core.h — Pure C executor state machine
 *
 * No FreeRTOS, HAL, NVS, GPIO, or dynamic memory.
 * Reentrant per-instance. Deterministic.
 * ================================================================ */

/* ---- Core execution phases ---- */

typedef enum {
    CORE_PHASE_IDLE = 0,
    CORE_PHASE_DISPATCHING,   /* sync dispatch in progress */
    CORE_PHASE_ACTIVE,        /* dispatch accepted, awaiting service terminal */
    CORE_PHASE_CANCELING,     /* cancel issued, awaiting cancel terminal */
    CORE_PHASE_RESETTING,     /* final-position reset in progress */
    CORE_PHASE_WAIT_LOAD,
    CORE_PHASE_WAIT_UNLOAD,
    CORE_PHASE_FAULT,
} core_phase_t;

/* ---- Core input events ---- */

typedef enum {
    CORE_EVENT_NONE = 0,
    CORE_EVENT_PROGRAM_ACCEPTED,
    CORE_EVENT_STEP_DISPATCH_OK,
    CORE_EVENT_STEP_DISPATCH_FAIL,
    CORE_EVENT_SERVICE_TERMINAL,
    CORE_EVENT_STEP_TIMEOUT,
    CORE_EVENT_SKIP_STEP,
    CORE_EVENT_CANCEL,
    CORE_EVENT_ABORT,
    CORE_EVENT_FAULT_ACK,
    CORE_EVENT_RESET_TERMINAL,
    CORE_EVENT_LOAD_CONFIRMED,
    CORE_EVENT_UNLOAD_CONFIRMED,
    CORE_EVENT_INTERNAL_CMD,
} core_event_type_t;

/* ---- Terminal token — carried on every dispatch and returned by publisher ---- */

#define TERMINAL_KIND_NORMAL    0
#define TERMINAL_KIND_CANCEL_ACK 1
#define TERMINAL_KIND_RESET_ACK  2

typedef struct {
    uint32_t program_id;
    uint32_t generation;
    uint16_t step_id;
    uint16_t request_id;
    uint8_t  service_kind;   /* wash_step_type_t of the dispatched step */
    uint8_t  terminal_kind;  /* TERMINAL_KIND_* */
} terminal_token_t;

typedef struct {
    core_event_type_t type;
    terminal_token_t token;       /* for SERVICE_TERMINAL / RESET_TERMINAL */
    bool success;
    uint32_t error_code;
} core_event_t;

/* ---- Core output commands ---- */

typedef enum {
    CORE_CMD_NONE = 0,
    CORE_CMD_DISPATCH_STEP,
    CORE_CMD_CANCEL_ACTIVE_STEP,
    CORE_CMD_REQUEST_FINAL_RESET,
    CORE_CMD_PUBLISH_PROGRAM_TERMINAL,
    CORE_CMD_ENTER_FAULT,
    CORE_CMD_RETURN_IDLE,
} core_cmd_type_t;

typedef struct {
    core_cmd_type_t type;
    terminal_token_t token;       /* for DISPATCH_STEP, CANCEL_ACTIVE_STEP */
    uint32_t program_id;
    bool terminal_success;
    bool final_position_confirmed; /* explicit evidence from core */
    uint32_t terminal_error;
    uint8_t  terminal_result;     /* COMPLETE/CANCELED/ABORTED/FAULT */
} core_cmd_t;

/* ---- Program terminal result codes ---- */

#define TERM_RESULT_COMPLETE  0
#define TERM_RESULT_CANCELED  1
#define TERM_RESULT_ABORTED   2
#define TERM_RESULT_FAULT     3

/* ---- Core instance ---- */

typedef struct {
    /* Phase */
    core_phase_t phase;
    wash_exec_event_t last_event;
    uint32_t generation;
    bool quiesced;

    /* Program — borrowed, NOT owned by core */
    const wash_program_t *program;
    uint32_t program_id;
    uint16_t total_steps;
    uint16_t current_step_index;
    uint16_t completed_steps;
    bool program_active;

    /* Dispatch tracking */
    uint16_t next_request_id;
    terminal_token_t active_token;
    bool token_valid;

    /* Cancel tracking */
    terminal_token_t cancel_token;
    bool cancel_token_valid;
    bool skip_in_progress;

    /* Reset tracking */
    terminal_token_t reset_token;
    bool reset_token_valid;

    /* Fault */
    bool fault_active;
    uint32_t fault_code;

    /* Terminal sink */
    bool terminal_pending;
    core_cmd_t pending_terminal;
} wash_executor_core_t;

/* ---- Core operations ---- */

void core_init(wash_executor_core_t *core);

/**
 * Process one event. Returns number of commands written to out_cmds.
 * Caller MUST provide at least 16 slots. Returns count even if capacity
 * was insufficient (written = min(count, capacity)).
 */
uint32_t core_process_event(
    wash_executor_core_t *core,
    const core_event_t *event,
    core_cmd_t *out_cmds,
    uint32_t out_capacity);

uint16_t core_next_request_id(wash_executor_core_t *core);

bool core_can_accept_program(const wash_executor_core_t *core);

/* True only while an active UV step can be safely skipped through the
 * cancel/close acknowledgement path. */
bool core_can_skip_current_step(const wash_executor_core_t *core);

const wash_step_t *core_get_current_step(const wash_executor_core_t *core);

/**
 * Exact 6-field token comparison.
 */
bool terminal_token_equal_exact(const terminal_token_t *a, const terminal_token_t *b);

/**
 * Build a dispatch token for the current step.
 */
terminal_token_t core_build_token(const wash_executor_core_t *core, uint16_t request_id);

/**
 * Check if a token matches the active dispatch.
 */
bool core_token_matches_active(
    const wash_executor_core_t *core,
    const terminal_token_t *token);

/**
 * Check if a token matches the cancel dispatch.
 */
bool core_token_matches_cancel(
    const wash_executor_core_t *core,
    const terminal_token_t *token);

/**
 * Check if a token matches the reset dispatch.
 */
bool core_token_matches_reset(
    const wash_executor_core_t *core,
    const terminal_token_t *token);

#ifdef __cplusplus
}
#endif
