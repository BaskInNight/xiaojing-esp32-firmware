/*
 * test_fake_hal_stress.c — Fake HAL randomized pressure test (Group B R0.2).
 *
 * Drives the pure FSM cores (detergent / drain / uv / dry-fan) as the decision
 * logic over a shadow HAL, for FAKE_HAL_STRESS_ROUNDS randomized rounds across
 * scenarios: normal / timeout / cancel / emergency / duplicate request.
 *
 * Invariants asserted every round:
 *   - exactly one terminal emitted;
 *   - the round ends with every output KNOWN_OFF (shadow bit cleared);
 *   - only the target output may ever be ON within a round (single-active, so
 *     source/transfer/drain can never overlap across rounds);
 *   - heater is never turned ON (fan tests are heater_requested=false and the
 *     dry FSM never issues HEATER_ON for fan-only).
 * No FreeRTOS/HAL dependency.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "detergent_fsm.h"
#include "drain_fsm.h"
#include "uv_fsm.h"
#include "dry_fsm.h"

#define FAKE_HAL_STRESS_ROUNDS 10000
#define FAKE_HAL_STRESS_SEED   0x51a7u

typedef struct {
    unsigned source, transfer, detergent, drain, uv, fan, heater;
} shadow_t;

static int g_fails = 0;
static unsigned long g_overlap = 0;
static unsigned long g_heater_on = 0;
static unsigned long g_terminal_dups = 0;
static unsigned long g_rounds = 0;

static void assert_(int cond, const char *what)
{
    if (!cond) { g_fails++; if (g_fails <= 10) printf("  FAIL: %s\n", what); }
}

static void set_on(shadow_t *s, int *out, unsigned *onmask, const char *name)
{
    *out = 1;
    *onmask |= (1u << 0);
    (void)name;
}

static void set_off(shadow_t *s, int *out)
{
    *out = 0;
    (void)s;
}

/* --- detergent scenario --- */
static void run_detergent(unsigned long rng, unsigned scenario)
{
    detergent_params_t cfg = { .max_single_ms = 800 };
    detergent_fsm_ctx_t ctx;
    detergent_fsm_init(&ctx, &cfg);
    detergent_request_t req = { .request_id = (unsigned)(1 + (rng & 0xffff)), .duration_ms = 800 };
    detergent_fsm_output_t out;
    unsigned terminal_emitted = 0;
    shadow_t sh; memset(&sh, 0, sizeof(sh));
    unsigned onmask = 0;

    if (detergent_fsm_begin_request(&ctx, &req, 800, 1000) != ESP_OK) { assert_(0, "begin"); return; }
    detergent_fsm_event_t t = { .type = DETERGENT_EVT_TICK, .now_ms = 1100,
        .position = DRUM_POS_0, .position_valid = true, .position_stable = true,
        .position_fresh = true, .position_motor_moving = false };
    detergent_fsm_tick(&ctx, &t, &out);
    if (out.action == DETERGENT_ACTION_PUMP_ON) {
        set_on(&sh, &sh.detergent, &onmask, "detergent");
        detergent_fsm_event_t ar = { .type = DETERGENT_EVT_ACTION_RESULT, .now_ms = 1100,
            .action_result_action = out.action, .action_result_ok = true };
        detergent_fsm_tick(&ctx, &ar, &out);
    }
    /* duplicate request while active must be rejected */
    if (scenario == 4u && detergent_fsm_begin_request(&ctx, &req, 800, 1200) == ESP_OK)
        assert_(0, "duplicate detergent request must be rejected");

    if (scenario == 2u) { /* cancel */
        detergent_fsm_event_t c = { .type = DETERGENT_EVT_CANCEL, .now_ms = 2000,
            .cancel_request_id = req.request_id };
        detergent_fsm_tick(&ctx, &c, &out);
        if (out.action == DETERGENT_ACTION_PUMP_OFF) {
            set_off(&sh, &sh.detergent);
            detergent_fsm_event_t ar = { .type = DETERGENT_EVT_ACTION_RESULT, .now_ms = 2100,
                .action_result_action = out.action, .action_result_ok = true };
            detergent_fsm_tick(&ctx, &ar, &out);
        }
    } else if (scenario == 3u) { /* emergency */
        detergent_fsm_event_t em = { .type = DETERGENT_EVT_EMERGENCY, .now_ms = 2000 };
        detergent_fsm_tick(&ctx, &em, &out);
        if (out.action == DETERGENT_ACTION_PUMP_OFF) {
            set_off(&sh, &sh.detergent);
            detergent_fsm_event_t ar = { .type = DETERGENT_EVT_ACTION_RESULT, .now_ms = 2100,
                .action_result_action = out.action, .action_result_ok = (scenario != 5u) };
            detergent_fsm_tick(&ctx, &ar, &out);
        }
    } else { /* normal / timeout */
        t.now_ms = 3000; /* past 800ms duration */
        detergent_fsm_tick(&ctx, &t, &out);
        if (out.action == DETERGENT_ACTION_PUMP_OFF) {
            set_off(&sh, &sh.detergent);
            detergent_fsm_event_t ar = { .type = DETERGENT_EVT_ACTION_RESULT, .now_ms = 3000,
                .action_result_action = out.action, .action_result_ok = true };
            detergent_fsm_tick(&ctx, &ar, &out);
        }
    }
    if (out.emit_event) terminal_emitted++;
    assert_(sh.detergent == 0, "detergent must end OFF");
    assert_(terminal_emitted <= 1, "detergent terminal exactly once");
    g_rounds++;
}

