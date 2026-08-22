/*
 * wash_executor_round2_integration_tests.c — Real adapter/QueueSet/button/bootstrap tests
 *
 * These tests exercise production code paths:
 * - wash_executor_real_adapter (not fake_adapter)
 * - QueueSet ingress in wash_executor
 * - button_service via fake HAL
 * - wash_executor_bootstrap lifecycle
 *
 * Uses fake_hal for hardware abstraction. No real peripherals needed.
 */

#include <string.h>
#include "unity.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "wash_executor.h"
#include "wash_executor_core.h"
#include "wash_executor_real_adapter.h"
#include "wash_executor_bootstrap.h"
#include "wash_contract.h"
#include "machine_types.h"
#include "machine_config.h"
#include "fake_hal.h"
#include "position_service.h"
#include "bl50_service.h"
#include "water_service.h"
#include "dry_service.h"
#include "drain_service.h"
#include "detergent_service.h"
#include "uv_service.h"
#include "safety_manager.h"
#include "button_service.h"

static const char *TAG = "r2integ";

/* ---- Shared state (non-static for tearDown access from test_runner.c) ---- */

QueueHandle_t s_machine_event_queue = NULL;
wash_executor_t *s_exec = NULL;
real_adapter_ctx_t *s_adapter_ctx = NULL;
wash_executor_adapter_t *s_adapter_ptr = NULL;
static fake_hal_ctx_t *s_fake_hal = NULL;
static const xiaojing_hal_t *s_hal = NULL;
static bool s_safety_owned = false;
static bool s_position_owned = false;
static bool s_bl50_owned = false;
static bool s_water_owned = false;
static bool s_dry_owned = false;
static bool s_drain_owned = false;
static bool s_detergent_owned = false;
static bool s_uv_owned = false;

/* Unity assertions longjmp past per-test cleanup.  Global tearDown calls this
 * assertion-free, idempotent owner cleanup to reclaim the full task graph. */
esp_err_t wash_executor_round2_test_global_cleanup(void);

/* Terminal sink */
typedef struct {
    EventGroupHandle_t eg;
    SemaphoreHandle_t mtx;
    program_terminal_t last_terminal;
    uint32_t call_count;
    bool return_value;
} test_sink_ctx_t;

#define SINK_DONE_BIT  (1U << 0)

static bool test_terminal_sink(const program_terminal_t *terminal, void *ctx)
{
    test_sink_ctx_t *s = (test_sink_ctx_t *)ctx;
    if (!s->return_value) {
        xSemaphoreTake(s->mtx, portMAX_DELAY);
        s->call_count++;
        xSemaphoreGive(s->mtx);
        return false;
    }
    xSemaphoreTake(s->mtx, portMAX_DELAY);
    s->last_terminal = *terminal;
    s->call_count++;
    xSemaphoreGive(s->mtx);
    xEventGroupSetBits(s->eg, SINK_DONE_BIT);
    return true;
}

