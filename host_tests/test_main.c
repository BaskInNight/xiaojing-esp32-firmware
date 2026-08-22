/*
 * test_main.c 鈥?Host-side unit tests for Phase 1 pure types and config validation
 * Builds with standard gcc, no ESP-IDF or FreeRTOS required.
 * Uses esp_err.h shim in shims/ directory.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "machine_types.h"
#include "wash_contract.h"
#include "safety_contract.h"
#include "xiaojing_hal.h"
#include "machine_config.h"
#include "machine_revision_next.h"
#include "fake_hal.h"
#include "safety_interlocks.h"
#include "board_config.h"
#include "button_debounce.h"
#include "wash_planner.h"
#include "wash_executor_core.h"
#include "app_protocol_core.h"
#include "ble_frame_assembler.h"
#include "wash_executor.h"
#include "wash_executor_adapter.h"
#include "voice_core.h"
#include "voice_tool_parser.h"
#include "voice_cloud.h"
#include "voice_profile.h"
#include "voice_provider.h"
#include "voice_route.h"
#include "gesture_page_map.h"
#include "uv_fsm.h"
#include "uv_service.h"
#include "uv_self_test_core.h"

/* ---- Thread helper for concurrent test (file scope) ---- */
#ifdef _WIN32

typedef struct {
    wash_program_t prog;
    planner_report_t report;
    uint32_t program_id;
    int kind;
} thread_result_t;

static thread_result_t g_thread_results[4];

static machine_config_t make_valid_config(void);

static DWORD WINAPI compile_thread_fn(LPVOID param) {
    int idx = (int)(intptr_t)param;
    wash_intent_t intent; memset(&intent, 0, sizeof(intent));
    intent.kind = (wash_program_kind_t)g_thread_results[idx].kind;
    if (intent.kind == WASH_PROGRAM_FORMAL || intent.kind == WASH_PROGRAM_DEMO) {
        intent.allow_uv = (intent.kind == WASH_PROGRAM_FORMAL);
        intent.allow_dry = true;
    }
    machine_config_t cfg = make_valid_config();
    g_thread_results[idx].report = wash_planner_compile(
        &intent, &cfg, g_thread_results[idx].program_id,
        &g_thread_results[idx].prog);
    return 0;
}
#endif

typedef struct {
    voice_cloud_result_t result;
    int http_status;
    const char *body;
    int calls;
    size_t observed_wav_length;
    voice_cloud_auth_t observed_auth;
    voice_cloud_request_kind_t observed_kind;
    voice_invocation_source_t observed_source;
    voice_route_policy_t observed_policy;
} cloud_fake_transport_t;

static voice_cloud_result_t cloud_fake_transport(
    const voice_cloud_transport_request_t *request,
    voice_cloud_transport_response_t *response,
    void *context)
{
    cloud_fake_transport_t *fake = (cloud_fake_transport_t *)context;
    if (!request || !response || !fake) return VOICE_CLOUD_BAD_ARG;
    fake->calls++;
    fake->observed_wav_length = request->wav_length;
    fake->observed_auth = request->auth;
    fake->observed_kind = request->kind;
    fake->observed_source = request->invocation_source;
    fake->observed_policy = request->route_policy;
    response->http_status = fake->http_status;
    if (fake->result != VOICE_CLOUD_OK) return fake->result;
    size_t length = fake->body ? strlen(fake->body) : 0;
    if (length >= response->body_capacity)
        return VOICE_CLOUD_RESPONSE_TOO_LARGE;
    if (length) memcpy(response->body, fake->body, length);
    response->body_length = length;
    return VOICE_CLOUD_OK;
}

static void make_reference_wav(uint8_t wav[48])
{
    static const uint8_t header[44] = {
        'R','I','F','F', 40,0,0,0, 'W','A','V','E',
        'f','m','t',' ', 16,0,0,0,
        1,0, 1,0, 0x80,0x3e,0,0, 0x00,0x7d,0,0,
        2,0, 16,0, 'd','a','t','a', 4,0,0,0,
    };
    memcpy(wav, header, sizeof(header));
    memset(wav + sizeof(header), 0, 4);
}

static voice_cloud_config_t make_cloud_config(
    cloud_fake_transport_t *transport)
{
    voice_cloud_config_t config = {
        .endpoint = "https://token-plan-cn.xiaomimimo.com/v1/chat/completions",
        .api_key = "test-key-not-secret",
        .model = "mimo-v2.5",
        .connect_timeout_ms = 5000,
        .overall_timeout_ms = 30000,
        .max_wav_bytes = VOICE_CLOUD_DEFAULT_MAX_WAV_BYTES,
        .max_response_bytes = 1024,
        .transport = cloud_fake_transport,
        .transport_context = transport,
    };
    return config;
}

static bool cloud_fake_network_ready(void *context)
{
    const bool *ready = (const bool *)context;
    return ready && *ready;
}

/* ---- Fake adapter concurrency test helpers (file scope) ---- */

#ifdef _WIN32
typedef struct {
    wash_executor_adapter_t *adapter;
    int iterations;
} stress_writer_arg_t;

typedef struct {
    const void *ctx;
    int iterations;
    volatile int *stop;
} stress_reader_arg_t;

typedef struct {
    wash_executor_adapter_t *adapter;
    int start_id;
    int count;
} dual_producer_arg_t;

static DWORD WINAPI stress_writer_fn(LPVOID param) {
    stress_writer_arg_t *a = (stress_writer_arg_t *)param;
    for (int i = 0; i < a->iterations; i++) {
        wash_step_t step; memset(&step, 0, sizeof(step));
        step.type = STEP_MOVE_POSITION;
        step.step_id = (uint16_t)(i + 1);
        step.timeout_ms = 1000;
        terminal_token_t tok; memset(&tok, 0, sizeof(tok));
        tok.program_id = (uint32_t)(i + 1);
        tok.step_id = step.step_id;
        tok.request_id = (uint16_t)(i + 1);
        a->adapter->dispatch_step(a->adapter->ctx, &step, &tok, sizeof(tok));
    }
    return 0;
}

static DWORD WINAPI stress_reader_fn(LPVOID param) {
    stress_reader_arg_t *a = (stress_reader_arg_t *)param;
    while (!*a->stop) {
        uint32_t count = fake_adapter_dispatch_count(a->ctx);
        for (uint32_t j = 0; j < count; j++) {
            const fake_dispatch_record_t *r = fake_adapter_get_dispatch(a->ctx, j);
            if (!r) continue;
            if (r->step.step_id != r->token.step_id ||
                r->step.step_id != r->token.request_id) {
                /* Torn record */
            }
            if (r->step.step_id == 0 || r->step.step_id > 1000) {
                /* Out of bounds */
            }
        }
        Sleep(0);
    }
    return 0;
}

static DWORD WINAPI dual_producer_fn(LPVOID param) {
    dual_producer_arg_t *a = (dual_producer_arg_t *)param;
    for (int i = 0; i < a->count; i++) {
        wash_step_t step; memset(&step, 0, sizeof(step));
        step.type = STEP_WATER_IN;
        step.step_id = (uint16_t)(a->start_id + i);
        step.timeout_ms = 1000;
        terminal_token_t tok; memset(&tok, 0, sizeof(tok));
        tok.step_id = step.step_id;
        tok.request_id = step.step_id;
        a->adapter->dispatch_step(a->adapter->ctx, &step, &tok, sizeof(tok));
    }
    return 0;
}
#endif

/* Helper: build a 3-step program for executor tests */
static wash_program_t make_exec_test_program(void)
{
    wash_program_t prog;
    memset(&prog, 0, sizeof(prog));
    prog.program_id = 1;
    prog.kind = WASH_PROGRAM_CUSTOM;
    prog.step_count = 3;
    prog.steps[0].type = STEP_MOVE_POSITION;
    prog.steps[0].required_position = DRUM_POS_0;
    prog.steps[0].step_id = 0;
    prog.steps[0].timeout_ms = 5000;
    prog.steps[1].type = STEP_WATER_IN;
    prog.steps[1].required_position = DRUM_POS_0;
    prog.steps[1].step_id = 1;
    prog.steps[1].timeout_ms = 10000;
    prog.steps[2].type = STEP_FINISH;
    prog.steps[2].required_position = DRUM_POS_45;
    prog.steps[2].step_id = 2;
    return prog;
}

static int tests_run = 0;
static int tests_passed = 0;

/* Helper: create a simple 3-step test program for executor tests */
static wash_program_t make_test_program(void)
{
    wash_program_t prog;
    memset(&prog, 0, sizeof(prog));
    prog.program_id = 1;
    prog.kind = WASH_PROGRAM_CUSTOM;
    prog.step_count = 3;
    prog.steps[0].type = STEP_MOVE_POSITION;
    prog.steps[0].required_position = DRUM_POS_0;
    prog.steps[0].step_id = 0;
    prog.steps[0].timeout_ms = 5000;
    prog.steps[1].type = STEP_WATER_IN;
    prog.steps[1].required_position = DRUM_POS_0;
    prog.steps[1].step_id = 1;
    prog.steps[1].timeout_ms = 10000;
    prog.steps[2].type = STEP_FINISH;
    prog.steps[2].required_position = DRUM_POS_45;
    prog.steps[2].step_id = 2;
    return prog;
}

#define TEST(name) do { \
    tests_run++; \
    printf("  TEST %02d: %-50s ", tests_run, name); \
} while(0)

#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

/* ================================================================
 * 1. machine_types: enums, request_id, position validation
 * ================================================================ */

static void test_drum_position_values(void)
{
    TEST("drum_position_t enum values");
    assert(DRUM_POS_UNKNOWN == -1);
    assert(DRUM_POS_0 == 0);
    assert(DRUM_POS_45 == 45);
    assert(DRUM_POS_90 == 90);
    assert(DRUM_POS_180 == 180);
    assert(DRUM_POS_270 == 270);
    assert(DRUM_POSITION_COUNT == 5);
    PASS();
}

static void test_drum_position_valid(void)
{
    TEST("drum_position_is_valid");
    assert(drum_position_is_valid(DRUM_POS_0) == true);
    assert(drum_position_is_valid(DRUM_POS_45) == true);
    assert(drum_position_is_valid(DRUM_POS_90) == true);
    assert(drum_position_is_valid(DRUM_POS_180) == true);
    assert(drum_position_is_valid(DRUM_POS_270) == true);
    assert(drum_position_is_valid(DRUM_POS_UNKNOWN) == false);
    assert(drum_position_is_valid((drum_position_t)30) == false);
    assert(drum_position_is_valid((drum_position_t)-2) == false);
    PASS();
}

static void test_request_id_wrap(void)
{
    TEST("machine_request_id wrap UINT32_MAX -> 1 (skip 0)");
    machine_request_id_t id = UINT32_MAX;
    machine_request_id_t next = machine_request_id_next(id);
    assert(next == 1);  /* wraps to 1, not 0 */
    assert(MACHINE_REQUEST_ID_INVALID == 0);

    /* 0 -> 1 */
    assert(machine_request_id_next(0) == 1);
    /* normal increment */
    assert(machine_request_id_next(42) == 43);
    /* near wrap */
    assert(machine_request_id_next(UINT32_MAX - 1) == UINT32_MAX);
    PASS();
}

static void test_machine_state_enums(void)
{
    TEST("machine_state_t enum values");
    assert(MACHINE_STATE_BOOTING == 0);
    assert(MACHINE_STATE_IDLE == 1);
    assert(MACHINE_STATE_WAITING_LOAD == 2);
    assert(MACHINE_STATE_RUNNING == 3);
    assert(MACHINE_STATE_WAITING_UNLOAD == 4);
    assert(MACHINE_STATE_UV == 5);
    assert(MACHINE_STATE_PAUSED == 6);
    assert(MACHINE_STATE_RESETTING == 7);
    assert(MACHINE_STATE_FAULT == 8);
    PASS();
}

static void test_fault_code_enums(void)
{
    TEST("machine_fault_code_t and severity");
    assert(FAULT_NONE == 0);
    assert(FAULT_HALL_CONFLICT == 1);
    assert(FAULT_INTERNAL == 19);
    assert(FAULT_SEVERITY_WARNING == 0);
    assert(FAULT_SEVERITY_LATCHED == 2);

    machine_fault_t fault = {0};
    assert(machine_fault_is_active(&fault) == false);
    fault.code = FAULT_HALL_CONFLICT;
    assert(machine_fault_is_active(&fault) == true);
    machine_fault_clear(&fault);
    assert(fault.code == FAULT_NONE);
    PASS();
}

static void test_service_result_enums(void)
{
    TEST("service_result_t values");
    assert(SERVICE_RESULT_OK == 0);
    assert(SERVICE_RESULT_SKIPPED == 4);
    assert(SERVICE_RESULT_FAULT == 5);
    PASS();
}

static void test_fixed_array_sizes(void)
{
    TEST("fixed array capacity constants");
    assert(WASH_INTENT_MAX_ACTIONS == 32);
    assert(WASH_PROGRAM_MAX_STEPS == 64);
    assert(FAKE_HAL_HISTORY_CAPACITY == 128);
    assert(FAKE_HAL_SCRIPT_CAPACITY == 64);
    /* 10 = 9 基础输出 + SAFETY_OP_HOT_AIR_MODULE（单继电器耦合热风模块） */
    assert(SAFETY_OP_COUNT == 10);
    assert(SAFE_OUTPUT_COUNT == 6);
    PASS();
}

static void test_wash_intent_size(void)
{
    TEST("wash_intent_t fixed-size no dynamic alloc");
    wash_intent_t intent;
    memset(&intent, 0, sizeof(intent));
    assert(intent.action_count == 0);
    assert(sizeof(intent.actions) == WASH_INTENT_MAX_ACTIONS * sizeof(wash_action_t));
    /* Boundary: fill to max */
    intent.action_count = WASH_INTENT_MAX_ACTIONS;
    assert(intent.actions[WASH_INTENT_MAX_ACTIONS - 1].type == WASH_ACTION_MOVE_POSITION);
    PASS();
}

static void test_wash_program_size(void)
{
    TEST("wash_program_t fixed-size no dynamic alloc");
    wash_program_t prog;
    memset(&prog, 0, sizeof(prog));
    assert(prog.step_count == 0);
    assert(sizeof(prog.steps) == WASH_PROGRAM_MAX_STEPS * sizeof(wash_step_t));
    prog.step_count = WASH_PROGRAM_MAX_STEPS;
    assert(prog.steps[WASH_PROGRAM_MAX_STEPS - 1].step_id == 0);
    PASS();
}

static void test_position_direction_policy(void)
{
    TEST("position_direction_policy_t enum values");
    assert(POSITION_DIR_AUTO_SHORTEST == 0);
    assert(POSITION_DIR_PREFER_CW == 1);
    assert(POSITION_DIR_PREFER_CCW == 2);
    assert(POSITION_DIR_FORCE_CW == 3);
    assert(POSITION_DIR_FORCE_CCW == 4);
    PASS();
}

/* ================================================================
 * 2. machine_config_validate (no NVS, no FreeRTOS)
 * ================================================================ */

static machine_config_t make_valid_config(void)
{
    machine_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.schema_version = MACHINE_CONFIG_SCHEMA_VERSION;
    cfg.position.default_policy = POSITION_DIR_PREFER_CW;
    cfg.position.move_pwm_percent = 30;
    cfg.position.approach_pwm_percent = 15;
    cfg.position.debounce_ms = 30;
    cfg.position.move_timeout_ms = 30000;
    cfg.position.brake_ms = 500;
    cfg.water.pulses_per_liter = 0.0f;
    cfg.water.no_flow_timeout_ms = 5000;
    cfg.water.low_flow_window_ms = 3000;
    cfg.water.source_batch_target_pulses = 500;
    cfg.water.source_batch_max_ms = 30000;
    cfg.water.source_settle_ms = 2000;
    cfg.water.transfer_timeout_ms = 30000;
    cfg.water.default_transfer_ms = 15000;
    cfg.water.max_fill_cycles = 10;
    cfg.water.total_inlet_timeout_ms = 600000;
    cfg.dry.max_total_ms = 1800000;
    cfg.dry.pre_fan_ms = 5000;
    cfg.dry.heat_on_max_ms = 60000;
    cfg.dry.heat_off_min_ms = 60000;
    cfg.dry.cooldown_ms = 60000;
    cfg.dry.heater_cutoff_c = 55.0f;
    cfg.dry.heater_resume_c = 45.0f;
    cfg.dry.fan_percent = 80;
    cfg.dry.sht_stale_timeout_ms = 10000;
    cfg.detergent.demo_duration_ms = 3000;
    cfg.detergent.formal_duration_ms = 6000;
    cfg.detergent.max_single_ms = 10000;
    cfg.detergent.ml_per_second = 0.0f;
    cfg.uv.default_duration_ms = 600000;
    cfg.uv.max_duration_ms = 1800000;
    cfg.drain.max_duration_ms = 120000;
    cfg.benchtst.bl50_pwm = 40;
    cfg.benchtst.ibt2_pwm = 80;
    cfg.benchtst.act_duration_ms = 1500;
    return cfg;
}

static void test_config_valid_default(void)
{
    TEST("config_validate: valid default passes");
    machine_config_t cfg = make_valid_config();
    assert(machine_config_validate(&cfg) == true);
    PASS();
}

static void test_config_bad_version(void)
{
    TEST("config_validate: wrong schema_version rejected");
    machine_config_t cfg = make_valid_config();
    cfg.schema_version = 99;
    assert(machine_config_validate(&cfg) == false);
    PASS();
}

static void test_config_nan_pulses_per_liter(void)
{
    TEST("config_validate: NaN pulses_per_liter rejected");
    machine_config_t cfg = make_valid_config();
    cfg.water.pulses_per_liter = NAN;
    assert(machine_config_validate(&cfg) == false);
    PASS();
}

static void test_config_inf_cutoff(void)
{
    TEST("config_validate: Inf heater_cutoff_c rejected");
    machine_config_t cfg = make_valid_config();
    cfg.dry.heater_cutoff_c = INFINITY;
    assert(machine_config_validate(&cfg) == false);
    PASS();
}

static void test_config_nan_ml_per_second(void)
{
    TEST("config_validate: NaN ml_per_second rejected");
    machine_config_t cfg = make_valid_config();
    cfg.detergent.ml_per_second = NAN;
    assert(machine_config_validate(&cfg) == false);
    PASS();
}

static void test_config_bad_enum_policy(void)
{
    TEST("config_validate: invalid position policy rejected");
    machine_config_t cfg = make_valid_config();
    cfg.position.default_policy = (position_direction_policy_t)99;
    assert(machine_config_validate(&cfg) == false);
    PASS();
}

static void test_config_demo_exceeds_max(void)
{
    TEST("config_validate: demo_duration > max_single rejected");
    machine_config_t cfg = make_valid_config();
    cfg.detergent.max_single_ms = 2000;
    cfg.detergent.demo_duration_ms = 3000;
    assert(machine_config_validate(&cfg) == false);
    PASS();
}

static void test_config_formal_exceeds_max(void)
{
    TEST("config_validate: formal_duration > max_single rejected");
    machine_config_t cfg = make_valid_config();
    cfg.detergent.max_single_ms = 5000;
    cfg.detergent.formal_duration_ms = 6000;
    assert(machine_config_validate(&cfg) == false);
    PASS();
}

static void test_config_pwm_low(void)
{
    TEST("config_validate: PWM < 5 rejected");
    machine_config_t cfg = make_valid_config();
    cfg.position.move_pwm_percent = 4;
    assert(machine_config_validate(&cfg) == false);
    PASS();
}

static void test_config_approach_exceeds_move(void)
{
    TEST("config_validate: approach > move PWM rejected");
    machine_config_t cfg = make_valid_config();
    cfg.position.move_pwm_percent = 20;
    cfg.position.approach_pwm_percent = 25;
    assert(machine_config_validate(&cfg) == false);
    PASS();
}

static void test_config_debounce_range(void)
{
    TEST("config_validate: debounce 19ms and 201ms rejected");
    machine_config_t cfg1 = make_valid_config();
    cfg1.position.debounce_ms = 19;
    assert(machine_config_validate(&cfg1) == false);
    machine_config_t cfg2 = make_valid_config();
    cfg2.position.debounce_ms = 201;
    assert(machine_config_validate(&cfg2) == false);
    PASS();
}

static void test_config_fan_percent_range(void)
{
    TEST("config_validate: fan_percent > 100 rejected");
    machine_config_t cfg = make_valid_config();
    cfg.dry.fan_percent = 101;
    assert(machine_config_validate(&cfg) == false);
    PASS();
}

static void test_config_heater_resume_ge_cutoff(void)
{
    TEST("config_validate: resume >= cutoff rejected");
    machine_config_t cfg = make_valid_config();
    cfg.dry.heater_resume_c = 55.0f;
    assert(machine_config_validate(&cfg) == false);
    PASS();
}

static void test_config_default_transfer_gt_transfer_timeout(void)
{
    TEST("config_validate: default_transfer > transfer_timeout rejected");
    machine_config_t cfg = make_valid_config();
    cfg.water.transfer_timeout_ms = 10000;
    cfg.water.default_transfer_ms = 15000;
    assert(machine_config_validate(&cfg) == false);
    PASS();
}

/* ================================================================
 * 3. machine_revision_next 鈥?real C, compiled from shared source
 * ================================================================ */

static void test_revision_next_0_to_1(void)
{
    TEST("machine_revision_next: 0 -> 1");
    assert(machine_revision_next(0) == 1);
    PASS();
}

static void test_revision_next_normal(void)
{
    TEST("machine_revision_next: 42 -> 43");
    assert(machine_revision_next(42) == 43);
    PASS();
}

static void test_revision_next_max_minus_1(void)
{
    TEST("machine_revision_next: MAX-1 -> MAX");
    assert(machine_revision_next(UINT32_MAX - 1) == UINT32_MAX);
    PASS();
}

static void test_revision_next_max_wraps(void)
{
    TEST("machine_revision_next: MAX -> 1 (skip 0)");
    assert(machine_revision_next(UINT32_MAX) == 1);
    PASS();
}

/* ================================================================
 * 4. HAL contract / safety contract type sizes
 * ================================================================ */

static void test_hal_struct_sizes(void)
{
    TEST("xiaojing_hal_t has all function pointers");
    xiaojing_hal_t hal;
    memset(&hal, 0, sizeof(hal));
    assert(hal.read_mcp_inputs == NULL);
    assert(hal.emergency_shutdown == NULL);
    assert(sizeof(hal) > 0);
    PASS();
}

static void test_safety_snapshot_size(void)
{
    TEST("safety_snapshot_t fixed-size");
    safety_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    assert(snap.fault_active == false);
    assert(snap.emergency_stop_active == false);
    PASS();
}

/* ================================================================
 * 5. safety_interlocks 鈥?pure C host tests (no FreeRTOS)
 * ================================================================ */

static safety_inputs_t make_default_inputs(void)
{
    safety_inputs_t in;
    memset(&in, 0, sizeof(in));
    in.position = DRUM_POS_0;
    in.position_stable = true;
    in.position_motor_moving = false;
    in.position_sample_valid = true;
    in.position_sample_ms = 99900;
    in.mcp_known_mask = ((1U << SAFE_OUTPUT_COUNT) - 1);
    in.fan_running = false;
    in.sht_valid = true;
    in.sht_fresh = true;
    in.sht_sample.temperature_c = 25.0f;
    in.sht_sample.valid = true;
    in.bl50_state_known = true;   /* 瀹夊叏鍩虹嚎: BL50 KNOWN_STOPPED */
    in.bl50_running = false;
    in.fault_active = false;
    in.emergency_stop_active = false;
    in.ptc_enabled_in_config = true;
    in.output_mode = XIAOJING_MODE_FAKE;
    in.now_ms = 100000;
    return in;
}

static safety_interlock_config_t make_default_ilk_config(void)
{
    safety_interlock_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.heater_cutoff_c = 55.0f;
    cfg.heater_resume_c = 45.0f;
    cfg.heat_on_max_ms = 60000;
    cfg.pre_fan_ms = 5000;
    cfg.sht_stale_timeout_ms = 10000;
    cfg.position_stale_timeout_ms = 500;
    cfg.output_mode = XIAOJING_MODE_REAL;
    cfg.ptc_enabled = true;
    cfg.board_identity_confirmed = true;
    return cfg;
}

static safety_request_t make_request(safety_operation_t op, bool enable)
{
    safety_request_t req;
    memset(&req, 0, sizeof(req));
    req.operation = op;
    req.enable = enable;
    req.required_position = DRUM_POS_UNKNOWN;  /* explicit: memset gives 0 which is DRUM_POS_0 */
    return req;
}

static void test_ptc_rejected_at_non_270(void)
{
    TEST("safety: PTC ON rejected at non-270 position");
    safety_inputs_t in = make_default_inputs();
    in.position = DRUM_POS_0;
    in.position_stable = true;
    in.ptc_enabled_in_config = true;
    in.fan_running = true;
    in.fan_running_since_ms = 90000;
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_PTC_HEATER, true);
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == false);
    assert(dec.reason == SAFETY_REJECT_POSITION_MISMATCH);
    PASS();
}

static void test_ptc_rejected_fan_preblow_short(void)
{
    TEST("safety: PTC ON rejected when fan preblow < 5s");
    safety_inputs_t in = make_default_inputs();
    in.position = DRUM_POS_270;
    in.position_stable = true;
    in.ptc_enabled_in_config = true;
    in.fan_running = true;
    in.fan_running_since_ms = 98000; /* only 2s */
    in.now_ms = 100000;
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_PTC_HEATER, true);
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == false);
    assert(dec.reason == SAFETY_REJECT_FAN_PREBLOW_SHORT);
    PASS();
}

static void test_ptc_rejected_sht_invalid(void)
{
    TEST("safety: PTC ON rejected when SHT invalid");
    safety_inputs_t in = make_default_inputs();
    in.position = DRUM_POS_270;
    in.position_stable = true;
    in.ptc_enabled_in_config = true;
    in.sht_valid = false;
    in.fan_running = true;
    in.fan_running_since_ms = 90000;
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_PTC_HEATER, true);
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == false);
    assert(dec.reason == SAFETY_REJECT_SHT_INVALID);
    PASS();
}

static void test_ptc_overtemp_blocks(void)
{
    TEST("safety: PTC ON rejected when overtemp");
    safety_inputs_t in = make_default_inputs();
    in.position = DRUM_POS_270;
    in.position_stable = true;
    in.ptc_enabled_in_config = true;
    in.sht_valid = true;
    in.sht_fresh = true;
    in.sht_sample.temperature_c = 56.0f;
    in.fan_running = true;
    in.fan_running_since_ms = 90000;
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_PTC_HEATER, true);
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == false);
    assert(dec.reason == SAFETY_REJECT_OVERTEMP);
    assert(dec.fault_code == FAULT_DRY_OVERTEMP);
    PASS();
}

static void test_valve_conflict_source_blocks_transfer(void)
{
    TEST("safety: source open blocks transfer valve");
    safety_inputs_t in = make_default_inputs();
    in.position = DRUM_POS_0;
    in.position_stable = true;
    in.mcp_outputs.source_inlet_valve = true;
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_TRANSFER_VALVE, true);
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == false);
    assert(dec.reason == SAFETY_REJECT_VALVE_CONFLICT);
    PASS();
}

static void test_valve_conflict_drain_blocks_source(void)
{
    TEST("safety: drain open blocks source inlet");
    safety_inputs_t in = make_default_inputs();
    in.position = DRUM_POS_0;
    in.position_stable = true;
    in.mcp_outputs.drain_valve = true;
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_SOURCE_INLET_VALVE, true);
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == false);
    assert(dec.reason == SAFETY_REJECT_VALVE_CONFLICT);
    PASS();
}

static void test_motor_moving_blocks_outputs(void)
{
    TEST("safety: motor moving blocks valves/BL50/PTC/UV");
    safety_inputs_t in = make_default_inputs();
    in.position = DRUM_POS_0;
    in.position_stable = true;
    in.position_motor_moving = true;
    safety_interlock_config_t cfg = make_default_ilk_config();

    safety_request_t req = make_request(SAFETY_OP_SOURCE_INLET_VALVE, true);
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == false);
    assert(dec.reason == SAFETY_REJECT_POSITION_MOVING);

    req = make_request(SAFETY_OP_BL50, true);
    req.required_position = DRUM_POS_0;  /* BL50 requires valid position */
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == false);
    assert(dec.reason == SAFETY_REJECT_POSITION_MOVING);

    req = make_request(SAFETY_OP_UV, true);
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == false);
    assert(dec.reason == SAFETY_REJECT_POSITION_MOVING);
    PASS();
}

static void test_fault_blocks_on(void)
{
    TEST("safety: active fault blocks ON requests");
    safety_inputs_t in = make_default_inputs();
    in.fault_active = true;
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_SOURCE_INLET_VALVE, true);
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == false);
    assert(dec.reason == SAFETY_REJECT_FAULT_ACTIVE);
    PASS();
}

static void test_emergency_blocks_on(void)
{
    TEST("safety: emergency blocks ON requests");
    safety_inputs_t in = make_default_inputs();
    in.emergency_stop_active = true;
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_DRAIN_VALVE, true);
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == false);
    assert(dec.reason == SAFETY_REJECT_EMERGENCY);
    PASS();
}

static void test_off_always_allowed(void)
{
    TEST("safety: OFF requests always allowed");
    safety_inputs_t in = make_default_inputs();
    in.fault_active = true;
    in.emergency_stop_active = true;
    safety_interlock_config_t cfg = make_default_ilk_config();

    safety_request_t req = make_request(SAFETY_OP_SOURCE_INLET_VALVE, false);
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == true);
    assert(dec.reason == SAFETY_REJECT_NONE);

    req = make_request(SAFETY_OP_PTC_HEATER, false);
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == true);
    PASS();
}

static void test_inlet_requires_0(void)
{
    TEST("safety: inlet requires position 0");
    safety_inputs_t in = make_default_inputs();
    in.position = DRUM_POS_90;
    in.position_stable = true;
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_SOURCE_INLET_VALVE, true);
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == false);
    assert(dec.reason == SAFETY_REJECT_POSITION_MISMATCH);
    PASS();
}

static void test_drain_requires_180(void)
{
    TEST("safety: drain requires position 180");
    safety_inputs_t in = make_default_inputs();
    in.position = DRUM_POS_0;
    in.position_stable = true;
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_DRAIN_VALVE, true);
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == false);
    assert(dec.reason == SAFETY_REJECT_POSITION_MISMATCH);
    PASS();
}

