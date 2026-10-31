/*
 * phase11_round1_1_tests.c — Phase 11 Group A Round 1.1 actuator regression
 *
 * Proves at the actuator_self_test component level:
 *   - SOURCE / TRANSFER fixed 1500 ms ON→OFF (device-side timer, even if the
 *     host never sends another frame);
 *   - DETERGENT fixed 800 ms ON→OFF;
 *   - a second actuator request while one is running is rejected
 *     (SELF_TEST_BUSY) and never overlaps ON;
 *   - terminal is set exactly once (emergency after COMPLETE does not flip it);
 *   - emergency during a running detergent test requests the pump OFF.
 * FreeRTOS + Fake HAL, no raw MCP writes (all through service → safety_manager).
 */

#include <string.h>

#include "unity.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "fake_hal.h"
#include "safety_manager.h"
#include "water_service.h"
#include "detergent_service.h"
#include "machine_config.h"
#include "actuator_self_test.h"

#define R11A_TAG "phase11_r11a"

static fake_hal_ctx_t *s_hal_ctx = NULL;
static const xiaojing_hal_t *s_hal = NULL;
static bool s_up = false;

static esp_err_t r11a_event_sink(const machine_event_t *event,
                                 uint32_t timeout_ms, void *context)
{
    (void)event; (void)timeout_ms; (void)context;
    return ESP_OK;
}

static esp_err_t r11a_setup(void)
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
    machine_event_sink_t sink = { .publish = r11a_event_sink, .context = NULL };
    if (water_service_init(&ws_cfg, s_hal, sink) != ESP_OK ||
        water_service_start() != ESP_OK ||
        detergent_service_init(&machine_config_get()->detergent, s_hal, sink)
            != ESP_OK ||
        detergent_service_start() != ESP_OK) {
        water_service_stop(); detergent_service_stop();
        safety_manager_stop(); safety_manager_test_reset();
        fake_hal_destroy(s_hal_ctx); s_hal_ctx = NULL; s_hal = NULL;
        return ESP_ERR_INVALID_STATE;
    }
    s_up = true;
    return ESP_OK;
}

static void r11a_teardown(void)
{
    if (s_up) { water_service_stop(); detergent_service_stop(); }
    s_up = false;
    safety_manager_stop();
    if (s_hal_ctx) { fake_hal_destroy(s_hal_ctx); s_hal_ctx = NULL; s_hal = NULL; }
    safety_manager_test_reset();
}

static bool r11a_history_has(safe_output_t output, bool enable)
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

/* Advance fake time past a duration and let the actuator/service tasks tick. */
static void r11a_advance_and_tick(int64_t delta_ms, uint32_t real_wait_ms)
{
    fake_hal_advance_time(s_hal_ctx, delta_ms);
    if (real_wait_ms) vTaskDelay(pdMS_TO_TICKS(real_wait_ms));
}

static esp_err_t r11a_run_to_accept(actuator_self_test_target_t target,
                                    uint32_t expect_duration)
{
    const char *code = NULL;
    uint32_t dur = 0;
    esp_err_t e = actuator_self_test_request(target, &code, &dur);
    if (e != ESP_OK) return e;
    if (!code) return ESP_ERR_INVALID_RESPONSE;
    if (strcmp(code, "ACTUATOR_SELF_TEST_ACCEPTED") != 0)
        return ESP_ERR_INVALID_STATE;
    if (dur != expect_duration) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

TEST_CASE("R11A-01: SOURCE fixed 1500ms ON→OFF via actuator_self_test",
          "[phase11][actuator]")
{
    TEST_ASSERT_EQUAL(ESP_OK, r11a_setup());
    fake_hal_clear_history(s_hal_ctx);
    actuator_self_test_config_t cfg = { .executor = NULL };
    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_init(&cfg));
    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_start());

    TEST_ASSERT_EQUAL(ESP_OK, r11a_run_to_accept(ACT_TARGET_SOURCE_VALVE, 1500U));
    vTaskDelay(pdMS_TO_TICKS(250));
    TEST_ASSERT_TRUE(r11a_history_has(SAFE_OUTPUT_TAP_VALVE, true));
    TEST_ASSERT_FALSE(r11a_history_has(SAFE_OUTPUT_TRANSFER_VALVE, true));

    /* 设备端计时到期自动 OFF：1500ms 后（无任何额外请求）。 */
    r11a_advance_and_tick(1700, 300);
    TEST_ASSERT_TRUE(r11a_history_has(SAFE_OUTPUT_TAP_VALVE, false));

    actuator_self_test_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(ACT_TERMINAL_COMPLETE, snap.terminal);
    TEST_ASSERT_TRUE(snap.output_confirmed_off);

    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_stop());
    r11a_teardown();
}