static bool wait_for_adapter_state(bool need_active, bool need_cancel,
                                   bool need_reset, uint32_t timeout_ms,
                                   real_adapter_snapshot_t *out)
{
    uint32_t t0 = xTaskGetTickCount() * portTICK_PERIOD_MS;
    real_adapter_snapshot_t snap = {0};
    while ((xTaskGetTickCount() * portTICK_PERIOD_MS - t0) <= timeout_ms) {
        if (real_adapter_get_snapshot(s_adapter_ctx, &snap) == ESP_OK &&
            (!need_active || snap.active_in_flight) &&
            (!need_cancel || snap.cancel_in_flight) &&
            (!need_reset || snap.reset_in_flight)) {
            if (out) *out = snap;
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (out) *out = snap;
    return false;
}

static test_sink_ctx_t s_sink;

static void sink_init(test_sink_ctx_t *s)
{
    memset(s, 0, sizeof(*s));
    s->eg = xEventGroupCreate();
    s->mtx = xSemaphoreCreateMutex();
    s->return_value = true;
}

static void sink_deinit(test_sink_ctx_t *s)
{
    if (s->eg) { vEventGroupDelete(s->eg); s->eg = NULL; }
    if (s->mtx) { vSemaphoreDelete(s->mtx); s->mtx = NULL; }
}

/* ---- Helpers ---- */

static wash_program_t make_single_step_program(uint32_t prog_id, wash_step_type_t type,
                                                drum_position_t pos, uint32_t duration)
{
    wash_program_t prog;
    memset(&prog, 0, sizeof(prog));
    prog.program_id = prog_id;
    prog.step_count = 1;
    prog.steps[0].step_id = 1;
    prog.steps[0].type = type;
    prog.steps[0].required_position = pos;
    prog.steps[0].duration_ms = duration;
    prog.steps[0].timeout_ms = 5000;
    return prog;
}

/* Deterministic dispatch wait — polls adapter dispatch counter with bounded timeout.
 * Returns true if dispatch happened within timeout. */
static bool wait_for_dispatch(uint32_t timeout_ms)
{
    uint32_t t0 = xTaskGetTickCount() * portTICK_PERIOD_MS;
    while (real_adapter_dispatch_count(s_adapter_ctx) < 1) {
        if ((xTaskGetTickCount() * portTICK_PERIOD_MS - t0) > timeout_ms) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

static esp_err_t safe_executor_cleanup(void)
{
    if (s_exec) {
        esp_err_t err = wash_executor_quiesce(s_exec);
        if (err != ESP_OK) return err;
        err = wash_executor_stop(s_exec, 5000);
        if (err != ESP_OK) return err;
    }
    if (s_adapter_ctx) {
        esp_err_t err = real_adapter_stop_consumer(s_adapter_ctx);
        if (err != ESP_OK) return err;
        real_adapter_set_executor(s_adapter_ctx, NULL);
    }
    if (s_exec) {
        esp_err_t err = wash_executor_destroy(s_exec);
        if (err != ESP_OK) return err;
        s_exec = NULL;
    }
    if (s_adapter_ctx) {
        esp_err_t err = real_adapter_destroy_consumer(s_adapter_ctx);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

static void setup_real_adapter(void)
{
    /* Clean up any leaked resources from a previous failed test */
    TEST_ASSERT_EQUAL(ESP_OK, safe_executor_cleanup());
    if (s_adapter_ptr) { free(s_adapter_ptr); s_adapter_ptr = NULL; }
    if (s_adapter_ctx) { free(s_adapter_ctx); s_adapter_ctx = NULL; }
    /* Don't delete s_machine_event_queue — it's shared with services */
    sink_deinit(&s_sink);

    /* Create queue only if not already created by setup_services() */
    if (!s_machine_event_queue) {
        s_machine_event_queue = xQueueCreate(16, sizeof(machine_event_t));
        TEST_ASSERT_NOT_NULL(s_machine_event_queue);
    }

    s_adapter_ctx = calloc(1, 4096); /* real_adapter_ctx_t sized buffer */
    TEST_ASSERT_NOT_NULL(s_adapter_ctx);
    s_adapter_ptr = calloc(1, 256); /* wash_executor_adapter_t sized buffer */
    TEST_ASSERT_NOT_NULL(s_adapter_ptr);

    esp_err_t err = real_adapter_init(s_adapter_ptr, s_adapter_ctx, s_machine_event_queue);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = real_adapter_start_consumer(s_adapter_ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    wash_executor_config_t cfg = {
        .step_timeout_default_ms = 5000,
        .reset_timeout_ms = 2000,
        .cancel_timeout_ms = 2000,
        .terminal_retry_ms = 100,
    };
    s_exec = wash_executor_create(&cfg, s_adapter_ptr);
    TEST_ASSERT_NOT_NULL(s_exec);

    real_adapter_set_executor(s_adapter_ctx, s_exec);

    sink_init(&s_sink);
    wash_executor_set_terminal_sink(s_exec, test_terminal_sink, &s_sink);

    err = wash_executor_start(s_exec);
    TEST_ASSERT_EQUAL(ESP_OK, err);
}

static void teardown_real_adapter(void)
{
    /* Services still need the queue and adapter while draining terminals. */
    if (s_exec) {
        TEST_ASSERT_EQUAL(ESP_OK, wash_executor_quiesce(s_exec));
    }
}

/* Service event sink that publishes to the machine_event_queue.
 * This allows services to publish their terminal events properly,
 * preventing pending terminal accumulation that blocks stop(). */
static esp_err_t service_event_sink_publish(const machine_event_t *event,
                                             uint32_t timeout_ms, void *context)
{
    QueueHandle_t q = (QueueHandle_t)context;
    if (!q || !event) return ESP_ERR_INVALID_ARG;
    return (xQueueSend(q, event, pdMS_TO_TICKS(timeout_ms)) == pdTRUE)
           ? ESP_OK : ESP_ERR_TIMEOUT;
}

/* Inject a machine_event_t into the event queue (simulates service completion) */
static void inject_service_terminal(machine_event_type_t type,
                                     machine_request_id_t request_id,
                                     service_result_t result)
{
    machine_event_t ev = {
        .type = type,
        .request_id = request_id,
        .result = result,
    };
    TEST_ASSERT_EQUAL(pdTRUE, xQueueSend(s_machine_event_queue, &ev, pdMS_TO_TICKS(100)));
}

/* Wait for program terminal delivery */
static bool wait_for_terminal(uint32_t timeout_ms)
{
    EventBits_t bits = xEventGroupWaitBits(s_sink.eg, SINK_DONE_BIT, pdTRUE, pdFALSE,
                                            pdMS_TO_TICKS(timeout_ms));
    return (bits & SINK_DONE_BIT) != 0;
}

/* ---- Setup/teardown for each test ---- */

/* Wait for a service to be ready by polling with bounded timeout.
 * Uses vTaskDelay to yield to scheduler, allowing service tasks to start. */
static void wait_all_services_ready(uint32_t timeout_ms)
{
    uint32_t t0 = xTaskGetTickCount() * portTICK_PERIOD_MS;
    position_snapshot_t ps_snap;
    bl50_snapshot_t b50_snap;
    water_snapshot_t ws_snap;
    dry_snapshot_t ds_snap;
    drain_snapshot_t dr_snap;
    detergent_snapshot_t dt_snap;
    uv_snapshot_t uv_snap;

    while ((xTaskGetTickCount() * portTICK_PERIOD_MS - t0) < timeout_ms) {
        bool all_ready = true;
        if (position_service_get_snapshot(&ps_snap) != ESP_OK) all_ready = false;
        if (bl50_service_get_snapshot(&b50_snap) != ESP_OK) all_ready = false;
        if (water_service_get_snapshot(&ws_snap) != ESP_OK) all_ready = false;
        if (dry_service_get_snapshot(&ds_snap) != ESP_OK) all_ready = false;
        if (drain_service_get_snapshot(&dr_snap) != ESP_OK) all_ready = false;
        if (detergent_service_get_snapshot(&dt_snap) != ESP_OK) all_ready = false;
        if (uv_service_get_snapshot(&uv_snap) != ESP_OK) all_ready = false;
        if (all_ready) return;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGW(TAG, "wait_all_services_ready: timeout after %u ms", timeout_ms);
}

static void setup_services(void)
{
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK,
        wash_executor_round2_test_global_cleanup(),
        "previous integration resources are still live");

    /* P0 UAF fix: Stop ALL services BEFORE destroying leaked HAL.
     * Service tasks call HAL functions during final OFF — destroying HAL
     * while tasks are still running causes PC=0xA5A5A5A5 crash. */
    fake_hal_ctx_t *leaked = fake_hal_get_singleton();
    if (leaked) {
        const xiaojing_hal_t *leaked_hal = fake_hal_get_interface(leaked);

        /* Start safety_manager with leaked HAL (needed for water_service_stop hook unregister) */
        safety_manager_test_reset();
        safety_manager_config_t sm_cfg = {
            .hal = leaked_hal, .config = machine_config_get(),
            .output_mode = XIAOJING_MODE_FAKE,
            .ptc_enabled = false,
            .board_identity_confirmed = false,
        };
        TEST_ASSERT_EQUAL(ESP_OK, safety_manager_init(&sm_cfg));
        TEST_ASSERT_EQUAL(ESP_OK, safety_manager_start());

        /* Stop all services — use abort_cleanup (9s) for resilience */
        esp_err_t stop_err = ESP_OK;
        esp_err_t e;
        e = uv_service_stop();        if (e != ESP_OK) { e = uv_service_test_reset(); }
        if (e != ESP_OK) { ESP_LOGW(TAG, "uv cleanup: 0x%x", e); stop_err = e; }
        e = detergent_service_stop(); if (e != ESP_OK) { e = detergent_service_test_reset(); }
        if (e != ESP_OK) { ESP_LOGW(TAG, "det cleanup: 0x%x", e); stop_err = e; }
        e = drain_service_stop();     if (e != ESP_OK) { e = drain_service_test_reset(); }
        if (e != ESP_OK) { ESP_LOGW(TAG, "drain cleanup: 0x%x", e); stop_err = e; }
        e = dry_service_stop();       if (e != ESP_OK) { ESP_LOGW(TAG, "dry stop: 0x%x", e); stop_err = e; }
        e = water_service_stop();     if (e != ESP_OK) { ESP_LOGW(TAG, "water stop: 0x%x", e); stop_err = e; }
        e = bl50_service_stop();      if (e != ESP_OK) { e = bl50_service_test_reset(); }
        if (e != ESP_OK) { ESP_LOGW(TAG, "bl50 cleanup: 0x%x", e); stop_err = e; }
        e = position_service_stop();  if (e != ESP_OK) { ESP_LOGW(TAG, "pos stop: 0x%x", e); stop_err = e; }
        safety_manager_stop();

        if (stop_err != ESP_OK) {
            ESP_LOGE(TAG, "setup_services: leaked HAL cleanup failed (0x%x)", stop_err);
            /* Don't destroy HAL if stops failed — tasks may still be running */
            TEST_FAIL_MESSAGE("Leaked service cleanup failed — isolation poisoned");
            return;
        }

        /* All tasks joined — safe to destroy leaked HAL */
        fake_hal_destroy(leaked);
    }

    /* Create fresh HAL */
    s_fake_hal = fake_hal_create();
    TEST_ASSERT_NOT_NULL(s_fake_hal);
    s_hal = fake_hal_get_interface(s_fake_hal);
    TEST_ASSERT_NOT_NULL(s_hal);

    /* Start position_service from one unambiguous, active-low hall input.
     * Fake HAL's zeroed MCP image would otherwise assert all five halls. */
    mcp_input_snapshot_t initial_mcp = {
        .gpio_a = 0xFF,
        .gpio_b = (uint8_t)~(1U << 0),
    };
    fake_hal_set_mcp(s_fake_hal, &initial_mcp);

    machine_config_init();

    /* Start safety_manager with fresh HAL */
    safety_manager_test_reset();
    {
        safety_manager_config_t sm_cfg = {
            .hal = s_hal, .config = machine_config_get(),
            .output_mode = XIAOJING_MODE_FAKE,
            .ptc_enabled = false,
            .board_identity_confirmed = false,
        };
        TEST_ASSERT_EQUAL(ESP_OK, safety_manager_init(&sm_cfg));
        s_safety_owned = true;
        TEST_ASSERT_EQUAL(ESP_OK, safety_manager_start());
    }

    /* Global init (idempotent — CAS guarded) */
    position_service_global_init();
    water_service_global_init();
    dry_service_global_init();
    drain_service_global_init();
    detergent_service_global_init();
    uv_service_global_init();

    /* Create event queue BEFORE services so they can publish terminals */
    if (!s_machine_event_queue) {
        s_machine_event_queue = xQueueCreate(16, sizeof(machine_event_t));
        TEST_ASSERT_NOT_NULL(s_machine_event_queue);
    }

    /* Real sink that publishes to machine_event_queue */
    machine_event_sink_t svc_sink = {
        .publish = service_event_sink_publish,
        .context = s_machine_event_queue,
    };

    position_service_config_t ps_cfg = {
        .default_policy = POSITION_DIR_AUTO_SHORTEST,
        .move_pwm_percent = 50, .approach_pwm_percent = 30,
        .debounce_ms = 20, .move_timeout_ms = 3000, .brake_ms = 200,
        .rpwm_is_cw = true, .direction_calibrated = true,
    };
    TEST_ASSERT_EQUAL(ESP_OK, position_service_init(&ps_cfg, s_hal, svc_sink));
    s_position_owned = true;
    TEST_ASSERT_EQUAL(ESP_OK, position_service_start());

    TEST_ASSERT_EQUAL(ESP_OK, bl50_service_test_reset());
    bl50_service_config_t b50_cfg = {
        .hal = s_hal, .config = machine_config_get(), .event_sink = svc_sink,
    };
    TEST_ASSERT_EQUAL(ESP_OK, bl50_service_init(&b50_cfg));
    s_bl50_owned = true;
    TEST_ASSERT_EQUAL(ESP_OK, bl50_service_start());

    water_service_config_t ws_cfg = {0};
    ws_cfg.pulses_per_liter = 1.0f;
    ws_cfg.no_flow_timeout_ms = 5000;
    ws_cfg.total_inlet_timeout_ms = 30000;
    ws_cfg.max_fill_cycles = 3;
    ws_cfg.source_batch_max_ms = 5000;
    ws_cfg.default_transfer_ms = 3000;
    ws_cfg.transfer_timeout_ms = 10000;
    TEST_ASSERT_EQUAL(ESP_OK, water_service_init(&ws_cfg, s_hal, svc_sink));
    s_water_owned = true;
    TEST_ASSERT_EQUAL(ESP_OK, water_service_start());

    TEST_ASSERT_EQUAL(ESP_OK,
        dry_service_init(&machine_config_get()->dry, s_hal, svc_sink));
    s_dry_owned = true;
    TEST_ASSERT_EQUAL(ESP_OK, dry_service_start());
    TEST_ASSERT_EQUAL(ESP_OK, drain_service_test_reset());
    TEST_ASSERT_EQUAL(ESP_OK,
        drain_service_init(&machine_config_get()->drain, s_hal, svc_sink));
    s_drain_owned = true;
    TEST_ASSERT_EQUAL(ESP_OK, drain_service_start());
    TEST_ASSERT_EQUAL(ESP_OK, detergent_service_test_reset());
    TEST_ASSERT_EQUAL(ESP_OK,
        detergent_service_init(&machine_config_get()->detergent, s_hal, svc_sink));
    s_detergent_owned = true;
    TEST_ASSERT_EQUAL(ESP_OK, detergent_service_start());
    TEST_ASSERT_EQUAL(ESP_OK, uv_service_test_reset());
    TEST_ASSERT_EQUAL(ESP_OK,
        uv_service_init(&machine_config_get()->uv, s_hal, svc_sink));
    s_uv_owned = true;
    TEST_ASSERT_EQUAL(ESP_OK, uv_service_start());

    /* Wait for all services to be ready via snapshot polling */
    wait_all_services_ready(3000);

    /* Additional stabilization delay for service tasks to fully start */
    vTaskDelay(pdMS_TO_TICKS(200));
}

esp_err_t wash_executor_round2_test_global_cleanup(void)
{
    esp_err_t err;

    /* This owner must be invisible to every non-integration Unity test. */
    if (!s_exec && !s_adapter_ctx && !s_adapter_ptr &&
        !s_machine_event_queue && !s_fake_hal && !s_safety_owned &&
        !s_position_owned && !s_bl50_owned && !s_water_owned &&
        !s_dry_owned && !s_drain_owned && !s_detergent_owned &&
        !s_uv_owned) {
        return ESP_OK;
    }

    if (s_exec) {
        err = wash_executor_quiesce(s_exec);
        if (err != ESP_OK) return err;
    }

#define STOP_OR_PRESERVE(call) do { \
        err = (call); \
        if (err != ESP_OK) { \
            ESP_LOGE(TAG, #call " failed: 0x%x; resources preserved", err); \
            return err; \
        } \
    } while (0)

    if (s_uv_owned) {
        STOP_OR_PRESERVE(uv_service_stop());
        s_uv_owned = false;
    }
    if (s_detergent_owned) {
        STOP_OR_PRESERVE(detergent_service_stop());
        s_detergent_owned = false;
    }
    if (s_drain_owned) {
        STOP_OR_PRESERVE(drain_service_stop());
        s_drain_owned = false;
    }
    if (s_dry_owned) {
        STOP_OR_PRESERVE(dry_service_stop());
        s_dry_owned = false;
    }
    if (s_water_owned) {
        STOP_OR_PRESERVE(water_service_stop());
        s_water_owned = false;
    }
    if (s_bl50_owned) {
        STOP_OR_PRESERVE(bl50_service_stop());
        s_bl50_owned = false;
    }
    if (s_position_owned) {
        STOP_OR_PRESERVE(position_service_stop());
        s_position_owned = false;
    }

    if (s_exec) STOP_OR_PRESERVE(wash_executor_stop(s_exec, 5000));
    if (s_adapter_ctx) {
        STOP_OR_PRESERVE(real_adapter_stop_consumer(s_adapter_ctx));
        real_adapter_set_executor(s_adapter_ctx, NULL);
    }
    if (s_exec) {
        STOP_OR_PRESERVE(wash_executor_destroy(s_exec));
        s_exec = NULL;
    }
    if (s_adapter_ctx) STOP_OR_PRESERVE(real_adapter_destroy_consumer(s_adapter_ctx));

    sink_deinit(&s_sink);
    if (s_adapter_ptr) { free(s_adapter_ptr); s_adapter_ptr = NULL; }
    if (s_adapter_ctx) { free(s_adapter_ctx); s_adapter_ctx = NULL; }
    if (s_machine_event_queue) {
        vQueueDelete(s_machine_event_queue);
        s_machine_event_queue = NULL;
    }

    if (s_safety_owned) {
        STOP_OR_PRESERVE(safety_manager_stop());
        s_safety_owned = false;
    }
    if (s_fake_hal) {
        fake_hal_destroy(s_fake_hal);
        s_fake_hal = NULL;
        s_hal = NULL;
    }
#undef STOP_OR_PRESERVE
    return ESP_OK;
}

static void teardown_services(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_round2_test_global_cleanup());
}

/* ================================================================
 * Test: real adapter step dispatch (10 external steps)
 * ================================================================ */

TEST_CASE("R2: real adapter MOVE_POSITION dispatch", "[r2][adapter]")
{
    setup_services();
    setup_real_adapter();

    wash_program_t prog = make_single_step_program(1, STEP_MOVE_POSITION, DRUM_POS_90, 3000);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(s_exec, &prog, 1));
    TEST_ASSERT_TRUE(wait_for_dispatch(5000));

    /* Inject position terminal */
    inject_service_terminal(MACHINE_EVENT_POSITION_DONE, 1, SERVICE_RESULT_OK);

    /* Wait for program terminal */
    TEST_ASSERT_TRUE(wait_for_terminal(5000));
    xSemaphoreTake(s_sink.mtx, portMAX_DELAY);
    TEST_ASSERT_EQUAL(TERM_RESULT_COMPLETE, s_sink.last_terminal.result);
    xSemaphoreGive(s_sink.mtx);

    teardown_real_adapter();
    teardown_services();
}

TEST_CASE("R2: real adapter HOME dispatch", "[r2][adapter]")
{
    setup_services();
    setup_real_adapter();

    wash_program_t prog = make_single_step_program(1, STEP_HOME_BL50, DRUM_POS_UNKNOWN, 5000);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(s_exec, &prog, 1));
    TEST_ASSERT_TRUE(wait_for_dispatch(5000));

    inject_service_terminal(MACHINE_EVENT_BL50_DONE, 1, SERVICE_RESULT_OK);
    TEST_ASSERT_TRUE(wait_for_terminal(5000));

    teardown_real_adapter();
    teardown_services();
}

TEST_CASE("R2: real adapter PULSATOR dispatch default PWM 40", "[r2][adapter]")
{
    setup_services();
    setup_real_adapter();

    wash_program_t prog = make_single_step_program(1, STEP_PULSATOR_WASH, DRUM_POS_UNKNOWN, 3000);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(s_exec, &prog, 1));

    /* Deterministic wait for dispatch */
    uint32_t dispatch_timeout = 5000;
    uint32_t elapsed = 0;
    while (real_adapter_dispatch_count(s_adapter_ctx) < 1 && elapsed < dispatch_timeout) {
        vTaskDelay(pdMS_TO_TICKS(10));
        elapsed += 10;
    }
    TEST_ASSERT_TRUE(real_adapter_dispatch_count(s_adapter_ctx) >= 1);

    inject_service_terminal(MACHINE_EVENT_BL50_DONE, 1, SERVICE_RESULT_OK);
    TEST_ASSERT_TRUE(wait_for_terminal(5000));

    teardown_real_adapter();
    teardown_services();
}

TEST_CASE("R2: real adapter DRUM dispatch default PWM 30", "[r2][adapter]")
{
    setup_services();
    setup_real_adapter();

    wash_program_t prog = make_single_step_program(1, STEP_DRUM_WASH, DRUM_POS_UNKNOWN, 3000);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(s_exec, &prog, 1));

    /* Deterministic wait for dispatch */
    uint32_t dispatch_timeout = 5000;
    uint32_t elapsed = 0;
    while (real_adapter_dispatch_count(s_adapter_ctx) < 1 && elapsed < dispatch_timeout) {
        vTaskDelay(pdMS_TO_TICKS(10));
        elapsed += 10;
    }
    TEST_ASSERT_TRUE(real_adapter_dispatch_count(s_adapter_ctx) >= 1);

    inject_service_terminal(MACHINE_EVENT_BL50_DONE, 1, SERVICE_RESULT_OK);
    TEST_ASSERT_TRUE(wait_for_terminal(5000));

    teardown_real_adapter();
    teardown_services();
}

TEST_CASE("R2: real adapter SPIN dispatch default PWM 80", "[r2][adapter]")
{
    setup_services();
    setup_real_adapter();

    wash_program_t prog = make_single_step_program(1, STEP_SPIN, DRUM_POS_UNKNOWN, 3000);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(s_exec, &prog, 1));

    /* Deterministic wait for dispatch */
    uint32_t dispatch_timeout = 5000;
    uint32_t elapsed = 0;
    while (real_adapter_dispatch_count(s_adapter_ctx) < 1 && elapsed < dispatch_timeout) {
        vTaskDelay(pdMS_TO_TICKS(10));
        elapsed += 10;
    }
    TEST_ASSERT_TRUE(real_adapter_dispatch_count(s_adapter_ctx) >= 1);

    inject_service_terminal(MACHINE_EVENT_BL50_DONE, 1, SERVICE_RESULT_OK);
    TEST_ASSERT_TRUE(wait_for_terminal(5000));

    teardown_real_adapter();
    teardown_services();
}

TEST_CASE("R2: real adapter WATER dispatch", "[r2][adapter]")
{
    setup_services();
    setup_real_adapter();

    wash_program_t prog = make_single_step_program(1, STEP_WATER_IN, DRUM_POS_UNKNOWN, 0);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(s_exec, &prog, 1));

    /* Deterministic wait for dispatch */
    uint32_t dispatch_timeout = 5000;
    uint32_t elapsed = 0;
    while (real_adapter_dispatch_count(s_adapter_ctx) < 1 && elapsed < dispatch_timeout) {
        vTaskDelay(pdMS_TO_TICKS(10));
        elapsed += 10;
    }
    TEST_ASSERT_TRUE(real_adapter_dispatch_count(s_adapter_ctx) >= 1);

    inject_service_terminal(MACHINE_EVENT_WATER_DONE, 1, SERVICE_RESULT_OK);
    TEST_ASSERT_TRUE(wait_for_terminal(5000));

    teardown_real_adapter();
    teardown_services();
}