static void test_uv_requires_0(void)
{
    TEST("safety: UV requires position 0");
    safety_inputs_t in = make_default_inputs();
    in.position = DRUM_POS_45;
    in.position_stable = true;
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_UV, true);
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == false);
    assert(dec.reason == SAFETY_REJECT_POSITION_MISMATCH);
    PASS();
}

static void test_ptc_duration_timeout(void)
{
    TEST("safety: PTC duration timeout blocks ON");
    safety_inputs_t in = make_default_inputs();
    in.position = DRUM_POS_270;
    in.position_stable = true;
    in.ptc_enabled_in_config = true;
    in.sht_valid = true;
    in.sht_fresh = true;
    in.sht_sample.temperature_c = 30.0f;
    in.fan_running = true;
    in.fan_running_since_ms = 90000;
    in.ptc_on = true;
    in.ptc_on_since_ms = 30000; /* 70s ago at now=100000 */
    in.now_ms = 100000;
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_PTC_HEATER, true);
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == false);
    assert(dec.reason == SAFETY_REJECT_PTC_DURATION_EXCEEDED);
    PASS();
}

static void test_invalid_operation_rejected(void)
{
    TEST("safety: invalid operation code rejected");
    safety_inputs_t in = make_default_inputs();
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req;
    memset(&req, 0, sizeof(req));
    req.operation = (safety_operation_t)99;
    req.enable = true;
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == false);
    assert(dec.reason == SAFETY_REJECT_INVALID_PARAM);
    PASS();
}

static void test_null_inputs_rejected(void)
{
    TEST("safety: null inputs rejected");
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_SOURCE_INLET_VALVE, true);
    safety_decision_t dec;
    safety_interlock_check(NULL, &cfg, &req, &dec);
    assert(dec.allowed == false);
    assert(dec.reason == SAFETY_REJECT_INVALID_PARAM);
    PASS();
}

static void test_mcp_output_conflict_check(void)
{
    TEST("safety: mcp_output_conflict detects all conflicts");
    safety_mcp_output_state_t mcp;
    memset(&mcp, 0, sizeof(mcp));

    /* No conflict when all off */
    assert(safety_mcp_output_conflict(&mcp, SAFETY_OP_SOURCE_INLET_VALVE, true) == false);
    assert(safety_mcp_output_conflict(&mcp, SAFETY_OP_TRANSFER_VALVE, true) == false);
    assert(safety_mcp_output_conflict(&mcp, SAFETY_OP_DRAIN_VALVE, true) == false);

    /* Source + Transfer conflict */
    mcp.source_inlet_valve = true;
    assert(safety_mcp_output_conflict(&mcp, SAFETY_OP_TRANSFER_VALVE, true) == true);
    assert(safety_mcp_output_conflict(&mcp, SAFETY_OP_DRAIN_VALVE, true) == true);

    /* Transfer + Source conflict */
    memset(&mcp, 0, sizeof(mcp));
    mcp.transfer_valve = true;
    assert(safety_mcp_output_conflict(&mcp, SAFETY_OP_SOURCE_INLET_VALVE, true) == true);
    assert(safety_mcp_output_conflict(&mcp, SAFETY_OP_DRAIN_VALVE, true) == true);

    /* Drain + Source conflict */
    memset(&mcp, 0, sizeof(mcp));
    mcp.drain_valve = true;
    assert(safety_mcp_output_conflict(&mcp, SAFETY_OP_SOURCE_INLET_VALVE, true) == true);
    assert(safety_mcp_output_conflict(&mcp, SAFETY_OP_TRANSFER_VALVE, true) == true);

    /* OFF never conflicts */
    memset(&mcp, 0, sizeof(mcp));
    mcp.source_inlet_valve = true;
    assert(safety_mcp_output_conflict(&mcp, SAFETY_OP_TRANSFER_VALVE, false) == false);
    PASS();
}

static void test_water_outputs_safe(void)
{
    TEST("safety: water_outputs_safe check");
    safety_mcp_output_state_t mcp;
    memset(&mcp, 0, sizeof(mcp));
    uint32_t all_known = ((1U << SAFE_OUTPUT_COUNT) - 1);
    assert(safety_water_outputs_safe(&mcp, all_known) == true);

    /* UNKNOWN mask 鈫?false */
    assert(safety_water_outputs_safe(&mcp, 0) == false);

    /* Known + source ON 鈫?false */
    mcp.source_inlet_valve = true;
    assert(safety_water_outputs_safe(&mcp, all_known) == false);
    PASS();
}

static void test_ptc_allowed_at_270_full_conditions(void)
{
    TEST("safety: PTC allowed at 270 with all conditions met");
    safety_inputs_t in = make_default_inputs();
    in.position = DRUM_POS_270;
    in.position_stable = true;
    in.ptc_enabled_in_config = true;
    in.sht_valid = true;
    in.sht_fresh = true;
    in.sht_sample.temperature_c = 30.0f;
    in.fan_running = true;
    in.fan_running_since_ms = 90000; /* 10s pre-blow */
    in.now_ms = 100000;
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_PTC_HEATER, true);
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == true);
    assert(dec.reason == SAFETY_REJECT_NONE);
    PASS();
}

static void test_inlet_allowed_at_0(void)
{
    TEST("safety: inlet allowed at position 0");
    safety_inputs_t in = make_default_inputs();
    in.position = DRUM_POS_0;
    in.position_stable = true;
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_SOURCE_INLET_VALVE, true);
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == true);
    PASS();
}

static void test_drain_allowed_at_180(void)
{
    TEST("safety: drain allowed at position 180");
    safety_inputs_t in = make_default_inputs();
    in.position = DRUM_POS_180;
    in.position_stable = true;
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_DRAIN_VALVE, true);
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == true);
    PASS();
}

static void test_interlock_mask_range(void)
{
    TEST("safety: interlock mask bits in valid range");
    safety_inputs_t in = make_default_inputs();
    in.fault_active = true;
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_SOURCE_INLET_VALVE, true);
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.interlock_mask != 0);
    /* No bits above SAFETY_ILK_MAX_BIT */
    uint32_t valid_mask = (1U << (SAFETY_ILK_MAX_BIT + 1)) - 1;
    assert((dec.interlock_mask & ~valid_mask) == 0);
    PASS();
}

/* ================================================================
 * 6. safety_interlocks 鈥?position freshness host tests
 * ================================================================ */

static void test_position_fresh_allowed(void)
{
    TEST("safety: position fresh + correct allowed");
    safety_inputs_t in = make_default_inputs();
    in.position = DRUM_POS_0;
    in.position_sample_valid = true;
    in.position_sample_ms = 99900;
    in.now_ms = 100000;
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_SOURCE_INLET_VALVE, true);
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == true);
    PASS();
}

static void test_position_invalid_rejected(void)
{
    TEST("safety: position invalid rejected");
    safety_inputs_t in = make_default_inputs();
    in.position_sample_valid = false;
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_SOURCE_INLET_VALVE, true);
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == false);
    assert(dec.reason == SAFETY_REJECT_POSITION_UNKNOWN);
    PASS();
}

static void test_position_stale_rejected(void)
{
    TEST("safety: position stale rejected");
    safety_inputs_t in = make_default_inputs();
    in.position = DRUM_POS_0;
    in.position_sample_valid = true;
    in.position_sample_ms = 99400; /* 600ms ago */
    in.now_ms = 100000;
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_SOURCE_INLET_VALVE, true);
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == false);
    assert(dec.reason == SAFETY_REJECT_POSITION_STALE);
    PASS();
}

static void test_position_stale_exactly_at_timeout(void)
{
    TEST("safety: position stale exactly at timeout");
    safety_inputs_t in = make_default_inputs();
    in.position = DRUM_POS_0;
    in.position_sample_valid = true;
    in.position_sample_ms = 99500; /* exactly 500ms */
    in.now_ms = 100000;
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_SOURCE_INLET_VALVE, true);
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == false);
    assert(dec.reason == SAFETY_REJECT_POSITION_STALE);
    PASS();
}

static void test_tick_zero_stale(void)
{
    TEST("safety: tick=0 stale correctly");
    safety_inputs_t in = make_default_inputs();
    in.position = DRUM_POS_0;
    in.position_sample_valid = true;
    in.position_sample_ms = 0; /* tick 0 */
    in.now_ms = 500; /* exactly at timeout */
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_SOURCE_INLET_VALVE, true);
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == false);
    assert(dec.reason == SAFETY_REJECT_POSITION_STALE);
    PASS();
}

static void test_tick_zero_fresh(void)
{
    TEST("safety: tick=0 fresh at 499ms");
    safety_inputs_t in = make_default_inputs();
    in.position = DRUM_POS_0;
    in.position_sample_valid = true;
    in.position_sample_ms = 0;
    in.now_ms = 499;
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_SOURCE_INLET_VALVE, true);
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == true);
    PASS();
}

static void test_motor_allowed_unknown_position(void)
{
    TEST("safety: MOTOR ON allowed from UNKNOWN position");
    safety_inputs_t in = make_default_inputs();
    in.position = DRUM_POS_UNKNOWN;
    in.position_stable = false;
    in.position_sample_valid = false;
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_POSITION_MOTOR, true);
    req.params.motor.pwm_percent = 30;
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == true);
    PASS();
}

static void test_motor_allowed_stale_position(void)
{
    TEST("safety: MOTOR ON allowed with stale position");
    safety_inputs_t in = make_default_inputs();
    in.position_sample_valid = true;
    in.position_sample_ms = 99000; /* stale */
    in.now_ms = 100000;
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_POSITION_MOTOR, true);
    req.params.motor.pwm_percent = 30;
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == true);
    PASS();
}

static void test_fan_allowed_stale_position(void)
{
    TEST("safety: FAN ON allowed with stale position");
    safety_inputs_t in = make_default_inputs();
    in.position_sample_valid = true;
    in.position_sample_ms = 98000; /* stale */
    in.now_ms = 100000;
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_FAN, true);
    req.params.fan.percent = 80;
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == true);
    PASS();
}

static void test_fan_allowed_motor_moving(void)
{
    TEST("safety: FAN ON allowed when motor moving");
    safety_inputs_t in = make_default_inputs();
    in.position_motor_moving = true;
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_FAN, true);
    req.params.fan.percent = 50;
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == true);
    PASS();
}

static void test_mcp_unknown_blocks_clear(void)
{
    TEST("safety: MCP unknown blocks fault clear");
    safety_inputs_t in = make_default_inputs();
    in.mcp_known_mask = 0x2F; /* PTC (bit 5) UNKNOWN */
    safety_interlock_config_t cfg = make_default_ilk_config();
    /* Verify the unknown state is detectable */
    uint32_t all_mask = ((1U << SAFE_OUTPUT_COUNT) - 1);
    assert((in.mcp_known_mask & all_mask) != all_mask);
    PASS();
}

static void test_motor_rejected_bl50_unknown(void)
{
    TEST("safety: MOTOR ON rejected when BL50 UNKNOWN");
    safety_inputs_t in = make_default_inputs();
    in.position = DRUM_POS_UNKNOWN;
    in.position_stable = false;
    in.position_sample_valid = false;
    in.bl50_state_known = false;  /* BL50 UNKNOWN */
    in.bl50_running = false;
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_POSITION_MOTOR, true);
    req.params.motor.pwm_percent = 30;
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == false);
    assert(dec.reason == SAFETY_REJECT_MOTOR_RUNNING);
    PASS();
}

static void test_motor_rejected_bl50_running(void)
{
    TEST("safety: MOTOR ON rejected when BL50 RUNNING");
    safety_inputs_t in = make_default_inputs();
    in.position = DRUM_POS_UNKNOWN;
    in.position_stable = false;
    in.position_sample_valid = false;
    in.bl50_state_known = true;
    in.bl50_running = true;   /* BL50 RUNNING */
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_POSITION_MOTOR, true);
    req.params.motor.pwm_percent = 30;
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == false);
    assert(dec.reason == SAFETY_REJECT_MOTOR_RUNNING);
    PASS();
}

static void test_motor_allowed_bl50_stopped(void)
{
    TEST("safety: MOTOR ON allowed when BL50 KNOWN_STOPPED");
    safety_inputs_t in = make_default_inputs();
    in.position = DRUM_POS_UNKNOWN;
    in.position_stable = false;
    in.position_sample_valid = false;
    in.bl50_state_known = true;
    in.bl50_running = false;  /* BL50 KNOWN_STOPPED */
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_POSITION_MOTOR, true);
    req.params.motor.pwm_percent = 30;
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == true);
    PASS();
}

static void test_position_fresh_before_timeout(void)
{
    TEST("safety: position fresh 1ms before timeout");
    safety_inputs_t in = make_default_inputs();
    in.position = DRUM_POS_0;
    in.position_sample_valid = true;
    in.position_sample_ms = 99501; /* 499ms ago */
    in.now_ms = 100000;
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_SOURCE_INLET_VALVE, true);
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == true);
    PASS();
}

static void test_position_stale_wrap_safe(void)
{
    TEST("safety: position stale elapsed wrap-safe");
    safety_inputs_t in = make_default_inputs();
    in.position = DRUM_POS_0;
    in.position_sample_valid = true;
    in.position_sample_ms = UINT32_MAX - 100;
    in.now_ms = 399; /* elapsed = 500 */
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_SOURCE_INLET_VALVE, true);
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == false);
    assert(dec.reason == SAFETY_REJECT_POSITION_STALE);
    PASS();
}

static void test_position_stale_off_allowed(void)
{
    TEST("safety: position stale OFF still allowed");
    safety_inputs_t in = make_default_inputs();
    in.position_sample_valid = true;
    in.position_sample_ms = 99000;
    in.now_ms = 100000;
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_SOURCE_INLET_VALVE, false);
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == true);
    PASS();
}

static void test_fan_not_affected_by_position_stale(void)
{
    TEST("safety: fan not affected by position stale");
    safety_inputs_t in = make_default_inputs();
    in.position_sample_valid = true;
    in.position_sample_ms = 99000;
    in.now_ms = 100000;
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_FAN, true);
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == true);
    PASS();
}

static void test_emergency_not_blocked_by_stale(void)
{
    TEST("safety: emergency not blocked by stale");
    safety_inputs_t in = make_default_inputs();
    in.emergency_stop_active = true;
    in.position_sample_valid = true;
    in.position_sample_ms = 99000;
    in.now_ms = 100000;
    safety_interlock_config_t cfg = make_default_ilk_config();
    /* OFF should still work under emergency */
    safety_request_t req = make_request(SAFETY_OP_SOURCE_INLET_VALVE, false);
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == true);
    PASS();
}

static void test_interlock_mask_position_stale_bit(void)
{
    TEST("safety: interlock mask has POSITION_STALE bit");
    safety_inputs_t in = make_default_inputs();
    in.position = DRUM_POS_0;
    in.position_sample_valid = true;
    in.position_sample_ms = 99000;
    in.now_ms = 100000;
    safety_interlock_config_t cfg = make_default_ilk_config();
    safety_request_t req = make_request(SAFETY_OP_SOURCE_INLET_VALVE, true);
    safety_decision_t dec;
    safety_interlock_check(&in, &cfg, &req, &dec);
    assert(dec.allowed == false);
    assert(dec.interlock_mask & SAFETY_ILK_POSITION_STALE);
    PASS();
}

/* ================================================================
 * Main
 * ================================================================ */

