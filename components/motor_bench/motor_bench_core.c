/*
 * motor_bench_core.c — 电机空载台架 纯 C 核心实现
 * 无 FreeRTOS/HAL/服务依赖；门禁 + 多步 FSM，host-testable。
 */

#include "motor_bench_core.h"

#include <string.h>

/* ================================================================
 * 类型
 * ================================================================ */

const char *motor_bench_type_name(motor_bench_type_t type)
{
    switch (type) {
    case MOTOR_BENCH_TYPE_PULSATOR:      return "pulsator";
    case MOTOR_BENCH_TYPE_DRUM:          return "drum";
    case MOTOR_BENCH_TYPE_HALL_SEQUENCE: return "hall_sequence";
    case MOTOR_BENCH_TYPE_PULSATOR_BEHAVIOR: return "pulsator_behavior";
    default:                             return "";
    }
}

bool motor_bench_type_from_name(const char *name, motor_bench_type_t *out)
{
    if (!name || !out) return false;
    if (strcmp(name, "pulsator") == 0)       { *out = MOTOR_BENCH_TYPE_PULSATOR;      return true; }
    if (strcmp(name, "drum") == 0)           { *out = MOTOR_BENCH_TYPE_DRUM;          return true; }
    if (strcmp(name, "hall_sequence") == 0)  { *out = MOTOR_BENCH_TYPE_HALL_SEQUENCE; return true; }
    if (strcmp(name, "pulsator_behavior") == 0) { *out = MOTOR_BENCH_TYPE_PULSATOR_BEHAVIOR; return true; }
    return false;
}

/* ================================================================
 * 默认程序
 * ================================================================ */

/* 生产动作序列（位置由 resolver 派生，不在此硬编码）：
 *   人工霍尔流程台架 = 主洗涤(0°) → 滚筒(90°) → 甩干(180°)
 *   单步自检        = 对应单动作 */
static const bl50_action_t s_seq_pulsator[] = { BL50_ACTION_PULSATOR };
static const bl50_action_t s_seq_drum[]     = { BL50_ACTION_DRUM };
static const bl50_action_t s_seq_hall[]     = { BL50_ACTION_PULSATOR,
                                                 BL50_ACTION_DRUM,
                                                 BL50_ACTION_SPIN };

bool motor_bench_build_default_program(motor_bench_type_t type,
                                       motor_bench_position_resolver_t resolver,
                                       motor_bench_step_t *out_steps,
                                       uint8_t *out_count)
{
    if (!resolver || !out_steps || !out_count) return false;

    const bl50_action_t *actions = NULL;
    uint8_t n = 0;
    switch (type) {
    case MOTOR_BENCH_TYPE_PULSATOR:
    case MOTOR_BENCH_TYPE_PULSATOR_BEHAVIOR:
        actions = s_seq_pulsator; n = 1; break;
    case MOTOR_BENCH_TYPE_DRUM:
        actions = s_seq_drum; n = 1; break;
    case MOTOR_BENCH_TYPE_HALL_SEQUENCE:
        actions = s_seq_hall; n = 3; break;
    default:
        return false;
    }

    for (uint8_t i = 0; i < n; i++) {
        out_steps[i].action = actions[i];
        out_steps[i].required_position = resolver(actions[i]);
    }
    *out_count = n;
    return true;
}

uint8_t motor_bench_type_step_count(motor_bench_type_t type)
{
    switch (type) {
    case MOTOR_BENCH_TYPE_PULSATOR:      return 1;
    case MOTOR_BENCH_TYPE_DRUM:          return 1;
    case MOTOR_BENCH_TYPE_HALL_SEQUENCE: return 3;
    case MOTOR_BENCH_TYPE_PULSATOR_BEHAVIOR: return 1;
    default:                             return 0;
    }
}

/* ================================================================
 * 门禁
 * ================================================================ */

