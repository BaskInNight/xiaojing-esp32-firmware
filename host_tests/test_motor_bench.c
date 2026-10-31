/*
 * test_motor_bench.c — 电机空载台架 core FSM + 门禁 host 测试
 *
 * 编译真实生产源：
 *   - components/motor_bench/motor_bench_core.c   （本组件纯 C 核心）
 *   - components/bl50_service/bl50_fsm.c           （生产动作→位置契约，单一权威来源）
 *
 * 覆盖（Phase 11 Group C Round 0 rule #8）：
 *   正确顺序 / 错误位置拒绝 / 多霍尔拒绝 / 位置与确认超时 / 取消 / 急停 /
 *   BLE 断线(emergency) / 输出重叠=0 / 禁止输出=0 / 终态 exactly-once /
 *   ≥10k 随机压力（完整 3 步序列 10k 轮 + 随机游走 10k 轮，逐 tick 不变式校验）。
 *
 * 位置语义一律由 bl50_action_to_position()（生产契约）派生，测试内不硬编码
 * 霍尔顺序猜测 —— 仅在断言中校验契约常量本身（PULSATOR→0° / DRUM→90° / SPIN→180°）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "motor_bench_core.h"
#include "bl50_fsm.h"   /* bl50_action_to_position：生产动作→位置权威映射 */

/* ---- 轻量测试框架 ---- */
static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("  TEST %02d: %-56s ", tests_run, name); \
} while (0)

#define PASS() do { tests_passed++; printf("PASS\n"); } while (0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while (0)

/* ---- 确定性 PRNG（xorshift32，可复现） ---- */
static uint32_t rng_state;

static void rng_seed(uint32_t s) { rng_state = s ? s : 0x9E3779B9u; }
static uint32_t rng_next(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng_state = x;
    return x;
}

/* ---- 门禁输入构造 ---- */

static uint8_t hall_mask_for(drum_position_t pos)
{
    switch (pos) {
    case DRUM_POS_0:   return 0x01;
    case DRUM_POS_45:  return 0x02;
    case DRUM_POS_90:  return 0x04;
    case DRUM_POS_180: return 0x08;
    case DRUM_POS_270: return 0x10;
    default:           return 0;
    }
}

/* 全默认健康输入；required_pos/pos 由调用方设为一致即通过门禁。 */
static motor_bench_gate_input_t make_gate(void)
{
    motor_bench_gate_input_t g;
    memset(&g, 0, sizeof(g));
    g.type = MOTOR_BENCH_TYPE_PULSATOR;
    g.required_pos = DRUM_POS_0;
    g.pos_valid = true;
    g.pos_fresh = true;
    g.pos_stable = true;
    g.pos_moving = false;
    g.pos = DRUM_POS_0;
    g.raw_hall_mask = 0x01;
    g.stable_hall_mask = 0x01;
    g.hall_conflict = false;
    g.fault_active = false;
    g.emergency_active = false;
    g.direction_calibrated = true;
    g.exec_idle = true;
    g.self_test_busy = false;
    g.bl50_idle = true;
    g.bl50_pwm = 0;
    g.position_service_idle = true;
    g.position_moving = false;
    g.forbidden_outputs_off = true;
    g.water_service_idle = true;
    g.detergent_service_idle = true;
    g.drain_service_idle = true;
    g.dry_service_idle = true;
    g.hot_air_service_idle = true;
    g.water_safe_off_synced = true;
    g.request_id_ok = true;
    g.snapshot_fail = false;
    return g;
}

/* 指定 required/actual 的门禁输入 */
static motor_bench_gate_input_t gate_at(drum_position_t required, drum_position_t actual)
{
    motor_bench_gate_input_t g = make_gate();
    g.required_pos = required;
    g.pos = actual;
    g.stable_hall_mask = hall_mask_for(actual);
    g.raw_hall_mask = hall_mask_for(actual);
    return g;
}

/* ---- FSM 输入构造 ---- */

static motor_bench_fsm_input_t make_fin(uint32_t now_ms, motor_bench_gate_result_t gate)
{
    motor_bench_fsm_input_t fin;
    memset(&fin, 0, sizeof(fin));
    fin.now_ms = now_ms;
    fin.gate = gate;
    fin.bl50_idle = true;
    fin.bl50_pwm = 0;
    fin.bl50_fault = false;
    fin.submit_failed = false;
    fin.forbidden_outputs_off = true;
    fin.user_confirm_pulse = false;
    fin.cancel_requested = false;
    fin.emergency = false;
    return fin;
}

/* NDEBUG 下 assert() 被整体编译掉，绝不用 assert 包裹生产调用。
 * 显式调用生产程序构建器并返回步数（失败返回 0，调用方须判错）。 */
static uint8_t build_program(motor_bench_type_t type,
                             motor_bench_step_t *steps,
                             uint8_t *count)
{
    *count = 0;
    if (!motor_bench_build_default_program(type, bl50_action_to_position,
                                           steps, count))
        return 0;
    return *count;
}

#define BUILD_PROGRAM(type, steps, count) \
    do { if (build_program((type), (steps), &(count)) == 0) { \
        FAIL("program build failed"); return; } } while (0)

/* 不变式：与 motor_bench_core.c 提交/终态语义严格对应。
 * terminal_before / state_before 为 tick 前捕获（tick 后 state 已推进，
 * 提交 tick 会 WAIT_CONFIRM→PULSING，故不能读 tick 后的 state）。 */
static int invariant_failures = 0;

static void check_invariants(bool terminal_before,
                             motor_bench_state_t state_before,
                             const motor_bench_fsm_ctx_t *ctx,
                             const motor_bench_fsm_input_t *in,
                             const motor_bench_fsm_output_t *out)
{
    static int dbg_printed = 0;
    const char *reason = NULL;
    if (out->submit_pulse) {
        /* 脉冲只在 WAIT_CONFIRM + 门禁 OK + BL50 空闲 + 用户确认（本步至多一次，重叠=0） */
        if (state_before != MOTOR_BENCH_STATE_WAIT_CONFIRM) reason = "submit:state!=WAIT_CONFIRM";
        if (in->gate != MOTOR_BENCH_GATE_OK) reason = "submit:gate!=OK";
        if (!in->bl50_idle) reason = "submit:bl50!idle";
        if (!in->user_confirm_pulse) reason = "submit:no_confirm";
        if (ctx->current_step >= ctx->step_count) reason = "submit:step_oob";
    }
    if (out->request_all_off && !reason) {
        /* 全 OFF 必须伴随终态；受理拒绝（REJECTED）从不全关 */
        if (!out->terminal) reason = "alloff:no_terminal";
        if (ctx->terminal == MOTOR_BENCH_TERMINAL_REJECTED) reason = "alloff:rejected";
    }
    if (out->terminal && !reason) {
        /* 非 COMPLETE 终态绝不声明输出已确认关闭（fail-closed 报告） */
        if (ctx->terminal == MOTOR_BENCH_TERMINAL_COMPLETE) {
            if (!ctx->output_confirmed_off) reason = "term:complete_no_off";
        } else {
            if (ctx->output_confirmed_off) reason = "term:noncomplete_off";
        }
    }
    /* 已是终态后再 tick：必须零输出（exactly-once） */
    if (terminal_before && !reason) {
        if (out->submit_pulse || out->request_all_off || out->terminal) reason = "post_terminal_out";
    }
    if (reason) {
        invariant_failures++;
        if (dbg_printed < 3) {
            dbg_printed++;
            printf("\n       [inv] %s | pre_state=%s post_state=%s term=%d step=%u/%u "
                   "bl50_idle=%d pwm=%d conf=%d gate=%d\n",
                   reason, motor_bench_state_name(state_before),
                   motor_bench_state_name(ctx->state), (int)ctx->terminal,
                   (unsigned)ctx->current_step, (unsigned)ctx->step_count,
                   in->bl50_idle ? 1 : 0, (int)in->bl50_pwm,
                   in->user_confirm_pulse ? 1 : 0, (int)in->gate);
        }
    }
}

/* ================================================================
 * [1] 类型名称
 * ================================================================ */

static void test_type_names(void)
{
    TEST("type_name/from_name round trip");
    int ok = 1;
    motor_bench_type_t t;
    if (strcmp(motor_bench_type_name(MOTOR_BENCH_TYPE_PULSATOR), "pulsator") != 0) ok = 0;
    if (strcmp(motor_bench_type_name(MOTOR_BENCH_TYPE_DRUM), "drum") != 0) ok = 0;
    if (strcmp(motor_bench_type_name(MOTOR_BENCH_TYPE_HALL_SEQUENCE), "hall_sequence") != 0) ok = 0;
    if (strcmp(motor_bench_type_name(MOTOR_BENCH_TYPE_PULSATOR_BEHAVIOR), "pulsator_behavior") != 0) ok = 0;
    if (strcmp(motor_bench_type_name(MOTOR_BENCH_TYPE_NONE), "") != 0) ok = 0;
    if (!motor_bench_type_from_name("pulsator", &t) || t != MOTOR_BENCH_TYPE_PULSATOR) ok = 0;
    if (!motor_bench_type_from_name("drum", &t) || t != MOTOR_BENCH_TYPE_DRUM) ok = 0;
    if (!motor_bench_type_from_name("hall_sequence", &t) || t != MOTOR_BENCH_TYPE_HALL_SEQUENCE) ok = 0;
    if (!motor_bench_type_from_name("pulsator_behavior", &t) || t != MOTOR_BENCH_TYPE_PULSATOR_BEHAVIOR) ok = 0;
    if (motor_bench_type_from_name("bogus", &t)) ok = 0;
    if (motor_bench_type_from_name(NULL, &t)) ok = 0;
    if (motor_bench_type_from_name("pulsator", NULL)) ok = 0;
    if (motor_bench_type_step_count(MOTOR_BENCH_TYPE_PULSATOR) != 1) ok = 0;
    if (motor_bench_type_step_count(MOTOR_BENCH_TYPE_DRUM) != 1) ok = 0;
    if (motor_bench_type_step_count(MOTOR_BENCH_TYPE_HALL_SEQUENCE) != 3) ok = 0;
    if (motor_bench_type_step_count(MOTOR_BENCH_TYPE_PULSATOR_BEHAVIOR) != 1) ok = 0;
    if (motor_bench_type_step_count(MOTOR_BENCH_TYPE_NONE) != 0) ok = 0;
    if (ok) PASS(); else FAIL("type mapping mismatch");
}

