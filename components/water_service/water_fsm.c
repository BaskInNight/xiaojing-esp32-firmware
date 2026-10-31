/*
 * water_fsm.c — 纯 C 进水状态机 (Round 5.4)
 *
 * commit_result requires pending_action != NONE and action == pending_action
 * commit_close_all requires pending_action == CLOSE_ALL
 * success ACTION_RESULT validates committed shadow
 * protocol violation → CLOSE_ALL
 */

#include "water_fsm.h"
#include <math.h>
#include <string.h>

static bool isfinite_positive(float v) { return isfinite(v) && v > 0.0f; }

static bool check_total_timeout(const water_fsm_ctx_t *ctx, int64_t now_ms)
{
    if (ctx->cfg.total_inlet_timeout_ms == 0) return false;
    return (now_ms - ctx->request_start_ms) >= (int64_t)ctx->cfg.total_inlet_timeout_ms;
}

static uint32_t compute_estimated_ml(const water_fsm_ctx_t *ctx)
{
    if (!isfinite_positive(ctx->cfg.pulses_per_liter)) return 0;
    if (ctx->total_flow_pulses == 0) return 0;
    double ml = (double)ctx->total_flow_pulses * 1000.0 / (double)ctx->cfg.pulses_per_liter;
    if (ml < 0.0 || ml > (double)UINT32_MAX) return UINT32_MAX;
    return (uint32_t)ml;
}

#define FLOW_IO_ERROR_THRESHOLD  3

void water_fsm_init(water_fsm_ctx_t *ctx, const water_service_config_t *cfg)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->state = WATER_STATE_IDLE;
    ctx->cfg = *cfg;
    ctx->request_id = MACHINE_REQUEST_ID_INVALID;
    ctx->source_valve = WATER_VALVE_UNKNOWN;
    ctx->transfer_valve = WATER_VALVE_UNKNOWN;
}

void water_fsm_begin_request(water_fsm_ctx_t *ctx,
                              const water_in_request_t *req, int64_t now_ms)
{
    ctx->request_id = req->request_id;
    ctx->complete_on_drum_full = req->complete_on_drum_full;
    ctx->request_start_ms = now_ms;
    ctx->completed_cycles = 0;
    ctx->current_batch_pulses = 0;
    ctx->total_flow_pulses = 0;
    ctx->low_flow_window_pulses = 0;
    ctx->flow_io_error_count = 0;
    ctx->terminal_event_emitted = false;
    ctx->pending_terminal_type = WATER_TERMINAL_NONE;
    ctx->pending_fault_code = FAULT_NONE;
    ctx->pending_action = WATER_FSM_ACTION_NONE;
    machine_fault_clear(&ctx->fault);
    uint32_t batch_max = req->source_batch_max_ms;
    if (batch_max == 0) batch_max = ctx->cfg.source_batch_max_ms;
    ctx->batch_max_ms_effective = batch_max;
    ctx->state = WATER_STATE_VALIDATING;
}

void water_fsm_snapshot(const water_fsm_ctx_t *ctx, water_snapshot_t *out)
{
    out->state = ctx->state;
    out->request_id = ctx->request_id;
    out->source_valve_on = (ctx->source_valve == WATER_VALVE_KNOWN_ON);
    out->transfer_valve_on = (ctx->transfer_valve == WATER_VALVE_KNOWN_ON);
    out->source_valve_unknown = (ctx->source_valve == WATER_VALVE_UNKNOWN);
    out->transfer_valve_unknown = (ctx->transfer_valve == WATER_VALVE_UNKNOWN);
    out->drum_water_full = ctx->drum_water_full;
    out->water_level_valid = ctx->water_level_valid;
    out->completed_cycles = ctx->completed_cycles;
    out->current_batch_pulses = ctx->current_batch_pulses;
    out->total_flow_pulses = ctx->total_flow_pulses;
    out->estimated_ml = compute_estimated_ml(ctx);
    out->elapsed_ms = 0;
    out->terminal_event_emitted = ctx->terminal_event_emitted;
    out->terminal_pending = !ctx->terminal_event_emitted && ctx->state != WATER_STATE_IDLE;
    out->fault = ctx->fault;
    out->revision = ctx->revision;
}

void water_fsm_mark_terminal_emitted(water_fsm_ctx_t *ctx)
{
    ctx->terminal_event_emitted = true;
    ctx->revision++;
}

/* ---- Helpers for terminal transitions ---- */

static void set_fault_terminal(water_fsm_output_t *out, water_fsm_ctx_t *ctx,
                                machine_fault_code_t code, fault_severity_t sev, int64_t now_ms)
{
    out->next_state = WATER_STATE_FAULT;
    out->terminal = WATER_TERMINAL_FAULT;
    out->fault_code = code;
    out->fault_severity = sev;
    out->emit_event = true;
    ctx->fault.code = code;
    ctx->fault.severity = sev;
    ctx->fault.timestamp_ms = now_ms;
}