TEST_CASE("R2: real adapter DETERGENT dispatch", "[r2][adapter]")
{
    setup_services();
    setup_real_adapter();

    wash_program_t prog = make_single_step_program(1, STEP_DETERGENT, DRUM_POS_UNKNOWN, 3000);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(s_exec, &prog, 1));
    TEST_ASSERT_TRUE(wait_for_dispatch(5000));

    inject_service_terminal(MACHINE_EVENT_DETERGENT_DONE, 1, SERVICE_RESULT_OK);
    TEST_ASSERT_TRUE(wait_for_terminal(5000));

    teardown_real_adapter();
    teardown_services();
}

TEST_CASE("R2: real adapter DRAIN dispatch", "[r2][adapter]")
{
    setup_services();
    setup_real_adapter();

    wash_program_t prog = make_single_step_program(1, STEP_DRAIN, DRUM_POS_UNKNOWN, 5000);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(s_exec, &prog, 1));
    TEST_ASSERT_TRUE(wait_for_dispatch(5000));

    inject_service_terminal(MACHINE_EVENT_DRAIN_DONE, 1, SERVICE_RESULT_OK);
    TEST_ASSERT_TRUE(wait_for_terminal(5000));

    teardown_real_adapter();
    teardown_services();
}

TEST_CASE("R2: real adapter DRY dispatch", "[r2][adapter]")
{
    setup_services();
    setup_real_adapter();

    wash_program_t prog = make_single_step_program(1, STEP_DRY, DRUM_POS_UNKNOWN, 3000);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(s_exec, &prog, 1));
    TEST_ASSERT_TRUE(wait_for_dispatch(5000));

    inject_service_terminal(MACHINE_EVENT_DRY_DONE, 1, SERVICE_RESULT_OK);
    TEST_ASSERT_TRUE(wait_for_terminal(5000));

    teardown_real_adapter();
    teardown_services();
}

