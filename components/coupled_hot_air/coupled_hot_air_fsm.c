#include "coupled_hot_air_fsm.h"
#include <string.h>

/* ================================================================
 * coupled_hot_air_fsm.c — 耦合热风模块状态机（纯 C，无 FreeRTOS/HAL 依赖）
 *
 * 单继电器耦合模型：无预吹、无延迟关风，风扇+加热丝同步启停。
 * 硬规则见头文件。
 * ================================================================ */

static inline uint32_t elapsed_ms_int(int64_t now, int64_t start)
{
    if (now <= start) return 0;
    return (uint32_t)(now - start);
}

static void no_action(cha_fsm_output_t *out, cha_fsm_state_t state)
{
    out->next_state = state;
    out->action = CHA_ACTION_NONE;
    out->terminal = CHA_TERMINAL_NONE;
    out->reason = NULL;
    out->off_confirmed = false;
}

static void set_action(cha_fsm_ctx_t *ctx, cha_fsm_output_t *out,
                       cha_fsm_state_t state, cha_action_t action)
{
    ctx->pending_action = action;
    out->next_state = state;
    out->action = action;
    out->terminal = CHA_TERMINAL_NONE;
    out->reason = NULL;
    out->off_confirmed = false;
}

/* 由 terminal reason 字符串推导 stop_reason（与 cha_fsm_reason_name 反向映射）。
 * 未知/空字符串 → FAULT（默认停止类别）。 */
static cha_stop_reason_t stop_reason_from_string(const char *reason)
{
    if (!reason) return CHA_STOP_FAULT;
    if (strcmp(reason, "EMERGENCY") == 0)      return CHA_STOP_EMERGENCY;
    if (strcmp(reason, "FAULT_ACTIVE") == 0)   return CHA_STOP_FAULT;
    if (strcmp(reason, "POSITION_LOST") == 0)  return CHA_STOP_POSITION;
    if (strcmp(reason, "SHT_INVALID") == 0)    return CHA_STOP_SHT_INVALID;
    if (strcmp(reason, "SHT_STALE") == 0)      return CHA_STOP_SHT_STALE;
    if (strcmp(reason, "OVERTEMP") == 0)       return CHA_STOP_OVERTEMP;
    if (strcmp(reason, "COOLDOWN_ACTIVE") == 0)return CHA_STOP_COOLDOWN;
    if (strcmp(reason, "DURATION_ELAPSED") == 0) return CHA_STOP_DURATION;
    if (strcmp(reason, "CANCELED") == 0)       return CHA_STOP_CANCELED;
    if (strcmp(reason, "APPLY_FAILED") == 0)   return CHA_STOP_APPLY_FAULT;
    if (strcmp(reason, "OFF_CONFIRM_FAILED") == 0) return CHA_STOP_OFF_FAIL;
    return CHA_STOP_FAULT;
}

static void emit_terminal(cha_fsm_ctx_t *ctx, cha_fsm_output_t *out,
                          coupled_hot_air_terminal_t term, const char *reason,
                          bool off_confirmed)
{
    ctx->state = CHA_FSM_COMPLETE;
    ctx->pending_action = CHA_ACTION_NONE;
    ctx->terminal = term;
    ctx->terminal_reason = reason;
    ctx->terminal_off_confirmed = off_confirmed;
    ctx->stop_reason = stop_reason_from_string(reason);
    out->next_state = CHA_FSM_COMPLETE;
    out->action = CHA_ACTION_NONE;
    out->terminal = term;
    out->reason = reason;
    out->off_confirmed = off_confirmed;
}

const char *cha_fsm_reason_name(cha_stop_reason_t reason)
{
    switch (reason) {
    case CHA_STOP_DURATION:     return "DURATION_ELAPSED";
    case CHA_STOP_CANCELED:     return "CANCELED";
    case CHA_STOP_EMERGENCY:    return "EMERGENCY";
    case CHA_STOP_FAULT:        return "FAULT_ACTIVE";
    case CHA_STOP_POSITION:     return "POSITION_LOST";
    case CHA_STOP_SHT_INVALID:  return "SHT_INVALID";
    case CHA_STOP_SHT_STALE:    return "SHT_STALE";
    case CHA_STOP_OVERTEMP:     return "OVERTEMP";
    case CHA_STOP_COOLDOWN:     return "COOLDOWN_ACTIVE";
    case CHA_STOP_APPLY_FAULT:  return "APPLY_FAILED";
    case CHA_STOP_OFF_FAIL:     return "OFF_CONFIRM_FAILED";
    default:                    return "UNKNOWN";
    }
}

