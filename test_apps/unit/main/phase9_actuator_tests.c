/*
 * phase9_actuator_tests.c — 执行器板测 Fake HAL 隔离与设备端自动关断测试
 *
 * 证明：
 *   - SOURCE 只改变 GPA0；TRANSFER 只改变 GPA1；DETERGENT 只改变 GPB7；
 *   - 不触碰 UV/排水/PTC；
 *   - 设备端（服务自身计时）到期自动 OFF；
 *   - 紧急关断（water_service_emergency_close / detergent emergency）关输出；
 *   - 非法参数不产生硬件写；
 *   - 所有写经 safety_manager（fake_hal 历史只含 set_safe_output，无 raw MCP）。
 */

#include <string.h>

#include "unity.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fake_hal.h"
#include "safety_manager.h"
#include "water_service.h"
#include "detergent_service.h"
#include "machine_config.h"

#define ACT_PHASE9_TAG "phase9_actuator"

static fake_hal_ctx_t *s_hal_ctx = NULL;
static const xiaojing_hal_t *s_hal = NULL;
static bool s_services_up = false;

static esp_err_t act_event_sink(const machine_event_t *event,
                                uint32_t timeout_ms, void *context)
{
    (void)event; (void)timeout_ms; (void)context;
    return ESP_OK;
}

static esp_err_t actuator_svc_setup(void)
{
    water_service_global_init();
    detergent_service_global_init();
    s_hal_ctx = fake_hal_create();
    if (!s_hal_ctx) return ESP_ERR_NO_MEM;
    s_hal = fake_hal_get_interface(s_hal_ctx);
    fake_hal_set_time(s_hal_ctx, 10000);
    safety_manager_config_t sm_cfg = {
        .hal = s_hal, .config = machine_config_get(),
        .output_mode = XIAOJING_MODE_FAKE, .ptc_enabled = false,
        .board_identity_confirmed = true,
    };
    if (safety_manager_init(&sm_cfg) != ESP_OK ||
        safety_manager_start() != ESP_OK) {
        fake_hal_destroy(s_hal_ctx); s_hal_ctx = NULL; s_hal = NULL;
        safety_manager_test_reset();
        return ESP_ERR_INVALID_STATE;
    }
    safety_manager_update_position(DRUM_POS_0, true, false, true, 10000);
    fake_hal_clear_history(s_hal_ctx);

    const water_params_t *wp = &machine_config_get()->water;
    water_service_config_t ws_cfg = {
        .pulses_per_liter = wp->pulses_per_liter,
        .no_flow_timeout_ms = wp->no_flow_timeout_ms,
        .low_flow_window_ms = wp->low_flow_window_ms, .low_flow_min_pulses = 0,
        .source_batch_target_pulses = wp->source_batch_target_pulses,
        .source_batch_max_ms = wp->source_batch_max_ms,
        .source_settle_ms = wp->source_settle_ms,
        .transfer_timeout_ms = wp->transfer_timeout_ms,
        .default_transfer_ms = wp->default_transfer_ms,
        .max_fill_cycles = wp->max_fill_cycles,
        .total_inlet_timeout_ms = wp->total_inlet_timeout_ms,
    };
    machine_event_sink_t sink = { .publish = act_event_sink, .context = NULL };
    if (water_service_init(&ws_cfg, s_hal, sink) != ESP_OK ||
        water_service_start() != ESP_OK ||
        detergent_service_init(&machine_config_get()->detergent, s_hal, sink)
            != ESP_OK ||
        detergent_service_start() != ESP_OK) {
        water_service_stop();
        detergent_service_stop();
        safety_manager_stop(); safety_manager_test_reset();
        fake_hal_destroy(s_hal_ctx); s_hal_ctx = NULL; s_hal = NULL;
        return ESP_ERR_INVALID_STATE;
    }
    s_services_up = true;
    return ESP_OK;
}