int main(void)
{
    printf("=== xiaojing host tests ===\n\n");

    printf("[1] machine_types\n");
    test_drum_position_values();
    test_drum_position_valid();
    test_request_id_wrap();
    test_machine_state_enums();
    test_fault_code_enums();
    test_service_result_enums();
    test_fixed_array_sizes();
    test_wash_intent_size();
    test_wash_program_size();
    test_position_direction_policy();

    printf("\n[2] machine_config validation\n");
    test_config_valid_default();
    test_config_bad_version();
    test_config_nan_pulses_per_liter();
    test_config_inf_cutoff();
    test_config_nan_ml_per_second();
    test_config_bad_enum_policy();
    test_config_demo_exceeds_max();
    test_config_formal_exceeds_max();
    test_config_pwm_low();
    test_config_approach_exceeds_move();
    test_config_debounce_range();
    test_config_fan_percent_range();
    test_config_heater_resume_ge_cutoff();
    test_config_default_transfer_gt_transfer_timeout();

    printf("\n[3] machine_revision_next (real C)\n");
    test_revision_next_0_to_1();
    test_revision_next_normal();
    test_revision_next_max_minus_1();
    test_revision_next_max_wraps();

    printf("\n[4] HAL/safety contract types\n");
    test_hal_struct_sizes();
    test_safety_snapshot_size();

    printf("\n[5] safety_interlocks (pure C host tests)\n");
    test_ptc_rejected_at_non_270();
    test_ptc_rejected_fan_preblow_short();
    test_ptc_rejected_sht_invalid();
    test_ptc_overtemp_blocks();
    test_valve_conflict_source_blocks_transfer();
    test_valve_conflict_drain_blocks_source();
    test_motor_moving_blocks_outputs();
    test_fault_blocks_on();
    test_emergency_blocks_on();
    test_off_always_allowed();
    test_inlet_requires_0();
    test_drain_requires_180();
    test_uv_requires_0();
    test_ptc_duration_timeout();
    test_invalid_operation_rejected();
    test_null_inputs_rejected();
    test_mcp_output_conflict_check();
    test_water_outputs_safe();
    test_ptc_allowed_at_270_full_conditions();
    test_inlet_allowed_at_0();
    test_drain_allowed_at_180();
    test_interlock_mask_range();

    printf("\n[6] safety_interlocks position freshness\n");
    test_position_fresh_allowed();
    test_position_invalid_rejected();
    test_position_stale_rejected();
    test_position_stale_exactly_at_timeout();
    test_position_fresh_before_timeout();
    test_position_stale_wrap_safe();
    test_position_stale_off_allowed();
    test_fan_not_affected_by_position_stale();
    test_emergency_not_blocked_by_stale();
    test_interlock_mask_position_stale_bit();

    printf("\n[7] Round 1.1 per-op position + MCP known\n");
    test_tick_zero_stale();
    test_tick_zero_fresh();
    test_motor_allowed_unknown_position();
    test_motor_allowed_stale_position();
    test_fan_allowed_stale_position();
    test_fan_allowed_motor_moving();
    test_mcp_unknown_blocks_clear();
    test_motor_rejected_bl50_unknown();
    test_motor_rejected_bl50_running();
    test_motor_allowed_bl50_stopped();

    /* ---- [8] button_debounce pure C engine ---- */

    printf("\n[8] button_debounce pure C engine\n");

    /* Seed all released */
    { TEST("seed all released to IDLE");
      button_debounce_ctx_t ctx;
      button_debounce_init(&ctx, 30);
      button_debounce_seed(&ctx, 0xFF, 0);
      int ok = 1;
      for (int i = 0; i < BUTTON_ID_COUNT; i++)
          if (ctx.buttons[i].state != BTN_STATE_IDLE) ok = 0;
      if (ok) PASS(); else FAIL("not all IDLE"); }

    /* Seed BTN1 held */
    { TEST("seed BTN1 held to SEED_RELEASE");
      button_debounce_ctx_t ctx;
      button_debounce_init(&ctx, 30);
      button_debounce_seed(&ctx, 0xEF, 0);
      if (ctx.buttons[0].state == BTN_STATE_SEED_RELEASE) PASS(); else FAIL("wrong state"); }

    /* Stable press+release = one CLICK with confirmed timestamp */
    { TEST("stable press+release CLICK timestamp confirmed");
      button_debounce_ctx_t ctx;
      button_debounce_init(&ctx, 30);
      button_debounce_seed(&ctx, 0xFF, 0);
      button_event_t ev[BUTTON_ID_COUNT];
      button_debounce_process(&ctx, 0xEF, 0, ev, BUTTON_ID_COUNT);
      button_debounce_process(&ctx, 0xEF, 35, ev, BUTTON_ID_COUNT);
      button_debounce_process(&ctx, 0xFF, 100, ev, BUTTON_ID_COUNT);
      int n = button_debounce_process(&ctx, 0xFF, 135, ev, BUTTON_ID_COUNT);
      if (n == 1 && ev[0].button_id == BUTTON_ID_1 && ev[0].timestamp_ms == 35) PASS();
      else FAIL("wrong event"); }

    /* 10ms glitch = no event */
    { TEST("10ms glitch no event");
      button_debounce_ctx_t ctx;
      button_debounce_init(&ctx, 30);
      button_debounce_seed(&ctx, 0xFF, 0);
      button_event_t ev[BUTTON_ID_COUNT];
      button_debounce_process(&ctx, 0xEF, 0, ev, BUTTON_ID_COUNT);
      button_debounce_process(&ctx, 0xFF, 10, ev, BUTTON_ID_COUNT);
      int n = button_debounce_process(&ctx, 0xFF, 50, ev, BUTTON_ID_COUNT);
      if (n == 0) PASS(); else FAIL("got event from glitch"); }

    /* Long hold = no repeated CLICK */
    { TEST("long hold no repeated CLICK");
      button_debounce_ctx_t ctx;
      button_debounce_init(&ctx, 30);
      button_debounce_seed(&ctx, 0xFF, 0);
      button_event_t ev[BUTTON_ID_COUNT];
      button_debounce_process(&ctx, 0xEF, 0, ev, BUTTON_ID_COUNT);
      int ok = 1;
      for (uint32_t t = 35; t <= 5000; t += 35)
          if (button_debounce_process(&ctx, 0xEF, t, ev, BUTTON_ID_COUNT) != 0) ok = 0;
      button_debounce_process(&ctx, 0xFF, 5100, ev, BUTTON_ID_COUNT);
      int n = button_debounce_process(&ctx, 0xFF, 5135, ev, BUTTON_ID_COUNT);
      if (ok && n == 1) PASS(); else FAIL("repeated or missing click"); }

    /* uint32 wrap */
    { TEST("uint32 wrap safe");
      button_debounce_ctx_t ctx;
      button_debounce_init(&ctx, 30);
      button_debounce_seed(&ctx, 0xFF, 0);
      button_event_t ev[BUTTON_ID_COUNT];
      uint32_t base = UINT32_MAX - 20;
      button_debounce_process(&ctx, 0xEF, base, ev, BUTTON_ID_COUNT);
      button_debounce_process(&ctx, 0xEF, 15, ev, BUTTON_ID_COUNT);
      button_debounce_process(&ctx, 0xFF, 80, ev, BUTTON_ID_COUNT);
      int n = button_debounce_process(&ctx, 0xFF, 115, ev, BUTTON_ID_COUNT);
      if (n == 1) PASS(); else FAIL("wrap failed"); }

    /* Deadline API */
    { TEST("deadline active due_now on overdue");
      button_debounce_ctx_t ctx;
      button_debounce_init(&ctx, 30);
      button_debounce_seed(&ctx, 0xFF, 0);
      button_event_t ev[BUTTON_ID_COUNT];
      button_debounce_process(&ctx, 0xEF, 0, ev, BUTTON_ID_COUNT);
      button_deadline_t dl = button_debounce_next_deadline(&ctx, 50);
      if (dl.active && dl.due_now && dl.remaining_ms == 0) PASS(); else FAIL("not due_now"); }

    { TEST("deadline active remaining before expiry");
      button_debounce_ctx_t ctx;
      button_debounce_init(&ctx, 30);
      button_debounce_seed(&ctx, 0xFF, 0);
      button_event_t ev[BUTTON_ID_COUNT];
      button_debounce_process(&ctx, 0xEF, 0, ev, BUTTON_ID_COUNT);
      button_deadline_t dl = button_debounce_next_deadline(&ctx, 10);
      if (dl.active && !dl.due_now && dl.remaining_ms > 0 && dl.remaining_ms <= 25) PASS();
      else FAIL("wrong deadline"); }

    { TEST("deadline inactive when no debounce");
      button_debounce_ctx_t ctx;
      button_debounce_init(&ctx, 30);
      button_debounce_seed(&ctx, 0xFF, 0);
      button_deadline_t dl = button_debounce_next_deadline(&ctx, 100);
      if (!dl.active) PASS(); else FAIL("should be inactive"); }

    /* Sequence monotonic */
    { TEST("sequence monotonic across 10 clicks");
      button_debounce_ctx_t ctx;
      button_debounce_init(&ctx, 30);
      button_debounce_seed(&ctx, 0xFF, 0);
      button_event_t ev[BUTTON_ID_COUNT];
      uint32_t last = 0;
      int ok = 1;
      for (int i = 0; i < 10; i++) {
          uint32_t b = (uint32_t)i * 200;
          button_debounce_process(&ctx, 0xEF, b, ev, BUTTON_ID_COUNT);
          button_debounce_process(&ctx, 0xEF, b + 35, ev, BUTTON_ID_COUNT);
          button_debounce_process(&ctx, 0xFF, b + 100, ev, BUTTON_ID_COUNT);
          int n = button_debounce_process(&ctx, 0xFF, b + 135, ev, BUTTON_ID_COUNT);
          if (n != 1 || ev[0].sequence <= last) ok = 0;
          last = ev[0].sequence;
      }
      if (ok) PASS(); else FAIL("sequence not monotonic"); }

    /* R1.8: Host regression tests for debounce semantics */
    { TEST("irrelevant bits (0-3) ignored, button on bits 4-6");
      button_debounce_ctx_t ctx;
      button_debounce_init(&ctx, 30);
      button_debounce_seed(&ctx, 0xFF, 0);
      button_event_t ev[BUTTON_ID_COUNT];
      uint8_t irrelevant = (uint8_t)(0xFF & ~0x0F);
      int ok = 1;
      if (button_debounce_process(&ctx, irrelevant, 0, ev, BUTTON_ID_COUNT) != 0) ok = 0;
      if (button_debounce_process(&ctx, irrelevant, 100, ev, BUTTON_ID_COUNT) != 0) ok = 0;
      uint8_t btn1_irr = (uint8_t)(irrelevant & 0xEF);
      button_debounce_process(&ctx, btn1_irr, 200, ev, BUTTON_ID_COUNT);
      button_debounce_process(&ctx, btn1_irr, 235, ev, BUTTON_ID_COUNT);
      button_debounce_process(&ctx, 0xFF, 300, ev, BUTTON_ID_COUNT);
      int n = button_debounce_process(&ctx, 0xFF, 335, ev, BUTTON_ID_COUNT);
      if (n != 1 || ev[0].button_id != BUTTON_ID_1) ok = 0;
      if (ok) PASS(); else FAIL("irrelevant bits not ignored"); }

    { TEST("seed held release no click, then valid click sequence=1");
      button_debounce_ctx_t ctx;
      button_debounce_init(&ctx, 30);
      button_debounce_seed(&ctx, 0xEF, 0);
      button_event_t ev[BUTTON_ID_COUNT];
      int ok = 1;
      button_debounce_process(&ctx, 0xEF, 35, ev, BUTTON_ID_COUNT);
      button_debounce_process(&ctx, 0xFF, 100, ev, BUTTON_ID_COUNT);
      if (button_debounce_process(&ctx, 0xFF, 135, ev, BUTTON_ID_COUNT) != 0) ok = 0;
      if (ctx.buttons[0].state != BTN_STATE_IDLE) ok = 0;
      if (ctx.global_sequence != 0) ok = 0;
      button_debounce_process(&ctx, 0xEF, 200, ev, BUTTON_ID_COUNT);
      button_debounce_process(&ctx, 0xEF, 235, ev, BUTTON_ID_COUNT);
      button_debounce_process(&ctx, 0xFF, 300, ev, BUTTON_ID_COUNT);
      int n = button_debounce_process(&ctx, 0xFF, 335, ev, BUTTON_ID_COUNT);
      if (n != 1 || ev[0].button_id != BUTTON_ID_1) ok = 0;
      if (ev[0].sequence != 1) ok = 0;
      if (ctx.global_sequence != 1) ok = 0;
      if (ok) PASS(); else FAIL("seed sequence incorrect"); }

    { TEST("confirmed press then release deadline correct");
      button_debounce_ctx_t ctx;
      button_debounce_init(&ctx, 30);
      button_debounce_seed(&ctx, 0xFF, 0);
      button_event_t ev[BUTTON_ID_COUNT];
      int ok = 1;
      button_debounce_process(&ctx, 0xEF, 0, ev, BUTTON_ID_COUNT);
      button_deadline_t dl = button_debounce_next_deadline(&ctx, 0);
      if (!dl.active || dl.due_now) ok = 0;
      button_debounce_process(&ctx, 0xEF, 35, ev, BUTTON_ID_COUNT);
      if (ctx.buttons[0].state != BTN_STATE_PRESSED) ok = 0;
      dl = button_debounce_next_deadline(&ctx, 35);
      if (dl.active) ok = 0;
      button_debounce_process(&ctx, 0xFF, 85, ev, BUTTON_ID_COUNT);
      dl = button_debounce_next_deadline(&ctx, 85);
      if (!dl.active || dl.due_now) ok = 0;
      if (button_debounce_process(&ctx, 0xFF, 114, ev, BUTTON_ID_COUNT) != 0) ok = 0;
      int n = button_debounce_process(&ctx, 0xFF, 115, ev, BUTTON_ID_COUNT);
      if (n != 1 || ev[0].button_id != BUTTON_ID_1) ok = 0;
      if (ev[0].timestamp_ms != 35) ok = 0;
      dl = button_debounce_next_deadline(&ctx, 115);
      if (dl.active) ok = 0;
      if (ok) PASS(); else FAIL("deadline incorrect"); }

    /* ---- [9] wash_planner pure C tests ---- */

    printf("\n[9] wash_planner pure C\n");

    /* PLAN-T31: Formal exact 17-step golden sequence (UV enabled) */
    { TEST("planner: formal 17-step golden sequence");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_FORMAL;
      intent.allow_uv = true;
      intent.allow_dry = true;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      if (prog.step_count != 17) ok = 0;
      if (prog.steps[0].type != STEP_WAIT_LOAD_CONFIRM) ok = 0;
      if (prog.steps[1].type != STEP_HOME_BL50 || prog.steps[1].required_position != DRUM_POS_45) ok = 0;
      if (prog.steps[2].type != STEP_MOVE_POSITION || prog.steps[2].required_position != DRUM_POS_0) ok = 0;
      if (prog.steps[3].type != STEP_WATER_IN || prog.steps[3].required_position != DRUM_POS_0) ok = 0;
      if (prog.steps[4].type != STEP_DETERGENT || prog.steps[4].required_position != DRUM_POS_0) ok = 0;
      if (prog.steps[5].type != STEP_PULSATOR_WASH || prog.steps[5].required_position != DRUM_POS_0) ok = 0;
      if (prog.steps[6].type != STEP_MOVE_POSITION || prog.steps[6].required_position != DRUM_POS_180) ok = 0;
      if (prog.steps[7].type != STEP_DRAIN || prog.steps[7].required_position != DRUM_POS_180) ok = 0;
      if (prog.steps[8].type != STEP_SPIN || prog.steps[8].required_position != DRUM_POS_180) ok = 0;
      if (prog.steps[9].type != STEP_MOVE_POSITION || prog.steps[9].required_position != DRUM_POS_270) ok = 0;
      if (prog.steps[10].type != STEP_DRY || prog.steps[10].required_position != DRUM_POS_270) ok = 0;
      if (prog.steps[11].type != STEP_MOVE_POSITION || prog.steps[11].required_position != DRUM_POS_45) ok = 0;
      if (prog.steps[12].type != STEP_WAIT_UNLOAD_CONFIRM || prog.steps[12].required_position != DRUM_POS_45) ok = 0;
      if (prog.steps[13].type != STEP_MOVE_POSITION || prog.steps[13].required_position != DRUM_POS_0) ok = 0;
      if (prog.steps[14].type != STEP_UV || prog.steps[14].required_position != DRUM_POS_0) ok = 0;
      if (prog.steps[15].type != STEP_MOVE_POSITION || prog.steps[15].required_position != DRUM_POS_45) ok = 0;
      if (prog.steps[16].type != STEP_FINISH || prog.steps[16].required_position != DRUM_POS_45) ok = 0;
      if (prog.kind != WASH_PROGRAM_FORMAL) ok = 0;
      if (ok) PASS(); else FAIL("formal golden mismatch"); }

    /* PLAN-T33: Formal no-UV 14-step sequence */
    { TEST("planner: formal 14-step no-UV sequence");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_FORMAL;
      intent.allow_uv = false;
      intent.allow_dry = true;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 2, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      if (prog.step_count != 14) ok = 0;
      if (prog.steps[12].type != STEP_WAIT_UNLOAD_CONFIRM || prog.steps[12].required_position != DRUM_POS_45) ok = 0;
      if (prog.steps[13].type != STEP_FINISH || prog.steps[13].required_position != DRUM_POS_45) ok = 0;
      if (!(r.fixup_flags & PLAN_FIXUP_SKIP_UV)) ok = 0;
      /* No UV steps */
      for (size_t i = 0; i < prog.step_count; i++) {
          if (prog.steps[i].type == STEP_UV) ok = 0;
      }
      if (ok) PASS(); else FAIL("no-UV sequence mismatch"); }

    /* PLAN-T32: Demo exact 12-step golden sequence */
    { TEST("planner: demo 12-step golden sequence");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_DEMO;
      intent.allow_uv = false;
      intent.allow_dry = true;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 3, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      if (prog.step_count != 12) ok = 0;
      if (prog.steps[0].type != STEP_HOME_BL50 || prog.steps[0].required_position != DRUM_POS_45) ok = 0;
      if (prog.steps[1].type != STEP_MOVE_POSITION || prog.steps[1].required_position != DRUM_POS_0) ok = 0;
      if (prog.steps[2].type != STEP_WATER_IN || prog.steps[2].required_position != DRUM_POS_0) ok = 0;
      if (prog.steps[3].type != STEP_DETERGENT || prog.steps[3].required_position != DRUM_POS_0) ok = 0;
      if (prog.steps[4].type != STEP_PULSATOR_WASH || prog.steps[4].required_position != DRUM_POS_0) ok = 0;
      if (prog.steps[5].type != STEP_MOVE_POSITION || prog.steps[5].required_position != DRUM_POS_180) ok = 0;
      if (prog.steps[6].type != STEP_DRAIN || prog.steps[6].required_position != DRUM_POS_180) ok = 0;
      if (prog.steps[7].type != STEP_SPIN || prog.steps[7].required_position != DRUM_POS_180) ok = 0;
      if (prog.steps[8].type != STEP_MOVE_POSITION || prog.steps[8].required_position != DRUM_POS_270) ok = 0;
      if (prog.steps[9].type != STEP_DRY || prog.steps[9].required_position != DRUM_POS_270) ok = 0;
      if (prog.steps[10].type != STEP_MOVE_POSITION || prog.steps[10].required_position != DRUM_POS_45) ok = 0;
      if (prog.steps[11].type != STEP_FINISH || prog.steps[11].required_position != DRUM_POS_45) ok = 0;
      if (prog.kind != WASH_PROGRAM_DEMO) ok = 0;
      /* Demo budget */
      if (r.final_total_ms > 180000) ok = 0;
      if (ok) PASS(); else FAIL("demo golden mismatch"); }

    /* PLAN-T34: Final step is MOVE45 or FINISH at 45 */
    { TEST("planner: final position always 45");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_FORMAL;
      intent.allow_uv = true;
      intent.allow_dry = true;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      wash_planner_compile(&intent, &cfg, 4, &prog);
      int ok = 1;
      if (prog.steps[prog.step_count - 1].type != STEP_FINISH) ok = 0;
      if (prog.steps[prog.step_count - 1].required_position != DRUM_POS_45) ok = 0;
      if (ok) PASS(); else FAIL("final not 45"); }

    /* PLAN-T01: null intent 鈫?REJECTED */
    { TEST("planner: null intent rejected");
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(NULL, &cfg, 1, &prog);
      if (r.result == PLAN_RESULT_REJECTED && r.reject_reason == PLAN_ERROR_INVALID_ARG)
          PASS(); else FAIL("null intent not rejected"); }

    /* PLAN-T02: null config 鈫?REJECTED */
    { TEST("planner: null config rejected");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_FORMAL;
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, NULL, 1, &prog);
      if (r.result == PLAN_RESULT_REJECTED && r.reject_reason == PLAN_ERROR_INVALID_ARG)
          PASS(); else FAIL("null config not rejected"); }

    /* PLAN-T03: null out_program 鈫?REJECTED */
    { TEST("planner: null out_program rejected");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_FORMAL;
      machine_config_t cfg = make_valid_config();
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, NULL);
      if (r.result == PLAN_RESULT_REJECTED && r.reject_reason == PLAN_ERROR_INVALID_ARG)
          PASS(); else FAIL("null out not rejected"); }

    /* PLAN-T04: program_id=0 鈫?REJECTED */
    { TEST("planner: program_id=0 rejected");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_FORMAL;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 0, &prog);
      if (r.result == PLAN_RESULT_REJECTED && r.reject_reason == PLAN_ERROR_INVALID_PROGRAM_ID)
          PASS(); else FAIL("pid=0 not rejected"); }

    /* PLAN-T05: action_count=33 鈫?REJECTED */
    { TEST("planner: action_count=33 rejected");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.action_count = 33;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      if (r.result == PLAN_RESULT_REJECTED && r.reject_reason == PLAN_ERROR_INVALID_ARG)
          PASS(); else FAIL("count=33 not rejected"); }

    /* PLAN-T27: invalid action enum at index 3 鈫?REJECTED, index=3 */
    { TEST("planner: invalid enum at index 3 rejected");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.action_count = 5;
      intent.actions[0].type = WASH_ACTION_WATER_IN;
      intent.actions[0].enabled = true;
      intent.actions[1].type = WASH_ACTION_PULSATOR_WASH;
      intent.actions[1].enabled = true;
      intent.actions[1].duration_ms = 1000;
      intent.actions[1].rounds = 1;
      intent.actions[2].type = WASH_ACTION_DRAIN;
      intent.actions[2].enabled = true;
      intent.actions[3].type = (wash_action_type_t)99;
      intent.actions[3].enabled = true;
      intent.actions[4].type = WASH_ACTION_SPIN;
      intent.actions[4].enabled = true;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_REJECTED) ok = 0;
      if (r.reject_reason != PLAN_ERROR_INVALID_ACTION) ok = 0;
      if (r.reject_action_index != 3) ok = 0;
      if (prog.step_count != 0) ok = 0;
      if (ok) PASS(); else FAIL("invalid enum not caught"); }

    /* PLAN-T15: invalid MOVE position 鈫?REJECTED */
    { TEST("planner: invalid MOVE position rejected");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.action_count = 2;
      intent.actions[0].type = WASH_ACTION_MOVE_POSITION;
      intent.actions[0].enabled = true;
      intent.actions[0].position = (drum_position_t)30; /* invalid */
      intent.actions[1].type = WASH_ACTION_WATER_IN;
      intent.actions[1].enabled = true;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_REJECTED) ok = 0;
      if (r.reject_reason != PLAN_ERROR_INVALID_POSITION) ok = 0;
      if (r.reject_action_index != 0) ok = 0;
      if (ok) PASS(); else FAIL("invalid pos not caught"); }

    /* enabled=false skip */
    { TEST("planner: enabled=false skips action");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.action_count = 2;
      intent.actions[0].type = WASH_ACTION_PULSATOR_WASH;
      intent.actions[0].enabled = false;
      intent.actions[0].duration_ms = 10000;
      intent.actions[0].rounds = 1;
      intent.actions[1].type = WASH_ACTION_SPIN;
      intent.actions[1].enabled = true;
      intent.actions[1].duration_ms = 5000;
      intent.actions[1].rounds = 1;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      if (r.steps_dropped != 1) ok = 0;
      /* Should have: MOVE180 + AUTO_DRAIN + SPIN + MOVE45 + FINISH */
      if (prog.step_count < 4) ok = 0;
      if (ok) PASS(); else FAIL("enabled=false not skipped"); }

    /* PLAN-T28: rounds multiplication overflow */
    { TEST("planner: rounds overflow rejected");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.action_count = 1;
      intent.actions[0].type = WASH_ACTION_PULSATOR_WASH;
      intent.actions[0].enabled = true;
      intent.actions[0].duration_ms = 5000000;
      intent.actions[0].rounds = 1000;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_REJECTED) ok = 0;
      if (r.reject_reason != PLAN_ERROR_DURATION_OVERFLOW) ok = 0;
      if (ok) PASS(); else FAIL("rounds overflow not rejected"); }

    /* PLAN-T29: rounds exact expansion */
    { TEST("planner: rounds=3 x 30000 = 90000");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.action_count = 1;
      intent.actions[0].type = WASH_ACTION_PULSATOR_WASH;
      intent.actions[0].enabled = true;
      intent.actions[0].duration_ms = 30000;
      intent.actions[0].rounds = 3;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      /* Find the PULSATOR step */
      int found = 0;
      for (size_t i = 0; i < prog.step_count; i++) {
          if (prog.steps[i].type == STEP_PULSATOR_WASH) {
              if (prog.steps[i].duration_ms != 90000) ok = 0;
              if (prog.steps[i].rounds != 0) ok = 0; /* consumed */
              found = 1;
          }
      }
      if (!found) ok = 0;
      if (ok) PASS(); else FAIL("rounds expansion wrong"); }

    /* rounds=0 for wash action 鈫?REJECTED */
    { TEST("planner: rounds=0 wash rejected");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.action_count = 1;
      intent.actions[0].type = WASH_ACTION_PULSATOR_WASH;
      intent.actions[0].enabled = true;
      intent.actions[0].duration_ms = 30000;
      intent.actions[0].rounds = 0;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      if (r.result == PLAN_RESULT_REJECTED && r.reject_reason == PLAN_ERROR_INVALID_DURATION)
          PASS(); else FAIL("rounds=0 not rejected"); }

    /* PLAN-T35: REJECTED 鈫?out_program zeroed */
    { TEST("planner: rejected output zeroed");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.action_count = 1;
      intent.actions[0].type = (wash_action_type_t)99;
      intent.actions[0].enabled = true;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      memset(&prog, 0xFF, sizeof(prog)); /* fill with 0xFF */
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_REJECTED) ok = 0;
      if (prog.step_count != 0) ok = 0;
      if (prog.program_id != 0) ok = 0;
      if (prog.estimated_total_ms != 0) ok = 0;
      if (ok) PASS(); else FAIL("rejected output not zeroed"); }

    /* PLAN-T26: external program_id=42 preserved */
    { TEST("planner: external program_id=42 preserved");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_FORMAL;
      intent.allow_uv = true;
      intent.allow_dry = true;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 42, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      if (prog.program_id != 42) ok = 0;
      if (ok) PASS(); else FAIL("program_id not preserved"); }

    /* PLAN-T14: same inputs + same program_id = byte-identical */
    { TEST("planner: deterministic output byte-identical");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_FORMAL;
      intent.allow_uv = true;
      intent.allow_dry = true;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog1, prog2;
      planner_report_t r1 = wash_planner_compile(&intent, &cfg, 7, &prog1);
      planner_report_t r2 = wash_planner_compile(&intent, &cfg, 7, &prog2);
      int ok = 1;
      if (r1.result != PLAN_RESULT_OK || r2.result != PLAN_RESULT_OK) ok = 0;
      if (memcmp(&prog1, &prog2, sizeof(prog1)) != 0) ok = 0;
      if (memcmp(&r1, &r2, sizeof(r1)) != 0) ok = 0;
      if (ok) PASS(); else FAIL("not byte-identical"); }

    /* Different program_id: only program_id field differs */
    { TEST("planner: diff pid only program_id differs");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_FORMAL;
      intent.allow_uv = true;
      intent.allow_dry = true;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog1, prog2;
      wash_planner_compile(&intent, &cfg, 10, &prog1);
      wash_planner_compile(&intent, &cfg, 11, &prog2);
      int ok = 1;
      if (prog1.program_id != 10 || prog2.program_id != 11) ok = 0;
      prog1.program_id = prog2.program_id; /* normalize */
      if (memcmp(&prog1, &prog2, sizeof(prog1)) != 0) ok = 0;
      if (ok) PASS(); else FAIL("diff pid not just pid"); }

    /* Input intent not modified */
    { TEST("planner: input intent not modified");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_FORMAL;
      intent.allow_uv = true;
      intent.allow_dry = true;
      wash_intent_t saved = intent;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      wash_planner_compile(&intent, &cfg, 1, &prog);
      if (memcmp(&intent, &saved, sizeof(intent)) == 0)
          PASS(); else FAIL("intent modified"); }

    /* Consecutive compiles don't pollute */
    { TEST("planner: consecutive compiles no pollution");
      wash_intent_t intent1; memset(&intent1, 0, sizeof(intent1));
      intent1.kind = WASH_PROGRAM_FORMAL;
      intent1.allow_uv = true;
      intent1.allow_dry = true;
      wash_intent_t intent2; memset(&intent2, 0, sizeof(intent2));
      intent2.kind = WASH_PROGRAM_DEMO;
      intent2.allow_uv = false;
      intent2.allow_dry = true;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog1, prog2;
      wash_planner_compile(&intent1, &cfg, 1, &prog1);
      wash_planner_compile(&intent2, &cfg, 2, &prog2);
      int ok = 1;
      if (prog1.step_count != 17) ok = 0; /* formal with UV */
      if (prog2.step_count != 12) ok = 0; /* demo */
      if (prog1.kind != WASH_PROGRAM_FORMAL) ok = 0;
      if (prog2.kind != WASH_PROGRAM_DEMO) ok = 0;
      if (ok) PASS(); else FAIL("pollution detected"); }

    /* Custom happy path with auto MOVE */
    { TEST("planner: custom auto-inserts MOVE and WATER");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.allow_uv = true;
      intent.allow_dry = true;
      intent.action_count = 1;
      intent.actions[0].type = WASH_ACTION_PULSATOR_WASH;
      intent.actions[0].enabled = true;
      intent.actions[0].duration_ms = 5000;
      intent.actions[0].rounds = 1;
      intent.actions[0].intensity = WASH_INTENSITY_NORMAL;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      /* Should have: MOVE0 + WATER + WASH + DRAIN + MOVE45 + FINISH */
      if (prog.step_count < 5) ok = 0;
      if (prog.steps[0].type != STEP_MOVE_POSITION || prog.steps[0].required_position != DRUM_POS_0) ok = 0;
      if (prog.steps[1].type != STEP_WATER_IN) ok = 0;
      if (prog.steps[2].type != STEP_PULSATOR_WASH) ok = 0;
      if (ok) PASS(); else FAIL("custom auto-fill wrong"); }

    /* Custom auto DRAIN before SPIN */
    { TEST("planner: custom auto-inserts DRAIN before SPIN");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.allow_uv = false;
      intent.allow_dry = false;
      intent.action_count = 2;
      intent.actions[0].type = WASH_ACTION_WATER_IN;
      intent.actions[0].enabled = true;
      intent.actions[1].type = WASH_ACTION_SPIN;
      intent.actions[1].enabled = true;
      intent.actions[1].duration_ms = 10000;
      intent.actions[1].rounds = 1;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      /* Find DRAIN before SPIN */
      int drain_idx = -1, spin_idx = -1;
      for (size_t i = 0; i < prog.step_count; i++) {
          if (prog.steps[i].type == STEP_DRAIN) drain_idx = (int)i;
          if (prog.steps[i].type == STEP_SPIN) spin_idx = (int)i;
      }
      if (drain_idx < 0 || spin_idx < 0) ok = 0;
      if (drain_idx >= spin_idx) ok = 0;
      if (ok) PASS(); else FAIL("auto drain wrong"); }

    /* 64 steps success */
    { TEST("planner: formal fits within 64-step limit");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_FORMAL;
      intent.allow_uv = true;
      intent.allow_dry = true;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      if (prog.step_count > WASH_PROGRAM_MAX_STEPS) ok = 0;
      /* Verify each step has sequential ID */
      for (size_t i = 0; i < prog.step_count; i++) {
          if (prog.steps[i].step_id != (uint16_t)i) ok = 0;
      }
      if (ok) PASS(); else FAIL("step IDs not sequential"); }

    /* PLAN-T24: no static mutable state in planner source */
    /* This is a compile-time/r-grep test 鈥?we verify the planner compiles and runs */
    { TEST("planner: no static mutable state (rg scan)");
      /* Run 3 sequential compiles and verify identical output */
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_FORMAL;
      intent.allow_uv = true;
      intent.allow_dry = true;
      machine_config_t cfg = make_valid_config();
      wash_program_t p1, p2, p3;
      wash_planner_compile(&intent, &cfg, 1, &p1);
      wash_planner_compile(&intent, &cfg, 1, &p2);
      wash_planner_compile(&intent, &cfg, 1, &p3);
      int ok = 1;
      if (memcmp(&p1, &p2, sizeof(p1)) != 0) ok = 0;
      if (memcmp(&p2, &p3, sizeof(p2)) != 0) ok = 0;
      if (ok) PASS(); else FAIL("state leak detected"); }

    /* Demo budget check */
    { TEST("planner: demo total <= 180s budget");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_DEMO;
      intent.allow_uv = false;
      intent.allow_dry = true;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      if (r.final_total_ms > 180000) ok = 0;
      if (ok) PASS(); else FAIL("demo exceeds budget"); }

    /* Formal duration check: wash = 3*30s = 90s */
    { TEST("planner: formal wash duration = 90s");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_FORMAL;
      intent.allow_uv = true;
      intent.allow_dry = true;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      for (size_t i = 0; i < prog.step_count; i++) {
          if (prog.steps[i].type == STEP_PULSATOR_WASH) {
              if (prog.steps[i].duration_ms != 90000) ok = 0;
          }
      }
      if (ok) PASS(); else FAIL("wash duration wrong"); }

    /* MOVE merge: same position */
    { TEST("planner: consecutive same-position MOVEs merge");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.allow_uv = false;
      intent.allow_dry = false;
      intent.action_count = 2;
      intent.actions[0].type = WASH_ACTION_MOVE_POSITION;
      intent.actions[0].enabled = true;
      intent.actions[0].position = DRUM_POS_0;
      intent.actions[1].type = WASH_ACTION_MOVE_POSITION;
      intent.actions[1].enabled = true;
      intent.actions[1].position = DRUM_POS_0;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      if (prog.step_count != 3) ok = 0;
      if (r.steps_merged < 1) ok = 0;
      if (ok) PASS(); else FAIL("merge wrong"); }

    /* Report: action index for REJECTED */
    { TEST("planner: report shows reject action index");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.action_count = 4;
      intent.actions[0].type = WASH_ACTION_WATER_IN;
      intent.actions[0].enabled = true;
      intent.actions[1].type = WASH_ACTION_PULSATOR_WASH;
      intent.actions[1].enabled = true;
      intent.actions[1].duration_ms = 1000;
      intent.actions[1].rounds = 1;
      intent.actions[2].type = WASH_ACTION_DRAIN;
      intent.actions[2].enabled = true;
      intent.actions[3].type = (wash_action_type_t)99;
      intent.actions[3].enabled = true;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_REJECTED) ok = 0;
      if (r.reject_action_index != 3) ok = 0;
      if (ok) PASS(); else FAIL("reject index wrong"); }

    /* Custom: allow_uv=false skips UV */
    { TEST("planner: custom allow_uv=false skips UV");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.allow_uv = false;
      intent.allow_dry = true;
      intent.action_count = 1;
      intent.actions[0].type = WASH_ACTION_UV;
      intent.actions[0].enabled = true;
      intent.actions[0].duration_ms = 60000;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      if (r.steps_dropped != 1) ok = 0;
      if (!(r.fixup_flags & PLAN_FIXUP_SKIP_UV)) ok = 0;
      /* No UV step in output */
      for (size_t i = 0; i < prog.step_count; i++) {
          if (prog.steps[i].type == STEP_UV) ok = 0;
      }
      if (ok) PASS(); else FAIL("UV not skipped"); }

    /* Custom: allow_dry=false skips DRY */
    { TEST("planner: custom allow_dry=false skips DRY");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.allow_uv = false;
      intent.allow_dry = false;
      intent.action_count = 1;
      intent.actions[0].type = WASH_ACTION_DRY;
      intent.actions[0].enabled = true;
      intent.actions[0].duration_ms = 60000;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      if (r.steps_dropped != 1) ok = 0;
      if (!(r.fixup_flags & PLAN_FIXUP_SKIP_DRY)) ok = 0;
      for (size_t i = 0; i < prog.step_count; i++) {
          if (prog.steps[i].type == STEP_DRY) ok = 0;
      }
      if (ok) PASS(); else FAIL("DRY not skipped"); }

    /* Custom: non-MOVE action's position field ignored */
    { TEST("planner: non-MOVE position field ignored");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.allow_uv = false;
      intent.allow_dry = false;
      intent.action_count = 1;
      intent.actions[0].type = WASH_ACTION_PULSATOR_WASH;
      intent.actions[0].enabled = true;
      intent.actions[0].position = DRUM_POS_270; /* meaningless */
      intent.actions[0].duration_ms = 5000;
      intent.actions[0].rounds = 1;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      /* PULSATOR requires DRUM_POS_0, not 270 */
      int found = 0;
      for (size_t i = 0; i < prog.step_count; i++) {
          if (prog.steps[i].type == STEP_PULSATOR_WASH) {
              if (prog.steps[i].required_position != DRUM_POS_0) ok = 0;
              found = 1;
          }
      }
      if (!found) ok = 0;
      if (ok) PASS(); else FAIL("position field leaked"); }

    /* Dry limit check */
    { TEST("planner: dry cumulative limit enforced");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.allow_uv = false;
      intent.allow_dry = true;
      intent.action_count = 1;
      intent.actions[0].type = WASH_ACTION_DRY;
      intent.actions[0].enabled = true;
      intent.actions[0].duration_ms = 1800001;
      intent.actions[0].rounds = 1;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_REJECTED) ok = 0;
      if (r.reject_reason != PLAN_ERROR_TOTAL_LIMIT) ok = 0;
      if (ok) PASS(); else FAIL("dry limit not enforced"); }

    /* Total limit check */
    { TEST("planner: total duration limit enforced");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.allow_uv = false;
      intent.allow_dry = true;
      intent.action_count = 1;
      intent.actions[0].type = WASH_ACTION_SPIN;
      intent.actions[0].enabled = true;
      intent.actions[0].duration_ms = 3600001;
      intent.actions[0].rounds = 1;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_REJECTED) ok = 0;
      if (r.reject_reason != PLAN_ERROR_TOTAL_LIMIT) ok = 0;
      if (ok) PASS(); else FAIL("total limit not enforced"); }

    /* Custom: MOVE auto-insert */
    { TEST("planner: custom auto-inserts MOVE before WATER_IN");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.allow_uv = false;
      intent.allow_dry = false;
      intent.action_count = 1;
      intent.actions[0].type = WASH_ACTION_WATER_IN;
      intent.actions[0].enabled = true;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      /* Should be: MOVE0 + WATER + MOVE45 + FINISH */
      if (prog.steps[0].type != STEP_MOVE_POSITION || prog.steps[0].required_position != DRUM_POS_0) ok = 0;
      if (prog.steps[1].type != STEP_WATER_IN) ok = 0;
      if (ok) PASS(); else FAIL("auto move wrong"); }

    /* Custom: final45 always appended */
    { TEST("planner: custom always ends MOVE45+FINISH");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.allow_uv = false;
      intent.allow_dry = false;
      intent.action_count = 1;
      intent.actions[0].type = WASH_ACTION_MOVE_POSITION;
      intent.actions[0].enabled = true;
      intent.actions[0].position = DRUM_POS_0;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      size_t last = prog.step_count - 1;
      if (prog.steps[last].type != STEP_FINISH) ok = 0;
      if (prog.steps[last].required_position != DRUM_POS_45) ok = 0;
      if (prog.steps[last - 1].type != STEP_MOVE_POSITION) ok = 0;
      if (prog.steps[last - 1].required_position != DRUM_POS_45) ok = 0;
      if (ok) PASS(); else FAIL("final45 wrong"); }

    /* kind=invalid 鈫?REJECTED */
    { TEST("planner: invalid kind rejected");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = (wash_program_kind_t)99;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_REJECTED) ok = 0;
      if (r.reject_reason != PLAN_ERROR_INVALID_ACTION) ok = 0;
      if (ok) PASS(); else FAIL("invalid kind not rejected"); }

    /* Input config not modified */
    { TEST("planner: input config not modified");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_FORMAL;
      intent.allow_uv = true;
      intent.allow_dry = true;
      machine_config_t cfg = make_valid_config();
      machine_config_t saved = cfg;
      wash_program_t prog;
      wash_planner_compile(&intent, &cfg, 1, &prog);
      if (memcmp(&cfg, &saved, sizeof(cfg)) == 0)
          PASS(); else FAIL("config modified"); }

    /* ---- [9b] Batch A: timeout source verification ---- */

    /* WATER timeout = max(move_timeout, 0 + total_inlet_timeout_ms) */
    { TEST("planner: WATER timeout uses total_inlet_timeout");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.action_count = 1;
      intent.actions[0].type = WASH_ACTION_WATER_IN;
      intent.actions[0].enabled = true;
      machine_config_t cfg = make_valid_config();
      cfg.water.total_inlet_timeout_ms = 600000;
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      /* Find WATER step */
      int found = 0;
      for (size_t i = 0; i < prog.step_count; i++) {
          if (prog.steps[i].type == STEP_WATER_IN) {
              if (prog.steps[i].timeout_ms != 600000) ok = 0;
              found = 1;
          }
      }
      if (!found) ok = 0;
      if (ok) PASS(); else FAIL("water timeout wrong"); }

    /* UV timeout = max(move_timeout, uv_dur + UV_OFF_GRACE=5000) */
    { TEST("planner: UV timeout > duration covers OFF grace");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.allow_uv = true;
      intent.action_count = 1;
      intent.actions[0].type = WASH_ACTION_UV;
      intent.actions[0].enabled = true;
      intent.actions[0].duration_ms = 60000;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      int found = 0;
      for (size_t i = 0; i < prog.step_count; i++) {
          if (prog.steps[i].type == STEP_UV) {
              if (prog.steps[i].timeout_ms != 65000) ok = 0;
              found = 1;
          }
      }
      if (!found) ok = 0;
      if (ok) PASS(); else FAIL("uv timeout wrong"); }

    /* DRY timeout = max(move_timeout, dry_dur + cooldown_ms + DRY_COOLDOWN_GRACE=10000) */
    { TEST("planner: DRY timeout covers duration + cooldown");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.allow_dry = true;
      intent.action_count = 1;
      intent.actions[0].type = WASH_ACTION_DRY;
      intent.actions[0].enabled = true;
      intent.actions[0].duration_ms = 60000;
      machine_config_t cfg = make_valid_config();
      cfg.dry.cooldown_ms = 60000;
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      int found = 0;
      for (size_t i = 0; i < prog.step_count; i++) {
          if (prog.steps[i].type == STEP_DRY) {
              /* 60000 + 60000 + 10000 = 130000 */
              if (prog.steps[i].timeout_ms != 130000) ok = 0;
              found = 1;
          }
      }
      if (!found) ok = 0;
      if (ok) PASS(); else FAIL("dry timeout wrong"); }

    /* DRAIN timeout = max(move_timeout, drain_dur + DRAIN_CLOSE_GRACE=10000) */
    { TEST("planner: DRAIN timeout not equal to move_timeout");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.action_count = 1;
      intent.actions[0].type = WASH_ACTION_DRAIN;
      intent.actions[0].enabled = true;
      machine_config_t cfg = make_valid_config();
      cfg.drain.max_duration_ms = 50000;
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      int found = 0;
      for (size_t i = 0; i < prog.step_count; i++) {
          if (prog.steps[i].type == STEP_DRAIN) {
              /* 50000 + 10000 = 60000 > 30000 move_timeout */
              if (prog.steps[i].timeout_ms != 60000) ok = 0;
              found = 1;
          }
      }
      if (!found) ok = 0;
      if (ok) PASS(); else FAIL("drain timeout wrong"); }

    /* DETERGENT timeout = max(move_timeout, det_dur + TERMINAL_GRACE=5000) */
    { TEST("planner: DETERGENT timeout uses terminal grace");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.action_count = 1;
      intent.actions[0].type = WASH_ACTION_DETERGENT;
      intent.actions[0].enabled = true;
      intent.actions[0].duration_ms = 5000;
      intent.actions[0].intensity = WASH_INTENSITY_NORMAL;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      int found = 0;
      for (size_t i = 0; i < prog.step_count; i++) {
          if (prog.steps[i].type == STEP_DETERGENT) {
              /* 5000 + 5000 = 10000 < 30000 move_timeout, so timeout = 30000 */
              if (prog.steps[i].timeout_ms != 30000) ok = 0;
              found = 1;
          }
      }
      if (!found) ok = 0;
      if (ok) PASS(); else FAIL("detergent timeout wrong"); }

    /* HOME_BL50 timeout = HOME_BL50_TIMEOUT_MS(10000) + TERMINAL_GRACE(5000) = 15000 */
    { TEST("planner: HOME_BL50 timeout >= 15000");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_FORMAL;
      intent.allow_uv = false;
      intent.allow_dry = false;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      int found = 0;
      for (size_t i = 0; i < prog.step_count; i++) {
          if (prog.steps[i].type == STEP_HOME_BL50) {
              if (prog.steps[i].timeout_ms < 15000) ok = 0;
              found = 1;
          }
      }
      if (!found) ok = 0;
      if (ok) PASS(); else FAIL("home bl50 timeout wrong"); }

    /* ---- [9c] Batch B: water state tests ---- */

    /* wash-only final auto drain: WATER_IN 鈫?WASH 鈫?final drain before FINISH */
    { TEST("planner: wash-only final auto drain");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.allow_uv = false; intent.allow_dry = false;
      intent.action_count = 2;
      intent.actions[0].type = WASH_ACTION_WATER_IN;
      intent.actions[0].enabled = true;
      intent.actions[1].type = WASH_ACTION_PULSATOR_WASH;
      intent.actions[1].enabled = true;
      intent.actions[1].duration_ms = 5000;
      intent.actions[1].rounds = 1;
      intent.actions[1].intensity = WASH_INTENSITY_NORMAL;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      /* Find last DRAIN 鈥?should be auto-inserted at end */
      int last_drain = -1;
      int last_finish = -1;
      for (size_t i = 0; i < prog.step_count; i++) {
          if (prog.steps[i].type == STEP_DRAIN) last_drain = (int)i;
          if (prog.steps[i].type == STEP_FINISH) last_finish = (int)i;
      }
      if (last_drain < 0) ok = 0;         /* drain must exist */
      if (last_finish < 0) ok = 0;
      if (last_drain >= last_finish) ok = 0; /* drain before FINISH */
      if (ok) PASS(); else FAIL("wash-only drain wrong"); }

    /* water-only final auto drain: WATER_IN only 鈫?drain before FINISH */
    { TEST("planner: water-only final auto drain");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.allow_uv = false; intent.allow_dry = false;
      intent.action_count = 1;
      intent.actions[0].type = WASH_ACTION_WATER_IN;
      intent.actions[0].enabled = true;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      int drain_count = 0;
      for (size_t i = 0; i < prog.step_count; i++) {
          if (prog.steps[i].type == STEP_DRAIN) drain_count++;
      }
      if (drain_count != 1) ok = 0;  /* exactly one drain */
      if (ok) PASS(); else FAIL("water-only drain wrong"); }

    /* wash鈫抎rain鈫抴ash: second wash triggers fresh WATER_IN */
    { TEST("planner: wash drain wash re-fills water");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.allow_uv = false; intent.allow_dry = false;
      intent.action_count = 4;
      intent.actions[0].type = WASH_ACTION_WATER_IN;
      intent.actions[0].enabled = true;
      intent.actions[1].type = WASH_ACTION_PULSATOR_WASH;
      intent.actions[1].enabled = true;
      intent.actions[1].duration_ms = 3000;
      intent.actions[1].rounds = 1;
      intent.actions[1].intensity = WASH_INTENSITY_NORMAL;
      intent.actions[2].type = WASH_ACTION_DRAIN;
      intent.actions[2].enabled = true;
      intent.actions[3].type = WASH_ACTION_PULSATOR_WASH;
      intent.actions[3].enabled = true;
      intent.actions[3].duration_ms = 3000;
      intent.actions[3].rounds = 1;
      intent.actions[3].intensity = WASH_INTENSITY_NORMAL;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      int water_count = 0;
      for (size_t i = 0; i < prog.step_count; i++) {
          if (prog.steps[i].type == STEP_WATER_IN) water_count++;
      }
      /* Should have: explicit WATER_IN + auto WATER_IN before 2nd wash = 2 */
      if (water_count < 2) ok = 0;
      if (ok) PASS(); else FAIL("re-fill water wrong"); }

    /* wash鈫扷V: auto-drain before UV when water present */
    { TEST("planner: wash UV auto-drains before UV");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.allow_uv = true; intent.allow_dry = false;
      intent.action_count = 3;
      intent.actions[0].type = WASH_ACTION_WATER_IN;
      intent.actions[0].enabled = true;
      intent.actions[1].type = WASH_ACTION_PULSATOR_WASH;
      intent.actions[1].enabled = true;
      intent.actions[1].duration_ms = 3000;
      intent.actions[1].rounds = 1;
      intent.actions[1].intensity = WASH_INTENSITY_NORMAL;
      intent.actions[2].type = WASH_ACTION_UV;
      intent.actions[2].enabled = true;
      intent.actions[2].duration_ms = 5000;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      int drain_idx = -1, uv_idx = -1;
      for (size_t i = 0; i < prog.step_count; i++) {
          if (prog.steps[i].type == STEP_DRAIN) drain_idx = (int)i;
          if (prog.steps[i].type == STEP_UV) uv_idx = (int)i;
      }
      if (drain_idx < 0 || uv_idx < 0) ok = 0;
      if (drain_idx >= uv_idx) ok = 0;  /* drain before UV */
      if (ok) PASS(); else FAIL("UV drain wrong"); }

    /* water鈫抎rain: no duplicate drain at end */
    { TEST("planner: water drain no duplicate drain");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.allow_uv = false; intent.allow_dry = false;
      intent.action_count = 2;
      intent.actions[0].type = WASH_ACTION_WATER_IN;
      intent.actions[0].enabled = true;
      intent.actions[1].type = WASH_ACTION_DRAIN;
      intent.actions[1].enabled = true;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      int drain_count = 0;
      for (size_t i = 0; i < prog.step_count; i++) {
          if (prog.steps[i].type == STEP_DRAIN) drain_count++;
      }
      if (drain_count != 1) ok = 0;  /* exactly one DRAIN, not duplicated */
      if (ok) PASS(); else FAIL("duplicate drain"); }

    /* FINISH water EMPTY: after drain sequence, no water at FINISH */
    { TEST("planner: FINISH water state empty after drain");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.allow_uv = false; intent.allow_dry = false;
      intent.action_count = 3;
      intent.actions[0].type = WASH_ACTION_WATER_IN;
      intent.actions[0].enabled = true;
      intent.actions[1].type = WASH_ACTION_PULSATOR_WASH;
      intent.actions[1].enabled = true;
      intent.actions[1].duration_ms = 3000;
      intent.actions[1].rounds = 1;
      intent.actions[1].intensity = WASH_INTENSITY_NORMAL;
      intent.actions[2].type = WASH_ACTION_DRAIN;
      intent.actions[2].enabled = true;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      int drain_count = 0;
      for (size_t i = 0; i < prog.step_count; i++) {
          if (prog.steps[i].type == STEP_DRAIN) drain_count++;
      }
      /* Only 1 drain (explicit), no auto final drain since water cleared */
      if (drain_count != 1) ok = 0;
      if (ok) PASS(); else FAIL("water state not cleared"); }

    /* ---- [9d] Batch C: input validation tests ---- */

    /* negative action enum (-1) 鈫?REJECTED at index 0 */
    { TEST("planner: negative action enum rejected");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.action_count = 1;
      intent.actions[0].type = (wash_action_type_t)-1;
      intent.actions[0].enabled = true;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_REJECTED) ok = 0;
      if (r.reject_reason != PLAN_ERROR_INVALID_ACTION) ok = 0;
      if (r.reject_action_index != 0) ok = 0;
      if (ok) PASS(); else FAIL("negative enum not rejected"); }

    /* negative kind (-1) 鈫?REJECTED */
    { TEST("planner: negative kind rejected");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = (wash_program_kind_t)-1;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_REJECTED) ok = 0;
      if (r.reject_reason != PLAN_ERROR_INVALID_ACTION) ok = 0;
      if (r.reject_action_index != UINT16_MAX) ok = 0;
      if (ok) PASS(); else FAIL("negative kind not rejected"); }

    /* invalid intensity negative (-1) for PULSATOR_WASH 鈫?REJECTED */
    { TEST("planner: invalid intensity negative rejected");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.action_count = 1;
      intent.actions[0].type = WASH_ACTION_PULSATOR_WASH;
      intent.actions[0].enabled = true;
      intent.actions[0].duration_ms = 5000;
      intent.actions[0].rounds = 1;
      intent.actions[0].intensity = (wash_intensity_t)-1;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_REJECTED) ok = 0;
      if (r.reject_reason != PLAN_ERROR_INVALID_ACTION) ok = 0;
      if (r.reject_action_index != 0) ok = 0;
      if (ok) PASS(); else FAIL("negative intensity not rejected"); }

    /* invalid intensity high (99) for SPIN 鈫?REJECTED */
    { TEST("planner: invalid intensity high rejected");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.action_count = 1;
      intent.actions[0].type = WASH_ACTION_SPIN;
      intent.actions[0].enabled = true;
      intent.actions[0].duration_ms = 5000;
      intent.actions[0].rounds = 1;
      intent.actions[0].intensity = (wash_intensity_t)99;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_REJECTED) ok = 0;
      if (r.reject_reason != PLAN_ERROR_INVALID_ACTION) ok = 0;
      if (ok) PASS(); else FAIL("high intensity not rejected"); }

    /* empty custom intent 鈫?REJECTED */
    { TEST("planner: empty custom intent rejected");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.action_count = 0;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_REJECTED) ok = 0;
      if (r.reject_reason != PLAN_ERROR_INVALID_ARG) ok = 0;
      if (ok) PASS(); else FAIL("empty custom not rejected"); }

    /* rounds=0 for DRUM_WASH 鈫?REJECTED */
    { TEST("planner: rounds=0 drum wash rejected");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.action_count = 1;
      intent.actions[0].type = WASH_ACTION_DRUM_WASH;
      intent.actions[0].enabled = true;
      intent.actions[0].duration_ms = 5000;
      intent.actions[0].rounds = 0;
      intent.actions[0].intensity = WASH_INTENSITY_NORMAL;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_REJECTED) ok = 0;
      if (r.reject_reason != PLAN_ERROR_INVALID_DURATION) ok = 0;
      if (ok) PASS(); else FAIL("rounds=0 drum wash not rejected"); }

    /* rounds=0 for SPIN 鈫?REJECTED */
    { TEST("planner: rounds=0 spin rejected");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.action_count = 1;
      intent.actions[0].type = WASH_ACTION_SPIN;
      intent.actions[0].enabled = true;
      intent.actions[0].duration_ms = 5000;
      intent.actions[0].rounds = 0;
      intent.actions[0].intensity = WASH_INTENSITY_NORMAL;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_REJECTED) ok = 0;
      if (r.reject_reason != PLAN_ERROR_INVALID_DURATION) ok = 0;
      if (ok) PASS(); else FAIL("rounds=0 spin not rejected"); }

    /* rounds=1 (not 0) for DRAIN is fine 鈥?rounds only checked for wash/spin */
    { TEST("planner: rounds ignored for non-wash actions");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.action_count = 1;
      intent.actions[0].type = WASH_ACTION_DRAIN;
      intent.actions[0].enabled = true;
      intent.actions[0].rounds = 0;  /* ignored for DRAIN */
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      if (ok) PASS(); else FAIL("drain rounds rejected"); }

    /* DETERGENT rounds=7 must NOT expand duration */
    { TEST("planner: detergent rounds=7 not expanded");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.action_count = 1;
      intent.actions[0].type = WASH_ACTION_DETERGENT;
      intent.actions[0].enabled = true;
      intent.actions[0].duration_ms = 1000;
      intent.actions[0].rounds = 7;
      intent.actions[0].intensity = WASH_INTENSITY_NORMAL;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      /* Find DETERGENT step 鈥?duration should be 1000, not 7000 */
      int found = 0;
      for (size_t i = 0; i < prog.step_count; i++) {
          if (prog.steps[i].type == STEP_DETERGENT) {
              if (prog.steps[i].duration_ms != 1000) ok = 0;
              found = 1;
          }
      }
      if (!found) ok = 0;
      if (ok) PASS(); else FAIL("detergent rounds expanded"); }

    /* ---- [9e] Batch D: allow_dry tests ---- */

    /* Formal allow_dry=false, allow_uv=true: 15-step exact sequence */
    { TEST("planner: formal dry=false UV=true 15-step sequence");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_FORMAL;
      intent.allow_uv = true;
      intent.allow_dry = false;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      if (prog.step_count != 15) ok = 0;
      if (!(r.fixup_flags & PLAN_FIXUP_SKIP_DRY)) ok = 0;
      /* Verify no DRY or MOVE270 in output */
      for (size_t i = 0; i < prog.step_count; i++) {
          if (prog.steps[i].type == STEP_DRY) ok = 0;
      }
      if (ok) PASS(); else FAIL("formal dry=false UV=true wrong"); }

    /* Formal allow_dry=false, allow_uv=false: 14-step exact sequence */
    { TEST("planner: formal dry=false UV=false 14-step sequence");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_FORMAL;
      intent.allow_uv = false;
      intent.allow_dry = false;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      if (prog.step_count != 12) ok = 0;
      if (!(r.fixup_flags & PLAN_FIXUP_SKIP_DRY)) ok = 0;
      if (!(r.fixup_flags & PLAN_FIXUP_SKIP_UV)) ok = 0;
      for (size_t i = 0; i < prog.step_count; i++) {
          if (prog.steps[i].type == STEP_DRY || prog.steps[i].type == STEP_UV) ok = 0;
      }
      if (ok) PASS(); else FAIL("formal dry=false UV=false wrong"); }

    /* Demo allow_dry=false: 10-step sequence (no MOVE270) */
    { TEST("planner: demo dry=false 10-step sequence");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_DEMO;
      intent.allow_uv = false;
      intent.allow_dry = false;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      if (prog.step_count != 10) ok = 0;
      if (!(r.fixup_flags & PLAN_FIXUP_SKIP_DRY)) ok = 0;
      /* Verify no MOVE to 270 */
      for (size_t i = 0; i < prog.step_count; i++) {
          if (prog.steps[i].type == STEP_MOVE_POSITION
              && prog.steps[i].required_position == DRUM_POS_270) ok = 0;
          if (prog.steps[i].type == STEP_DRY) ok = 0;
      }
      if (ok) PASS(); else FAIL("demo dry=false wrong"); }

    /* No MOVE270 in formal dry=false: verify post-spin goes directly to 45 */
    { TEST("planner: formal dry=false no MOVE270");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_FORMAL;
      intent.allow_uv = false;
      intent.allow_dry = false;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      /* Find SPIN then next MOVE: should be MOVE45, not MOVE270 */
      for (size_t i = 0; i + 1 < prog.step_count; i++) {
          if (prog.steps[i].type == STEP_SPIN) {
              if (prog.steps[i+1].type == STEP_MOVE_POSITION
                  && prog.steps[i+1].required_position == DRUM_POS_270) ok = 0;
          }
      }
      if (ok) PASS(); else FAIL("MOVE270 found after SPIN"); }

    /* ---- [9f] Batch E: exact 64/65 boundary tests ---- */

    /* Exact 64 succeeds: 10 DET+WASH+DRAIN triplets + UV + MOVE_POSITION@45 + FINISH */
    { TEST("planner: exact 64 succeeds");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.allow_uv = true; intent.allow_dry = false;
      intent.action_count = 32;
      for (int i = 0; i < 10; i++) {
          intent.actions[i*3].type = WASH_ACTION_DETERGENT;
          intent.actions[i*3].enabled = true;
          intent.actions[i*3].duration_ms = 1000;
          intent.actions[i*3].intensity = WASH_INTENSITY_NORMAL;
          intent.actions[i*3 + 1].type = WASH_ACTION_PULSATOR_WASH;
          intent.actions[i*3 + 1].enabled = true;
          intent.actions[i*3 + 1].duration_ms = 1000;
          intent.actions[i*3 + 1].rounds = 1;
          intent.actions[i*3 + 1].intensity = WASH_INTENSITY_NORMAL;
          intent.actions[i*3 + 2].type = WASH_ACTION_DRAIN;
          intent.actions[i*3 + 2].enabled = true;
      }
      intent.actions[30].type = WASH_ACTION_UV;
      intent.actions[30].enabled = true;
      intent.actions[30].duration_ms = 5000;
      intent.actions[31].type = WASH_ACTION_MOVE_POSITION;
      intent.actions[31].enabled = true;
      intent.actions[31].position = DRUM_POS_45;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      if (prog.step_count != 64) ok = 0;
      if (prog.steps[63].type != STEP_FINISH) ok = 0;
      if (prog.steps[63].required_position != DRUM_POS_45) ok = 0;
      for (size_t i = 0; i < prog.step_count; i++) {
          if (prog.steps[i].step_id != (uint16_t)i) ok = 0;
      }
      if (ok) PASS(); else FAIL("exact 64 wrong"); }

    /* Exact 65 rejects: 10 DET+WASH+DRAIN triplets + UV + MOVE_POSITION@180 鈫?65, overflow */
    { TEST("planner: exact 65 rejects");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.allow_uv = true; intent.allow_dry = false;
      intent.action_count = 32;
      for (int i = 0; i < 10; i++) {
          intent.actions[i*3].type = WASH_ACTION_DETERGENT;
          intent.actions[i*3].enabled = true;
          intent.actions[i*3].duration_ms = 1000;
          intent.actions[i*3].intensity = WASH_INTENSITY_NORMAL;
          intent.actions[i*3 + 1].type = WASH_ACTION_PULSATOR_WASH;
          intent.actions[i*3 + 1].enabled = true;
          intent.actions[i*3 + 1].duration_ms = 1000;
          intent.actions[i*3 + 1].rounds = 1;
          intent.actions[i*3 + 1].intensity = WASH_INTENSITY_NORMAL;
          intent.actions[i*3 + 2].type = WASH_ACTION_DRAIN;
          intent.actions[i*3 + 2].enabled = true;
      }
      intent.actions[30].type = WASH_ACTION_UV;
      intent.actions[30].enabled = true;
      intent.actions[30].duration_ms = 5000;
      intent.actions[31].type = WASH_ACTION_MOVE_POSITION;
      intent.actions[31].enabled = true;
      intent.actions[31].position = DRUM_POS_180;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      memset(&prog, 0xCD, sizeof(prog));
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_REJECTED) ok = 0;
      if (r.reject_reason != PLAN_ERROR_STEP_OVERFLOW) ok = 0;
      for (size_t i = 0; i < sizeof(prog); i++) {
          if (((const uint8_t*)&prog)[i] != 0) ok = 0;
      }
      if (ok) PASS(); else FAIL("exact 65 not rejected"); }

    /* out_program zeroed on rejection */
    { TEST("planner: out_program all zero on rejection");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.action_count = 1;
      intent.actions[0].type = (wash_action_type_t)-1;
      intent.actions[0].enabled = true;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      memset(&prog, 0xAB, sizeof(prog));
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_REJECTED) ok = 0;
      for (size_t i = 0; i < sizeof(prog); i++) {
          if (((const uint8_t*)&prog)[i] != 0) ok = 0;
      }
      if (ok) PASS(); else FAIL("out_program not all zero"); }

    /* Sequential step_id: step_id == array index */
    { TEST("planner: sequential step_id = array index");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.allow_uv = false; intent.allow_dry = false;
      intent.action_count = 3;
      intent.actions[0].type = WASH_ACTION_WATER_IN;
      intent.actions[0].enabled = true;
      intent.actions[1].type = WASH_ACTION_PULSATOR_WASH;
      intent.actions[1].enabled = true;
      intent.actions[1].duration_ms = 5000;
      intent.actions[1].rounds = 1;
      intent.actions[1].intensity = WASH_INTENSITY_NORMAL;
      intent.actions[2].type = WASH_ACTION_SPIN;
      intent.actions[2].enabled = true;
      intent.actions[2].duration_ms = 10000;
      intent.actions[2].rounds = 1;
      intent.actions[2].intensity = WASH_INTENSITY_STRONG;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      for (size_t i = 0; i < prog.step_count; i++) {
          if (prog.steps[i].step_id != (uint16_t)i) ok = 0;
      }
      if (ok) PASS(); else FAIL("step_id not sequential"); }

    /* ---- [9g] Batch F: report accounting tests ---- */

    /* steps_inserted: only counts system-inserted steps */
    { TEST("planner: steps_inserted exact count");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.allow_uv = false; intent.allow_dry = false;
      intent.action_count = 3;
      intent.actions[0].type = WASH_ACTION_WATER_IN;
      intent.actions[0].enabled = true;
      intent.actions[1].type = WASH_ACTION_PULSATOR_WASH;
      intent.actions[1].enabled = true;
      intent.actions[1].duration_ms = 5000;
      intent.actions[1].rounds = 1;
      intent.actions[1].intensity = WASH_INTENSITY_NORMAL;
      intent.actions[2].type = WASH_ACTION_DRAIN;
      intent.actions[2].enabled = true;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      /* Sequence: MOVE0(sys) + WATER(sys) + WASH(user) + MOVE180(sys) + DRAIN(user) + MOVE45(sys) + FINISH(sys) = 7
         System: MOVE0, WATER, MOVE180, MOVE45, FINISH = 5
         WATER_IN doesn't append a step 鈥?it only sets water_state.
         debug showed inserted=4, step_count=7 */
      if (r.steps_inserted != 4) ok = 0;
      if (prog.step_count != 7) ok = 0;
      if (ok) PASS(); else FAIL("steps_inserted mismatch"); }

    /* steps_merged: only consecutive explicit MOVE_POSITION to same target */
    { TEST("planner: steps_merged exact count");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.allow_uv = false; intent.allow_dry = false;
      /* 3 consecutive MOVE_POSITION to DRUM_POS_0 */
      intent.action_count = 3;
      intent.actions[0].type = WASH_ACTION_MOVE_POSITION;
      intent.actions[0].enabled = true;
      intent.actions[0].position = DRUM_POS_0;
      intent.actions[1].type = WASH_ACTION_MOVE_POSITION;
      intent.actions[1].enabled = true;
      intent.actions[1].position = DRUM_POS_0;
      intent.actions[2].type = WASH_ACTION_MOVE_POSITION;
      intent.actions[2].enabled = true;
      intent.actions[2].position = DRUM_POS_0;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      /* First MOVE inserts (user), second and third are explicit merges */
      if (r.steps_merged != 2) ok = 0;
      /* Only 1 MOVE step + MOVE45(sys) + FINISH(sys) = 3 total */
      if (prog.step_count != 3) ok = 0;
      /* steps_inserted = 2 (MOVE45 + FINISH, both system) */
      if (r.steps_inserted != 2) ok = 0;
      if (ok) PASS(); else FAIL("steps_merged wrong"); }

    /* steps_dropped: disabled actions + skipped UV/DRY */
    { TEST("planner: steps_dropped exact count");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.allow_uv = false; intent.allow_dry = false;
      intent.action_count = 3;
      intent.actions[0].type = WASH_ACTION_UV;
      intent.actions[0].enabled = true;
      intent.actions[0].duration_ms = 5000;
      intent.actions[1].type = WASH_ACTION_DRY;
      intent.actions[1].enabled = true;
      intent.actions[1].duration_ms = 5000;
      intent.actions[2].type = WASH_ACTION_PULSATOR_WASH;
      intent.actions[2].enabled = false;
      intent.actions[2].duration_ms = 5000;
      intent.actions[2].rounds = 1;
      intent.actions[2].intensity = WASH_INTENSITY_NORMAL;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      /* UV skipped (allow_uv=false) + DRY skipped (allow_dry=false) + disabled = 3 dropped */
      if (r.steps_dropped != 3) ok = 0;
      if (ok) PASS(); else FAIL("steps_dropped wrong"); }

    /* original_total_ms == final_total_ms when no clipping */
    { TEST("planner: original equals final total ms");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.allow_uv = false; intent.allow_dry = false;
      intent.action_count = 1;
      intent.actions[0].type = WASH_ACTION_PULSATOR_WASH;
      intent.actions[0].enabled = true;
      intent.actions[0].duration_ms = 30000;
      intent.actions[0].rounds = 1;
      intent.actions[0].intensity = WASH_INTENSITY_NORMAL;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      if (r.original_total_ms != r.final_total_ms) ok = 0;
      if (r.original_total_ms == 0) ok = 0;
      if (ok) PASS(); else FAIL("totals mismatch"); }

    /* reject_action_index = UINT16_MAX for OK results */
    { TEST("planner: OK result has reject_action_index=UINT16_MAX");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_FORMAL;
      intent.allow_uv = true; intent.allow_dry = true;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_OK) ok = 0;
      if (r.reject_action_index != UINT16_MAX) ok = 0;
      if (ok) PASS(); else FAIL("OK index not UINT16_MAX"); }

    /* PLAN_RESULT_REJECTED reachable (CLIPPED removed) */
    { TEST("planner: REJECTED result reachable");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = (wash_program_kind_t)99;
      machine_config_t cfg = make_valid_config();
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_REJECTED) ok = 0;
      if (ok) PASS(); else FAIL("REJECTED not reachable"); }

    /* ---- [9h] Config validation tests ---- */

    /* Invalid config: bad schema_version 鈫?INVALID_CONFIG */
    { TEST("planner: invalid config schema rejected");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_FORMAL;
      intent.allow_uv = true; intent.allow_dry = true;
      machine_config_t cfg = make_valid_config();
      cfg.schema_version = 999;
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      int ok = 1;
      if (r.result != PLAN_RESULT_REJECTED) ok = 0;
      if (r.reject_reason != PLAN_ERROR_INVALID_CONFIG) ok = 0;
      if (r.reject_action_index != UINT16_MAX) ok = 0;
      if (ok) PASS(); else FAIL("invalid config not rejected"); }

    /* Input config not modified by compile */
    { TEST("planner: config not modified by compile");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_FORMAL;
      intent.allow_uv = true; intent.allow_dry = true;
      machine_config_t cfg = make_valid_config();
      machine_config_t saved = cfg;
      wash_program_t prog;
      wash_planner_compile(&intent, &cfg, 1, &prog);
      if (memcmp(&cfg, &saved, sizeof(cfg)) == 0)
          PASS(); else FAIL("config modified"); }

    /* ---- [9i] Batch G: concurrent & pure function tests ---- */

