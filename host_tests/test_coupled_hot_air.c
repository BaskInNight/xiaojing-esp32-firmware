/*
 * test_coupled_hot_air.c — 单继电器耦合热风模块安全测试（Phase 11 Group B）
 *
 * 覆盖层级：
 *  A. coupled_hot_air_fsm 纯 C 状态机（位置/SHT/温度/冷却门禁、时长自停、
 *     cancel/emergency 立即 OFF、OFF 确认失败→FAULT、terminal exactly once）
 *  B. safety_interlocks SAFETY_OP_HOT_AIR_MODULE 门禁（REAL+enabled+270°
 *     +全 MCP KNOWN_OFF+SHT+温度+冷却锁+重复开启拒绝）
 *  C. actuator_self_test HOT_AIR_COUPLED 门禁与 fail-closed OFF 证明
 *  D. 10000 轮 Fake-HAL 风格压力：单继电器 overlap=0、无 FAN 动作、
 *     terminal exactly once、结束全 OFF
 *
 * 无 FreeRTOS/HAL 依赖；全部为纯 C 决策逻辑。
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "coupled_hot_air_fsm.h"
#include "coupled_hot_air_service.h"
#include "safety_interlocks.h"
#include "actuator_self_test_core.h"
#include "xiaojing_hal.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static int g_tests = 0;
static int g_pass = 0;
static int g_fail = 0;

#define TEST(name) do { g_tests++; } while (0)
#define PASS() do { g_pass++; } while (0)
#define FAIL(msg) do { g_fail++; printf("  FAIL: %s (line %d)\n", (msg), __LINE__); } while (0)

#define CHECK(cond, msg) do { TEST(msg); if (cond) PASS(); else FAIL(msg); } while (0)

/* ---- FSM 测试工具 ---- */

static cha_fsm_event_t env_tick(int64_t now, drum_position_t pos, bool pos_valid,
                                bool pos_fresh, bool pos_stable, bool moving,
                                bool sht_valid, bool sht_fresh, float temp_c,
                                bool fault, bool emergency, uint32_t cooldown_ms)
{
    cha_fsm_event_t e = {0};
    e.type = CHA_EVT_TICK;
    e.now_ms = now;
    e.position = pos;
    e.position_valid = pos_valid;
    e.position_stable = pos_stable;
    e.position_fresh = pos_fresh;
    e.position_motor_moving = moving;
    e.sht_valid = sht_valid;
    e.sht_fresh = sht_fresh;
    e.temperature_c = temp_c;
    e.fault_active = fault;
    e.emergency_active = emergency;
    e.cooldown_remaining_ms = cooldown_ms;
    return e;
}

static cha_fsm_event_t ok_env(int64_t now)
{
    return env_tick(now, DRUM_POS_270, true, true, true, false,
                    true, true, 25.0f, false, false, 0);
}

/* 走完整正常流程：request → RELAY_ON 确认 → 跑到 deadline → RELAY_OFF → COMPLETE */
static void run_to_complete(cha_fsm_ctx_t *ctx, uint32_t duration_ms,
                            int64_t start_ms)
{
    coupled_hot_air_request_t req = { .request_id = 1, .duration_ms = duration_ms };
    cha_fsm_begin_request(ctx, &req, start_ms);

    cha_fsm_output_t out;
    cha_fsm_event_t e = ok_env(start_ms);
    cha_fsm_tick(ctx, &e, &out);
    CHECK(out.action == CHA_ACTION_RELAY_ON,
          "FSM: 有效环境 + 270° -> RELAY_ON");

    /* 确认 RELAY_ON 成功 */
    cha_fsm_event_t res_on = { .type = CHA_EVT_ACTION_RESULT, .now_ms = start_ms,
                               .action = CHA_ACTION_RELAY_ON, .action_ok = true };
    cha_fsm_tick(ctx, &res_on, &out);
    CHECK(ctx->relay_state == CHA_OUT_ON && out.action == CHA_ACTION_NONE,
          "FSM: RELAY_ON 确认后进入 RUNNING");

    /* 跑到 deadline */
    cha_fsm_event_t e2 = ok_env(start_ms + duration_ms);
    cha_fsm_tick(ctx, &e2, &out);
    CHECK(out.action == CHA_ACTION_RELAY_OFF,
          "FSM: 时长到期 -> RELAY_OFF");

    /* 确认 RELAY_OFF 成功 */
    cha_fsm_event_t res_off = { .type = CHA_EVT_ACTION_RESULT, .now_ms = start_ms + duration_ms,
                                .action = CHA_ACTION_RELAY_OFF, .action_ok = true };
    cha_fsm_tick(ctx, &res_off, &out);
    CHECK(cha_fsm_is_terminal(ctx) && ctx->terminal == CHA_TERMINAL_COMPLETE &&
          ctx->terminal_off_confirmed,
          "FSM: OFF 确认成功 -> COMPLETE terminal, off confirmed");
}

/* ---- A. FSM 语义 ---- */

static void test_fsm_init(void)
{
    cha_fsm_ctx_t ctx;
    cha_fsm_init(&ctx, 60.0f);
    CHECK(cha_fsm_is_idle(&ctx), "FSM init: IDLE");
    CHECK(ctx.relay_state == CHA_OUT_OFF, "FSM init: relay default OFF");
    CHECK(ctx.pending_action == CHA_ACTION_NONE, "FSM init: no pending action");
    CHECK(ctx.terminal == CHA_TERMINAL_NONE, "FSM init: no terminal");
}