static void actuator_svc_teardown(void)
{
    if (s_services_up) {
        water_service_stop();
        detergent_service_stop();
    }
    s_services_up = false;
    safety_manager_stop();
    if (s_hal_ctx) {
        fake_hal_destroy(s_hal_ctx); s_hal_ctx = NULL; s_hal = NULL;
    }
    safety_manager_test_reset();
}

/* Count safe-output writes of a given output (any enable value). */
static size_t count_safe_writes(fake_output_type_t type, safe_output_t output)
{
    fake_output_record_t recs[FAKE_HAL_HISTORY_CAPACITY];
    size_t n = fake_hal_get_history(s_hal_ctx, recs, FAKE_HAL_HISTORY_CAPACITY);
    size_t count = 0;
    for (size_t i = 0; i < n; i++)
        if (recs[i].type == type && recs[i].data.safe.output == output)
            count++;
    return count;
}

static bool history_contains_safe_write(safe_output_t output, bool enable)
{
    fake_output_record_t recs[FAKE_HAL_HISTORY_CAPACITY];
    size_t n = fake_hal_get_history(s_hal_ctx, recs, FAKE_HAL_HISTORY_CAPACITY);
    for (size_t i = 0; i < n; i++)
        if (recs[i].type == FAKE_OUTPUT_SET_SAFE &&
            recs[i].data.safe.output == output &&
            recs[i].data.safe.enable == enable)
            return true;
    return false;
}

/* Advance fake time past a duration and let service tasks tick. */
static void advance_and_tick(int64_t delta_ms, uint32_t real_wait_ms)
{
    fake_hal_advance_time(s_hal_ctx, delta_ms);
    if (real_wait_ms) vTaskDelay(pdMS_TO_TICKS(real_wait_ms));
}

TEST_CASE("ACT-01: source valve test writes only GPA0 and auto-closes ",
          "[phase9][actuator]")
{
    TEST_ASSERT_EQUAL(ESP_OK, actuator_svc_setup());
    fake_hal_clear_history(s_hal_ctx);

    TEST_ASSERT_EQUAL(ESP_OK, water_service_actuator_valve_test(
        WATER_ACTUATOR_VALVE_SOURCE, 41, 1500));
    vTaskDelay(pdMS_TO_TICKS(200)); /* water task processes open */

    TEST_ASSERT_TRUE(history_contains_safe_write(SAFE_OUTPUT_TAP_VALVE, true));
    TEST_ASSERT_FALSE(history_contains_safe_write(SAFE_OUTPUT_TRANSFER_VALVE, true));
    TEST_ASSERT_FALSE(history_contains_safe_write(SAFE_OUTPUT_UV, true));
    TEST_ASSERT_FALSE(history_contains_safe_write(SAFE_OUTPUT_DRAIN_VALVE, true));
    TEST_ASSERT_FALSE(history_contains_safe_write(SAFE_OUTPUT_PTC_HEATER, true));
    TEST_ASSERT_FALSE(history_contains_safe_write(SAFE_OUTPUT_DETERGENT_PUMP, true));

    /* 设备端计时到期自动 OFF。 */
    advance_and_tick(2000, 200);
    TEST_ASSERT_TRUE(history_contains_safe_write(SAFE_OUTPUT_TAP_VALVE, false));

    water_snapshot_t ws;
    TEST_ASSERT_EQUAL(ESP_OK, water_service_get_snapshot(&ws));
    TEST_ASSERT_FALSE(ws.source_valve_on);
    TEST_ASSERT_FALSE(ws.source_valve_unknown);

    actuator_svc_teardown();
}