/* ================================================================
 * [2] 默认程序：由生产契约派生位置（禁止硬编码猜测）
 * ================================================================ */

static void test_default_program(void)
{
    TEST("default program via production resolver");
    motor_bench_step_t steps[MOTOR_BENCH_MAX_STEPS];
    uint8_t count = 0;
    int ok = 1;

    if (!motor_bench_build_default_program(MOTOR_BENCH_TYPE_PULSATOR,
                                           bl50_action_to_position, steps, &count)) ok = 0;
    if (count != 1) ok = 0;
    if (steps[0].action != BL50_ACTION_PULSATOR) ok = 0;
    /* 生产契约断言：PULSATOR→0°（与 bl50_fsm 同源，非猜测） */
    if (steps[0].required_position != DRUM_POS_0) ok = 0;
    if (steps[0].required_position != bl50_action_to_position(BL50_ACTION_PULSATOR)) ok = 0;

    if (!motor_bench_build_default_program(MOTOR_BENCH_TYPE_DRUM,
                                           bl50_action_to_position, steps, &count)) ok = 0;
    if (count != 1) ok = 0;
    if (steps[0].action != BL50_ACTION_DRUM) ok = 0;
    if (steps[0].required_position != DRUM_POS_90) ok = 0;
    if (steps[0].required_position != bl50_action_to_position(BL50_ACTION_DRUM)) ok = 0;

    if (!motor_bench_build_default_program(MOTOR_BENCH_TYPE_HALL_SEQUENCE,
                                           bl50_action_to_position, steps, &count)) ok = 0;
    if (count != 3) ok = 0;
    if (steps[0].action != BL50_ACTION_PULSATOR || steps[0].required_position != DRUM_POS_0) ok = 0;
    if (steps[1].action != BL50_ACTION_DRUM || steps[1].required_position != DRUM_POS_90) ok = 0;
    if (steps[2].action != BL50_ACTION_SPIN || steps[2].required_position != DRUM_POS_180) ok = 0;

    /* 波轮行为诊断：单步 PULSATOR @ 0°（服务层提交长交替运行） */
    if (!motor_bench_build_default_program(MOTOR_BENCH_TYPE_PULSATOR_BEHAVIOR,
                                           bl50_action_to_position, steps, &count)) ok = 0;
    if (count != 1) ok = 0;
    if (steps[0].action != BL50_ACTION_PULSATOR) ok = 0;
    if (steps[0].required_position != DRUM_POS_0) ok = 0;

    /* 与生产契约逐一对照 */
    if (steps[0].required_position != bl50_action_to_position(BL50_ACTION_PULSATOR)) ok = 0;
    if (steps[1].required_position != bl50_action_to_position(BL50_ACTION_DRUM)) ok = 0;
    if (steps[2].required_position != bl50_action_to_position(BL50_ACTION_SPIN)) ok = 0;
    /* 生产契约本身：HOME→45° */
    if (bl50_action_to_position(BL50_ACTION_HOME) != DRUM_POS_45) ok = 0;

    /* 非法类型/空参数 fail-closed */
    if (motor_bench_build_default_program(MOTOR_BENCH_TYPE_NONE,
                                          bl50_action_to_position, steps, &count)) ok = 0;
    if (motor_bench_build_default_program(MOTOR_BENCH_TYPE_PULSATOR,
                                          NULL, steps, &count)) ok = 0;
    if (motor_bench_build_default_program(MOTOR_BENCH_TYPE_PULSATOR,
                                          bl50_action_to_position, NULL, &count)) ok = 0;
    if (motor_bench_build_default_program(MOTOR_BENCH_TYPE_PULSATOR,
                                          bl50_action_to_position, steps, NULL)) ok = 0;
    if (ok) PASS(); else FAIL("default program mismatch");
}

/* ================================================================
 * [3] 门禁：fail-closed 全分支
 * ================================================================ */

static void test_gate_fail_closed(void)
{
    TEST("gate: every rejection branch fail-closed");
    int ok = 1;

    if (motor_bench_gate_check(NULL) != MOTOR_BENCH_GATE_SNAPSHOT_UNAVAILABLE) ok = 0;

    { motor_bench_gate_input_t g = make_gate(); g.snapshot_fail = true;
      if (motor_bench_gate_check(&g) != MOTOR_BENCH_GATE_SNAPSHOT_UNAVAILABLE) ok = 0; }
    { motor_bench_gate_input_t g = make_gate(); g.exec_idle = false;
      if (motor_bench_gate_check(&g) != MOTOR_BENCH_GATE_EXECUTOR_BUSY) ok = 0; }
    { motor_bench_gate_input_t g = make_gate(); g.self_test_busy = true;
      if (motor_bench_gate_check(&g) != MOTOR_BENCH_GATE_SELF_TEST_BUSY) ok = 0; }
    { motor_bench_gate_input_t g = make_gate(); g.fault_active = true;
      if (motor_bench_gate_check(&g) != MOTOR_BENCH_GATE_FAULT_ACTIVE) ok = 0; }
    { motor_bench_gate_input_t g = make_gate(); g.emergency_active = true;
      if (motor_bench_gate_check(&g) != MOTOR_BENCH_GATE_EMERGENCY_ACTIVE) ok = 0; }
    { motor_bench_gate_input_t g = make_gate(); g.direction_calibrated = false;
      if (motor_bench_gate_check(&g) != MOTOR_BENCH_GATE_DIRECTION_NOT_CALIBRATED) ok = 0; }
    { motor_bench_gate_input_t g = make_gate(); g.pos_valid = false;
      if (motor_bench_gate_check(&g) != MOTOR_BENCH_GATE_POSITION_UNKNOWN) ok = 0; }
    { motor_bench_gate_input_t g = make_gate(); g.pos_fresh = false;
      if (motor_bench_gate_check(&g) != MOTOR_BENCH_GATE_POSITION_STALE) ok = 0; }
    { motor_bench_gate_input_t g = make_gate(); g.pos_stable = false;
      if (motor_bench_gate_check(&g) != MOTOR_BENCH_GATE_POSITION_UNSTABLE) ok = 0; }
    { motor_bench_gate_input_t g = make_gate(); g.pos_moving = true;
      if (motor_bench_gate_check(&g) != MOTOR_BENCH_GATE_MOTOR_MOVING) ok = 0; }
    { motor_bench_gate_input_t g = make_gate(); g.position_moving = true;
      if (motor_bench_gate_check(&g) != MOTOR_BENCH_GATE_MOTOR_MOVING) ok = 0; }
    { motor_bench_gate_input_t g = make_gate(); g.hall_conflict = true;
      if (motor_bench_gate_check(&g) != MOTOR_BENCH_GATE_HALL_CONFLICT) ok = 0; }
    { motor_bench_gate_input_t g = make_gate(); g.pos = DRUM_POS_45;
      if (motor_bench_gate_check(&g) != MOTOR_BENCH_GATE_POSITION_MISMATCH) ok = 0; }
    { motor_bench_gate_input_t g = make_gate(); g.bl50_idle = false;
      if (motor_bench_gate_check(&g) != MOTOR_BENCH_GATE_BL50_BUSY) ok = 0; }
    { motor_bench_gate_input_t g = make_gate(); g.bl50_pwm = 5;
      if (motor_bench_gate_check(&g) != MOTOR_BENCH_GATE_BL50_BUSY) ok = 0; }
    { motor_bench_gate_input_t g = make_gate(); g.position_service_idle = false;
      if (motor_bench_gate_check(&g) != MOTOR_BENCH_GATE_POSITION_SERVICE_BUSY) ok = 0; }
    { motor_bench_gate_input_t g = make_gate(); g.forbidden_outputs_off = false;
      if (motor_bench_gate_check(&g) != MOTOR_BENCH_GATE_FORBIDDEN_OUTPUT_ON) ok = 0; }
    { motor_bench_gate_input_t g = make_gate(); g.water_service_idle = false;
      if (motor_bench_gate_check(&g) != MOTOR_BENCH_GATE_SERVICE_BUSY) ok = 0; }
    { motor_bench_gate_input_t g = make_gate(); g.detergent_service_idle = false;
      if (motor_bench_gate_check(&g) != MOTOR_BENCH_GATE_SERVICE_BUSY) ok = 0; }
    { motor_bench_gate_input_t g = make_gate(); g.drain_service_idle = false;
      if (motor_bench_gate_check(&g) != MOTOR_BENCH_GATE_SERVICE_BUSY) ok = 0; }
    { motor_bench_gate_input_t g = make_gate(); g.dry_service_idle = false;
      if (motor_bench_gate_check(&g) != MOTOR_BENCH_GATE_SERVICE_BUSY) ok = 0; }
    { motor_bench_gate_input_t g = make_gate(); g.hot_air_service_idle = false;
      if (motor_bench_gate_check(&g) != MOTOR_BENCH_GATE_SERVICE_BUSY) ok = 0; }
    { motor_bench_gate_input_t g = make_gate(); g.water_safe_off_synced = false;
      if (motor_bench_gate_check(&g) != MOTOR_BENCH_GATE_OUTPUT_STATE_UNKNOWN) ok = 0; }
    { motor_bench_gate_input_t g = make_gate(); g.request_id_ok = false;
      if (motor_bench_gate_check(&g) != MOTOR_BENCH_GATE_REQUEST_ID_INVALID) ok = 0; }
    { motor_bench_gate_input_t g = make_gate();
      if (motor_bench_gate_check(&g) != MOTOR_BENCH_GATE_OK) ok = 0; }

    /* 状态码字符串 */
    if (strcmp(motor_bench_gate_code(MOTOR_BENCH_GATE_OK),
               "MOTOR_BENCH_ACCEPTED") != 0) ok = 0;
    if (strcmp(motor_bench_gate_code(MOTOR_BENCH_GATE_POSITION_MISMATCH),
               "MOTOR_POSITION_MISMATCH") != 0) ok = 0;
    if (strcmp(motor_bench_gate_code(MOTOR_BENCH_GATE_HALL_CONFLICT),
               "HALL_CONFLICT") != 0) ok = 0;
    if (strcmp(motor_bench_gate_code(MOTOR_BENCH_GATE_DIRECTION_NOT_CALIBRATED),
               "DIRECTION_NOT_CALIBRATED") != 0) ok = 0;
    if (strcmp(motor_bench_state_name(MOTOR_BENCH_STATE_WAIT_CONFIRM),
               "WAIT_CONFIRM") != 0) ok = 0;
    if (ok) PASS(); else FAIL("gate branch mismatch");
}