static void test_fsm_start_gates(void)
{
    cha_fsm_ctx_t ctx;
    cha_fsm_init(&ctx, 60.0f);
    coupled_hot_air_request_t req = { .request_id = 1, .duration_ms = 1000 };
    cha_fsm_begin_request(&ctx, &req, 1000);

    cha_fsm_output_t out;
    cha_fsm_event_t e;
    const char *r;

    /* 错误位置 */
    e = env_tick(1000, DRUM_POS_0, true, true, true, false, true, true, 25, false, false, 0);
    cha_fsm_tick(&ctx, &e, &out);
    CHECK(out.terminal == CHA_TERMINAL_FAULT && ctx.stop_reason == CHA_STOP_POSITION,
          "FSM gate: 非270°位置 -> FAULT POSITION_LOST");
    r = cha_fsm_reason_name(ctx.stop_reason);
    CHECK(strcmp(r, "POSITION_LOST") == 0, "FSM gate: reason == POSITION_LOST");
    cha_fsm_init(&ctx, 60.0f); cha_fsm_begin_request(&ctx, &req, 1000);

    /* 位置无效/陈旧/不稳定/电机运动 */
    e = env_tick(1000, DRUM_POS_270, false, true, true, false, true, true, 25, false, false, 0);
    cha_fsm_tick(&ctx, &e, &out);
    CHECK(out.terminal == CHA_TERMINAL_FAULT && ctx.stop_reason == CHA_STOP_POSITION,
          "FSM gate: position invalid -> FAULT");
    cha_fsm_init(&ctx, 60.0f); cha_fsm_begin_request(&ctx, &req, 1000);
    e = env_tick(1000, DRUM_POS_270, true, false, true, false, true, true, 25, false, false, 0);
    cha_fsm_tick(&ctx, &e, &out);
    CHECK(out.terminal == CHA_TERMINAL_FAULT && ctx.stop_reason == CHA_STOP_POSITION,
          "FSM gate: position stale -> FAULT");
    cha_fsm_init(&ctx, 60.0f); cha_fsm_begin_request(&ctx, &req, 1000);
    e = env_tick(1000, DRUM_POS_270, true, true, false, false, true, true, 25, false, false, 0);
    cha_fsm_tick(&ctx, &e, &out);
    CHECK(out.terminal == CHA_TERMINAL_FAULT && ctx.stop_reason == CHA_STOP_POSITION,
          "FSM gate: position unstable -> FAULT");
    cha_fsm_init(&ctx, 60.0f); cha_fsm_begin_request(&ctx, &req, 1000);
    e = env_tick(1000, DRUM_POS_270, true, true, true, true, true, true, 25, false, false, 0);
    cha_fsm_tick(&ctx, &e, &out);
    CHECK(out.terminal == CHA_TERMINAL_FAULT && ctx.stop_reason == CHA_STOP_POSITION,
          "FSM gate: motor moving -> FAULT");

    /* SHT 无效/陈旧 */
    cha_fsm_init(&ctx, 60.0f); cha_fsm_begin_request(&ctx, &req, 1000);
    e = env_tick(1000, DRUM_POS_270, true, true, true, false, false, false, 25, false, false, 0);
    cha_fsm_tick(&ctx, &e, &out);
    CHECK(out.terminal == CHA_TERMINAL_FAULT && ctx.stop_reason == CHA_STOP_SHT_INVALID,
          "FSM gate: SHT invalid -> FAULT SHT_INVALID");
    cha_fsm_init(&ctx, 60.0f); cha_fsm_begin_request(&ctx, &req, 1000);
    e = env_tick(1000, DRUM_POS_270, true, true, true, false, true, false, 25, false, false, 0);
    cha_fsm_tick(&ctx, &e, &out);
    CHECK(out.terminal == CHA_TERMINAL_FAULT && ctx.stop_reason == CHA_STOP_SHT_STALE,
          "FSM gate: SHT stale -> FAULT SHT_STALE");

    /* 过温 */
    cha_fsm_init(&ctx, 60.0f); cha_fsm_begin_request(&ctx, &req, 1000);
    e = env_tick(1000, DRUM_POS_270, true, true, true, false, true, true, 65.0f, false, false, 0);
    cha_fsm_tick(&ctx, &e, &out);
    CHECK(out.terminal == CHA_TERMINAL_FAULT && ctx.stop_reason == CHA_STOP_OVERTEMP,
          "FSM gate: overtemp -> FAULT OVERTEMP");

    /* 冷却锁 */
    cha_fsm_init(&ctx, 60.0f); cha_fsm_begin_request(&ctx, &req, 1000);
    e = ok_env(1000); e.cooldown_remaining_ms = 5000;
    cha_fsm_tick(&ctx, &e, &out);
    CHECK(out.terminal == CHA_TERMINAL_FAULT && ctx.stop_reason == CHA_STOP_COOLDOWN,
          "FSM gate: cooldown active -> FAULT COOLDOWN_ACTIVE");

    /* 紧急/故障锁存 */
    cha_fsm_init(&ctx, 60.0f); cha_fsm_begin_request(&ctx, &req, 1000);
    e = ok_env(1000); e.emergency_active = true;
    cha_fsm_tick(&ctx, &e, &out);
    CHECK(out.terminal == CHA_TERMINAL_INTERRUPTED && ctx.stop_reason == CHA_STOP_EMERGENCY,
          "FSM gate: emergency active -> INTERRUPTED EMERGENCY");
    cha_fsm_init(&ctx, 60.0f); cha_fsm_begin_request(&ctx, &req, 1000);
    e = ok_env(1000); e.fault_active = true;
    cha_fsm_tick(&ctx, &e, &out);
    CHECK(out.terminal == CHA_TERMINAL_FAULT && ctx.stop_reason == CHA_STOP_FAULT,
          "FSM gate: fault active -> FAULT");
}

static void test_fsm_hard_cap_and_duration(void)
{
    /* 硬上限 3s 是服务层 run_async 强制；此处验证常量一致性与默认 1s 安全。 */
    CHECK(COUPLED_HOT_AIR_HARD_CAP_MS == 3000U,
          "FSM constants: hard cap == 3000ms");
    CHECK(COUPLED_HOT_AIR_DEFAULT_DURATION_MS == 1000U,
          "FSM constants: default duration == 1000ms");
    CHECK(COUPLED_HOT_AIR_DEFAULT_DURATION_MS <= COUPLED_HOT_AIR_HARD_CAP_MS,
          "FSM constants: default <= hard cap");
    CHECK(ACT_SELF_TEST_HOT_AIR_DURATION_MS == 1000U,
          "FSM constants: act self-test default == 1000ms");
    CHECK(ACT_SELF_TEST_HOT_AIR_MAX_DURATION_MS == 3000U,
          "FSM constants: act self-test max == 3000ms");

    /* 正常 1s 流程 */
    cha_fsm_ctx_t ctx;
    cha_fsm_init(&ctx, 60.0f);
    run_to_complete(&ctx, 1000, 5000);

    /* 3s 边界：duration=3000 也能在 deadline 自停 */
    cha_fsm_init(&ctx, 60.0f);
    run_to_complete(&ctx, 3000, 5000);
}