static void set_complete_terminal(water_fsm_output_t *out, water_fsm_ctx_t *ctx)
{
    out->next_state = WATER_STATE_COMPLETE;
    out->terminal = WATER_TERMINAL_COMPLETE;
    out->emit_event = true;
}

static void begin_close_all_for_terminal(water_fsm_output_t *out, water_fsm_ctx_t *ctx,
                                          water_terminal_t term_type,
                                          machine_fault_code_t fault_code)
{
    ctx->pending_terminal_type = term_type;
    ctx->pending_fault_code = fault_code;
    out->action = WATER_FSM_ACTION_CLOSE_ALL;
    ctx->pending_action = WATER_FSM_ACTION_CLOSE_ALL;
    out->next_state = WATER_STATE_CLOSING_ALL;
}

/* Validate that ACTION_RESULT matches the expected pending action.
 * Requires ctx->pending_action == expected (not just event action ID). */
static bool action_result_matches(const water_fsm_ctx_t *ctx,
                                   const water_event_t *evt,
                                   water_fsm_action_t expected)
{
    return ctx != NULL && evt != NULL &&
           evt->type == WATER_EVT_ACTION_RESULT &&
           expected != WATER_FSM_ACTION_NONE &&
           ctx->pending_action == expected &&
           evt->action_result_action == expected;
}

/* Reject wrong action ID: protocol violation, best-effort CLOSE_ALL */
static void reject_wrong_action(water_fsm_output_t *out, water_fsm_ctx_t *ctx, int64_t now_ms)
{
    (void)now_ms;
    begin_close_all_for_terminal(out, ctx, WATER_TERMINAL_FAULT, FAULT_INTERNAL);
}

/* Check if action is a valid single-valve action (not CLOSE_ALL, NONE, RESET_FLOW) */
static bool is_single_valve_action(water_fsm_action_t action)
{
    return action == WATER_FSM_ACTION_OPEN_SOURCE ||
           action == WATER_FSM_ACTION_CLOSE_SOURCE ||
           action == WATER_FSM_ACTION_OPEN_TRANSFER ||
           action == WATER_FSM_ACTION_CLOSE_TRANSFER;
}

/* Validate shadow state matches expected after a success ACTION_RESULT.
 * Returns true if shadow is consistent, false if protocol violation. */
static bool validate_shadow_on_success(const water_fsm_ctx_t *ctx, water_fsm_action_t action)
{
    switch (action) {
    case WATER_FSM_ACTION_OPEN_SOURCE:    return ctx->source_valve == WATER_VALVE_KNOWN_ON;
    case WATER_FSM_ACTION_CLOSE_SOURCE:   return ctx->source_valve == WATER_VALVE_KNOWN_OFF;
    case WATER_FSM_ACTION_OPEN_TRANSFER:  return ctx->transfer_valve == WATER_VALVE_KNOWN_ON;
    case WATER_FSM_ACTION_CLOSE_TRANSFER: return ctx->transfer_valve == WATER_VALVE_KNOWN_OFF;
    case WATER_FSM_ACTION_CLOSE_ALL:      return ctx->source_valve == WATER_VALVE_KNOWN_OFF &&
                                                ctx->transfer_valve == WATER_VALVE_KNOWN_OFF;
    default: return true;
    }
}

/* ---- State handlers ---- */

static void handle_idle(water_fsm_ctx_t *ctx, const water_event_t *evt, water_fsm_output_t *out)
{
    (void)ctx; (void)evt;
    out->next_state = WATER_STATE_IDLE;
}

static void handle_validating(water_fsm_ctx_t *ctx, const water_event_t *evt, water_fsm_output_t *out)
{
    if (!evt->water_level_valid) {
        set_fault_terminal(out, ctx, FAULT_WATER_LEVEL_INVALID, FAULT_SEVERITY_RECOVERABLE, evt->now_ms);
        return;
    }
    if (evt->drum_full) {
        /* Initial drum full: still need to verify valves are OFF before COMPLETE */
        begin_close_all_for_terminal(out, ctx, WATER_TERMINAL_COMPLETE, FAULT_NONE);
        return;
    }
    ctx->batch_start_ms = evt->now_ms;
    ctx->window_start_ms = evt->now_ms;
    ctx->current_batch_pulses = 0;
    ctx->low_flow_window_pulses = 0;
    ctx->last_flow_pulse_ms = evt->now_ms;
    ctx->flow_io_error_count = 0;
    out->next_state = WATER_STATE_RESETTING_FLOW;
    out->action = WATER_FSM_ACTION_RESET_FLOW;
}

static void handle_resetting_flow(water_fsm_ctx_t *ctx, const water_event_t *evt, water_fsm_output_t *out)
{
    if (evt->type != WATER_EVT_FLOW_RESET_DONE) { out->next_state = WATER_STATE_RESETTING_FLOW; return; }
    if (!evt->action_result_ok) {
        set_fault_terminal(out, ctx, FAULT_FLOW_METER_RESET, FAULT_SEVERITY_RECOVERABLE, evt->now_ms);
        return;
    }
    out->next_state = WATER_STATE_OPENING_SOURCE;
    out->action = WATER_FSM_ACTION_OPEN_SOURCE;
    ctx->pending_action = WATER_FSM_ACTION_OPEN_SOURCE;
}

