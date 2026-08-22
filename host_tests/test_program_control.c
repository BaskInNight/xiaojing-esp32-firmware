/*
 * test_program_control.c — program_control 纯 C 模型 host 测试
 *
 * 编译真实生产源 program_control_core.c（recipe/仲裁/BTN1/手势/审计），
 * 验证 DEFAULT_DEMO_V1 26 阶段规范、编译后位置可达性、优先级仲裁、
 * BTN1 互斥、手势冷却/重武装/尾串抑制、输出审计 fail-closed。
 */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "program_control_core.h"

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("  TEST %02d: %-60s ", tests_run, name); \
} while (0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while (0)
#define FAIL(msg) do { tests_failed++; printf("FAIL: %s\n", msg); } while (0)

static void expect_true(int cond, const char *msg)
{
    if (cond) PASS(); else FAIL(msg);
}

/* ================================================================
 * DEFAULT_DEMO_V1 规范阶段表
 * ================================================================ */

static void test_phase_table_shape(void)
{
    TEST("phase table count == 26");
    expect_true(DEFAULT_DEMO_PHASE_COUNT == 26, "count != 26");

    const default_demo_phase_t *ph = default_demo_phase_table();
    TEST("phase ids 1..26 in order");
    int ok = 1;
    for (uint8_t i = 0; i < DEFAULT_DEMO_PHASE_COUNT; i++) {
        if (ph[i].phase != i + 1) { ok = 0; break; }
    }
    expect_true(ok, "phase id sequence broken");

    TEST("first phase is order acceptance (WAIT_LOAD_CONFIRM)");
    expect_true(ph[0].step_type == STEP_WAIT_LOAD_CONFIRM, "not WAIT_LOAD");

    TEST("last phase is COMPLETE (FINISH)");
    expect_true(ph[DEFAULT_DEMO_PHASE_COUNT - 1].step_type == STEP_FINISH,
                "not FINISH");
}

static void test_phase_table_step_sequence(void)
{
    static const wash_step_type_t expected[DEFAULT_DEMO_PHASE_COUNT] = {
        STEP_WAIT_LOAD_CONFIRM,   /* 1  order */
        STEP_UV,                  /* 2  UV */
        STEP_HOME_BL50,           /* 3  precheck */
        STEP_MOVE_POSITION,       /* 4  position 0 */
        STEP_WATER_IN,            /* 5  water */
        STEP_DETERGENT,           /* 6  detergent */
        STEP_PULSATOR_WASH,       /* 7  prewash */
        STEP_SETTLE,              /* 8  settle */
        STEP_MOVE_POSITION,       /* 9  position 90 */
        STEP_DRUM_WASH,           /* 10 drum */
        STEP_SETTLE,              /* 11 settle */
        STEP_MOVE_POSITION,       /* 12 position 180 */
        STEP_DRAIN,               /* 13 drain */
        STEP_SPIN,                /* 14 spin ramp */
        STEP_SPIN,                /* 15 spin hold */
        STEP_SPIN,                /* 16 spin ramp_down */
        STEP_SETTLE,              /* 17 settle */
        STEP_MOVE_POSITION,       /* 18 position 270 */
        STEP_DRY,                 /* 19 hot air */
        STEP_DRY,                 /* 20 cooling */
        STEP_SETTLE,              /* 21 settle */
        STEP_MOVE_POSITION,       /* 22 position 45 */
        STEP_AUDIT_UV_OFF,        /* 23 uv off audit */
        STEP_AUDIT_SAFE_OFF,      /* 24 safe-off audit */
        STEP_WAIT_UNLOAD_CONFIRM, /* 25 wait unload */
        STEP_FINISH,              /* 26 complete */
    };
    const default_demo_phase_t *ph = default_demo_phase_table();
    TEST("phase step_type sequence matches 26-step recipe");
    int ok = 1;
    for (uint8_t i = 0; i < DEFAULT_DEMO_PHASE_COUNT; i++) {
        if (ph[i].step_type != expected[i]) { ok = 0; break; }
    }
    expect_true(ok, "step sequence mismatch");
}

static void test_phase_table_position_gates(void)
{
    /* 服务步骤的位置门禁必须与生产契约一致。 */
    static const struct { wash_step_type_t type; drum_position_t pos; } gates[] = {
        { STEP_UV,            DRUM_POS_0 },
        { STEP_HOME_BL50,     DRUM_POS_45 },
        { STEP_WATER_IN,      DRUM_POS_0 },
        { STEP_DETERGENT,     DRUM_POS_0 },
        { STEP_PULSATOR_WASH, DRUM_POS_0 },
        { STEP_DRUM_WASH,     DRUM_POS_90 },
        { STEP_DRAIN,         DRUM_POS_180 },
        { STEP_SPIN,          DRUM_POS_180 },
        { STEP_DRY,           DRUM_POS_270 },
    };
    const default_demo_phase_t *ph = default_demo_phase_table();
    TEST("all position-gated phases carry correct required position");
    int ok = 1;
    for (uint8_t i = 0; i < DEFAULT_DEMO_PHASE_COUNT; i++) {
        for (size_t g = 0; g < sizeof(gates) / sizeof(gates[0]); g++) {
            if (ph[i].step_type == gates[g].type) {
                if (ph[i].pos != gates[g].pos) { ok = 0; }
                break;
            }
        }
    }
    expect_true(ok, "a phase gate position is wrong");

    TEST("hot-air phase carries HEATER_ON flag, cooling does not");
    int hot_ok = 0, cool_ok = 0;
    for (uint8_t i = 0; i < DEFAULT_DEMO_PHASE_COUNT; i++) {
        if (ph[i].phase == DEMO_PHASE_HOT_AIR)
            hot_ok = (ph[i].flags & WASH_STEP_FLAG_HEATER_ON) != 0;
        if (ph[i].phase == DEMO_PHASE_COOL_TUMBLE)
            cool_ok = (ph[i].flags & WASH_STEP_FLAG_HEATER_ON) == 0;
    }
    expect_true(hot_ok && cool_ok, "hot/cool HEATER_ON mismatch");
}

/* ================================================================
 * default_demo_build_program
 * ================================================================ */

static void test_build_program_ok(void)
{
    wash_program_t prog;
    memset(&prog, 0, sizeof(prog));
    planner_report_t rep = default_demo_build_program(
        NULL, 42, DEFAULT_DEMO_START_POSITION, &prog);

    TEST("build returns OK, program_id set, kind DEMO");
    expect_true(rep.result == PLAN_RESULT_OK &&
                prog.program_id == 42 && prog.kind == WASH_PROGRAM_DEMO,
                "result/id/kind");

    TEST("steps_inserted == 2 (MOVE@0 for UV, MOVE@45 for HOME), 28 total");
    expect_true(rep.steps_inserted == 2, "inserted != 2");
    expect_true(prog.step_count == 28, "step_count != 28");

    TEST("estimated_total_ms == 83000 (active phases only)");
    expect_true(prog.estimated_total_ms == 83000, "estimate mismatch");

    TEST("position trace invariant holds");
    expect_true(default_demo_validate_position_trace(
                    &prog, DEFAULT_DEMO_START_POSITION),
                "trace broken");
}

static void test_build_program_sequence(void)
{
    wash_program_t prog;
    memset(&prog, 0, sizeof(prog));
    default_demo_build_program(NULL, 1, DEFAULT_DEMO_START_POSITION, &prog);

    static const wash_step_type_t expected[] = {
        STEP_WAIT_LOAD_CONFIRM,
        STEP_MOVE_POSITION,      /* system: 0 */
        STEP_UV,
        STEP_MOVE_POSITION,      /* system: 45 */
        STEP_HOME_BL50,
        STEP_MOVE_POSITION,      /* phase 4: 0 */
        STEP_WATER_IN,
        STEP_DETERGENT,
        STEP_PULSATOR_WASH,
        STEP_SETTLE,
        STEP_MOVE_POSITION,      /* 90 */
        STEP_DRUM_WASH,
        STEP_SETTLE,
        STEP_MOVE_POSITION,      /* 180 */
        STEP_DRAIN,
        STEP_SPIN,               /* ramp */
        STEP_SPIN,               /* hold */
        STEP_SPIN,               /* ramp_down */
        STEP_SETTLE,
        STEP_MOVE_POSITION,      /* 270 */
        STEP_DRY,                /* hot */
        STEP_DRY,                /* cool */
        STEP_SETTLE,
        STEP_MOVE_POSITION,      /* 45 */
        STEP_AUDIT_UV_OFF,
        STEP_AUDIT_SAFE_OFF,
        STEP_WAIT_UNLOAD_CONFIRM,
        STEP_FINISH,
    };
    TEST("compiled step-type sequence exact");
    int ok = prog.step_count == sizeof(expected) / sizeof(expected[0]);
    if (ok) {
        for (size_t i = 0; i < prog.step_count; i++) {
            if (prog.steps[i].type != expected[i]) { ok = 0; break; }
        }
    }
    expect_true(ok, "sequence mismatch");

    TEST("system-inserted moves flagged SYSTEM_INSERTED, phases not");
    ok = 1;
    if (prog.steps[1].type == STEP_MOVE_POSITION &&
        !(prog.steps[1].flags & WASH_STEP_FLAG_SYSTEM_INSERTED)) ok = 0;
    if (prog.steps[3].type == STEP_MOVE_POSITION &&
        !(prog.steps[3].flags & WASH_STEP_FLAG_SYSTEM_INSERTED)) ok = 0;
    if (prog.steps[5].flags & WASH_STEP_FLAG_SYSTEM_INSERTED) ok = 0; /* phase 4 */
    expect_true(ok, "flag mismatch");

    TEST("system MOVE @0 precedes UV; MOVE @45 precedes HOME");
    ok = (prog.steps[1].required_position == DRUM_POS_0 &&
          prog.steps[2].type == STEP_UV &&
          prog.steps[3].required_position == DRUM_POS_45 &&
          prog.steps[4].type == STEP_HOME_BL50);
    expect_true(ok, "pre-move placement");

    TEST("hot dry has HEATER_ON, cool dry does not");
    ok = 0;
    for (size_t i = 0; i < prog.step_count; i++) {
        if (prog.steps[i].type == STEP_DRY) {
            ok = (prog.steps[i].flags & WASH_STEP_FLAG_HEATER_ON) != 0;
            break;
        }
    }
    int cool_ok = 0;
    for (size_t i = 0; i < prog.step_count; i++) {
        if (prog.steps[i].type == STEP_DRY) {
            if ((prog.steps[i].flags & WASH_STEP_FLAG_HEATER_ON) == 0)
                cool_ok = 1;
        }
    }
    expect_true(ok && cool_ok, "heater flag on dry steps");
}

static void test_build_program_variants(void)
{
    wash_program_t prog;
    memset(&prog, 0, sizeof(prog));
    planner_report_t rep = default_demo_build_program(
        NULL, 2, DRUM_POS_0, &prog);
    TEST("initial=0 -> 1 system move (only MOVE@45 before HOME), 27 total");
    expect_true(rep.steps_inserted == 1 && prog.step_count == 27,
                "inserted/total wrong");
    expect_true(default_demo_validate_position_trace(&prog, DRUM_POS_0),
                "trace broken for initial=0");

    memset(&prog, 0, sizeof(prog));
    rep = default_demo_build_program(NULL, 3, DRUM_POS_UNKNOWN, &prog);
    TEST("initial=UNKNOWN -> UV needs MOVE@0, HOME needs MOVE@45 (2 moves)");
    expect_true(rep.steps_inserted == 2, "unknown-start moves != 2");
    expect_true(default_demo_validate_position_trace(&prog, DRUM_POS_UNKNOWN),
                "trace broken for unknown");

    TEST("NULL out_program -> REJECTED INVALID_ARG");
    planner_report_t r2 = default_demo_build_program(NULL, 4, DRUM_POS_45, NULL);
    expect_true(r2.result == PLAN_RESULT_REJECTED &&
                r2.reject_reason == PLAN_ERROR_INVALID_ARG,
                "null out not rejected");
}

static void test_position_trace_negative(void)
{
    /* 手工构造一个 SPIN@180 但之前没有移动到 180 的程序 → 不变量必须失败 */
    wash_program_t prog;
    memset(&prog, 0, sizeof(prog));
    prog.program_id = 9;
    prog.kind = WASH_PROGRAM_CUSTOM;
    prog.steps[prog.step_count].type = STEP_MOVE_POSITION;
    prog.steps[prog.step_count].required_position = DRUM_POS_0;
    prog.steps[prog.step_count].step_id = (uint16_t)prog.step_count;
    prog.step_count++;
    prog.steps[prog.step_count].type = STEP_SPIN;
    prog.steps[prog.step_count].required_position = DRUM_POS_180; /* not reached */
    prog.steps[prog.step_count].step_id = (uint16_t)prog.step_count;
    prog.step_count++;

    TEST("position trace rejects a gated step at unreachable position");
    expect_true(!default_demo_validate_position_trace(&prog, DRUM_POS_0),
                "should have failed");

    /* MOVE 到 180 后再 SPIN → 通过 */
    memset(&prog, 0, sizeof(prog));
    prog.steps[prog.step_count].type = STEP_MOVE_POSITION;
    prog.steps[prog.step_count].required_position = DRUM_POS_180;
    prog.steps[prog.step_count].step_id = 0;
    prog.step_count++;
    prog.steps[prog.step_count].type = STEP_SPIN;
    prog.steps[prog.step_count].required_position = DRUM_POS_180;
    prog.steps[prog.step_count].step_id = 1;
    prog.step_count++;
    TEST("position trace accepts MOVE then gated step at same position");
    expect_true(default_demo_validate_position_trace(&prog, DRUM_POS_45),
                "should have passed");

    TEST("NULL program -> trace false");
    expect_true(!default_demo_validate_position_trace(NULL, DRUM_POS_45),
                "null program not false");
}

/* ================================================================
 * 输出审计
 * ================================================================ */

static void test_audit_safe_off_ok(void)
{
    pc_output_audit_t a;
    memset(&a, 0, sizeof(a));
    a.uv = 1; a.heater = 1; a.fan = 1; a.tap_valve = 1;
    a.transfer_valve = 1; a.detergent_pump = 1; a.drain_valve = 1;
    TEST("safe-off: all confirmed OFF + motors idle -> true");
    expect_true(pc_audit_safe_off(&a), "should pass");

    TEST("uv-off: uv confirmed OFF -> true");
    expect_true(pc_audit_uv_off(&a), "uv should pass");
}

static void test_audit_fail_closed(void)
{
    pc_output_audit_t base;
    memset(&base, 0, sizeof(base));
    base.uv = 1; base.heater = 1; base.fan = 1; base.tap_valve = 1;
    base.transfer_valve = 1; base.detergent_pump = 1; base.drain_valve = 1;

    TEST("safe-off: any UNKNOWN (0) -> false (never treat UNKNOWN as OFF)");
    pc_output_audit_t a = base; a.uv = 0;
    expect_true(!pc_audit_safe_off(&a), "UNKNOWN uv passed");
    a = base; a.fan = 0;
    expect_true(!pc_audit_safe_off(&a), "UNKNOWN fan passed");
    a = base; a.drain_valve = 0;
    expect_true(!pc_audit_safe_off(&a), "UNKNOWN drain passed");

    TEST("safe-off: any ON (2) -> false");
    a = base; a.heater = 2;
    expect_true(!pc_audit_safe_off(&a), "ON heater passed");
    a = base; a.tap_valve = 2;
    expect_true(!pc_audit_safe_off(&a), "ON valve passed");

    TEST("safe-off: bl50 busy -> false");
    a = base; a.bl50_busy = true;
    expect_true(!pc_audit_safe_off(&a), "busy bl50 passed");

    TEST("safe-off: position moving -> false");
    a = base; a.position_moving = true;
    expect_true(!pc_audit_safe_off(&a), "moving position passed");

    TEST("uv-off: UNKNOWN / ON -> false");
    a = base; a.uv = 0;
    expect_true(!pc_audit_uv_off(&a), "UNKNOWN uv passed");
    a = base; a.uv = 2;
    expect_true(!pc_audit_uv_off(&a), "ON uv passed");

    TEST("audits: NULL pointer -> false");
    expect_true(!pc_audit_safe_off(NULL) && !pc_audit_uv_off(NULL),
                "NULL passed");
}

/* ================================================================
 * 仲裁
 * ================================================================ */

static void test_arbiter_priority(void)
{
    pc_arbiter_t a;
    memset(&a, 0, sizeof(a));

    TEST("START from IDLE -> START_DEMO, latch STARTING");
    expect_true(pc_arbitrate(&a, PC_REQ_START_DEMO) == PC_ACTION_START_DEMO &&
                a.state == PC_STATE_STARTING, "start");

    TEST("START dedupe while STARTING -> NONE");
    expect_true(pc_arbitrate(&a, PC_REQ_START_DEMO) == PC_ACTION_NONE,
                "dup start");

    /* 程序激活 */
    a.program_active = true;
    a.state = PC_STATE_RUNNING;
    TEST("START while running -> NONE");
    expect_true(pc_arbitrate(&a, PC_REQ_START_DEMO) == PC_ACTION_NONE,
                "start while running");

    TEST("STOP while running -> STOP_PROGRAM, latch STOPPING");
    expect_true(pc_arbitrate(&a, PC_REQ_STOP) == PC_ACTION_STOP_PROGRAM &&
                a.state == PC_STATE_STOPPING, "stop");

    TEST("STOP dedupe while STOPPING -> NONE");
    expect_true(pc_arbitrate(&a, PC_REQ_STOP) == PC_ACTION_NONE, "dup stop");

    TEST("START during STOPPING -> NONE (stop wins)");
    expect_true(pc_arbitrate(&a, PC_REQ_START_DEMO) == PC_ACTION_NONE,
                "start during stop");

    /* 程序结束后 STOPPING 塌缩回 IDLE */
    a.program_active = false;
    pc_arbiter_refresh(&a);
    TEST("refresh collapses STOPPING -> IDLE when program inactive");
    expect_true(a.state == PC_STATE_IDLE, "not idle after refresh");

    TEST("STOP while IDLE -> NONE");
    expect_true(pc_arbitrate(&a, PC_REQ_STOP) == PC_ACTION_NONE, "stop idle");

    /* ABORT */
    memset(&a, 0, sizeof(a));
    a.program_active = true;
    a.state = PC_STATE_RUNNING;
    TEST("ABORT while running -> ABORT_PROGRAM, latch STOPPING");
    expect_true(pc_arbitrate(&a, PC_REQ_ABORT) == PC_ACTION_ABORT_PROGRAM &&
                a.state == PC_STATE_STOPPING, "abort");

    TEST("ABORT dedupe while STOPPING -> NONE");
    expect_true(pc_arbitrate(&a, PC_REQ_ABORT) == PC_ACTION_NONE, "dup abort");

    TEST("ABORT while IDLE -> NONE");
    a.program_active = false;
    pc_arbiter_refresh(&a);
    expect_true(pc_arbitrate(&a, PC_REQ_ABORT) == PC_ACTION_NONE, "abort idle");
}

static void test_arbiter_emergency(void)
{
    pc_arbiter_t a;
    memset(&a, 0, sizeof(a));
    a.program_active = true;
    a.state = PC_STATE_RUNNING;

    TEST("EMERGENCY -> EMERGENCY_STOP, latched, state EMERGENCY");
    expect_true(pc_arbitrate(&a, PC_REQ_EMERGENCY) == PC_ACTION_EMERGENCY_STOP &&
                a.emergency_latched && a.state == PC_STATE_EMERGENCY,
                "emergency");

    TEST("after emergency: START / STOP -> NONE");
    expect_true(pc_arbitrate(&a, PC_REQ_START_DEMO) == PC_ACTION_NONE &&
                pc_arbitrate(&a, PC_REQ_STOP) == PC_ACTION_NONE,
                "blocked");

    TEST("after emergency: BACKEND -> NONE");
    expect_true(pc_arbitrate(&a, PC_REQ_BACKEND_SWITCH) == PC_ACTION_NONE,
                "backend blocked");

    TEST("refresh keeps EMERGENCY latched");
    pc_arbiter_refresh(&a);
    expect_true(a.state == PC_STATE_EMERGENCY, "emergency dropped");
}

static void test_arbiter_fault_busy(void)
{
    pc_arbiter_t a;
    memset(&a, 0, sizeof(a));

    TEST("external_busy (diagnostics hold executor) blocks START");
    a.external_busy = true;
    expect_true(pc_arbitrate(&a, PC_REQ_START_DEMO) == PC_ACTION_NONE,
                "start while busy");

    a.external_busy = false;
    a.fault_active = true;
    TEST("fault blocks START / STOP / BACKEND");
    expect_true(pc_arbitrate(&a, PC_REQ_START_DEMO) == PC_ACTION_NONE &&
                pc_arbitrate(&a, PC_REQ_STOP) == PC_ACTION_NONE &&
                pc_arbitrate(&a, PC_REQ_BACKEND_SWITCH) == PC_ACTION_NONE,
                "fault not blocking");

    TEST("refresh -> FAULT state");
    pc_arbiter_refresh(&a);
    expect_true(a.state == PC_STATE_FAULT, "not fault state");

    a.fault_active = false;
    a.program_active = true;
    pc_arbiter_refresh(&a);
    TEST("refresh -> RUNNING when program active");
    expect_true(a.state == PC_STATE_RUNNING, "not running");
}

/* ================================================================
 * BTN1
 * ================================================================ */

static void test_btn1(void)
{
    TEST("BTN1 CLICK -> START");
    expect_true(pc_btn1_decide(PC_BTN1_EV_CLICK, false) == PC_BTN1_ACTION_START,
                "click");

    TEST("BTN1 LONG_PRESS + voice -> BACKEND");
    expect_true(pc_btn1_decide(PC_BTN1_EV_LONG_PRESS, true) ==
                    PC_BTN1_ACTION_BACKEND, "long voice");

    TEST("BTN1 LONG_PRESS without voice -> NONE (mutual exclusion)");
    expect_true(pc_btn1_decide(PC_BTN1_EV_LONG_PRESS, false) ==
                    PC_BTN1_ACTION_NONE, "long no voice");

    TEST("BTN1 LONG_RELEASE / NONE -> NONE (no spurious START)");
    expect_true(pc_btn1_decide(PC_BTN1_EV_LONG_RELEASE, true) ==
                    PC_BTN1_ACTION_NONE, "long release");
    expect_true(pc_btn1_decide(PC_BTN1_EV_NONE, true) == PC_BTN1_ACTION_NONE,
                "none");
}

/* ================================================================
 * 手势
 * ================================================================ */

static void test_gesture_start_stop(void)
{
    pc_gesture_state_t s;
    memset(&s, 0, sizeof(s));
    s.armed = true;   /* 服务 init 后首手势可触发 */

    TEST("UP (armed) -> START");
    expect_true(pc_gesture_step(&s, HAL_GESTURE_UP, 1000, 1000) ==
                    PC_GESTURE_ACTION_START, "up");

    TEST("immediate repeat UP -> NONE (tail suppressed, disarmed)");
    expect_true(pc_gesture_step(&s, HAL_GESTURE_UP, 1010, 1000) ==
                    PC_GESTURE_ACTION_NONE, "tail");

    TEST("during cooldown DOWN -> NONE");
    expect_true(pc_gesture_step(&s, HAL_GESTURE_DOWN, 1500, 1000) ==
                    PC_GESTURE_ACTION_NONE, "cooldown");

    /* 冷却过后，先见 NONE 重武装，再 DOWN */
    TEST("NONE after cooldown re-arms; DOWN -> STOP");
    expect_true(pc_gesture_step(&s, HAL_GESTURE_NONE, 3000, 1000) ==
                    PC_GESTURE_ACTION_NONE, "none");
    expect_true(s.armed, "not re-armed");
    expect_true(pc_gesture_step(&s, HAL_GESTURE_DOWN, 3100, 1000) ==
                    PC_GESTURE_ACTION_STOP, "down");
}

static pc_gesture_state_t fresh_armed_gesture(void)
{
    pc_gesture_state_t s;
    memset(&s, 0, sizeof(s));
    s.armed = true;
    return s;
}

static void test_gesture_paging_and_others(void)
{
    pc_gesture_state_t s = fresh_armed_gesture();

    TEST("LEFT / BACKWARD -> PAGE_PREV");
    expect_true(pc_gesture_step(&s, HAL_GESTURE_LEFT, 100, 1000) ==
                    PC_GESTURE_ACTION_PAGE_PREV, "left");
    s = fresh_armed_gesture();
    expect_true(pc_gesture_step(&s, HAL_GESTURE_BACKWARD, 200, 1000) ==
                    PC_GESTURE_ACTION_PAGE_PREV, "backward");

    s = fresh_armed_gesture();
    TEST("RIGHT / FORWARD -> PAGE_NEXT");
    expect_true(pc_gesture_step(&s, HAL_GESTURE_RIGHT, 300, 1000) ==
                    PC_GESTURE_ACTION_PAGE_NEXT, "right");
    s = fresh_armed_gesture();
    expect_true(pc_gesture_step(&s, HAL_GESTURE_FORWARD, 400, 1000) ==
                    PC_GESTURE_ACTION_PAGE_NEXT, "forward");

    s = fresh_armed_gesture();
    TEST("CLOCKWISE / COUNTER_CW -> NONE");
    expect_true(pc_gesture_step(&s, HAL_GESTURE_CLOCKWISE, 500, 1000) ==
                    PC_GESTURE_ACTION_NONE, "cw");
    s = fresh_armed_gesture();
    expect_true(pc_gesture_step(&s, HAL_GESTURE_COUNTER_CW, 600, 1000) ==
                    PC_GESTURE_ACTION_NONE, "ccw");

    TEST("disarmed at boot: first gesture suppressed until re-armed");
    pc_gesture_state_t s2;
    memset(&s2, 0, sizeof(s2));   /* armed=false */
    expect_true(pc_gesture_step(&s2, HAL_GESTURE_UP, 0, 1000) ==
                    PC_GESTURE_ACTION_NONE, "boot first gesture");
    expect_true(pc_gesture_step(&s2, HAL_GESTURE_NONE, 2000, 1000) ==
                    PC_GESTURE_ACTION_NONE, "re-arm none");
    expect_true(pc_gesture_step(&s2, HAL_GESTURE_UP, 2100, 1000) ==
                    PC_GESTURE_ACTION_START, "post re-arm up");

    TEST("NULL state -> NONE");
    expect_true(pc_gesture_step(NULL, HAL_GESTURE_UP, 0, 1000) ==
                    PC_GESTURE_ACTION_NONE, "null");
}

/* ================================================================
 * 确定性压力测试（1000+ 轮）
 * ================================================================ */

static uint32_t g_seed = 0x12345678u;

static uint32_t lcg_next(void)
{
    /* Deterministic LCG so the stress is reproducible across runs. */
    g_seed = g_seed * 1664525u + 1013904223u;
    return g_seed;
}

static void test_arbiter_stress(void)
{
    const uint32_t rounds = 5000;
    unsigned invalid_state = 0, invalid_emergency = 0;
    pc_arbiter_t a;
    memset(&a, 0, sizeof(a));

    for (uint32_t i = 0; i < rounds; i++) {
        /* Mutate external truth occasionally */
        if ((i % 137) == 0) a.program_active = !a.program_active;
        if ((i % 211) == 0) a.fault_active = !a.fault_active;
        if ((i % 199) == 0) a.external_busy = !a.external_busy;

        pc_request_t req = (pc_request_t)(lcg_next() % 5);
        pc_action_t action = pc_arbitrate(&a, req);

        if (a.state > PC_STATE_EMERGENCY) invalid_state++;
        /* 急停闩锁后：非 EMERGENCY 请求必须一律返回 NONE（闩锁当次请求
         * 本身返回 EMERGENCY_STOP，不计入）。 */
        if (req != PC_REQ_EMERGENCY && a.emergency_latched &&
            action != PC_ACTION_NONE) invalid_emergency++;
        if (req != PC_REQ_EMERGENCY && a.emergency_latched &&
            a.state != PC_STATE_EMERGENCY) invalid_emergency++;

        /* Refresh at intervals */
        if ((i % 29) == 0) pc_arbiter_refresh(&a);
        if (a.state > PC_STATE_EMERGENCY) invalid_state++;
    }

    TEST("arbiter 5000-round stress: no invalid state/emergency");
    expect_true(invalid_state == 0 && invalid_emergency == 0,
                "invariant broken");
}

static void test_gesture_stress(void)
{
    const uint32_t rounds = 5000;
    unsigned bad_action = 0, bad_armed = 0;
    pc_gesture_state_t s;
    memset(&s, 0, sizeof(s));
    s.armed = true;

    for (uint32_t i = 0; i < rounds; i++) {
        uint32_t r = lcg_next();
        hal_gesture_t g = (hal_gesture_t)(r % 9);   /* 0..8 = all HAL_GESTURE_* */
        uint32_t now = (uint32_t)(i * 17);           /* monotonic-ish, sparse */
        pc_gesture_action_t action = pc_gesture_step(&s, g, now, 100);

        switch (action) {
        case PC_GESTURE_ACTION_START:
        case PC_GESTURE_ACTION_STOP:
        case PC_GESTURE_ACTION_PAGE_PREV:
        case PC_GESTURE_ACTION_PAGE_NEXT:
            /* firing disarms immediately */
            if (s.armed) bad_armed++;
            break;
        default:
            break;
        }
        if (action < PC_GESTURE_ACTION_NONE || action > PC_GESTURE_ACTION_PAGE_NEXT)
            bad_action++;
    }

    TEST("gesture 5000-round stress: no invalid action / disarmed fire");
    expect_true(bad_action == 0 && bad_armed == 0, "invariant broken");
}

int main(void)
{
    printf("== program_control pure-core host tests ==\n");
    test_phase_table_shape();
    test_phase_table_step_sequence();
    test_phase_table_position_gates();
    test_build_program_ok();
    test_build_program_sequence();
    test_build_program_variants();
    test_position_trace_negative();
    test_audit_safe_off_ok();
    test_audit_fail_closed();
    test_arbiter_priority();
    test_arbiter_emergency();
    test_arbiter_fault_busy();
    test_btn1();
    test_gesture_start_stop();
    test_gesture_paging_and_others();
    test_arbiter_stress();
    test_gesture_stress();

    printf("\n%d/%d checks passed, %d failed\n",
           tests_passed, tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