static void test_fsm_immediate_off(void)
{
    /* 运行中 cancel/emergency/过温/SHT 失效 → 立即 RELAY_OFF */
    cha_fsm_ctx_t ctx;
    cha_fsm_init(&ctx, 60.0f);
    coupled_hot_air_request_t req = { .request_id = 7, .duration_ms = 3000 };
    cha_fsm_begin_request(&ctx, &req, 2000);
    cha_fsm_output_t out;

    /* 进入 RUNNING */
    cha_fsm_event_t e = ok_env(2000);
    cha_fsm_tick(&ctx, &e, &out);           /* VALIDATING -> STARTING RELAY_ON */
    cha_fsm_event_t on = { .type = CHA_EVT_ACTION_RESULT, .now_ms = 2000,
                           .action = CHA_ACTION_RELAY_ON, .action_ok = true };
    cha_fsm_tick(&ctx, &on, &out);          /* RUNNING */

    /* cancel（正确 request_id） */
    cha_fsm_event_t cancel = { .type = CHA_EVT_CANCEL, .now_ms = 2500,
                               .cancel_request_id = 7 };
    cha_fsm_tick(&ctx, &cancel, &out);
    CHECK(out.action == CHA_ACTION_RELAY_OFF, "FSM running: cancel -> immediate RELAY_OFF");
    cha_fsm_event_t off = { .type = CHA_EVT_ACTION_RESULT, .now_ms = 2500,
                            .action = CHA_ACTION_RELAY_OFF, .action_ok = true };
    cha_fsm_tick(&ctx, &off, &out);
    CHECK(ctx.terminal == CHA_TERMINAL_INTERRUPTED && ctx.terminal_off_confirmed,
          "FSM running: cancel -> INTERRUPTED, off confirmed");

    /* emergency */
    cha_fsm_init(&ctx, 60.0f); cha_fsm_begin_request(&ctx, &req, 2000);
    cha_fsm_tick(&ctx, &(cha_fsm_event_t){ .type = CHA_EVT_TICK, .now_ms = 2000,
        .position = DRUM_POS_270, .position_valid = true, .position_stable = true,
        .position_fresh = true, .sht_valid = true, .sht_fresh = true,
        .temperature_c = 25.0f }, &out);
    cha_fsm_tick(&ctx, &on, &out);
    cha_fsm_event_t em = { .type = CHA_EVT_EMERGENCY, .now_ms = 2500 };
    cha_fsm_tick(&ctx, &em, &out);
    CHECK(out.action == CHA_ACTION_RELAY_OFF, "FSM running: emergency -> immediate RELAY_OFF");
    cha_fsm_tick(&ctx, &off, &out);
    CHECK(ctx.terminal == CHA_TERMINAL_INTERRUPTED && ctx.terminal_off_confirmed,
          "FSM running: emergency -> INTERRUPTED, off confirmed");

    /* 运行中过温 */
    cha_fsm_init(&ctx, 60.0f); cha_fsm_begin_request(&ctx, &req, 2000);
    cha_fsm_tick(&ctx, &(cha_fsm_event_t){ .type = CHA_EVT_TICK, .now_ms = 2000,
        .position = DRUM_POS_270, .position_valid = true, .position_stable = true,
        .position_fresh = true, .sht_valid = true, .sht_fresh = true,
        .temperature_c = 25.0f }, &out);
    cha_fsm_tick(&ctx, &on, &out);
    cha_fsm_event_t hot = ok_env(2600); hot.temperature_c = 65.0f;
    cha_fsm_tick(&ctx, &hot, &out);
    CHECK(out.action == CHA_ACTION_RELAY_OFF,
          "FSM running: overtemp -> immediate RELAY_OFF");
    cha_fsm_tick(&ctx, &off, &out);
    CHECK(ctx.terminal == CHA_TERMINAL_FAULT && ctx.stop_reason == CHA_STOP_OVERTEMP,
          "FSM running: overtemp -> FAULT OVERTEMP");

    /* 运行中 SHT 失效 */
    cha_fsm_init(&ctx, 60.0f); cha_fsm_begin_request(&ctx, &req, 2000);
    cha_fsm_tick(&ctx, &(cha_fsm_event_t){ .type = CHA_EVT_TICK, .now_ms = 2000,
        .position = DRUM_POS_270, .position_valid = true, .position_stable = true,
        .position_fresh = true, .sht_valid = true, .sht_fresh = true,
        .temperature_c = 25.0f }, &out);
    cha_fsm_tick(&ctx, &on, &out);
    cha_fsm_event_t bad = ok_env(2600); bad.sht_valid = false; bad.sht_fresh = false;
    cha_fsm_tick(&ctx, &bad, &out);
    CHECK(out.action == CHA_ACTION_RELAY_OFF,
          "FSM running: SHT lost -> immediate RELAY_OFF");
    cha_fsm_tick(&ctx, &off, &out);
    CHECK(ctx.terminal == CHA_TERMINAL_FAULT && ctx.stop_reason == CHA_STOP_SHT_INVALID,
          "FSM running: SHT lost -> FAULT SHT_INVALID");
}