static void handle_opening_source(water_fsm_ctx_t *ctx, const water_event_t *evt, water_fsm_output_t *out)
{
    if (evt->type != WATER_EVT_ACTION_RESULT) { out->next_state = WATER_STATE_OPENING_SOURCE; return; }
    if (!action_result_matches(ctx, evt, WATER_FSM_ACTION_OPEN_SOURCE)) {
        /* Wrong action ID: protocol violation, best-effort CLOSE_ALL */
        begin_close_all_for_terminal(out, ctx, WATER_TERMINAL_FAULT, FAULT_INTERNAL);
        return;
    }
    ctx->pending_action = WATER_FSM_ACTION_NONE; /* consumed */
    if (!evt->action_result_ok) {
        /* OPEN failed: valve state UNKNOWN, best-effort CLOSE_ALL first */
        begin_close_all_for_terminal(out, ctx, WATER_TERMINAL_FAULT, FAULT_MCP_IO);
        return;
    }
    /* Success: validate shadow was committed */
    if (!validate_shadow_on_success(ctx, WATER_FSM_ACTION_OPEN_SOURCE)) {
        begin_close_all_for_terminal(out, ctx, WATER_TERMINAL_FAULT, FAULT_INTERNAL);
        return;
    }
    /* OPEN_SOURCE succeeded → SOURCE_FILL */
    out->next_state = WATER_STATE_SOURCE_FILL;
}

static void handle_source_fill(water_fsm_ctx_t *ctx, const water_event_t *evt, water_fsm_output_t *out)
{
    if (check_total_timeout(ctx, evt->now_ms)) {
        begin_close_all_for_terminal(out, ctx, WATER_TERMINAL_FAULT, FAULT_WATER_TIMEOUT);
        return;
    }
    if (!evt->flow_read_ok) {
        ctx->flow_io_error_count++;
        if (ctx->flow_io_error_count >= FLOW_IO_ERROR_THRESHOLD) {
            out->action = WATER_FSM_ACTION_CLOSE_SOURCE;
            ctx->pending_action = WATER_FSM_ACTION_CLOSE_SOURCE;
            out->next_state = WATER_STATE_CLOSING_SOURCE;
            ctx->pending_terminal_type = WATER_TERMINAL_FAULT;
            ctx->pending_fault_code = FAULT_FLOW_METER_IO;
            return;
        }
    } else {
        ctx->flow_io_error_count = 0;
        if (evt->flow_delta > 0) {
            ctx->current_batch_pulses += evt->flow_delta;
            ctx->total_flow_pulses += evt->flow_delta;
            ctx->low_flow_window_pulses += evt->flow_delta;
            ctx->last_flow_pulse_ms = evt->now_ms;
        }
    }
    if (evt->flow_read_ok) {
        if ((evt->now_ms - ctx->last_flow_pulse_ms) >= (int64_t)ctx->cfg.no_flow_timeout_ms) {
            out->action = WATER_FSM_ACTION_CLOSE_SOURCE;
            ctx->pending_action = WATER_FSM_ACTION_CLOSE_SOURCE;
            out->next_state = WATER_STATE_CLOSING_SOURCE;
            ctx->pending_terminal_type = WATER_TERMINAL_FAULT;
            ctx->pending_fault_code = FAULT_WATER_NO_FLOW;
            return;
        }
    }
    if (ctx->source_valve == WATER_VALVE_KNOWN_ON && !evt->safety_gpa0_active_ok) {
        out->action = WATER_FSM_ACTION_CLOSE_SOURCE;
        ctx->pending_action = WATER_FSM_ACTION_CLOSE_SOURCE;
        out->next_state = WATER_STATE_CLOSING_SOURCE;
        ctx->pending_terminal_type = WATER_TERMINAL_FAULT;
        ctx->pending_fault_code = FAULT_INTERNAL;
        return;
    }
    if (!evt->safety_apply_ok) {
        if (ctx->source_valve == WATER_VALVE_KNOWN_ON) {
            /* Close valve first, then emit fault terminal after result */
            begin_close_all_for_terminal(out, ctx, WATER_TERMINAL_FAULT, FAULT_MCP_IO);
            return;
        }
        set_fault_terminal(out, ctx, FAULT_MCP_IO, FAULT_SEVERITY_RECOVERABLE, evt->now_ms);
        return;
    }
    if (ctx->cfg.low_flow_min_pulses > 0) {
        int64_t we = evt->now_ms - ctx->window_start_ms;
        if (we >= (int64_t)ctx->cfg.low_flow_window_ms) {
            if (ctx->low_flow_window_pulses < ctx->cfg.low_flow_min_pulses) {
                out->action = WATER_FSM_ACTION_CLOSE_SOURCE;
                ctx->pending_action = WATER_FSM_ACTION_CLOSE_SOURCE;
                out->next_state = WATER_STATE_CLOSING_SOURCE;
                ctx->pending_terminal_type = WATER_TERMINAL_FAULT;
                ctx->pending_fault_code = FAULT_WATER_LOW_FLOW;
                return;
            }
            ctx->low_flow_window_pulses = 0;
            ctx->window_start_ms = evt->now_ms;
        }
    }
    bool batch_done = false;
    if (isfinite_positive(ctx->cfg.pulses_per_liter) && ctx->cfg.source_batch_target_pulses > 0) {
        if (ctx->current_batch_pulses >= ctx->cfg.source_batch_target_pulses) batch_done = true;
    }
    if ((evt->now_ms - ctx->batch_start_ms) >= (int64_t)ctx->batch_max_ms_effective) batch_done = true;

    if (batch_done) {
        out->action = WATER_FSM_ACTION_CLOSE_SOURCE;
        ctx->pending_action = WATER_FSM_ACTION_CLOSE_SOURCE;
        out->next_state = WATER_STATE_CLOSING_SOURCE;
        ctx->pending_terminal_type = WATER_TERMINAL_NONE; /* normal close */
        return;
    }
    out->next_state = WATER_STATE_SOURCE_FILL;
}