/* ================================================================
 * [4] FSM：单步 pulsator 完整成功路径
 * ================================================================ */

static void test_single_step_happy_path(void)
{
    TEST("pulsator single step full happy path");
    motor_bench_step_t steps[MOTOR_BENCH_MAX_STEPS];
    uint8_t count = 0;
    BUILD_PROGRAM(MOTOR_BENCH_TYPE_PULSATOR, steps, count);
    motor_bench_fsm_ctx_t ctx;
    motor_bench_fsm_init(&ctx, MOTOR_BENCH_TYPE_PULSATOR, steps, count, 1);
    motor_bench_fsm_output_t out;
    motor_bench_fsm_input_t fin;

    int ok = 1;
    if (ctx.state != MOTOR_BENCH_STATE_PRECHECK) ok = 0;
    if (ctx.terminal != MOTOR_BENCH_TERMINAL_NONE) ok = 0;
    if (ctx.current_step != 0 || ctx.step_count != 1) ok = 0;
    if (ctx.output_confirmed_off) ok = 0;

    /* PRECHECK OK → WAIT_POSITION */
    fin = make_fin(1000, MOTOR_BENCH_GATE_OK);
    motor_bench_fsm_tick(&ctx, &fin, &out);
    if (ctx.state != MOTOR_BENCH_STATE_WAIT_POSITION) ok = 0;
    if (out.terminal || out.submit_pulse || out.request_all_off) ok = 0;

    /* 位置就位 OK → WAIT_CONFIRM（两次 tick 分离，防止误确认） */
    fin = make_fin(1100, MOTOR_BENCH_GATE_OK);
    fin.user_confirm_pulse = true;   /* 即便带确认边沿，WAIT_POSITION 阶段也不提交 */
    motor_bench_fsm_tick(&ctx, &fin, &out);
    if (ctx.state != MOTOR_BENCH_STATE_WAIT_CONFIRM) ok = 0;
    if (out.submit_pulse) ok = 0;
    if (strcmp(ctx.last_code, "POSITION_READY") != 0) ok = 0;

    /* 用户确认 → 提交一次 ≤300ms 低档脉冲 */
    fin = make_fin(1200, MOTOR_BENCH_GATE_OK);
    fin.user_confirm_pulse = true;
    motor_bench_fsm_tick(&ctx, &fin, &out);
    if (out.submit_pulse != true) ok = 0;
    if (ctx.state != MOTOR_BENCH_STATE_PULSING) ok = 0;
    if (!ctx.pulse_issued || ctx.pulse_issued_ms != 1200) ok = 0;
    if (out.terminal || out.request_all_off) ok = 0;

    /* 受理宽限内 BL50 尚未运行 */
    fin = make_fin(1300, MOTOR_BENCH_GATE_OK);   /* elapsed=100 < 300 */
    motor_bench_fsm_tick(&ctx, &fin, &out);
    if (ctx.state != MOTOR_BENCH_STATE_PULSING) ok = 0;
    if (out.terminal || out.submit_pulse) ok = 0;

    /* BL50 受理并运行 → 记录 seen_running */
    fin = make_fin(1400, MOTOR_BENCH_GATE_OK);
    fin.bl50_idle = false;
    fin.bl50_pwm = 15;
    motor_bench_fsm_tick(&ctx, &fin, &out);
    if (ctx.state != MOTOR_BENCH_STATE_PULSING) ok = 0;
    if (!ctx.pulse_seen_running) ok = 0;

    /* 自动回 IDLE → OFF_CONFIRM */
    fin = make_fin(1500, MOTOR_BENCH_GATE_OK);
    fin.bl50_idle = true;
    motor_bench_fsm_tick(&ctx, &fin, &out);
    if (ctx.state != MOTOR_BENCH_STATE_OFF_CONFIRM) ok = 0;
    if (out.terminal || out.request_all_off) ok = 0;

    /* 确认 OFF（IDLE + PWM=0）→ COMPLETE，输出已确认关闭 */
    fin = make_fin(1600, MOTOR_BENCH_GATE_OK);
    fin.bl50_idle = true;
    fin.bl50_pwm = 0;
    motor_bench_fsm_tick(&ctx, &fin, &out);
    if (out.terminal != true || out.terminal_kind != MOTOR_BENCH_TERMINAL_COMPLETE) ok = 0;
    if (strcmp(out.terminal_code, "MOTOR_BENCH_COMPLETE") != 0) ok = 0;
    if (ctx.state != MOTOR_BENCH_STATE_COMPLETE) ok = 0;
    if (!ctx.output_confirmed_off) ok = 0;
    if (out.request_all_off || out.submit_pulse) ok = 0;

    if (ok) PASS(); else FAIL("happy path mismatch");
}

/* ================================================================
 * [5] FSM：3 步霍尔台架完整成功路径（每步独立确认 + 脉冲 + OFF 确认）
 * ================================================================ */

static void test_hall_sequence_happy_path(void)
{
    TEST("hall_sequence 3-step full happy path");
    motor_bench_step_t steps[MOTOR_BENCH_MAX_STEPS];
    uint8_t count = 0;
    BUILD_PROGRAM(MOTOR_BENCH_TYPE_HALL_SEQUENCE, steps, count);
    motor_bench_fsm_ctx_t ctx;
    motor_bench_fsm_init(&ctx, MOTOR_BENCH_TYPE_HALL_SEQUENCE, steps, count, 2);
    motor_bench_fsm_output_t out;
    motor_bench_fsm_input_t fin;
    const bl50_action_t expected[3] = { BL50_ACTION_PULSATOR, BL50_ACTION_DRUM, BL50_ACTION_SPIN };

    int ok = 1;
    uint32_t t = 1000;
    int pulses = 0;

    /* PRECHECK */
    motor_bench_state_t before = ctx.state;
    fin = make_fin(t, MOTOR_BENCH_GATE_OK);
    motor_bench_fsm_tick(&ctx, &fin, &out);
    check_invariants(false, before, &ctx, &fin, &out);
    if (invariant_failures) { ok = 0; invariant_failures = 0; }
    if (ctx.state != MOTOR_BENCH_STATE_WAIT_POSITION) ok = 0;

    while (!motor_bench_fsm_is_terminal(&ctx)) {
        t += 50;
        before = ctx.state;
        fin = make_fin(t, MOTOR_BENCH_GATE_OK);
        fin.forbidden_outputs_off = true;
        if (ctx.state == MOTOR_BENCH_STATE_WAIT_CONFIRM) {
            fin.user_confirm_pulse = true;
        } else if (ctx.state == MOTOR_BENCH_STATE_PULSING) {
            /* 模拟 BL50 受理：提交后宽限内仍 IDLE，随后运行窗口，再回 IDLE */
            uint32_t el = t - ctx.pulse_issued_ms;
            if (el <= 40) { fin.bl50_idle = true; fin.bl50_pwm = 0; }
            else if (el <= 160) { fin.bl50_idle = false; fin.bl50_pwm = 15; }
            else { fin.bl50_idle = true; fin.bl50_pwm = 0; }
        } else if (ctx.state == MOTOR_BENCH_STATE_OFF_CONFIRM) {
            fin.bl50_idle = true;
            fin.bl50_pwm = 0;
        }
        motor_bench_fsm_tick(&ctx, &fin, &out);
        if (out.submit_pulse) {
            pulses++;
            /* 脉冲动作 = 当前步生产动作（位置契约派生，非硬编码） */
            if (ctx.steps[ctx.current_step].action != expected[ctx.current_step]) ok = 0;
            if (ctx.steps[ctx.current_step].required_position !=
                bl50_action_to_position(expected[ctx.current_step])) ok = 0;
        }
        check_invariants(false, before, &ctx, &fin, &out);
        if (invariant_failures) { ok = 0; invariant_failures = 0; break; }
    }

    if (pulses != 3) ok = 0;                        /* 3 步 × 每步恰 1 次脉冲（重叠=0） */
    if (ctx.terminal != MOTOR_BENCH_TERMINAL_COMPLETE) ok = 0;
    if (!ctx.output_confirmed_off) ok = 0;
    if (ctx.current_step != 2) ok = 0;              /* 完成时停在末步下标 */
    if (ok) PASS(); else FAIL("hall sequence happy path mismatch");
}