motor_bench_gate_result_t motor_bench_gate_check(const motor_bench_gate_input_t *in)
{
    if (!in) return MOTOR_BENCH_GATE_SNAPSHOT_UNAVAILABLE;
    /* fail-closed：任一关键快照读取失败 → 不读取失败字段，直接拒绝 */
    if (in->snapshot_fail) return MOTOR_BENCH_GATE_SNAPSHOT_UNAVAILABLE;
    if (!in->exec_idle) return MOTOR_BENCH_GATE_EXECUTOR_BUSY;
    if (in->self_test_busy) return MOTOR_BENCH_GATE_SELF_TEST_BUSY;
    if (in->fault_active) return MOTOR_BENCH_GATE_FAULT_ACTIVE;
    if (in->emergency_active) return MOTOR_BENCH_GATE_EMERGENCY_ACTIVE;
    if (!in->direction_calibrated) return MOTOR_BENCH_GATE_DIRECTION_NOT_CALIBRATED;
    /* 位置视图（valid/fresh/stable 分离，stale 可拒绝） */
    if (!in->pos_valid) return MOTOR_BENCH_GATE_POSITION_UNKNOWN;
    if (!in->pos_fresh) return MOTOR_BENCH_GATE_POSITION_STALE;
    if (!in->pos_stable) return MOTOR_BENCH_GATE_POSITION_UNSTABLE;
    if (in->pos_moving || in->position_moving) return MOTOR_BENCH_GATE_MOTOR_MOVING;
    if (in->hall_conflict) return MOTOR_BENCH_GATE_HALL_CONFLICT;
    if (in->pos != in->required_pos) return MOTOR_BENCH_GATE_POSITION_MISMATCH;
    /* 电机必须空闲（BL50 回 IDLE 且 PWM=0；姿态电机未运动） */
    if (!in->bl50_idle) return MOTOR_BENCH_GATE_BL50_BUSY;
    if (in->bl50_pwm != 0U) return MOTOR_BENCH_GATE_BL50_BUSY;
    if (!in->position_service_idle) return MOTOR_BENCH_GATE_POSITION_SERVICE_BUSY;
    /* 禁止输出全 OFF（水阀/转移/排水/蠕动/UV/热风/烘干） */
    if (!in->forbidden_outputs_off) return MOTOR_BENCH_GATE_FORBIDDEN_OUTPUT_ON;
    if (!in->water_service_idle || !in->detergent_service_idle ||
        !in->drain_service_idle || !in->dry_service_idle ||
        !in->hot_air_service_idle) return MOTOR_BENCH_GATE_SERVICE_BUSY;
    if (!in->water_safe_off_synced) return MOTOR_BENCH_GATE_OUTPUT_STATE_UNKNOWN;
    if (!in->request_id_ok) return MOTOR_BENCH_GATE_REQUEST_ID_INVALID;
    return MOTOR_BENCH_GATE_OK;
}