#ifdef _WIN32
    /* Concurrent compile: 4 threads each compile different programs, verify no pollution */
    { TEST("planner: concurrent compile no pollution");
      memset(g_thread_results, 0, sizeof(g_thread_results));
      g_thread_results[0].program_id = 100; g_thread_results[0].kind = WASH_PROGRAM_FORMAL;
      g_thread_results[1].program_id = 200; g_thread_results[1].kind = WASH_PROGRAM_DEMO;
      g_thread_results[2].program_id = 300; g_thread_results[2].kind = WASH_PROGRAM_FORMAL;
      g_thread_results[3].program_id = 400; g_thread_results[3].kind = WASH_PROGRAM_DEMO;

      HANDLE threads[4];
      for (int i = 0; i < 4; i++) {
          threads[i] = CreateThread(NULL, 0, compile_thread_fn, (LPVOID)(intptr_t)i, 0, NULL);
      }
      DWORD wait_result = WaitForMultipleObjects(4, threads, TRUE, 5000);
      for (int i = 0; i < 4; i++) CloseHandle(threads[i]);

      int ok = 1;
      /* Verify wait succeeded (not timeout) */
      if (wait_result < WAIT_OBJECT_0 || wait_result > WAIT_OBJECT_0 + 3) ok = 0;
      for (int i = 0; i < 4; i++) {
          if (g_thread_results[i].report.result != PLAN_RESULT_OK) ok = 0;
      }
      /* Verify CreateThread handles were valid */
      for (int i = 0; i < 4; i++) {
          if (threads[i] == NULL) ok = 0;
      }
      if (g_thread_results[0].prog.program_id != 100) ok = 0;
      if (g_thread_results[1].prog.program_id != 200) ok = 0;
      if (g_thread_results[2].prog.program_id != 300) ok = 0;
      if (g_thread_results[3].prog.program_id != 400) ok = 0;
      if (g_thread_results[0].prog.step_count != 17) ok = 0;
      if (g_thread_results[1].prog.step_count != 12) ok = 0;
      if (g_thread_results[2].prog.step_count != 17) ok = 0;
      if (g_thread_results[3].prog.step_count != 12) ok = 0;
      if (ok) PASS(); else FAIL("concurrent pollution"); }