/* 上面驱动里 PULSING 的模拟对 3 步长序可能因宽限不足而误判，
 * 这里用确定性驱动重写，确保每步都能走完：提交→seen→IDLE→OFF_CONFIRM→推进。 */
static int drive_step_to_advance(motor_bench_fsm_ctx_t *ctx, uint32_t *t,
                                 int *pulses, int *failures)
{
    motor_bench_fsm_output_t out;
    motor_bench_fsm_input_t fin;

    /* 等待位置就位：连续 OK 门禁直到 WAIT_CONFIRM */
    for (int i = 0; i < 8 && ctx->state == MOTOR_BENCH_STATE_WAIT_POSITION; i++) {
        *t += 50;
        motor_bench_state_t before = ctx->state;
        fin = make_fin(*t, MOTOR_BENCH_GATE_OK);
        fin.forbidden_outputs_off = true;
        motor_bench_fsm_tick(ctx, &fin, &out);
        check_invariants(false, before, ctx, &fin, &out);
        if (invariant_failures) { (*failures)++; return 0; }
        if (out.terminal) return 0;
    }
    if (ctx->state != MOTOR_BENCH_STATE_WAIT_CONFIRM) return 0;

    /* 等待确认：OK + 确认 → 提交脉冲 */
    *t += 50;
    motor_bench_state_t before = ctx->state;
    fin = make_fin(*t, MOTOR_BENCH_GATE_OK);
    fin.forbidden_outputs_off = true;
    fin.user_confirm_pulse = true;
    motor_bench_fsm_tick(ctx, &fin, &out);
    check_invariants(false, before, ctx, &fin, &out);
    if (invariant_failures) { (*failures)++; return 0; }
    if (!out.submit_pulse) return 0;
    (*pulses)++;

    /* 宽限内（<300ms）BL50 未运行 → 仍 PULSING */
    *t += 50;
    before = ctx->state;
    fin = make_fin(*t, MOTOR_BENCH_GATE_OK);
    fin.bl50_idle = true;
    motor_bench_fsm_tick(ctx, &fin, &out);
    check_invariants(false, before, ctx, &fin, &out);
    if (invariant_failures) { (*failures)++; return 0; }
    if (ctx->state != MOTOR_BENCH_STATE_PULSING || out.terminal) return 0;

    /* BL50 运行（非 IDLE）→ seen_running */
    *t += 50;
    before = ctx->state;
    fin = make_fin(*t, MOTOR_BENCH_GATE_OK);
    fin.bl50_idle = false;
    fin.bl50_pwm = 15;
    motor_bench_fsm_tick(ctx, &fin, &out);
    check_invariants(false, before, ctx, &fin, &out);
    if (invariant_failures) { (*failures)++; return 0; }
    if (!ctx->pulse_seen_running || out.terminal) return 0;

    /* 回 IDLE → OFF_CONFIRM */
    *t += 50;
    before = ctx->state;
    fin = make_fin(*t, MOTOR_BENCH_GATE_OK);
    fin.bl50_idle = true;
    fin.bl50_pwm = 0;
    motor_bench_fsm_tick(ctx, &fin, &out);
    check_invariants(false, before, ctx, &fin, &out);
    if (invariant_failures) { (*failures)++; return 0; }
    if (ctx->state != MOTOR_BENCH_STATE_OFF_CONFIRM || out.terminal) return 0;

    /* OFF 确认（IDLE+PWM0）→ 推进到下一 WAIT_POSITION 或 COMPLETE */
    *t += 50;
    before = ctx->state;
    fin = make_fin(*t, MOTOR_BENCH_GATE_OK);
    fin.bl50_idle = true;
    fin.bl50_pwm = 0;
    motor_bench_fsm_tick(ctx, &fin, &out);
    check_invariants(false, before, ctx, &fin, &out);
    if (invariant_failures) { (*failures)++; return 0; }
    if (out.terminal) {
        if (ctx->terminal != MOTOR_BENCH_TERMINAL_COMPLETE) return 0;
        if (!ctx->output_confirmed_off) return 0;
        return 1;   /* 完成 */
    }
    if (ctx->state != MOTOR_BENCH_STATE_WAIT_POSITION) return 0;
    if (ctx->pulse_issued || ctx->pulse_seen_running) return 0;  /* 步间标志复位 */
    return 0;
}

static void test_hall_sequence_deterministic(void)
{
    TEST("hall_sequence deterministic 3-pulse run");
    motor_bench_step_t steps[MOTOR_BENCH_MAX_STEPS];
    uint8_t count = 0;
    BUILD_PROGRAM(MOTOR_BENCH_TYPE_HALL_SEQUENCE, steps, count);
    motor_bench_fsm_ctx_t ctx;
    motor_bench_fsm_init(&ctx, MOTOR_BENCH_TYPE_HALL_SEQUENCE, steps, count, 3);
    motor_bench_fsm_output_t out;
    motor_bench_fsm_input_t fin;
    int failures = 0;
    int pulses = 0;
    uint32_t t = 5000;

    /* PRECHECK */
    motor_bench_state_t before = ctx.state;
    fin = make_fin(t, MOTOR_BENCH_GATE_OK);
    motor_bench_fsm_tick(&ctx, &fin, &out);
    check_invariants(false, before, &ctx, &fin, &out);
    if (invariant_failures) { failures += invariant_failures; invariant_failures = 0; }
    if (ctx.state != MOTOR_BENCH_STATE_WAIT_POSITION) failures++;

    while (!motor_bench_fsm_is_terminal(&ctx) && pulses < 3) {
        if (drive_step_to_advance(&ctx, &t, &pulses, &failures) && pulses >= 3) break;
        if (failures) break;
    }

    int ok = 1;
    if (failures) ok = 0;
    if (pulses != 3) ok = 0;
    if (!motor_bench_fsm_is_terminal(&ctx)) ok = 0;
    if (ctx.terminal != MOTOR_BENCH_TERMINAL_COMPLETE) ok = 0;
    if (!ctx.output_confirmed_off) ok = 0;
    if (ctx.current_step != 2) ok = 0;              /* 完成时停在末步下标 */
    if (ok) PASS(); else FAIL("deterministic hall mismatch");
}

/* ================================================================
 * [6] 错误位置 / 多霍尔 / 超时：fail-closed 不推进
 * ================================================================ */

static void test_wrong_position_rejection(void)
{
    TEST("wrong position rejected, no advance, then timeout");
    motor_bench_step_t steps[MOTOR_BENCH_MAX_STEPS];
    uint8_t count = 0;
    BUILD_PROGRAM(MOTOR_BENCH_TYPE_PULSATOR, steps, count);
    motor_bench_fsm_ctx_t ctx;
    motor_bench_fsm_init(&ctx, MOTOR_BENCH_TYPE_PULSATOR, steps, count, 4);
    motor_bench_fsm_output_t out;
    motor_bench_fsm_input_t fin;
    int ok = 1;

    /* PRECHECK OK → WAIT_POSITION @ t=10000 */
    fin = make_fin(10000, MOTOR_BENCH_GATE_OK);
    motor_bench_fsm_tick(&ctx, &fin, &out);
    if (ctx.state != MOTOR_BENCH_STATE_WAIT_POSITION) ok = 0;

    /* 错误霍尔：pos=45 != 所需 0 */
    for (int i = 0; i < 5; i++) {
        fin = make_fin(10100u + (uint32_t)i * 100, MOTOR_BENCH_GATE_POSITION_MISMATCH);
        motor_bench_fsm_tick(&ctx, &fin, &out);
        if (ctx.state != MOTOR_BENCH_STATE_WAIT_POSITION) ok = 0;
        if (out.terminal || out.request_all_off || out.submit_pulse) ok = 0;
        if (strcmp(ctx.last_code, "MOTOR_POSITION_MISMATCH") != 0) ok = 0;
        if (ctx.pulse_issued) ok = 0;   /* 绝不推进到脉冲 */
    }

    /* 位置超时：step_enter=10000，t > 112000 触发 POSITION_TIMEOUT */
    fin = make_fin(10000u + MOTOR_BENCH_POSITION_WAIT_TIMEOUT_MS + 1,
                   MOTOR_BENCH_GATE_POSITION_MISMATCH);
    motor_bench_fsm_tick(&ctx, &fin, &out);
    if (out.terminal != true || out.terminal_kind != MOTOR_BENCH_TERMINAL_TIMEOUT) ok = 0;
    if (strcmp(out.terminal_code, "POSITION_TIMEOUT") != 0) ok = 0;
    if (!out.request_all_off) ok = 0;
    if (ctx.output_confirmed_off) ok = 0;   /* 非 COMPLETE 不声明 OFF */

    if (ok) PASS(); else FAIL("wrong-position/timeout mismatch");
}