static coupled_hot_air_terminal_t terminal_for_stop(cha_stop_reason_t reason)
{
    switch (reason) {
    case CHA_STOP_DURATION:   return CHA_TERMINAL_COMPLETE;
    case CHA_STOP_CANCELED:   return CHA_TERMINAL_INTERRUPTED;
    case CHA_STOP_EMERGENCY:  return CHA_TERMINAL_INTERRUPTED;
    default:                  return CHA_TERMINAL_FAULT;
    }
}

static void enter_stopping(cha_fsm_ctx_t *ctx, cha_fsm_output_t *out,
                           cha_stop_reason_t reason)
{
    ctx->stop_reason = reason;
    ctx->off_attempts = 0;
    ctx->state = CHA_FSM_STOPPING;

    if (ctx->relay_state == CHA_OUT_OFF) {
        /* 从未通电或已确认 OFF：直接按原因发终态。 */
        emit_terminal(ctx, out, terminal_for_stop(reason),
                      cha_fsm_reason_name(reason), true);
        return;
    }
    set_action(ctx, out, CHA_FSM_STOPPING, CHA_ACTION_RELAY_OFF);
    ctx->off_attempts++;
}

static void handle_stopping(cha_fsm_ctx_t *ctx, cha_fsm_output_t *out)
{
    if (ctx->relay_state == CHA_OUT_OFF) {
        emit_terminal(ctx, out, terminal_for_stop(ctx->stop_reason),
                      cha_fsm_reason_name(ctx->stop_reason), true);
        return;
    }
    /* ON 或 UNKNOWN：重试 OFF，达到上限仍未确认 → FAULT（off=false）。
     * 不依赖 relay_ever_on：即使本次从未通电，只要 OFF 一直无法确认
     * （继电器状态保持 UNKNOWN），也必须保证重试链必然收敛到终态。 */
    if (ctx->relay_state == CHA_OUT_UNKNOWN &&
        ctx->off_attempts >= CHA_OFF_RETRY_MAX) {
        emit_terminal(ctx, out, CHA_TERMINAL_FAULT, "OFF_CONFIRM_FAILED", false);
        return;
    }
    set_action(ctx, out, CHA_FSM_STOPPING, CHA_ACTION_RELAY_OFF);
    ctx->off_attempts++;
}

static bool position_ok(const cha_fsm_ctx_t *ctx)
{
    return ctx->position_valid && ctx->position_stable && ctx->position_fresh &&
           !ctx->position_motor_moving && ctx->position == DRUM_POS_270;
}

static void handle_validating(cha_fsm_ctx_t *ctx, cha_fsm_output_t *out)
{
    if (ctx->emergency_active) {
        emit_terminal(ctx, out, CHA_TERMINAL_INTERRUPTED, "EMERGENCY", true);
        return;
    }
    if (ctx->fault_active) {
        emit_terminal(ctx, out, CHA_TERMINAL_FAULT, "FAULT_ACTIVE", true);
        return;
    }
    if (!position_ok(ctx)) {
        emit_terminal(ctx, out, CHA_TERMINAL_FAULT, "POSITION_LOST", true);
        return;
    }
    if (!ctx->sht_valid) {
        emit_terminal(ctx, out, CHA_TERMINAL_FAULT, "SHT_INVALID", true);
        return;
    }
    if (!ctx->sht_fresh) {
        emit_terminal(ctx, out, CHA_TERMINAL_FAULT, "SHT_STALE", true);
        return;
    }
    if (ctx->temperature_c >= ctx->cutoff_c) {
        emit_terminal(ctx, out, CHA_TERMINAL_FAULT, "OVERTEMP", true);
        return;
    }
    if (ctx->cooldown_remaining_ms > 0) {
        emit_terminal(ctx, out, CHA_TERMINAL_FAULT, "COOLDOWN_ACTIVE", true);
        return;
    }
    ctx->state = CHA_FSM_STARTING;
    set_action(ctx, out, CHA_FSM_STARTING, CHA_ACTION_RELAY_ON);
}