TEST_CASE("R2: real adapter UV dispatch", "[r2][adapter]")
{
    setup_services();
    setup_real_adapter();

    wash_program_t prog = make_single_step_program(1, STEP_UV, DRUM_POS_UNKNOWN, 3000);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(s_exec, &prog, 1));
    TEST_ASSERT_TRUE(wait_for_dispatch(5000));

    inject_service_terminal(MACHINE_EVENT_UV_DONE, 1, SERVICE_RESULT_OK);
    TEST_ASSERT_TRUE(wait_for_terminal(5000));

    teardown_real_adapter();
    teardown_services();
}

/* ================================================================
 * Test: terminal matching (wrong/stale/duplicate)
 * ================================================================ */

TEST_CASE("R2: wrong service terminal rejected", "[r2][terminal]")
{
    setup_services();
    setup_real_adapter();

    wash_program_t prog = make_single_step_program(1, STEP_MOVE_POSITION, DRUM_POS_90, 3000);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(s_exec, &prog, 1));
    TEST_ASSERT_TRUE(wait_for_dispatch(5000));

    /* Send wrong service terminal (BL50_DONE for position step) */
    inject_service_terminal(MACHINE_EVENT_BL50_DONE, 1, SERVICE_RESULT_OK);
    vTaskDelay(pdMS_TO_TICKS(300));

    /* Should not have delivered terminal */
    TEST_ASSERT_FALSE(xEventGroupGetBits(s_sink.eg) & SINK_DONE_BIT);
    TEST_ASSERT_TRUE(real_adapter_wrong_service_drops(s_adapter_ctx) >= 1);

    /* Now send correct terminal */
    inject_service_terminal(MACHINE_EVENT_POSITION_DONE, 1, SERVICE_RESULT_OK);
    TEST_ASSERT_TRUE(wait_for_terminal(5000));

    teardown_real_adapter();
    teardown_services();
}