static void test_multi_hall_rejection(void)
{
    TEST("multi-hall conflict rejected fail-closed");
    motor_bench_step_t steps[MOTOR_BENCH_MAX_STEPS];
    uint8_t count = 0;
    BUILD_PROGRAM(MOTOR_BENCH_TYPE_PULSATOR, steps, count);
    motor_bench_fsm_ctx_t ctx;
    motor_bench_fsm_init(&ctx, MOTOR_BENCH_TYPE_PULSATOR, steps, count, 5);
    motor_bench_fsm_output_t out;
    motor_bench_fsm_input_t fin;
    int ok = 1;

    /* 门禁层：多霍尔 → HALL_CONFLICT */
    { motor_bench_gate_input_t g = gate_at(DRUM_POS_0, DRUM_POS_0);
      g.stable_hall_mask = 0x01 | 0x04; g.hall_conflict = true;
      if (motor_bench_gate_check(&g) != MOTOR_BENCH_GATE_HALL_CONFLICT) ok = 0; }

    fin = make_fin(2000, MOTOR_BENCH_GATE_OK);
    motor_bench_fsm_tick(&ctx, &fin, &out);
    if (ctx.state != MOTOR_BENCH_STATE_WAIT_POSITION) ok = 0;

    /* 多霍尔：良性拒绝，记录原因，不推进 */
    fin = make_fin(2100, MOTOR_BENCH_GATE_HALL_CONFLICT);
    motor_bench_fsm_tick(&ctx, &fin, &out);
    if (ctx.state != MOTOR_BENCH_STATE_WAIT_POSITION) ok = 0;
    if (out.terminal || out.request_all_off || out.submit_pulse) ok = 0;
    if (strcmp(ctx.last_code, "HALL_CONFLICT") != 0) ok = 0;
    if (ctx.pulse_issued) ok = 0;

    /* 之后位置转正 → 仍可正常走 WAIT_CONFIRM（不因先前矛盾而卡死） */
    fin = make_fin(2200, MOTOR_BENCH_GATE_OK);
    motor_bench_fsm_tick(&ctx, &fin, &out);
    if (ctx.state != MOTOR_BENCH_STATE_WAIT_CONFIRM) ok = 0;

    if (ok) PASS(); else FAIL("multi-hall mismatch");
}

static void test_confirm_timeout(void)
{
    TEST("confirm timeout after position ready");
    motor_bench_step_t steps[MOTOR_BENCH_MAX_STEPS];
    uint8_t count = 0;
    BUILD_PROGRAM(MOTOR_BENCH_TYPE_PULSATOR, steps, count);
    motor_bench_fsm_ctx_t ctx;
    motor_bench_fsm_init(&ctx, MOTOR_BENCH_TYPE_PULSATOR, steps, count, 6);
    motor_bench_fsm_output_t out;
    motor_bench_fsm_input_t fin;
    int ok = 1;

    fin = make_fin(3000, MOTOR_BENCH_GATE_OK);
    motor_bench_fsm_tick(&ctx, &fin, &out);           /* PRECHECK → WAIT_POSITION */
    fin = make_fin(3100, MOTOR_BENCH_GATE_OK);
    motor_bench_fsm_tick(&ctx, &fin, &out);           /* → WAIT_CONFIRM @ step_enter=3100 */

    /* 用户不确认：持续 OK，超过 20s → CONFIRM_TIMEOUT + 全关 */
    fin = make_fin(3100u + MOTOR_BENCH_CONFIRM_TIMEOUT_MS + 1, MOTOR_BENCH_GATE_OK);
    motor_bench_fsm_tick(&ctx, &fin, &out);
    if (out.terminal != true || out.terminal_kind != MOTOR_BENCH_TERMINAL_TIMEOUT) ok = 0;
    if (strcmp(out.terminal_code, "CONFIRM_TIMEOUT") != 0) ok = 0;
    if (!out.request_all_off) ok = 0;
    if (ctx.output_confirmed_off) ok = 0;

    if (ok) PASS(); else FAIL("confirm timeout mismatch");
}

/* ================================================================
 * [7] 取消 / 急停 / BLE 断线 / 禁止输出不变式
 * ================================================================ */

static void test_cancel_interrupts(void)
{
    TEST("cancel → INTERRUPTED + all off");
    motor_bench_step_t steps[MOTOR_BENCH_MAX_STEPS];
    uint8_t count = 0;
    BUILD_PROGRAM(MOTOR_BENCH_TYPE_PULSATOR, steps, count);
    motor_bench_fsm_ctx_t ctx;
    motor_bench_fsm_init(&ctx, MOTOR_BENCH_TYPE_PULSATOR, steps, count, 7);
    motor_bench_fsm_output_t out;
    motor_bench_fsm_input_t fin;
    int ok = 1;

    fin = make_fin(4000, MOTOR_BENCH_GATE_OK);
    motor_bench_fsm_tick(&ctx, &fin, &out);           /* → WAIT_POSITION */

    fin = make_fin(4050, MOTOR_BENCH_GATE_OK);
    fin.cancel_requested = true;
    motor_bench_fsm_tick(&ctx, &fin, &out);
    if (out.terminal != true || out.terminal_kind != MOTOR_BENCH_TERMINAL_INTERRUPTED) ok = 0;
    if (strcmp(out.terminal_code, "CANCELED") != 0) ok = 0;
    if (!out.request_all_off) ok = 0;
    if (ctx.output_confirmed_off) ok = 0;

    /* 取消在任何状态优先：PULSING 中取消同样全关 */
    motor_bench_fsm_ctx_t ctx2;
    motor_bench_fsm_init(&ctx2, MOTOR_BENCH_TYPE_PULSATOR, steps, count, 8);
    fin = make_fin(5000, MOTOR_BENCH_GATE_OK);
    motor_bench_fsm_tick(&ctx2, &fin, &out);
    fin = make_fin(5100, MOTOR_BENCH_GATE_OK);
    motor_bench_fsm_tick(&ctx2, &fin, &out);           /* WAIT_CONFIRM */
    fin = make_fin(5200, MOTOR_BENCH_GATE_OK);
    fin.user_confirm_pulse = true;
    motor_bench_fsm_tick(&ctx2, &fin, &out);           /* PULSING */
    if (ctx2.state != MOTOR_BENCH_STATE_PULSING) ok = 0;
    fin = make_fin(5300, MOTOR_BENCH_GATE_OK);
    fin.cancel_requested = true;
    motor_bench_fsm_tick(&ctx2, &fin, &out);
    if (out.terminal != true || out.terminal_kind != MOTOR_BENCH_TERMINAL_INTERRUPTED) ok = 0;
    if (!out.request_all_off) ok = 0;

    if (ok) PASS(); else FAIL("cancel mismatch");
}

static void test_emergency_interrupts(void)
{
    TEST("emergency/BLE-disconnect → INTERRUPTED + all off");
    motor_bench_step_t steps[MOTOR_BENCH_MAX_STEPS];
    uint8_t count = 0;
    BUILD_PROGRAM(MOTOR_BENCH_TYPE_PULSATOR, steps, count);
    motor_bench_fsm_ctx_t ctx;
    motor_bench_fsm_init(&ctx, MOTOR_BENCH_TYPE_PULSATOR, steps, count, 9);
    motor_bench_fsm_output_t out;
    motor_bench_fsm_input_t fin;
    int ok = 1;

    fin = make_fin(6000, MOTOR_BENCH_GATE_OK);
    motor_bench_fsm_tick(&ctx, &fin, &out);           /* → WAIT_POSITION */

    /* BLE 断线/急停（service 层 emergency 闩锁 → fin.emergency） */
    fin = make_fin(6100, MOTOR_BENCH_GATE_OK);
    fin.emergency = true;
    motor_bench_fsm_tick(&ctx, &fin, &out);
    if (out.terminal != true || out.terminal_kind != MOTOR_BENCH_TERMINAL_INTERRUPTED) ok = 0;
    if (strcmp(out.terminal_code, "EMERGENCY_REQUESTED") != 0) ok = 0;
    if (!out.request_all_off) ok = 0;
    if (ctx.output_confirmed_off) ok = 0;

    /* PRECHECK 阶段 emergency 由门禁以 EMERGENCY_ACTIVE 表达（REJECTED），
       不在此直接中断 —— 与 core 设计注释一致，且绝不产生脉冲。 */
    motor_bench_fsm_ctx_t ctx2;
    motor_bench_fsm_init(&ctx2, MOTOR_BENCH_TYPE_PULSATOR, steps, count, 10);
    fin = make_fin(7000, MOTOR_BENCH_GATE_EMERGENCY_ACTIVE);
    fin.emergency = true;
    motor_bench_fsm_tick(&ctx2, &fin, &out);
    if (ctx2.terminal != MOTOR_BENCH_TERMINAL_REJECTED) ok = 0;
    if (strcmp(ctx2.last_code, "EMERGENCY_ACTIVE") != 0) ok = 0;
    if (out.request_all_off) ok = 0;                  /* 受理拒绝不触发全关 */
    if (ctx2.output_confirmed_off) ok = 0;

    if (ok) PASS(); else FAIL("emergency mismatch");
}

static void test_forbidden_output_invariant(void)
{
    TEST("forbidden output ON at run time → FAULT + all off");
    motor_bench_step_t steps[MOTOR_BENCH_MAX_STEPS];
    uint8_t count = 0;
    BUILD_PROGRAM(MOTOR_BENCH_TYPE_PULSATOR, steps, count);
    motor_bench_fsm_ctx_t ctx;
    motor_bench_fsm_init(&ctx, MOTOR_BENCH_TYPE_PULSATOR, steps, count, 11);
    motor_bench_fsm_output_t out;
    motor_bench_fsm_input_t fin;
    int ok = 1;

    fin = make_fin(8000, MOTOR_BENCH_GATE_OK);
    motor_bench_fsm_tick(&ctx, &fin, &out);           /* → WAIT_POSITION */

    /* 每 tick 复核：禁止输出出现 ON → FAULT FORBIDDEN_OUTPUT_ON + 全关 */
    fin = make_fin(8100, MOTOR_BENCH_GATE_OK);
    fin.forbidden_outputs_off = false;
    motor_bench_fsm_tick(&ctx, &fin, &out);
    if (out.terminal != true || out.terminal_kind != MOTOR_BENCH_TERMINAL_FAULT) ok = 0;
    if (strcmp(out.terminal_code, "FORBIDDEN_OUTPUT_ON") != 0) ok = 0;
    if (!out.request_all_off) ok = 0;
    if (ctx.output_confirmed_off) ok = 0;

    /* WAIT_CONFIRM 阶段同样触发 */
    motor_bench_fsm_ctx_t ctx2;
    motor_bench_fsm_init(&ctx2, MOTOR_BENCH_TYPE_PULSATOR, steps, count, 12);
    fin = make_fin(9000, MOTOR_BENCH_GATE_OK);
    motor_bench_fsm_tick(&ctx2, &fin, &out);
    fin = make_fin(9100, MOTOR_BENCH_GATE_OK);
    motor_bench_fsm_tick(&ctx2, &fin, &out);           /* WAIT_CONFIRM */
    fin = make_fin(9200, MOTOR_BENCH_GATE_OK);
    fin.forbidden_outputs_off = false;
    motor_bench_fsm_tick(&ctx2, &fin, &out);
    if (out.terminal != true || out.terminal_kind != MOTOR_BENCH_TERMINAL_FAULT) ok = 0;
    if (!out.request_all_off) ok = 0;

    if (ok) PASS(); else FAIL("forbidden-output mismatch");
}

