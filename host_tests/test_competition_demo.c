#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "program_control_core.h"
#include "wash_executor_core.h"
#include "safety_interlocks.h"
#include "button_debounce.h"
#include "bl50_fsm.h"
#include "uv_fsm.h"
#include "competition_demo.h"

static safety_inputs_t inputs(void)
{
    safety_inputs_t s = {0};
    s.now_ms = 1000; s.position_sample_ms = 1000;
    s.position = DRUM_POS_270; s.position_sample_valid = true; s.position_stable = true;
    s.bl50_state_known = true;
    s.mcp_known_mask = (1U << SAFE_OUTPUT_COUNT) - 1;
    s.sht_valid = s.sht_fresh = true; s.sht_sample.temperature_c = 25;
    s.demo_program_id = 42; s.demo_generation = 1; s.demo_deadline_ms = 180000;
    return s;
}

static safety_interlock_config_t config(void)
{
    safety_interlock_config_t c = {0};
    c.output_mode = XIAOJING_MODE_REAL; c.ptc_enabled = true;
    c.board_identity_confirmed = true; c.position_stale_timeout_ms = 500;
    c.hot_air_module_enabled = true;
    c.heater_cutoff_c = 55; c.heater_resume_c = 45;
    return c;
}

static bool allowed(safety_inputs_t *s, safety_operation_t op, bool on)
{
    safety_request_t r = {.operation = op, .enable = on, .required_position = s->position};
    r.params.motor.pwm_percent = 30;
    safety_decision_t d; safety_interlock_config_t c = config();
    safety_interlock_check(s, &c, &r, &d); return d.allowed;
}

static void test_policy(void)
{
    safety_inputs_t s = inputs(); s.mcp_outputs.uv = true;
    assert(allowed(&s, SAFETY_OP_POSITION_MOTOR, true));
    assert(allowed(&s, SAFETY_OP_BL50, true));
    assert(!allowed(&s, SAFETY_OP_HOT_AIR_MODULE, true));
    s.demo_dry_phase = true;
    assert(allowed(&s, SAFETY_OP_HOT_AIR_MODULE, true));
    s.sht_fresh = false; assert(!allowed(&s, SAFETY_OP_HOT_AIR_MODULE, true));
    s.sht_fresh = true; s.sht_sample.temperature_c = 55;
    assert(!allowed(&s, SAFETY_OP_HOT_AIR_MODULE, true));
    s.sht_sample.temperature_c = 25;
    s.mcp_outputs.ptc_heater = true;
    assert(allowed(&s, SAFETY_OP_BL50, true));
    assert(!allowed(&s, SAFETY_OP_POSITION_MOTOR, true));
    s.position = DRUM_POS_90; assert(!allowed(&s, SAFETY_OP_BL50, true));
    s.position = DRUM_POS_270;
    s.mcp_known_mask &= ~(1U << SAFE_OUTPUT_PTC_HEATER);
    assert(!allowed(&s, SAFETY_OP_BL50, true));
    s = inputs(); s.position = DRUM_POS_0;
    assert(!allowed(&s, SAFETY_OP_SOURCE_INLET_VALVE, true));
    s = inputs(); s.mcp_outputs.uv = true;
    s.demo_program_id = 0; assert(!allowed(&s, SAFETY_OP_BL50, true));
    s.demo_program_id = 42; s.now_ms = s.demo_deadline_ms;
    assert(!allowed(&s, SAFETY_OP_BL50, true));
    s = inputs(); s.fault_active = true;
    assert(!allowed(&s, SAFETY_OP_UV, true)); assert(allowed(&s, SAFETY_OP_UV, false));
    s.fault_active = false; s.emergency_stop_active = true;
    assert(!allowed(&s, SAFETY_OP_BL50, true)); assert(allowed(&s, SAFETY_OP_BL50, false));
}

static void test_stop(void)
{
    wash_program_t p;
    assert(competition_demo_build_program(0, &p).result == PLAN_RESULT_REJECTED);
    assert(competition_demo_build_program(42, &p).result == PLAN_RESULT_OK);
    assert(p.step_count == 1 && p.steps[0].type == STEP_COMPETITION_DEMO);
    assert(p.estimated_total_ms < 180000 && p.steps[0].timeout_ms >= 180000);
    for (int failure = 0; failure < 2; ++failure) {
        wash_executor_core_t c; core_init(&c); c.program = &p; c.total_steps = 1;
        core_cmd_t cmds[16]; core_event_t e = {.type = CORE_EVENT_PROGRAM_ACCEPTED};
        e.token.program_id = 42;
        assert(core_process_event(&c, &e, cmds, 16) == 1);
        e = (core_event_t){.type = CORE_EVENT_STEP_DISPATCH_OK};
        core_process_event(&c, &e, cmds, 16);
        assert(!core_can_skip_current_step(&c));
        terminal_token_t stale = c.active_token;
        e.type = CORE_EVENT_CANCEL;
        assert(core_process_event(&c, &e, cmds, 16) == 1);
        assert(cmds[0].type == CORE_CMD_CANCEL_ACTIVE_STEP);
        e = (core_event_t){.type = CORE_EVENT_SERVICE_TERMINAL, .token = stale, .success = true};
        assert(core_process_event(&c, &e, cmds, 16) == 0);
        e.token = c.cancel_token; e.success = !failure;
        uint32_t n = core_process_event(&c, &e, cmds, 16);
        bool terminal = false;
        for (uint32_t i=0; i<n; ++i) {
            assert(cmds[i].type != CORE_CMD_REQUEST_FINAL_RESET);
            if (cmds[i].type == CORE_CMD_PUBLISH_PROGRAM_TERMINAL) {
                terminal = true; assert(!cmds[i].terminal_success);
                assert(!cmds[i].final_position_confirmed);
                assert(cmds[i].terminal_result == (failure ? TERM_RESULT_FAULT : TERM_RESULT_CANCELED));
            }
        }
        assert(terminal && !c.program_active);
        assert(core_process_event(&c, &e, cmds, 16) == 0);
    }
}