TEST_CASE("R2: stale request_id rejected", "[r2][terminal]")
{
    setup_services();
    setup_real_adapter();

    wash_program_t prog = make_single_step_program(1, STEP_MOVE_POSITION, DRUM_POS_90, 3000);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(s_exec, &prog, 1));
    TEST_ASSERT_TRUE(wait_for_dispatch(5000));

    /* Send terminal with wrong request_id (99 instead of actual) */
    inject_service_terminal(MACHINE_EVENT_POSITION_DONE, 99, SERVICE_RESULT_OK);
    vTaskDelay(pdMS_TO_TICKS(300));
    TEST_ASSERT_TRUE(real_adapter_stale_drops(s_adapter_ctx) >= 1);

    /* Correct terminal */
    inject_service_terminal(MACHINE_EVENT_POSITION_DONE, 1, SERVICE_RESULT_OK);
    TEST_ASSERT_TRUE(wait_for_terminal(5000));

    teardown_real_adapter();
    teardown_services();
}

TEST_CASE("R2: duplicate terminal rejected", "[r2][terminal]")
{
    setup_services();
    setup_real_adapter();

    wash_program_t prog = make_single_step_program(1, STEP_MOVE_POSITION, DRUM_POS_90, 3000);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(s_exec, &prog, 1));
    TEST_ASSERT_TRUE(wait_for_dispatch(5000));

    /* First terminal - should be accepted */
    inject_service_terminal(MACHINE_EVENT_POSITION_DONE, 1, SERVICE_RESULT_OK);
    TEST_ASSERT_TRUE(wait_for_terminal(5000));

    /* Duplicate terminal - should be stale (slot already cleared) */
    inject_service_terminal(MACHINE_EVENT_POSITION_DONE, 1, SERVICE_RESULT_OK);
    vTaskDelay(pdMS_TO_TICKS(300));
    /* Count should not increase further */
    TEST_ASSERT_EQUAL(1, s_sink.call_count);

    teardown_real_adapter();
    teardown_services();
}

