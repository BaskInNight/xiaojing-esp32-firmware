#include "bl50_fsm.h"
#include <string.h>

/* ================================================================
 * bl50_fsm.c — BL50 纯 C 状态机
 * 无 FreeRTOS/HAL/GPIO/LEDC 依赖
 * 所有时间使用 uint32_t now_ms，tick wrap-safe
 * ================================================================ */

/* ---- 姿态-动作映射 ---- */

drum_position_t bl50_action_to_position(bl50_action_t action)
{
    switch (action) {
    case BL50_ACTION_HOME:      return DRUM_POS_45;
    case BL50_ACTION_PULSATOR:  return DRUM_POS_0;
    case BL50_ACTION_DRUM:      return DRUM_POS_90;
    case BL50_ACTION_SPIN:      return DRUM_POS_180;
    default:                    return DRUM_POS_UNKNOWN;
    }
}

/* ---- 时间辅助 (wrap-safe) ---- */

static uint32_t elapsed_ms(uint32_t now, uint32_t start)
{
    return now - start;  /* uint32 无符号减法，自动 wrap-safe */
}

/* ---- 初始化 ---- */

void bl50_fsm_init(bl50_fsm_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->state = BL50_SVC_STATE_IDLE;
    ctx->request_id = MACHINE_REQUEST_ID_INVALID;
}

/* ---- begin_request ---- */

