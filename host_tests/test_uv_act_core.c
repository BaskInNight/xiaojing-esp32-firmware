/*
 * test_uv_act_core.c — Host tests for the UV audit fixes and the actuator
 * board-test core:
 *   - explicit internal→public enum mapping (UV + detergent)
 *   - UV terminal semantics (CANCELED/SKIPPED/TIMEOUT never COMPLETE)
 *   - UV self-test gate: POSITION_STALE reachable (valid && !fresh)
 *   - actuator self-test gate matrix + fixed durations + target parsing
 * No FreeRTOS/HAL/LVGL dependency.
 */

#include <stdio.h>
#include <string.h>

#include "uv_fsm.h"
#include "detergent_fsm.h"
#include "uv_self_test_core.h"
#include "actuator_self_test_core.h"
#include "water_fsm.h"
#include "drain_fsm.h"
#include "dry_fsm.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("  TEST %02d: %-58s ", tests_run, name); \
} while (0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while (0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while (0)

/* ---- UV enum mapping ---- */

static void test_uv_enum_mapping(void)
{
    TEST("uv_fsm_state_to_public maps all 8 internal states");
    int ok = uv_fsm_state_to_public(UV_FSM_IDLE) == UV_STATE_IDLE &&
             uv_fsm_state_to_public(UV_FSM_VALIDATING) == UV_STATE_VALIDATING &&
             uv_fsm_state_to_public(UV_FSM_RUNNING) == UV_STATE_RUNNING &&
             uv_fsm_state_to_public(UV_FSM_STOPPING) == UV_STATE_STOPPING &&
             uv_fsm_state_to_public(UV_FSM_COMPLETE) == UV_STATE_COMPLETE &&
             uv_fsm_state_to_public(UV_FSM_CANCELING) == UV_STATE_CANCELING &&
             uv_fsm_state_to_public(UV_FSM_SKIPPING) == UV_STATE_SKIPPING &&
             uv_fsm_state_to_public(UV_FSM_FAULT) == UV_STATE_FAULT;
    if (ok) PASS(); else FAIL("mapping mismatch");

    TEST("uv public IDLE(1) != internal IDLE(0) (no reliance on equal values)");
    if (UV_STATE_IDLE != (uv_state_t)UV_FSM_IDLE) PASS();
    else FAIL("public/internal IDLE must differ");

    TEST("uv_fsm_state_to_public unknown value maps to safe FAULT");
    if (uv_fsm_state_to_public((uv_fsm_state_t)99) == UV_STATE_FAULT) PASS();
    else FAIL("unknown must map to FAULT, never IDLE");
}

static void test_detergent_enum_mapping(void)
{
    TEST("detergent_fsm_state_to_public maps all 7 internal states");
    int ok = detergent_fsm_state_to_public(DETERGENT_FSM_IDLE) ==
                 DETERGENT_STATE_IDLE &&
             detergent_fsm_state_to_public(DETERGENT_FSM_VALIDATING) ==
                 DETERGENT_STATE_VALIDATING &&
             detergent_fsm_state_to_public(DETERGENT_FSM_RUNNING) ==
                 DETERGENT_STATE_RUNNING &&
             detergent_fsm_state_to_public(DETERGENT_FSM_STOPPING) ==
                 DETERGENT_STATE_STOPPING &&
             detergent_fsm_state_to_public(DETERGENT_FSM_COMPLETE) ==
                 DETERGENT_STATE_COMPLETE &&
             detergent_fsm_state_to_public(DETERGENT_FSM_CANCELING) ==
                 DETERGENT_STATE_CANCELING &&
             detergent_fsm_state_to_public(DETERGENT_FSM_FAULT) ==
                 DETERGENT_STATE_FAULT;
    if (ok) PASS(); else FAIL("mapping mismatch");

    TEST("detergent_fsm_state_to_public unknown maps to safe FAULT");
    if (detergent_fsm_state_to_public((detergent_fsm_state_t)42) ==
        DETERGENT_STATE_FAULT) PASS();
    else FAIL("unknown must map to FAULT");
}

/* ---- UV terminal semantics ---- */

static void test_uv_terminal_semantics(void)
{
    TEST("UV_TERMINAL_COMPLETE -> COMPLETE");
    if (uv_self_test_map_terminal(UV_TERMINAL_COMPLETE) ==
        UV_STATE_SELFTEST_COMPLETE) PASS();
    else FAIL("complete");

    TEST("UV_TERMINAL_REJECTED -> REJECTED");
    if (uv_self_test_map_terminal(UV_TERMINAL_REJECTED) ==
        UV_STATE_SELFTEST_REJECTED) PASS();
    else FAIL("rejected");

    TEST("UV_TERMINAL_FAULT -> FAULT");
    if (uv_self_test_map_terminal(UV_TERMINAL_FAULT) ==
        UV_STATE_SELFTEST_FAULT) PASS();
    else FAIL("fault");

    TEST("UV_TERMINAL_CANCELED -> INTERRUPTED (never COMPLETE)");
    if (uv_self_test_map_terminal(UV_TERMINAL_CANCELED) ==
        UV_STATE_SELFTEST_INTERRUPTED) PASS();
    else FAIL("canceled");

    TEST("UV_TERMINAL_SKIPPED -> INTERRUPTED (never COMPLETE)");
    if (uv_self_test_map_terminal(UV_TERMINAL_SKIPPED) ==
        UV_STATE_SELFTEST_INTERRUPTED) PASS();
    else FAIL("skipped");

    TEST("UV_TERMINAL_TIMEOUT -> TIMEOUT (never COMPLETE)");
    if (uv_self_test_map_terminal(UV_TERMINAL_TIMEOUT) ==
        UV_STATE_SELFTEST_TIMEOUT) PASS();
    else FAIL("timeout");

    TEST("UV_TERMINAL_NONE -> UNKNOWN (never COMPLETE)");
    if (uv_self_test_map_terminal(UV_TERMINAL_NONE) ==
        UV_STATE_SELFTEST_UNKNOWN) PASS();
    else FAIL("none");
}

/* ---- UV self-test gate: stale reachable ---- */

static uv_self_test_gate_input_t ok_gate(void)
{
    uv_self_test_gate_input_t in;
    memset(&in, 0, sizeof(in));
    in.exec_idle = true;
    in.uv_idle = true;
    in.uv_confirmed_off = true;
    in.fault_active = false;
    in.emergency_active = false;
    in.mcp_uv_known = true;
    in.pos_valid = true;
    in.pos_fresh = true;
    in.pos_stable = true;
    in.pos_moving = false;
    in.pos = DRUM_POS_0;
    return in;
}

static void test_uv_stale_gate(void)
{
    TEST("UV gate OK when valid+fresh+stable+zero");
    uv_self_test_gate_input_t in = ok_gate();
    if (uv_self_test_gate_check(&in) == UV_ST_GATE_OK) PASS();
    else FAIL("expected OK");

    TEST("UV gate POSITION_STALE reachable: valid=true fresh=false");
    uv_self_test_gate_input_t stale = ok_gate();
    stale.pos_valid = true;
    stale.pos_fresh = false;
    if (uv_self_test_gate_check(&stale) == UV_ST_GATE_POSITION_STALE) PASS();
    else FAIL("stale must reject, not collapse to POSITION_UNKNOWN");

    TEST("UV gate POSITION_UNKNOWN when sample missing (valid=false)");
    uv_self_test_gate_input_t unknown = ok_gate();
    unknown.pos_valid = false;
    unknown.pos_fresh = true;
    if (uv_self_test_gate_check(&unknown) == UV_ST_GATE_POSITION_UNKNOWN) PASS();
    else FAIL("unknown");

    TEST("UV gate POSITION_UNSTABLE when not stable");
    uv_self_test_gate_input_t unstable = ok_gate();
    unstable.pos_stable = false;
    if (uv_self_test_gate_check(&unstable) == UV_ST_GATE_POSITION_UNSTABLE) PASS();
    else FAIL("unstable");
}

/* ---- Actuator self-test core ---- */

static void test_actuator_targets(void)
{
    TEST("actuator fixed durations: source 1500 / transfer 1500 / pump 800");
    if (actuator_self_test_duration_ms(ACT_TARGET_SOURCE_VALVE) == 1500U &&
        actuator_self_test_duration_ms(ACT_TARGET_TRANSFER_VALVE) == 1500U &&
        actuator_self_test_duration_ms(ACT_TARGET_DETERGENT_PUMP) == 800U)
        PASS();
    else FAIL("durations");

    TEST("actuator invalid target -> duration 0");
    if (actuator_self_test_duration_ms((actuator_self_test_target_t)99) == 0U)
        PASS();
    else FAIL("invalid duration");

    TEST("actuator target name round-trip");
    actuator_self_test_target_t t = ACT_TARGET_NONE;
    int ok = actuator_self_test_target_from_name("source_valve", &t) &&
             t == ACT_TARGET_SOURCE_VALVE &&
             strcmp(actuator_self_test_target_name(ACT_TARGET_SOURCE_VALVE),
                    "source_valve") == 0 &&
             actuator_self_test_target_from_name("transfer_valve", &t) &&
             t == ACT_TARGET_TRANSFER_VALVE &&
             actuator_self_test_target_from_name("detergent_pump", &t) &&
             t == ACT_TARGET_DETERGENT_PUMP;
    if (ok) PASS(); else FAIL("round-trip");

    TEST("actuator unknown target name rejected");
    actuator_self_test_target_t bad;
    if (!actuator_self_test_target_from_name("heater", &bad)) PASS();
    else FAIL("unknown target accepted");
}

static actuator_self_test_gate_input_t act_ok_gate(void)
{
    actuator_self_test_gate_input_t in;
    memset(&in, 0, sizeof(in));
    in.exec_idle = true;
    in.self_test_busy = false;
    in.fault_active = false;
    in.emergency_active = false;
    in.pos_valid = true;
    in.pos_fresh = true;
    in.pos_stable = true;
    in.pos_moving = false;
    in.pos = DRUM_POS_0;
    in.required_pos = DRUM_POS_0;
    in.mcp_target_known = true;
    in.water_safe_off_synced = true;   /* HW-FIX-1: 初始安全 OFF 同步已完成 */
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
    in.request_id_ok = true;
    return in;
}

static void test_actuator_gates(void)
{
    struct { const char *name; int set; actuator_self_test_gate_result_t want; } cases[] = {
        { "executor busy",          1, ACT_GATE_EXECUTOR_BUSY },
        { "self-test busy",         2, ACT_GATE_SELF_TEST_BUSY },
        { "fault active",           3, ACT_GATE_FAULT_ACTIVE },
        { "emergency active",       4, ACT_GATE_EMERGENCY_ACTIVE },
        { "pos unknown",            5, ACT_GATE_POSITION_UNKNOWN },
        { "pos stale",              6, ACT_GATE_POSITION_STALE },
        { "pos unstable",           7, ACT_GATE_POSITION_UNSTABLE },
        { "motor moving",           8, ACT_GATE_MOTOR_MOVING },
        { "pos not zero",           9, ACT_GATE_POSITION_NOT_ZERO },
        { "output unknown",        10, ACT_GATE_OUTPUT_STATE_UNKNOWN },
        { "water safe-off pending",15, ACT_GATE_OUTPUT_STATE_UNKNOWN },
        { "output already on",     11, ACT_GATE_OUTPUT_ALREADY_ON },
        { "conflict output on",    12, ACT_GATE_CONFLICT_OUTPUT_ON },
        { "service busy",          13, ACT_GATE_SERVICE_BUSY },
        { "request id invalid",    14, ACT_GATE_REQUEST_ID_INVALID },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        actuator_self_test_gate_input_t in = act_ok_gate();
        switch (cases[i].set) {
        case 1: in.exec_idle = false; break;
        case 2: in.self_test_busy = true; break;
        case 3: in.fault_active = true; break;
        case 4: in.emergency_active = true; break;
        case 5: in.pos_valid = false; break;
        case 6: in.pos_fresh = false; break;
        case 7: in.pos_stable = false; break;
        case 8: in.pos_moving = true; break;
        case 9: in.pos = DRUM_POS_180; break;
        case 10: in.mcp_target_known = false; break;
        case 11: in.target_confirmed_off = false; break;
        case 12: in.conflict_confirmed_off = false; break;
        case 13: in.water_service_idle = false; break;
        case 14: in.request_id_ok = false; break;
        case 15: in.water_safe_off_synced = false; break;
        }
        actuator_self_test_gate_result_t got = actuator_self_test_gate_check(&in);
        char buf[64];
        TEST(cases[i].name);
        if (got == cases[i].want) {
            PASS();
        } else {
            snprintf(buf, sizeof(buf), "got %s want %s",
                     actuator_self_test_gate_code(got),
                     actuator_self_test_gate_code(cases[i].want));
            FAIL(buf);
        }
    }

    TEST("actuator gate OK for detergent pump");
    actuator_self_test_gate_input_t ok = act_ok_gate();
    if (actuator_self_test_gate_check(&ok) == ACT_GATE_OK) PASS();
    else FAIL("expected OK");

    TEST("actuator stale: valid=true fresh=false -> POSITION_STALE (not UNKNOWN)");
    actuator_self_test_gate_input_t stale = act_ok_gate();
    stale.pos_valid = true;
    stale.pos_fresh = false;
    if (actuator_self_test_gate_check(&stale) == ACT_GATE_POSITION_STALE) PASS();
    else FAIL("stale must reject distinctly");
}

static void test_actuator_request_id(void)
{
    TEST("actuator next_request_id skips 0");
    if (actuator_self_test_next_request_id(UINT32_MAX - 1) == UINT32_MAX) PASS();
    else FAIL("wrap");

    TEST("actuator next_request_id wraps past 0 to 1");
    if (actuator_self_test_next_request_id(UINT32_MAX) == 1U) PASS();
    else FAIL("wrap past invalid");
}

/* P1-2 fail-closed OFF proof: snapshot failure / timeout / UNKNOWN must never
 * read as "closed".  Only both-ok + all-known-off returns true. */
static void test_act_off_fail_closed(void)
{
    actuator_off_proof_t ok = {
        .water_snapshot_ok = true, .source_off = true,
        .source_unknown = false, .transfer_off = true,
        .transfer_unknown = false,
        .detergent_snapshot_ok = true, .detergent_off = true,
        .drain_snapshot_ok = true, .drain_off = true,
        .dry_snapshot_ok = true, .fan_off = true, .heater_off = true,
        .hot_air_snapshot_ok = true, .hot_air_off = true,
    };
    TEST("OFF proof: all snapshots ok + all KNOWN_OFF -> true");
    if (actuator_outputs_confirmed_off(&ok)) PASS();
    else FAIL("known-off must prove closed");

    actuator_off_proof_t p = ok;
    p.water_snapshot_ok = false;
    TEST("OFF proof: water snapshot error -> false");
    if (!actuator_outputs_confirmed_off(&p)) PASS();
    else FAIL("water snapshot error must NOT prove closed");

    p = ok;
    p.detergent_snapshot_ok = false;
    TEST("OFF proof: detergent snapshot error -> false");
    if (!actuator_outputs_confirmed_off(&p)) PASS();
    else FAIL("detergent snapshot error must NOT prove closed");

    p = ok;
    p.source_off = false;
    TEST("OFF proof: source ON -> false");
    if (!actuator_outputs_confirmed_off(&p)) PASS();
    else FAIL("source ON must NOT prove closed");

    p = ok;
    p.source_unknown = true;
    TEST("OFF proof: source UNKNOWN -> false");
    if (!actuator_outputs_confirmed_off(&p)) PASS();
    else FAIL("source UNKNOWN must NOT prove closed");

    p = ok;
    p.transfer_off = false;
    TEST("OFF proof: transfer ON -> false");
    if (!actuator_outputs_confirmed_off(&p)) PASS();
    else FAIL("transfer ON must NOT prove closed");

    p = ok;
    p.transfer_unknown = true;
    TEST("OFF proof: transfer UNKNOWN -> false");
    if (!actuator_outputs_confirmed_off(&p)) PASS();
    else FAIL("transfer UNKNOWN must NOT prove closed");

    p = ok;
    p.detergent_off = false;
    TEST("OFF proof: detergent not KNOWN_OFF -> false");
    if (!actuator_outputs_confirmed_off(&p)) PASS();
    else FAIL("detergent unknown/on must NOT prove closed");

    p = ok;
    p.drain_snapshot_ok = false;
    TEST("OFF proof: drain snapshot error -> false");
    if (!actuator_outputs_confirmed_off(&p)) PASS();
    else FAIL("drain snapshot error must NOT prove closed");

    p = ok;
    p.drain_off = false;
    TEST("OFF proof: drain ON -> false");
    if (!actuator_outputs_confirmed_off(&p)) PASS();
    else FAIL("drain ON must NOT prove closed");

    p = ok;
    p.fan_off = false;
    TEST("OFF proof: fan ON -> false");
    if (!actuator_outputs_confirmed_off(&p)) PASS();
    else FAIL("fan ON must NOT prove closed");

    p = ok;
    p.heater_off = false;
    TEST("OFF proof: heater ON -> false");
    if (!actuator_outputs_confirmed_off(&p)) PASS();
    else FAIL("heater ON must NOT prove closed");

    p = ok;
    p.hot_air_snapshot_ok = false;
    TEST("OFF proof: hot air snapshot error -> false");
    if (!actuator_outputs_confirmed_off(&p)) PASS();
    else FAIL("hot air snapshot error must NOT prove closed");

    p = ok;
    p.hot_air_off = false;
    TEST("OFF proof: hot air ON -> false");
    if (!actuator_outputs_confirmed_off(&p)) PASS();
    else FAIL("hot air ON must NOT prove closed");

    TEST("OFF proof: NULL -> false");
    if (!actuator_outputs_confirmed_off(NULL)) PASS();
    else FAIL("NULL must NOT prove closed");
}

/* ---- HW-FIX-1: water valve initial safe-OFF synchronization ---- */

static water_fsm_ctx_t make_water_ctx(void)
{
    water_service_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.pulses_per_liter = 100.0f;
    cfg.source_batch_max_ms = 5000;
    cfg.source_settle_ms = 500;
    cfg.transfer_timeout_ms = 2000;
    cfg.default_transfer_ms = 1000;
    cfg.max_fill_cycles = 3;
    cfg.total_inlet_timeout_ms = 30000;
    water_fsm_ctx_t ctx;
    water_fsm_init(&ctx, &cfg);
    return ctx;
}

static void test_water_initial_off(void)
{
    TEST("HW-FIX-1: boot -> both valves UNKNOWN (never assumed OFF)");
    water_fsm_ctx_t ctx = make_water_ctx();
    if (ctx.source_valve == WATER_VALVE_UNKNOWN &&
        ctx.transfer_valve == WATER_VALVE_UNKNOWN) PASS();
    else FAIL("boot must be UNKNOWN, not OFF");

    TEST("HW-FIX-1: sync OK -> both KNOWN_OFF");
    ctx = make_water_ctx();
    water_fsm_output_t out;
    if (water_fsm_commit_initial_off(&ctx, WATER_COMMIT_OK, WATER_COMMIT_OK, &out)
        == ESP_OK &&
        ctx.source_valve == WATER_VALVE_KNOWN_OFF &&
        ctx.transfer_valve == WATER_VALVE_KNOWN_OFF) PASS();
    else FAIL("successful OFF sync must commit KNOWN_OFF");

    TEST("HW-FIX-1: sync FAIL -> both UNKNOWN (fail-closed)");
    ctx = make_water_ctx();
    if (water_fsm_commit_initial_off(&ctx, WATER_COMMIT_FAILED,
                                     WATER_COMMIT_FAILED, &out) == ESP_OK &&
        ctx.source_valve == WATER_VALVE_UNKNOWN &&
        ctx.transfer_valve == WATER_VALVE_UNKNOWN) PASS();
    else FAIL("failed OFF sync must stay UNKNOWN");

    TEST("HW-FIX-1: partial sync -> per-valve (source OFF, transfer UNKNOWN)");
    ctx = make_water_ctx();
    if (water_fsm_commit_initial_off(&ctx, WATER_COMMIT_OK,
                                     WATER_COMMIT_FAILED, &out) == ESP_OK &&
        ctx.source_valve == WATER_VALVE_KNOWN_OFF &&
        ctx.transfer_valve == WATER_VALVE_UNKNOWN) PASS();
    else FAIL("per-valve commit must be independent");

    TEST("HW-FIX-1: initial-off commit rejected when FSM not IDLE");
    ctx = make_water_ctx();
    ctx.state = WATER_STATE_FAULT;  /* non-idle forces fail-closed */
    if (water_fsm_commit_initial_off(&ctx, WATER_COMMIT_OK, WATER_COMMIT_OK, &out)
        == ESP_ERR_INVALID_STATE) PASS();
    else FAIL("initial-off commit must require IDLE");

    TEST("HW-FIX-1: UNKNOWN must not satisfy the OFF gate (pre-sync)");
    ctx = make_water_ctx();
    water_snapshot_t snap;
    water_fsm_snapshot(&ctx, &snap);
    actuator_self_test_gate_input_t in = act_ok_gate();
    in.water_safe_off_synced = false;
    in.target_confirmed_off = !snap.source_valve_on && !snap.source_valve_unknown;
    if (actuator_self_test_gate_check(&in) == ACT_GATE_OUTPUT_STATE_UNKNOWN) PASS();
    else FAIL("pre-sync must reject with OUTPUT_STATE_UNKNOWN, not accept");

    TEST("HW-FIX-1: post-sync KNOWN_OFF -> SOURCE gate OK (first test accepted)");
    ctx = make_water_ctx();
    water_fsm_commit_initial_off(&ctx, WATER_COMMIT_OK, WATER_COMMIT_OK, &out);
    water_fsm_snapshot(&ctx, &snap);
    in = act_ok_gate();
    in.water_safe_off_synced = true;
    in.target_confirmed_off = !snap.source_valve_on && !snap.source_valve_unknown;
    if (actuator_self_test_gate_check(&in) == ACT_GATE_OK) PASS();
    else FAIL("synced KNOWN_OFF must let SOURCE through");

    TEST("HW-FIX-1: sync failed (UNKNOWN) -> reject fail-closed, never accept");
    ctx = make_water_ctx();
    water_fsm_commit_initial_off(&ctx, WATER_COMMIT_FAILED, WATER_COMMIT_FAILED, &out);
    water_fsm_snapshot(&ctx, &snap);
    in = act_ok_gate();
    in.water_safe_off_synced = true;
    in.target_confirmed_off = !snap.source_valve_on && !snap.source_valve_unknown;
    if (actuator_self_test_gate_check(&in) == ACT_GATE_OUTPUT_ALREADY_ON) PASS();
    else FAIL("UNKNOWN target after sync must reject with OUTPUT_ALREADY_ON");

    TEST("HW-FIX-1: source active -> transfer rejected (conflict, no overlap)");
    ctx = make_water_ctx();
    water_fsm_commit_initial_off(&ctx, WATER_COMMIT_OK, WATER_COMMIT_OK, &out);
    ctx.source_valve = WATER_VALVE_KNOWN_ON;   /* simulate active SOURCE */
    water_fsm_snapshot(&ctx, &snap);
    in = act_ok_gate();
    in.water_safe_off_synced = true;
    in.target_confirmed_off = !snap.transfer_valve_on && !snap.transfer_valve_unknown;
    in.conflict_confirmed_off = (!snap.source_valve_on && !snap.source_valve_unknown);
    if (actuator_self_test_gate_check(&in) == ACT_GATE_CONFLICT_OUTPUT_ON) PASS();
    else FAIL("active source must block transfer (CONFLICT_OUTPUT_ON)");
}

/* ---- HW-FIX-2: detergent idle/output mapping ---- */

static void test_detergent_output_contract(void)
{
    detergent_fsm_ctx_t ctx;
    detergent_params_t cfg = { .max_single_ms = 800 };
    detergent_fsm_init(&ctx, &cfg);

    TEST("HW-FIX-2: detergent_fsm_output_to_public explicit mapping");
    int ok = detergent_fsm_output_to_public(DETERGENT_OUTPUT_UNKNOWN) == 0 &&
             detergent_fsm_output_to_public(DETERGENT_OUTPUT_KNOWN_OFF) == 1 &&
             detergent_fsm_output_to_public(DETERGENT_OUTPUT_KNOWN_ON) == 2 &&
             detergent_fsm_output_to_public((detergent_output_state_t)88) == 0;
    if (ok) PASS(); else FAIL("output mapping");

    TEST("HW-FIX-2: terminal_pending false at idle boot (no SERVICE_BUSY)");
    if (!detergent_fsm_terminal_pending(&ctx)) PASS();
    else FAIL("idle must not report a pending terminal");

    TEST("HW-FIX-2: boot idle maps to public IDLE (gate busy=false)");
    if (detergent_fsm_state_to_public(ctx.state) == DETERGENT_STATE_IDLE) PASS();
    else FAIL("boot idle must map to DETERGENT_STATE_IDLE");
}

/* ---- HW-FIX-3: UV commanded-OFF confirmation contract ---- */

static void uv_drive_to_running(uv_fsm_ctx_t *ctx, uint32_t now_ms)
{
    uv_fsm_event_t t = { .type = UV_EVT_TICK, .now_ms = now_ms,
        .position = DRUM_POS_0, .position_valid = true,
        .position_stable = true, .position_fresh = true,
        .position_motor_moving = false };
    uv_fsm_output_t out;
    uv_fsm_tick(ctx, &t, &out);
    uv_fsm_event_t ar = { .type = UV_EVT_ACTION_RESULT, .now_ms = now_ms,
        .action_result_action = out.action, .action_result_ok = true };
    uv_fsm_tick(ctx, &ar, &out);   /* RUNNING + KNOWN_ON */
}

static void test_uv_output_contract(void)
{
    uv_fsm_ctx_t ctx;
    uv_params_t cfg = { .default_duration_ms = 3000, .max_duration_ms = 3000 };
    uv_fsm_init(&ctx, &cfg);

    TEST("HW-FIX-3: uv_fsm_output_to_public explicit mapping");
    int ok = uv_fsm_output_to_public(UV_OUTPUT_UNKNOWN) == 0 &&
             uv_fsm_output_to_public(UV_OUTPUT_KNOWN_OFF) == 1 &&
             uv_fsm_output_to_public(UV_OUTPUT_KNOWN_ON) == 2 &&
             uv_fsm_output_to_public((uv_output_state_t)77) == 0;
    if (ok) PASS(); else FAIL("output mapping");

    TEST("HW-FIX-3: terminal_pending false at idle boot");
    if (!uv_fsm_terminal_pending(&ctx)) PASS(); else FAIL("idle must not be pending");

    uv_request_t req = { .request_id = 7, .duration_ms = 3000 };
    uv_fsm_output_t out;

    TEST("HW-FIX-3: normal complete -> terminal COMPLETE + output KNOWN_OFF");
    uv_fsm_init(&ctx, &cfg);
    uv_fsm_begin_request(&ctx, &req, 3000, 1000);
    uv_drive_to_running(&ctx, 1100);
    uv_fsm_event_t t = { .type = UV_EVT_TICK, .now_ms = 4100, .position = DRUM_POS_0,
        .position_valid = true, .position_stable = true, .position_fresh = true };
    uv_fsm_tick(&ctx, &t, &out);   /* → STOPPING PUMP_OFF */
    uv_fsm_event_t ar = { .type = UV_EVT_ACTION_RESULT, .now_ms = 4100,
        .action_result_action = out.action, .action_result_ok = true };
    uv_fsm_tick(&ctx, &ar, &out);  /* → COMPLETE */
    if (ctx.terminal == UV_TERMINAL_COMPLETE &&
        ctx.output_state == UV_OUTPUT_KNOWN_OFF) PASS();
    else FAIL("normal complete must end KNOWN_OFF");

    TEST("HW-FIX-3: cancel -> CANCELED terminal + KNOWN_OFF");
    uv_fsm_init(&ctx, &cfg);
    uv_fsm_begin_request(&ctx, &req, 3000, 1000);
    uv_drive_to_running(&ctx, 1100);
    uv_fsm_event_t c = { .type = UV_EVT_CANCEL, .now_ms = 1200, .cancel_request_id = 7 };
    uv_fsm_tick(&ctx, &c, &out);   /* CANCELING PUMP_OFF */
    ar = (uv_fsm_event_t){ .type = UV_EVT_ACTION_RESULT, .now_ms = 1300,
        .action_result_action = out.action, .action_result_ok = true };
    uv_fsm_tick(&ctx, &ar, &out);
    if (ctx.terminal == UV_TERMINAL_CANCELED &&
        ctx.output_state == UV_OUTPUT_KNOWN_OFF) PASS();
    else FAIL("cancel must end KNOWN_OFF");

    TEST("HW-FIX-3: emergency -> FAULT terminal + KNOWN_OFF");
    uv_fsm_init(&ctx, &cfg);
    uv_fsm_begin_request(&ctx, &req, 3000, 1000);
    uv_drive_to_running(&ctx, 1100);
    uv_fsm_event_t em = { .type = UV_EVT_EMERGENCY, .now_ms = 1200 };
    uv_fsm_tick(&ctx, &em, &out);  /* STOPPING PUMP_OFF terminal FAULT */
    ar = (uv_fsm_event_t){ .type = UV_EVT_ACTION_RESULT, .now_ms = 1300,
        .action_result_action = out.action, .action_result_ok = true };
    uv_fsm_tick(&ctx, &ar, &out);
    if (ctx.terminal == UV_TERMINAL_FAULT &&
        ctx.output_state == UV_OUTPUT_KNOWN_OFF) PASS();
    else FAIL("emergency must end KNOWN_OFF");

    TEST("HW-FIX-3: OFF apply failure -> output UNKNOWN (never claim OFF)");
    uv_fsm_init(&ctx, &cfg);
    uv_fsm_begin_request(&ctx, &req, 3000, 1000);
    uv_drive_to_running(&ctx, 1100);
    t.now_ms = 4100;
    uv_fsm_tick(&ctx, &t, &out);   /* STOPPING PUMP_OFF */
    ar = (uv_fsm_event_t){ .type = UV_EVT_ACTION_RESULT, .now_ms = 4100,
        .action_result_action = out.action, .action_result_ok = false };
    uv_fsm_tick(&ctx, &ar, &out);
    if (ctx.output_state == UV_OUTPUT_UNKNOWN) PASS();
    else FAIL("failed OFF must leave UNKNOWN, never OFF");

    TEST("HW-FIX-3: post-reversion IDLE retains COMPLETE terminal + KNOWN_OFF");
    uv_fsm_init(&ctx, &cfg);
    uv_fsm_begin_request(&ctx, &req, 3000, 1000);
    uv_drive_to_running(&ctx, 1100);
    t.now_ms = 4100;
    uv_fsm_tick(&ctx, &t, &out);
    ar = (uv_fsm_event_t){ .type = UV_EVT_ACTION_RESULT, .now_ms = 4100,
        .action_result_action = out.action, .action_result_ok = true };
    uv_fsm_tick(&ctx, &ar, &out);  /* COMPLETE */
    uv_fsm_mark_terminal_emitted(&ctx);   /* 服务发布 terminal */
    ctx.state = UV_FSM_IDLE;              /* 服务回 IDLE，保留 terminal/output */
    if (ctx.terminal == UV_TERMINAL_COMPLETE &&
        ctx.output_state == UV_OUTPUT_KNOWN_OFF &&
        !uv_fsm_terminal_pending(&ctx)) PASS();
    else FAIL("IDLE must retain COMPLETE + KNOWN_OFF so the poller can conclude");

    TEST("HW-FIX-3: request_id retained after reversion (guards stale terminal)");
    if (ctx.request_id == 7) PASS(); else FAIL("request_id must be retained");
}

/* ---- Group B: drain + fan self-test targets ---- */

static void test_group_b_targets(void)
{
    TEST("B: drain duration 2000 / fan duration 5000");
    if (actuator_self_test_duration_ms(ACT_TARGET_DRAIN_PUMP) == 2000U &&
        actuator_self_test_duration_ms(ACT_TARGET_FAN) == 5000U) PASS();
    else FAIL("durations");

    TEST("B: drain/fan target names round-trip");
    actuator_self_test_target_t t = ACT_TARGET_NONE;
    int ok = actuator_self_test_target_from_name("drain_pump", &t) &&
             t == ACT_TARGET_DRAIN_PUMP &&
             strcmp(actuator_self_test_target_name(ACT_TARGET_DRAIN_PUMP),
                    "drain_pump") == 0 &&
             actuator_self_test_target_from_name("fan", &t) &&
             t == ACT_TARGET_FAN &&
             strcmp(actuator_self_test_target_name(ACT_TARGET_FAN), "fan") == 0;
    if (ok) PASS(); else FAIL("drain/fan round-trip");

    TEST("B: drain gate accepts at 180° (all OFF + idle)");
    actuator_self_test_gate_input_t in = act_ok_gate();
    in.required_pos = DRUM_POS_180;
    in.pos = DRUM_POS_180;
    if (actuator_self_test_gate_check(&in) == ACT_GATE_OK) PASS();
    else FAIL("drain at 180 must pass");

    TEST("B: drain gate rejects at 0° (POSITION_NOT_ZERO)");
    in = act_ok_gate();
    in.required_pos = DRUM_POS_180;
    in.pos = DRUM_POS_0;
    if (actuator_self_test_gate_check(&in) == ACT_GATE_POSITION_NOT_ZERO) PASS();
    else FAIL("drain at 0 must reject");

    TEST("B: fan gate accepts at 270° (fan OFF, heater OFF, temp OK)");
    in = act_ok_gate();
    in.required_pos = DRUM_POS_270;
    in.pos = DRUM_POS_270;
    if (actuator_self_test_gate_check(&in) == ACT_GATE_OK) PASS();
    else FAIL("fan at 270 must pass");

    TEST("B: fan gate rejects when heater is ON (SERVICE_BUSY)");
    in = act_ok_gate();
    in.required_pos = DRUM_POS_270;
    in.pos = DRUM_POS_270;
    in.heater_confirmed_off = false;
    if (actuator_self_test_gate_check(&in) == ACT_GATE_SERVICE_BUSY) PASS();
    else FAIL("fan with heater ON must reject");

    TEST("B: fan gate rejects when fan output not KNOWN_OFF");
    in = act_ok_gate();
    in.required_pos = DRUM_POS_270;
    in.pos = DRUM_POS_270;
    in.fan_confirmed_off = false;
    in.target_confirmed_off = false;
    if (actuator_self_test_gate_check(&in) == ACT_GATE_OUTPUT_ALREADY_ON) PASS();
    else FAIL("fan already on must reject OUTPUT_ALREADY_ON");

    TEST("B: fan gate rejects when temp stale (SERVICE_BUSY)");
    in = act_ok_gate();
    in.required_pos = DRUM_POS_270;
    in.pos = DRUM_POS_270;
    in.sht_valid_fresh = false;
    if (actuator_self_test_gate_check(&in) == ACT_GATE_SERVICE_BUSY) PASS();
    else FAIL("fan with stale temp must reject");

    TEST("B: drain gate rejects when dry_service busy (SERVICE_BUSY)");
    in = act_ok_gate();
    in.required_pos = DRUM_POS_180;
    in.pos = DRUM_POS_180;
    in.dry_service_idle = false;
    if (actuator_self_test_gate_check(&in) == ACT_GATE_SERVICE_BUSY) PASS();
    else FAIL("drain with dry busy must reject");

    TEST("B: drain gate rejects when a water valve is active (conflict)");
    in = act_ok_gate();
    in.required_pos = DRUM_POS_180;
    in.pos = DRUM_POS_180;
    in.conflict_confirmed_off = false;
    if (actuator_self_test_gate_check(&in) == ACT_GATE_CONFLICT_OUTPUT_ON) PASS();
    else FAIL("drain with active water valve must reject");
}

/* ---- Group B: drain/dry FSM mapping + initial-off sync ---- */

static void test_drain_dry_mapping(void)
{
    TEST("B: drain_fsm_state_to_public maps offset states correctly");
    int ok = drain_fsm_state_to_public(DRAIN_FSM_IDLE) == DRAIN_STATE_IDLE &&
             drain_fsm_state_to_public(DRAIN_FSM_RUNNING) == DRAIN_STATE_RUNNING &&
             drain_fsm_state_to_public(DRAIN_FSM_FAULT) == DRAIN_STATE_FAULT &&
             drain_fsm_state_to_public((drain_fsm_state_t)99) == DRAIN_STATE_FAULT &&
             drain_fsm_state_to_public(DRAIN_FSM_IDLE) !=
                 (drain_state_t)DRAIN_FSM_IDLE;  /* 显式映射 ≠ 原始数值 */
    if (ok) PASS(); else FAIL("drain state mapping");

    TEST("B: drain_fsm_output_to_public explicit mapping");
    ok = drain_fsm_output_to_public(DRAIN_OUTPUT_UNKNOWN) == 0 &&
         drain_fsm_output_to_public(DRAIN_OUTPUT_KNOWN_OFF) == 1 &&
         drain_fsm_output_to_public(DRAIN_OUTPUT_KNOWN_ON) == 2 &&
         drain_fsm_output_to_public((drain_output_state_t)55) == 0;
    if (ok) PASS(); else FAIL("drain output mapping");

    TEST("B: drain_fsm_terminal_pending false at idle boot");
    drain_params_t dp = { .max_duration_ms = 120000 };
    drain_fsm_ctx_t dctx;
    drain_fsm_init(&dctx, &dp);
    if (!drain_fsm_terminal_pending(&dctx)) PASS(); else FAIL("drain idle must not be pending");

    TEST("B: dry_fsm_commit_initial_off OK -> fan/heater KNOWN_OFF");
    dry_params_t dryp;
    memset(&dryp, 0, sizeof(dryp));
    dryp.pre_fan_ms = 5000; dryp.heat_on_max_ms = 60000; dryp.heat_off_min_ms = 1000;
    dryp.cooldown_ms = 60000; dryp.max_total_ms = 1800000;
    dryp.heater_cutoff_c = 55.0f; dryp.heater_resume_c = 45.0f; dryp.fan_percent = 30;
    dry_fsm_ctx_t dryctx;
    dry_fsm_init(&dryctx, &dryp);
    if (dry_fsm_commit_initial_off(&dryctx, true, true) == ESP_OK &&
        dryctx.fan_state == DRY_OUTPUT_KNOWN_OFF &&
        dryctx.heater_state == DRY_OUTPUT_KNOWN_OFF) PASS();
    else FAIL("dry initial-off sync must commit KNOWN_OFF");

    TEST("B: dry_fsm_commit_initial_off FAIL -> UNKNOWN (fail-closed)");
    dry_fsm_init(&dryctx, &dryp);
    if (dry_fsm_commit_initial_off(&dryctx, false, true) == ESP_OK &&
        dryctx.fan_state == DRY_OUTPUT_UNKNOWN &&
        dryctx.heater_state == DRY_OUTPUT_KNOWN_OFF) PASS();
    else FAIL("dry failed fan OFF must stay UNKNOWN");

    TEST("B: dry_fsm_commit_initial_off rejected when not IDLE");
    dry_fsm_init(&dryctx, &dryp);
    dryctx.state = DRY_FSM_HEATING;
    if (dry_fsm_commit_initial_off(&dryctx, true, true) == ESP_ERR_INVALID_STATE) PASS();
    else FAIL("dry initial-off commit must require IDLE");

    TEST("B: dry_fsm_terminal_pending false at idle boot");
    dry_fsm_init(&dryctx, &dryp);
    if (!dry_fsm_terminal_pending(&dryctx)) PASS(); else FAIL("dry idle must not be pending");
}

/* ---- P1-1: dry emergency must not terminate lifecycle (FSM-level) ---- */

/* Drive a fan-only dry request to PRE_FAN (fan KNOWN_ON). begin_request resets
 * output shadows to UNKNOWN, so this re-establishes the OFF baseline
 * (HEATER_OFF, FAN_OFF) then turns the fan on. */
static void dry_drive_to_pre_fan(dry_fsm_ctx_t *ctx, dry_fsm_output_t *out)
{
    dry_fsm_event_t t = { .type = DRY_EVT_TICK, .now_ms = 1100,
        .position = DRUM_POS_270, .position_valid = true, .position_stable = true,
        .position_fresh = true, .position_motor_moving = false,
        .sht_valid = true, .sht_fresh = true, .temperature_c = 25.0f };
    dry_fsm_event_t ar;
    int guard = 0;
    dry_fsm_tick(ctx, &t, out);               /* VALIDATING -> ENSURE_PTC_OFF */
    while (out->action != DRY_ACTION_NONE && guard++ < 8) {
        ar = (dry_fsm_event_t){ .type = DRY_EVT_ACTION_RESULT, .now_ms = 1200,
            .action_result_action = out->action, .action_result_ok = true };
        dry_fsm_tick(ctx, &ar, out);
    }
    dry_fsm_tick(ctx, &t, out);               /* ENSURE_PTC_OFF -> (HEATER_OFF if needed) */
    while (out->action != DRY_ACTION_NONE && guard++ < 8) {
        ar = (dry_fsm_event_t){ .type = DRY_EVT_ACTION_RESULT, .now_ms = 1200,
            .action_result_action = out->action, .action_result_ok = true };
        dry_fsm_tick(ctx, &ar, out);
    }
    dry_fsm_tick(ctx, &t, out);               /* ENSURE_FAN_OFF */
    while (out->action != DRY_ACTION_NONE && guard++ < 8) {
        ar = (dry_fsm_event_t){ .type = DRY_EVT_ACTION_RESULT, .now_ms = 1200,
            .action_result_action = out->action, .action_result_ok = true };
        dry_fsm_tick(ctx, &ar, out);
    }
    dry_fsm_tick(ctx, &t, out);               /* STARTING_FAN -> FAN_ON */
    while (out->action != DRY_ACTION_NONE && guard++ < 8) {
        ar = (dry_fsm_event_t){ .type = DRY_EVT_ACTION_RESULT, .now_ms = 1200,
            .action_result_action = out->action, .action_result_ok = true };
        dry_fsm_tick(ctx, &ar, out);
    }
}

/* Drive the FSM shutdown phases (ticks + action results) until COMPLETE or guard.
 * off_ok=false simulates an OFF-apply failure. */
static void dry_drive_shutdown(dry_fsm_ctx_t *ctx, dry_fsm_output_t *out, bool off_ok)
{
    dry_fsm_event_t t = { .type = DRY_EVT_TICK, .now_ms = 3000,
        .position = DRUM_POS_270, .position_valid = true, .position_stable = true,
        .position_fresh = true, .position_motor_moving = false,
        .sht_valid = true, .sht_fresh = true, .temperature_c = 25.0f };
    int guard = 0;
    while (ctx->state != DRY_FSM_COMPLETE && guard++ < 12) {
        dry_fsm_tick(ctx, &t, out);
        if (out->action != DRY_ACTION_NONE) {
            dry_fsm_event_t ar = { .type = DRY_EVT_ACTION_RESULT, .now_ms = 3000,
                .action_result_action = out->action, .action_result_ok = off_ok };
            dry_fsm_tick(ctx, &ar, out);
        }
    }
    dry_fsm_tick(ctx, &t, out);   /* COMPLETE -> emit retained terminal */
}

static void test_dry_emergency_semantics(void)
{
    dry_params_t dryp;
    memset(&dryp, 0, sizeof(dryp));
    dryp.pre_fan_ms = 5000; dryp.heat_on_max_ms = 60000; dryp.heat_off_min_ms = 1000;
    dryp.cooldown_ms = 60000; dryp.max_total_ms = 1800000;
    dryp.heater_cutoff_c = 55.0f; dryp.heater_resume_c = 45.0f; dryp.fan_percent = 30;
    dry_fsm_ctx_t ctx;
    dry_fsm_init(&ctx, &dryp);
    dry_fsm_output_t out;

    TEST("P1-1: dry IDLE + emergency -> stays IDLE, no terminal, no action");
    dry_fsm_event_t em = { .type = DRY_EVT_EMERGENCY, .now_ms = 1000 };
    dry_fsm_tick(&ctx, &em, &out);
    if (ctx.state == DRY_FSM_IDLE && out.terminal == DRY_TERMINAL_NONE &&
        out.action == DRY_ACTION_NONE && !out.emit_event) PASS();
    else FAIL("IDLE emergency must be idempotent no-op");

    TEST("P1-1: dry RUNNING + emergency -> PTC OFF first, FAN OFF, terminal CANCELED");
    dry_fsm_init(&ctx, &dryp);
    dry_request_t req = { .request_id = 9, .duration_ms = 5000, .heater_requested = false };
    dry_fsm_begin_request(&ctx, &req, 1000);
    dry_drive_to_pre_fan(&ctx, &out);
    if (ctx.state != DRY_FSM_PRE_FAN || ctx.fan_state != DRY_OUTPUT_KNOWN_ON) {
        FAIL("setup: expected PRE_FAN with fan KNOWN_ON");
        return;
    }
    em.now_ms = 2000;
    dry_fsm_tick(&ctx, &em, &out);              /* emergency -> begin_shutdown (CANCELING) */
    dry_drive_shutdown(&ctx, &out, true);       /* PTC off first, FAN off, COMPLETE + terminal */
    if (ctx.fan_state == DRY_OUTPUT_KNOWN_OFF &&
        ctx.heater_state == DRY_OUTPUT_KNOWN_OFF &&
        ctx.terminal == DRY_TERMINAL_CANCELED) PASS();
    else FAIL("emergency must end fan+heater OFF + terminal CANCELED");

    TEST("P1-1: dry emergency OFF-apply failure -> output UNKNOWN (never forced OFF)");
    dry_fsm_init(&ctx, &dryp);
    dry_fsm_begin_request(&ctx, &req, 1000);
    dry_drive_to_pre_fan(&ctx, &out);
    em.now_ms = 2000;
    dry_fsm_tick(&ctx, &em, &out);
    dry_drive_shutdown(&ctx, &out, false);      /* FAN_OFF fails -> fan stays UNKNOWN */
    if (ctx.fan_state == DRY_OUTPUT_UNKNOWN) PASS();
    else FAIL("failed FAN_OFF must leave UNKNOWN, never claim OFF");
}

/* ---- P1-2: gate snapshot failure fail-closed ---- */

static void test_snapshot_fail_closed(void)
{
    TEST("P1-2: WATER snapshot failure -> WATER_SNAPSHOT_UNAVAILABLE");
    actuator_self_test_gate_input_t in = act_ok_gate();
    in.snapshot_fail = ACT_GATE_WATER_SNAPSHOT_UNAVAILABLE;
    if (actuator_self_test_gate_check(&in) == ACT_GATE_WATER_SNAPSHOT_UNAVAILABLE) PASS();
    else FAIL("water snapshot unavailable must reject distinctly");

    TEST("P1-2: DETERGENT snapshot failure -> DETERGENT_SNAPSHOT_UNAVAILABLE");
    in = act_ok_gate();
    in.snapshot_fail = ACT_GATE_DETERGENT_SNAPSHOT_UNAVAILABLE;
    if (actuator_self_test_gate_check(&in) == ACT_GATE_DETERGENT_SNAPSHOT_UNAVAILABLE) PASS();
    else FAIL("detergent snapshot unavailable must reject distinctly");

    TEST("P1-2: DRAIN snapshot failure -> DRAIN_SNAPSHOT_UNAVAILABLE");
    in = act_ok_gate();
    in.snapshot_fail = ACT_GATE_DRAIN_SNAPSHOT_UNAVAILABLE;
    if (actuator_self_test_gate_check(&in) == ACT_GATE_DRAIN_SNAPSHOT_UNAVAILABLE) PASS();
    else FAIL("drain snapshot unavailable must reject distinctly");

    TEST("P1-2: DRY snapshot failure -> DRY_SNAPSHOT_UNAVAILABLE");
    in = act_ok_gate();
    in.snapshot_fail = ACT_GATE_DRY_SNAPSHOT_UNAVAILABLE;
    if (actuator_self_test_gate_check(&in) == ACT_GATE_DRY_SNAPSHOT_UNAVAILABLE) PASS();
    else FAIL("dry snapshot unavailable must reject distinctly");

    TEST("P1-2: snapshot_fail codes are machine-readable strings");
    if (strcmp(actuator_self_test_gate_code(ACT_GATE_WATER_SNAPSHOT_UNAVAILABLE),
               "WATER_SNAPSHOT_UNAVAILABLE") == 0 &&
        strcmp(actuator_self_test_gate_code(ACT_GATE_DRY_SNAPSHOT_UNAVAILABLE),
               "DRY_SNAPSHOT_UNAVAILABLE") == 0) PASS();
    else FAIL("gate code strings");

    TEST("P1-2: no snapshot failure + all idle -> gate OK (act_ok_gate baseline)");
    in = act_ok_gate();
    if (actuator_self_test_gate_check(&in) == ACT_GATE_OK) PASS();
    else FAIL("baseline must stay OK");
}

int main(void)
{
    test_uv_enum_mapping();
    test_detergent_enum_mapping();
    test_uv_terminal_semantics();
    test_uv_stale_gate();
    test_actuator_targets();
    test_actuator_gates();
    test_actuator_request_id();
    test_act_off_fail_closed();
    test_water_initial_off();
    test_detergent_output_contract();
    test_uv_output_contract();
    test_group_b_targets();
    test_drain_dry_mapping();
    test_dry_emergency_semantics();
    test_snapshot_fail_closed();

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