static void handle_starting_result(cha_fsm_ctx_t *ctx, cha_fsm_output_t *out,
                                   bool ok, int64_t now)
{
    if (ok) {
        ctx->relay_state = CHA_OUT_ON;
        ctx->relay_ever_on = true;
        ctx->started_at_ms = now;
        ctx->deadline_ms = now + (int64_t)ctx->duration_ms;
        ctx->state = CHA_FSM_RUNNING;
        no_action(out, CHA_FSM_RUNNING);
        return;
    }
    /* RELAY_ON 失败：命令不确定 → 置 UNKNOWN（fail-closed），走 OFF 序列。 */
    ctx->relay_state = CHA_OUT_UNKNOWN;
    enter_stopping(ctx, out, CHA_STOP_APPLY_FAULT);
}

static void handle_running(cha_fsm_ctx_t *ctx, cha_fsm_output_t *out)
{
    if (ctx->emergency_active) {
        enter_stopping(ctx, out, CHA_STOP_EMERGENCY);
        return;
    }
    if (ctx->fault_active) {
        enter_stopping(ctx, out, CHA_STOP_FAULT);
        return;
    }
    if (!position_ok(ctx)) {
        enter_stopping(ctx, out, CHA_STOP_POSITION);
        return;
    }
    if (!ctx->sht_valid) {
        enter_stopping(ctx, out, CHA_STOP_SHT_INVALID);
        return;
    }
    if (!ctx->sht_fresh) {
        enter_stopping(ctx, out, CHA_STOP_SHT_STALE);
        return;
    }
    if (ctx->temperature_c >= ctx->cutoff_c) {
        enter_stopping(ctx, out, CHA_STOP_OVERTEMP);
        return;
    }
    if (ctx->tick_ms >= ctx->deadline_ms) {
        enter_stopping(ctx, out, CHA_STOP_DURATION);
        return;
    }
    no_action(out, CHA_FSM_RUNNING);
}

static void handle_stopping_result(cha_fsm_ctx_t *ctx, cha_fsm_output_t *out,
                                   bool ok)
{
    if (ok) {
        ctx->relay_state = CHA_OUT_OFF;
    } else {
        ctx->relay_state = CHA_OUT_UNKNOWN;
    }
    handle_stopping(ctx, out);
}

/* ---- 事件处理 ---- */

static void handle_action_result(cha_fsm_ctx_t *ctx, cha_fsm_output_t *out,
                                 const cha_fsm_event_t *evt)
{
    if (ctx->pending_action == CHA_ACTION_NONE) {
        no_action(out, ctx->state);
        return;
    }
    if (evt->action != ctx->pending_action) {
        no_action(out, ctx->state);
        return;
    }
    ctx->pending_action = CHA_ACTION_NONE;

    switch (ctx->state) {
    case CHA_FSM_STARTING:
        handle_starting_result(ctx, out, evt->action_ok, evt->now_ms);
        break;
    case CHA_FSM_STOPPING:
        handle_stopping_result(ctx, out, evt->action_ok);
        break;
    default:
        no_action(out, ctx->state);
        break;
    }
}

static void handle_cancel(cha_fsm_ctx_t *ctx, cha_fsm_output_t *out,
                          const cha_fsm_event_t *evt)
{
    if (evt->cancel_request_id != ctx->request_id) {
        no_action(out, ctx->state);
        return;
    }
    if (ctx->state == CHA_FSM_IDLE || ctx->state == CHA_FSM_COMPLETE ||
        ctx->state == CHA_FSM_STOPPING) {
        no_action(out, ctx->state);
        return;
    }
    enter_stopping(ctx, out, CHA_STOP_CANCELED);
}

static void handle_emergency(cha_fsm_ctx_t *ctx, cha_fsm_output_t *out)
{
    if (ctx->state == CHA_FSM_IDLE || ctx->state == CHA_FSM_COMPLETE ||
        ctx->state == CHA_FSM_STOPPING) {
        no_action(out, ctx->state);
        return;
    }
    enter_stopping(ctx, out, CHA_STOP_EMERGENCY);
}

/* ---- 状态分发 ---- */