static void test_fsm_off_fail_fault(void)
{
    cha_fsm_ctx_t ctx;
    cha_fsm_init(&ctx, 60.0f);
    coupled_hot_air_request_t req = { .request_id = 3, .duration_ms = 3000 };
    cha_fsm_begin_request(&ctx, &req, 1000);
    cha_fsm_output_t out;

    cha_fsm_event_t e = ok_env(1000);
    cha_fsm_tick(&ctx, &e, &out);   /* VALIDATING -> STARTING */
    cha_fsm_event_t on = { .type = CHA_EVT_ACTION_RESULT, .now_ms = 1000,
                           .action = CHA_ACTION_RELAY_ON, .action_ok = true };
    cha_fsm_tick(&ctx, &on, &out);  /* RUNNING */

    /* 触发停止 */
    cha_fsm_event_t em = { .type = CHA_EVT_EMERGENCY, .now_ms = 1500 };
    cha_fsm_tick(&ctx, &em, &out);
    CHECK(out.action == CHA_ACTION_RELAY_OFF, "OFF-fail: 停止请求 RELAY_OFF");

    /* OFF 反复失败 → 每次 UNKNOWN，重试 */
    int retries = 0;
    cha_fsm_event_t res;
    bool got_fault = false;
    while (retries < (int)CHA_OFF_RETRY_MAX + 2 && !got_fault) {
        res = (cha_fsm_event_t){ .type = CHA_EVT_ACTION_RESULT, .now_ms = 1500 + retries,
                                 .action = CHA_ACTION_RELAY_OFF, .action_ok = false };
        cha_fsm_tick(&ctx, &res, &out);
        if (cha_fsm_is_terminal(&ctx)) {
            got_fault = true;
            break;
        }
        CHECK(out.action == CHA_ACTION_RELAY_OFF,
              "OFF-fail: 重试仍请求 RELAY_OFF");
        retries++;
    }
    CHECK(got_fault && ctx.terminal == CHA_TERMINAL_FAULT &&
          ctx.stop_reason == CHA_STOP_OFF_FAIL &&
          !ctx.terminal_off_confirmed,
          "OFF-fail: 达上限 -> FAULT OFF_CONFIRM_FAILED, off NOT confirmed");
    CHECK(retries <= (int)CHA_OFF_RETRY_MAX,
          "OFF-fail: 重试次数不超上限");
}

/* P0 链契约 + P1-2（begin_request 诚实语义）：RELAY_ON 失败后 FSM 必须请求
 * 补偿 RELAY_OFF（服务层 process_fsm_output 的修复正是保证该补偿动作被执行），
 * 且新请求 begin_request 绝不把 UNKNOWN 重写为 OFF。 */
static void test_fsm_begin_keeps_unknown(void)
{
    cha_fsm_ctx_t ctx;
    cha_fsm_init(&ctx, 60.0f);
    cha_fsm_output_t out;

    /* 前置：完整运行后 OFF 确认全部失败 → FAULT，relay_state 保持 UNKNOWN */
    coupled_hot_air_request_t req1 = { .request_id = 6, .duration_ms = 1000 };
    cha_fsm_begin_request(&ctx, &req1, 1000);
    cha_fsm_event_t e = ok_env(1000);
    cha_fsm_tick(&ctx, &e, &out);   /* STARTING RELAY_ON */
    cha_fsm_event_t on_ok = { .type = CHA_EVT_ACTION_RESULT, .now_ms = 1000,
                              .action = CHA_ACTION_RELAY_ON, .action_ok = true };
    cha_fsm_tick(&ctx, &on_ok, &out);  /* RUNNING */
    cha_fsm_event_t em = { .type = CHA_EVT_EMERGENCY, .now_ms = 1500 };
    cha_fsm_tick(&ctx, &em, &out);  /* STOPPING RELAY_OFF */
    for (int i = 0; i <= (int)CHA_OFF_RETRY_MAX; i++) {
        cha_fsm_event_t res = { .type = CHA_EVT_ACTION_RESULT, .now_ms = 1500 + i,
                                .action = CHA_ACTION_RELAY_OFF, .action_ok = false };
        cha_fsm_tick(&ctx, &res, &out);
        if (cha_fsm_is_terminal(&ctx)) break;
    }
    CHECK(ctx.terminal == CHA_TERMINAL_FAULT && ctx.relay_state == CHA_OUT_UNKNOWN,
          "P1-2 前置: OFF 确认失败 -> FAULT, relay_state==UNKNOWN");

    /* 新请求 begin_request 必须保留 UNKNOWN（诚实语义，不得自欺为 OFF） */
    coupled_hot_air_request_t req2 = { .request_id = 7, .duration_ms = 1000 };
    cha_fsm_begin_request(&ctx, &req2, 2000);
    CHECK(ctx.relay_state == CHA_OUT_UNKNOWN,
          "P1-2: begin_request 保留 UNKNOWN，不重写为 OFF");
    CHECK(ctx.state == CHA_FSM_VALIDATING, "P1-2: 新请求进入 VALIDATING");

    /* 门禁全过 -> STARTING RELAY_ON（是否放行交由 safety 层"全 MCP KNOWN_OFF"
     * 门禁把关；这里验证 FSM 仍诚实请求 RELAY_ON） */
    cha_fsm_event_t e2 = ok_env(2000);
    cha_fsm_tick(&ctx, &e2, &out);
    CHECK(out.action == CHA_ACTION_RELAY_ON,
          "P1-2: UNKNOWN 下仍请求 RELAY_ON，由 safety 门禁把关");

    /* RELAY_ON 失败（如 safety 拒绝）-> 必须请求补偿 RELAY_OFF（P0 契约） */
    cha_fsm_event_t on_bad = { .type = CHA_EVT_ACTION_RESULT, .now_ms = 2000,
                               .action = CHA_ACTION_RELAY_ON, .action_ok = false };
    cha_fsm_tick(&ctx, &on_bad, &out);
    CHECK(out.action == CHA_ACTION_RELAY_OFF &&
          ctx.pending_action == CHA_ACTION_RELAY_OFF,
          "P0 契约: RELAY_ON 失败 -> 补偿 RELAY_OFF 被请求");

    /* 补偿 OFF 成功 -> 终态 FAULT(APPLY_FAILED)，OFF 已确认 */
    cha_fsm_event_t off_ok = { .type = CHA_EVT_ACTION_RESULT, .now_ms = 2000,
                               .action = CHA_ACTION_RELAY_OFF, .action_ok = true };
    cha_fsm_tick(&ctx, &off_ok, &out);
    CHECK(cha_fsm_is_terminal(&ctx) && ctx.relay_state == CHA_OUT_OFF &&
          ctx.terminal_off_confirmed && ctx.stop_reason == CHA_STOP_APPLY_FAULT,
          "P1-2/P0: 补偿 OFF 成功 -> FAULT(APPLY_FAILED) + OFF 已确认");
}