const char *motor_bench_gate_code(motor_bench_gate_result_t result)
{
    switch (result) {
    case MOTOR_BENCH_GATE_OK:                    return "MOTOR_BENCH_ACCEPTED";
    case MOTOR_BENCH_GATE_EXECUTOR_BUSY:         return "EXECUTOR_BUSY";
    case MOTOR_BENCH_GATE_SELF_TEST_BUSY:        return "SELF_TEST_BUSY";
    case MOTOR_BENCH_GATE_FAULT_ACTIVE:          return "FAULT_ACTIVE";
    case MOTOR_BENCH_GATE_EMERGENCY_ACTIVE:      return "EMERGENCY_ACTIVE";
    case MOTOR_BENCH_GATE_DIRECTION_NOT_CALIBRATED:
                                                    return "DIRECTION_NOT_CALIBRATED";
    case MOTOR_BENCH_GATE_POSITION_UNKNOWN:      return "POSITION_UNKNOWN";
    case MOTOR_BENCH_GATE_POSITION_STALE:        return "POSITION_STALE";
    case MOTOR_BENCH_GATE_POSITION_UNSTABLE:     return "POSITION_UNSTABLE";
    case MOTOR_BENCH_GATE_MOTOR_MOVING:          return "MOTOR_MOVING";
    case MOTOR_BENCH_GATE_POSITION_MISMATCH:     return "MOTOR_POSITION_MISMATCH";
    case MOTOR_BENCH_GATE_HALL_CONFLICT:         return "HALL_CONFLICT";
    case MOTOR_BENCH_GATE_OUTPUT_STATE_UNKNOWN:  return "OUTPUT_STATE_UNKNOWN";
    case MOTOR_BENCH_GATE_FORBIDDEN_OUTPUT_ON:   return "FORBIDDEN_OUTPUT_ON";
    case MOTOR_BENCH_GATE_BL50_BUSY:             return "BL50_BUSY";
    case MOTOR_BENCH_GATE_POSITION_SERVICE_BUSY: return "POSITION_SERVICE_BUSY";
    case MOTOR_BENCH_GATE_SERVICE_BUSY:          return "SERVICE_BUSY";
    case MOTOR_BENCH_GATE_REQUEST_ID_INVALID:    return "REQUEST_ID_INVALID";
    case MOTOR_BENCH_GATE_SNAPSHOT_UNAVAILABLE:  return "MOTOR_BENCH_SNAPSHOT_UNAVAILABLE";
    default:                                     return "INTERNAL";
    }
}

/* ================================================================
 * 禁止输出确认（fail-closed 聚合）
 * ================================================================ */

bool motor_bench_forbidden_outputs_off(const motor_bench_forbidden_clauses_t *clauses)
{
    if (!clauses) return false;
    /* 基值必须为 true：若基值为 false，任何子句都不能把它"救回"，
     * forbidden_outputs_off 将恒 false，台架永远被 FORBIDDEN_OUTPUT_ON 拒绝。
     * 任一子句未确认 OFF → 整体 false（fail-closed）。 */
    bool off = true;
    off = off && clauses->water_valves_off && clauses->detergent_off &&
                clauses->drain_off;
    off = off && clauses->dry_off && clauses->hot_air_off;
    if (clauses->uv_clause_applicable) off = off && clauses->uv_off;
    return off;
}

/* self_test_busy 聚合契约（host-testable）：
 * 台架的"自检忙"仅来自外部 UV 自检 / 执行器板测。台架自身 test_active 不参与，
 * 否则受理后每 tick 自锁 SELF_TEST_BUSY → 一受理即 FAULT（P0，板测发现）。
 * 重入由服务层 request() 顶部的 test_active 检查独占。 */
bool motor_bench_external_self_test_busy(bool uv_selftest_busy,
                                         bool actuator_selftest_busy)
{
    return uv_selftest_busy || actuator_selftest_busy;
}

const char *motor_bench_state_name(motor_bench_state_t state)
{
    switch (state) {
    case MOTOR_BENCH_STATE_INACTIVE:      return "INACTIVE";
    case MOTOR_BENCH_STATE_PRECHECK:      return "PRECHECK";
    case MOTOR_BENCH_STATE_WAIT_POSITION: return "WAIT_POSITION";
    case MOTOR_BENCH_STATE_WAIT_CONFIRM:  return "WAIT_CONFIRM";
    case MOTOR_BENCH_STATE_PULSING:       return "PULSING";
    case MOTOR_BENCH_STATE_OFF_CONFIRM:   return "OFF_CONFIRM";
    case MOTOR_BENCH_STATE_COMPLETE:      return "COMPLETE";
    case MOTOR_BENCH_STATE_REJECTED:      return "REJECTED";
    case MOTOR_BENCH_STATE_INTERRUPTED:   return "INTERRUPTED";
    case MOTOR_BENCH_STATE_TIMEOUT:       return "TIMEOUT";
    case MOTOR_BENCH_STATE_FAULT:         return "FAULT";
    default:                              return "UNKNOWN";
    }
}

/* ================================================================
 * FSM
 * ================================================================ */