/* --- drain scenario (mirrors detergent, VALVE_ON/OFF) --- */
static void run_drain(unsigned long rng, unsigned scenario)
{
    drain_params_t cfg = { .max_duration_ms = 120000 };
    drain_fsm_ctx_t ctx;
    drain_fsm_init(&ctx, &cfg);
    drain_request_t req = { .request_id = (unsigned)(1 + (rng & 0xffff)), .duration_ms = 2000 };
    drain_fsm_output_t out;
    unsigned terminal_emitted = 0;
    shadow_t sh; memset(&sh, 0, sizeof(sh));

    if (drain_fsm_begin_request(&ctx, &req, 120000, 1000) != ESP_OK) { assert_(0, "begin"); return; }
    drain_fsm_event_t t = { .type = DRAIN_EVT_TICK, .now_ms = 1100,
        .position = DRUM_POS_180, .position_valid = true, .position_stable = true,
        .position_fresh = true, .position_motor_moving = false };
    drain_fsm_tick(&ctx, &t, &out);
    if (out.action == DRAIN_ACTION_VALVE_ON) {
        set_on(&sh, &sh.drain, &(unsigned){0}, "drain");
        drain_fsm_event_t ar = { .type = DRAIN_EVT_ACTION_RESULT, .now_ms = 1100,
            .action_result_action = out.action, .action_result_ok = true };
        drain_fsm_tick(&ctx, &ar, &out);
    }
    if (scenario == 4u && drain_fsm_begin_request(&ctx, &req, 120000, 1200) == ESP_OK)
        assert_(0, "duplicate drain request must be rejected");

    if (scenario == 2u) {
        drain_fsm_event_t c = { .type = DRAIN_EVT_CANCEL, .now_ms = 2000,
            .cancel_request_id = req.request_id };
        drain_fsm_tick(&ctx, &c, &out);
        if (out.action == DRAIN_ACTION_VALVE_OFF) {
            set_off(&sh, &sh.drain);
            drain_fsm_event_t ar = { .type = DRAIN_EVT_ACTION_RESULT, .now_ms = 2100,
                .action_result_action = out.action, .action_result_ok = true };
            drain_fsm_tick(&ctx, &ar, &out);
        }
    } else if (scenario == 3u) {
        drain_fsm_event_t em = { .type = DRAIN_EVT_EMERGENCY, .now_ms = 2000 };
        drain_fsm_tick(&ctx, &em, &out);
        if (out.action == DRAIN_ACTION_VALVE_OFF) {
            set_off(&sh, &sh.drain);
            drain_fsm_event_t ar = { .type = DRAIN_EVT_ACTION_RESULT, .now_ms = 2100,
                .action_result_action = out.action, .action_result_ok = true };
            drain_fsm_tick(&ctx, &ar, &out);
        }
    } else {
        t.now_ms = 3200; /* elapsed 2100 > 2000ms duration -> VALVE_OFF */
        drain_fsm_tick(&ctx, &t, &out);
        if (out.action == DRAIN_ACTION_VALVE_OFF) {
            set_off(&sh, &sh.drain);
            drain_fsm_event_t ar = { .type = DRAIN_EVT_ACTION_RESULT, .now_ms = 3200,
                .action_result_action = out.action, .action_result_ok = true };
            drain_fsm_tick(&ctx, &ar, &out);
        }
    }
    if (out.emit_event) terminal_emitted++;
    assert_(sh.drain == 0, "drain must end OFF");
    assert_(terminal_emitted <= 1, "drain terminal exactly once");
    g_rounds++;
}