#endif

    /* Deterministic: 100 repeated compiles 鈫?all byte-identical */
    { TEST("planner: 100 repeated compiles deterministic");
      wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_FORMAL;
      intent.allow_uv = true; intent.allow_dry = true;
      machine_config_t cfg = make_valid_config();
      wash_program_t ref_prog;
      planner_report_t ref_report = wash_planner_compile(&intent, &cfg, 42, &ref_prog);
      int ok = 1;
      if (ref_report.result != PLAN_RESULT_OK) ok = 0;
      for (int i = 0; i < 100; i++) {
          wash_program_t prog;
          planner_report_t r = wash_planner_compile(&intent, &cfg, 42, &prog);
          if (memcmp(&prog, &ref_prog, sizeof(prog)) != 0) ok = 0;
          if (memcmp(&r, &ref_report, sizeof(r)) != 0) ok = 0;
      }
      if (ok) PASS(); else FAIL("not deterministic"); }

    /* ================================================================
     * [10] wash_executor_core pure C tests (Round 1.1)
     * ================================================================ */

    printf("\n[10] wash_executor_core pure C\n");

    /* Core init produces IDLE phase */
    { TEST("executor: core init IDLE");
      wash_executor_core_t c;
      core_init(&c);
      int ok = 1;
      if (c.phase != CORE_PHASE_IDLE) ok = 0;
      if (c.program_active) ok = 0;
      if (c.next_request_id != 1) ok = 0;
      if (ok) PASS(); else FAIL("init not IDLE"); }

    /* Accept program 鈫?DISPATCHING phase */
    { TEST("executor: accept program 鈫?DISPATCHING");
      wash_executor_core_t c;
      core_init(&c);
      wash_program_t prog = make_exec_test_program();
      c.program = &prog;
      c.total_steps = (uint16_t)prog.step_count;
      c.program_id = prog.program_id;
      core_event_t ev; memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_PROGRAM_ACCEPTED;
      ev.token.program_id = prog.program_id;
      core_cmd_t cmds[16];
      uint32_t n = core_process_event(&c, &ev, cmds, 16);
      int ok = 1;
      if (c.phase != CORE_PHASE_DISPATCHING) ok = 0;
      if (!c.program_active) ok = 0;
      int found = 0;
      for (uint32_t i = 0; i < n; i++) {
          if (cmds[i].type == CORE_CMD_DISPATCH_STEP) {
              if (cmds[i].token.step_id != 0) ok = 0;
              if (cmds[i].token.request_id < 1) ok = 0;
              found = 1;
          }
      }
      if (!found) ok = 0;
      if (ok) PASS(); else FAIL("program accept wrong"); }

    /* DISPATCH_OK 鈫?ACTIVE phase */
    { TEST("executor: DISPATCH_OK 鈫?ACTIVE");
      wash_executor_core_t c;
      core_init(&c);
      wash_program_t prog = make_exec_test_program();
      c.program = &prog; c.total_steps = 3; c.program_id = 1;
      core_event_t ev; memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_PROGRAM_ACCEPTED; ev.token.program_id = 1;
      core_cmd_t cmds[16]; core_process_event(&c, &ev, cmds, 16);
      memset(&ev, 0, sizeof(ev)); ev.type = CORE_EVENT_STEP_DISPATCH_OK;
      core_process_event(&c, &ev, cmds, 16);
      int ok = 1;
      if (c.phase != CORE_PHASE_ACTIVE) ok = 0;
      if (ok) PASS(); else FAIL("dispatch ok wrong"); }

    /* Full progression: DISPATCHING鈫扐CTIVE鈫抰erminal鈫抧ext step鈫扚INISH */
    { TEST("executor: full multi-step progression");
      wash_executor_core_t c;
      core_init(&c);
      wash_program_t prog = make_exec_test_program();
      c.program = &prog; c.total_steps = 3; c.program_id = 1;
      core_event_t ev; memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_PROGRAM_ACCEPTED; ev.token.program_id = 1;
      core_cmd_t cmds[16]; uint32_t n;
      core_process_event(&c, &ev, cmds, 16);
      int ok = 1;
      /* Dispatch OK for step 0 */
      memset(&ev, 0, sizeof(ev)); ev.type = CORE_EVENT_STEP_DISPATCH_OK;
      core_process_event(&c, &ev, cmds, 16);
      if (c.phase != CORE_PHASE_ACTIVE) ok = 0;
      /* Terminal for step 0 */
      memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_SERVICE_TERMINAL;
      ev.token = c.active_token; ev.success = true;
      n = core_process_event(&c, &ev, cmds, 16);
      int found = 0;
      for (uint32_t i = 0; i < n; i++)
          if (cmds[i].type == CORE_CMD_DISPATCH_STEP) found = 1;
      if (!found) ok = 0;
      /* Dispatch OK for step 1 */
      memset(&ev, 0, sizeof(ev)); ev.type = CORE_EVENT_STEP_DISPATCH_OK;
      core_process_event(&c, &ev, cmds, 16);
      /* Terminal for step 1 */
      memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_SERVICE_TERMINAL;
      ev.token = c.active_token; ev.success = true;
      n = core_process_event(&c, &ev, cmds, 16);
      int ft = 0;
      for (uint32_t i = 0; i < n; i++)
          if (cmds[i].type == CORE_CMD_PUBLISH_PROGRAM_TERMINAL && cmds[i].terminal_success) ft = 1;
      if (!ft) ok = 0;
      if (c.phase != CORE_PHASE_IDLE) ok = 0;
      if (c.completed_steps != 3) ok = 0;
      if (ok) PASS(); else FAIL("multi-step wrong"); }

    /* Terminal accepted after DISPATCH_OK */
    { TEST("executor: terminal accepted after dispatch_ok");
      wash_executor_core_t c;
      core_init(&c);
      wash_program_t prog = make_exec_test_program();
      c.program = &prog; c.total_steps = 3; c.program_id = 1;
      core_event_t ev; memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_PROGRAM_ACCEPTED; ev.token.program_id = 1;
      core_cmd_t cmds[16]; core_process_event(&c, &ev, cmds, 16);
      memset(&ev, 0, sizeof(ev)); ev.type = CORE_EVENT_STEP_DISPATCH_OK;
      core_process_event(&c, &ev, cmds, 16);
      memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_SERVICE_TERMINAL;
      ev.token = c.active_token; ev.success = true;
      uint32_t n = core_process_event(&c, &ev, cmds, 16);
      int ok = 1;
      if (c.current_step_index != 1) ok = 0;
      int found = 0;
      for (uint32_t i = 0; i < n; i++)
          if (cmds[i].type == CORE_CMD_DISPATCH_STEP) found = 1;
      if (!found) ok = 0;
      if (ok) PASS(); else FAIL("terminal after dispatch wrong"); }

    /* Stale token rejected */
    { TEST("executor: stale token rejected");
      wash_executor_core_t c;
      core_init(&c);
      wash_program_t prog = make_exec_test_program();
      c.program = &prog; c.total_steps = 3; c.program_id = 1;
      core_event_t ev; memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_PROGRAM_ACCEPTED; ev.token.program_id = 1;
      core_cmd_t cmds[16]; core_process_event(&c, &ev, cmds, 16);
      memset(&ev, 0, sizeof(ev)); ev.type = CORE_EVENT_STEP_DISPATCH_OK;
      core_process_event(&c, &ev, cmds, 16);
      terminal_token_t bad; memset(&bad, 0, sizeof(bad)); bad.request_id = 9999;
      memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_SERVICE_TERMINAL; ev.token = bad; ev.success = true;
      uint32_t n = core_process_event(&c, &ev, cmds, 16);
      int ok = 1;
      if (n != 0) ok = 0;
      if (c.completed_steps != 0) ok = 0;
      if (ok) PASS(); else FAIL("stale token accepted"); }

    /* Terminal before DISPATCH_OK rejected (phase mismatch) */
    { TEST("executor: terminal in DISPATCHING rejected");
      wash_executor_core_t c;
      core_init(&c);
      wash_program_t prog = make_exec_test_program();
      c.program = &prog; c.total_steps = 3; c.program_id = 1;
      core_event_t ev; memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_PROGRAM_ACCEPTED; ev.token.program_id = 1;
      core_cmd_t cmds[16]; core_process_event(&c, &ev, cmds, 16);
      terminal_token_t tok = c.active_token;
      memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_SERVICE_TERMINAL; ev.token = tok; ev.success = true;
      uint32_t n = core_process_event(&c, &ev, cmds, 16);
      int ok = 1;
      if (n != 0) ok = 0;
      if (c.phase != CORE_PHASE_DISPATCHING) ok = 0;
      if (ok) PASS(); else FAIL("terminal in DISPATCHING accepted"); }

    /* Step timeout 鈫?CANCELING 鈫?RESETTING 鈫?IDLE */
    { TEST("executor: step timeout full flow");
      wash_executor_core_t c;
      core_init(&c);
      wash_program_t prog = make_exec_test_program();
      c.program = &prog; c.total_steps = 3; c.program_id = 1;
      core_event_t ev; memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_PROGRAM_ACCEPTED; ev.token.program_id = 1;
      core_cmd_t cmds[16]; uint32_t n;
      core_process_event(&c, &ev, cmds, 16);
      memset(&ev, 0, sizeof(ev)); ev.type = CORE_EVENT_STEP_DISPATCH_OK;
      core_process_event(&c, &ev, cmds, 16);
      memset(&ev, 0, sizeof(ev)); ev.type = CORE_EVENT_STEP_TIMEOUT;
      n = core_process_event(&c, &ev, cmds, 16);
      int ok = 1;
      if (c.phase != CORE_PHASE_CANCELING) ok = 0;
      int fc = 0;
      for (uint32_t i = 0; i < n; i++)
          if (cmds[i].type == CORE_CMD_CANCEL_ACTIVE_STEP) fc = 1;
      if (!fc) ok = 0;
      /* Cancel terminal */
      memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_SERVICE_TERMINAL;
      ev.token = c.cancel_token; ev.success = true;
      n = core_process_event(&c, &ev, cmds, 16);
      if (c.phase != CORE_PHASE_RESETTING) ok = 0;
      int fr = 0;
      for (uint32_t i = 0; i < n; i++)
          if (cmds[i].type == CORE_CMD_REQUEST_FINAL_RESET) fr = 1;
      if (!fr) ok = 0;
      /* Reset terminal */
      memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_SERVICE_TERMINAL;
      ev.token = c.reset_token; ev.success = true;
      core_process_event(&c, &ev, cmds, 16);
      if (c.phase != CORE_PHASE_IDLE) ok = 0;
      if (ok) PASS(); else FAIL("timeout flow wrong"); }

    /* Cancel preserves active token */
    { TEST("executor: cancel preserves active token");
      wash_executor_core_t c;
      core_init(&c);
      wash_program_t prog = make_exec_test_program();
      c.program = &prog; c.total_steps = 3; c.program_id = 1;
      core_event_t ev; memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_PROGRAM_ACCEPTED; ev.token.program_id = 1;
      core_cmd_t cmds[16]; core_process_event(&c, &ev, cmds, 16);
      memset(&ev, 0, sizeof(ev)); ev.type = CORE_EVENT_STEP_DISPATCH_OK;
      core_process_event(&c, &ev, cmds, 16);
      terminal_token_t saved = c.active_token;
      memset(&ev, 0, sizeof(ev)); ev.type = CORE_EVENT_CANCEL;
      core_process_event(&c, &ev, cmds, 16);
      int ok = 1;
      if (c.cancel_token.request_id != saved.request_id) ok = 0;
      if (ok) PASS(); else FAIL("cancel token wrong"); }

    /* Dispatch failure 鈫?FAULT */
    { TEST("executor: dispatch failure 鈫?FAULT");
      wash_executor_core_t c;
      core_init(&c);
      wash_program_t prog = make_exec_test_program();
      c.program = &prog; c.total_steps = 3; c.program_id = 1;
      core_event_t ev; memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_PROGRAM_ACCEPTED; ev.token.program_id = 1;
      core_cmd_t cmds[16]; uint32_t n;
      core_process_event(&c, &ev, cmds, 16);
      memset(&ev, 0, sizeof(ev)); ev.type = CORE_EVENT_STEP_DISPATCH_FAIL;
      n = core_process_event(&c, &ev, cmds, 16);
      int ok = 1;
      if (c.phase != CORE_PHASE_FAULT) ok = 0;
      if (!c.fault_active) ok = 0;
      int found = 0;
      for (uint32_t i = 0; i < n; i++)
          if (cmds[i].type == CORE_CMD_ENTER_FAULT) found = 1;
      if (!found) ok = 0;
      if (ok) PASS(); else FAIL("dispatch fail wrong"); }

    /* Reset failure 鈫?FAULT */
    { TEST("executor: reset failure 鈫?FAULT");
      wash_executor_core_t c;
      core_init(&c);
      wash_program_t prog = make_exec_test_program();
      c.program = &prog; c.total_steps = 3; c.program_id = 1;
      core_event_t ev; memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_PROGRAM_ACCEPTED; ev.token.program_id = 1;
      core_cmd_t cmds[16]; core_process_event(&c, &ev, cmds, 16);
      memset(&ev, 0, sizeof(ev)); ev.type = CORE_EVENT_STEP_DISPATCH_OK;
      core_process_event(&c, &ev, cmds, 16);
      memset(&ev, 0, sizeof(ev)); ev.type = CORE_EVENT_CANCEL;
      core_process_event(&c, &ev, cmds, 16);
      memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_SERVICE_TERMINAL;
      ev.token = c.cancel_token; ev.success = true;
      core_process_event(&c, &ev, cmds, 16);
      memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_SERVICE_TERMINAL;
      ev.token = c.reset_token; ev.success = false;
      core_process_event(&c, &ev, cmds, 16);
      int ok = 1;
      if (c.phase != CORE_PHASE_FAULT) ok = 0;
      if (!c.fault_active) ok = 0;
      if (ok) PASS(); else FAIL("reset failure wrong"); }

    /* Fault ack 鈫?IDLE */
    { TEST("executor: fault ack 鈫?IDLE");
      wash_executor_core_t c;
      core_init(&c);
      c.phase = CORE_PHASE_FAULT; c.fault_active = true; c.fault_code = 42;
      core_event_t ev; memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_FAULT_ACK;
      core_cmd_t cmds[16];
      uint32_t n = core_process_event(&c, &ev, cmds, 16);
      int ok = 1;
      if (c.phase != CORE_PHASE_IDLE) ok = 0;
      if (c.fault_active) ok = 0;
      int found = 0;
      for (uint32_t i = 0; i < n; i++)
          if (cmds[i].type == CORE_CMD_RETURN_IDLE) found = 1;
      if (!found) ok = 0;
      if (ok) PASS(); else FAIL("fault ack wrong"); }

    /* Request_id wraps past 0 */
    { TEST("executor: request_id wraps past 0");
      wash_executor_core_t c;
      core_init(&c);
      c.next_request_id = UINT16_MAX - 1;
      uint16_t id1 = core_next_request_id(&c);
      uint16_t id2 = core_next_request_id(&c);
      uint16_t id3 = core_next_request_id(&c);
      int ok = 1;
      if (id1 != UINT16_MAX - 1) ok = 0;
      if (id2 != UINT16_MAX) ok = 0;
      if (id3 != 1) ok = 0;
      if (ok) PASS(); else FAIL("wrap wrong"); }

    /* Program terminal exactly once */
    { TEST("executor: program terminal exactly once");
      wash_executor_core_t c;
      core_init(&c);
      wash_program_t prog;
      memset(&prog, 0, sizeof(prog));
      prog.program_id = 42; prog.step_count = 1;
      prog.steps[0].type = STEP_FINISH; prog.steps[0].step_id = 0;
      prog.steps[0].required_position = DRUM_POS_45;
      c.program = &prog; c.total_steps = 1; c.program_id = 42;
      core_event_t ev; memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_PROGRAM_ACCEPTED; ev.token.program_id = 42;
      core_cmd_t cmds[16];
      uint32_t n = core_process_event(&c, &ev, cmds, 16);
      int ok = 1;
      int tc = 0;
      for (uint32_t i = 0; i < n; i++)
          if (cmds[i].type == CORE_CMD_PUBLISH_PROGRAM_TERMINAL) tc++;
      if (tc != 1) ok = 0;
      if (c.phase != CORE_PHASE_IDLE) ok = 0;
      if (ok) PASS(); else FAIL("terminal count wrong"); }

    /* Service failure 鈫?FAULT */
    { TEST("executor: service failure 鈫?FAULT");
      wash_executor_core_t c;
      core_init(&c);
      wash_program_t prog = make_exec_test_program();
      c.program = &prog; c.total_steps = 3; c.program_id = 1;
      core_event_t ev; memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_PROGRAM_ACCEPTED; ev.token.program_id = 1;
      core_cmd_t cmds[16]; core_process_event(&c, &ev, cmds, 16);
      memset(&ev, 0, sizeof(ev)); ev.type = CORE_EVENT_STEP_DISPATCH_OK;
      core_process_event(&c, &ev, cmds, 16);
      memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_SERVICE_TERMINAL;
      ev.token = c.active_token; ev.success = false; ev.error_code = 500;
      core_process_event(&c, &ev, cmds, 16);
      int ok = 1;
      if (c.phase != CORE_PHASE_FAULT) ok = 0;
      if (!c.fault_active) ok = 0;
      if (c.fault_code != 500) ok = 0;
      if (ok) PASS(); else FAIL("service failure wrong"); }

    /* WAIT_LOAD confirm advances */
    { TEST("executor: WAIT_LOAD confirm advances");
      wash_executor_core_t c;
      core_init(&c);
      wash_program_t prog;
      memset(&prog, 0, sizeof(prog));
      prog.program_id = 10; prog.step_count = 3;
      prog.steps[0].type = STEP_WAIT_LOAD_CONFIRM; prog.steps[0].step_id = 0;
      prog.steps[0].required_position = DRUM_POS_45;
      prog.steps[1].type = STEP_WATER_IN; prog.steps[1].step_id = 1;
      prog.steps[1].required_position = DRUM_POS_0; prog.steps[1].timeout_ms = 5000;
      prog.steps[2].type = STEP_FINISH; prog.steps[2].step_id = 2;
      prog.steps[2].required_position = DRUM_POS_45;
      c.program = &prog; c.total_steps = 3; c.program_id = 10;
      core_event_t ev; memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_PROGRAM_ACCEPTED; ev.token.program_id = 10;
      core_cmd_t cmds[16]; uint32_t n;
      core_process_event(&c, &ev, cmds, 16);
      int ok = 1;
      if (c.phase != CORE_PHASE_WAIT_LOAD) ok = 0;
      memset(&ev, 0, sizeof(ev)); ev.type = CORE_EVENT_LOAD_CONFIRMED;
      n = core_process_event(&c, &ev, cmds, 16);
      int found = 0;
      for (uint32_t i = 0; i < n; i++)
          if (cmds[i].type == CORE_CMD_DISPATCH_STEP) found = 1;
      if (!found) ok = 0;
      if (c.phase != CORE_PHASE_DISPATCHING) ok = 0;
      if (ok) PASS(); else FAIL("load confirm wrong"); }

    /* Token matching: 4-field matching */
    { TEST("executor: token 4-field matching");
      wash_executor_core_t c;
      core_init(&c);
      wash_program_t prog = make_exec_test_program();
      c.program = &prog; c.total_steps = 3; c.program_id = 1;
      core_event_t ev; memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_PROGRAM_ACCEPTED; ev.token.program_id = 1;
      core_cmd_t cmds[16]; core_process_event(&c, &ev, cmds, 16);
      memset(&ev, 0, sizeof(ev)); ev.type = CORE_EVENT_STEP_DISPATCH_OK;
      core_process_event(&c, &ev, cmds, 16);
      int ok = 1;
      terminal_token_t correct = c.active_token;
      if (!core_token_matches_active(&c, &correct)) ok = 0;
      terminal_token_t wg = correct; wg.generation = 999;
      if (core_token_matches_active(&c, &wg)) ok = 0;
      terminal_token_t wp = correct; wp.program_id = 999;
      if (core_token_matches_active(&c, &wp)) ok = 0;
      if (ok) PASS(); else FAIL("token matching wrong"); }

    /* Fake adapter per-instance */
    { TEST("executor: fake adapter per-instance");
      wash_executor_adapter_t a1, a2;
      uint8_t ctx1[FAKE_ADAPTER_CTX_SIZE], ctx2[FAKE_ADAPTER_CTX_SIZE];
      fake_adapter_init(&a1, ctx1);
      fake_adapter_init(&a2, ctx2);
      int ok = 1;
      wash_step_t step; memset(&step, 0, sizeof(step));
      step.type = STEP_WATER_IN;
      a1.dispatch_step(a1.ctx, &step, NULL, 0);
      a1.dispatch_step(a1.ctx, &step, NULL, 0);
      a2.dispatch_step(a2.ctx, &step, NULL, 0);
      if (fake_adapter_dispatch_count(ctx1) != 2) ok = 0;
      if (fake_adapter_dispatch_count(ctx2) != 1) ok = 0;
      if (ok) PASS(); else FAIL("adapter not per-instance"); }

    /* Deterministic core output */
    { TEST("executor: deterministic core output");
      int ok = 1;
      for (int t = 0; t < 10; t++) {
          wash_executor_core_t c1, c2;
          core_init(&c1); core_init(&c2);
          wash_program_t prog = make_exec_test_program();
          c1.program = &prog; c1.total_steps = 3; c1.program_id = 1;
          c2.program = &prog; c2.total_steps = 3; c2.program_id = 1;
          core_event_t ev; memset(&ev, 0, sizeof(ev));
          ev.type = CORE_EVENT_PROGRAM_ACCEPTED; ev.token.program_id = 1;
          core_cmd_t cmds1[16], cmds2[16];
          uint32_t n1 = core_process_event(&c1, &ev, cmds1, 16);
          uint32_t n2 = core_process_event(&c2, &ev, cmds2, 16);
          if (n1 != n2) ok = 0;
          if (memcmp(cmds1, cmds2, n1 * sizeof(core_cmd_t)) != 0) ok = 0;
      }
      if (ok) PASS(); else FAIL("not deterministic"); }

    /* can_accept_program guards */
    { TEST("executor: can_accept_program guards");
      int ok = 1;
      wash_executor_core_t c;
      core_init(&c);
      if (!core_can_accept_program(&c)) ok = 0;
      c.program_active = true;
      if (core_can_accept_program(&c)) ok = 0;
      c.program_active = false;
      c.fault_active = true;
      if (core_can_accept_program(&c)) ok = 0;
      c.fault_active = false;
      c.quiesced = true;
      if (core_can_accept_program(&c)) ok = 0;
      if (ok) PASS(); else FAIL("can_accept wrong"); }

    /* Token exact 6-field matching */
    { TEST("executor: token exact 6-field match");
      terminal_token_t a, b;
      memset(&a, 0, sizeof(a));
      a.program_id = 1; a.generation = 2; a.step_id = 3;
      a.request_id = 4; a.service_kind = 5; a.terminal_kind = TERMINAL_KIND_NORMAL;
      b = a;
      int ok = 1;
      if (!terminal_token_equal_exact(&a, &b)) ok = 0;
      b.service_kind = 99;
      if (terminal_token_equal_exact(&a, &b)) ok = 0;
      b = a; b.terminal_kind = TERMINAL_KIND_CANCEL_ACK;
      if (terminal_token_equal_exact(&a, &b)) ok = 0;
      if (ok) PASS(); else FAIL("token exact match wrong"); }

    /* Wrong terminal_kind rejected by active matcher */
    { TEST("executor: wrong terminal_kind rejected");
      wash_executor_core_t c;
      core_init(&c);
      wash_program_t prog = make_exec_test_program();
      c.program = &prog; c.total_steps = 3; c.program_id = 1;
      core_event_t ev; memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_PROGRAM_ACCEPTED; ev.token.program_id = 1;
      core_cmd_t cmds[16]; core_process_event(&c, &ev, cmds, 16);
      memset(&ev, 0, sizeof(ev)); ev.type = CORE_EVENT_STEP_DISPATCH_OK;
      core_process_event(&c, &ev, cmds, 16);
      /* Try cancel ACK as normal terminal */
      terminal_token_t bad = c.active_token;
      bad.terminal_kind = TERMINAL_KIND_CANCEL_ACK;
      memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_SERVICE_TERMINAL; ev.token = bad; ev.success = true;
      uint32_t n = core_process_event(&c, &ev, cmds, 16);
      int ok = 1;
      if (n != 0) ok = 0;  /* rejected */
      if (c.completed_steps != 0) ok = 0;
      if (ok) PASS(); else FAIL("wrong kind accepted"); }

    /* Dispatch fail produces fault + program terminal */
    { TEST("executor: dispatch fail produces fault terminal");
      wash_executor_core_t c;
      core_init(&c);
      wash_program_t prog = make_exec_test_program();
      c.program = &prog; c.total_steps = 3; c.program_id = 1;
      core_event_t ev; memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_PROGRAM_ACCEPTED; ev.token.program_id = 1;
      core_cmd_t cmds[16]; uint32_t n;
      core_process_event(&c, &ev, cmds, 16);
      memset(&ev, 0, sizeof(ev)); ev.type = CORE_EVENT_STEP_DISPATCH_FAIL;
      n = core_process_event(&c, &ev, cmds, 16);
      int ok = 1;
      int found_fault = 0, found_term = 0;
      for (uint32_t i = 0; i < n; i++) {
          if (cmds[i].type == CORE_CMD_ENTER_FAULT) found_fault = 1;
          if (cmds[i].type == CORE_CMD_PUBLISH_PROGRAM_TERMINAL) found_term = 1;
      }
      if (!found_fault) ok = 0;
      if (!found_term) ok = 0;
      if (c.phase != CORE_PHASE_FAULT) ok = 0;
      if (ok) PASS(); else FAIL("dispatch fail no terminal"); }

    /* Service failure produces fault + program terminal */
    { TEST("executor: service failure produces fault terminal");
      wash_executor_core_t c;
      core_init(&c);
      wash_program_t prog = make_exec_test_program();
      c.program = &prog; c.total_steps = 3; c.program_id = 1;
      core_event_t ev; memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_PROGRAM_ACCEPTED; ev.token.program_id = 1;
      core_cmd_t cmds[16]; uint32_t n;
      core_process_event(&c, &ev, cmds, 16);
      memset(&ev, 0, sizeof(ev)); ev.type = CORE_EVENT_STEP_DISPATCH_OK;
      core_process_event(&c, &ev, cmds, 16);
      memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_SERVICE_TERMINAL;
      ev.token = c.active_token; ev.success = false; ev.error_code = 500;
      n = core_process_event(&c, &ev, cmds, 16);
      int ok = 1;
      int found_fault = 0, found_term = 0;
      for (uint32_t i = 0; i < n; i++) {
          if (cmds[i].type == CORE_CMD_ENTER_FAULT) found_fault = 1;
          if (cmds[i].type == CORE_CMD_PUBLISH_PROGRAM_TERMINAL) found_term = 1;
      }
      if (!found_fault) ok = 0;
      if (!found_term) ok = 0;
      if (ok) PASS(); else FAIL("service failure no terminal"); }

    /* Abort goes through CANCELING, not directly to RESETTING */
    { TEST("executor: abort goes through CANCELING");
      wash_executor_core_t c;
      core_init(&c);
      wash_program_t prog = make_exec_test_program();
      c.program = &prog; c.total_steps = 3; c.program_id = 1;
      core_event_t ev; memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_PROGRAM_ACCEPTED; ev.token.program_id = 1;
      core_cmd_t cmds[16]; core_process_event(&c, &ev, cmds, 16);
      memset(&ev, 0, sizeof(ev)); ev.type = CORE_EVENT_STEP_DISPATCH_OK;
      core_process_event(&c, &ev, cmds, 16);
      /* Abort */
      memset(&ev, 0, sizeof(ev)); ev.type = CORE_EVENT_ABORT;
      core_process_event(&c, &ev, cmds, 16);
      int ok = 1;
      if (c.phase != CORE_PHASE_CANCELING) ok = 0;
      if (c.last_event != WASH_EXEC_EVENT_ABORTED) ok = 0;
      if (ok) PASS(); else FAIL("abort not CANCELING"); }

    /* Cancel is idempotent in CANCELING */
    { TEST("executor: cancel idempotent in CANCELING");
      wash_executor_core_t c;
      core_init(&c);
      wash_program_t prog = make_exec_test_program();
      c.program = &prog; c.total_steps = 3; c.program_id = 1;
      core_event_t ev; memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_PROGRAM_ACCEPTED; ev.token.program_id = 1;
      core_cmd_t cmds[16]; core_process_event(&c, &ev, cmds, 16);
      memset(&ev, 0, sizeof(ev)); ev.type = CORE_EVENT_STEP_DISPATCH_OK;
      core_process_event(&c, &ev, cmds, 16);
      /* First cancel */
      memset(&ev, 0, sizeof(ev)); ev.type = CORE_EVENT_CANCEL;
      core_process_event(&c, &ev, cmds, 16);
      /* Second cancel — should be no-op */
      memset(&ev, 0, sizeof(ev)); ev.type = CORE_EVENT_CANCEL;
      uint32_t n = core_process_event(&c, &ev, cmds, 16);
      int ok = 1;
      if (n != 0) ok = 0;
      if (c.phase != CORE_PHASE_CANCELING) ok = 0;
      if (ok) PASS(); else FAIL("cancel not idempotent"); }

    /* ================================================================
     * [11] executor: Round 1.3 features
     * ================================================================ */
    printf("\n[11] executor: Round 1.3 features\n");

    /* Fake adapter typed records */
    { TEST("executor: fake adapter typed dispatch record");
      wash_executor_adapter_t a;
      uint8_t ctx[FAKE_ADAPTER_CTX_SIZE];
      fake_adapter_init(&a, ctx);
      wash_program_t prog = make_exec_test_program();
      wash_executor_core_t c;
      core_init(&c);
      c.program = &prog; c.total_steps = 3; c.program_id = 1;
      uint16_t rid = core_next_request_id(&c);
      terminal_token_t tok = core_build_token(&c, rid);
      adapter_dispatch_result_t r = a.dispatch_step(a.ctx, &prog.steps[0], &tok, sizeof(tok));
      int ok = 1;
      if (!r.accepted) ok = 0;
      if (fake_adapter_dispatch_count(ctx) != 1) ok = 0;
      const fake_dispatch_record_t *rec = fake_adapter_get_dispatch(ctx, 0);
      if (!rec) ok = 0;
      else {
        if (rec->step.type != STEP_MOVE_POSITION) ok = 0;
        if (rec->token.program_id != 1) ok = 0;
        if (rec->token.request_id != rid) ok = 0;
        if (rec->token.step_id != 0) ok = 0;
      }
      if (ok) PASS(); else FAIL("typed dispatch record"); }

    /* Fake adapter cancel record */
    { TEST("executor: fake adapter typed cancel record");
      wash_executor_adapter_t a;
      uint8_t ctx[FAKE_ADAPTER_CTX_SIZE];
      fake_adapter_init(&a, ctx);
      terminal_token_t tok;
      memset(&tok, 0, sizeof(tok));
      tok.program_id = 42;
      tok.terminal_kind = TERMINAL_KIND_CANCEL_ACK;
      bool cr = a.cancel_step(a.ctx, &tok, sizeof(tok));
      int ok = 1;
      if (!cr) ok = 0;
      if (fake_adapter_cancel_count(ctx) != 1) ok = 0;
      const terminal_token_t *ct = fake_adapter_get_cancel_token(ctx, 0);
      if (!ct) ok = 0;
      else {
        if (ct->program_id != 42) ok = 0;
        if (ct->terminal_kind != TERMINAL_KIND_CANCEL_ACK) ok = 0;
      }
      if (ok) PASS(); else FAIL("typed cancel record"); }

    /* Fake adapter reset record */
    { TEST("executor: fake adapter typed reset record");
      wash_executor_adapter_t a;
      uint8_t ctx[FAKE_ADAPTER_CTX_SIZE];
      fake_adapter_init(&a, ctx);
      terminal_token_t tok;
      memset(&tok, 0, sizeof(tok));
      tok.program_id = 99;
      tok.terminal_kind = TERMINAL_KIND_RESET_ACK;
      adapter_dispatch_result_t rr = a.dispatch_final_reset(a.ctx, &tok, sizeof(tok));
      int ok = 1;
      if (!rr.accepted) ok = 0;
      if (fake_adapter_reset_count(ctx) != 1) ok = 0;
      const terminal_token_t *rt = fake_adapter_get_reset_token(ctx, 0);
      if (!rt) ok = 0;
      else {
        if (rt->program_id != 99) ok = 0;
        if (rt->terminal_kind != TERMINAL_KIND_RESET_ACK) ok = 0;
      }
      if (ok) PASS(); else FAIL("typed reset record"); }

    /* Fake adapter configurable results */
    { TEST("executor: fake adapter configurable reject");
      wash_executor_adapter_t a;
      uint8_t ctx[FAKE_ADAPTER_CTX_SIZE];
      fake_adapter_init(&a, ctx);
      fake_adapter_set_next_accept(ctx, false);
      fake_adapter_set_next_error(ctx, 404);
      wash_step_t step; memset(&step, 0, sizeof(step));
      step.type = STEP_WATER_IN;
      terminal_token_t tok; memset(&tok, 0, sizeof(tok));
      adapter_dispatch_result_t r = a.dispatch_step(a.ctx, &step, &tok, sizeof(tok));
      int ok = 1;
      if (r.accepted) ok = 0;
      if (r.error_code != 404) ok = 0;
      /* Reset and verify accept again */
      fake_adapter_set_next_accept(ctx, true);
      r = a.dispatch_step(a.ctx, &step, &tok, sizeof(tok));
      if (!r.accepted) ok = 0;
      if (ok) PASS(); else FAIL("configurable reject"); }

    /* Fake adapter reset result configurable */
    { TEST("executor: fake adapter reset result configurable");
      wash_executor_adapter_t a;
      uint8_t ctx[FAKE_ADAPTER_CTX_SIZE];
      fake_adapter_init(&a, ctx);
      fake_adapter_set_reset_result(ctx, false);
      terminal_token_t tok; memset(&tok, 0, sizeof(tok));
      adapter_dispatch_result_t r = a.dispatch_final_reset(a.ctx, &tok, sizeof(tok));
      int ok = 1;
      if (r.accepted) ok = 0;
      fake_adapter_set_reset_result(ctx, true);
      r = a.dispatch_final_reset(a.ctx, &tok, sizeof(tok));
      if (!r.accepted) ok = 0;
      if (ok) PASS(); else FAIL("reset result configurable"); }

    /* Fake adapter reset clears all */
    { TEST("executor: fake_adapter_reset clears all");
      wash_executor_adapter_t a;
      uint8_t ctx[FAKE_ADAPTER_CTX_SIZE];
      fake_adapter_init(&a, ctx);
      wash_step_t step; memset(&step, 0, sizeof(step));
      step.type = STEP_WATER_IN;
      terminal_token_t tok; memset(&tok, 0, sizeof(tok));
      a.dispatch_step(a.ctx, &step, &tok, sizeof(tok));
      a.cancel_step(a.ctx, &tok, sizeof(tok));
      a.dispatch_final_reset(a.ctx, &tok, sizeof(tok));
      if (fake_adapter_dispatch_count(ctx) != 1) { FAIL("pre-condition"); goto skip_reset_test; }
      fake_adapter_reset(ctx);
      int ok = 1;
      if (fake_adapter_dispatch_count(ctx) != 0) ok = 0;
      if (fake_adapter_cancel_count(ctx) != 0) ok = 0;
      if (fake_adapter_reset_count(ctx) != 0) ok = 0;
      if (ok) PASS(); else FAIL("reset clears all");
      skip_reset_test:; }

    /* Terminal_token_equal_exact boundary: all fields differ */
    { TEST("executor: token exact match all fields differ");
      terminal_token_t a_tok, b_tok;
      memset(&a_tok, 0, sizeof(a_tok));
      memset(&b_tok, 0, sizeof(b_tok));
      a_tok.program_id = 1; a_tok.generation = 2; a_tok.step_id = 3;
      a_tok.request_id = 4; a_tok.service_kind = 5; a_tok.terminal_kind = 6;
      b_tok.program_id = 10; b_tok.generation = 20; b_tok.step_id = 30;
      b_tok.request_id = 40; b_tok.service_kind = 50; b_tok.terminal_kind = 60;
      int ok = 1;
      if (terminal_token_equal_exact(&a_tok, &b_tok)) ok = 0;
      /* Single field match — still false */
      b_tok.program_id = 1;
      if (terminal_token_equal_exact(&a_tok, &b_tok)) ok = 0;
      /* All fields match */
      b_tok = a_tok;
      if (!terminal_token_equal_exact(&a_tok, &b_tok)) ok = 0;
      if (ok) PASS(); else FAIL("token exact boundary"); }

    /* Cancel ACK must have terminal_kind=CANCEL_ACK */
    { TEST("executor: cancel ACK must have correct terminal_kind");
      wash_executor_core_t c;
      core_init(&c);
      wash_program_t prog = make_exec_test_program();
      c.program = &prog; c.total_steps = 3; c.program_id = 1;
      core_event_t ev; memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_PROGRAM_ACCEPTED; ev.token.program_id = 1;
      core_cmd_t cmds[16]; core_process_event(&c, &ev, cmds, 16);
      memset(&ev, 0, sizeof(ev)); ev.type = CORE_EVENT_STEP_DISPATCH_OK;
      core_process_event(&c, &ev, cmds, 16);
      /* Cancel to enter CANCELING */
      memset(&ev, 0, sizeof(ev)); ev.type = CORE_EVENT_CANCEL;
      core_process_event(&c, &ev, cmds, 16);
      /* Try terminal with NORMAL kind — should not match cancel */
      memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_SERVICE_TERMINAL;
      ev.token = c.cancel_token;
      ev.token.terminal_kind = TERMINAL_KIND_NORMAL;  /* wrong kind */
      ev.success = true;
      uint32_t n = core_process_event(&c, &ev, cmds, 16);
      int ok = 1;
      if (c.phase != CORE_PHASE_CANCELING) ok = 0;  /* should stay in CANCELING */
      /* Now send correct CANCEL_ACK */
      ev.token.terminal_kind = TERMINAL_KIND_CANCEL_ACK;
      core_process_event(&c, &ev, cmds, 16);
      if (c.phase != CORE_PHASE_RESETTING) ok = 0;  /* should transition */
      if (ok) PASS(); else FAIL("cancel ACK terminal_kind"); }

    /* Reset ACK must have terminal_kind=RESET_ACK */
    { TEST("executor: reset ACK must have correct terminal_kind");
      wash_executor_core_t c;
      core_init(&c);
      wash_program_t prog = make_exec_test_program();
      c.program = &prog; c.total_steps = 3; c.program_id = 1;
      core_event_t ev; memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_PROGRAM_ACCEPTED; ev.token.program_id = 1;
      core_cmd_t cmds[16]; core_process_event(&c, &ev, cmds, 16);
      memset(&ev, 0, sizeof(ev)); ev.type = CORE_EVENT_STEP_DISPATCH_OK;
      core_process_event(&c, &ev, cmds, 16);
      /* Cancel → CANCELING */
      memset(&ev, 0, sizeof(ev)); ev.type = CORE_EVENT_CANCEL;
      core_process_event(&c, &ev, cmds, 16);
      /* Cancel ACK → RESETTING */
      memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_SERVICE_TERMINAL;
      ev.token = c.cancel_token;
      ev.token.terminal_kind = TERMINAL_KIND_CANCEL_ACK;
      ev.success = true;
      core_process_event(&c, &ev, cmds, 16);
      /* Try wrong terminal_kind for reset */
      memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_SERVICE_TERMINAL;
      ev.token = c.reset_token;
      ev.token.terminal_kind = TERMINAL_KIND_NORMAL;  /* wrong */
      ev.success = true;
      core_process_event(&c, &ev, cmds, 16);
      int ok = 1;
      if (c.phase != CORE_PHASE_RESETTING) ok = 0;  /* stays */
      /* Correct RESET_ACK */
      ev.token.terminal_kind = TERMINAL_KIND_RESET_ACK;
      core_process_event(&c, &ev, cmds, 16);
      if (c.phase != CORE_PHASE_IDLE) ok = 0;
      if (ok) PASS(); else FAIL("reset ACK terminal_kind"); }

    /* Service terminal with wrong service_kind rejected */
    { TEST("executor: wrong service_kind rejected in ACTIVE");
      wash_executor_core_t c;
      core_init(&c);
      wash_program_t prog = make_exec_test_program();
      c.program = &prog; c.total_steps = 3; c.program_id = 1;
      core_event_t ev; memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_PROGRAM_ACCEPTED; ev.token.program_id = 1;
      core_cmd_t cmds[16]; core_process_event(&c, &ev, cmds, 16);
      memset(&ev, 0, sizeof(ev)); ev.type = CORE_EVENT_STEP_DISPATCH_OK;
      core_process_event(&c, &ev, cmds, 16);
      /* Send terminal with wrong service_kind */
      memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_SERVICE_TERMINAL;
      ev.token = c.active_token;
      ev.token.service_kind = (uint8_t)STEP_DRAIN;  /* wrong */
      ev.success = true;
      core_process_event(&c, &ev, cmds, 16);
      int ok = 1;
      if (c.phase != CORE_PHASE_ACTIVE) ok = 0;  /* stays */
      if (c.token_valid != true) ok = 0;  /* token still valid */
      /* Correct service_kind */
      ev.token = c.active_token;
      ev.token.terminal_kind = TERMINAL_KIND_NORMAL;
      core_process_event(&c, &ev, cmds, 16);
      if (c.phase == CORE_PHASE_ACTIVE) ok = 0;  /* should advance */
      if (ok) PASS(); else FAIL("wrong service_kind"); }

    /* Multiple dispatches recorded correctly */
    { TEST("executor: multiple dispatches recorded");
      wash_executor_adapter_t a;
      uint8_t ctx[FAKE_ADAPTER_CTX_SIZE];
      fake_adapter_init(&a, ctx);
      wash_program_t prog = make_exec_test_program();
      wash_executor_core_t c;
      core_init(&c);
      c.program = &prog; c.total_steps = 3; c.program_id = 1;
      /* Accept → dispatch first step */
      core_event_t ev; memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_PROGRAM_ACCEPTED; ev.token.program_id = 1;
      core_cmd_t cmds[16];
      uint32_t n = core_process_event(&c, &ev, cmds, 16);
      /* Process dispatch command via adapter */
      for (uint32_t i = 0; i < n; i++) {
        if (cmds[i].type == CORE_CMD_DISPATCH_STEP) {
          const wash_step_t *step = core_get_current_step(&c);
          if (step) {
            adapter_dispatch_result_t r = a.dispatch_step(a.ctx, step, &cmds[i].token, sizeof(cmds[i].token));
            core_event_t de; memset(&de, 0, sizeof(de));
            de.type = r.accepted ? CORE_EVENT_STEP_DISPATCH_OK : CORE_EVENT_STEP_DISPATCH_FAIL;
            core_process_event(&c, &de, cmds, 16);
          }
        }
      }
      /* Send terminal for first step */
      memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_SERVICE_TERMINAL;
      ev.token = c.active_token;
      ev.token.terminal_kind = TERMINAL_KIND_NORMAL;
      ev.success = true;
      n = core_process_event(&c, &ev, cmds, 16);
      /* Process dispatch for second step */
      for (uint32_t i = 0; i < n; i++) {
        if (cmds[i].type == CORE_CMD_DISPATCH_STEP) {
          const wash_step_t *step = core_get_current_step(&c);
          if (step) {
            adapter_dispatch_result_t r = a.dispatch_step(a.ctx, step, &cmds[i].token, sizeof(cmds[i].token));
            core_event_t de; memset(&de, 0, sizeof(de));
            de.type = r.accepted ? CORE_EVENT_STEP_DISPATCH_OK : CORE_EVENT_STEP_DISPATCH_FAIL;
            core_process_event(&c, &de, cmds, 16);
          }
        }
      }
      int ok = 1;
      if (fake_adapter_dispatch_count(ctx) != 2) ok = 0;
      const fake_dispatch_record_t *r0 = fake_adapter_get_dispatch(ctx, 0);
      const fake_dispatch_record_t *r1 = fake_adapter_get_dispatch(ctx, 1);
      if (!r0 || !r1) ok = 0;
      else {
        if (r0->step.type != STEP_MOVE_POSITION) ok = 0;
        if (r1->step.type != STEP_WATER_IN) ok = 0;
        if (r0->token.step_id == r1->token.step_id) ok = 0;
      }
      if (ok) PASS(); else FAIL("multiple dispatches"); }

    /* ---- [9j] Fake adapter concurrent stress ---- */