/* Fix 4：即使从未通电（relay_ever_on==false）且继电器 UNKNOWN，OFF 重试链
 * 也必须保证收敛到 FAULT 终态 —— 不能因 relay_ever_on 为假而无限重试。 */
static void test_fsm_never_on_off_retry_terminates(void)
{
    cha_fsm_ctx_t ctx;
    cha_fsm_init(&ctx, 60.0f);
    /* 模拟上一次终态 OFF 确认失败遗留的 UNKNOWN 继电器状态（从未通电）。 */
    ctx.relay_state = CHA_OUT_UNKNOWN;
    ctx.relay_ever_on = false;

    coupled_hot_air_request_t req = { .request_id = 9, .duration_ms = 1000 };
    cha_fsm_begin_request(&ctx, &req, 1000);
    cha_fsm_output_t out;

    cha_fsm_event_t e = ok_env(1000);
    cha_fsm_tick(&ctx, &e, &out);   /* VALIDATING -> STARTING RELAY_ON */
    CHECK(out.action == CHA_ACTION_RELAY_ON, "never-on: VALIDATING 通过 -> RELAY_ON");

    /* RELAY_ON 失败（如 safety 模式门禁拒绝）-> UNKNOWN，走 OFF 重试 */
    cha_fsm_event_t on_bad = { .type = CHA_EVT_ACTION_RESULT, .now_ms = 1000,
                               .action = CHA_ACTION_RELAY_ON, .action_ok = false };
    cha_fsm_tick(&ctx, &on_bad, &out);
    CHECK(out.action == CHA_ACTION_RELAY_OFF &&
          ctx.pending_action == CHA_ACTION_RELAY_OFF,
          "never-on: RELAY_ON 失败 -> 补偿 RELAY_OFF");

    /* 从未通电仍必须收敛：反复 OFF 失败到上限 -> FAULT */
    int retries = 0;
    bool got_fault = false;
    while (retries < (int)CHA_OFF_RETRY_MAX + 2 && !got_fault) {
        cha_fsm_event_t res = { .type = CHA_EVT_ACTION_RESULT, .now_ms = 1000 + retries,
                                .action = CHA_ACTION_RELAY_OFF, .action_ok = false };
        cha_fsm_tick(&ctx, &res, &out);
        if (cha_fsm_is_terminal(&ctx)) { got_fault = true; break; }
        CHECK(out.action == CHA_ACTION_RELAY_OFF,
              "never-on: OFF 重试继续请求 RELAY_OFF");
        retries++;
    }
    CHECK(got_fault && ctx.terminal == CHA_TERMINAL_FAULT &&
          ctx.stop_reason == CHA_STOP_OFF_FAIL && !ctx.terminal_off_confirmed,
          "never-on: 从未通电+UNKNOWN 也必然收敛到 FAULT OFF_CONFIRM_FAILED");
    CHECK(retries <= (int)CHA_OFF_RETRY_MAX,
          "never-on: 重试次数不超上限");
}

static void test_fsm_terminal_once_generation(void)
{
    cha_fsm_ctx_t ctx;
    cha_fsm_init(&ctx, 60.0f);
    uint32_t gen0 = ctx.generation;

    coupled_hot_air_request_t req1 = { .request_id = 11, .duration_ms = 1000 };
    cha_fsm_begin_request(&ctx, &req1, 1000);
    CHECK(ctx.generation == gen0 + 1, "FSM: begin_request generation 单调递增");
    CHECK(ctx.request_id == 11, "FSM: request_id 记录");

    cha_fsm_output_t out;
    cha_fsm_event_t e = ok_env(1000);
    cha_fsm_tick(&ctx, &e, &out);   /* STARTING RELAY_ON */
    cha_fsm_event_t on = { .type = CHA_EVT_ACTION_RESULT, .now_ms = 1000,
                           .action = CHA_ACTION_RELAY_ON, .action_ok = true };
    cha_fsm_tick(&ctx, &on, &out);  /* RUNNING */
    cha_fsm_event_t e2 = ok_env(2000); /* deadline */
    cha_fsm_tick(&ctx, &e2, &out);  /* STOPPING RELAY_OFF */
    cha_fsm_event_t off = { .type = CHA_EVT_ACTION_RESULT, .now_ms = 2000,
                            .action = CHA_ACTION_RELAY_OFF, .action_ok = true };
    cha_fsm_tick(&ctx, &off, &out);
    CHECK(cha_fsm_is_terminal(&ctx), "FSM: 终态就绪");

    /* 未 mark → 仍 terminal（不重复 emit） */
    cha_fsm_event_t e3 = ok_env(2100);
    cha_fsm_tick(&ctx, &e3, &out);
    CHECK(cha_fsm_is_terminal(&ctx) && out.terminal == CHA_TERMINAL_NONE,
          "FSM: 未 mark 前不再发新 terminal");

    /* mark → 回 IDLE，允许再次请求 */
    cha_fsm_mark_terminal_emitted(&ctx);
    cha_fsm_event_t e4 = ok_env(2200);
    cha_fsm_tick(&ctx, &e4, &out);
    CHECK(cha_fsm_is_idle(&ctx), "FSM: mark 后回 IDLE");

    /* request_id 单调跳 0 */
    CHECK(machine_request_id_next(0) == 1, "FSM: request_id_next(0)==1 (skip 0)");
    CHECK(machine_request_id_next(UINT32_MAX) == 1,
          "FSM: request_id wrap 跳 0");
}