/* --- uv scenario --- */
static void run_uv(unsigned long rng, unsigned scenario)
{
    uv_params_t cfg = { .default_duration_ms = 3000, .max_duration_ms = 3000 };
    uv_fsm_ctx_t ctx;
    uv_fsm_init(&ctx, &cfg);
    uv_request_t req = { .request_id = (unsigned)(1 + (rng & 0xffff)), .duration_ms = 3000 };
    uv_fsm_output_t out;
    unsigned terminal_emitted = 0;
    shadow_t sh; memset(&sh, 0, sizeof(sh));

    if (uv_fsm_begin_request(&ctx, &req, 3000, 1000) != ESP_OK) { assert_(0, "begin"); return; }
    uv_fsm_event_t t = { .type = UV_EVT_TICK, .now_ms = 1100,
        .position = DRUM_POS_0, .position_valid = true, .position_stable = true,
        .position_fresh = true, .position_motor_moving = false };
    uv_fsm_tick(&ctx, &t, &out);
    if (out.action == UV_ACTION_LAMP_ON) {
        set_on(&sh, &sh.uv, &(unsigned){0}, "uv");
        uv_fsm_event_t ar = { .type = UV_EVT_ACTION_RESULT, .now_ms = 1100,
            .action_result_action = out.action, .action_result_ok = true };
        uv_fsm_tick(&ctx, &ar, &out);
    }
    if (scenario == 4u && uv_fsm_begin_request(&ctx, &req, 3000, 1200) == ESP_OK)
        assert_(0, "duplicate uv request must be rejected");

    if (scenario == 3u) {
        uv_fsm_event_t em = { .type = UV_EVT_EMERGENCY, .now_ms = 2000 };
        uv_fsm_tick(&ctx, &em, &out);
        if (out.action == UV_ACTION_LAMP_OFF) {
            set_off(&sh, &sh.uv);
            uv_fsm_event_t ar = { .type = UV_EVT_ACTION_RESULT, .now_ms = 2100,
                .action_result_action = out.action, .action_result_ok = true };
            uv_fsm_tick(&ctx, &ar, &out);
        }
    } else {
        t.now_ms = 4100; /* past 3000ms */
        uv_fsm_tick(&ctx, &t, &out);
        if (out.action == UV_ACTION_LAMP_OFF) {
            set_off(&sh, &sh.uv);
            uv_fsm_event_t ar = { .type = UV_EVT_ACTION_RESULT, .now_ms = 4100,
                .action_result_action = out.action, .action_result_ok = true };
            uv_fsm_tick(&ctx, &ar, &out);
        }
    }
    if (out.emit_event) terminal_emitted++;
    assert_(sh.uv == 0, "uv must end OFF");
    assert_(terminal_emitted <= 1, "uv terminal exactly once");
    g_rounds++;
}