TEST_CASE("R11A-02: TRANSFER fixed 1500ms ON→OFF via actuator_self_test",
          "[phase11][actuator]")
{
    TEST_ASSERT_EQUAL(ESP_OK, r11a_setup());
    fake_hal_clear_history(s_hal_ctx);
    actuator_self_test_config_t cfg = { .executor = NULL };
    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_init(&cfg));
    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_start());

    TEST_ASSERT_EQUAL(ESP_OK, r11a_run_to_accept(ACT_TARGET_TRANSFER_VALVE, 1500U));
    vTaskDelay(pdMS_TO_TICKS(250));
    TEST_ASSERT_TRUE(r11a_history_has(SAFE_OUTPUT_TRANSFER_VALVE, true));
    TEST_ASSERT_FALSE(r11a_history_has(SAFE_OUTPUT_TAP_VALVE, true));

    r11a_advance_and_tick(1700, 300);
    TEST_ASSERT_TRUE(r11a_history_has(SAFE_OUTPUT_TRANSFER_VALVE, false));

    actuator_self_test_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(ACT_TERMINAL_COMPLETE, snap.terminal);
    TEST_ASSERT_TRUE(snap.output_confirmed_off);

    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_stop());
    r11a_teardown();
}

TEST_CASE("R11A-03: DETERGENT fixed 800ms ON→OFF via actuator_self_test",
          "[phase11][actuator]")
{
    TEST_ASSERT_EQUAL(ESP_OK, r11a_setup());
    fake_hal_clear_history(s_hal_ctx);
    actuator_self_test_config_t cfg = { .executor = NULL };
    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_init(&cfg));
    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_start());

    TEST_ASSERT_EQUAL(ESP_OK, r11a_run_to_accept(ACT_TARGET_DETERGENT_PUMP, 800U));
    vTaskDelay(pdMS_TO_TICKS(250));
    TEST_ASSERT_TRUE(r11a_history_has(SAFE_OUTPUT_DETERGENT_PUMP, true));
    TEST_ASSERT_FALSE(r11a_history_has(SAFE_OUTPUT_TRANSFER_VALVE, true));

    r11a_advance_and_tick(1000, 300);
    TEST_ASSERT_TRUE(r11a_history_has(SAFE_OUTPUT_DETERGENT_PUMP, false));

    actuator_self_test_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(ACT_TERMINAL_COMPLETE, snap.terminal);
    TEST_ASSERT_TRUE(snap.output_confirmed_off);

    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_stop());
    r11a_teardown();
}