/* P0-1 回归：forbidden_outputs_off 聚合纯函数。
 * 基值必须为 true（从 false 起 AND 恒 false → 台架永远无法受理，曾为 P0 缺陷）；
 * 任一子句未确认 OFF → 整体 false（fail-closed）；fault/emergency 激活时跳过 UV。 */
static void test_forbidden_aggregation(void)
{
    TEST("forbidden_outputs_off aggregation: base true, each clause fail-closed");
    int ok = 1;
    motor_bench_forbidden_clauses_t c;

    /* 全部确认 OFF → true */
    memset(&c, 0, sizeof(c));
    c.water_valves_off = true;
    c.detergent_off = true;
    c.drain_off = true;
    c.dry_off = true;
    c.hot_air_off = true;
    c.uv_clause_applicable = true;
    c.uv_off = true;
    if (!motor_bench_forbidden_outputs_off(&c)) ok = 0;

    /* UV 子句跳过（fault/emergency 激活）：uv_off=false 仍 true */
    memset(&c, 0, sizeof(c));
    c.water_valves_off = true;
    c.detergent_off = true;
    c.drain_off = true;
    c.dry_off = true;
    c.hot_air_off = true;
    c.uv_clause_applicable = false;
    c.uv_off = false;
    if (!motor_bench_forbidden_outputs_off(&c)) ok = 0;

    /* UV 适用但 uv_off=false → false */
    memset(&c, 0, sizeof(c));
    c.water_valves_off = true;
    c.detergent_off = true;
    c.drain_off = true;
    c.dry_off = true;
    c.hot_air_off = true;
    c.uv_clause_applicable = true;
    c.uv_off = false;
    if (motor_bench_forbidden_outputs_off(&c)) ok = 0;

    /* 每一子句单独为 false（其余全 OFF）→ false，逐一验证 fail-closed */
    struct { bool *field; const char *name; } clauses[] = {
        { &c.water_valves_off, "water_valves_off" },
        { &c.detergent_off,    "detergent_off" },
        { &c.drain_off,        "drain_off" },
        { &c.dry_off,          "dry_off" },
        { &c.hot_air_off,      "hot_air_off" },
    };
    for (size_t i = 0; i < sizeof(clauses) / sizeof(clauses[0]); i++) {
        memset(&c, 0, sizeof(c));
        c.water_valves_off = true;
        c.detergent_off = true;
        c.drain_off = true;
        c.dry_off = true;
        c.hot_air_off = true;
        c.uv_clause_applicable = true;
        c.uv_off = true;
        *clauses[i].field = false;
        if (motor_bench_forbidden_outputs_off(&c)) {
            printf("      [clause] %s failed to fail-closed\n", clauses[i].name);
            ok = 0;
        }
    }

    /* NULL → false */
    if (motor_bench_forbidden_outputs_off(NULL)) ok = 0;

    if (ok) PASS(); else FAIL("forbidden aggregation mismatch");
}

/* P0 回归（板测发现）：self_test_busy 聚合纯函数。
 * 台架自身 test_active 刻意不参与 —— 受理后 test_active 置 true，若计入则
 * 门禁每 tick 自锁 SELF_TEST_BUSY，台架"一受理即 FAULT"（曾真实发生于板测，
 * host 纯 FSM 测不到，缺陷在服务层聚合）。本函数签名不接受台架自身活动标志。 */
static void test_external_self_test_busy_aggregation(void)
{
    TEST("self_test_busy aggregates ONLY external UV/actuator activity");
    if (motor_bench_external_self_test_busy(false, false) == false &&
        motor_bench_external_self_test_busy(true, false) == true &&
        motor_bench_external_self_test_busy(false, true) == true &&
        motor_bench_external_self_test_busy(true, true) == true) PASS();
    else FAIL("self_test_busy aggregation mismatch");
}

/* ================================================================
 * [8] PULSING / OFF_CONFIRM 故障路径 + 终态 exactly-once
 * ================================================================ */

static void test_pulsing_fault_paths(void)
{
    TEST("PULSING submit-fail / bl50-fault / grace-timeout fail-closed");
    motor_bench_step_t steps[MOTOR_BENCH_MAX_STEPS];
    uint8_t count = 0;
    BUILD_PROGRAM(MOTOR_BENCH_TYPE_PULSATOR, steps, count);
    motor_bench_fsm_output_t out;
    motor_bench_fsm_input_t fin;
    int ok = 1;

    /* (a) 提交失败 */
    { motor_bench_fsm_ctx_t ctx;
      motor_bench_fsm_init(&ctx, MOTOR_BENCH_TYPE_PULSATOR, steps, count, 13);
      fin = make_fin(10000, MOTOR_BENCH_GATE_OK);
      motor_bench_fsm_tick(&ctx, &fin, &out);
      fin = make_fin(10100, MOTOR_BENCH_GATE_OK);
      motor_bench_fsm_tick(&ctx, &fin, &out);
      fin = make_fin(10200, MOTOR_BENCH_GATE_OK);
      fin.user_confirm_pulse = true;
      motor_bench_fsm_tick(&ctx, &fin, &out);
      if (ctx.state != MOTOR_BENCH_STATE_PULSING) ok = 0;
      fin = make_fin(10200, MOTOR_BENCH_GATE_OK);
      fin.submit_failed = true;
      motor_bench_fsm_tick(&ctx, &fin, &out);
      if (out.terminal != true || out.terminal_kind != MOTOR_BENCH_TERMINAL_FAULT) ok = 0;
      if (strcmp(out.terminal_code, "SUBMIT_FAILED") != 0) ok = 0;
      if (!out.request_all_off) ok = 0;
      if (ctx.output_confirmed_off) ok = 0; }

    /* (b) BL50 故障 */
    { motor_bench_fsm_ctx_t ctx;
      motor_bench_fsm_init(&ctx, MOTOR_BENCH_TYPE_PULSATOR, steps, count, 14);
      fin = make_fin(11000, MOTOR_BENCH_GATE_OK);
      motor_bench_fsm_tick(&ctx, &fin, &out);
      fin = make_fin(11100, MOTOR_BENCH_GATE_OK);
      motor_bench_fsm_tick(&ctx, &fin, &out);
      fin = make_fin(11200, MOTOR_BENCH_GATE_OK);
      fin.user_confirm_pulse = true;
      motor_bench_fsm_tick(&ctx, &fin, &out);
      fin = make_fin(11250, MOTOR_BENCH_GATE_OK);
      fin.bl50_fault = true;
      motor_bench_fsm_tick(&ctx, &fin, &out);
      if (out.terminal != true || out.terminal_kind != MOTOR_BENCH_TERMINAL_FAULT) ok = 0;
      if (strcmp(out.terminal_code, "BL50_FAULT") != 0) ok = 0;
      if (!out.request_all_off) ok = 0; }

    /* (c) 宽限超时（提交后始终未见 BL50 运行） */
    { motor_bench_fsm_ctx_t ctx;
      motor_bench_fsm_init(&ctx, MOTOR_BENCH_TYPE_PULSATOR, steps, count, 15);
      fin = make_fin(12000, MOTOR_BENCH_GATE_OK);
      motor_bench_fsm_tick(&ctx, &fin, &out);
      fin = make_fin(12100, MOTOR_BENCH_GATE_OK);
      motor_bench_fsm_tick(&ctx, &fin, &out);
      fin = make_fin(12200, MOTOR_BENCH_GATE_OK);
      fin.user_confirm_pulse = true;
      motor_bench_fsm_tick(&ctx, &fin, &out);
      if (out.submit_pulse != true) ok = 0;
      uint32_t issued = ctx.pulse_issued_ms;   /* 12200 */
      /* elapsed <=300 宽限内持续等待 */
      fin = make_fin(issued + 100, MOTOR_BENCH_GATE_OK);
      motor_bench_fsm_tick(&ctx, &fin, &out);
      if (out.terminal) ok = 0;
      fin = make_fin(issued + 301, MOTOR_BENCH_GATE_OK);   /* elapsed 301 > 300 */
      motor_bench_fsm_tick(&ctx, &fin, &out);
      if (out.terminal != true || out.terminal_kind != MOTOR_BENCH_TERMINAL_FAULT) ok = 0;
      if (strcmp(out.terminal_code, "SUBMIT_FAILED") != 0) ok = 0;
      if (!out.request_all_off) ok = 0; }

    if (ok) PASS(); else FAIL("pulsing fault mismatch");
}