esp_err_t bl50_fsm_begin_request(bl50_fsm_ctx_t *ctx,
                                  const bl50_request_t *request,
                                  uint8_t home_pwm,
                                  uint32_t home_timeout_ms,
                                  uint32_t max_action_duration_ms,
                                  bool direction_calibrated,
                                  bool is_fake_mode)
{
    if (ctx->state != BL50_SVC_STATE_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!request) {
        return ESP_ERR_INVALID_ARG;
    }
    if (request->request_id == MACHINE_REQUEST_ID_INVALID) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((int)request->action < 0 || request->action > BL50_ACTION_SPIN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (request->duration_ms == 0 && request->action != BL50_ACTION_HOME) {
        return ESP_ERR_INVALID_ARG;
    }
    if (request->target_pwm_percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (request->duration_ms > max_action_duration_ms) {
        return ESP_ERR_INVALID_ARG;
    }

    /* REAL 模式未标定拒绝运动（HOME 可能需要方向标定） */
    if (!is_fake_mode && !direction_calibrated) {
        return ESP_ERR_INVALID_STATE;
    }

    ctx->request_id = request->request_id;
    ctx->action = request->action;
    ctx->duration_ms = request->duration_ms;
    ctx->target_pwm_percent = request->target_pwm_percent;
    ctx->direction_interval_ms = request->direction_interval_ms;
    ctx->current_pwm_percent = 0;
    ctx->running_cw = true;
    ctx->total_run_ms = 0;
    ctx->run_start_ms = 0;
    ctx->ramp_step = 0;
    ctx->home_sensor_triggered = false;
    ctx->terminal_event_emitted = false;
    ctx->terminal_result = SERVICE_RESULT_OK;
    ctx->terminal_fault_code = FAULT_NONE;
    ctx->terminal_fault_detail = 0;
    ctx->pending_action = BL50_FSM_ACTION_NONE;
    ctx->pending_action_result = BL50_FSM_ACTION_RESULT_NONE;
    ctx->pending_action_id = 0;
    ctx->state = BL50_SVC_STATE_VALIDATING;
    return ESP_OK;
}

/* ---- 输出辅助 ---- */

static void set_output_stop(bl50_fsm_output_t *out)
{
    out->action = BL50_FSM_ACTION_STOP;
    out->pwm_percent = 0;
}

static void set_output_brake(bl50_fsm_output_t *out)
{
    out->action = BL50_FSM_ACTION_BRAKE;
    out->pwm_percent = 0;
}

static void set_output_run_cw(bl50_fsm_output_t *out, uint8_t pwm)
{
    out->action = BL50_FSM_ACTION_RUN_CW;
    out->pwm_percent = pwm;
}

static void set_output_run_ccw(bl50_fsm_output_t *out, uint8_t pwm)
{
    out->action = BL50_FSM_ACTION_RUN_CCW;
    out->pwm_percent = pwm;
}

static void set_terminal(bl50_fsm_output_t *out,
                          bl50_fsm_ctx_t *ctx,
                          service_result_t result,
                          machine_fault_code_t fault_code,
                          uint32_t fault_detail)
{
    out->terminal = true;
    out->emit_event = true;
    out->event_result = result;
    out->fault_code = fault_code;
    out->fault_detail = fault_detail;
    ctx->terminal_result = result;
    ctx->terminal_fault_code = fault_code;
    ctx->terminal_fault_detail = fault_detail;
}

static void transition(bl50_fsm_ctx_t *ctx, uint8_t new_state, uint32_t now_ms)
{
    ctx->prev_state = ctx->state;
    ctx->state = new_state;
    ctx->state_enter_ms = now_ms;
}

/* ---- VALIDATING handler ---- */

static void handle_validating(bl50_fsm_ctx_t *ctx,
                               uint32_t now_ms,
                               bool position_valid,
                               bool position_stable,
                               drum_position_t current_position,
                               bl50_fsm_output_t *out)
{
    drum_position_t required = bl50_action_to_position((bl50_action_t)ctx->action);

    /* 位置必须有效、稳定且匹配 */
    if (!position_valid || current_position == DRUM_POS_UNKNOWN) {
        ctx->fault.code = FAULT_POSITION_DRIVER;
        ctx->fault.severity = FAULT_SEVERITY_RECOVERABLE;
        ctx->fault.detail = (uint32_t)current_position;
        set_output_stop(out);
        transition(ctx, BL50_SVC_STATE_STOPPING, now_ms);
        return;
    }
    if (!position_stable || current_position != required) {
        ctx->fault.code = FAULT_POSITION_DRIVER;
        ctx->fault.severity = FAULT_SEVERITY_RECOVERABLE;
        ctx->fault.detail = (uint32_t)current_position;
        set_output_stop(out);
        transition(ctx, BL50_SVC_STATE_STOPPING, now_ms);
        return;
    }

    /* 位置正确，进入动作状态。run_start_ms 记录本次请求运行起点，
     * 作为跨换向总时长的基准（反转运行也据此强制到点停止）。 */
    ctx->run_start_ms = now_ms;
    switch ((bl50_action_t)ctx->action) {
    case BL50_ACTION_HOME:
        transition(ctx, BL50_SVC_STATE_HOMING, now_ms);
        break;
    case BL50_ACTION_PULSATOR:
    case BL50_ACTION_DRUM:
    case BL50_ACTION_SPIN:
        transition(ctx, BL50_SVC_STATE_RAMPING_UP, now_ms);
        ctx->ramp_step = 0;
        break;
    }
}

/* ---- HOMING handler ---- */

static void handle_homing(bl50_fsm_ctx_t *ctx,
                           uint32_t now_ms,
                           bool home_sensor_triggered,
                           bl50_fsm_output_t *out)
{
    uint32_t timeout = ctx->duration_ms > 0 ? ctx->duration_ms : BL50_DEFAULT_HOME_TIMEOUT_MS;

    /* 传感器已触发 → 停止并完成 */
    if (home_sensor_triggered || ctx->home_sensor_triggered) {
        set_output_brake(out);
        transition(ctx, BL50_SVC_STATE_BRAKING, now_ms);
        return;
    }

    /* 超时 → 停止并故障 */
    if (elapsed_ms(now_ms, ctx->state_enter_ms) >= timeout) {
        ctx->fault.code = FAULT_BL50_HOME_TIMEOUT;
        ctx->fault.severity = FAULT_SEVERITY_RECOVERABLE;
        ctx->fault.detail = timeout; /* 实际超时时长 */
        set_output_brake(out);
        transition(ctx, BL50_SVC_STATE_BRAKING, now_ms);
        return;
    }

    /* 低速运行（HOME 默认 CW，由配置确定） */
    if (ctx->current_pwm_percent == 0) {
        set_output_run_cw(out, BL50_DEFAULT_HOME_PWM);
        ctx->current_pwm_percent = BL50_DEFAULT_HOME_PWM;
    }
}

/* ---- RAMPING_UP handler ---- */

static void handle_ramping_up(bl50_fsm_ctx_t *ctx,
                               uint32_t now_ms,
                               bl50_fsm_output_t *out)
{
    uint32_t interval = BL50_DEFAULT_RAMP_INTERVAL_MS;
    uint8_t step_size = BL50_DEFAULT_RAMP_STEP_PERCENT;
    uint8_t target = ctx->target_pwm_percent > 0 ? ctx->target_pwm_percent : 40;

    if (elapsed_ms(now_ms, ctx->state_enter_ms) < interval && ctx->ramp_step > 0) {
        return; /* 等待间隔 */
    }

    ctx->ramp_step++;
    uint8_t next_pwm = (uint8_t)((uint32_t)ctx->ramp_step * step_size);
    if (next_pwm >= target) {
        next_pwm = target;
    }

    ctx->current_pwm_percent = next_pwm;

    /* 输出正确方向 */
    if (ctx->running_cw) {
        set_output_run_cw(out, next_pwm);
    } else {
        set_output_run_ccw(out, next_pwm);
    }

    if (next_pwm >= target) {
        /* 达到目标，进入运行状态 */
        transition(ctx, ctx->running_cw ? BL50_SVC_STATE_RUNNING_CW : BL50_SVC_STATE_RUNNING_CCW, now_ms);
    }
}

/* ---- RUNNING_CW handler ---- */

static void handle_running_cw(bl50_fsm_ctx_t *ctx,
                               uint32_t now_ms,
                               bl50_fsm_output_t *out)
{
    uint32_t dir_interval = ctx->direction_interval_ms > 0 ?
                            ctx->direction_interval_ms : BL50_DEFAULT_PULSATOR_DIR_MS;

    /* 总时长检查：从 run_start_ms 起算（跨换向累计），反转运行也强制到点停止 */
    if (ctx->duration_ms > 0 &&
        elapsed_ms(now_ms, ctx->run_start_ms) >= ctx->duration_ms) {
        set_output_brake(out);
        transition(ctx, BL50_SVC_STATE_BRAKING, now_ms);
        return;
    }

    /* SPIN: 单方向，不需要反转 */
    if ((bl50_action_t)ctx->action == BL50_ACTION_SPIN) {
        return; /* 保持 CW 运行 */
    }

    /* PULSATOR/DRUM: 方向交替 */
    if (elapsed_ms(now_ms, ctx->state_enter_ms) >= dir_interval) {
        set_output_run_cw(out, 0); /* signal to stop for reversal */
        transition(ctx, BL50_SVC_STATE_RAMPING_DOWN, now_ms);
    }
}

/* ---- RAMPING_DOWN handler ---- */

static void handle_ramping_down(bl50_fsm_ctx_t *ctx,
                                 uint32_t now_ms,
                                 bl50_fsm_output_t *out)
{
    uint32_t interval = BL50_DEFAULT_RAMP_INTERVAL_MS;
    uint8_t step_size = BL50_DEFAULT_RAMP_STEP_PERCENT;

    if (elapsed_ms(now_ms, ctx->state_enter_ms) < interval && ctx->ramp_step > 0) {
        return;
    }

    if (ctx->current_pwm_percent <= step_size) {
        ctx->current_pwm_percent = 0;
        set_output_stop(out);
        transition(ctx, BL50_SVC_STATE_REVERSAL_WAIT, now_ms);
        ctx->ramp_step = 0;
        return;
    }

    ctx->current_pwm_percent -= step_size;
    /* 输出正确方向 */
    if (ctx->running_cw) {
        set_output_run_cw(out, ctx->current_pwm_percent);
    } else {
        set_output_run_ccw(out, ctx->current_pwm_percent);
    }
    ctx->ramp_step = 1; /* mark as mid-ramp */
}

/* ---- REVERSAL_WAIT handler ---- */

static void handle_reversal_wait(bl50_fsm_ctx_t *ctx,
                                  uint32_t now_ms,
                                  bl50_fsm_output_t *out)
{
    (void)out;
    uint32_t wait_ms = BL50_DEFAULT_REVERSAL_STOP_MS;

    if (elapsed_ms(now_ms, ctx->state_enter_ms) >= wait_ms) {
        /* 切换方向 */
        ctx->running_cw = !ctx->running_cw;
        ctx->ramp_step = 0;
        transition(ctx, BL50_SVC_STATE_RAMPING_UP, now_ms);
    }
}

/* ---- RUNNING_CCW handler ---- */

static void handle_running_ccw(bl50_fsm_ctx_t *ctx,
                                uint32_t now_ms,
                                bl50_fsm_output_t *out)
{
    uint32_t dir_interval = ctx->direction_interval_ms > 0 ?
                            ctx->direction_interval_ms : BL50_DEFAULT_PULSATOR_DIR_MS;

    /* 总时长检查：从 run_start_ms 起算（跨换向累计），反转运行也强制到点停止 */
    if (ctx->duration_ms > 0 &&
        elapsed_ms(now_ms, ctx->run_start_ms) >= ctx->duration_ms) {
        set_output_brake(out);
        transition(ctx, BL50_SVC_STATE_BRAKING, now_ms);
        return;
    }

    /* CCW 运行时间到达方向间隔 → 回到 CW */
    if (elapsed_ms(now_ms, ctx->state_enter_ms) >= dir_interval) {
        transition(ctx, BL50_SVC_STATE_RAMPING_DOWN, now_ms);
        ctx->ramp_step = 0;
    }
}

/* ---- BRAKING handler ---- */

static void handle_braking(bl50_fsm_ctx_t *ctx,
                            uint32_t now_ms,
                            bl50_fsm_output_t *out)
{
    uint32_t brake_ms = BL50_DEFAULT_REVERSAL_STOP_MS;
    uint32_t elapsed = elapsed_ms(now_ms, ctx->state_enter_ms);

    if (elapsed >= brake_ms) {
        set_output_stop(out);
        transition(ctx, BL50_SVC_STATE_STOPPING, now_ms);
    }
}

/* ---- STOPPING handler ---- */

static void handle_stopping(bl50_fsm_ctx_t *ctx,
                             uint32_t now_ms,
                             bl50_fsm_output_t *out)
{
    /* 等待 STOP 命令确认后才发终端 */
    if (ctx->pending_action == BL50_FSM_ACTION_STOP &&
        ctx->pending_action_result == BL50_FSM_ACTION_RESULT_OK) {
        if (ctx->fault.code != FAULT_NONE) {
            transition(ctx, BL50_SVC_STATE_FAULT, now_ms);
            set_terminal(out, ctx, SERVICE_RESULT_FAULT,
                        ctx->fault.code, ctx->fault.detail);
        } else {
            transition(ctx, BL50_SVC_STATE_COMPLETE, now_ms);
            set_terminal(out, ctx, SERVICE_RESULT_OK, FAULT_NONE, 0);
        }
    } else if (ctx->pending_action == BL50_FSM_ACTION_STOP &&
               ctx->pending_action_result == BL50_FSM_ACTION_RESULT_FAILED) {
        /* STOP HAL failed — enter FAULT regardless of prior fault code */
        if (ctx->fault.code == FAULT_NONE) {
            ctx->fault.code = FAULT_BL50;
            ctx->fault.severity = FAULT_SEVERITY_RECOVERABLE;
        }
        transition(ctx, BL50_SVC_STATE_FAULT, now_ms);
        set_terminal(out, ctx, SERVICE_RESULT_FAULT,
                    ctx->fault.code, ctx->fault.detail);
    } else if (ctx->pending_action == BL50_FSM_ACTION_STOP &&
               ctx->pending_action_result == BL50_FSM_ACTION_RESULT_NONE) {
        /* bl50_fsm_fail injected STOP — output it so task executes */
        set_output_stop(out);
    }
}

/* ---- COMPLETE/FAULT handler ---- */

static void handle_terminal_state(bl50_fsm_ctx_t *ctx,
                                   uint32_t now_ms,
                                   bl50_fsm_output_t *out)
{
    if (ctx->terminal_event_emitted) {
        ctx->prev_state = ctx->state;
        ctx->request_id = MACHINE_REQUEST_ID_INVALID;
        ctx->state = BL50_SVC_STATE_IDLE;
        /* 输出已确认为 stop（out->pwm_percent=0）。同步内部跟踪值，使
         * 快照在 IDLE 时如实报 pwm=0 —— 否则 BRAKING→STOPPING→COMPLETE
         * 路径残留 ramp 值，电机空载台架的 OFF_CONFIRM（bl50_idle &&
         * bl50_pwm==0）会永远失败。 */
        ctx->current_pwm_percent = 0;
    }
}

/* ---- 主 tick ---- */

void bl50_fsm_tick(bl50_fsm_ctx_t *ctx,
                    uint32_t now_ms,
                    bool home_sensor_triggered,
                    bool position_valid,
                    bool position_stable,
                    drum_position_t current_position,
                    bl50_fsm_output_t *out)
{
    out->action = BL50_FSM_ACTION_NONE;
    out->pwm_percent = 0;
    out->terminal = false;
    out->emit_event = false;
    out->event_result = SERVICE_RESULT_OK;
    out->fault_code = FAULT_NONE;
    out->fault_detail = 0;

    /* 保存 home 传感器状态 */
    if (home_sensor_triggered) {
        ctx->home_sensor_triggered = true;
    }

    /* 检查总时长超限（非 IDLE/VALIDATING/COMPLETE/FAULT） */
    if (ctx->state != BL50_SVC_STATE_IDLE &&
        ctx->state != BL50_SVC_STATE_VALIDATING &&
        ctx->state != BL50_SVC_STATE_COMPLETE &&
        ctx->state != BL50_SVC_STATE_FAULT &&
        ctx->action != BL50_ACTION_HOME) {
        uint32_t max_ms = ctx->duration_ms > 0 ? ctx->duration_ms * 2 : BL50_DEFAULT_MAX_ACTION_MS;
        if (elapsed_ms(now_ms, ctx->home_start_ms > 0 ? ctx->home_start_ms : ctx->state_enter_ms) > max_ms) {
            ctx->fault.code = FAULT_BL50;
            ctx->fault.severity = FAULT_SEVERITY_RECOVERABLE;
            ctx->fault.detail = 2; /* max duration exceeded */
            set_output_brake(out);
            transition(ctx, BL50_SVC_STATE_STOPPING, now_ms);
            return;
        }
    }

    switch ((bl50_service_state_t)ctx->state) {
    case BL50_SVC_STATE_IDLE:
        break;

    case BL50_SVC_STATE_VALIDATING:
        handle_validating(ctx, now_ms, position_valid, position_stable,
                         current_position, out);
        /* 如果从 VALIDATING 进入 HOMING，记录 home_start */
        if (ctx->state == BL50_SVC_STATE_HOMING) {
            ctx->home_start_ms = now_ms;
        }
        break;

    case BL50_SVC_STATE_HOMING:
        handle_homing(ctx, now_ms, home_sensor_triggered, out);
        break;

    case BL50_SVC_STATE_RAMPING_UP:
        handle_ramping_up(ctx, now_ms, out);
        break;

    case BL50_SVC_STATE_RUNNING_CW:
        handle_running_cw(ctx, now_ms, out);
        break;

    case BL50_SVC_STATE_RAMPING_DOWN:
        handle_ramping_down(ctx, now_ms, out);
        break;

    case BL50_SVC_STATE_REVERSAL_WAIT:
        handle_reversal_wait(ctx, now_ms, out);
        break;

    case BL50_SVC_STATE_RUNNING_CCW:
        handle_running_ccw(ctx, now_ms, out);
        break;

    case BL50_SVC_STATE_BRAKING:
        handle_braking(ctx, now_ms, out);
        break;

    case BL50_SVC_STATE_STOPPING:
        handle_stopping(ctx, now_ms, out);
        break;

    case BL50_SVC_STATE_CANCELING:
        /* cancel 已经在 pending 中，等待当前动作确认 */
        if (ctx->pending_action == BL50_FSM_ACTION_STOP &&
            ctx->pending_action_result == BL50_FSM_ACTION_RESULT_OK) {
            transition(ctx, BL50_SVC_STATE_COMPLETE, now_ms);
            set_terminal(out, ctx, SERVICE_RESULT_CANCELED, FAULT_NONE, 0);
        }
        break;

    case BL50_SVC_STATE_COMPLETE:
    case BL50_SVC_STATE_FAULT:
        handle_terminal_state(ctx, now_ms, out);
        break;
    }

    /* 如果有新动作需要执行，记录 pending */
    if (out->action != BL50_FSM_ACTION_NONE) {
        ctx->pending_action = out->action;
        ctx->pending_action_result = BL50_FSM_ACTION_RESULT_NONE;
        ctx->pending_action_id++;
    }
}

/* ---- 提交动作结果 ---- */

void bl50_fsm_commit_result(bl50_fsm_ctx_t *ctx,
                             bl50_fsm_action_result_t result)
{
    ctx->pending_action_result = result;
}

/* ---- 取消 ---- */

bool bl50_fsm_cancel(bl50_fsm_ctx_t *ctx, machine_request_id_t request_id)
{
    if (ctx->request_id != request_id) {
        return false;
    }
    if (ctx->state == BL50_SVC_STATE_IDLE ||
        ctx->state == BL50_SVC_STATE_COMPLETE ||
        ctx->state == BL50_SVC_STATE_FAULT) {
        return false;
    }

    ctx->fault.code = FAULT_NONE;
    ctx->state = BL50_SVC_STATE_CANCELING;
    ctx->pending_action = BL50_FSM_ACTION_STOP;
    ctx->pending_action_result = BL50_FSM_ACTION_RESULT_NONE;
    return true;
}

/* ---- 急停 ---- */

void bl50_fsm_emergency_stop(bl50_fsm_ctx_t *ctx)
{
    if (ctx->state == BL50_SVC_STATE_IDLE ||
        ctx->state == BL50_SVC_STATE_COMPLETE ||
        ctx->state == BL50_SVC_STATE_FAULT) {
        return;
    }
    ctx->state = BL50_SVC_STATE_STOPPING;
    ctx->pending_action = BL50_FSM_ACTION_STOP;
    ctx->pending_action_result = BL50_FSM_ACTION_RESULT_NONE;
    ctx->fault.code = FAULT_BL50;
    ctx->fault.severity = FAULT_SEVERITY_RECOVERABLE;
    ctx->fault.detail = 99; /* emergency */
}

/* ---- 故障（带明确身份） ---- */

void bl50_fsm_fail(bl50_fsm_ctx_t *ctx,
                   machine_fault_code_t code,
                   uint32_t detail)
{
    if (ctx->state == BL50_SVC_STATE_IDLE ||
        ctx->state == BL50_SVC_STATE_COMPLETE ||
        ctx->state == BL50_SVC_STATE_FAULT) {
        return;
    }
    ctx->state = BL50_SVC_STATE_STOPPING;
    ctx->pending_action = BL50_FSM_ACTION_STOP;
    ctx->pending_action_result = BL50_FSM_ACTION_RESULT_NONE;
    ctx->fault.code = code;
    ctx->fault.severity = FAULT_SEVERITY_RECOVERABLE;
    ctx->fault.detail = detail;
}

/* ---- 终态标记 ---- */

void bl50_fsm_mark_terminal_emitted(bl50_fsm_ctx_t *ctx)
{
    ctx->terminal_event_emitted = true;
}

/* ---- 查询 ---- */

bl50_service_state_t bl50_fsm_get_state(const bl50_fsm_ctx_t *ctx)
{
    return (bl50_service_state_t)ctx->state;
}

machine_request_id_t bl50_fsm_get_request_id(const bl50_fsm_ctx_t *ctx)
{
    return ctx->request_id;
}