static void test_button(void)
{
    button_debounce_ctx_t c; button_event_t ev[3];
    uint8_t idle = BUTTON_RAW_MASK, pressed = idle & ~(1U << (BUTTON_RAW_BIT_BTN1+1));
    button_debounce_init(&c, 30); c.press_event_mask = 1U << (BUTTON_RAW_BIT_BTN1+1);
    button_debounce_seed(&c, pressed, 0);
    assert(button_debounce_process(&c, pressed, 100, ev, 3) == 0);
    button_debounce_process(&c, idle, 110, ev, 3);
    assert(button_debounce_process(&c, idle, 141, ev, 3) == 0);
    assert(button_debounce_process(&c, pressed, 200, ev, 3) == 0);
    assert(button_debounce_process(&c, pressed, 229, ev, 3) == 0);
    assert(button_debounce_process(&c, pressed, 230, ev, 3) == 1);
    assert(ev[0].button_id == BUTTON_ID_2 && ev[0].event_type == BUTTON_EVENT_PRESS);
    assert(button_debounce_process(&c, pressed, 250, ev, 3) == 0);
}

static void test_tumble(void)
{
    bl50_fsm_ctx_t c; bl50_fsm_init(&c);
    bl50_request_t r = {.request_id = 1, .action = BL50_ACTION_TUMBLE,
                       .duration_ms = 2000, .target_pwm_percent = 40};
    assert(bl50_action_to_position(r.action) == DRUM_POS_270);
    r.target_pwm_percent = 41;
    assert(bl50_fsm_begin_request(&c, &r, 40, 15000, 30000, true, false) == ESP_ERR_INVALID_ARG);
    r.target_pwm_percent = 40; r.duration_ms = 2001;
    assert(bl50_fsm_begin_request(&c, &r, 40, 15000, 30000, true, false) == ESP_ERR_INVALID_ARG);
    r.duration_ms = 2000;
    assert(bl50_fsm_begin_request(&c, &r, 40, 15000, 30000, true, false) == ESP_OK);
}

static void test_uv_indicator(void)
{
    uv_params_t config = {.max_duration_ms = 180000};
    uv_fsm_ctx_t c; uv_fsm_output_t out;
    uv_request_t r = {.request_id = 1, .duration_ms = 180000, .competition_demo = true};
    uv_fsm_event_t e = {.type = UV_EVT_TICK, .now_ms = 100,
        .position = DRUM_POS_90, .position_valid = true, .position_fresh = true,
        .position_stable = true};
    uv_fsm_init(&c, &config);
    assert(uv_fsm_begin_request(&c, &r, 180000, 0) == ESP_OK);
    uv_fsm_tick(&c, &e, &out);
    assert(out.action != UV_ACTION_LAMP_ON && out.terminal == UV_TERMINAL_REJECTED);
    uv_fsm_init(&c, &config);
    assert(uv_fsm_begin_request(&c, &r, 180000, 0) == ESP_OK);
    e.demo_indicator_allowed = true; e.position_motor_moving = true;
    uv_fsm_tick(&c, &e, &out);
    assert(out.action == UV_ACTION_LAMP_ON);
    e = (uv_fsm_event_t){.type = UV_EVT_ACTION_RESULT, .now_ms = 110,
        .action_result_action = UV_ACTION_LAMP_ON, .action_result_ok = true};
    uv_fsm_tick(&c, &e, &out);
    e = (uv_fsm_event_t){.type = UV_EVT_TICK, .now_ms = 120};
    uv_fsm_tick(&c, &e, &out);
    assert(out.action == UV_ACTION_LAMP_OFF && !out.emit_event);
    e = (uv_fsm_event_t){.type = UV_EVT_ACTION_RESULT, .now_ms = 130,
        .action_result_action = UV_ACTION_LAMP_OFF, .action_result_ok = true};
    uv_fsm_tick(&c, &e, &out);
    assert(out.emit_event && out.terminal == UV_TERMINAL_TIMEOUT);
}

int main(void)
{
    assert(!COMPETITION_DEMO_SHAFT_HOME_REQUIRED);
    machine_event_t failure = {.result = SERVICE_RESULT_FAULT,
        .fault = {.code = FAULT_BL50_HOME_TIMEOUT, .detail = 10000}};
    assert(competition_demo_event_fault(&failure) == FAULT_BL50_HOME_TIMEOUT);
    failure.fault.code = FAULT_NONE; failure.result = SERVICE_RESULT_TIMEOUT;
    assert(competition_demo_event_fault(&failure) == FAULT_PROGRAM_TIMEOUT);
    test_policy(); test_stop(); test_button(); test_tumble(); test_uv_indicator();
    puts("competition policy, cancel acknowledgement, stale terminal, debounce, tumble: PASS");
    return 0;
}