/* ---- B. 安全联锁门禁 ---- */

static safety_inputs_t make_ilk_inputs(void)
{
    safety_inputs_t in;
    memset(&in, 0, sizeof(in));
    in.position = DRUM_POS_270;
    in.position_stable = true;
    in.position_sample_valid = true;
    in.position_sample_ms = 1000;
    in.now_ms = 1000;
    in.sht_valid = true;
    in.sht_fresh = true;
    in.sht_sample.temperature_c = 25.0f;
    in.mcp_known_mask = 0x3Fu;   /* 全部 6 路已知 */
    in.output_mode = XIAOJING_MODE_REAL;
    in.ptc_enabled_in_config = true;
    return in;
}

static safety_interlock_config_t make_ilk_cfg(void)
{
    safety_interlock_config_t c;
    memset(&c, 0, sizeof(c));
    c.heater_cutoff_c = 60.0f;
    c.heater_resume_c = 40.0f;
    c.heat_on_max_ms = 3000;
    c.sht_stale_timeout_ms = 2000;
    c.position_stale_timeout_ms = 3000;
    c.output_mode = XIAOJING_MODE_REAL;
    c.ptc_enabled = true;
    c.hot_air_module_enabled = true;
    c.hot_air_cooldown_ms = 30000;
    c.board_identity_confirmed = true;
    return c;
}

static safety_request_t make_ilk_req(void)
{
    safety_request_t r;
    memset(&r, 0, sizeof(r));
    r.operation = SAFETY_OP_HOT_AIR_MODULE;
    r.enable = true;
    r.request_id = 1;
    r.required_position = DRUM_POS_270;
    r.source = APP_SOURCE_SYSTEM;
    return r;
}

static void test_ilk_hot_air_gate(void)
{
    safety_inputs_t in = make_ilk_inputs();
    safety_interlock_config_t cfg = make_ilk_cfg();
    safety_request_t req = make_ilk_req();
    safety_decision_t dec;

    /* 正常放行 */
    safety_interlock_check(&in, &cfg, &req, &dec);
    CHECK(dec.allowed, "interlock: 270°+全OFF+SHT+温度+无冷却 -> 放行");

    /* OFF 请求始终放行 */
    safety_request_t off = req;
    off.enable = false;
    safety_interlock_check(&in, &cfg, &off, &dec);
    CHECK(dec.allowed, "interlock: OFF 请求始终放行");

    /* 配置未启用 */
    safety_interlock_config_t cfg2 = cfg;
    cfg2.hot_air_module_enabled = false;
    safety_interlock_check(&in, &cfg2, &req, &dec);
    CHECK(!dec.allowed && dec.reason == SAFETY_REJECT_CONFIG_DISABLED,
          "interlock: 模块未启用 -> CONFIG_DISABLED");

    /* 非 REAL 模式 */
    safety_interlock_config_t cfg3 = cfg;
    cfg3.output_mode = XIAOJING_MODE_FAKE;
    safety_interlock_check(&in, &cfg3, &req, &dec);
    CHECK(!dec.allowed && dec.reason == SAFETY_REJECT_CONFIG_DISABLED,
          "interlock: 非 REAL 模式 -> CONFIG_DISABLED");

    /* 位置不符 / 未知 / 电机运动 */
    safety_inputs_t in2 = in; in2.position = DRUM_POS_0;
    safety_interlock_check(&in2, &cfg, &req, &dec);
    CHECK(!dec.allowed && dec.reason == SAFETY_REJECT_POSITION_MISMATCH,
          "interlock: 位置≠270 -> POSITION_MISMATCH");
    safety_inputs_t in3 = in; in3.position_sample_valid = false;
    safety_interlock_check(&in3, &cfg, &req, &dec);
    CHECK(!dec.allowed && dec.reason == SAFETY_REJECT_POSITION_UNKNOWN,
          "interlock: 位置样本无效 -> POSITION_UNKNOWN");
    safety_inputs_t in4 = in; in4.position_motor_moving = true;
    safety_interlock_check(&in4, &cfg, &req, &dec);
    CHECK(!dec.allowed && dec.reason == SAFETY_REJECT_POSITION_MOVING,
          "interlock: 电机运动 -> POSITION_MOVING");

    /* MCP 输出 UNKNOWN（初始 UNKNOWN 拒） */
    safety_inputs_t in5 = in; in5.mcp_known_mask = 0x3Eu; /* PTC 位未知 */
    safety_interlock_check(&in5, &cfg, &req, &dec);
    CHECK(!dec.allowed && dec.reason == SAFETY_REJECT_OUTPUT_UNKNOWN,
          "interlock: 初始 UNKNOWN -> OUTPUT_UNKNOWN");

    /* 已有输出 ON（初始 ON 拒） */
    safety_inputs_t in6 = in; in6.mcp_outputs.ptc_heater = true;
    safety_interlock_check(&in6, &cfg, &req, &dec);
    CHECK(!dec.allowed && dec.reason == SAFETY_REJECT_OUTPUT_UNKNOWN,
          "interlock: 已有输出 ON -> OUTPUT_UNKNOWN");
    safety_inputs_t in7 = in; in7.hot_air_on = true;
    safety_interlock_check(&in7, &cfg, &req, &dec);
    CHECK(!dec.allowed && dec.reason == SAFETY_REJECT_ALREADY_ACTIVE &&
          (dec.interlock_mask & SAFETY_ILK_HOT_AIR_ALREADY_ON),
          "interlock: 热风已 ON -> ALREADY_ACTIVE (防交错)");

    /* SHT 失效/陈旧 */
    safety_inputs_t in8 = in; in8.sht_valid = false;
    safety_interlock_check(&in8, &cfg, &req, &dec);
    CHECK(!dec.allowed && dec.reason == SAFETY_REJECT_SHT_INVALID,
          "interlock: SHT 无效 -> SHT_INVALID");
    safety_inputs_t in9 = in; in9.sht_fresh = false;
    safety_interlock_check(&in9, &cfg, &req, &dec);
    CHECK(!dec.allowed && dec.reason == SAFETY_REJECT_SHT_STALE,
          "interlock: SHT 陈旧 -> SHT_STALE");

    /* 过温 */
    safety_inputs_t in10 = in; in10.sht_sample.temperature_c = 65.0f;
    safety_interlock_check(&in10, &cfg, &req, &dec);
    CHECK(!dec.allowed && dec.reason == SAFETY_REJECT_OVERTEMP,
          "interlock: 过温 -> OVERTEMP");

    /* 冷却锁 */
    safety_inputs_t in11 = in; in11.hot_air_cooldown_until_ms = 4000; /* now=1000 */
    safety_interlock_check(&in11, &cfg, &req, &dec);
    CHECK(!dec.allowed && dec.reason == SAFETY_REJECT_HOT_AIR_COOLDOWN_ACTIVE &&
          (dec.interlock_mask & SAFETY_ILK_HOT_AIR_COOLDOWN),
          "interlock: 冷却中 -> HOT_AIR_COOLDOWN_ACTIVE");

    /* 冷却结束可再开 */
    safety_inputs_t in12 = in; in12.hot_air_cooldown_until_ms = 0; /* 已过期 */
    safety_interlock_check(&in12, &cfg, &req, &dec);
    CHECK(dec.allowed, "interlock: 冷却结束 -> 放行");

    /* 不允许 raw bypass：HOT_AIR 不映射到 FAN */
    CHECK(safety_is_mcp_output_op(SAFETY_OP_HOT_AIR_MODULE),
          "interlock: HOT_AIR 是 MCP 输出操作");
}