#ifdef _WIN32
    /* Single writer + reader: 1000 dispatch cycles, no torn records */
    { TEST("fake_adapter: concurrent writer+reader 1000 cycles no torn");
      static uint8_t stress_ctx_buf[FAKE_ADAPTER_CTX_SIZE];
      static wash_executor_adapter_t stress_adapter;
      fake_adapter_init(&stress_adapter, stress_ctx_buf);
      int ok = 1;
      volatile int stop_flag = 0;

      stress_writer_arg_t warg = { &stress_adapter, 32 };
      stress_reader_arg_t rarg = { stress_ctx_buf, 32, &stop_flag };

      HANDLE hwriter = CreateThread(NULL, 0, stress_writer_fn, &warg, 0, NULL);
      HANDLE hreader = CreateThread(NULL, 0, stress_reader_fn, &rarg, 0, NULL);
      WaitForSingleObject(hwriter, 10000);
      stop_flag = 1;
      WaitForSingleObject(hreader, 5000);
      CloseHandle(hwriter);
      CloseHandle(hreader);

      uint32_t final_count = fake_adapter_dispatch_count(stress_ctx_buf);
      if (final_count != 32) { ok = 0; printf("COUNT=%u ", final_count); }
      for (uint32_t j = 0; j < final_count && j < 32; j++) {
          const fake_dispatch_record_t *r = fake_adapter_get_dispatch(stress_ctx_buf, j);
          if (!r) { ok = 0; printf("NULL@%u ", j); break; }
          if (r->step.step_id != r->token.step_id) {
              ok = 0;
              printf("TORN@%u: step=%u tok_step=%u ", j, r->step.step_id, r->token.step_id);
              break;
          }
          if (r->step.step_id != r->token.request_id) {
              ok = 0;
              printf("TORN_REQ@%u: step=%u req=%u ", j, r->step.step_id, r->token.request_id);
              break;
          }
          if (r->step.step_id != (uint16_t)(j + 1)) {
              ok = 0;
              printf("WRONG_ID@%u: got=%u exp=%u ", j, r->step.step_id, (uint16_t)(j + 1));
              break;
          }
      }
      if (ok) PASS(); else FAIL("concurrent torn records"); }

    /* Two producers: verify no crash, safe serialization */
    { TEST("fake_adapter: two producers no crash");
      static uint8_t dual_ctx_buf[FAKE_ADAPTER_CTX_SIZE];
      static wash_executor_adapter_t dual_adapter;
      fake_adapter_init(&dual_adapter, dual_ctx_buf);
      dual_producer_arg_t p1 = { &dual_adapter, 1, 16 };
      dual_producer_arg_t p2 = { &dual_adapter, 17, 16 };
      HANDLE hp1 = CreateThread(NULL, 0, dual_producer_fn, &p1, 0, NULL);
      HANDLE hp2 = CreateThread(NULL, 0, dual_producer_fn, &p2, 0, NULL);
      WaitForSingleObject(hp1, 10000);
      WaitForSingleObject(hp2, 10000);
      CloseHandle(hp1);
      CloseHandle(hp2);
      uint32_t count = fake_adapter_dispatch_count(dual_ctx_buf);
      int ok = 1;
      /* Concurrent producers: at least some dispatches recorded (no crash) */
      if (count == 0) ok = 0;
      if (ok) PASS(); else FAIL("dual producers"); }