static void handle_closing_source(water_fsm_ctx_t *ctx, const water_event_t *evt, water_fsm_output_t *out)
{
    if (evt->type != WATER_EVT_ACTION_RESULT) { out->next_state = WATER_STATE_CLOSING_SOURCE; return; }
    if (!action_result_matches(ctx, evt, WATER_FSM_ACTION_CLOSE_SOURCE)) {
        reject_wrong_action(out, ctx, evt->now_ms);
        return;
    }
    ctx->pending_action = WATER_FSM_ACTION_NONE; /* consumed */
    if (!evt->action_result_ok) {
        set_fault_terminal(out, ctx, FAULT_MCP_IO, FAULT_SEVERITY_RECOVERABLE, evt->now_ms);
        return;
    }
    /* Success: validate shadow was committed */
    if (!validate_shadow_on_success(ctx, WATER_FSM_ACTION_CLOSE_SOURCE)) {
        set_fault_terminal(out, ctx, FAULT_INTERNAL, FAULT_SEVERITY_RECOVERABLE, evt->now_ms);
        return;
    }
    /* Close succeeded */
    if (ctx->pending_terminal_type != WATER_TERMINAL_NONE) {
        /* This was a fault-triggered close */
        out->next_state = WATER_STATE_FAULT;
        out->terminal = ctx->pending_terminal_type;
        out->fault_code = ctx->pending_fault_code;
        out->fault_severity = FAULT_SEVERITY_RECOVERABLE;
        out->emit_event = true;
        ctx->fault.code = ctx->pending_fault_code;
        ctx->fault.severity = FAULT_SEVERITY_RECOVERABLE;
        ctx->fault.timestamp_ms = evt->now_ms;
        return;
    }
    /* Normal batch close → settle */
    out->next_state = WATER_STATE_SOURCE_SETTLE;
    ctx->settle_start_ms = evt->now_ms;
}

static void handle_source_settle(water_fsm_ctx_t *ctx, const water_event_t *evt, water_fsm_output_t *out)
{
    if (check_total_timeout(ctx, evt->now_ms)) {
        begin_close_all_for_terminal(out, ctx, WATER_TERMINAL_FAULT, FAULT_WATER_TIMEOUT);
        return;
    }
    if ((evt->now_ms - ctx->settle_start_ms) >= (int64_t)ctx->cfg.source_settle_ms) {
        out->next_state = WATER_STATE_OPENING_TRANSFER;
        out->action = WATER_FSM_ACTION_OPEN_TRANSFER;
        ctx->pending_action = WATER_FSM_ACTION_OPEN_TRANSFER;
    } else {
        out->next_state = WATER_STATE_SOURCE_SETTLE;
    }
}

static void handle_opening_transfer(water_fsm_ctx_t *ctx, const water_event_t *evt, water_fsm_output_t *out)
{
    if (evt->type != WATER_EVT_ACTION_RESULT) { out->next_state = WATER_STATE_OPENING_TRANSFER; return; }
    if (!action_result_matches(ctx, evt, WATER_FSM_ACTION_OPEN_TRANSFER)) {
        /* Wrong action ID: protocol violation, best-effort CLOSE_ALL */
        begin_close_all_for_terminal(out, ctx, WATER_TERMINAL_FAULT, FAULT_INTERNAL);
        return;
    }
    ctx->pending_action = WATER_FSM_ACTION_NONE; /* consumed */
    if (!evt->action_result_ok) {
        /* OPEN failed: valve state UNKNOWN, best-effort CLOSE_ALL first */
        begin_close_all_for_terminal(out, ctx, WATER_TERMINAL_FAULT, FAULT_MCP_IO);
        return;
    }
    /* Success: validate shadow was committed */
    if (!validate_shadow_on_success(ctx, WATER_FSM_ACTION_OPEN_TRANSFER)) {
        begin_close_all_for_terminal(out, ctx, WATER_TERMINAL_FAULT, FAULT_INTERNAL);
        return;
    }
    /* OPEN_TRANSFER succeeded → TRANSFER, start transfer timer from success time */
    ctx->transfer_start_ms = evt->now_ms;
    out->next_state = WATER_STATE_TRANSFER;
}