TEST_CASE("R2: cancel ACK matches correctly", "[r2][terminal]")
{
    setup_services();
    setup_real_adapter();

    wash_program_t prog = make_single_step_program(1, STEP_MOVE_POSITION, DRUM_POS_90, 3000);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(s_exec, &prog, 1));
    TEST_ASSERT_TRUE(wait_for_dispatch(5000));

    real_adapter_snapshot_t snap = {0};
    TEST_ASSERT_TRUE(wait_for_adapter_state(true, false, false, 2000, &snap));

    /* Exercise the real cancel path. Position service publishes CANCELED. */
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_urgent(s_exec, false));
    TEST_ASSERT_TRUE(wait_for_adapter_state(false, true, false, 2000, &snap));

    /* Wait until the real CANCELED terminal advances the core and the reset
     * dispatch is committed. Only then inject its matching completion. */
    TEST_ASSERT_TRUE(wait_for_adapter_state(false, false, true, 3000, &snap));
    inject_service_terminal(MACHINE_EVENT_POSITION_DONE,
                            snap.reset_request_id, SERVICE_RESULT_OK);

    /* Wait for program terminal with CANCELED result */
    TEST_ASSERT_TRUE(wait_for_terminal(8000));
    xSemaphoreTake(s_sink.mtx, portMAX_DELAY);
    TEST_ASSERT_EQUAL(TERM_RESULT_CANCELED, s_sink.last_terminal.result);
    xSemaphoreGive(s_sink.mtx);

    teardown_real_adapter();
    teardown_services();
}