static void dispatch(cha_fsm_ctx_t *ctx, cha_fsm_output_t *out)
{
    switch (ctx->state) {
    case CHA_FSM_IDLE:
        no_action(out, CHA_FSM_IDLE);
        break;
    case CHA_FSM_VALIDATING:
        handle_validating(ctx, out);
        break;
    case CHA_FSM_STARTING:
        no_action(out, CHA_FSM_STARTING);
        break;
    case CHA_FSM_RUNNING:
        handle_running(ctx, out);
        break;
    case CHA_FSM_STOPPING:
        handle_stopping(ctx, out);
        break;
    case CHA_FSM_COMPLETE:
        if (ctx->terminal_emitted) {
            ctx->state = CHA_FSM_IDLE;
            no_action(out, CHA_FSM_IDLE);
        } else {
            no_action(out, CHA_FSM_COMPLETE);
        }
        break;
    default:
        no_action(out, CHA_FSM_IDLE);
        break;
    }
}

/* ---- Public API ---- */

void cha_fsm_init(cha_fsm_ctx_t *ctx, float cutoff_c)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->state = CHA_FSM_IDLE;
    ctx->relay_state = CHA_OUT_OFF;
    ctx->cutoff_c = cutoff_c;
    ctx->revision = 1;
}

void cha_fsm_begin_request(cha_fsm_ctx_t *ctx, const coupled_hot_air_request_t *req,
                           int64_t now_ms)
{
    ctx->request_id = req->request_id;
    ctx->duration_ms = req->duration_ms;
    ctx->generation++;
    ctx->tick_ms = now_ms;
    ctx->started_at_ms = now_ms;
    ctx->deadline_ms = now_ms + (int64_t)req->duration_ms;
    ctx->state = CHA_FSM_VALIDATING;
    /* 保留 relay_state 原值（诚实语义）：绝不在未知状态下把 UNKNOWN 重写为
     * OFF。若上次终态为 OFF 确认失败（relay_state==UNKNOWN），本请求的
     * RELAY_ON 会被 safety 层 "所有 MCP 输出必须 KNOWN_OFF" 门禁拒绝，走
     * fail-closed 的 OFF 重试直至 FAULT，物理继电器不会被误加电（P1-2）。 */
    ctx->relay_ever_on = false;
    ctx->stop_reason = CHA_STOP_NONE;
    ctx->off_attempts = 0;
    ctx->terminal = CHA_TERMINAL_NONE;
    ctx->terminal_reason = NULL;
    ctx->terminal_emitted = false;
    ctx->pending_action = CHA_ACTION_NONE;
    ctx->revision++;
}

void cha_fsm_tick(cha_fsm_ctx_t *ctx, const cha_fsm_event_t *evt,
                  cha_fsm_output_t *out)
{
    memset(out, 0, sizeof(*out));
    if (evt->now_ms > 0) {
        ctx->tick_ms = evt->now_ms;
    }

    if (evt->type == CHA_EVT_TICK) {
        ctx->position = evt->position;
        ctx->position_valid = evt->position_valid;
        ctx->position_stable = evt->position_stable;
        ctx->position_fresh = evt->position_fresh;
        ctx->position_motor_moving = evt->position_motor_moving;
        ctx->sht_valid = evt->sht_valid;
        ctx->sht_fresh = evt->sht_fresh;
        ctx->temperature_c = evt->temperature_c;
        ctx->emergency_active = evt->emergency_active;
        ctx->fault_active = evt->fault_active;
        ctx->cooldown_remaining_ms = evt->cooldown_remaining_ms;
    }

    if (evt->type == CHA_EVT_ACTION_RESULT) {
        handle_action_result(ctx, out, evt);
        return;
    }
    if (evt->type == CHA_EVT_CANCEL) {
        handle_cancel(ctx, out, evt);
        return;
    }
    if (evt->type == CHA_EVT_EMERGENCY) {
        handle_emergency(ctx, out);
        return;
    }

    /* pending action → 等待结果 */
    if (ctx->pending_action != CHA_ACTION_NONE) {
        no_action(out, ctx->state);
        return;
    }

    dispatch(ctx, out);
}

void cha_fsm_mark_terminal_emitted(cha_fsm_ctx_t *ctx)
{
    ctx->terminal_emitted = true;
    ctx->revision++;
}

bool cha_fsm_is_idle(const cha_fsm_ctx_t *ctx)
{
    return ctx && ctx->state == CHA_FSM_IDLE;
}

bool cha_fsm_is_terminal(const cha_fsm_ctx_t *ctx)
{
    return ctx && ctx->state == CHA_FSM_COMPLETE &&
           ctx->terminal != CHA_TERMINAL_NONE;
}