static void handle_transfer(water_fsm_ctx_t *ctx, const water_event_t *evt, water_fsm_output_t *out)
{
    if (check_total_timeout(ctx, evt->now_ms)) {
        begin_close_all_for_terminal(out, ctx, WATER_TERMINAL_FAULT, FAULT_WATER_TIMEOUT);
        return;
    }
    if (ctx->transfer_valve == WATER_VALVE_KNOWN_ON && !evt->safety_gpa1_active_ok) {
        out->action = WATER_FSM_ACTION_CLOSE_TRANSFER;
        ctx->pending_action = WATER_FSM_ACTION_CLOSE_TRANSFER;
        out->next_state = WATER_STATE_CLOSING_TRANSFER;
        ctx->pending_terminal_type = WATER_TERMINAL_FAULT;
        ctx->pending_fault_code = FAULT_INTERNAL;
        return;
    }
    if (!evt->safety_apply_ok) {
        if (ctx->transfer_valve == WATER_VALVE_KNOWN_ON) {
            /* Close valve first, then emit fault terminal after result */
            begin_close_all_for_terminal(out, ctx, WATER_TERMINAL_FAULT, FAULT_MCP_IO);
            return;
        }
        set_fault_terminal(out, ctx, FAULT_MCP_IO, FAULT_SEVERITY_RECOVERABLE, evt->now_ms);
        return;
    }
    int64_t te = evt->now_ms - ctx->transfer_start_ms;
    if (te >= (int64_t)ctx->cfg.default_transfer_ms || te >= (int64_t)ctx->cfg.transfer_timeout_ms) {
        out->action = WATER_FSM_ACTION_CLOSE_TRANSFER;
        ctx->pending_action = WATER_FSM_ACTION_CLOSE_TRANSFER;
        out->next_state = WATER_STATE_CLOSING_TRANSFER;
        ctx->pending_terminal_type = WATER_TERMINAL_NONE;
        return;
    }
    out->next_state = WATER_STATE_TRANSFER;
}

static void handle_closing_transfer(water_fsm_ctx_t *ctx, const water_event_t *evt, water_fsm_output_t *out)
{
    if (evt->type != WATER_EVT_ACTION_RESULT) { out->next_state = WATER_STATE_CLOSING_TRANSFER; return; }
    if (!action_result_matches(ctx, evt, WATER_FSM_ACTION_CLOSE_TRANSFER)) {
        reject_wrong_action(out, ctx, evt->now_ms);
        return;
    }
    ctx->pending_action = WATER_FSM_ACTION_NONE; /* consumed */
    if (!evt->action_result_ok) {
        set_fault_terminal(out, ctx, FAULT_MCP_IO, FAULT_SEVERITY_RECOVERABLE, evt->now_ms);
        return;
    }
    /* Success: validate shadow was committed */
    if (!validate_shadow_on_success(ctx, WATER_FSM_ACTION_CLOSE_TRANSFER)) {
        set_fault_terminal(out, ctx, FAULT_INTERNAL, FAULT_SEVERITY_RECOVERABLE, evt->now_ms);
        return;
    }
    if (ctx->pending_terminal_type != WATER_TERMINAL_NONE) {
        out->next_state = WATER_STATE_FAULT;
        out->terminal = ctx->pending_terminal_type;
        out->fault_code = ctx->pending_fault_code;
        out->fault_severity = FAULT_SEVERITY_RECOVERABLE;
        out->emit_event = true;
        ctx->fault.code = ctx->pending_fault_code;
        ctx->fault.severity = FAULT_SEVERITY_RECOVERABLE;
        ctx->fault.timestamp_ms = evt->now_ms;
        return;
    }
    out->next_state = WATER_STATE_CHECK_DRUM_LEVEL;
}

static void handle_check_drum_level(water_fsm_ctx_t *ctx, const water_event_t *evt, water_fsm_output_t *out)
{
    if (check_total_timeout(ctx, evt->now_ms)) {
        begin_close_all_for_terminal(out, ctx, WATER_TERMINAL_FAULT, FAULT_WATER_TIMEOUT);
        return;
    }
    if (!evt->water_level_valid) {
        begin_close_all_for_terminal(out, ctx, WATER_TERMINAL_FAULT, FAULT_WATER_LEVEL_INVALID);
        return;
    }
    if (evt->drum_full) {
        set_complete_terminal(out, ctx);
        return;
    }
    ctx->completed_cycles++;
    if (ctx->completed_cycles >= ctx->cfg.max_fill_cycles) {
        begin_close_all_for_terminal(out, ctx, WATER_TERMINAL_FAULT, FAULT_WATER_TIMEOUT);
        return;
    }
    ctx->current_batch_pulses = 0;
    ctx->low_flow_window_pulses = 0;
    ctx->flow_io_error_count = 0;
    ctx->batch_start_ms = evt->now_ms;
    ctx->window_start_ms = evt->now_ms;
    ctx->last_flow_pulse_ms = evt->now_ms;
    out->next_state = WATER_STATE_RESETTING_FLOW;
    out->action = WATER_FSM_ACTION_RESET_FLOW;
}