static uint32_t bench_elapsed(uint32_t now, uint32_t start)
{
    return now - start;  /* uint32 无符号减法，wrap-safe */
}

void motor_bench_fsm_init(motor_bench_fsm_ctx_t *ctx,
                          motor_bench_type_t type,
                          const motor_bench_step_t *steps,
                          uint8_t step_count,
                          machine_request_id_t rid)
{
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->type = type;
    if (steps && step_count > 0 && step_count <= MOTOR_BENCH_MAX_STEPS) {
        ctx->step_count = step_count;
        for (uint8_t i = 0; i < step_count; i++) ctx->steps[i] = steps[i];
    }
    ctx->current_step = 0;
    ctx->state = MOTOR_BENCH_STATE_PRECHECK;
    ctx->terminal = MOTOR_BENCH_TERMINAL_NONE;
    ctx->request_id = rid;
    ctx->last_code = "PRECHECK";
    ctx->output_confirmed_off = false;
}

bool motor_bench_fsm_is_terminal(const motor_bench_fsm_ctx_t *ctx)
{
    return ctx && ctx->terminal != MOTOR_BENCH_TERMINAL_NONE;
}

static motor_bench_state_t state_from_terminal(motor_bench_terminal_t t)
{
    switch (t) {
    case MOTOR_BENCH_TERMINAL_COMPLETE:    return MOTOR_BENCH_STATE_COMPLETE;
    case MOTOR_BENCH_TERMINAL_REJECTED:    return MOTOR_BENCH_STATE_REJECTED;
    case MOTOR_BENCH_TERMINAL_INTERRUPTED: return MOTOR_BENCH_STATE_INTERRUPTED;
    case MOTOR_BENCH_TERMINAL_TIMEOUT:     return MOTOR_BENCH_STATE_TIMEOUT;
    case MOTOR_BENCH_TERMINAL_FAULT:       return MOTOR_BENCH_STATE_FAULT;
    default:                               return MOTOR_BENCH_STATE_INACTIVE;
    }
}

static void bench_set_terminal(motor_bench_fsm_ctx_t *ctx,
                               motor_bench_fsm_output_t *out,
                               motor_bench_terminal_t t,
                               const char *code)
{
    out->terminal = true;
    out->terminal_kind = t;
    out->terminal_code = code;
    ctx->terminal = t;
    ctx->last_code = code;
    ctx->state = state_from_terminal(t);
}

/* 良性位置结果：等待中，记录原因，不推进（错误位置/多霍尔/抖动/未就位） */
static bool bench_is_benign_position(motor_bench_gate_result_t g)
{
    switch (g) {
    case MOTOR_BENCH_GATE_POSITION_UNKNOWN:
    case MOTOR_BENCH_GATE_POSITION_STALE:
    case MOTOR_BENCH_GATE_POSITION_UNSTABLE:
    case MOTOR_BENCH_GATE_MOTOR_MOVING:
    case MOTOR_BENCH_GATE_POSITION_MISMATCH:
    case MOTOR_BENCH_GATE_HALL_CONFLICT:
        return true;
    default:
        return false;
    }
}

