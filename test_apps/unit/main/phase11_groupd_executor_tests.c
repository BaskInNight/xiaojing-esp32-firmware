/*
 * phase11_groupd_executor_tests.c — Phase 11 Group D R0 executor wrapper tests
 *
 * Covers the internal-step (STEP_SETTLE / STEP_AUDIT_*) handling added to
 * wash_executor.c for DEFAULT_DEMO_V1:
 *   - SETTLE auto-completes after its duration, never reaching the adapter;
 *   - AUDIT passes → completes; AUDIT fails / no sink → program faults
 *     (fail-closed);
 *   - a full DEFAULT_DEMO_V1 program runs end-to-end through the executor
 *     (all 26 service phases + 2 system moves + 4 settles + 2 audits).
 *
 * Runs on ESP32-S3 with FreeRTOS (real time). FAKE adapter — no real outputs.
 */

#include "unity.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "wash_executor.h"
#include "wash_executor_core.h"
#include "wash_executor_adapter.h"
#include "wash_contract.h"
#include "machine_types.h"
#include "program_control_core.h"
#include <string.h>

static const char *TAG = "gd_exec";

static wash_executor_adapter_t g_adapter;
static uint8_t g_adapter_ctx[FAKE_ADAPTER_CTX_SIZE];
static wash_program_t g_prog;

static wash_executor_config_t g_cfg = {
    .step_timeout_default_ms = 20000,
    .reset_timeout_ms = 2000,
    .cancel_timeout_ms = 1000,
    .terminal_retry_ms = 50
};

/* ---- Terminal sink ---- */

typedef struct {
    SemaphoreHandle_t mutex;
    EventGroupHandle_t received;
    program_terminal_t last_terminal;
    uint32_t call_count;
} gd_sink_t;

static gd_sink_t g_sink;

static bool gd_terminal_sink(const program_terminal_t *terminal, void *ctx)
{
    gd_sink_t *s = (gd_sink_t *)ctx;
    xSemaphoreTake(s->mutex, portMAX_DELAY);
    s->last_terminal = *terminal;
    s->call_count++;
    xSemaphoreGive(s->mutex);
    xEventGroupSetBits(s->received, 1);
    return true;
}

static void gd_sink_init(gd_sink_t *s)
{
    if (s->mutex) vSemaphoreDelete(s->mutex);
    if (s->received) vEventGroupDelete(s->received);
    memset(s, 0, sizeof(*s));
    s->mutex = xSemaphoreCreateMutex();
    s->received = xEventGroupCreate();
}

/* ---- Audit sink ---- */

static bool g_audit_pass;

static bool gd_audit_fn(wash_step_type_t step_type, void *ctx)
{
    (void)step_type; (void)ctx;
    return g_audit_pass;
}

/* ---- Safe cleanup ---- */

static bool gd_cleanup(wash_executor_t *exec)
{
    if (!exec) return true;
    esp_err_t sr = wash_executor_stop(exec, 10000);
    if (sr != ESP_OK) { ESP_LOGE(TAG, "stop failed 0x%x", sr); return false; }
    esp_err_t dr = wash_executor_destroy(exec);
    if (dr != ESP_OK) { ESP_LOGE(TAG, "destroy failed 0x%x", dr); return false; }
    return true;
}

/* ---- Internal-step tests ---- */

TEST_CASE("groupD: SETTLE auto-completes internally (no adapter dispatch)",
          "[executor][groupd]")
{
    fake_adapter_init(&g_adapter, g_adapter_ctx);
    wash_executor_t *exec = wash_executor_create(&g_cfg, &g_adapter);
    TEST_ASSERT_NOT_NULL(exec);
    gd_sink_init(&g_sink);
    wash_executor_set_terminal_sink(exec, gd_terminal_sink, &g_sink);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_start(exec));

    memset(&g_prog, 0, sizeof(g_prog));
    g_prog.program_id = 1;
    g_prog.kind = WASH_PROGRAM_CUSTOM;
    g_prog.step_count = 2;
    g_prog.steps[0].type = STEP_SETTLE;
    g_prog.steps[0].required_position = DRUM_POS_0;
    g_prog.steps[0].duration_ms = 300;
    g_prog.steps[0].timeout_ms = 0;
    g_prog.steps[0].step_id = 0;
    g_prog.steps[1].type = STEP_FINISH;
    g_prog.steps[1].required_position = DRUM_POS_0;
    g_prog.steps[1].step_id = 1;

    TEST_ASSERT_EQUAL(ESP_OK,
        wash_executor_submit_program(exec, &g_prog, 1));

    EventBits_t bits = xEventGroupWaitBits(
        g_sink.received, 1, pdFALSE, pdFALSE, pdMS_TO_TICKS(3000));
    TEST_ASSERT_TRUE((bits & 1) != 0);
    xSemaphoreTake(g_sink.mutex, portMAX_DELAY);
    program_terminal_t t = g_sink.last_terminal;
    xSemaphoreGive(g_sink.mutex);
    TEST_ASSERT_EQUAL(TERM_RESULT_COMPLETE, (unsigned)t.result);
    TEST_ASSERT_EQUAL(1u, t.program_id);

    /* SETTLE must never reach the adapter. */
    TEST_ASSERT_EQUAL(0u, fake_adapter_dispatch_count(g_adapter_ctx));

    TEST_ASSERT_TRUE(gd_cleanup(exec));
}