TEST_CASE("R11A-04: second actuator request rejected, no overlap ON",
          "[phase11][actuator]")
{
    TEST_ASSERT_EQUAL(ESP_OK, r11a_setup());
    fake_hal_clear_history(s_hal_ctx);
    actuator_self_test_config_t cfg = { .executor = NULL };
    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_init(&cfg));
    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_start());

    TEST_ASSERT_EQUAL(ESP_OK, r11a_run_to_accept(ACT_TARGET_SOURCE_VALVE, 1500U));
    vTaskDelay(pdMS_TO_TICKS(250));
    TEST_ASSERT_TRUE(r11a_history_has(SAFE_OUTPUT_TAP_VALVE, true));

    /* 第二个请求必须被单活跃门禁拒绝（不产生重叠 ON）。 */
    const char *code = NULL;
    uint32_t dur = 99;
    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_request(
        ACT_TARGET_TRANSFER_VALVE, &code, &dur));
    TEST_ASSERT_NOT_NULL(code);
    TEST_ASSERT_NOT_EQUAL(0, strcmp(code, "ACTUATOR_SELF_TEST_ACCEPTED"));
    TEST_ASSERT_EQUAL_STRING("SELF_TEST_BUSY", code);
    TEST_ASSERT_EQUAL(0U, dur);

    /* 转移阀从未 ON；进水阀保持 ON 直至设备端计时关闭。 */
    TEST_ASSERT_FALSE(r11a_history_has(SAFE_OUTPUT_TRANSFER_VALVE, true));
    TEST_ASSERT_TRUE(r11a_history_has(SAFE_OUTPUT_TAP_VALVE, true));

    r11a_advance_and_tick(1700, 300);
    TEST_ASSERT_TRUE(r11a_history_has(SAFE_OUTPUT_TAP_VALVE, false));
    TEST_ASSERT_FALSE(r11a_history_has(SAFE_OUTPUT_TRANSFER_VALVE, true));

    actuator_self_test_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(ACT_TERMINAL_COMPLETE, snap.terminal);

    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_stop());
    r11a_teardown();
}

TEST_CASE("R11A-05: terminal exactly once (emergency after COMPLETE does not flip)",
          "[phase11][actuator]")
{
    TEST_ASSERT_EQUAL(ESP_OK, r11a_setup());
    actuator_self_test_config_t cfg = { .executor = NULL };
    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_init(&cfg));
    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_start());

    TEST_ASSERT_EQUAL(ESP_OK, r11a_run_to_accept(ACT_TARGET_SOURCE_VALVE, 1500U));
    r11a_advance_and_tick(1700, 350);

    actuator_self_test_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(ACT_TERMINAL_COMPLETE, snap.terminal);

    /* terminal exactly once：已终态后 emergency 不得把 COMPLETE 翻成
     * INTERRUPTED/FAULT。 */
    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_emergency());
    vTaskDelay(pdMS_TO_TICKS(250));
    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_get_snapshot(&snap));
    TEST_ASSERT_EQUAL(ACT_TERMINAL_COMPLETE, snap.terminal);

    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_stop());
    r11a_teardown();
}

TEST_CASE("R11A-06: emergency during running detergent requests pump OFF",
          "[phase11][actuator]")
{
    TEST_ASSERT_EQUAL(ESP_OK, r11a_setup());
    fake_hal_clear_history(s_hal_ctx);
    actuator_self_test_config_t cfg = { .executor = NULL };
    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_init(&cfg));
    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_start());

    TEST_ASSERT_EQUAL(ESP_OK, r11a_run_to_accept(ACT_TARGET_DETERGENT_PUMP, 800U));
    vTaskDelay(pdMS_TO_TICKS(250));
    TEST_ASSERT_TRUE(r11a_history_has(SAFE_OUTPUT_DETERGENT_PUMP, true));

    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_emergency());
    vTaskDelay(pdMS_TO_TICKS(300));
    TEST_ASSERT_TRUE(r11a_history_has(SAFE_OUTPUT_DETERGENT_PUMP, false));
    TEST_ASSERT_FALSE(r11a_history_has(SAFE_OUTPUT_TRANSFER_VALVE, true));
    TEST_ASSERT_FALSE(r11a_history_has(SAFE_OUTPUT_TAP_VALVE, true));

    actuator_self_test_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_get_snapshot(&snap));
    TEST_ASSERT_TRUE(snap.terminal == ACT_TERMINAL_INTERRUPTED ||
                     snap.terminal == ACT_TERMINAL_FAULT ||
                     snap.terminal == ACT_TERMINAL_COMPLETE);

    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_stop());
    r11a_teardown();
}
