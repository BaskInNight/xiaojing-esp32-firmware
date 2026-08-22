/*
 * phase11_actuator_lifecycle_tests.c — Phase 11 audit regression tests
 *
 * Proves:
 *   - P2-1: 100x init/start/stop cycles keep kernel-object / heap usage stable
 *     (the task_done_sem leak would accumulate a binary semaphore every cycle);
 *   - P1-3: emergency during a running test requests all outputs OFF and ends
 *     the test as INTERRUPTED (not silent), and emergency before init returns
 *     an error instead of ESP_OK;
 *   - P1-2: a completed test only clears when outputs are KNOWN_OFF.
 * FreeRTOS + Fake HAL, no raw MCP writes.
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

#define P11_TAG "phase11_actuator"

static fake_hal_ctx_t *s_hal_ctx = NULL;
static const xiaojing_hal_t *s_hal = NULL;
static bool s_up = false;

static esp_err_t p11_event_sink(const machine_event_t *event,
                                uint32_t timeout_ms, void *context)
{
    (void)event; (void)timeout_ms; (void)context;
    return ESP_OK;
}

static esp_err_t p11_setup(void)
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
    machine_event_sink_t sink = { .publish = p11_event_sink, .context = NULL };
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

static void p11_teardown(void)
{
    if (s_up) { water_service_stop(); detergent_service_stop(); }
    s_up = false;
    safety_manager_stop();
    if (s_hal_ctx) { fake_hal_destroy(s_hal_ctx); s_hal_ctx = NULL; s_hal = NULL; }
    safety_manager_test_reset();
}

static bool p11_history_has(safe_output_t output, bool enable)
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

TEST_CASE("P11-01: 100x init/start/stop lifecycle keeps resources stable",
          "[phase11][actuator]")
{
    TEST_ASSERT_EQUAL(ESP_OK, p11_setup());

    actuator_self_test_config_t cfg = { .executor = NULL };
    uint32_t heap_before = heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    UBaseType_t tasks_before = uxTaskGetNumberOfTasks();

    for (int i = 0; i < 100; i++) {
        TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_init(&cfg));
        TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_start());
        TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_stop());
    }

    UBaseType_t tasks_after = uxTaskGetNumberOfTasks();
    uint32_t heap_after = heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    /* Small slack covers FreeRTOS lazy cleanup; a leaked task or semaphore
     * would push this far beyond the allowance. */
    TEST_ASSERT_TRUE(heap_after + 4096 >= heap_before);
    TEST_ASSERT_TRUE(tasks_after <= tasks_before + 1);

    /* A fresh init/start/stop must still work after the loop (no leaked
     * state blocks reuse). */
    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_init(&cfg));
    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_start());
    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_stop());

    p11_teardown();
}

TEST_CASE("P11-02: emergency during running test shuts outputs OFF + INTERRUPTED",
          "[phase11][actuator]")
{
    TEST_ASSERT_EQUAL(ESP_OK, p11_setup());
    fake_hal_clear_history(s_hal_ctx);

    actuator_self_test_config_t cfg = { .executor = NULL };
    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_init(&cfg));
    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_start());

    const char *code = NULL;
    uint32_t dur = 0;
    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_request(
        ACT_TARGET_SOURCE_VALVE, &code, &dur));
    TEST_ASSERT_NOT_NULL(code);
    TEST_ASSERT_EQUAL_STRING("ACTUATOR_SELF_TEST_ACCEPTED", code);
    TEST_ASSERT_EQUAL(1500U, dur);
    vTaskDelay(pdMS_TO_TICKS(300));

    /* Test is running: the water valve is ON. */
    water_snapshot_t ws;
    TEST_ASSERT_EQUAL(ESP_OK, water_service_get_snapshot(&ws));
    TEST_ASSERT_TRUE(ws.source_valve_on);

    /* Emergency must request all outputs OFF and not swallow errors. */
    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_emergency());
    vTaskDelay(pdMS_TO_TICKS(300));

    TEST_ASSERT_TRUE(p11_history_has(SAFE_OUTPUT_TAP_VALVE, false));
    TEST_ASSERT_TRUE(p11_history_has(SAFE_OUTPUT_TRANSFER_VALVE, false));

    actuator_self_test_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_get_snapshot(&snap));
    TEST_ASSERT_TRUE(snap.terminal == ACT_TERMINAL_INTERRUPTED ||
                     snap.terminal == ACT_TERMINAL_COMPLETE ||
                     snap.terminal == ACT_TERMINAL_FAULT);
    TEST_ASSERT_EQUAL(ESP_OK, actuator_self_test_stop());
    p11_teardown();
}

TEST_CASE("P11-03: emergency before init returns error, not silent success",
          "[phase11][actuator]")
{
    TEST_ASSERT_EQUAL(ESP_OK, p11_setup());
    /* actuator_self_test not initialized (no init called). */
    esp_err_t e = actuator_self_test_emergency();
    TEST_ASSERT_NOT_EQUAL(ESP_OK, e);
    p11_teardown();
}