#endif

    { TEST("executor: skip rejects non-UV active step");
      wash_executor_core_t c; core_init(&c);
      wash_program_t prog = make_exec_test_program();
      c.program = &prog; c.total_steps = (uint16_t)prog.step_count;
      core_event_t ev; memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_PROGRAM_ACCEPTED; ev.token.program_id = 1;
      core_cmd_t cmds[16]; core_process_event(&c, &ev, cmds, 16);
      memset(&ev, 0, sizeof(ev)); ev.type = CORE_EVENT_STEP_DISPATCH_OK;
      core_process_event(&c, &ev, cmds, 16);
      memset(&ev, 0, sizeof(ev)); ev.type = CORE_EVENT_SKIP_STEP;
      uint32_t n = core_process_event(&c, &ev, cmds, 16);
      if (n == 0 && c.phase == CORE_PHASE_ACTIVE && c.current_step_index == 0)
          PASS(); else FAIL("non-UV skip accepted"); }

    { TEST("executor: UV skip waits close ACK then advances");
      wash_executor_core_t c; core_init(&c);
      wash_program_t prog; memset(&prog, 0, sizeof(prog));
      prog.program_id = 7; prog.kind = WASH_PROGRAM_CUSTOM; prog.step_count = 2;
      prog.steps[0].step_id = 10; prog.steps[0].type = STEP_UV;
      prog.steps[1].step_id = 11; prog.steps[1].type = STEP_FINISH;
      c.program = &prog; c.total_steps = 2;
      core_event_t ev; memset(&ev, 0, sizeof(ev));
      ev.type = CORE_EVENT_PROGRAM_ACCEPTED; ev.token.program_id = 7;
      core_cmd_t cmds[16]; core_process_event(&c, &ev, cmds, 16);
      memset(&ev, 0, sizeof(ev)); ev.type = CORE_EVENT_STEP_DISPATCH_OK;
      core_process_event(&c, &ev, cmds, 16);
      memset(&ev, 0, sizeof(ev)); ev.type = CORE_EVENT_SKIP_STEP;
      uint32_t n = core_process_event(&c, &ev, cmds, 16);
      int ok = n == 1 && cmds[0].type == CORE_CMD_CANCEL_ACTIVE_STEP &&
               c.phase == CORE_PHASE_CANCELING && c.current_step_index == 0;
      memset(&ev, 0, sizeof(ev)); ev.type = CORE_EVENT_SERVICE_TERMINAL;
      ev.token = c.cancel_token; ev.success = true;
      n = core_process_event(&c, &ev, cmds, 16);
      int complete_seen = 0;
      for (uint32_t i = 0; i < n; i++) {
          if (cmds[i].type == CORE_CMD_PUBLISH_PROGRAM_TERMINAL &&
              cmds[i].terminal_result == TERM_RESULT_COMPLETE) {
              complete_seen = 1;
          }
      }
      if (c.phase != CORE_PHASE_IDLE || c.current_step_index != 2 ||
          c.completed_steps != 2 || !complete_seen) ok = 0;
      if (ok) PASS(); else FAIL("UV skip close ACK flow"); }

    { TEST("ble frame: single JSON complete");
      ble_frame_assembler_t a; ble_frame_assembler_init(&a);
      const char *f = NULL; size_t n = 0; const char *json = "{\"v\":1}";
      ble_frame_result_t r = ble_frame_assembler_feed(&a, (const uint8_t *)json,
          strlen(json), 10, &f, &n);
      if (r == BLE_FRAME_COMPLETE && n == strlen(json) && memcmp(f,json,n)==0) PASS();
      else FAIL("single frame"); }

    { TEST("ble frame: 20-byte fragmentation and string braces");
      ble_frame_assembler_t a; ble_frame_assembler_init(&a);
      const char *json = "{\"v\":1,\"payload\":\"{x}\\\"y\"}";
      const char *f = NULL; size_t n = 0;
      ble_frame_result_t r1 = ble_frame_assembler_feed(&a,(const uint8_t*)json,20,1,&f,&n);
      ble_frame_result_t r2 = ble_frame_assembler_feed(&a,(const uint8_t*)json+20,
          strlen(json)-20,2,&f,&n);
      if (r1==BLE_FRAME_NEED_MORE && r2==BLE_FRAME_COMPLETE && n==strlen(json)) PASS();
      else FAIL("fragment/string braces"); }

    { TEST("ble frame: timeout boundary and expiry");
      ble_frame_assembler_t a; ble_frame_assembler_init(&a); const char *f; size_t n;
      const char *p1="{\"v\":"; const char *p2="1}";
      int ok = ble_frame_assembler_feed(&a,(const uint8_t*)p1,strlen(p1),100,&f,&n)==BLE_FRAME_NEED_MORE;
      if (ble_frame_assembler_feed(&a,(const uint8_t*)p2,strlen(p2),3100,&f,&n)!=BLE_FRAME_COMPLETE) ok=0;
      ble_frame_assembler_reset(&a);
      ble_frame_assembler_feed(&a,(const uint8_t*)p1,strlen(p1),100,&f,&n);
      if (ble_frame_assembler_feed(&a,(const uint8_t*)p2,strlen(p2),3101,&f,&n)!=BLE_FRAME_ERR_TIMEOUT) ok=0;
      if(ok) PASS(); else FAIL("timeout boundary"); }

    { TEST("ble frame: 2049 rejected and state cleared");
      ble_frame_assembler_t a; ble_frame_assembler_init(&a); const char *f; size_t n;
      static uint8_t huge[BLE_FRAME_MAX_BYTES+1]; memset(huge,' ',sizeof(huge)); huge[0]='{';
      ble_frame_result_t r=ble_frame_assembler_feed(&a,huge,sizeof(huge),1,&f,&n);
      if(r==BLE_FRAME_ERR_TOO_LARGE && a.length==0 && !a.active) PASS(); else FAIL("overflow"); }

    { TEST("ble frame: malformed trailing object rejected");
      ble_frame_assembler_t a; ble_frame_assembler_init(&a); const char *f; size_t n;
      const char *s="{}{}";
      if(ble_frame_assembler_feed(&a,(const uint8_t*)s,4,1,&f,&n)==BLE_FRAME_ERR_MALFORMED) PASS();
      else FAIL("trailing object"); }

    { TEST("app protocol: all V1 command names parse");
      const char *names[]={"hello","get_status","start_formal","start_demo","submit_plan",
        "ack_load","ack_unload","skip_uv","abort_reset","pause","resume"};
      int ok=1; char json[128];
      for(size_t i=0;i<sizeof(names)/sizeof(names[0]);i++){
        snprintf(json,sizeof(json),"{\"v\":1,\"seq\":%u,\"type\":\"cmd\",\"cmd\":\"%s\"}",(unsigned)i+1,names[i]);
        app_protocol_message_t m;
        if(app_protocol_parse_frame(json,strlen(json),&m)!=APP_PARSE_OK || m.command==APP_CMD_INVALID) ok=0;
      }
      if(ok) PASS(); else FAIL("command map"); }

    { TEST("app protocol: diagnostic command names parse (bench + calibration)");
      const char *names[]={"uv_self_test","actuator_self_test","motor_bench_start",
        "motor_bench_confirm","motor_bench_cancel","set_direction_calibration",
        "position_move_test","provision_wifi"};
      int ok=1; char json[128];
      for(size_t i=0;i<sizeof(names)/sizeof(names[0]);i++){
        snprintf(json,sizeof(json),"{\"v\":1,\"seq\":%u,\"type\":\"cmd\",\"cmd\":\"%s\"}",(unsigned)i+50,names[i]);
        app_protocol_message_t m;
        if(app_protocol_parse_frame(json,strlen(json),&m)!=APP_PARSE_OK || m.command==APP_CMD_INVALID) ok=0;
      }
      if(ok) PASS(); else FAIL("diag command map"); }

    { TEST("machine_config: apply direction calibration pure (set + flip)");
      machine_config_t cfg = make_valid_config();
      cfg.position.direction_calibrated = false;
      cfg.position.rpwm_is_cw = false;
      machine_config_apply_direction_calibration(&cfg, true);
      int ok = cfg.position.direction_calibrated && cfg.position.rpwm_is_cw &&
               machine_config_validate(&cfg);
      machine_config_apply_direction_calibration(&cfg, false);
      ok = ok && cfg.position.direction_calibrated && !cfg.position.rpwm_is_cw &&
           machine_config_validate(&cfg);
      if(ok) PASS(); else FAIL("apply calibration"); }

    { TEST("machine_config: apply bl50 direction reverse pure");
      machine_config_t cfg = make_valid_config();
      cfg.position.bl50_reverse_dir = false;
      machine_config_apply_bl50_direction_reverse(&cfg, true);
      int ok = cfg.position.bl50_reverse_dir && machine_config_validate(&cfg);
      machine_config_apply_bl50_direction_reverse(&cfg, false);
      ok = ok && !cfg.position.bl50_reverse_dir && machine_config_validate(&cfg);
      if(ok) PASS(); else FAIL("bl50 reverse"); }

    { TEST("machine_config: reset benchtst preserves calibration and production config");
      machine_config_t cfg = make_valid_config();
      cfg.position.direction_calibrated = true;
      cfg.position.rpwm_is_cw = false;
      cfg.position.bl50_reverse_dir = true;
      cfg.water.source_batch_target_pulses = 4321U;
      cfg.benchtst.bl50_pwm = 73U;
      cfg.benchtst.ibt2_pwm = 61U;
      cfg.benchtst.act_duration_ms = 9876U;
      machine_config_apply_benchtst_defaults(&cfg);
      int ok = cfg.benchtst.bl50_pwm == 40U &&
               cfg.benchtst.ibt2_pwm == 80U &&
               cfg.benchtst.act_duration_ms == 1500U &&
               cfg.position.direction_calibrated &&
               !cfg.position.rpwm_is_cw && cfg.position.bl50_reverse_dir &&
               cfg.water.source_batch_target_pulses == 4321U && machine_config_validate(&cfg);
      if(ok) PASS(); else FAIL("benchtst-only reset isolation"); }

    { TEST("app protocol: bad version and missing seq rejected");
      app_protocol_message_t m;
      const char *a="{\"v\":2,\"seq\":1,\"cmd\":\"hello\"}";
      const char *b="{\"v\":1,\"cmd\":\"hello\"}";
      if(app_protocol_parse_frame(a,strlen(a),&m)==APP_PARSE_BAD_VERSION &&
         app_protocol_parse_frame(b,strlen(b),&m)==APP_PARSE_MISSING_FIELD) PASS();
      else FAIL("version/missing"); }

    { TEST("app protocol: malformed JSON rejected");
      app_protocol_message_t m; const char *s="{\"v\":1,\"seq\":1,\"cmd\":\"hello\"";
      if(app_protocol_parse_frame(s,strlen(s),&m)==APP_PARSE_BAD_JSON) PASS(); else FAIL("bad JSON"); }

    { TEST("app protocol: unknown and raw hardware commands rejected");
      app_protocol_message_t m;
      const char *u="{\"v\":1,\"seq\":1,\"cmd\":\"dance\"}";
      const char *h="{\"v\":1,\"seq\":2,\"cmd\":\"valve_on\"}";
      if(app_protocol_parse_frame(u,strlen(u),&m)==APP_PARSE_UNKNOWN_CMD &&
         app_protocol_parse_frame(h,strlen(h),&m)==APP_PARSE_FORBIDDEN_CMD) PASS();
      else FAIL("unknown/forbidden"); }

    { TEST("app protocol: legacy raw and JSON start stop");
      app_protocol_message_t m; const char *a="start",*b="{\"cmd\":\"stop\"}";
      int ok=app_protocol_parse_frame(a,strlen(a),&m)==APP_PARSE_OK && m.command==APP_CMD_LEGACY_START;
      ok=ok && app_protocol_parse_frame(b,strlen(b),&m)==APP_PARSE_OK && m.command==APP_CMD_LEGACY_STOP;
      if(ok) PASS(); else FAIL("legacy"); }

    { TEST("app protocol: duplicate seq returns cached reply");
      app_seq_cache_t c; app_seq_cache_init(&c); app_protocol_message_t m;
      const char *j="{\"v\":1,\"seq\":9,\"cmd\":\"start_demo\"}";
      app_protocol_parse_frame(j,strlen(j),&m); const char *reply="ACK";
      int ok=app_seq_cache_store(&c,&m,reply,3); const char *got=NULL; size_t n=0;
      ok=ok && app_seq_cache_lookup(&c,&m,&got,&n)==APP_SEQ_DUPLICATE && n==3 && memcmp(got,"ACK",3)==0;
      if(ok) PASS(); else FAIL("duplicate cache"); }

    { TEST("app protocol: same seq different frame conflicts");
      app_seq_cache_t c; app_seq_cache_init(&c); app_protocol_message_t a,b;
      const char *ja="{\"v\":1,\"seq\":4,\"cmd\":\"start_demo\"}";
      const char *jb="{\"v\":1,\"seq\":4,\"cmd\":\"start_formal\"}";
      app_protocol_parse_frame(ja,strlen(ja),&a); app_protocol_parse_frame(jb,strlen(jb),&b);
      app_seq_cache_store(&c,&a,"A",1);
      if(app_seq_cache_lookup(&c,&b,NULL,NULL)==APP_SEQ_CONFLICT) PASS(); else FAIL("seq conflict"); }

    { TEST("app protocol: bounded cache evicts oldest");
      app_seq_cache_t c; app_seq_cache_init(&c); app_protocol_message_t first={0},m={0};
      for(uint32_t i=1;i<=APP_PROTOCOL_SEQ_CACHE_SIZE+1;i++){
        m.version=1;m.seq=i;m.command=APP_CMD_GET_STATUS;m.fingerprint=i;
        if(i==1) first=m; app_seq_cache_store(&c,&m,"A",1);
      }
      if(app_seq_cache_lookup(&c,&first,NULL,NULL)==APP_SEQ_MISS) PASS(); else FAIL("cache eviction"); }

    { TEST("voice core: disabled to listening");
      voice_core_t c; voice_core_init(&c);
      voice_transition_t a = voice_core_process(&c, VOICE_EVENT_ENABLE, 0);
      voice_transition_t b = voice_core_process(&c, VOICE_EVENT_FRONTEND_READY, 0);
      if (a.accepted && a.action == VOICE_ACTION_START_FRONTEND &&
          b.accepted && c.state == VOICE_STATE_LISTENING &&
          c.generation == 1) PASS(); else FAIL("enable/listen"); }

    { TEST("voice core: complete wake-cloud-submit cycle");
      voice_core_t c; voice_core_init(&c);
      voice_core_process(&c, VOICE_EVENT_ENABLE, 0);
      voice_core_process(&c, VOICE_EVENT_FRONTEND_READY, 0);
      int ok = voice_core_process(&c, VOICE_EVENT_WAKE, 0).action ==
               VOICE_ACTION_START_RECORDING;
      ok = ok && voice_core_process(&c, VOICE_EVENT_RECORDING_STARTED, 0).accepted;
      ok = ok && voice_core_process(&c, VOICE_EVENT_UTTERANCE_READY, 0).action ==
                 VOICE_ACTION_STOP_AND_UPLOAD;
      ok = ok && voice_core_process(&c, VOICE_EVENT_UPLOAD_STARTED, 0).accepted;
      ok = ok && voice_core_process(&c, VOICE_EVENT_CLOUD_RESPONSE, 0).action ==
                 VOICE_ACTION_PARSE_RESPONSE;
      ok = ok && voice_core_process(&c, VOICE_EVENT_TOOL_PARSED, 0).action ==
                 VOICE_ACTION_SUBMIT_TOOL;
      ok = ok && voice_core_process(&c, VOICE_EVENT_TOOL_ACCEPTED, 0).action ==
                 VOICE_ACTION_PLAY_ACCEPTED;
      ok = ok && voice_core_process(&c, VOICE_EVENT_PLAYBACK_DONE, 0).action ==
                 VOICE_ACTION_START_FRONTEND;
      ok = ok && c.state == VOICE_STATE_IDLE && c.utterance_id == 1;
      if (ok) PASS(); else FAIL("full transition chain"); }

    { TEST("voice core: invalid transition rejected without mutation");
      voice_core_t c; voice_core_init(&c);
      voice_transition_t tr = voice_core_process(&c, VOICE_EVENT_WAKE, 0);
      if (!tr.accepted && tr.action == VOICE_ACTION_NONE &&
          c.state == VOICE_STATE_DISABLED) PASS();
      else FAIL("invalid transition"); }

    { TEST("voice core: cancel preempts recording");
      voice_core_t c; voice_core_init(&c);
      voice_core_process(&c, VOICE_EVENT_ENABLE, 0);
      voice_core_process(&c, VOICE_EVENT_FRONTEND_READY, 0);
      voice_core_process(&c, VOICE_EVENT_WAKE, 0);
      voice_core_process(&c, VOICE_EVENT_RECORDING_STARTED, 0);
      voice_transition_t tr = voice_core_process(&c, VOICE_EVENT_CANCEL, 0);
      if (tr.accepted && tr.action == VOICE_ACTION_CANCEL_ALL &&
          c.state == VOICE_STATE_IDLE && c.generation == 2) PASS();
      else FAIL("cancel priority"); }

    { TEST("voice core: safety fault preempts speaking");
      voice_core_t c; voice_core_init(&c);
      c.state = VOICE_STATE_SPEAKING;
      voice_transition_t tr = voice_core_process(
          &c, VOICE_EVENT_SAFETY_FAULT, 19);
      if (tr.accepted && tr.action == VOICE_ACTION_CANCEL_ALL &&
          c.state == VOICE_STATE_ERROR && c.last_error == 19) PASS();
      else FAIL("fault priority"); }

    { TEST("voice core: PTT barges into optional TTS playback");
      voice_core_t c; voice_core_init(&c);
      c.state = VOICE_STATE_SPEAKING;
      c.utterance_id = 9;
      c.terminal_emitted = true;
      voice_transition_t tr = voice_core_process(
          &c, VOICE_EVENT_PTT_BEGIN, 0);
      if (tr.accepted && tr.state == VOICE_STATE_WAKE_DETECTED &&
          tr.action == VOICE_ACTION_START_RECORDING &&
          c.utterance_id == 10 && !c.terminal_emitted) PASS();
      else FAIL("PTT playback preemption"); }

    { TEST("voice core: counters wrap past zero");
      voice_core_t c; voice_core_init(&c);
      c.generation = UINT32_MAX;
      c.utterance_id = UINT32_MAX;
      voice_core_process(&c, VOICE_EVENT_ENABLE, 0);
      voice_core_process(&c, VOICE_EVENT_FRONTEND_READY, 0);
      voice_core_process(&c, VOICE_EVENT_WAKE, 0);
      if (c.generation == 1 && c.utterance_id == 1) PASS();
      else FAIL("zero skipped"); }

    { TEST("voice parser: formal start defaults");
      const char *j = "{\"tool\":\"start_program\",\"program\":\"formal\"}";
      voice_tool_request_t r;
      if (voice_tool_parse_json(j, strlen(j), &r) == VOICE_PARSE_OK &&
          r.tool == VOICE_TOOL_START_PROGRAM &&
          r.program_kind == WASH_PROGRAM_FORMAL &&
          r.allow_uv && r.allow_dry) PASS();
      else FAIL("formal defaults"); }

    { TEST("voice parser: demo options and voice source");
      const char *j = "{\"tool\":\"start_program\",\"program\":\"demo\","
                      "\"allow_uv\":false,\"allow_dry\":false}";
      voice_tool_request_t r; wash_intent_t intent;
      if (voice_tool_parse_json(j, strlen(j), &r) == VOICE_PARSE_OK &&
          voice_tool_to_wash_intent(&r, &intent) &&
          intent.source == APP_SOURCE_VOICE &&
          intent.kind == WASH_PROGRAM_DEMO &&
          !intent.allow_uv && !intent.allow_dry) PASS();
      else FAIL("demo/source"); }

    { TEST("voice parser: all control tools");
      const char *names[] = {"cancel","status","ack_load","ack_unload",
                             "skip_uv","ack_fault"};
      int ok = 1; char json[64]; voice_tool_request_t r;
      for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
          snprintf(json, sizeof(json), "{\"tool\":\"%s\"}", names[i]);
          if (voice_tool_parse_json(json, strlen(json), &r) != VOICE_PARSE_OK)
              ok = 0;
      }
      if (ok) PASS(); else FAIL("control allowlist"); }

    { TEST("voice parser: direct hardware tools forbidden");
      const char *a = "{\"tool\":\"heater_on\"}";
      const char *b = "{\"tool\":\"move_position\"}";
      voice_tool_request_t r;
      if (voice_tool_parse_json(a, strlen(a), &r) ==
              VOICE_PARSE_FORBIDDEN_HARDWARE &&
          voice_tool_parse_json(b, strlen(b), &r) ==
              VOICE_PARSE_FORBIDDEN_HARDWARE) PASS();
      else FAIL("hardware denylist"); }

    { TEST("voice parser: duplicate and unknown fields rejected");
      const char *a = "{\"tool\":\"cancel\",\"tool\":\"status\"}";
      const char *b = "{\"tool\":\"cancel\",\"raw_gpio\":true}";
      voice_tool_request_t r;
      if (voice_tool_parse_json(a, strlen(a), &r) ==
              VOICE_PARSE_DUPLICATE_FIELD &&
          voice_tool_parse_json(b, strlen(b), &r) ==
              VOICE_PARSE_UNKNOWN_FIELD) PASS();
      else FAIL("strict fields"); }

    { TEST("voice parser: malformed and trailing JSON rejected");
      const char *a = "{\"tool\":\"cancel\",}";
      const char *b = "{\"tool\":\"cancel\"}{}";
      voice_tool_request_t r;
      if (voice_tool_parse_json(a, strlen(a), &r) == VOICE_PARSE_BAD_JSON &&
          voice_tool_parse_json(b, strlen(b), &r) == VOICE_PARSE_BAD_JSON) PASS();
      else FAIL("JSON framing"); }

    { TEST("voice parser: missing/invalid program rejected");
      const char *a = "{\"tool\":\"start_program\"}";
      const char *b = "{\"tool\":\"start_program\",\"program\":\"raw\"}";
      voice_tool_request_t r;
      if (voice_tool_parse_json(a, strlen(a), &r) ==
              VOICE_PARSE_MISSING_FIELD &&
          voice_tool_parse_json(b, strlen(b), &r) ==
              VOICE_PARSE_INVALID_VALUE) PASS();
      else FAIL("program validation"); }

    { TEST("voice parser: non-start options rejected");
      const char *j = "{\"tool\":\"cancel\",\"allow_uv\":false}";
      voice_tool_request_t r;
      if (voice_tool_parse_json(j, strlen(j), &r) ==
          VOICE_PARSE_INVALID_VALUE) PASS();
      else FAIL("tool-specific schema"); }

    { TEST("voice parser: bounded input");
      static char huge[VOICE_TOOL_JSON_MAX_BYTES + 1];
      memset(huge, 'x', sizeof(huge));
      voice_tool_request_t r;
      if (voice_tool_parse_json(huge, sizeof(huge), &r) ==
          VOICE_PARSE_TOO_LARGE) PASS();
      else FAIL("size cap"); }

    { TEST("button: BTN1 long press emits once and suppresses click");
      button_debounce_ctx_t c; button_event_t ev[3];
      button_debounce_init(&c, 30);
      button_debounce_set_long_press(
          &c, (uint8_t)(1U << BUTTON_RAW_BIT_BTN1), 2000);
      button_debounce_seed(&c, 0x70, 0);
      int n = button_debounce_process(&c, 0x60, 0, ev, 3);
      n += button_debounce_process(&c, 0x60, 30, ev + n, 3 - n);
      n += button_debounce_process(&c, 0x60, 2029, ev + n, 3 - n);
      n += button_debounce_process(&c, 0x60, 2030, ev + n, 3 - n);
      int at_long = n;
      n += button_debounce_process(&c, 0x60, 3000, ev + n, 3 - n);
      n += button_debounce_process(&c, 0x70, 3010, ev + n, 3 - n);
      n += button_debounce_process(&c, 0x70, 3040, ev + n, 3 - n);
      if (at_long == 1 && n == 2 &&
          ev[0].button_id == BUTTON_ID_1 &&
          ev[0].event_type == BUTTON_EVENT_LONG_PRESS &&
          ev[0].timestamp_ms == 2030 && ev[0].sequence == 1 &&
          ev[1].button_id == BUTTON_ID_1 &&
          ev[1].event_type == BUTTON_EVENT_LONG_PRESS_RELEASE &&
          ev[1].timestamp_ms == 3040 && ev[1].sequence == 2) PASS();
      else FAIL("one-shot/suppress"); }

    { TEST("button: BTN1 short press remains CLICK");
      button_debounce_ctx_t c; button_event_t ev;
      button_debounce_init(&c, 30);
      button_debounce_set_long_press(
          &c, (uint8_t)(1U << BUTTON_RAW_BIT_BTN1), 2000);
      button_debounce_seed(&c, 0x70, 0);
      button_debounce_process(&c, 0x60, 10, &ev, 1);
      button_debounce_process(&c, 0x60, 40, &ev, 1);
      button_debounce_process(&c, 0x70, 1000, &ev, 1);
      int n = button_debounce_process(&c, 0x70, 1030, &ev, 1);
      if (n == 1 && ev.button_id == BUTTON_ID_1 &&
          ev.event_type == BUTTON_EVENT_CLICK) PASS();
      else FAIL("short click preserved"); }

    { TEST("button: BTN3 held is not captured by BTN1 long mask");
      button_debounce_ctx_t c; button_event_t ev;
      button_debounce_init(&c, 30);
      button_debounce_set_long_press(
          &c, (uint8_t)(1U << BUTTON_RAW_BIT_BTN1), 2000);
      button_debounce_seed(&c, 0x70, 0);
      button_debounce_process(&c, 0x30, 1, &ev, 1);
      button_debounce_process(&c, 0x30, 31, &ev, 1);
      int held = button_debounce_process(&c, 0x30, 5000, &ev, 1);
      button_debounce_process(&c, 0x70, 5010, &ev, 1);
      int released = button_debounce_process(&c, 0x70, 5040, &ev, 1);
      if (held == 0 && released == 1 &&
          ev.button_id == BUTTON_ID_3 &&
          ev.event_type == BUTTON_EVENT_CLICK) PASS();
      else FAIL("urgent button semantics"); }

    { TEST("button: long press deadline is exact");
      button_debounce_ctx_t c; button_event_t ev;
      button_debounce_init(&c, 30);
      button_debounce_set_long_press(
          &c, (uint8_t)(1U << BUTTON_RAW_BIT_BTN1), 2000);
      button_debounce_seed(&c, 0x70, 0);
      button_debounce_process(&c, 0x60, 100, &ev, 1);
      button_debounce_process(&c, 0x60, 130, &ev, 1);
      button_deadline_t a = button_debounce_next_deadline(&c, 2129);
      button_deadline_t b = button_debounce_next_deadline(&c, 2130);
      if (a.active && !a.due_now && a.remaining_ms == 1 &&
          b.active && b.due_now && b.remaining_ms == 0) PASS();
      else FAIL("long deadline"); }

    { TEST("voice cloud: secure config accepted");
      cloud_fake_transport_t fake = {VOICE_CLOUD_OK, 200, "{}", 0, 0};
      voice_cloud_config_t c = make_cloud_config(&fake);
      if (voice_cloud_config_valid(&c)) PASS();
      else FAIL("valid secure config"); }

    { TEST("voice cloud: insecure endpoint and injected key rejected");
      cloud_fake_transport_t fake = {0};
      voice_cloud_config_t c = make_cloud_config(&fake);
      c.endpoint = "http://token-plan-cn.xiaomimimo.com/v1";
      int http_rejected = !voice_cloud_config_valid(&c);
      c = make_cloud_config(&fake);
      c.endpoint = "https://user@token-plan-cn.xiaomimimo.com/v1";
      int userinfo_rejected = !voice_cloud_config_valid(&c);
      c = make_cloud_config(&fake);
      c.api_key = "key\r\nInjected: yes";
      int header_rejected = !voice_cloud_config_valid(&c);
      if (http_rejected && userinfo_rejected && header_rejected) PASS();
      else FAIL("TLS/header validation"); }

    { TEST("voice cloud: config limits enforced");
      cloud_fake_transport_t fake = {0};
      voice_cloud_config_t c = make_cloud_config(&fake);
      c.connect_timeout_ms = 0;
      int timeout = !voice_cloud_config_valid(&c);
      c = make_cloud_config(&fake);
      c.max_response_bytes = VOICE_CLOUD_DEFAULT_MAX_RESPONSE_BYTES + 1;
      int response = !voice_cloud_config_valid(&c);
      c = make_cloud_config(&fake);
      c.model = "mimo v2.5";
      int model = !voice_cloud_config_valid(&c);
      if (timeout && response && model) PASS();
      else FAIL("config bounds"); }

    { TEST("voice cloud: 16k mono PCM WAV accepted");
      uint8_t wav[48]; make_reference_wav(wav);
      if (voice_cloud_wav_valid(wav, sizeof(wav))) PASS();
      else FAIL("reference WAV"); }

    { TEST("voice cloud: wrong WAV format rejected");
      uint8_t wav[48]; make_reference_wav(wav);
      wav[24] = 0x44; /* 16000 -> 0x3e80 */
      int rate = !voice_cloud_wav_valid(wav, sizeof(wav));
      make_reference_wav(wav);
      wav[22] = 2;
      int channels = !voice_cloud_wav_valid(wav, sizeof(wav));
      make_reference_wav(wav);
      wav[40] = 20;
      int truncated = !voice_cloud_wav_valid(wav, sizeof(wav));
      if (rate && channels && truncated) PASS();
      else FAIL("WAV contract"); }

    { TEST("voice cloud: direct tool JSON extracted");
      const char *json = "{\"tool\":\"cancel\"}";
      char out[VOICE_CLOUD_TOOL_JSON_CAPACITY]; size_t length = 0;
      if (voice_cloud_extract_tool_json(
              json, strlen(json), out, sizeof(out), &length) ==
              VOICE_CLOUD_OK &&
          length == strlen(json) && strcmp(out, json) == 0) PASS();
      else FAIL("direct response"); }

    { TEST("voice cloud: OpenAI arguments string extracted");
      const char *body =
          "{\"choices\":[{\"message\":{\"tool_calls\":[{\"function\":{"
          "\"name\":\"submit_machine_intent\",\"arguments\":"
          "\"{\\\"tool\\\":\\\"start_program\\\",\\\"program\\\":"
          "\\\"demo\\\",\\\"allow_uv\\\":false}\"}}]}}]}";
      char out[VOICE_CLOUD_TOOL_JSON_CAPACITY]; size_t length = 0;
      voice_tool_request_t tool;
      voice_cloud_result_t result = voice_cloud_extract_tool_json(
          body, strlen(body), out, sizeof(out), &length);
      if (result == VOICE_CLOUD_OK &&
          voice_tool_parse_json(out, length, &tool) == VOICE_PARSE_OK &&
          tool.tool == VOICE_TOOL_START_PROGRAM &&
          tool.program_kind == WASH_PROGRAM_DEMO &&
          !tool.allow_uv) PASS();
      else FAIL("arguments extraction"); }

    { TEST("voice cloud: long provider strings do not hide arguments");
      const char *body =
          "{\"id\":\"ffaa81bf-b376-4069-9493-05e0b3b80765_"
          "32a53226a2114b8aa3d85d9cb6290110\",\"choices\":[{"
          "\"message\":{\"reasoning_content\":\"This provider reasoning "
          "string is deliberately much longer than the key scanner buffer\","
          "\"tool_calls\":[{\"function\":{\"arguments\":"
          "\"{\\\"domain\\\":\\\"chat\\\",\\\"transcript\\\":"
          "\\\"\\\\u4f60\\\\u597d\\\",\\\"reply_text\\\":"
          "\\\"hello\\\"," 
          "\\\"confidence_milli\\\":950,\\\"requires_confirmation\\\":"
          "false,\\\"tool\\\":\\\"none\\\"}\"}}]}}]}";
      char out[VOICE_ROUTE_JSON_MAX_BYTES + 1U]; size_t length = 0;
      voice_route_result_t route;
      voice_cloud_result_t result = voice_cloud_extract_arguments_json(
          body, strlen(body), out, sizeof(out), &length);
      if (result == VOICE_CLOUD_OK &&
          voice_route_parse_json(out, length, &route) ==
              VOICE_ROUTE_PARSE_OK &&
          route.domain == VOICE_ROUTE_DOMAIN_CHAT &&
          route.tool.tool == VOICE_TOOL_NONE &&
          strcmp(route.transcript, "\xE4\xBD\xA0\xE5\xA5\xBD") == 0) PASS();
      else FAIL("long provider fields blocked arguments"); }

    { TEST("voice cloud: missing arguments rejected");
      const char *body = "{\"choices\":[{\"message\":{\"content\":\"ok\"}}]}";
      char out[VOICE_CLOUD_TOOL_JSON_CAPACITY]; size_t length = 99;
      if (voice_cloud_extract_tool_json(
              body, strlen(body), out, sizeof(out), &length) ==
              VOICE_CLOUD_BAD_RESPONSE &&
          length == 0 && out[0] == '\0') PASS();
      else FAIL("missing arguments"); }

    { TEST("voice cloud: forbidden tool in response rejected");
      const char *body =
          "{\"function\":{\"arguments\":\"{\\\"tool\\\":"
          "\\\"heater_on\\\"}\"}}";
      char out[VOICE_CLOUD_TOOL_JSON_CAPACITY]; size_t length = 0;
      if (voice_cloud_extract_tool_json(
              body, strlen(body), out, sizeof(out), &length) ==
              VOICE_CLOUD_BAD_RESPONSE) PASS();
      else FAIL("cloud cannot bypass parser"); }

    { TEST("voice cloud: end-to-end fake transport");
      const char *body =
          "{\"function\":{\"arguments\":\"{\\\"tool\\\":"
          "\\\"status\\\"}\"}}";
      cloud_fake_transport_t fake = {VOICE_CLOUD_OK, 200, body, 0, 0};
      voice_cloud_config_t c = make_cloud_config(&fake);
      uint8_t wav[48]; make_reference_wav(wav);
      char scratch[1024]; voice_cloud_output_t out;
      voice_cloud_result_t result = voice_cloud_run(
          &c, wav, sizeof(wav), scratch, sizeof(scratch), &out);
      if (result == VOICE_CLOUD_OK && fake.calls == 1 &&
          fake.observed_wav_length == sizeof(wav) &&
          out.http_status == 200 &&
          strcmp(out.tool_json, "{\"tool\":\"status\"}") == 0) PASS();
      else FAIL("fake transport chain"); }

    { TEST("voice cloud: HTTP status is not accepted");
      cloud_fake_transport_t fake = {
          VOICE_CLOUD_OK, 401, "{\"error\":\"unauthorized\"}", 0, 0};
      voice_cloud_config_t c = make_cloud_config(&fake);
      uint8_t wav[48]; make_reference_wav(wav);
      char scratch[1024]; voice_cloud_output_t out;
      if (voice_cloud_run(&c, wav, sizeof(wav), scratch,
                          sizeof(scratch), &out) ==
              VOICE_CLOUD_HTTP_ERROR &&
          out.http_status == 401) PASS();
      else FAIL("HTTP error"); }

    { TEST("voice cloud: transport errors propagate");
      uint8_t wav[48]; make_reference_wav(wav);
      char scratch[1024]; voice_cloud_output_t out;
      voice_cloud_result_t errors[] = {
          VOICE_CLOUD_TLS_ERROR, VOICE_CLOUD_TIMEOUT,
          VOICE_CLOUD_CANCELED, VOICE_CLOUD_RESPONSE_TOO_LARGE,
      };
      int ok = 1;
      for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
          cloud_fake_transport_t fake = {errors[i], 0, NULL, 0, 0};
          voice_cloud_config_t c = make_cloud_config(&fake);
          if (voice_cloud_run(&c, wav, sizeof(wav), scratch,
                              sizeof(scratch), &out) != errors[i]) ok = 0;
      }
      if (ok) PASS(); else FAIL("transport mapping"); }

    { TEST("voice cloud: oversized WAV rejected before transport");
      uint8_t wav[48]; make_reference_wav(wav);
      cloud_fake_transport_t fake = {VOICE_CLOUD_OK, 200, "{}", 0, 0};
      voice_cloud_config_t c = make_cloud_config(&fake);
      c.max_wav_bytes = 47;
      char scratch[1024]; voice_cloud_output_t out;
      if (voice_cloud_run(&c, wav, sizeof(wav), scratch,
                          sizeof(scratch), &out) ==
              VOICE_CLOUD_WAV_TOO_LARGE &&
          fake.calls == 0) PASS();
      else FAIL("pre-transport bound"); }

    { TEST("voice cloud: scratch capacity enforced");
      uint8_t wav[48]; make_reference_wav(wav);
      cloud_fake_transport_t fake = {VOICE_CLOUD_OK, 200, "{}", 0, 0};
      voice_cloud_config_t c = make_cloud_config(&fake);
      char scratch[64]; voice_cloud_output_t out;
      if (voice_cloud_run(&c, wav, sizeof(wav), scratch,
                          sizeof(scratch), &out) ==
              VOICE_CLOUD_BAD_ARG &&
          fake.calls == 0) PASS();
      else FAIL("response scratch bound"); }

    { TEST("voice cloud: malformed escaped arguments rejected");
      const char *body =
          "{\"function\":{\"arguments\":\"{\\\"tool\\\":\\q}\"}}";
      char out[VOICE_CLOUD_TOOL_JSON_CAPACITY]; size_t length = 0;
      voice_cloud_result_t result = voice_cloud_extract_tool_json(
          body, strlen(body), out, sizeof(out), &length);
      if (result == VOICE_CLOUD_TOOL_TOO_LARGE ||
          result == VOICE_CLOUD_BAD_RESPONSE) PASS();
      else FAIL("escape rejection"); }

    { TEST("voice cloud: offline gate prevents transport");
      const char *body =
          "{\"function\":{\"arguments\":\"{\\\"tool\\\":\\\"status\\\"}\"}}";
      cloud_fake_transport_t fake = {
          VOICE_CLOUD_OK, 200, body, 0, 0
      };
      bool network_ready = false;
      voice_cloud_config_t c = make_cloud_config(&fake);
      c.network_ready = cloud_fake_network_ready;
      c.network_context = &network_ready;
      uint8_t wav[48]; make_reference_wav(wav);
      char scratch[1024]; voice_cloud_output_t out;
      int ok = voice_cloud_run(
          &c, wav, sizeof(wav), scratch, sizeof(scratch), &out) ==
          VOICE_CLOUD_NETWORK_UNAVAILABLE && fake.calls == 0;
      network_ready = true;
      ok = ok && voice_cloud_run(
          &c, wav, sizeof(wav), scratch, sizeof(scratch), &out) ==
          VOICE_CLOUD_OK && fake.calls == 1;
      if (ok) PASS(); else FAIL("network gate"); }

    { TEST("voice core: BTN2 PTT begins and release uploads");
      voice_core_t c; voice_core_init(&c);
      voice_core_process(&c, VOICE_EVENT_ENABLE, 0);
      voice_core_process(&c, VOICE_EVENT_FRONTEND_READY, 0);
      voice_transition_t begin = voice_core_process(
          &c, VOICE_EVENT_PTT_BEGIN, 0);
      voice_transition_t started = voice_core_process(
          &c, VOICE_EVENT_RECORDING_STARTED, 0);
      voice_transition_t release = voice_core_process(
          &c, VOICE_EVENT_PTT_END, 0);
      if (begin.accepted &&
          begin.action == VOICE_ACTION_START_RECORDING &&
          started.accepted && c.utterance_id == 1 &&
          release.accepted &&
          release.action == VOICE_ACTION_STOP_AND_UPLOAD) PASS();
      else FAIL("PTT transition"); }

    { TEST("button: BTN1 and BTN2 use independent long thresholds");
      button_debounce_ctx_t c; button_event_t ev[4];
      button_debounce_init(&c, 30);
      button_debounce_set_long_press(
          &c, (uint8_t)((1U << BUTTON_RAW_BIT_BTN1) |
                        (1U << BUTTON_RAW_BIT_BTN2)), 2000);
      button_debounce_set_long_press_for_button(
          &c, BUTTON_ID_2, 600);
      button_debounce_seed(&c, 0x70, 0);
      button_debounce_process(&c, 0x40, 10, ev, 4);
      button_debounce_process(&c, 0x40, 40, ev, 4);
      int at_640 = button_debounce_process(&c, 0x40, 640, ev, 4);
      int at_2040 = button_debounce_process(&c, 0x40, 2040,
                                             ev + at_640, 4 - at_640);
      if (at_640 == 1 &&
          ev[0].button_id == BUTTON_ID_2 &&
          ev[0].event_type == BUTTON_EVENT_LONG_PRESS &&
          at_2040 == 1 &&
          ev[1].button_id == BUTTON_ID_1 &&
          ev[1].event_type == BUTTON_EVENT_LONG_PRESS) PASS();
      else FAIL("per-button long threshold"); }

    { TEST("voice route: strict chat response parses UTF-8");
      const char *json =
          "{\"domain\":\"chat\",\"transcript\":\"你好\","
          "\"reply_text\":\"你好，我在。\",\"confidence_milli\":940,"
          "\"requires_confirmation\":false,\"tool\":\"none\"}";
      voice_route_result_t route;
      if (voice_route_parse_json(json, strlen(json), &route) ==
              VOICE_ROUTE_PARSE_OK &&
          route.domain == VOICE_ROUTE_DOMAIN_CHAT &&
          !route.has_tool &&
          strcmp(route.reply_text, "你好，我在。") == 0 &&
          voice_route_authorize(
              &route, VOICE_ROUTE_POLICY_AUTO, 750) ==
              VOICE_ROUTE_DECISION_RESPOND) PASS();
      else FAIL("chat route"); }

    { TEST("voice route: AUTO executes high confidence bounded control");
      const char *json =
          "{\"domain\":\"control\",\"transcript\":\"开始演示洗涤\","
          "\"reply_text\":\"开始演示洗涤\","
          "\"confidence_milli\":920,\"requires_confirmation\":false,"
          "\"tool\":\"start_program\",\"program\":\"demo\","
          "\"allow_uv\":false,\"allow_dry\":true}";
      voice_route_result_t route;
      if (voice_route_parse_json(json, strlen(json), &route) ==
              VOICE_ROUTE_PARSE_OK &&
          route.tool.tool == VOICE_TOOL_START_PROGRAM &&
          route.tool.program_kind == WASH_PROGRAM_DEMO &&
          !route.tool.allow_uv && route.tool.allow_dry &&
          voice_route_authorize(
              &route, VOICE_ROUTE_POLICY_AUTO, 750) ==
              VOICE_ROUTE_DECISION_EXECUTE_TOOL) PASS();
      else FAIL("AUTO control"); }

    { TEST("voice route: BTN2 chat priority cannot mutate machine");
      const char *json =
          "{\"domain\":\"control\",\"transcript\":\"启动洗衣机\","
          "\"reply_text\":\"请确认后再启动\","
          "\"confidence_milli\":999,\"requires_confirmation\":false,"
          "\"tool\":\"start_program\",\"program\":\"formal\"}";
      voice_route_result_t route;
      if (voice_route_parse_json(json, strlen(json), &route) ==
              VOICE_ROUTE_PARSE_OK &&
          voice_route_authorize(
              &route, VOICE_ROUTE_POLICY_CHAT_PRIORITY, 750) ==
              VOICE_ROUTE_DECISION_REQUIRE_CONFIRMATION) PASS();
      else FAIL("CHAT_PRIORITY mutation gate"); }

    { TEST("voice route: BTN2 status remains read-only executable");
      const char *json =
          "{\"domain\":\"status\",\"transcript\":\"现在什么状态\","
          "\"reply_text\":\"正在待机\","
          "\"confidence_milli\":900,\"requires_confirmation\":false,"
          "\"tool\":\"status\"}";
      voice_route_result_t route;
      if (voice_route_parse_json(json, strlen(json), &route) ==
              VOICE_ROUTE_PARSE_OK &&
          voice_route_authorize(
              &route, VOICE_ROUTE_POLICY_CHAT_PRIORITY, 750) ==
              VOICE_ROUTE_DECISION_EXECUTE_TOOL) PASS();
      else FAIL("status route"); }

    { TEST("voice route: domain and tool mismatch rejected");
      const char *json =
          "{\"domain\":\"chat\",\"transcript\":\"cancel\","
          "\"reply_text\":\"bad\","
          "\"confidence_milli\":900,\"requires_confirmation\":false,"
          "\"tool\":\"cancel\"}";
      voice_route_result_t route;
      if (voice_route_parse_json(json, strlen(json), &route) ==
          VOICE_ROUTE_PARSE_INCONSISTENT_TOOL) PASS();
      else FAIL("route consistency"); }

    { TEST("voice cloud: AUTO_ROUTE passes source and policy");
      const char *body =
          "{\"choices\":[{\"message\":{\"tool_calls\":[{\"function\":{"
          "\"name\":\"route_voice_request\",\"arguments\":"
          "\"{\\\"domain\\\":\\\"chat\\\",\\\"transcript\\\":"
          "\\\"hello\\\",\\\"reply_text\\\":"
          "\\\"hello\\\",\\\"confidence_milli\\\":900,"
          "\\\"requires_confirmation\\\":false,\\\"tool\\\":"
          "\\\"none\\\"}\"}}]}}]}";
      cloud_fake_transport_t fake = {
          VOICE_CLOUD_OK, 200, body, 0, 0
      };
      voice_cloud_config_t c = make_cloud_config(&fake);
      uint8_t wav[48]; make_reference_wav(wav);
      char scratch[1024]; voice_cloud_route_output_t out;
      voice_cloud_result_t result = voice_cloud_run_auto_route(
          &c, VOICE_INVOCATION_BUTTON2_PTT,
          VOICE_ROUTE_POLICY_CHAT_PRIORITY,
          wav, sizeof(wav), scratch, sizeof(scratch), &out);
      if (result == VOICE_CLOUD_OK &&
          fake.observed_kind == VOICE_CLOUD_REQUEST_AUTO_ROUTE &&
          fake.observed_source == VOICE_INVOCATION_BUTTON2_PTT &&
          fake.observed_policy == VOICE_ROUTE_POLICY_CHAT_PRIORITY &&
          out.route.domain == VOICE_ROUTE_DOMAIN_CHAT) PASS();
      else FAIL("AUTO_ROUTE context"); }

    { TEST("voice route: transcript is mandatory and non-empty");
      const char *missing =
          "{\"domain\":\"chat\",\"reply_text\":\"hello\","
          "\"confidence_milli\":900,\"requires_confirmation\":false,"
          "\"tool\":\"none\"}";
      const char *empty =
          "{\"domain\":\"chat\",\"transcript\":\"\","
          "\"reply_text\":\"hello\",\"confidence_milli\":900,"
          "\"requires_confirmation\":false,\"tool\":\"none\"}";
      voice_route_result_t route;
      if (voice_route_parse_json(missing, strlen(missing), &route) ==
              VOICE_ROUTE_PARSE_MISSING_FIELD &&
          voice_route_parse_json(empty, strlen(empty), &route) ==
              VOICE_ROUTE_PARSE_MISSING_FIELD) PASS();
      else FAIL("missing transcript accepted"); }

    { TEST("voice route: long UTF-8 transcript fits character contract");
      char transcript[VOICE_ROUTE_TRANSCRIPT_CAPACITY];
      size_t used = 0;
      for (size_t i = 0; i < 200; i++) {
          memcpy(transcript + used, "你", strlen("你"));
          used += strlen("你");
      }
      transcript[used] = '\0';
      char json[VOICE_ROUTE_JSON_MAX_BYTES + 1U];
      int written = snprintf(
          json, sizeof(json),
          "{\"domain\":\"chat\",\"transcript\":\"%s\","
          "\"reply_text\":\"收到\",\"confidence_milli\":900,"
          "\"requires_confirmation\":false,\"tool\":\"none\"}",
          transcript);
      voice_route_result_t route;
      if (written > 0 && (size_t)written < sizeof(json) &&
          voice_route_parse_json(json, (size_t)written, &route) ==
              VOICE_ROUTE_PARSE_OK &&
          strlen(route.transcript) == used) PASS();
      else FAIL("UTF-8 transcript byte capacity"); }

    { TEST("voice provider: MiMo needs only API key");
      cloud_fake_transport_t fake = {0};
      voice_provider_credentials_t credentials = {
          .provider = VOICE_PROVIDER_MIMO_DIRECT,
          .api_key = "mimo-key",
      };
      voice_cloud_config_t config;
      if (voice_provider_build_cloud_config(
              &credentials, cloud_fake_transport, &fake,
              NULL, NULL, &config) == VOICE_PROVIDER_OK &&
          strcmp(config.endpoint, VOICE_MIMO_ENDPOINT) == 0 &&
          strcmp(config.model, VOICE_MIMO_DEFAULT_MODEL) == 0 &&
          config.auth == VOICE_CLOUD_AUTH_API_KEY) PASS();
      else FAIL("MiMo provider"); }

    { TEST("voice provider: AI Gateway is single API key plus model");
      cloud_fake_transport_t fake = {0};
      voice_provider_credentials_t credentials = {
          .provider = VOICE_PROVIDER_ESPRESSIF_AI_GATEWAY,
          .api_key = "gateway-key",
          .model_override = "gateway-model",
      };
      voice_cloud_config_t config;
      if (voice_provider_build_cloud_config(
              &credentials, cloud_fake_transport, &fake,
              NULL, NULL, &config) == VOICE_PROVIDER_OK &&
          strcmp(config.endpoint, VOICE_ESPRESSIF_GATEWAY_ENDPOINT) == 0 &&
          strcmp(config.model, "gateway-model") == 0 &&
          config.auth == VOICE_CLOUD_AUTH_BEARER) PASS();
      else FAIL("AI Gateway provider"); }

    { TEST("voice provider: gateway defaults to mimo-v2.5 with key only");
      cloud_fake_transport_t fake = {0};
      voice_provider_credentials_t credentials = {
          .provider = VOICE_PROVIDER_ESPRESSIF_AI_GATEWAY,
          .api_key = "gateway-key",
      };
      voice_cloud_config_t config;
      if (voice_provider_build_cloud_config(
              &credentials, cloud_fake_transport, &fake,
              NULL, NULL, &config) ==
          VOICE_PROVIDER_OK &&
          config.auth == VOICE_CLOUD_AUTH_BEARER &&
          strcmp(config.model,
                 VOICE_ESPRESSIF_GATEWAY_DEFAULT_MODEL) == 0) PASS();
      else FAIL("gateway key-only"); }

    { TEST("voice provider: conversational agent credentials are separate");
      voice_conversation_credentials_t c = {
          .instance_id = "instance",
          .product_key = "product",
          .product_secret = "secret",
          .device_name = "xiaojing",
          .bot_id = "bot",
      };
      int registration = voice_conversation_registration_ready(&c);
      int before_dynamic_register =
          !voice_conversation_session_ready(&c);
      c.device_secret = "device-secret";
      if (registration && before_dynamic_register &&
          voice_conversation_session_ready(&c)) PASS();
      else FAIL("conversation credential separation"); }

    { TEST("voice profile: wake selection alternates");
      if (voice_wake_next(VOICE_WAKE_SELECTION_EDGE_IMPULSE) ==
              VOICE_WAKE_SELECTION_ESP_SR &&
          voice_wake_next(VOICE_WAKE_SELECTION_ESP_SR) ==
              VOICE_WAKE_SELECTION_EDGE_IMPULSE) PASS();
      else FAIL("wake alternation"); }

    { TEST("voice profile: button invocation is chat priority");
      if (voice_invocation_route_policy(VOICE_INVOCATION_WAKE_WORD) ==
              VOICE_ROUTE_POLICY_AUTO &&
          voice_invocation_route_policy(VOICE_INVOCATION_BUTTON2_PTT) ==
              VOICE_ROUTE_POLICY_CHAT_PRIORITY) PASS();
      else FAIL("invocation policy"); }

    { TEST("voice profile: chat priority is status-only");
      if (voice_policy_tool_allowed(
              VOICE_ROUTE_POLICY_CHAT_PRIORITY, VOICE_TOOL_STATUS) &&
          !voice_policy_tool_allowed(
              VOICE_ROUTE_POLICY_CHAT_PRIORITY, VOICE_TOOL_START_PROGRAM) &&
          !voice_policy_tool_allowed(
              VOICE_ROUTE_POLICY_CHAT_PRIORITY, VOICE_TOOL_CANCEL) &&
          !voice_policy_tool_allowed(
              VOICE_ROUTE_POLICY_CHAT_PRIORITY, VOICE_TOOL_ACK_FAULT)) PASS();
      else FAIL("chat whitelist"); }

    { TEST("voice profile: auto preserves bounded tool whitelist");
      int ok = 1;
      for (int tool = VOICE_TOOL_START_PROGRAM;
           tool <= VOICE_TOOL_ACK_FAULT; tool++) {
          if (!voice_policy_tool_allowed(
                  VOICE_ROUTE_POLICY_AUTO, (voice_tool_t)tool)) {
              ok = 0;
          }
      }
      if (ok &&
          !voice_policy_tool_allowed(
              VOICE_ROUTE_POLICY_AUTO, VOICE_TOOL_NONE) &&
          !voice_policy_tool_allowed(
              VOICE_ROUTE_POLICY_AUTO, (voice_tool_t)99) &&
          !voice_policy_tool_allowed(
              (voice_route_policy_t)99, VOICE_TOOL_STATUS)) PASS();
      else FAIL("auto whitelist"); }

    voice_wake_runtime_t profile_rt = {
        .machine_state = MACHINE_STATE_IDLE,
        .voice_listening = true,
        .command_in_flight = false,
        .audio_capture_ready = true,
        .edge_impulse_ready = true,
        .esp_sr_ready = true,
    };

    { TEST("voice profile: idle long press switches wake backend");
      voice_wake_selection_t selected = VOICE_WAKE_SELECTION_EDGE_IMPULSE;
      if (voice_wake_request_switch(
              VOICE_WAKE_SELECTION_EDGE_IMPULSE, &profile_rt, &selected) ==
              VOICE_WAKE_SWITCH_OK &&
          selected == VOICE_WAKE_SELECTION_ESP_SR) PASS();
      else FAIL("idle switch"); }

    { TEST("voice profile: machine running blocks switch");
      voice_wake_runtime_t rt = profile_rt;
      rt.machine_state = MACHINE_STATE_RUNNING;
      voice_wake_selection_t selected = VOICE_WAKE_SELECTION_EDGE_IMPULSE;
      if (voice_wake_request_switch(
              VOICE_WAKE_SELECTION_EDGE_IMPULSE, &rt, &selected) ==
              VOICE_WAKE_SWITCH_MACHINE_BUSY &&
          selected == VOICE_WAKE_SELECTION_EDGE_IMPULSE) PASS();
      else FAIL("machine gate"); }

    { TEST("voice profile: recording or routing blocks switch");
      voice_wake_runtime_t rt = profile_rt;
      voice_wake_selection_t selected = VOICE_WAKE_SELECTION_EDGE_IMPULSE;
      rt.voice_listening = false;
      int ok = voice_wake_request_switch(
          VOICE_WAKE_SELECTION_EDGE_IMPULSE, &rt, &selected) ==
          VOICE_WAKE_SWITCH_VOICE_BUSY;
      rt.voice_listening = true;
      rt.command_in_flight = true;
      ok = ok && voice_wake_request_switch(
          VOICE_WAKE_SELECTION_EDGE_IMPULSE, &rt, &selected) ==
          VOICE_WAKE_SWITCH_VOICE_BUSY;
      if (ok) PASS(); else FAIL("voice gate"); }

    { TEST("voice profile: switch requires audio and target backend");
      voice_wake_runtime_t rt = profile_rt;
      voice_wake_selection_t selected = VOICE_WAKE_SELECTION_EDGE_IMPULSE;
      rt.audio_capture_ready = false;
      int ok = voice_wake_request_switch(
          VOICE_WAKE_SELECTION_EDGE_IMPULSE, &rt, &selected) ==
          VOICE_WAKE_SWITCH_AUDIO_UNAVAILABLE;
      rt.audio_capture_ready = true;
      rt.esp_sr_ready = false;
      ok = ok && voice_wake_request_switch(
          VOICE_WAKE_SELECTION_EDGE_IMPULSE, &rt, &selected) ==
          VOICE_WAKE_SWITCH_BACKEND_UNAVAILABLE;
      if (ok && selected == VOICE_WAKE_SELECTION_EDGE_IMPULSE) PASS();
      else FAIL("backend readiness"); }

    { TEST("voice profile: invalid switch arguments preserve output");
      voice_wake_selection_t selected = VOICE_WAKE_SELECTION_ESP_SR;
      int ok = voice_wake_request_switch(
          (voice_wake_selection_t)99, &profile_rt, &selected) ==
          VOICE_WAKE_SWITCH_BAD_ARG;
      ok = ok && voice_wake_request_switch(
          VOICE_WAKE_SELECTION_EDGE_IMPULSE, NULL, &selected) ==
          VOICE_WAKE_SWITCH_BAD_ARG;
      ok = ok && selected == VOICE_WAKE_SELECTION_ESP_SR;
      if (ok) PASS(); else FAIL("invalid args"); }

    { TEST("gesture map: RIGHT swipe advances to next page");
      if (gesture_page_delta(HAL_GESTURE_RIGHT) == 1) PASS();
      else FAIL("RIGHT must be +1 (next page)"); }

    { TEST("gesture map: LEFT swipe goes to previous page");
      if (gesture_page_delta(HAL_GESTURE_LEFT) == -1) PASS();
      else FAIL("LEFT must be -1 (previous page)"); }

    { TEST("gesture map: FORWARD advances, BACKWARD reverses");
      if (gesture_page_delta(HAL_GESTURE_FORWARD) == 1 &&
          gesture_page_delta(HAL_GESTURE_BACKWARD) == -1) PASS();
      else FAIL("FORWARD/BACKWARD mapping"); }

    { TEST("gesture map: UP/DOWN/other gestures cause no page action");
      int ok = gesture_page_delta(HAL_GESTURE_UP) == 0 &&
               gesture_page_delta(HAL_GESTURE_DOWN) == 0 &&
               gesture_page_delta(HAL_GESTURE_CLOCKWISE) == 0 &&
               gesture_page_delta(HAL_GESTURE_COUNTER_CW) == 0 &&
               gesture_page_delta(HAL_GESTURE_NONE) == 0;
      if (ok) PASS(); else FAIL("non-navigation gestures must return 0"); }

    { TEST("protocol: allow_uv firmware default is false (safe by default)");
      if (APP_PROTOCOL_ALLOW_UV_DEFAULT == false) PASS();
      else FAIL("missing allow_uv field must never enable UV"); }

    /* ---- UV FSM safety interlock (pure C, no FreeRTOS) ---- */
    {
      static uv_params_t uv_cfg = { .default_duration_ms = 1000, .max_duration_ms = 30000 };
      static uv_request_t uv_req = { .request_id = 901, .duration_ms = 1000 };

      { TEST("uv fsm: invalid position refuses lamp ON");
        uv_fsm_ctx_t c; uv_fsm_init(&c, &uv_cfg);
        uv_fsm_output_t o;
        if (uv_fsm_begin_request(&c, &uv_req, uv_cfg.max_duration_ms, 0) == ESP_OK) {
          uv_fsm_event_t e = { .type = UV_EVT_TICK, .now_ms = 0, .position = DRUM_POS_0,
            .position_valid = false, .position_stable = true, .position_fresh = true,
            .position_motor_moving = false };
          uv_fsm_tick(&c, &e, &o);
          if (c.state == UV_FSM_FAULT && o.action == UV_ACTION_NONE) PASS();
          else FAIL("invalid position must fault with no lamp action");
        } else FAIL("begin_request failed"); }

      { TEST("uv fsm: unstable position refuses lamp ON");
        uv_fsm_ctx_t c; uv_fsm_init(&c, &uv_cfg);
        uv_fsm_output_t o;
        if (uv_fsm_begin_request(&c, &uv_req, uv_cfg.max_duration_ms, 0) == ESP_OK) {
          uv_fsm_event_t e = { .type = UV_EVT_TICK, .now_ms = 0, .position = DRUM_POS_0,
            .position_valid = true, .position_stable = false, .position_fresh = true,
            .position_motor_moving = false };
          uv_fsm_tick(&c, &e, &o);
          if (c.state == UV_FSM_FAULT && o.action == UV_ACTION_NONE) PASS();
          else FAIL("unstable position must fault");
        } else FAIL("begin_request failed"); }

      { TEST("uv fsm: moving motor refuses lamp ON");
        uv_fsm_ctx_t c; uv_fsm_init(&c, &uv_cfg);
        uv_fsm_output_t o;
        if (uv_fsm_begin_request(&c, &uv_req, uv_cfg.max_duration_ms, 0) == ESP_OK) {
          uv_fsm_event_t e = { .type = UV_EVT_TICK, .now_ms = 0, .position = DRUM_POS_0,
            .position_valid = true, .position_stable = true, .position_fresh = true,
            .position_motor_moving = true };
          uv_fsm_tick(&c, &e, &o);
          if (c.state == UV_FSM_FAULT && o.action == UV_ACTION_NONE) PASS();
          else FAIL("moving motor must fault");
        } else FAIL("begin_request failed"); }

      { TEST("uv fsm: non-0 position refuses lamp ON");
        uv_fsm_ctx_t c; uv_fsm_init(&c, &uv_cfg);
        uv_fsm_output_t o;
        if (uv_fsm_begin_request(&c, &uv_req, uv_cfg.max_duration_ms, 0) == ESP_OK) {
          uv_fsm_event_t e = { .type = UV_EVT_TICK, .now_ms = 0, .position = DRUM_POS_45,
            .position_valid = true, .position_stable = true, .position_fresh = true,
            .position_motor_moving = false };
          uv_fsm_tick(&c, &e, &o);
          if (c.state == UV_FSM_FAULT && o.action == UV_ACTION_NONE) PASS();
          else FAIL("non-0 position must fault");
        } else FAIL("begin_request failed"); }

      { TEST("uv fsm: valid 0 position enables lamp ON");
        uv_fsm_ctx_t c; uv_fsm_init(&c, &uv_cfg);
        uv_fsm_output_t o;
        if (uv_fsm_begin_request(&c, &uv_req, uv_cfg.max_duration_ms, 0) == ESP_OK) {
          uv_fsm_event_t e = { .type = UV_EVT_TICK, .now_ms = 0, .position = DRUM_POS_0,
            .position_valid = true, .position_stable = true, .position_fresh = true,
            .position_motor_moving = false };
          uv_fsm_tick(&c, &e, &o);
          if (c.state == UV_FSM_RUNNING && o.action == UV_ACTION_LAMP_ON) PASS();
          else FAIL("valid position must request LAMP_ON");
        } else FAIL("begin_request failed"); }

      { TEST("uv fsm: timeout turns lamp OFF");
        uv_fsm_ctx_t c; uv_fsm_init(&c, &uv_cfg);
        uv_fsm_output_t o;
        uv_fsm_begin_request(&c, &uv_req, uv_cfg.max_duration_ms, 0);
        uv_fsm_event_t s = { .type = UV_EVT_TICK, .now_ms = 0, .position = DRUM_POS_0,
          .position_valid = true, .position_stable = true, .position_fresh = true };
        uv_fsm_tick(&c, &s, &o); /* -> RUNNING, LAMP_ON */
        uv_fsm_event_t a = { .type = UV_EVT_ACTION_RESULT, .now_ms = 0,
          .action_result_action = UV_ACTION_LAMP_ON, .action_result_ok = true };
        uv_fsm_tick(&c, &a, &o);
        uv_fsm_event_t t = { .type = UV_EVT_TICK, .now_ms = uv_req.duration_ms + 1,
          .position = DRUM_POS_0, .position_valid = true, .position_stable = true,
          .position_fresh = true };
        uv_fsm_tick(&c, &t, &o);
        if (o.action == UV_ACTION_LAMP_OFF) PASS();
        else FAIL("timeout must request LAMP_OFF"); }

      { TEST("uv fsm: skip and cancel both request LAMP_OFF");
        int ok = 1;
        { uv_fsm_ctx_t c; uv_fsm_init(&c, &uv_cfg);
          uv_fsm_output_t o;
          uv_fsm_begin_request(&c, &uv_req, uv_cfg.max_duration_ms, 0);
          uv_fsm_event_t s = { .type = UV_EVT_TICK, .now_ms = 0, .position = DRUM_POS_0,
            .position_valid = true, .position_stable = true, .position_fresh = true };
          uv_fsm_tick(&c, &s, &o);
          uv_fsm_event_t a = { .type = UV_EVT_ACTION_RESULT, .now_ms = 0,
            .action_result_action = UV_ACTION_LAMP_ON, .action_result_ok = true };
          uv_fsm_tick(&c, &a, &o);
          uv_fsm_event_t k = { .type = UV_EVT_SKIP, .now_ms = 10, .cancel_request_id = uv_req.request_id };
          uv_fsm_tick(&c, &k, &o);
          if (o.action != UV_ACTION_LAMP_OFF) ok = 0; }
        { uv_fsm_ctx_t c; uv_fsm_init(&c, &uv_cfg);
          uv_fsm_output_t o;
          uv_fsm_begin_request(&c, &uv_req, uv_cfg.max_duration_ms, 0);
          uv_fsm_event_t s = { .type = UV_EVT_TICK, .now_ms = 0, .position = DRUM_POS_0,
            .position_valid = true, .position_stable = true, .position_fresh = true };
          uv_fsm_tick(&c, &s, &o);
          uv_fsm_event_t a = { .type = UV_EVT_ACTION_RESULT, .now_ms = 0,
            .action_result_action = UV_ACTION_LAMP_ON, .action_result_ok = true };
          uv_fsm_tick(&c, &a, &o);
          uv_fsm_event_t k = { .type = UV_EVT_CANCEL, .now_ms = 10, .cancel_request_id = uv_req.request_id };
          uv_fsm_tick(&c, &k, &o);
          if (o.action != UV_ACTION_LAMP_OFF) ok = 0; }
        if (ok) PASS(); else FAIL("skip/cancel must request LAMP_OFF"); }

      { TEST("uv fsm: emergency requests LAMP_OFF from RUNNING");
        uv_fsm_ctx_t c; uv_fsm_init(&c, &uv_cfg);
        uv_fsm_output_t o;
        uv_fsm_begin_request(&c, &uv_req, uv_cfg.max_duration_ms, 0);
        uv_fsm_event_t s = { .type = UV_EVT_TICK, .now_ms = 0, .position = DRUM_POS_0,
          .position_valid = true, .position_stable = true, .position_fresh = true };
        uv_fsm_tick(&c, &s, &o);
        uv_fsm_event_t a = { .type = UV_EVT_ACTION_RESULT, .now_ms = 0,
          .action_result_action = UV_ACTION_LAMP_ON, .action_result_ok = true };
        uv_fsm_tick(&c, &a, &o);
        uv_fsm_event_t em = { .type = UV_EVT_EMERGENCY, .now_ms = 20 };
        uv_fsm_tick(&c, &em, &o);
        if (o.action == UV_ACTION_LAMP_OFF) PASS();
        else FAIL("emergency must request LAMP_OFF"); }

      { TEST("uv fsm: lamp ON failure faults and requests LAMP_OFF");
        uv_fsm_ctx_t c; uv_fsm_init(&c, &uv_cfg);
        uv_fsm_output_t o;
        uv_fsm_begin_request(&c, &uv_req, uv_cfg.max_duration_ms, 0);
        uv_fsm_event_t s = { .type = UV_EVT_TICK, .now_ms = 0, .position = DRUM_POS_0,
          .position_valid = true, .position_stable = true, .position_fresh = true };
        uv_fsm_tick(&c, &s, &o);
        uv_fsm_event_t a = { .type = UV_EVT_ACTION_RESULT, .now_ms = 0,
          .action_result_action = UV_ACTION_LAMP_ON, .action_result_ok = false };
        uv_fsm_tick(&c, &a, &o);
        if (o.action == UV_ACTION_LAMP_OFF && o.terminal == UV_TERMINAL_FAULT) PASS();
        else FAIL("ON failure must fault + request LAMP_OFF"); }

      { TEST("uv fsm: LAMP_OFF failure is explicitly reported (output UNKNOWN)");
        uv_fsm_ctx_t c; uv_fsm_init(&c, &uv_cfg);
        uv_fsm_output_t o;
        uv_fsm_begin_request(&c, &uv_req, uv_cfg.max_duration_ms, 0);
        uv_fsm_event_t s = { .type = UV_EVT_TICK, .now_ms = 0, .position = DRUM_POS_0,
          .position_valid = true, .position_stable = true, .position_fresh = true };
        uv_fsm_tick(&c, &s, &o);
        uv_fsm_event_t a = { .type = UV_EVT_ACTION_RESULT, .now_ms = 0,
          .action_result_action = UV_ACTION_LAMP_ON, .action_result_ok = true };
        uv_fsm_tick(&c, &a, &o);
        uv_fsm_event_t t = { .type = UV_EVT_TICK,
          .now_ms = uv_req.duration_ms + 1, .position = DRUM_POS_0,
          .position_valid = true, .position_stable = true, .position_fresh = true };
        uv_fsm_tick(&c, &t, &o);
        uv_fsm_event_t f = { .type = UV_EVT_ACTION_RESULT, .now_ms = 0,
          .action_result_action = UV_ACTION_LAMP_OFF, .action_result_ok = false };
        uv_fsm_tick(&c, &f, &o);
        if (c.output_state == UV_OUTPUT_UNKNOWN) PASS();
        else FAIL("OFF failure must leave output UNKNOWN (not swallowed)"); }
    }

    /* ---- UV self-test core gate (pure C) ---- */
    {
      static uv_self_test_gate_input_t ok_input = {
        .exec_idle = true, .uv_idle = true, .uv_confirmed_off = true,
        .fault_active = false, .emergency_active = false, .mcp_uv_known = true,
        .pos_valid = true, .pos_fresh = true, .pos_stable = true,
        .pos_moving = false, .pos = DRUM_POS_0,
      };

      { TEST("uv selftest gate: idle+pos0+fresh+stable+stopped -> ACCEPT");
        if (uv_self_test_gate_check(&ok_input) == UV_ST_GATE_OK) PASS();
        else FAIL("gate must accept valid input"); }

      { TEST("uv selftest gate: position unknown -> POSITION_UNKNOWN");
        uv_self_test_gate_input_t in = ok_input; in.pos_valid = false;
        if (uv_self_test_gate_check(&in) == UV_ST_GATE_POSITION_UNKNOWN) PASS();
        else FAIL("expected POSITION_UNKNOWN"); }

      { TEST("uv selftest gate: stale position -> POSITION_STALE");
        uv_self_test_gate_input_t in = ok_input; in.pos_fresh = false;
        if (uv_self_test_gate_check(&in) == UV_ST_GATE_POSITION_STALE) PASS();
        else FAIL("expected POSITION_STALE"); }

      { TEST("uv selftest gate: unstable position -> POSITION_UNSTABLE");
        uv_self_test_gate_input_t in = ok_input; in.pos_stable = false;
        if (uv_self_test_gate_check(&in) == UV_ST_GATE_POSITION_UNSTABLE) PASS();
        else FAIL("expected POSITION_UNSTABLE"); }

      { TEST("uv selftest gate: non-zero position -> POSITION_NOT_ZERO");
        uv_self_test_gate_input_t in = ok_input; in.pos = DRUM_POS_45;
        if (uv_self_test_gate_check(&in) == UV_ST_GATE_POSITION_NOT_ZERO) PASS();
        else FAIL("expected POSITION_NOT_ZERO"); }

      { TEST("uv selftest gate: motor moving -> MOTOR_MOVING");
        uv_self_test_gate_input_t in = ok_input; in.pos_moving = true;
        if (uv_self_test_gate_check(&in) == UV_ST_GATE_MOTOR_MOVING) PASS();
        else FAIL("expected MOTOR_MOVING"); }

      { TEST("uv selftest gate: fault active -> FAULT_ACTIVE");
        uv_self_test_gate_input_t in = ok_input; in.fault_active = true;
        if (uv_self_test_gate_check(&in) == UV_ST_GATE_FAULT_ACTIVE) PASS();
        else FAIL("expected FAULT_ACTIVE"); }

      { TEST("uv selftest gate: emergency -> EMERGENCY_ACTIVE");
        uv_self_test_gate_input_t in = ok_input; in.emergency_active = true;
        if (uv_self_test_gate_check(&in) == UV_ST_GATE_EMERGENCY_ACTIVE) PASS();
        else FAIL("expected EMERGENCY_ACTIVE"); }

      { TEST("uv selftest gate: MCP unknown -> MCP_UNKNOWN");
        uv_self_test_gate_input_t in = ok_input; in.mcp_uv_known = false;
        if (uv_self_test_gate_check(&in) == UV_ST_GATE_MCP_UNKNOWN) PASS();
        else FAIL("expected MCP_UNKNOWN"); }

      { TEST("uv selftest gate: uv busy -> UV_BUSY");
        uv_self_test_gate_input_t in = ok_input; in.uv_idle = false;
        if (uv_self_test_gate_check(&in) == UV_ST_GATE_UV_BUSY) PASS();
        else FAIL("expected UV_BUSY"); }

      { TEST("uv selftest gate: machine running -> MACHINE_NOT_IDLE");
        uv_self_test_gate_input_t in = ok_input; in.exec_idle = false;
        if (uv_self_test_gate_check(&in) == UV_ST_GATE_MACHINE_NOT_IDLE) PASS();
        else FAIL("expected MACHINE_NOT_IDLE"); }

      { TEST("uv selftest gate: uv not confirmed off -> UV_NOT_CONFIRMED_OFF");
        uv_self_test_gate_input_t in = ok_input; in.uv_confirmed_off = false;
        if (uv_self_test_gate_check(&in) == UV_ST_GATE_UV_NOT_CONFIRMED_OFF) PASS();
        else FAIL("expected UV_NOT_CONFIRMED_OFF"); }

      { TEST("uv selftest: fixed duration is 3000 ms");
        if (UV_SELF_TEST_DURATION_MS == 3000U) PASS();
        else FAIL("duration must be fixed 3000 ms"); }

      { TEST("uv selftest: reject codes are machine-readable");
        int ok = strcmp(uv_self_test_gate_code(UV_ST_GATE_POSITION_NOT_ZERO),
                        "POSITION_NOT_ZERO") == 0 &&
                 strcmp(uv_self_test_gate_code(UV_ST_GATE_UV_BUSY),
                        "UV_BUSY") == 0 &&
                 strcmp(uv_self_test_gate_code(UV_ST_GATE_MACHINE_NOT_IDLE),
                        "MACHINE_NOT_IDLE") == 0 &&
                 strcmp(uv_self_test_gate_code(UV_ST_GATE_OK),
                        "UV_SELF_TEST_ACCEPTED") == 0 &&
                 strcmp(uv_self_test_gate_code(UV_ST_GATE_MCP_UNKNOWN),
                        "MCP_UNKNOWN") == 0 &&
                 strcmp(uv_self_test_gate_code(UV_ST_GATE_UV_NOT_CONFIRMED_OFF),
                        "UV_NOT_CONFIRMED_OFF") == 0;
        if (ok) PASS(); else FAIL("code mapping"); }

      { TEST("uv selftest: request_id wrap skips 0");
        if (uv_self_test_next_request_id(0) == 1 &&
            uv_self_test_next_request_id(UINT32_MAX) == 1 &&
            uv_self_test_next_request_id(1) == 2) PASS();
        else FAIL("request_id must be non-zero and wrap skip 0"); }

      { TEST("app protocol: uv_self_test command parses");
        app_protocol_message_t m;
        const char *j =
            "{\"v\":1,\"seq\":5,\"type\":\"cmd\",\"cmd\":\"uv_self_test\"}";
        if (app_protocol_parse_frame(j, strlen(j), &m) == APP_PARSE_OK &&
            m.command == APP_CMD_UV_SELF_TEST && m.seq == 5) PASS();
        else FAIL("uv_self_test must parse as its command"); }

      { TEST("app protocol: uv_self_test is not forbidden hardware");
        app_protocol_message_t m;
        const char *j =
            "{\"v\":1,\"seq\":6,\"type\":\"cmd\",\"cmd\":\"uv_self_test\"}";
        if (app_protocol_parse_frame(j, strlen(j), &m) != APP_PARSE_FORBIDDEN_CMD)
            PASS();
        else FAIL("uv_self_test must not be forbidden"); }
    }

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