TEST_CASE("groupD: AUDIT passes -> completes; fails -> fault (fail-closed)",
          "[executor][groupd]")
{
    /* ---- pass ---- */
    fake_adapter_init(&g_adapter, g_adapter_ctx);
    wash_executor_t *exec = wash_executor_create(&g_cfg, &g_adapter);
    TEST_ASSERT_NOT_NULL(exec);
    gd_sink_init(&g_sink);
    wash_executor_set_terminal_sink(exec, gd_terminal_sink, &g_sink);
    g_audit_pass = true;
    wash_executor_set_audit_sink(exec, gd_audit_fn, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_start(exec));

    memset(&g_prog, 0, sizeof(g_prog));
    g_prog.program_id = 2;
    g_prog.kind = WASH_PROGRAM_CUSTOM;
    g_prog.step_count = 2;
    g_prog.steps[0].type = STEP_AUDIT_UV_OFF;
    g_prog.steps[0].required_position = DRUM_POS_45;
    g_prog.steps[0].timeout_ms = 5000;
    g_prog.steps[0].step_id = 0;
    g_prog.steps[1].type = STEP_FINISH;
    g_prog.steps[1].required_position = DRUM_POS_45;
    g_prog.steps[1].step_id = 1;

    TEST_ASSERT_EQUAL(ESP_OK,
        wash_executor_submit_program(exec, &g_prog, 2));
    EventBits_t bits = xEventGroupWaitBits(
        g_sink.received, 1, pdFALSE, pdFALSE, pdMS_TO_TICKS(3000));
    TEST_ASSERT_TRUE((bits & 1) != 0);
    xSemaphoreTake(g_sink.mutex, portMAX_DELAY);
    program_terminal_t t = g_sink.last_terminal;
    xSemaphoreGive(g_sink.mutex);
    TEST_ASSERT_EQUAL(TERM_RESULT_COMPLETE, (unsigned)t.result);
    TEST_ASSERT_EQUAL(0u, fake_adapter_dispatch_count(g_adapter_ctx));
    TEST_ASSERT_TRUE(gd_cleanup(exec));

    /* ---- fail (audit returns false) → FAULT ---- */
    fake_adapter_init(&g_adapter, g_adapter_ctx);
    exec = wash_executor_create(&g_cfg, &g_adapter);
    TEST_ASSERT_NOT_NULL(exec);
    gd_sink_init(&g_sink);
    wash_executor_set_terminal_sink(exec, gd_terminal_sink, &g_sink);
    g_audit_pass = false;
    wash_executor_set_audit_sink(exec, gd_audit_fn, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_start(exec));

    memset(&g_prog, 0, sizeof(g_prog));
    g_prog.program_id = 3;
    g_prog.kind = WASH_PROGRAM_CUSTOM;
    g_prog.step_count = 1;
    g_prog.steps[0].type = STEP_AUDIT_SAFE_OFF;
    g_prog.steps[0].required_position = DRUM_POS_45;
    g_prog.steps[0].timeout_ms = 5000;
    g_prog.steps[0].step_id = 0;

    TEST_ASSERT_EQUAL(ESP_OK,
        wash_executor_submit_program(exec, &g_prog, 3));
    bits = xEventGroupWaitBits(
        g_sink.received, 1, pdFALSE, pdFALSE, pdMS_TO_TICKS(3000));
    TEST_ASSERT_TRUE((bits & 1) != 0);
    xSemaphoreTake(g_sink.mutex, portMAX_DELAY);
    t = g_sink.last_terminal;
    xSemaphoreGive(g_sink.mutex);
    TEST_ASSERT_EQUAL(TERM_RESULT_FAULT, (unsigned)t.result);
    TEST_ASSERT_TRUE(gd_cleanup(exec));

    /* ---- no sink registered → fail-closed FAULT ---- */
    fake_adapter_init(&g_adapter, g_adapter_ctx);
    exec = wash_executor_create(&g_cfg, &g_adapter);
    TEST_ASSERT_NOT_NULL(exec);
    gd_sink_init(&g_sink);
    wash_executor_set_terminal_sink(exec, gd_terminal_sink, &g_sink);
    /* no audit sink set */
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_start(exec));

    memset(&g_prog, 0, sizeof(g_prog));
    g_prog.program_id = 4;
    g_prog.kind = WASH_PROGRAM_CUSTOM;
    g_prog.step_count = 1;
    g_prog.steps[0].type = STEP_AUDIT_SAFE_OFF;
    g_prog.steps[0].required_position = DRUM_POS_45;
    g_prog.steps[0].timeout_ms = 5000;
    g_prog.steps[0].step_id = 0;

    TEST_ASSERT_EQUAL(ESP_OK,
        wash_executor_submit_program(exec, &g_prog, 4));
    bits = xEventGroupWaitBits(
        g_sink.received, 1, pdFALSE, pdFALSE, pdMS_TO_TICKS(3000));
    TEST_ASSERT_TRUE((bits & 1) != 0);
    xSemaphoreTake(g_sink.mutex, portMAX_DELAY);
    t = g_sink.last_terminal;
    xSemaphoreGive(g_sink.mutex);
    TEST_ASSERT_EQUAL(TERM_RESULT_FAULT, (unsigned)t.result);
    TEST_ASSERT_TRUE(gd_cleanup(exec));
}