void motor_bench_fsm_tick(motor_bench_fsm_ctx_t *ctx,
                          const motor_bench_fsm_input_t *in,
                          motor_bench_fsm_output_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!ctx || !in || ctx->terminal != MOTOR_BENCH_TERMINAL_NONE) return;

    const bool precheck = (ctx->state == MOTOR_BENCH_STATE_PRECHECK);

    /* 公共：取消在任何状态优先；急停/禁止输出不变式仅对运行中状态生效
     * （PRECHECK 阶段 emergency/禁止输出由门禁以 REJECTED 表达）。 */
    if (in->cancel_requested) {
        out->request_all_off = true;
        bench_set_terminal(ctx, out, MOTOR_BENCH_TERMINAL_INTERRUPTED, "CANCELED");
        return;
    }
    if (!precheck && in->emergency) {
        out->request_all_off = true;
        bench_set_terminal(ctx, out, MOTOR_BENCH_TERMINAL_INTERRUPTED,
                           "EMERGENCY_REQUESTED");
        return;
    }
    if (!precheck && !in->forbidden_outputs_off) {
        /* 测试绝不触发禁止输出；运行中出现即 fail-closed，全部 OFF。 */
        out->request_all_off = true;
        bench_set_terminal(ctx, out, MOTOR_BENCH_TERMINAL_FAULT,
                           "FORBIDDEN_OUTPUT_ON");
        return;
    }

    switch (ctx->state) {

    case MOTOR_BENCH_STATE_PRECHECK:
        if (in->gate == MOTOR_BENCH_GATE_OK) {
            ctx->state = MOTOR_BENCH_STATE_WAIT_POSITION;
            ctx->step_enter_ms = in->now_ms;
            ctx->last_code = "WAIT_POSITION";
        } else {
            /* 受理拒绝：无任何输出被本测试触发，不强制全关（其他子系统可能
             * 正在合法运行）；以 REJECTED 终态返回门禁码。 */
            bench_set_terminal(ctx, out, MOTOR_BENCH_TERMINAL_REJECTED,
                               motor_bench_gate_code(in->gate));
        }
        break;

    case MOTOR_BENCH_STATE_WAIT_POSITION: {
        if (in->gate == MOTOR_BENCH_GATE_OK) {
            /* 用户已手动摆放到位且稳定 → 进入等待确认 */
            ctx->state = MOTOR_BENCH_STATE_WAIT_CONFIRM;
            ctx->step_enter_ms = in->now_ms;
            ctx->last_code = "POSITION_READY";
            break;
        }
        if (bench_is_benign_position(in->gate)) {
            /* 记录原因，不推进（错误位置/多霍尔/抖动/未就位） */
            ctx->last_code = motor_bench_gate_code(in->gate);
            /* 仍在此步等待：位置等待超时 → fail-closed TIMEOUT + 全 OFF */
            if (bench_elapsed(in->now_ms, ctx->step_enter_ms) >
                MOTOR_BENCH_POSITION_WAIT_TIMEOUT_MS) {
                out->request_all_off = true;
                bench_set_terminal(ctx, out, MOTOR_BENCH_TERMINAL_TIMEOUT,
                                   "POSITION_TIMEOUT");
            }
            break;
        }
        /* 致命门禁 → fail-closed FAULT + 全 OFF */
        out->request_all_off = true;
        bench_set_terminal(ctx, out, MOTOR_BENCH_TERMINAL_FAULT,
                           motor_bench_gate_code(in->gate));
        break;
    }

    case MOTOR_BENCH_STATE_WAIT_CONFIRM: {
        if (bench_is_benign_position(in->gate)) {
            /* 位置回退 → 重新等待（超时重新计时） */
            ctx->state = MOTOR_BENCH_STATE_WAIT_POSITION;
            ctx->step_enter_ms = in->now_ms;
            ctx->last_code = motor_bench_gate_code(in->gate);
            break;
        }
        if (in->gate != MOTOR_BENCH_GATE_OK) {
            out->request_all_off = true;
            bench_set_terminal(ctx, out, MOTOR_BENCH_TERMINAL_FAULT,
                               motor_bench_gate_code(in->gate));
            break;
        }
        /* 位置保持 OK：等待用户显式确认后提交一次脉冲 */
        if (in->user_confirm_pulse && in->bl50_idle && !ctx->pulse_issued) {
            out->submit_pulse = true;
            ctx->pulse_issued = true;
            ctx->pulse_issued_ms = in->now_ms;
            ctx->state = MOTOR_BENCH_STATE_PULSING;
            ctx->last_code = "PULSING";
            break;
        }
        if (bench_elapsed(in->now_ms, ctx->step_enter_ms) >
            MOTOR_BENCH_CONFIRM_TIMEOUT_MS) {
            out->request_all_off = true;
            bench_set_terminal(ctx, out, MOTOR_BENCH_TERMINAL_TIMEOUT,
                               "CONFIRM_TIMEOUT");
            break;
        }
        break;
    }

    case MOTOR_BENCH_STATE_PULSING: {
        if (in->bl50_fault) {
            out->request_all_off = true;
            bench_set_terminal(ctx, out, MOTOR_BENCH_TERMINAL_FAULT, "BL50_FAULT");
            break;
        }
        if (in->submit_failed) {
            out->request_all_off = true;
            bench_set_terminal(ctx, out, MOTOR_BENCH_TERMINAL_FAULT, "SUBMIT_FAILED");
            break;
        }
        if (!in->bl50_idle) {
            /* 已受理并运行（脉冲进行中） */
            ctx->pulse_seen_running = true;
            break;
        }
        if (ctx->pulse_seen_running) {
            /* ZS-X11B 已自动回 IDLE（5s 测试 + ramp/coast stop）→ 确认 OFF */
            ctx->state = MOTOR_BENCH_STATE_OFF_CONFIRM;
            ctx->last_code = "OFF_CONFIRM";
            break;
        }
        /* 提交后尚未见 BL50 非 IDLE：受理宽限内等待，超宽限判提交失败 */
        if (bench_elapsed(in->now_ms, ctx->pulse_issued_ms) >
            MOTOR_BENCH_PULSE_ACCEPT_GRACE_MS) {
            out->request_all_off = true;
            bench_set_terminal(ctx, out, MOTOR_BENCH_TERMINAL_FAULT, "SUBMIT_FAILED");
            break;
        }
        /* 行为模式跑 ~15s 交替；单方向5s测试另留停止完成宽限。 */
        {
            uint32_t pulse_timeout =
                (ctx->type == MOTOR_BENCH_TYPE_PULSATOR_BEHAVIOR)
                    ? MOTOR_BENCH_BEHAVIOR_TIMEOUT_MS
                    : MOTOR_BENCH_PULSE_TIMEOUT_MS;
            if (bench_elapsed(in->now_ms, ctx->pulse_issued_ms) > pulse_timeout) {
                out->request_all_off = true;
                bench_set_terminal(ctx, out, MOTOR_BENCH_TERMINAL_FAULT,
                                   "PULSE_TIMEOUT");
                break;
            }
        }
        break;
    }

    case MOTOR_BENCH_STATE_OFF_CONFIRM: {
        if (in->bl50_fault) {
            out->request_all_off = true;
            bench_set_terminal(ctx, out, MOTOR_BENCH_TERMINAL_FAULT, "BL50_FAULT");
            break;
        }
        if (in->bl50_idle && in->bl50_pwm == 0U) {
            /* 本步输出已确认关闭 → 推进 */
            if (ctx->current_step + 1U >= ctx->step_count) {
                ctx->output_confirmed_off = true;
                bench_set_terminal(ctx, out, MOTOR_BENCH_TERMINAL_COMPLETE,
                                   "MOTOR_BENCH_COMPLETE");
                break;
            }
            ctx->current_step++;
            ctx->step_enter_ms = in->now_ms;
            ctx->pulse_issued = false;
            ctx->pulse_seen_running = false;
            ctx->state = MOTOR_BENCH_STATE_WAIT_POSITION;
            ctx->last_code = "WAIT_POSITION";
            break;
        }
        if (bench_elapsed(in->now_ms, ctx->pulse_issued_ms) >
            MOTOR_BENCH_OFF_CONFIRM_TIMEOUT_MS) {
            /* 输出确认失败 → fail-closed：强制全 OFF + FAULT */
            out->request_all_off = true;
            bench_set_terminal(ctx, out, MOTOR_BENCH_TERMINAL_FAULT,
                               "OFF_CONFIRM_FAILED");
            break;
        }
        break;
    }

    default:
        /* 终态/未初始化：无操作 */
        break;
    }
}
