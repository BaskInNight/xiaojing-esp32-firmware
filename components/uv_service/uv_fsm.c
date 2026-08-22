/*
 * uv_fsm.c — UV 杀菌状态机（纯 C，无 FreeRTOS/HAL 依赖）
 */

#include "uv_fsm.h"
#include "safety_contract.h"
#include <string.h>

static uint32_t elapsed_ms(uint32_t now, uint32_t start)
{
    return (now - start);
}

static void set_terminal(uv_fsm_ctx_t *ctx, uv_fsm_output_t *out,
                          uv_terminal_t terminal,
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

void uv_fsm_init(uv_fsm_ctx_t *ctx, const uv_params_t *config)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->max_duration_ms = config->max_duration_ms;
    ctx->state = UV_FSM_IDLE;
    ctx->output_state = UV_OUTPUT_KNOWN_OFF;
}

esp_err_t uv_fsm_begin_request(uv_fsm_ctx_t *ctx, const uv_request_t *req,
                                 uint32_t max_duration_ms, uint32_t now_ms)
{
    if (ctx->state != UV_FSM_IDLE) {
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
    ctx->state = UV_FSM_VALIDATING;
    ctx->state_enter_ms = now_ms;
    ctx->tick_ms = now_ms;
    ctx->terminal_event_emitted = false;
    ctx->terminal = UV_TERMINAL_NONE;
    ctx->pending_action = UV_ACTION_NONE;
    ctx->fault_code = FAULT_NONE;
    ctx->fault_severity = FAULT_SEVERITY_WARNING;
    return ESP_OK;
}

static void tick_validating(uv_fsm_ctx_t *ctx, const uv_fsm_event_t *evt,
                              uv_fsm_output_t *out)
{
    if (evt->fault_active || evt->emergency_active) {
        ctx->state = UV_FSM_FAULT;
        out->next_state = UV_FSM_FAULT;
        set_terminal(ctx, out, UV_TERMINAL_FAULT,
                     FAULT_INTERNAL, FAULT_SEVERITY_RECOVERABLE);
        return;
    }
    if (!evt->position_valid) {
        ctx->state = UV_FSM_FAULT;
        out->next_state = UV_FSM_FAULT;
        set_terminal(ctx, out, UV_TERMINAL_REJECTED,
                     FAULT_NONE, FAULT_SEVERITY_WARNING);
        return;
    }
    if (!evt->position_fresh || !evt->position_stable) {
        ctx->state = UV_FSM_FAULT;
        out->next_state = UV_FSM_FAULT;
        set_terminal(ctx, out, UV_TERMINAL_REJECTED,
                     FAULT_NONE, FAULT_SEVERITY_WARNING);
        return;
    }
    if (evt->position_motor_moving) {
        ctx->state = UV_FSM_FAULT;
        out->next_state = UV_FSM_FAULT;
        set_terminal(ctx, out, UV_TERMINAL_REJECTED,
                     FAULT_NONE, FAULT_SEVERITY_WARNING);
        return;
    }
    if (evt->position != DRUM_POS_0) {
        ctx->state = UV_FSM_FAULT;
        out->next_state = UV_FSM_FAULT;
        set_terminal(ctx, out, UV_TERMINAL_REJECTED,
                     FAULT_NONE, FAULT_SEVERITY_WARNING);
        return;
    }

    ctx->state = UV_FSM_RUNNING;
    ctx->state_enter_ms = evt->now_ms;
    out->next_state = UV_FSM_RUNNING;
    out->action = UV_ACTION_LAMP_ON;
    ctx->pending_action = UV_ACTION_LAMP_ON;
}

static void tick_running(uv_fsm_ctx_t *ctx, const uv_fsm_event_t *evt,
                           uv_fsm_output_t *out)
{
    if (elapsed_ms(evt->now_ms, ctx->state_enter_ms) >= ctx->duration_ms) {
        ctx->state = UV_FSM_STOPPING;
        out->next_state = UV_FSM_STOPPING;
        out->action = UV_ACTION_LAMP_OFF;
        ctx->pending_action = UV_ACTION_LAMP_OFF;
        return;
    }
    out->next_state = UV_FSM_RUNNING;
    out->action = UV_ACTION_NONE;
}

static void handle_action_result(uv_fsm_ctx_t *ctx,
                                   const uv_fsm_event_t *evt,
                                   uv_fsm_output_t *out)
{
    if (evt->action_result_action != ctx->pending_action) {
        return;
    }

    bool ok = evt->action_result_ok;
    uv_fsm_action_t completed = ctx->pending_action;
    ctx->pending_action = UV_ACTION_NONE;

    if (completed == UV_ACTION_LAMP_ON) {
        if (ok) {
            ctx->output_state = UV_OUTPUT_KNOWN_ON;
        } else {
            ctx->output_state = UV_OUTPUT_UNKNOWN;
            ctx->state = UV_FSM_STOPPING;
            out->next_state = UV_FSM_STOPPING;
            out->action = UV_ACTION_LAMP_OFF;
            ctx->pending_action = UV_ACTION_LAMP_OFF;
            set_terminal(ctx, out, UV_TERMINAL_FAULT,
                         FAULT_MCP_IO, FAULT_SEVERITY_RECOVERABLE);
        }
    } else if (completed == UV_ACTION_LAMP_OFF) {
        if (ok) {
            ctx->output_state = UV_OUTPUT_KNOWN_OFF;
        } else {
            ctx->output_state = UV_OUTPUT_UNKNOWN;
        }
        if (ctx->state == UV_FSM_STOPPING) {
            ctx->state = UV_FSM_COMPLETE;
            out->next_state = UV_FSM_COMPLETE;
            if (ctx->terminal == UV_TERMINAL_NONE) {
                set_terminal(ctx, out, UV_TERMINAL_COMPLETE,
                             FAULT_NONE, FAULT_SEVERITY_WARNING);
            } else {
                out->terminal = ctx->terminal;
                out->fault_code = ctx->fault_code;
                out->fault_severity = ctx->fault_severity;
                out->emit_event = !ctx->terminal_event_emitted;
            }
        } else if (ctx->state == UV_FSM_CANCELING) {
            ctx->state = UV_FSM_COMPLETE;
            out->next_state = UV_FSM_COMPLETE;
            set_terminal(ctx, out, UV_TERMINAL_CANCELED,
                         FAULT_NONE, FAULT_SEVERITY_WARNING);
        } else if (ctx->state == UV_FSM_SKIPPING) {
            ctx->state = UV_FSM_COMPLETE;
            out->next_state = UV_FSM_COMPLETE;
            set_terminal(ctx, out, UV_TERMINAL_SKIPPED,
                         FAULT_NONE, FAULT_SEVERITY_WARNING);
        }
    }
}

void uv_fsm_tick(uv_fsm_ctx_t *ctx, const uv_fsm_event_t *evt,
                  uv_fsm_output_t *out)
{
    memset(out, 0, sizeof(*out));
    out->next_state = ctx->state;
    out->action = UV_ACTION_NONE;

    if (evt->type == UV_EVT_ACTION_RESULT) {
        handle_action_result(ctx, evt, out);
        return;
    }

    if (evt->type == UV_EVT_CANCEL) {
        if (evt->cancel_request_id == ctx->request_id &&
            (ctx->state == UV_FSM_VALIDATING ||
             ctx->state == UV_FSM_RUNNING)) {
            ctx->state = UV_FSM_CANCELING;
            out->next_state = UV_FSM_CANCELING;
            out->action = UV_ACTION_LAMP_OFF;
            ctx->pending_action = UV_ACTION_LAMP_OFF;
        }
        return;
    }

    if (evt->type == UV_EVT_SKIP) {
        if (evt->cancel_request_id == ctx->request_id &&
            (ctx->state == UV_FSM_VALIDATING ||
             ctx->state == UV_FSM_RUNNING)) {
            ctx->state = UV_FSM_SKIPPING;
            out->next_state = UV_FSM_SKIPPING;
            out->action = UV_ACTION_LAMP_OFF;
            ctx->pending_action = UV_ACTION_LAMP_OFF;
        }
        return;
    }

    if (evt->type == UV_EVT_EMERGENCY) {
        if (ctx->state != UV_FSM_IDLE &&
            ctx->state != UV_FSM_COMPLETE &&
            ctx->state != UV_FSM_FAULT) {
            ctx->state = UV_FSM_STOPPING;
            out->next_state = UV_FSM_STOPPING;
            out->action = UV_ACTION_LAMP_OFF;
            ctx->pending_action = UV_ACTION_LAMP_OFF;
            set_terminal(ctx, out, UV_TERMINAL_FAULT,
                         FAULT_INTERNAL, FAULT_SEVERITY_RECOVERABLE);
        }
        return;
    }

    /* TICK */
    ctx->tick_ms = evt->now_ms;

    switch (ctx->state) {
    case UV_FSM_VALIDATING:
        tick_validating(ctx, evt, out);
        break;
    case UV_FSM_RUNNING:
        tick_running(ctx, evt, out);
        break;
    case UV_FSM_STOPPING:
    case UV_FSM_CANCELING:
    case UV_FSM_SKIPPING:
        break;
    default:
        break;
    }
}

void uv_fsm_mark_terminal_emitted(uv_fsm_ctx_t *ctx)
{
    ctx->terminal_event_emitted = true;
}

uv_state_t uv_fsm_state_to_public(uv_fsm_state_t fsm)
{
    switch (fsm) {
    case UV_FSM_IDLE:       return UV_STATE_IDLE;
    case UV_FSM_VALIDATING: return UV_STATE_VALIDATING;
    case UV_FSM_RUNNING:    return UV_STATE_RUNNING;
    case UV_FSM_STOPPING:   return UV_STATE_STOPPING;
    case UV_FSM_COMPLETE:   return UV_STATE_COMPLETE;
    case UV_FSM_CANCELING:  return UV_STATE_CANCELING;
    case UV_FSM_SKIPPING:   return UV_STATE_SKIPPING;
    case UV_FSM_FAULT:      return UV_STATE_FAULT;
    default:                return UV_STATE_FAULT;
    }
}

int uv_fsm_output_to_public(uv_output_state_t out)
{
    switch (out) {
    case UV_OUTPUT_KNOWN_OFF: return 1;
    case UV_OUTPUT_KNOWN_ON:  return 2;
    default:                  return 0; /* UNKNOWN / 未知映射 → fail-closed */
    }
}

bool uv_fsm_terminal_pending(const uv_fsm_ctx_t *ctx)
{
    if (!ctx) return false;
    return !ctx->terminal_event_emitted &&
           ctx->terminal != UV_TERMINAL_NONE;
}