static void test_off_confirm_timeout(void)
{
    TEST("OFF_CONFIRM timeout → FAULT + all off");
    motor_bench_step_t steps[MOTOR_BENCH_MAX_STEPS];
    uint8_t count = 0;
    BUILD_PROGRAM(MOTOR_BENCH_TYPE_PULSATOR, steps, count);
    motor_bench_fsm_ctx_t ctx;
    motor_bench_fsm_init(&ctx, MOTOR_BENCH_TYPE_PULSATOR, steps, count, 16);
    motor_bench_fsm_output_t out;
    motor_bench_fsm_input_t fin;
    int ok = 1;

    fin = make_fin(13000, MOTOR_BENCH_GATE_OK);
    motor_bench_fsm_tick(&ctx, &fin, &out);
    fin = make_fin(13100, MOTOR_BENCH_GATE_OK);
    motor_bench_fsm_tick(&ctx, &fin, &out);
    fin = make_fin(13200, MOTOR_BENCH_GATE_OK);
    fin.user_confirm_pulse = true;
    motor_bench_fsm_tick(&ctx, &fin, &out);
    if (ctx.state != MOTOR_BENCH_STATE_PULSING) ok = 0;
    uint32_t issued = ctx.pulse_issued_ms;   /* 13200 */
    /* 运行 */
    fin = make_fin(issued + 50, MOTOR_BENCH_GATE_OK);
    fin.bl50_idle = false; fin.bl50_pwm = 15;
    motor_bench_fsm_tick(&ctx, &fin, &out);
    /* 回 IDLE → OFF_CONFIRM */
    fin = make_fin(issued + 100, MOTOR_BENCH_GATE_OK);
    fin.bl50_idle = true; fin.bl50_pwm = 0;
    motor_bench_fsm_tick(&ctx, &fin, &out);
    if (ctx.state != MOTOR_BENCH_STATE_OFF_CONFIRM) ok = 0;
    /* BL50 一直不确认 OFF（保持非 IDLE）超过 2000ms → OFF_CONFIRM_FAILED */
    fin = make_fin(issued + 2001, MOTOR_BENCH_GATE_OK);
    fin.bl50_idle = false; fin.bl50_pwm = 15;
    motor_bench_fsm_tick(&ctx, &fin, &out);
    if (out.terminal != true || out.terminal_kind != MOTOR_BENCH_TERMINAL_FAULT) ok = 0;
    if (strcmp(out.terminal_code, "OFF_CONFIRM_FAILED") != 0) ok = 0;
    if (!out.request_all_off) ok = 0;
    if (ctx.output_confirmed_off) ok = 0;

    if (ok) PASS(); else FAIL("off-confirm timeout mismatch");
}

static void test_terminal_exactly_once(void)
{
    TEST("terminal fires exactly once, later ticks no-op");
    motor_bench_step_t steps[MOTOR_BENCH_MAX_STEPS];
    uint8_t count = 0;
    BUILD_PROGRAM(MOTOR_BENCH_TYPE_PULSATOR, steps, count);
    motor_bench_fsm_ctx_t ctx;
    motor_bench_fsm_init(&ctx, MOTOR_BENCH_TYPE_PULSATOR, steps, count, 17);
    motor_bench_fsm_output_t out;
    motor_bench_fsm_input_t fin;
    int ok = 1;

    /* 快速走完 → COMPLETE */
    fin = make_fin(14000, MOTOR_BENCH_GATE_OK);
    motor_bench_fsm_tick(&ctx, &fin, &out);
    fin = make_fin(14100, MOTOR_BENCH_GATE_OK);
    motor_bench_fsm_tick(&ctx, &fin, &out);
    fin = make_fin(14200, MOTOR_BENCH_GATE_OK);
    fin.user_confirm_pulse = true;
    motor_bench_fsm_tick(&ctx, &fin, &out);
    uint32_t issued = ctx.pulse_issued_ms;
    fin = make_fin(issued + 50, MOTOR_BENCH_GATE_OK);
    fin.bl50_idle = false; fin.bl50_pwm = 15;
    motor_bench_fsm_tick(&ctx, &fin, &out);
    fin = make_fin(issued + 100, MOTOR_BENCH_GATE_OK);
    fin.bl50_idle = true; fin.bl50_pwm = 0;
    motor_bench_fsm_tick(&ctx, &fin, &out);
    if (ctx.state != MOTOR_BENCH_STATE_OFF_CONFIRM) ok = 0;
    fin = make_fin(issued + 150, MOTOR_BENCH_GATE_OK);
    fin.bl50_idle = true; fin.bl50_pwm = 0;
    motor_bench_fsm_tick(&ctx, &fin, &out);
    if (out.terminal != true || out.terminal_kind != MOTOR_BENCH_TERMINAL_COMPLETE) ok = 0;
    if (out.request_all_off) ok = 0;   /* COMPLETE 不再触发 all_off */

    /* 终态后继续 tick：必须全零输出，状态不再变化 */
    motor_bench_state_t saved = ctx.state;
    motor_bench_terminal_t saved_term = ctx.terminal;
    bool saved_off = ctx.output_confirmed_off;
    for (int i = 0; i < 5; i++) {
        fin = make_fin(20000u + (uint32_t)i * 100, MOTOR_BENCH_GATE_OK);
        fin.user_confirm_pulse = true;
        fin.cancel_requested = (i == 1);
        fin.emergency = (i == 2);
        fin.forbidden_outputs_off = (i != 3);
        motor_bench_fsm_tick(&ctx, &fin, &out);
        if (out.submit_pulse || out.request_all_off || out.terminal) ok = 0;
    }
    if (ctx.state != saved) ok = 0;
    if (ctx.terminal != saved_term) ok = 0;
    if (ctx.output_confirmed_off != saved_off) ok = 0;

    if (ok) PASS(); else FAIL("exactly-once mismatch");
}

/* ================================================================
 * [9] WAIT_CONFIRM 位置回退 + 陈旧确认防护
 * ================================================================ */

static void test_confirm_regression_and_stale_edge(void)
{
    TEST("position regress from WAIT_CONFIRM + stale confirm edge ignored");
    motor_bench_step_t steps[MOTOR_BENCH_MAX_STEPS];
    uint8_t count = 0;
    BUILD_PROGRAM(MOTOR_BENCH_TYPE_PULSATOR, steps, count);
    motor_bench_fsm_ctx_t ctx;
    motor_bench_fsm_init(&ctx, MOTOR_BENCH_TYPE_PULSATOR, steps, count, 18);
    motor_bench_fsm_output_t out;
    motor_bench_fsm_input_t fin;
    int ok = 1;

    fin = make_fin(15000, MOTOR_BENCH_GATE_OK);
    motor_bench_fsm_tick(&ctx, &fin, &out);           /* WAIT_POSITION */
    fin = make_fin(15100, MOTOR_BENCH_GATE_OK);
    motor_bench_fsm_tick(&ctx, &fin, &out);           /* WAIT_CONFIRM @15000? step_enter=15100 */
    if (ctx.state != MOTOR_BENCH_STATE_WAIT_CONFIRM) ok = 0;

    /* 位置回退（错误霍尔）→ 回到 WAIT_POSITION，超时重新计时 */
    fin = make_fin(15200, MOTOR_BENCH_GATE_POSITION_MISMATCH);
    motor_bench_fsm_tick(&ctx, &fin, &out);
    if (ctx.state != MOTOR_BENCH_STATE_WAIT_POSITION) ok = 0;
    if (out.terminal || out.request_all_off || out.submit_pulse) ok = 0;
    if (ctx.step_enter_ms != 15200) ok = 0;
    if (ctx.pulse_issued) ok = 0;

    /* 陈旧确认边沿：仍在 WAIT_POSITION 时带确认 + OK → 只进入 WAIT_CONFIRM，不提交 */
    fin = make_fin(15300, MOTOR_BENCH_GATE_OK);
    fin.user_confirm_pulse = true;
    motor_bench_fsm_tick(&ctx, &fin, &out);
    if (ctx.state != MOTOR_BENCH_STATE_WAIT_CONFIRM) ok = 0;
    if (out.submit_pulse) ok = 0;                     /* 必须两次 tick 分离 */
    if (ctx.pulse_issued) ok = 0;

    /* 下一 tick 再确认 → 正常提交 */
    fin = make_fin(15400, MOTOR_BENCH_GATE_OK);
    fin.user_confirm_pulse = true;
    motor_bench_fsm_tick(&ctx, &fin, &out);
    if (out.submit_pulse != true) ok = 0;
    if (ctx.state != MOTOR_BENCH_STATE_PULSING) ok = 0;

    /* 提交后确认边沿被消费：PULSING 中即使再带确认也不会重复提交 */
    fin = make_fin(15450, MOTOR_BENCH_GATE_OK);
    fin.user_confirm_pulse = true;
    motor_bench_fsm_tick(&ctx, &fin, &out);
    if (out.submit_pulse) ok = 0;

    if (ok) PASS(); else FAIL("regression/stale-edge mismatch");
}

/* ================================================================
 * [10] ≥10k 随机压力
 * ================================================================ */

