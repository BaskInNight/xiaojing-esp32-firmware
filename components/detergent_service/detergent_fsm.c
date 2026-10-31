/*
 * detergent_fsm.c — 洗衣液投放状态机（纯 C，无 FreeRTOS/HAL 依赖）
 */

#include "detergent_fsm.h"
#include "safety_contract.h"
#include <string.h>

static uint32_t elapsed_ms(uint32_t now, uint32_t start)
{
    return (now - start);
}

static void set_terminal(detergent_fsm_ctx_t *ctx, detergent_fsm_output_t *out,
                          detergent_terminal_t terminal,
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

void detergent_fsm_init(detergent_fsm_ctx_t *ctx, const detergent_params_t *config)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->max_single_ms = config->max_single_ms;
    ctx->state = DETERGENT_FSM_IDLE;
    ctx->output_state = DETERGENT_OUTPUT_KNOWN_OFF;
}

esp_err_t detergent_fsm_begin_request(detergent_fsm_ctx_t *ctx,
                                        const detergent_request_t *req,
                                        uint32_t max_single_ms,
                                        uint32_t now_ms)
{
    if (ctx->state != DETERGENT_FSM_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (req->request_id == MACHINE_REQUEST_ID_INVALID) {
        return ESP_ERR_INVALID_ARG;
    }
    if (req->duration_ms == 0 || req->duration_ms > max_single_ms) {
        return ESP_ERR_INVALID_ARG;
    }

    ctx->request_id = req->request_id;
    ctx->duration_ms = req->duration_ms;
    ctx->state = DETERGENT_FSM_VALIDATING;
    ctx->state_enter_ms = now_ms;
    ctx->tick_ms = now_ms;
    ctx->terminal_event_emitted = false;
    ctx->terminal = DETERGENT_TERMINAL_NONE;
    ctx->pending_action = DETERGENT_ACTION_NONE;
    ctx->fault_code = FAULT_NONE;
    ctx->fault_severity = FAULT_SEVERITY_WARNING;
    return ESP_OK;
}

static void tick_validating(detergent_fsm_ctx_t *ctx,
                              const detergent_fsm_event_t *evt,
                              detergent_fsm_output_t *out)
{
    if (evt->fault_active || evt->emergency_active) {
        ctx->state = DETERGENT_FSM_FAULT;
        out->next_state = DETERGENT_FSM_FAULT;
        set_terminal(ctx, out, DETERGENT_TERMINAL_FAULT,
                     FAULT_INTERNAL, FAULT_SEVERITY_RECOVERABLE);
        return;
    }
    if (!evt->position_valid) {
        ctx->state = DETERGENT_FSM_FAULT;
        out->next_state = DETERGENT_FSM_FAULT;
        set_terminal(ctx, out, DETERGENT_TERMINAL_REJECTED,
                     FAULT_NONE, FAULT_SEVERITY_WARNING);
        return;
    }
    if (!evt->position_fresh || !evt->position_stable) {
        ctx->state = DETERGENT_FSM_FAULT;
        out->next_state = DETERGENT_FSM_FAULT;
        set_terminal(ctx, out, DETERGENT_TERMINAL_REJECTED,
                     FAULT_NONE, FAULT_SEVERITY_WARNING);
        return;
    }
    if (evt->position_motor_moving) {
        ctx->state = DETERGENT_FSM_FAULT;
        out->next_state = DETERGENT_FSM_FAULT;
        set_terminal(ctx, out, DETERGENT_TERMINAL_REJECTED,
                     FAULT_NONE, FAULT_SEVERITY_WARNING);
        return;
    }
    if (evt->position != DRUM_POS_0) {
        ctx->state = DETERGENT_FSM_FAULT;
        out->next_state = DETERGENT_FSM_FAULT;
        set_terminal(ctx, out, DETERGENT_TERMINAL_REJECTED,
                     FAULT_NONE, FAULT_SEVERITY_WARNING);
        return;
    }

    ctx->state = DETERGENT_FSM_RUNNING;
    ctx->state_enter_ms = evt->now_ms;
    out->next_state = DETERGENT_FSM_RUNNING;
    out->action = DETERGENT_ACTION_PUMP_ON;
    ctx->pending_action = DETERGENT_ACTION_PUMP_ON;
}

static void tick_running(detergent_fsm_ctx_t *ctx,
                           const detergent_fsm_event_t *evt,
                           detergent_fsm_output_t *out)
{
    if (elapsed_ms(evt->now_ms, ctx->state_enter_ms) >= ctx->duration_ms) {
        ctx->state = DETERGENT_FSM_STOPPING;
        out->next_state = DETERGENT_FSM_STOPPING;
        out->action = DETERGENT_ACTION_PUMP_OFF;
        ctx->pending_action = DETERGENT_ACTION_PUMP_OFF;
        return;
    }
    out->next_state = DETERGENT_FSM_RUNNING;
    out->action = DETERGENT_ACTION_NONE;
}

static void handle_action_result(detergent_fsm_ctx_t *ctx,
                                   const detergent_fsm_event_t *evt,
                                   detergent_fsm_output_t *out)
{
    if (evt->action_result_action != ctx->pending_action) {
        return;
    }

    bool ok = evt->action_result_ok;
    detergent_fsm_action_t completed = ctx->pending_action;
    ctx->pending_action = DETERGENT_ACTION_NONE;

    if (completed == DETERGENT_ACTION_PUMP_ON) {
        if (ok) {
            ctx->output_state = DETERGENT_OUTPUT_KNOWN_ON;
        } else {
            ctx->output_state = DETERGENT_OUTPUT_UNKNOWN;
            ctx->state = DETERGENT_FSM_STOPPING;
            out->next_state = DETERGENT_FSM_STOPPING;
            out->action = DETERGENT_ACTION_PUMP_OFF;
            ctx->pending_action = DETERGENT_ACTION_PUMP_OFF;
            set_terminal(ctx, out, DETERGENT_TERMINAL_FAULT,
                         FAULT_MCP_IO, FAULT_SEVERITY_RECOVERABLE);
        }
    } else if (completed == DETERGENT_ACTION_PUMP_OFF) {
        if (ok) {
            ctx->output_state = DETERGENT_OUTPUT_KNOWN_OFF;
        } else {
            ctx->output_state = DETERGENT_OUTPUT_UNKNOWN;
        }
        if (ctx->state == DETERGENT_FSM_STOPPING) {
            ctx->state = DETERGENT_FSM_COMPLETE;
            out->next_state = DETERGENT_FSM_COMPLETE;
            if (ctx->terminal == DETERGENT_TERMINAL_NONE) {
                set_terminal(ctx, out, DETERGENT_TERMINAL_COMPLETE,
                             FAULT_NONE, FAULT_SEVERITY_WARNING);
            } else {
                out->terminal = ctx->terminal;
                out->fault_code = ctx->fault_code;
                out->fault_severity = ctx->fault_severity;
                out->emit_event = !ctx->terminal_event_emitted;
            }
        } else if (ctx->state == DETERGENT_FSM_CANCELING) {
            ctx->state = DETERGENT_FSM_COMPLETE;
            out->next_state = DETERGENT_FSM_COMPLETE;
            set_terminal(ctx, out, DETERGENT_TERMINAL_CANCELED,
                         FAULT_NONE, FAULT_SEVERITY_WARNING);
        }
    }
}

void detergent_fsm_tick(detergent_fsm_ctx_t *ctx,
                         const detergent_fsm_event_t *evt,
                         detergent_fsm_output_t *out)
{
    memset(out, 0, sizeof(*out));
    out->next_state = ctx->state;
    out->action = DETERGENT_ACTION_NONE;

    if (evt->type == DETERGENT_EVT_ACTION_RESULT) {
        handle_action_result(ctx, evt, out);
        return;
    }

    if (evt->type == DETERGENT_EVT_CANCEL) {
        if (evt->cancel_request_id == ctx->request_id &&
            ctx->state == DETERGENT_FSM_RUNNING) {
            ctx->state = DETERGENT_FSM_CANCELING;
            out->next_state = DETERGENT_FSM_CANCELING;
            out->action = DETERGENT_ACTION_PUMP_OFF;
            ctx->pending_action = DETERGENT_ACTION_PUMP_OFF;
        }
        return;
    }

    if (evt->type == DETERGENT_EVT_EMERGENCY) {
        if (ctx->state != DETERGENT_FSM_IDLE &&
            ctx->state != DETERGENT_FSM_COMPLETE &&
            ctx->state != DETERGENT_FSM_FAULT) {
            ctx->state = DETERGENT_FSM_STOPPING;
            out->next_state = DETERGENT_FSM_STOPPING;
            out->action = DETERGENT_ACTION_PUMP_OFF;
            ctx->pending_action = DETERGENT_ACTION_PUMP_OFF;
            set_terminal(ctx, out, DETERGENT_TERMINAL_FAULT,
                         FAULT_INTERNAL, FAULT_SEVERITY_RECOVERABLE);
        }
        return;
    }

    /* TICK */
    ctx->tick_ms = evt->now_ms;

    switch (ctx->state) {
    case DETERGENT_FSM_VALIDATING:
        tick_validating(ctx, evt, out);
        break;
    case DETERGENT_FSM_RUNNING:
        tick_running(ctx, evt, out);
        break;
    case DETERGENT_FSM_STOPPING:
    case DETERGENT_FSM_CANCELING:
        /* Waiting for action_result */
        break;
    default:
        break;
    }
}

void detergent_fsm_mark_terminal_emitted(detergent_fsm_ctx_t *ctx)
{
    ctx->terminal_event_emitted = true;
}

detergent_state_t detergent_fsm_state_to_public(detergent_fsm_state_t fsm)
{
    switch (fsm) {
    case DETERGENT_FSM_IDLE:       return DETERGENT_STATE_IDLE;
    case DETERGENT_FSM_VALIDATING: return DETERGENT_STATE_VALIDATING;
    case DETERGENT_FSM_RUNNING:    return DETERGENT_STATE_RUNNING;
    case DETERGENT_FSM_STOPPING:   return DETERGENT_STATE_STOPPING;
    case DETERGENT_FSM_COMPLETE:   return DETERGENT_STATE_COMPLETE;
    case DETERGENT_FSM_CANCELING:  return DETERGENT_STATE_CANCELING;
    case DETERGENT_FSM_FAULT:      return DETERGENT_STATE_FAULT;
    default:                       return DETERGENT_STATE_FAULT;
    }
}

int detergent_fsm_output_to_public(detergent_output_state_t out)
{
    switch (out) {
    case DETERGENT_OUTPUT_KNOWN_OFF: return 1;
    case DETERGENT_OUTPUT_KNOWN_ON:  return 2;
    default:                         return 0; /* UNKNOWN / 未知映射 → fail-closed */
    }
}

bool detergent_fsm_terminal_pending(const detergent_fsm_ctx_t *ctx)
{
    if (!ctx) return false;
    return !ctx->terminal_event_emitted &&
           ctx->terminal != DETERGENT_TERMINAL_NONE;
}