/* ---- DEFAULT_DEMO_V1 end-to-end through the executor ---- */

TEST_CASE("groupD: DEFAULT_DEMO_V1 runs end-to-end (26 phases + moves + settle/audit)",
          "[executor][groupd]")
{
    fake_adapter_init(&g_adapter, g_adapter_ctx);
    wash_executor_t *exec = wash_executor_create(&g_cfg, &g_adapter);
    TEST_ASSERT_NOT_NULL(exec);
    gd_sink_init(&g_sink);
    wash_executor_set_terminal_sink(exec, gd_terminal_sink, &g_sink);
    g_audit_pass = true;
    wash_executor_set_audit_sink(exec, gd_audit_fn, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_start(exec));

    planner_report_t rep = default_demo_build_program(
        NULL, 5, DEFAULT_DEMO_START_POSITION, &g_prog);
    TEST_ASSERT_EQUAL(PLAN_RESULT_OK, rep.result);
    TEST_ASSERT_EQUAL(28u, (unsigned)g_prog.step_count);
    TEST_ASSERT_TRUE(default_demo_validate_position_trace(
        &g_prog, DEFAULT_DEMO_START_POSITION));

    TEST_ASSERT_EQUAL(ESP_OK,
        wash_executor_submit_program(exec, &g_prog, 5));

    /* Drive: confirm load/unload, publish service terminals, let internal
     * steps (SETTLE/AUDIT) auto-complete via deadline. */
    uint32_t dispatched = 0;
    bool confirmed_load = false, confirmed_unload = false;
    uint64_t deadline = (uint64_t)esp_timer_get_time() + 20ULL * 1000000ULL;
    while ((uint64_t)esp_timer_get_time() < deadline) {
        EventBits_t bits = xEventGroupGetBits(g_sink.received);
        if (bits & 1) break;

        /* publish any newly-dispatched service step terminals */
        uint32_t dcount = fake_adapter_dispatch_count(g_adapter_ctx);
        while (dispatched < dcount) {
            const fake_dispatch_record_t *rec =
                fake_adapter_get_dispatch(g_adapter_ctx, dispatched);
            if (rec) {
                wash_executor_publish_terminal(
                    exec, &rec->token, sizeof(rec->token), true, 0);
            }
            dispatched++;
        }

        /* load / unload confirmation */
        wash_exec_snapshot_t snap;
        memset(&snap, 0, sizeof(snap));
        if (wash_executor_get_snapshot(exec, &snap) == ESP_OK) {
            if (!confirmed_load && snap.state == WASH_EXEC_STATE_WAIT_LOAD) {
                TEST_ASSERT_EQUAL(ESP_OK, wash_executor_confirm_load(exec));
                confirmed_load = true;
            }
            if (!confirmed_unload &&
                snap.state == WASH_EXEC_STATE_WAIT_UNLOAD) {
                TEST_ASSERT_EQUAL(ESP_OK, wash_executor_confirm_unload(exec));
                confirmed_unload = true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    EventBits_t bits = xEventGroupGetBits(g_sink.received);
    TEST_ASSERT_TRUE((bits & 1) != 0);
    xSemaphoreTake(g_sink.mutex, portMAX_DELAY);
    program_terminal_t t = g_sink.last_terminal;
    xSemaphoreGive(g_sink.mutex);
    TEST_ASSERT_EQUAL(TERM_RESULT_COMPLETE, (unsigned)t.result);
    TEST_ASSERT_EQUAL(5u, t.program_id);
    TEST_ASSERT_TRUE(confirmed_load);
    TEST_ASSERT_TRUE(confirmed_unload);

    /* 28 steps - 6 internal (4 SETTLE + 2 AUDIT) = 22 service dispatches */
    TEST_ASSERT_EQUAL(22u, fake_adapter_dispatch_count(g_adapter_ctx));

    TEST_ASSERT_TRUE(gd_cleanup(exec));
}