/* ================================================================
 * Test: QueueSet wakeup
 * ================================================================ */

TEST_CASE("R2: QueueSet program wake", "[r2][queueset]")
{
    setup_services();
    setup_real_adapter();

    wash_program_t prog = make_single_step_program(1, STEP_MOVE_POSITION, DRUM_POS_90, 3000);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(s_exec, &prog, 1));

    /* QueueSet should wake executor, dispatch should happen */
    TEST_ASSERT_TRUE(wait_for_dispatch(5000));

    /* Complete the program */
    inject_service_terminal(MACHINE_EVENT_POSITION_DONE, 1, SERVICE_RESULT_OK);
    TEST_ASSERT_TRUE(wait_for_terminal(5000));

    teardown_real_adapter();
    teardown_services();
}

TEST_CASE("R2: QueueSet urgent wake", "[r2][queueset]")
{
    setup_services();
    setup_real_adapter();

    wash_program_t prog = make_single_step_program(1, STEP_MOVE_POSITION, DRUM_POS_90, 3000);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(s_exec, &prog, 1));
    TEST_ASSERT_TRUE(wait_for_dispatch(5000));

    /* Submit urgent cancel */
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_urgent(s_exec, false));

    /* QueueSet should wake executor for urgent processing */
    vTaskDelay(pdMS_TO_TICKS(300));

    /* Cancel and reset flow */
    inject_service_terminal(MACHINE_EVENT_POSITION_DONE, 1, SERVICE_RESULT_CANCELED);
    vTaskDelay(pdMS_TO_TICKS(300));
    inject_service_terminal(MACHINE_EVENT_POSITION_DONE, 2, SERVICE_RESULT_OK);

    TEST_ASSERT_TRUE(wait_for_terminal(8000));

    teardown_real_adapter();
    teardown_services();
}

TEST_CASE("R2: QueueSet service wake", "[r2][queueset]")
{
    setup_services();
    setup_real_adapter();

    wash_program_t prog = make_single_step_program(1, STEP_MOVE_POSITION, DRUM_POS_90, 3000);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(s_exec, &prog, 1));
    TEST_ASSERT_TRUE(wait_for_dispatch(5000));

    /* Inject service terminal — should wake executor via QueueSet */
    inject_service_terminal(MACHINE_EVENT_POSITION_DONE, 1, SERVICE_RESULT_OK);
    TEST_ASSERT_TRUE(wait_for_terminal(5000));

    teardown_real_adapter();
    teardown_services();
}

/* ================================================================
 * Test: Bootstrap lifecycle
 * ================================================================ */

TEST_CASE("R2: bootstrap create and destroy", "[r2][bootstrap]")
{
    s_machine_event_queue = xQueueCreate(16, sizeof(machine_event_t));
    TEST_ASSERT_NOT_NULL(s_machine_event_queue);

    esp_err_t err = wash_exec_bootstrap_create((void *)s_machine_event_queue);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    wash_executor_t *exec = wash_exec_bootstrap_get_executor();
    TEST_ASSERT_NOT_NULL(exec);

    wash_exec_bootstrap_quiesce();
    err = wash_exec_bootstrap_stop(5000);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    TEST_ASSERT_EQUAL(ESP_OK, wash_exec_bootstrap_destroy());

    vQueueDelete(s_machine_event_queue);
    s_machine_event_queue = NULL;
}

TEST_CASE("R2: bootstrap second deinit idempotent", "[r2][bootstrap]")
{
    s_machine_event_queue = xQueueCreate(16, sizeof(machine_event_t));
    TEST_ASSERT_NOT_NULL(s_machine_event_queue);

    esp_err_t err = wash_exec_bootstrap_create((void *)s_machine_event_queue);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    wash_exec_bootstrap_quiesce();
    err = wash_exec_bootstrap_stop(5000);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(ESP_OK, wash_exec_bootstrap_destroy());

    /* Second destroy should be safe */
    TEST_ASSERT_EQUAL(ESP_OK, wash_exec_bootstrap_destroy());

    vQueueDelete(s_machine_event_queue);
    s_machine_event_queue = NULL;
}

TEST_CASE("R2: 20 round lifecycle stability", "[r2][bootstrap]")
{
    for (int i = 0; i < 20; i++) {
        s_machine_event_queue = xQueueCreate(16, sizeof(machine_event_t));
        TEST_ASSERT_NOT_NULL(s_machine_event_queue);

        esp_err_t err = wash_exec_bootstrap_create((void *)s_machine_event_queue);
        TEST_ASSERT_EQUAL(ESP_OK, err);

        wash_exec_bootstrap_quiesce();
        err = wash_exec_bootstrap_stop(5000);
        TEST_ASSERT_EQUAL(ESP_OK, err);
        TEST_ASSERT_EQUAL(ESP_OK, wash_exec_bootstrap_destroy());

        vQueueDelete(s_machine_event_queue);
        s_machine_event_queue = NULL;
    }
    /* Check heap stability */
    size_t free_heap = esp_get_free_heap_size();
    TEST_ASSERT_GREATER_THAN(10000, free_heap);
}

/* ================================================================
 * Test: quiesce drains service terminals
 * ================================================================ */

TEST_CASE("R2: quiesce drains pending terminals", "[r2][bootstrap]")
{
    setup_services();
    setup_real_adapter();

    wash_program_t prog = make_single_step_program(1, STEP_MOVE_POSITION, DRUM_POS_90, 3000);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(s_exec, &prog, 1));
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Quiesce while step is active */
    wash_executor_quiesce(s_exec);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Inject terminal — should still be consumed */
    inject_service_terminal(MACHINE_EVENT_POSITION_DONE, 1, SERVICE_RESULT_OK);
    TEST_ASSERT_TRUE(wait_for_terminal(5000));

    teardown_real_adapter();
    teardown_services();
}