TEST_CASE("ACT-02: transfer valve test writes only GPA1 and auto-closes ",
          "[phase9][actuator]")
{
    TEST_ASSERT_EQUAL(ESP_OK, actuator_svc_setup());
    fake_hal_clear_history(s_hal_ctx);

    TEST_ASSERT_EQUAL(ESP_OK, water_service_actuator_valve_test(
        WATER_ACTUATOR_VALVE_TRANSFER, 42, 1500));
    vTaskDelay(pdMS_TO_TICKS(200));

    TEST_ASSERT_TRUE(history_contains_safe_write(SAFE_OUTPUT_TRANSFER_VALVE, true));
    TEST_ASSERT_FALSE(history_contains_safe_write(SAFE_OUTPUT_TAP_VALVE, true));
    TEST_ASSERT_FALSE(history_contains_safe_write(SAFE_OUTPUT_UV, true));
    TEST_ASSERT_FALSE(history_contains_safe_write(SAFE_OUTPUT_DRAIN_VALVE, true));
    TEST_ASSERT_FALSE(history_contains_safe_write(SAFE_OUTPUT_PTC_HEATER, true));
    TEST_ASSERT_FALSE(history_contains_safe_write(SAFE_OUTPUT_DETERGENT_PUMP, true));

    advance_and_tick(2000, 200);
    TEST_ASSERT_TRUE(history_contains_safe_write(SAFE_OUTPUT_TRANSFER_VALVE, false));

    water_snapshot_t ws;
    TEST_ASSERT_EQUAL(ESP_OK, water_service_get_snapshot(&ws));
    TEST_ASSERT_FALSE(ws.transfer_valve_on);

    actuator_svc_teardown();
}

TEST_CASE("ACT-03: detergent pump test writes only GPB7 and auto-closes ",
          "[phase9][actuator]")
{
    TEST_ASSERT_EQUAL(ESP_OK, actuator_svc_setup());
    fake_hal_clear_history(s_hal_ctx);

    detergent_request_t req = { .request_id = 43, .duration_ms = 800 };
    TEST_ASSERT_EQUAL(ESP_OK, detergent_service_run_async(&req));
    vTaskDelay(pdMS_TO_TICKS(200));

    TEST_ASSERT_TRUE(history_contains_safe_write(SAFE_OUTPUT_DETERGENT_PUMP, true));
    TEST_ASSERT_FALSE(history_contains_safe_write(SAFE_OUTPUT_TAP_VALVE, true));
    TEST_ASSERT_FALSE(history_contains_safe_write(SAFE_OUTPUT_TRANSFER_VALVE, true));
    TEST_ASSERT_FALSE(history_contains_safe_write(SAFE_OUTPUT_UV, true));
    TEST_ASSERT_FALSE(history_contains_safe_write(SAFE_OUTPUT_DRAIN_VALVE, true));
    TEST_ASSERT_FALSE(history_contains_safe_write(SAFE_OUTPUT_PTC_HEATER, true));

    advance_and_tick(2000, 200);
    TEST_ASSERT_TRUE(history_contains_safe_write(SAFE_OUTPUT_DETERGENT_PUMP, false));

    detergent_snapshot_t ds;
    TEST_ASSERT_EQUAL(ESP_OK, detergent_service_get_snapshot(&ds));
    TEST_ASSERT_EQUAL(1, ds.output_state); /* KNOWN_OFF */

    actuator_svc_teardown();
}

TEST_CASE("ACT-04: invalid actuator args produce no hardware write ",
          "[phase9][actuator]")
{
    TEST_ASSERT_EQUAL(ESP_OK, actuator_svc_setup());
    fake_hal_clear_history(s_hal_ctx);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, water_service_actuator_valve_test(
        (water_actuator_valve_t)99, 44, 1500));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, water_service_actuator_valve_test(
        WATER_ACTUATOR_VALVE_SOURCE, 0, 1500));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, water_service_actuator_valve_test(
        WATER_ACTUATOR_VALVE_SOURCE, 45, 0));

    vTaskDelay(pdMS_TO_TICKS(100));
    size_t writes = count_safe_writes(FAKE_OUTPUT_SET_SAFE, SAFE_OUTPUT_TAP_VALVE) +
                    count_safe_writes(FAKE_OUTPUT_SET_SAFE, SAFE_OUTPUT_TRANSFER_VALVE) +
                    count_safe_writes(FAKE_OUTPUT_SET_SAFE, SAFE_OUTPUT_DETERGENT_PUMP);
    TEST_ASSERT_EQUAL(0, writes);

    actuator_svc_teardown();
}