#define CLOSE_ALL_SHADOW_RETRY_MAX 2

static void handle_closing_all(water_fsm_ctx_t *ctx, const water_event_t *evt, water_fsm_output_t *out)
{
    if (evt->type != WATER_EVT_ACTION_RESULT) { out->next_state = WATER_STATE_CLOSING_ALL; return; }
    if (!action_result_matches(ctx, evt, WATER_FSM_ACTION_CLOSE_ALL)) {
        /* Wrong action ID: stay in CLOSING_ALL, re-issue CLOSE_ALL */
        out->action = WATER_FSM_ACTION_CLOSE_ALL;
        ctx->pending_action = WATER_FSM_ACTION_CLOSE_ALL;
        out->next_state = WATER_STATE_CLOSING_ALL;
        return;
    }
    ctx->pending_action = WATER_FSM_ACTION_NONE; /* consumed */
    if (!evt->action_result_ok) {
        /* Close failed — upgrade to FAULT regardless of original terminal type */
        set_fault_terminal(out, ctx, FAULT_MCP_IO, FAULT_SEVERITY_RECOVERABLE, evt->now_ms);
        return;
    }
    /* Success: validate shadow was committed */
    if (!validate_shadow_on_success(ctx, WATER_FSM_ACTION_CLOSE_ALL)) {
        /* Shadow mismatch: bounded retry */
        if (ctx->close_all_shadow_retry_count < CLOSE_ALL_SHADOW_RETRY_MAX) {
            ctx->close_all_shadow_retry_count++;
            out->action = WATER_FSM_ACTION_CLOSE_ALL;
            ctx->pending_action = WATER_FSM_ACTION_CLOSE_ALL;
            out->next_state = WATER_STATE_CLOSING_ALL;
            return;
        }
        /* Retries exhausted */
        set_fault_terminal(out, ctx, FAULT_INTERNAL, FAULT_SEVERITY_RECOVERABLE, evt->now_ms);
        return;
    }
    /* Close succeeded — use saved terminal type */
    if (ctx->pending_terminal_type == WATER_TERMINAL_CANCELLED) {
        out->next_state = WATER_STATE_COMPLETE;
        out->terminal = WATER_TERMINAL_CANCELLED;
        out->emit_event = true;
    } else if (ctx->pending_terminal_type == WATER_TERMINAL_FAULT) {
        out->next_state = WATER_STATE_FAULT;
        out->terminal = WATER_TERMINAL_FAULT;
        out->fault_code = ctx->pending_fault_code;
        out->fault_severity = FAULT_SEVERITY_RECOVERABLE;
        out->emit_event = true;
        ctx->fault.code = ctx->pending_fault_code;
        ctx->fault.severity = FAULT_SEVERITY_RECOVERABLE;
        ctx->fault.timestamp_ms = evt->now_ms;
    } else {
        /* No specific terminal type — just close and go to COMPLETE */
        out->next_state = WATER_STATE_COMPLETE;
        out->terminal = WATER_TERMINAL_COMPLETE;
        out->emit_event = true;
    }
}

static void handle_complete(water_fsm_ctx_t *ctx, const water_event_t *evt, water_fsm_output_t *out)
{
    (void)evt;
    if (ctx->terminal_event_emitted) {
        ctx->request_id = MACHINE_REQUEST_ID_INVALID;
        out->next_state = WATER_STATE_IDLE;
    } else {
        out->next_state = WATER_STATE_COMPLETE;
    }
}

static void handle_fault(water_fsm_ctx_t *ctx, const water_event_t *evt, water_fsm_output_t *out)
{
    (void)evt;
    if (ctx->terminal_event_emitted) {
        ctx->request_id = MACHINE_REQUEST_ID_INVALID;
        out->next_state = WATER_STATE_IDLE;
    } else {
        out->next_state = WATER_STATE_FAULT;
    }
}

/* ================================================================
 * Main tick
 * ================================================================ */