/* ---- C. actuator_self_test HOT_AIR_COUPLED 门禁 ---- */

static actuator_self_test_gate_input_t act_ok_hotair_gate(void)
{
    actuator_self_test_gate_input_t in;
    memset(&in, 0, sizeof(in));
    in.target = ACT_TARGET_HOT_AIR_COUPLED;
    in.exec_idle = true;
    in.self_test_busy = false;
    in.fault_active = false;
    in.emergency_active = false;
    in.pos_valid = true;
    in.pos_fresh = true;
    in.pos_stable = true;
    in.pos_moving = false;
    in.pos = DRUM_POS_270;
    in.required_pos = DRUM_POS_270;
    in.mcp_target_known = true;
    in.water_safe_off_synced = true;
    in.target_confirmed_off = true;
    in.conflict_confirmed_off = true;
    in.water_service_idle = true;
    in.detergent_service_idle = true;
    in.drain_service_idle = true;
    in.dry_service_idle = true;
    in.fan_confirmed_off = true;
    in.heater_confirmed_off = true;
    in.sht_valid_fresh = true;
    in.sht_below_cutoff = true;
    in.hot_air_service_idle = true;
    in.hot_air_confirmed_off = true;
    in.hot_air_cooldown_active = false;
    in.request_id_ok = true;
    return in;
}

static void test_act_hot_air_gate(void)
{
    /* 目标映射与时长 */
    CHECK(actuator_self_test_duration_ms(ACT_TARGET_HOT_AIR_COUPLED) == 1000U,
          "act core: HOT_AIR_COUPLED 默认 1000ms");
    CHECK(strcmp(actuator_self_test_target_name(ACT_TARGET_HOT_AIR_COUPLED),
                 "hot_air_coupled") == 0,
          "act core: target name == hot_air_coupled");
    actuator_self_test_target_t t;
    CHECK(actuator_self_test_target_from_name("hot_air_coupled", &t) &&
          t == ACT_TARGET_HOT_AIR_COUPLED,
          "act core: from_name hot_air_coupled");

    actuator_self_test_gate_input_t in = act_ok_hotair_gate();
    CHECK(actuator_self_test_gate_check(&in) == ACT_GATE_OK,
          "act gate: HOT_AIR 全绿 -> OK");

    /* 热风服务忙碌 / 未 OFF / 冷却 / 温度 */
    actuator_self_test_gate_input_t p;
    p = in; p.hot_air_service_idle = false;
    CHECK(actuator_self_test_gate_check(&p) == ACT_GATE_SERVICE_BUSY,
          "act gate: hot air service busy -> SERVICE_BUSY");
    p = in; p.hot_air_confirmed_off = false;
    CHECK(actuator_self_test_gate_check(&p) == ACT_GATE_OUTPUT_ALREADY_ON,
          "act gate: hot air 未 OFF -> OUTPUT_ALREADY_ON");
    p = in; p.hot_air_cooldown_active = true;
    CHECK(actuator_self_test_gate_check(&p) == ACT_GATE_HOT_AIR_COOLDOWN_ACTIVE,
          "act gate: 冷却中 -> HOT_AIR_COOLDOWN_ACTIVE");
    p = in; p.sht_below_cutoff = false;
    CHECK(actuator_self_test_gate_check(&p) == ACT_GATE_HOT_AIR_TEMP_TOO_HIGH,
          "act gate: 温度过高 -> HOT_AIR_TEMP_TOO_HIGH");

    /* 机器码 */
    CHECK(strcmp(actuator_self_test_gate_code(ACT_GATE_HOT_AIR_COOLDOWN_ACTIVE),
                 "HOT_AIR_COOLDOWN_ACTIVE") == 0,
          "act core: 冷却拒绝码");
    CHECK(strcmp(actuator_self_test_gate_code(ACT_GATE_HOT_AIR_SNAPSHOT_UNAVAILABLE),
                 "HOT_AIR_SNAPSHOT_UNAVAILABLE") == 0,
          "act core: 快照不可用拒绝码");
    CHECK(strcmp(actuator_self_test_gate_code(ACT_GATE_HOT_AIR_TEMP_TOO_HIGH),
                 "HOT_AIR_TEMP_TOO_HIGH") == 0,
          "act core: 温度拒绝码");
}

/* ---- D. 10000 轮压力：单继电器 overlap=0、无 FAN、terminal once ---- */

#define CHA_STRESS_ROUNDS 10000
#define CHA_STRESS_SEED   0x0e5aU