TEST_CASE("ACT-05: water emergency close drives both valves OFF ",
          "[phase9][actuator]")
{
    TEST_ASSERT_EQUAL(ESP_OK, actuator_svc_setup());
    fake_hal_clear_history(s_hal_ctx);

    TEST_ASSERT_EQUAL(ESP_OK, water_service_actuator_valve_test(
        WATER_ACTUATOR_VALVE_SOURCE, 46, 1500));
    vTaskDelay(pdMS_TO_TICKS(200));
    TEST_ASSERT_TRUE(history_contains_safe_write(SAFE_OUTPUT_TAP_VALVE, true));

    TEST_ASSERT_EQUAL(ESP_OK, water_service_emergency_close());
    vTaskDelay(pdMS_TO_TICKS(200));
    TEST_ASSERT_TRUE(history_contains_safe_write(SAFE_OUTPUT_TAP_VALVE, false));
    TEST_ASSERT_TRUE(history_contains_safe_write(SAFE_OUTPUT_TRANSFER_VALVE, false));

    water_snapshot_t ws;
    TEST_ASSERT_EQUAL(ESP_OK, water_service_get_snapshot(&ws));
    TEST_ASSERT_FALSE(ws.source_valve_on);
    TEST_ASSERT_FALSE(ws.transfer_valve_on);

    actuator_svc_teardown();
}

TEST_CASE("ACT-06: detergent emergency stop drives pump OFF ",
          "[phase9][actuator]")
{
    TEST_ASSERT_EQUAL(ESP_OK, actuator_svc_setup());
    fake_hal_clear_history(s_hal_ctx);

    detergent_request_t req = { .request_id = 47, .duration_ms = 800 };
    TEST_ASSERT_EQUAL(ESP_OK, detergent_service_run_async(&req));
    vTaskDelay(pdMS_TO_TICKS(200));
    TEST_ASSERT_TRUE(history_contains_safe_write(SAFE_OUTPUT_DETERGENT_PUMP, true));

    TEST_ASSERT_EQUAL(ESP_OK, detergent_service_emergency_stop());
    vTaskDelay(pdMS_TO_TICKS(200));
    TEST_ASSERT_TRUE(history_contains_safe_write(SAFE_OUTPUT_DETERGENT_PUMP, false));

    detergent_snapshot_t ds;
    TEST_ASSERT_EQUAL(ESP_OK, detergent_service_get_snapshot(&ds));
    TEST_ASSERT_EQUAL(1, ds.output_state);

    actuator_svc_teardown();
}

TEST_CASE("ACT-07: all writes go through set_safe_output (no raw MCP bypass) ",
          "[phase9][actuator]")
{
    TEST_ASSERT_EQUAL(ESP_OK, actuator_svc_setup());
    fake_hal_clear_history(s_hal_ctx);

    TEST_ASSERT_EQUAL(ESP_OK, water_service_actuator_valve_test(
        WATER_ACTUATOR_VALVE_SOURCE, 48, 1500));
    detergent_request_t req = { .request_id = 49, .duration_ms = 800 };
    TEST_ASSERT_EQUAL(ESP_OK, detergent_service_run_async(&req));
    vTaskDelay(pdMS_TO_TICKS(200));

    fake_output_record_t recs[FAKE_HAL_HISTORY_CAPACITY];
    size_t n = fake_hal_get_history(s_hal_ctx, recs, FAKE_HAL_HISTORY_CAPACITY);
    for (size_t i = 0; i < n; i++)
        TEST_ASSERT_EQUAL(FAKE_OUTPUT_SET_SAFE, recs[i].type);

    actuator_svc_teardown();
}