void water_fsm_tick(water_fsm_ctx_t *ctx, const water_event_t *evt, water_fsm_output_t *out)
{
    if (evt && evt->type == WATER_EVT_TICK) {
        ctx->water_level_valid = evt->water_level_valid;
        ctx->drum_water_full = evt->drum_full;
    }
    out->next_state = ctx->state;
    out->action = WATER_FSM_ACTION_NONE;
    out->terminal = WATER_TERMINAL_NONE;
    out->fault_code = FAULT_NONE;
    out->fault_severity = FAULT_SEVERITY_WARNING;
    out->emit_event = false;

    if (evt->type == WATER_EVT_CANCEL &&
        evt->cancel_request_id == ctx->request_id &&
        ctx->request_id != MACHINE_REQUEST_ID_INVALID &&
        ctx->state != WATER_STATE_CLOSING_ALL &&
        ctx->state != WATER_STATE_COMPLETE &&
        ctx->state != WATER_STATE_FAULT &&
        ctx->state != WATER_STATE_IDLE) {
        water_fsm_cancel(ctx, evt->cancel_request_id, evt->now_ms, out);
        return;
    }
    if (evt->type == WATER_EVT_EMERGENCY &&
        ctx->state != WATER_STATE_IDLE &&
        ctx->state != WATER_STATE_UNINITIALIZED &&
        ctx->state != WATER_STATE_CLOSING_ALL &&
        ctx->state != WATER_STATE_COMPLETE &&
        ctx->state != WATER_STATE_FAULT) {
        water_fsm_emergency(ctx, evt->now_ms, out);
        return;
    }

    /* ACTION_RESULT must only be consumed by action-pending states.
     * Protocol violation: best-effort CLOSE_ALL before FAULT. */
    if (evt->type == WATER_EVT_ACTION_RESULT &&
        ctx->state != WATER_STATE_OPENING_SOURCE &&
        ctx->state != WATER_STATE_OPENING_TRANSFER &&
        ctx->state != WATER_STATE_CLOSING_SOURCE &&
        ctx->state != WATER_STATE_CLOSING_TRANSFER &&
        ctx->state != WATER_STATE_CLOSING_ALL) {
        begin_close_all_for_terminal(out, ctx, WATER_TERMINAL_FAULT, FAULT_INTERNAL);
        ctx->state = out->next_state;
        ctx->revision++;
        return;
    }

    switch (ctx->state) {
    case WATER_STATE_IDLE:              handle_idle(ctx, evt, out);             break;
    case WATER_STATE_VALIDATING:        handle_validating(ctx, evt, out);       break;
    case WATER_STATE_RESETTING_FLOW:    handle_resetting_flow(ctx, evt, out);   break;
    case WATER_STATE_OPENING_SOURCE:    handle_opening_source(ctx, evt, out);   break;
    case WATER_STATE_SOURCE_FILL:       handle_source_fill(ctx, evt, out);      break;
    case WATER_STATE_CLOSING_SOURCE:    handle_closing_source(ctx, evt, out);   break;
    case WATER_STATE_SOURCE_SETTLE:     handle_source_settle(ctx, evt, out);    break;
    case WATER_STATE_OPENING_TRANSFER:  handle_opening_transfer(ctx, evt, out); break;
    case WATER_STATE_TRANSFER:          handle_transfer(ctx, evt, out);         break;
    case WATER_STATE_CLOSING_TRANSFER:  handle_closing_transfer(ctx, evt, out); break;
    case WATER_STATE_CHECK_DRUM_LEVEL:  handle_check_drum_level(ctx, evt, out); break;
    case WATER_STATE_CLOSING_ALL:       handle_closing_all(ctx, evt, out);      break;
    case WATER_STATE_COMPLETE:          handle_complete(ctx, evt, out);         break;
    case WATER_STATE_FAULT:             handle_fault(ctx, evt, out);            break;
    default:                            out->next_state = WATER_STATE_FAULT;    break;
    }

    ctx->state = out->next_state;
    ctx->revision++;
}

/* ================================================================
 * Commit result — 阀门已知状态更新 (strict contract R5.4)
 * ================================================================ */

esp_err_t water_fsm_commit_result(water_fsm_ctx_t *ctx, water_fsm_action_t action,
                                  water_valve_commit_result_t result, water_fsm_output_t *out)
{
    out->next_state = ctx->state;
    out->action = WATER_FSM_ACTION_NONE;
    out->terminal = WATER_TERMINAL_NONE;
    out->fault_code = FAULT_NONE;
    out->emit_event = false;

    /* Strict guards: reject stale or mismatched commits */
    if (ctx->pending_action == WATER_FSM_ACTION_NONE) return ESP_ERR_INVALID_STATE;
    if (action != ctx->pending_action) return ESP_ERR_INVALID_STATE;
    if (!is_single_valve_action(action)) return ESP_ERR_INVALID_STATE;

    if (result == WATER_COMMIT_OK) {
        switch (action) {
        case WATER_FSM_ACTION_OPEN_SOURCE:     ctx->source_valve = WATER_VALVE_KNOWN_ON;   break;
        case WATER_FSM_ACTION_CLOSE_SOURCE:    ctx->source_valve = WATER_VALVE_KNOWN_OFF;  break;
        case WATER_FSM_ACTION_OPEN_TRANSFER:   ctx->transfer_valve = WATER_VALVE_KNOWN_ON;  break;
        case WATER_FSM_ACTION_CLOSE_TRANSFER:  ctx->transfer_valve = WATER_VALVE_KNOWN_OFF; break;
        default: break;
        }
    } else {
        switch (action) {
        case WATER_FSM_ACTION_OPEN_SOURCE:
        case WATER_FSM_ACTION_CLOSE_SOURCE:    ctx->source_valve = WATER_VALVE_UNKNOWN;    break;
        case WATER_FSM_ACTION_OPEN_TRANSFER:
        case WATER_FSM_ACTION_CLOSE_TRANSFER:  ctx->transfer_valve = WATER_VALVE_UNKNOWN;  break;
        default: break;
        }
    }
    return ESP_OK;
}