/* 走完整 3 步序列 10k 轮，逐 tick 校验不变式 + 每次恰 3 脉冲 */
static void stress_complete_rounds(void)
{
    TEST("10k full 3-step random-timing completions, invariants hold");
    int failures = 0;
    int ok = 1;
    for (int round = 0; round < 10000; round++) {
        motor_bench_step_t steps[MOTOR_BENCH_MAX_STEPS];
        uint8_t count = 0;
        BUILD_PROGRAM(MOTOR_BENCH_TYPE_HALL_SEQUENCE, steps, count);
        motor_bench_fsm_ctx_t ctx;
        motor_bench_fsm_init(&ctx, MOTOR_BENCH_TYPE_HALL_SEQUENCE, steps, count,
                             (machine_request_id_t)(1u + (uint32_t)round));
        motor_bench_fsm_output_t out;
        motor_bench_fsm_input_t fin;
        uint32_t t = rng_next() % 100000u;
        int pulses = 0;

        for (int i = 0; i < 3000 && !motor_bench_fsm_is_terminal(&ctx); i++) {
            t += (rng_next() % 60u) + 1u;
            motor_bench_state_t before = ctx.state;
            drum_position_t req = ctx.steps[ctx.current_step].required_position;
            fin = make_fin(t, MOTOR_BENCH_GATE_OK);
            fin.forbidden_outputs_off = true;
            if (ctx.state == MOTOR_BENCH_STATE_WAIT_CONFIRM) {
                fin.user_confirm_pulse = (rng_next() % 10u) < 9u;   /* 通常确认 */
            }
            if (ctx.state == MOTOR_BENCH_STATE_PULSING) {
                /* 提交后：宽限内可停留数 tick，随后运行数 tick，再回 IDLE */
                uint32_t el = t - ctx.pulse_issued_ms;
                if (el <= 40) {
                    fin.bl50_idle = true;              /* 宽限内尚未受理 */
                } else if (el <= 160) {
                    fin.bl50_idle = false; fin.bl50_pwm = 15;   /* 运行窗口 */
                } else {
                    fin.bl50_idle = true; fin.bl50_pwm = 0;     /* 回 IDLE */
                }
            }
            if (ctx.state == MOTOR_BENCH_STATE_OFF_CONFIRM) {
                fin.bl50_idle = true; fin.bl50_pwm = 0;
            }
            motor_bench_fsm_tick(&ctx, &fin, &out);
            if (out.submit_pulse) pulses++;
            check_invariants(false, before, &ctx, &fin, &out);
            if (invariant_failures) { failures += invariant_failures; invariant_failures = 0; ok = 0; break; }
        }
        if (!motor_bench_fsm_is_terminal(&ctx) || pulses != 3) { ok = 0; }
        if (ctx.terminal == MOTOR_BENCH_TERMINAL_COMPLETE) {
            if (!ctx.output_confirmed_off) ok = 0;
        }
        if (!ok) break;
    }
    if (ok) PASS(); else FAIL("stress complete mismatch");
}

/* 随机游走 10k 轮：随机良性/致命门禁、随机确认/取消/急停/禁止输出，
 * 逐 tick 校验不变式；每轮强制终止以聚合终态种类。 */
static void stress_random_walks(void)
{
    TEST("10k random walks, all invariant + terminal aggregate");
    int failures = 0;
    int ok = 1;
    int n_complete = 0, n_rejected = 0, n_interrupted = 0, n_timeout = 0, n_fault = 0;
    for (int round = 0; round < 10000; round++) {
        motor_bench_step_t steps[MOTOR_BENCH_MAX_STEPS];
        uint8_t count = 0;
        BUILD_PROGRAM(MOTOR_BENCH_TYPE_HALL_SEQUENCE, steps, count);
        motor_bench_fsm_ctx_t ctx;
        motor_bench_fsm_init(&ctx, MOTOR_BENCH_TYPE_HALL_SEQUENCE, steps, count,
                             (machine_request_id_t)(100000u + (uint32_t)round));
        motor_bench_fsm_output_t out;
        motor_bench_fsm_input_t fin;
        motor_bench_gate_input_t gin;
        uint32_t t = (rng_next() % 100000u) + 1u;

        for (int i = 0; i < 5000 && !motor_bench_fsm_is_terminal(&ctx); i++) {
            t += (rng_next() % 120u) + 1u;
            motor_bench_state_t before = ctx.state;
            drum_position_t req = ctx.steps[ctx.current_step].required_position;

            /* 构造随机门禁输入 */
            if ((rng_next() % 10u) < 6u) {
                /* 健康位置（大概率匹配当前步） */
                gin = gate_at(req, ((rng_next() % 10u) < 8u) ? req : (drum_position_t)(((uint32_t)req + 45u) % 180u));
            } else {
                gin = make_gate();
                gin.required_pos = req;
                gin.pos = req;
                if (rng_next() % 3u == 0u) { gin.pos_valid = false; }
                if (rng_next() % 3u == 0u) { gin.pos_fresh = false; }
                if (rng_next() % 3u == 0u) { gin.pos_stable = false; }
                if (rng_next() % 5u == 0u) { gin.hall_conflict = true; }
                if (rng_next() % 7u == 0u) { gin.bl50_idle = false; }
                if (rng_next() % 9u == 0u) { gin.forbidden_outputs_off = false; }
            }
            if (rng_next() % 37u == 0u) { gin.exec_idle = false; }
            if (rng_next() % 41u == 0u) { gin.self_test_busy = true; }
            if (rng_next() % 53u == 0u) { gin.fault_active = true; }
            if (rng_next() % 59u == 0u) { gin.emergency_active = true; }

            motor_bench_gate_result_t gate = motor_bench_gate_check(&gin);
            fin = make_fin(t, gate);
            fin.forbidden_outputs_off = gin.forbidden_outputs_off;
            fin.bl50_idle = gin.bl50_idle;
            if (ctx.state == MOTOR_BENCH_STATE_PULSING) {
                /* 模拟提交后 BL50 受理：宽限内 IDLE，随后随机运行/空闲 */
                uint32_t el = t - ctx.pulse_issued_ms;
                if (el <= 40) { fin.bl50_idle = true; fin.bl50_pwm = 0; }
                else if (el <= 200) { fin.bl50_idle = (rng_next() % 4u) == 0u; fin.bl50_pwm = fin.bl50_idle ? 0 : 15; }
                else { fin.bl50_idle = (rng_next() % 3u) != 0u; fin.bl50_pwm = fin.bl50_idle ? 0 : 15; }
            }
            if (ctx.state == MOTOR_BENCH_STATE_WAIT_CONFIRM) {
                fin.user_confirm_pulse = (rng_next() % 10u) < 8u;
            }
            if (rng_next() % 200u == 0u) { fin.cancel_requested = true; }
            if (rng_next() % 250u == 0u) { fin.emergency = true; }
            if (rng_next() % 300u == 0u) { fin.forbidden_outputs_off = false; }
            if (rng_next() % 400u == 0u) { fin.submit_failed = true; }
            if (rng_next() % 500u == 0u) { fin.bl50_fault = true; }

            motor_bench_fsm_tick(&ctx, &fin, &out);
            check_invariants(false, before, &ctx, &fin, &out);
            if (invariant_failures) { failures += invariant_failures; invariant_failures = 0; ok = 0; break; }
        }

        /* 强制终止，避免某轮永不收敛 */
        if (!motor_bench_fsm_is_terminal(&ctx)) {
            motor_bench_state_t before_term = ctx.state;
            fin = make_fin(t + 1, MOTOR_BENCH_GATE_OK);
            fin.cancel_requested = true;
            motor_bench_fsm_tick(&ctx, &fin, &out);
            check_invariants(false, before_term, &ctx, &fin, &out);
            if (invariant_failures) { failures += invariant_failures; invariant_failures = 0; ok = 0; }
        }

        switch (ctx.terminal) {
        case MOTOR_BENCH_TERMINAL_COMPLETE:    n_complete++;    break;
        case MOTOR_BENCH_TERMINAL_REJECTED:    n_rejected++;    break;
        case MOTOR_BENCH_TERMINAL_INTERRUPTED: n_interrupted++; break;
        case MOTOR_BENCH_TERMINAL_TIMEOUT:     n_timeout++;     break;
        case MOTOR_BENCH_TERMINAL_FAULT:       n_fault++;       break;
        default:                               ok = 0;          break;
        }
        if (!ok) break;
    }

    printf("      [rounds] complete=%d rejected=%d interrupted=%d timeout=%d fault=%d\n",
           n_complete, n_rejected, n_interrupted, n_timeout, n_fault);
    /* 随机游走应覆盖多类终态；至少观察到完成 + 至少一类非完成终态 */
    if (n_complete == 0) ok = 0;
    if (n_rejected + n_interrupted + n_timeout + n_fault == 0) ok = 0;
    if (ok) PASS(); else FAIL("random walk mismatch");
}

/* ================================================================
 * main
 * ================================================================ */

int main(void)
{
    printf("=== motor_bench core host tests ===\n\n");
    rng_seed(0x1C2F3A4Bu);

    printf("[1] type name / step count\n");
    test_type_names();

    printf("\n[2] default program (production contract)\n");
    test_default_program();

    printf("\n[3] gate fail-closed\n");
    test_gate_fail_closed();

    printf("\n[4] single-step happy path\n");
    test_single_step_happy_path();

    printf("\n[5] hall sequence happy path\n");
    test_hall_sequence_happy_path();
    test_hall_sequence_deterministic();

    printf("\n[6] wrong position / multi-hall / timeout\n");
    test_wrong_position_rejection();
    test_multi_hall_rejection();
    test_confirm_timeout();

    printf("\n[7] cancel / emergency / forbidden-output invariant\n");
    test_cancel_interrupts();
    test_emergency_interrupts();
    test_forbidden_output_invariant();
    test_forbidden_aggregation();
    test_external_self_test_busy_aggregation();

    printf("\n[8] pulsing / off-confirm fault paths + terminal exactly-once\n");
    test_pulsing_fault_paths();
    test_off_confirm_timeout();
    test_terminal_exactly_once();

    printf("\n[9] confirm regression + stale edge\n");
    test_confirm_regression_and_stale_edge();

    printf("\n[10] >=10k random stress\n");
    stress_complete_rounds();
    stress_random_walks();

    printf("\n=== result: %d/%d passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