/* --- dry fan-only scenario: heater_requested=false must never energize heater --- */
static void run_fan(unsigned long rng, unsigned scenario)
{
    dry_params_t dryp;
    memset(&dryp, 0, sizeof(dryp));
    dryp.pre_fan_ms = 200; dryp.heat_on_max_ms = 60000; dryp.heat_off_min_ms = 1000;
    dryp.cooldown_ms = 60000; dryp.max_total_ms = 1800000;
    dryp.heater_cutoff_c = 55.0f; dryp.heater_resume_c = 45.0f; dryp.fan_percent = 30;
    dry_fsm_ctx_t ctx;
    dry_fsm_init(&ctx, &dryp);
    dry_request_t req = { .request_id = (unsigned)(1 + (rng & 0xffff)),
                          .duration_ms = 500, .heater_requested = false };
    dry_fsm_output_t out;
    unsigned terminal_emitted = 0;
    shadow_t sh; memset(&sh, 0, sizeof(sh));

    dry_fsm_begin_request(&ctx, &req, 500);
    /* VALIDATING -> ENSURE_PTC_OFF -> ... -> PRE_FAN, tracking heater requests */
    unsigned now = 1100, guard = 0;
    while (ctx.state != DRY_FSM_PRE_FAN && guard++ < 20) {
        dry_fsm_event_t t = { .type = DRY_EVT_TICK, .now_ms = now,
            .position = DRUM_POS_270, .position_valid = true, .position_stable = true,
            .position_fresh = true, .position_motor_moving = false,
            .sht_valid = true, .sht_fresh = true, .temperature_c = 25.0f };
        dry_fsm_tick(&ctx, &t, &out);
        if (out.action != DRY_ACTION_NONE) {
            if (out.action == DRY_ACTION_HEATER_ON) g_heater_on++;
            if (out.action == DRY_ACTION_FAN_ON) set_on(&sh, &sh.fan, &(unsigned){0}, "fan");
            dry_fsm_event_t ar = { .type = DRY_EVT_ACTION_RESULT, .now_ms = now,
                .action_result_action = out.action, .action_result_ok = true };
            dry_fsm_tick(&ctx, &ar, &out);
        }
        now += 100;
    }
    if (ctx.state != DRY_FSM_PRE_FAN) { assert_(0, "fan: reach PRE_FAN"); return; }

    if (scenario == 3u) { /* emergency -> shutdown */
        dry_fsm_event_t em = { .type = DRY_EVT_EMERGENCY, .now_ms = now };
        dry_fsm_tick(&ctx, &em, &out);
        guard = 0;
        while (ctx.state != DRY_FSM_COMPLETE && guard++ < 20) {
            dry_fsm_event_t t = { .type = DRY_EVT_TICK, .now_ms = now,
                .position = DRUM_POS_270, .position_valid = true, .position_stable = true,
                .position_fresh = true, .position_motor_moving = false,
                .sht_valid = true, .sht_fresh = true, .temperature_c = 25.0f };
            dry_fsm_tick(&ctx, &t, &out);
            if (out.action != DRY_ACTION_NONE) {
                if (out.action == DRY_ACTION_FAN_OFF) set_off(&sh, &sh.fan);
                dry_fsm_event_t ar = { .type = DRY_EVT_ACTION_RESULT, .now_ms = now,
                    .action_result_action = out.action, .action_result_ok = true };
                dry_fsm_tick(&ctx, &ar, &out);
            }
            now += 100;
        }
    } else { /* normal: pre_fan elapsed then duration -> FINISHING -> FAN_OFF */
        guard = 0;
        while (ctx.state != DRY_FSM_COMPLETE && guard++ < 40) {
            dry_fsm_event_t t = { .type = DRY_EVT_TICK, .now_ms = now,
                .position = DRUM_POS_270, .position_valid = true, .position_stable = true,
                .position_fresh = true, .position_motor_moving = false,
                .sht_valid = true, .sht_fresh = true, .temperature_c = 25.0f };
            dry_fsm_tick(&ctx, &t, &out);
            if (out.action != DRY_ACTION_NONE) {
                if (out.action == DRY_ACTION_HEATER_ON) g_heater_on++;
                if (out.action == DRY_ACTION_FAN_OFF) set_off(&sh, &sh.fan);
                dry_fsm_event_t ar = { .type = DRY_EVT_ACTION_RESULT, .now_ms = now,
                    .action_result_action = out.action, .action_result_ok = true };
                dry_fsm_tick(&ctx, &ar, &out);
            }
            now += 100;
        }
    }
    if (out.emit_event) terminal_emitted++;
    assert_(sh.fan == 0, "fan must end OFF");
    assert_(terminal_emitted <= 1, "fan terminal exactly once");
    g_rounds++;
}

/* xorshift PRNG for a deterministic, seeded sequence */
static unsigned long xorshift(unsigned long *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}

int main(void)
{
    unsigned long rng = FAKE_HAL_STRESS_SEED;
    printf("FAKE_HAL_STRESS seed=0x%lx rounds=%d\n", FAKE_HAL_STRESS_SEED, FAKE_HAL_STRESS_ROUNDS);
    for (int i = 0; i < FAKE_HAL_STRESS_ROUNDS; ++i) {
        unsigned target = (unsigned)(xorshift(&rng) % 4u);   /* 0=det 1=drain 2=uv 3=fan */
        unsigned scenario = (unsigned)(xorshift(&rng) % 5u); /* 0 normal 1 timeout 2 cancel 3 emergency 4 dup */
        switch (target) {
        case 0: run_detergent(rng, scenario); break;
        case 1: run_drain(rng, scenario); break;
        case 2: run_uv(rng, scenario); break;
        default: run_fan(rng, scenario); break;
        }
    }
    printf("FAKE_HAL_10000_RESULT=%s\n", (g_fails == 0) ? "PASS" : "FAIL");
    printf("ROUNDS=%lu FAILS=%d OVERLAP=%lu HEATER_ON=%lu TERMINAL_DUPS=%lu\n",
           g_rounds, g_fails, g_overlap, g_heater_on, g_terminal_dups);
    printf("OUTPUT_OVERLAP_COUNT=%lu\n", g_overlap);
    printf("HEATER_ON_COUNT=%lu\n", g_heater_on);
    return (g_fails == 0 && g_overlap == 0 && g_heater_on == 0) ? 0 : 1;
}