esp_err_t water_fsm_commit_close_all(water_fsm_ctx_t *ctx,
                                     water_valve_commit_result_t source_result,
                                     water_valve_commit_result_t transfer_result,
                                     water_fsm_output_t *out)
{
    out->next_state = ctx->state;
    out->action = WATER_FSM_ACTION_NONE;
    out->terminal = WATER_TERMINAL_NONE;
    out->fault_code = FAULT_NONE;
    out->emit_event = false;

    /* Only valid when pending_action == CLOSE_ALL */
    if (ctx->pending_action != WATER_FSM_ACTION_CLOSE_ALL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Commit each valve independently */
    ctx->source_valve = (source_result == WATER_COMMIT_OK) ? WATER_VALVE_KNOWN_OFF : WATER_VALVE_UNKNOWN;
    ctx->transfer_valve = (transfer_result == WATER_COMMIT_OK) ? WATER_VALVE_KNOWN_OFF : WATER_VALVE_UNKNOWN;
    return ESP_OK;
}

esp_err_t water_fsm_commit_initial_off(water_fsm_ctx_t *ctx,
                                       water_valve_commit_result_t source_result,
                                       water_valve_commit_result_t transfer_result,
                                       water_fsm_output_t *out)
{
    if (!ctx || !out) return ESP_ERR_INVALID_ARG;
    /* 初始安全 OFF 同步只在 FSM 空闲（无请求在飞）时进行。 */
    if (ctx->state != WATER_STATE_IDLE) return ESP_ERR_INVALID_STATE;

    memset(out, 0, sizeof(*out));
    out->next_state = ctx->state;
    out->action = WATER_FSM_ACTION_NONE;
    out->terminal = WATER_TERMINAL_NONE;
    out->fault_code = FAULT_NONE;
    out->fault_severity = FAULT_SEVERITY_WARNING;
    out->emit_event = false;

    /* 每个阀独立：OFF 请求成功 → KNOWN_OFF；失败 → UNKNOWN（fail-closed）。
     * UNKNOWN 绝不静默当作 OFF。 */
    ctx->source_valve = (source_result == WATER_COMMIT_OK)
        ? WATER_VALVE_KNOWN_OFF : WATER_VALVE_UNKNOWN;
    ctx->transfer_valve = (transfer_result == WATER_COMMIT_OK)
        ? WATER_VALVE_KNOWN_OFF : WATER_VALVE_UNKNOWN;
    return ESP_OK;
}

/* ================================================================
 * Cancel — outputs CLOSE_ALL, terminal decided after result
 * ================================================================ */

void water_fsm_cancel(water_fsm_ctx_t *ctx, machine_request_id_t request_id,
                       int64_t now_ms, water_fsm_output_t *out)
{
    out->action = WATER_FSM_ACTION_NONE;
    out->terminal = WATER_TERMINAL_NONE;
    out->fault_code = FAULT_NONE;
    out->emit_event = false;

    if (request_id != ctx->request_id || ctx->request_id == MACHINE_REQUEST_ID_INVALID) {
        out->next_state = ctx->state; return;
    }
    if (ctx->terminal_event_emitted) {
        out->next_state = ctx->state; return;
    }

    begin_close_all_for_terminal(out, ctx, WATER_TERMINAL_CANCELLED, FAULT_NONE);
    ctx->state = out->next_state;
    ctx->revision++;
}

void water_fsm_emergency(water_fsm_ctx_t *ctx, int64_t now_ms, water_fsm_output_t *out)
{
    out->fault_code = FAULT_NONE;
    out->emit_event = false;

    if (ctx->state == WATER_STATE_FAULT || ctx->state == WATER_STATE_IDLE ||
        ctx->state == WATER_STATE_UNINITIALIZED || ctx->state == WATER_STATE_CLOSING_ALL) {
        out->next_state = ctx->state;
        out->action = WATER_FSM_ACTION_NONE;
        out->terminal = WATER_TERMINAL_NONE;
        return;
    }

    begin_close_all_for_terminal(out, ctx, WATER_TERMINAL_FAULT, FAULT_INTERNAL);
    ctx->fault.code = FAULT_INTERNAL;
    ctx->fault.severity = FAULT_SEVERITY_RECOVERABLE;
    ctx->fault.timestamp_ms = now_ms;
    ctx->state = out->next_state;
    ctx->revision++;
}
