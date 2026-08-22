/*
 * drain_fsm.c — 排水状态机（纯 C，无 FreeRTOS/HAL 依赖）
 */

#include "drain_fsm.h"
#include "safety_contract.h"
#include <string.h>

/* ---- elapsed helper (wrap-safe) ---- */

static uint32_t elapsed_ms(uint32_t now, uint32_t start)
{
    return (now - start);
}

/* ---- set terminal ---- */

static void set_terminal(drain_fsm_ctx_t *ctx, drain_fsm_output_t *out,
                          drain_terminal_t terminal,
                          machine_fault_code_t fault_code,
                          fault_severity_t fault_severity)
{
    ctx->terminal = terminal;
    ctx->fault_code = fault_code;
    ctx->fault_severity = fault_severity;
    out->terminal = terminal;
    out->fault_code = fault_code;
    out->fault_severity = fault_severity;
    out->emit_event = true;
}

/* ---- init ---- */

void drain_fsm_init(drain_fsm_ctx_t *ctx, const drain_params_t *config)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->max_duration_ms = config->max_duration_ms;
    ctx->state = DRAIN_FSM_IDLE;
    ctx->output_state = DRAIN_OUTPUT_KNOWN_OFF;
}

/* ---- begin request ---- */

esp_err_t drain_fsm_begin_request(drain_fsm_ctx_t *ctx,
                                    const drain_request_t *req,
                                    uint32_t max_duration_ms,
                                    uint32_t now_ms)
{
    if (ctx->state != DRAIN_FSM_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (req->request_id == MACHINE_REQUEST_ID_INVALID) {
        return ESP_ERR_INVALID_ARG;
    }
    if (req->duration_ms == 0 || req->duration_ms > max_duration_ms) {
        return ESP_ERR_INVALID_ARG;
    }

    ctx->request_id = req->request_id;
    ctx->duration_ms = req->duration_ms;
    ctx->state = DRAIN_FSM_VALIDATING;
    ctx->state_enter_ms = now_ms;
    ctx->tick_ms = now_ms;
    ctx->terminal_event_emitted = false;
    ctx->terminal = DRAIN_TERMINAL_NONE;
    ctx->pending_action = DRAIN_ACTION_NONE;
    ctx->fault_code = FAULT_NONE;
    ctx->fault_severity = FAULT_SEVERITY_WARNING;
    return ESP_OK;
}

/* ---- tick dispatch ---- */

static void tick_validating(drain_fsm_ctx_t *ctx, const drain_fsm_event_t *evt,
                              drain_fsm_output_t *out)
{
    if (evt->fault_active || evt->emergency_active) {
        ctx->state = DRAIN_FSM_FAULT;
        out->next_state = DRAIN_FSM_FAULT;
        set_terminal(ctx, out, DRAIN_TERMINAL_FAULT,
                     FAULT_INTERNAL, FAULT_SEVERITY_RECOVERABLE);
        return;
    }
    if (!evt->position_valid) {
        ctx->state = DRAIN_FSM_FAULT;
        out->next_state = DRAIN_FSM_FAULT;
        set_terminal(ctx, out, DRAIN_TERMINAL_REJECTED,
                     FAULT_NONE, FAULT_SEVERITY_WARNING);
        return;
    }
    if (!evt->position_fresh) {
        ctx->state = DRAIN_FSM_FAULT;
        out->next_state = DRAIN_FSM_FAULT;
        set_terminal(ctx, out, DRAIN_TERMINAL_REJECTED,
                     FAULT_NONE, FAULT_SEVERITY_WARNING);
        return;
    }
    if (!evt->position_stable) {
        ctx->state = DRAIN_FSM_FAULT;
        out->next_state = DRAIN_FSM_FAULT;
        set_terminal(ctx, out, DRAIN_TERMINAL_REJECTED,
                     FAULT_NONE, FAULT_SEVERITY_WARNING);
        return;
    }
    if (evt->position_motor_moving) {
        ctx->state = DRAIN_FSM_FAULT;
        out->next_state = DRAIN_FSM_FAULT;
        set_terminal(ctx, out, DRAIN_TERMINAL_REJECTED,
                     FAULT_NONE, FAULT_SEVERITY_WARNING);
        return;
    }
    if (evt->position != DRUM_POS_180) {
        ctx->state = DRAIN_FSM_FAULT;
        out->next_state = DRAIN_FSM_FAULT;
        set_terminal(ctx, out, DRAIN_TERMINAL_REJECTED,
                     FAULT_NONE, FAULT_SEVERITY_WARNING);
        return;
    }

    /* All checks passed → request valve ON */
    ctx->state = DRAIN_FSM_RUNNING;
    ctx->state_enter_ms = evt->now_ms;
    out->next_state = DRAIN_FSM_RUNNING;
    out->action = DRAIN_ACTION_VALVE_ON;
    ctx->pending_action = DRAIN_ACTION_VALVE_ON;
}

static void tick_running(drain_fsm_ctx_t *ctx, const drain_fsm_event_t *evt,
                           drain_fsm_output_t *out)
{
    /* Check timeout */
    if (elapsed_ms(evt->now_ms, ctx->state_enter_ms) >= ctx->duration_ms) {
        ctx->state = DRAIN_FSM_STOPPING;
        out->next_state = DRAIN_FSM_STOPPING;
        out->action = DRAIN_ACTION_VALVE_OFF;
        ctx->pending_action = DRAIN_ACTION_VALVE_OFF;
        return;
    }
    out->next_state = DRAIN_FSM_RUNNING;
    out->action = DRAIN_ACTION_NONE;
}

static void tick_stopping(drain_fsm_ctx_t *ctx, const drain_fsm_event_t *evt,
                            drain_fsm_output_t *out)
{
    /* Waiting for VALVE_OFF confirmation via action_result */
    out->next_state = DRAIN_FSM_STOPPING;
    out->action = DRAIN_ACTION_NONE;
}

static void tick_canceling(drain_fsm_ctx_t *ctx, const drain_fsm_event_t *evt,
                             drain_fsm_output_t *out)
{
    out->next_state = DRAIN_FSM_CANCELING;
    out->action = DRAIN_ACTION_NONE;
}

static void handle_action_result(drain_fsm_ctx_t *ctx,
                                   const drain_fsm_event_t *evt,
                                   drain_fsm_output_t *out)
{
    if (evt->action_result_action != ctx->pending_action) {
        /* Stale result, ignore */
        return;
    }

    bool ok = evt->action_result_ok;
    drain_fsm_action_t completed = ctx->pending_action;
    ctx->pending_action = DRAIN_ACTION_NONE;

    if (completed == DRAIN_ACTION_VALVE_ON) {
        if (ok) {
            ctx->output_state = DRAIN_OUTPUT_KNOWN_ON;
        } else {
            /* ON failed → FAULT, output UNKNOWN */
            ctx->output_state = DRAIN_OUTPUT_UNKNOWN;
            ctx->state = DRAIN_FSM_STOPPING;
            out->next_state = DRAIN_FSM_STOPPING;
            out->action = DRAIN_ACTION_VALVE_OFF;
            ctx->pending_action = DRAIN_ACTION_VALVE_OFF;
            set_terminal(ctx, out, DRAIN_TERMINAL_FAULT,
                         FAULT_MCP_IO, FAULT_SEVERITY_RECOVERABLE);
        }
    } else if (completed == DRAIN_ACTION_VALVE_OFF) {
        if (ok) {
            ctx->output_state = DRAIN_OUTPUT_KNOWN_OFF;
        } else {
            ctx->output_state = DRAIN_OUTPUT_UNKNOWN;
        }
        /* Regardless of OFF result, transition to terminal */
        if (ctx->state == DRAIN_FSM_STOPPING) {
            ctx->state = DRAIN_FSM_COMPLETE;
            out->next_state = DRAIN_FSM_COMPLETE;
            if (ctx->terminal == DRAIN_TERMINAL_NONE) {
                set_terminal(ctx, out, DRAIN_TERMINAL_COMPLETE,
                             FAULT_NONE, FAULT_SEVERITY_WARNING);
            } else {
                /* Already set (e.g. FAULT from ON failure) */
                out->terminal = ctx->terminal;
                out->fault_code = ctx->fault_code;
                out->fault_severity = ctx->fault_severity;
                out->emit_event = !ctx->terminal_event_emitted;
            }
        } else if (ctx->state == DRAIN_FSM_CANCELING) {
            ctx->state = DRAIN_FSM_COMPLETE;
            out->next_state = DRAIN_FSM_COMPLETE;
            set_terminal(ctx, out, DRAIN_TERMINAL_CANCELED,
                         FAULT_NONE, FAULT_SEVERITY_WARNING);
        }
    }
}

/* ---- main tick ---- */

void drain_fsm_tick(drain_fsm_ctx_t *ctx, const drain_fsm_event_t *evt,
                     drain_fsm_output_t *out)
{
    memset(out, 0, sizeof(*out));
    out->next_state = ctx->state;
    out->action = DRAIN_ACTION_NONE;

    if (evt->type == DRAIN_EVT_ACTION_RESULT) {
        handle_action_result(ctx, evt, out);
        return;
    }

    if (evt->type == DRAIN_EVT_CANCEL) {
        if (evt->cancel_request_id == ctx->request_id &&
            ctx->state == DRAIN_FSM_RUNNING) {
            ctx->state = DRAIN_FSM_CANCELING;
            out->next_state = DRAIN_FSM_CANCELING;
            out->action = DRAIN_ACTION_VALVE_OFF;
            ctx->pending_action = DRAIN_ACTION_VALVE_OFF;
        }
        return;
    }

    if (evt->type == DRAIN_EVT_EMERGENCY) {
        if (ctx->state != DRAIN_FSM_IDLE &&
            ctx->state != DRAIN_FSM_COMPLETE &&
            ctx->state != DRAIN_FSM_FAULT) {
            ctx->state = DRAIN_FSM_STOPPING;
            out->next_state = DRAIN_FSM_STOPPING;
            out->action = DRAIN_ACTION_VALVE_OFF;
            ctx->pending_action = DRAIN_ACTION_VALVE_OFF;
            set_terminal(ctx, out, DRAIN_TERMINAL_FAULT,
                         FAULT_INTERNAL, FAULT_SEVERITY_RECOVERABLE);
        }
        return;
    }

    /* TICK */
    ctx->tick_ms = evt->now_ms;

    switch (ctx->state) {
    case DRAIN_FSM_VALIDATING:
        tick_validating(ctx, evt, out);
        break;
    case DRAIN_FSM_RUNNING:
        tick_running(ctx, evt, out);
        break;
    case DRAIN_FSM_STOPPING:
        tick_stopping(ctx, evt, out);
        break;
    case DRAIN_FSM_CANCELING:
        tick_canceling(ctx, evt, out);
        break;
    case DRAIN_FSM_COMPLETE:
    case DRAIN_FSM_FAULT:
        /* Terminal states handled by service layer */
        break;
    default:
        break;
    }
}

/* ---- mark terminal emitted ---- */

void drain_fsm_mark_terminal_emitted(drain_fsm_ctx_t *ctx)
{
    ctx->terminal_event_emitted = true;
}

/* ---- 显式枚举映射（与 detergent/uv 同契约，禁止数值依赖） ---- */

drain_state_t drain_fsm_state_to_public(drain_fsm_state_t fsm)
{
    switch (fsm) {
    case DRAIN_FSM_IDLE:       return DRAIN_STATE_IDLE;
    case DRAIN_FSM_VALIDATING: return DRAIN_STATE_VALIDATING;
    case DRAIN_FSM_RUNNING:    return DRAIN_STATE_RUNNING;
    case DRAIN_FSM_STOPPING:   return DRAIN_STATE_STOPPING;
    case DRAIN_FSM_COMPLETE:   return DRAIN_STATE_COMPLETE;
    case DRAIN_FSM_CANCELING:  return DRAIN_STATE_CANCELING;
    case DRAIN_FSM_FAULT:      return DRAIN_STATE_FAULT;
    default:                   return DRAIN_STATE_FAULT;
    }
}

int drain_fsm_output_to_public(drain_output_state_t out)
{
    switch (out) {
    case DRAIN_OUTPUT_KNOWN_OFF: return 1;
    case DRAIN_OUTPUT_KNOWN_ON:  return 2;
    default:                     return 0; /* UNKNOWN / 未知 → fail-closed */
    }
}

bool drain_fsm_terminal_pending(const drain_fsm_ctx_t *ctx)
{
    if (!ctx) return false;
    return !ctx->terminal_event_emitted &&
           ctx->terminal != DRAIN_TERMINAL_NONE;
}