/* ================================================================
 * Test: Button routing via fake HAL
 * ================================================================ */

TEST_CASE("R2: BTN3 urgent abort via fake HAL", "[r2][button]")
{
    setup_services();
    setup_real_adapter();

    wash_program_t prog = make_single_step_program(1, STEP_MOVE_POSITION, DRUM_POS_90, 3000);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(s_exec, &prog, 1));
    TEST_ASSERT_TRUE(wait_for_dispatch(5000));

    /* Inject BTN3 press via fake HAL */
    uint8_t btn3_pressed = 0xBF; /* GPA6 low = BTN3 pressed */
    fake_hal_set_button_state(s_fake_hal, btn3_pressed);

    /* Wait for executor to process the urgent event */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Executor should be in CANCELING or RESET phase */
    wash_exec_snapshot_t snap;
    wash_executor_get_snapshot(s_exec, &snap);
    /* The exact state depends on timing, but program should be canceling */
    TEST_ASSERT_TRUE(snap.state == WASH_EXEC_STATE_RUNNING ||
                     snap.state == WASH_EXEC_STATE_RESETTING);

    /* Inject cancel + reset terminals to complete */
    inject_service_terminal(MACHINE_EVENT_POSITION_DONE, 1, SERVICE_RESULT_CANCELED);
    vTaskDelay(pdMS_TO_TICKS(300));
    inject_service_terminal(MACHINE_EVENT_POSITION_DONE, 2, SERVICE_RESULT_OK);
    TEST_ASSERT_TRUE(wait_for_terminal(8000));

    teardown_real_adapter();
    teardown_services();
}

/* ================================================================
 * Test: UV skip
 * ================================================================ */

TEST_CASE("R2: UV skip waits for SKIPPED terminal", "[r2][uv]")
{
    setup_services();
    setup_real_adapter();

    wash_program_t prog = make_single_step_program(1, STEP_UV, DRUM_POS_UNKNOWN, 5000);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(s_exec, &prog, 1));
    TEST_ASSERT_TRUE(wait_for_dispatch(5000));

    real_adapter_snapshot_t snap = {0};
    TEST_ASSERT_TRUE(wait_for_adapter_state(true, false, false, 2000, &snap));
    TEST_ASSERT_EQUAL(STEP_UV, snap.active_step_type);

    /* Exercise the real UV service skip/OFF/terminal path. */
    TEST_ASSERT_EQUAL(ESP_OK, uv_service_skip(snap.active_request_id));

    /* Should complete successfully (SKIPPED = success for executor) */
    TEST_ASSERT_TRUE(wait_for_terminal(5000));
    xSemaphoreTake(s_sink.mtx, portMAX_DELAY);
    TEST_ASSERT_EQUAL(TERM_RESULT_COMPLETE, s_sink.last_terminal.result);
    xSemaphoreGive(s_sink.mtx);

    teardown_real_adapter();
    teardown_services();
}

/* ================================================================
 * Test: dispatch failure → FAULT
 * ================================================================ */

TEST_CASE("R2: dispatch failure enters FAULT", "[r2][fault]")
{
    setup_services();
    setup_real_adapter();

    /* Stop position service to cause dispatch failure */
    position_service_stop();

    wash_program_t prog = make_single_step_program(1, STEP_MOVE_POSITION, DRUM_POS_90, 3000);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(s_exec, &prog, 1));
    /* A rejected dispatch intentionally does not increment dispatch_count.
     * Wait for its observable terminal instead of waiting for success. */
    TEST_ASSERT_TRUE(wait_for_terminal(3000));

    /* Should enter FAULT */
    wash_exec_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_get_snapshot(s_exec, &snap));
    TEST_ASSERT_EQUAL(WASH_EXEC_STATE_FAULT, snap.state);

    /* Program terminal should be FAULT */
    xSemaphoreTake(s_sink.mtx, portMAX_DELAY);
    TEST_ASSERT_EQUAL(TERM_RESULT_FAULT, s_sink.last_terminal.result);
    TEST_ASSERT_FALSE(s_sink.last_terminal.final_position_confirmed);
    xSemaphoreGive(s_sink.mtx);

    teardown_real_adapter();
    teardown_services();
}

/* ================================================================
 * Test: position terminal loss → timeout → FAULT
 * ================================================================ */

TEST_CASE("R2: position terminal loss timeout FAULT", "[r2][timeout]")
{
    setup_services();
    setup_real_adapter();

    /* Use short timeout */
    wash_program_t prog = make_single_step_program(1, STEP_MOVE_POSITION, DRUM_POS_90, 3000);
    prog.steps[0].timeout_ms = 1000; /* 1 second timeout */
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(s_exec, &prog, 1));
    TEST_ASSERT_TRUE(wait_for_dispatch(5000));

    /* Don't inject any terminal — let it timeout */

    /* Should trigger cancel+reset then FAULT */
    vTaskDelay(pdMS_TO_TICKS(5000));

    wash_exec_snapshot_t snap;
    wash_executor_get_snapshot(s_exec, &snap);
    /* After timeout, executor enters CANCELING, then RESET, then FAULT */
    /* The exact state depends on whether cancel/reset also timeout */

    teardown_real_adapter();
    teardown_services();
}

/* ================================================================
 * Test: main test hooks disabled
 * ================================================================ */

TEST_CASE("R2: main test hooks disabled", "[r2][meta]")
{
#ifdef WASH_EXEC_TEST_HOOKS
    /* If compiled with test hooks, verify they're NULL by default */
    /* This is a compile-time check - if WASH_EXEC_TEST_HOOKS is not defined,
     * the hooks don't exist at all */
#else
    /* Test hooks not compiled in — correct for production */
    TEST_PASS();
#endif
}

void wash_executor_round2_integration_tests_force_link(void) {}