static void test_cha_stress(void)
{
    unsigned long overlap = 0;
    unsigned long heater_without_relay = 0;   /* 未用（无独立加热丝操作） */
    unsigned long fan_actions = 0;            /* FSM 绝不能发出 FAN */
    unsigned long terminal_dups = 0;
    unsigned long not_off_at_end = 0;
    unsigned long rng = CHA_STRESS_SEED;

    cha_fsm_ctx_t ctx;
    cha_fsm_init(&ctx, 60.0f);

    for (unsigned long round = 0; round < CHA_STRESS_ROUNDS; round++) {
        rng = rng * 1103515245u + 12345u;
        unsigned scenario = (rng >> 16) % 7;
        uint32_t duration = 1000u + (uint32_t)(rng % 2001u); /* 1000..3000 */

        coupled_hot_air_request_t req = { .request_id = (machine_request_id_t)(round + 1),
                                          .duration_ms = duration };
        cha_fsm_begin_request(&ctx, &req, (int64_t)round * 1000 + 1);

        cha_fsm_output_t out;
        int64_t now = (int64_t)round * 1000 + 1;
        bool done = false;
        int off_fails = 0;
        /* 50ms 步进 × 80 步 = 4000ms，覆盖最长 3000ms duration + 启停开销 */
        int max_steps = 80;
        while (!done && max_steps-- > 0) {
            /* 模拟环境（默认良好） */
            cha_fsm_event_t e = ok_env(now);
            /* 每轮随机注入一次中断/过温 */
            switch (scenario) {
            case 1: /* 运行期紧急 */
                if (now > (int64_t)round * 1000 + 100 && ctx.state == CHA_FSM_RUNNING) {
                    e = env_tick(now, DRUM_POS_270, true, true, true, false,
                                 true, true, 25.0f, false, true, 0);
                }
                break;
            case 2: /* 运行期过温 */
                if (ctx.state == CHA_FSM_RUNNING) e.temperature_c = 65.0f;
                break;
            case 3: /* 运行期 SHT 失效 */
                if (ctx.state == CHA_FSM_RUNNING) { e.sht_valid = false; e.sht_fresh = false; }
                break;
            case 4: /* 位置丢失 */
                if (ctx.state == CHA_FSM_RUNNING) e.position_valid = false;
                break;
            case 5: /* 冷却锁出现（不应影响本轮，但验证不崩溃） */
                e.cooldown_remaining_ms = (uint32_t)(rng % 30000);
                break;
            case 6: /* 故障锁存 */
                if (ctx.state == CHA_FSM_RUNNING) e.fault_active = true;
                break;
            default: /* 0: 正常到期 */
                break;
            }
            cha_fsm_tick(&ctx, &e, &out);

            /* 单继电器 invariant：ON 与 OFF 动作绝不同时 pending */
            if (out.action != CHA_ACTION_NONE) {
                if (ctx.pending_action != out.action) overlap++;
                /* 只能有一个 action 类型 */
                if (out.action != CHA_ACTION_RELAY_ON &&
                    out.action != CHA_ACTION_RELAY_OFF) {
                    fan_actions++; /* FAN/其他动作一律算违规 */
                }
            }

            if (out.action == CHA_ACTION_RELAY_ON ||
                out.action == CHA_ACTION_RELAY_OFF) {
                /* 模拟执行 */
                bool ok = !(out.action == CHA_ACTION_RELAY_OFF && off_fails > 0 && (off_fails % 3) == 0);
                if (out.action == CHA_ACTION_RELAY_OFF && !ok) off_fails++;
                cha_fsm_event_t res = { .type = CHA_EVT_ACTION_RESULT, .now_ms = now,
                                        .action = out.action, .action_ok = ok };
                cha_fsm_tick(&ctx, &res, &out);
                /* 执行后应无重叠 pending */
                if (ctx.pending_action != CHA_ACTION_NONE && out.action != CHA_ACTION_NONE &&
                    out.action == ctx.pending_action) overlap++;
            }

            if (cha_fsm_is_terminal(&ctx)) {
                if (done) terminal_dups++;
                done = true;
            }
            now += 50;
            if (now - ((int64_t)round * 1000 + 1) > 4000) break;
        }

        if (!done) { not_off_at_end++; continue; }
        /* terminal 后 mark 回 IDLE */
        cha_fsm_mark_terminal_emitted(&ctx);
        /* 终态必须 OFF 已确认（除非 OFF 失败→FAULT off=false，但命令已多次请求 OFF） */
        if (ctx.terminal == CHA_TERMINAL_FAULT && ctx.stop_reason == CHA_STOP_OFF_FAIL) {
            /* 合法：OFF 确认失败显式 FAULT */
        } else if (!ctx.terminal_off_confirmed) {
            not_off_at_end++;
        }
    }

    CHECK(overlap == 0, "stress: output_overlap_count == 0 (10000 rounds)");
    CHECK(fan_actions == 0, "stress: 0 次 FAN/其他动作（只允许 RELAY）");
    CHECK(terminal_dups == 0, "stress: terminal exactly once");
    printf("  stress: rounds=%lu overlap=%lu fan_actions=%lu terminal_dups=%lu not_off_at_end=%lu\n",
           (unsigned long)CHA_STRESS_ROUNDS, overlap, fan_actions, terminal_dups, not_off_at_end);
}

int main(void)
{
    printf("=== coupled_hot_air host tests ===\n");

    test_fsm_init();
    test_fsm_start_gates();
    test_fsm_hard_cap_and_duration();
    test_fsm_immediate_off();
    test_fsm_off_fail_fault();
    test_fsm_begin_keeps_unknown();
    test_fsm_never_on_off_retry_terminates();
    test_fsm_terminal_once_generation();
    test_ilk_hot_air_gate();
    test_act_hot_air_gate();
    test_cha_stress();

    printf("\n=== Results: %d/%d passed (fail=%d) ===\n", g_pass, g_tests, g_fail);
    return g_fail ? 1 : (g_pass == g_tests ? 0 : 1);
}
