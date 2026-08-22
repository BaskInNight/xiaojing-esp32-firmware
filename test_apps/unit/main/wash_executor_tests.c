/*
 * wash_executor_tests.c — Unity wrapper integration tests (Round 1.3)
 *
 * Runs on ESP32-S3 with FreeRTOS. Uses real queues/tasks/mutex.
 * Protocol-correct: reads tokens from fake adapter, explicit terminal.
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
#include <string.h>

static const char *TAG = "exe_test";

/* ---- Static buffers (main task stack is small) ---- */

static wash_executor_adapter_t g_adapter;
static uint8_t g_adapter_ctx[FAKE_ADAPTER_CTX_SIZE];
static wash_executor_adapter_t g_adapter2;
static uint8_t g_adapter_ctx2[FAKE_ADAPTER_CTX_SIZE];
static wash_program_t g_prog;
static wash_executor_config_t g_cfg = {
    .step_timeout_default_ms = 5000,
    .reset_timeout_ms = 1000,
    .cancel_timeout_ms = 500,
    .terminal_retry_ms = 100
};

/* ---- Helper: complete a program by publishing step terminals ---- */
static void complete_program_steps(
    wash_executor_t *exec, void *adapter_ctx,
    uint32_t expected_steps)
{
    for (uint32_t i = 0; i < expected_steps; i++) {
        if (!fake_adapter_wait_dispatch(adapter_ctx, i + 1, 3000)) break;
        const fake_dispatch_record_t *rec = fake_adapter_get_dispatch(adapter_ctx, i);
        if (!rec) break;
        terminal_token_t tok = rec->token;
        wash_executor_publish_terminal(exec, &tok, sizeof(tok), true, 0);
    }
}

/* ---- Test helpers ---- */

/* Safe cleanup: stop then destroy, asserting success.
 * Returns false if cleanup failed (orphan task risk). */
static bool safe_cleanup(wash_executor_t *exec, uint32_t stop_timeout_ms)
{
    if (!exec) return true;
    esp_err_t sr = wash_executor_stop(exec, stop_timeout_ms);
    if (sr != ESP_OK) {
        ESP_LOGE(TAG, "safe_cleanup: stop failed 0x%x", sr);
        return false;
    }
    esp_err_t dr = wash_executor_destroy(exec);
    if (dr != ESP_OK) {
        ESP_LOGE(TAG, "safe_cleanup: destroy failed 0x%x", dr);
        return false;
    }
    return true;
}

static wash_program_t make_simple_program(uint32_t prog_id)
{
    wash_program_t prog;
    memset(&prog, 0, sizeof(prog));
    prog.program_id = prog_id;
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

/* Terminal sink tracking */
typedef struct {
    SemaphoreHandle_t mutex;
    EventGroupHandle_t received;
    program_terminal_t last_terminal;
    uint32_t call_count;
    bool return_value;
} test_sink_ctx_t;

static test_sink_ctx_t g_sink;
static test_sink_ctx_t g_sink2;

static bool test_terminal_sink(const program_terminal_t *terminal, void *ctx)
{
    test_sink_ctx_t *s = (test_sink_ctx_t *)ctx;
    xSemaphoreTake(s->mutex, portMAX_DELAY);
    s->last_terminal = *terminal;
    s->call_count++;
    bool rv = s->return_value;
    xSemaphoreGive(s->mutex);
    xEventGroupSetBits(s->received, 1);
    return rv;
}

static void sink_ctx_init(test_sink_ctx_t *s)
{
    /* Clean up previous resources if any */
    if (s->mutex) vSemaphoreDelete(s->mutex);
    if (s->received) vEventGroupDelete(s->received);
    memset(s, 0, sizeof(*s));
    s->mutex = xSemaphoreCreateMutex();
    s->received = xEventGroupCreate();
    s->return_value = true;
}

static void sink_ctx_deinit(test_sink_ctx_t *s)
{
    /* Only deinit if no executor task could be using it */
    /* For static sinks, skip deinit — init handles cleanup */
}

/* ---- EXE-W01: adapter receives dispatch (typed records) ---- */
TEST_CASE("executor: adapter receives dispatch typed", "[executor][exe]")
{
    fake_adapter_init(&g_adapter, g_adapter_ctx);
    g_cfg.step_timeout_default_ms = 10000;
    g_cfg.reset_timeout_ms = 1000;
    g_cfg.cancel_timeout_ms = 500;
    g_cfg.terminal_retry_ms = 100;
    wash_executor_t *exec = wash_executor_create(&g_cfg, &g_adapter);
    TEST_ASSERT_NOT_NULL(exec);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_start(exec));

    g_prog = make_simple_program(1);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(exec, &g_prog, 1));

    TEST_ASSERT_TRUE(fake_adapter_wait_dispatch(g_adapter_ctx, 1, 2000));
    TEST_ASSERT_EQUAL(1u, fake_adapter_dispatch_count(g_adapter_ctx));

    const fake_dispatch_record_t *rec = fake_adapter_get_dispatch(g_adapter_ctx, 0);
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL(STEP_MOVE_POSITION, rec->step.type);
    TEST_ASSERT_EQUAL(1u, rec->token.program_id);
    TEST_ASSERT_EQUAL(TERMINAL_KIND_NORMAL, rec->token.terminal_kind);

    /* Complete all steps for clean shutdown */
    terminal_token_t tok = rec->token;
    wash_executor_publish_terminal(exec, &tok, sizeof(tok), true, 0);
    TEST_ASSERT_TRUE(fake_adapter_wait_dispatch(g_adapter_ctx, 2, 2000));
    rec = fake_adapter_get_dispatch(g_adapter_ctx, 1);
    tok = rec->token;
    wash_executor_publish_terminal(exec, &tok, sizeof(tok), true, 0);

    /* Wait for task to process all terminals and return to IDLE */
    vTaskDelay(pdMS_TO_TICKS(200));

    TEST_ASSERT_TRUE(safe_cleanup(exec, 10000));
}

/* ---- EXE-W02: submit deep copy independent ---- */
TEST_CASE("executor: submit deep copy independent", "[executor][exe]")
{
    vTaskDelay(pdMS_TO_TICKS(500));  /* let previous test fully clean up */
    fake_adapter_init(&g_adapter, g_adapter_ctx);
    g_cfg.step_timeout_default_ms = 5000;
    g_cfg.reset_timeout_ms = 1000;
    g_cfg.cancel_timeout_ms = 500;
    g_cfg.terminal_retry_ms = 100;
    wash_executor_t *exec = wash_executor_create(&g_cfg, &g_adapter);
    TEST_ASSERT_NOT_NULL(exec);
    wash_executor_set_terminal_sink(exec, test_terminal_sink, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_start(exec));

    g_prog = make_simple_program(10);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(exec, &g_prog, 10));

    /* Modify caller's copy after submit */
    g_prog.program_id = 999;
    g_prog.step_count = 0;

    TEST_ASSERT_TRUE(fake_adapter_wait_dispatch(g_adapter_ctx, 1, 2000));

    /* Verify executor used the original copy */
    wash_exec_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_get_snapshot(exec, &snap));
    TEST_ASSERT_EQUAL(10u, snap.program_id);

    /* Complete the program to allow clean shutdown */
    const fake_dispatch_record_t *rec = fake_adapter_get_dispatch(g_adapter_ctx, 0);
    terminal_token_t tok = rec->token;
    wash_executor_publish_terminal(exec, &tok, sizeof(tok), true, 0);

    TEST_ASSERT_TRUE(fake_adapter_wait_dispatch(g_adapter_ctx, 2, 2000));
    rec = fake_adapter_get_dispatch(g_adapter_ctx, 1);
    tok = rec->token;
    wash_executor_publish_terminal(exec, &tok, sizeof(tok), true, 0);

    TEST_ASSERT_TRUE(safe_cleanup(exec, 10000));
}

/* ---- EXE-W04: concurrent submit — two tasks, barrier ---- */
typedef struct {
    wash_executor_t *exec;
    wash_program_t *prog;
    uint32_t program_id;
    SemaphoreHandle_t barrier;
    esp_err_t result;
} submit_task_arg_t;

static void submit_task_fn(void *arg)
{
    submit_task_arg_t *a = (submit_task_arg_t *)arg;
    xSemaphoreTake(a->barrier, portMAX_DELAY);
    xSemaphoreGive(a->barrier);
    a->result = wash_executor_submit_program(a->exec, a->prog, a->program_id);
    vTaskDelete(NULL);
}

TEST_CASE("executor: concurrent submit one success", "[executor][exe]")
{
    fake_adapter_init(&g_adapter, g_adapter_ctx);
    g_cfg.step_timeout_default_ms = 5000;
    g_cfg.reset_timeout_ms = 1000;
    g_cfg.cancel_timeout_ms = 500;
    g_cfg.terminal_retry_ms = 100;
    wash_executor_t *exec = wash_executor_create(&g_cfg, &g_adapter);
    TEST_ASSERT_NOT_NULL(exec);
    wash_executor_set_terminal_sink(exec, test_terminal_sink, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_start(exec));

    wash_program_t prog1 = make_simple_program(1);
    wash_program_t prog2 = make_simple_program(2);

    SemaphoreHandle_t barrier = xSemaphoreCreateMutex();
    submit_task_arg_t arg1 = { exec, &prog1, 1, barrier, ESP_FAIL };
    submit_task_arg_t arg2 = { exec, &prog2, 2, barrier, ESP_FAIL };

    /* Pre-fill barrier: both tasks can immediately proceed */
    xSemaphoreTake(barrier, portMAX_DELAY);
    xTaskCreate(submit_task_fn, "sub1", 2048, &arg1, 6, NULL);
    xTaskCreate(submit_task_fn, "sub2", 2048, &arg2, 6, NULL);
    vTaskDelay(pdMS_TO_TICKS(10));
    xSemaphoreGive(barrier);

    vTaskDelay(pdMS_TO_TICKS(200));

    int ok_count = 0;
    if (arg1.result == ESP_OK) ok_count++;
    if (arg2.result == ESP_OK) ok_count++;
    TEST_ASSERT_EQUAL(1, ok_count);

    /* Complete the program that was accepted */
    complete_program_steps(exec, g_adapter_ctx, 2);

    vSemaphoreDelete(barrier);
    TEST_ASSERT_TRUE(safe_cleanup(exec, 10000));
}

/* ---- EXE-W05: step-by-step program with explicit terminal ---- */
TEST_CASE("executor: full program with explicit terminal", "[executor][exe]")
{
    fake_adapter_init(&g_adapter, g_adapter_ctx);
    g_cfg.step_timeout_default_ms = 10000;
    g_cfg.reset_timeout_ms = 1000;
    g_cfg.cancel_timeout_ms = 500;
    g_cfg.terminal_retry_ms = 100;
    wash_executor_t *exec = wash_executor_create(&g_cfg, &g_adapter);
    TEST_ASSERT_NOT_NULL(exec);

    sink_ctx_init(&g_sink);
    wash_executor_set_terminal_sink(exec, test_terminal_sink, &g_sink);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_start(exec));

    g_prog = make_simple_program(1);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(exec, &g_prog, 1));

    uint32_t expected_steps[] = { STEP_MOVE_POSITION, STEP_WATER_IN };
    for (int i = 0; i < 2; i++) {
        TEST_ASSERT_TRUE(fake_adapter_wait_dispatch(g_adapter_ctx, i + 1, 2000));
        const fake_dispatch_record_t *rec = fake_adapter_get_dispatch(g_adapter_ctx, i);
        TEST_ASSERT_NOT_NULL(rec);
        TEST_ASSERT_EQUAL(expected_steps[i], rec->step.type);
        TEST_ASSERT_EQUAL(TERMINAL_KIND_NORMAL, rec->token.terminal_kind);

        /* Send terminal for this step */
        terminal_token_t tok = rec->token;
        TEST_ASSERT_EQUAL(ESP_OK,
            wash_executor_publish_terminal(exec, &tok, sizeof(tok), true, 0));
    }

    /* STEP_FINISH auto-completes, terminal arrives */
    EventBits_t bits = xEventGroupWaitBits(g_sink.received, 1, pdTRUE, pdFALSE,
                                            pdMS_TO_TICKS(5000));
    TEST_ASSERT_TRUE(bits & 1);
    xSemaphoreTake(g_sink.mutex, portMAX_DELAY);
    TEST_ASSERT_EQUAL(1u, g_sink.last_terminal.program_id);
    TEST_ASSERT_EQUAL(TERM_RESULT_COMPLETE, g_sink.last_terminal.result);
    TEST_ASSERT_EQUAL(3u, g_sink.last_terminal.completed_steps);
    xSemaphoreGive(g_sink.mutex);

    sink_ctx_deinit(&g_sink);
    TEST_ASSERT_TRUE(safe_cleanup(exec, 10000));
}

/* ---- EXE-W13: full cancel → CANCEL_ACK → reset → RESET_ACK → CANCELED ---- */
TEST_CASE("executor: cancel reset full protocol", "[executor][exe]")
{
    fake_adapter_init(&g_adapter, g_adapter_ctx);
    g_cfg.step_timeout_default_ms = 10000;
    g_cfg.reset_timeout_ms = 1000;
    g_cfg.cancel_timeout_ms = 500;
    g_cfg.terminal_retry_ms = 100;
    wash_executor_t *exec = wash_executor_create(&g_cfg, &g_adapter);
    TEST_ASSERT_NOT_NULL(exec);

    sink_ctx_init(&g_sink);
    wash_executor_set_terminal_sink(exec, test_terminal_sink, &g_sink);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_start(exec));

    g_prog = make_simple_program(1);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(exec, &g_prog, 1));

    /* Wait for first dispatch → ACTIVE */
    TEST_ASSERT_TRUE(fake_adapter_wait_dispatch(g_adapter_ctx, 1, 2000));
    const fake_dispatch_record_t *d0 = fake_adapter_get_dispatch(g_adapter_ctx, 0);
    TEST_ASSERT_NOT_NULL(d0);

    /* Cancel */
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_urgent(exec, false));

    /* Wait for cancel dispatch */
    TEST_ASSERT_TRUE(fake_adapter_wait_cancel(g_adapter_ctx, 1, 2000));
    const terminal_token_t *cancel_tok = fake_adapter_get_cancel_token(g_adapter_ctx, 0);
    TEST_ASSERT_NOT_NULL(cancel_tok);
    TEST_ASSERT_EQUAL(TERMINAL_KIND_CANCEL_ACK, cancel_tok->terminal_kind);

    /* Publish CANCEL_ACK */
    terminal_token_t ack = *cancel_tok;
    TEST_ASSERT_EQUAL(ESP_OK,
        wash_executor_publish_terminal(exec, &ack, sizeof(ack), true, 0));

    /* Wait for reset dispatch */
    TEST_ASSERT_TRUE(fake_adapter_wait_reset(g_adapter_ctx, 1, 2000));
    const terminal_token_t *reset_tok = fake_adapter_get_reset_token(g_adapter_ctx, 0);
    TEST_ASSERT_NOT_NULL(reset_tok);
    TEST_ASSERT_EQUAL(TERMINAL_KIND_RESET_ACK, reset_tok->terminal_kind);

    /* Publish RESET_ACK */
    terminal_token_t rack = *reset_tok;
    TEST_ASSERT_EQUAL(ESP_OK,
        wash_executor_publish_terminal(exec, &rack, sizeof(rack), true, 0));

    /* Wait for CANCELED terminal */
    EventBits_t bits = xEventGroupWaitBits(g_sink.received, 1, pdTRUE, pdFALSE,
                                            pdMS_TO_TICKS(5000));
    TEST_ASSERT_TRUE(bits & 1);
    xSemaphoreTake(g_sink.mutex, portMAX_DELAY);
    TEST_ASSERT_EQUAL(TERM_RESULT_CANCELED, g_sink.last_terminal.result);
    TEST_ASSERT_EQUAL(1u, g_sink.last_terminal.program_id);
    xSemaphoreGive(g_sink.mutex);

    sink_ctx_deinit(&g_sink);
    TEST_ASSERT_TRUE(safe_cleanup(exec, 10000));
}

/* ---- EXE-W18: sink failure → pending → retry → success ---- */
TEST_CASE("executor: sink failure retry", "[executor][exe]")
{
    fake_adapter_init(&g_adapter, g_adapter_ctx);
    g_cfg.step_timeout_default_ms = 5000;
    g_cfg.reset_timeout_ms = 1000;
    g_cfg.cancel_timeout_ms = 500;
    g_cfg.terminal_retry_ms = 200;
    wash_executor_t *exec = wash_executor_create(&g_cfg, &g_adapter);
    TEST_ASSERT_NOT_NULL(exec);

    sink_ctx_init(&g_sink);
    g_sink.return_value = false;  /* sink rejects */
    wash_executor_set_terminal_sink(exec, test_terminal_sink, &g_sink);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_start(exec));

    g_prog = make_simple_program(1);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(exec, &g_prog, 1));

    /* Progress through all steps */
    for (int i = 0; i < 2; i++) {
        TEST_ASSERT_TRUE(fake_adapter_wait_dispatch(g_adapter_ctx, i + 1, 2000));
        const fake_dispatch_record_t *rec = fake_adapter_get_dispatch(g_adapter_ctx, i);
        TEST_ASSERT_NOT_NULL(rec);
        terminal_token_t tok = rec->token;
        TEST_ASSERT_EQUAL(ESP_OK,
            wash_executor_publish_terminal(exec, &tok, sizeof(tok), true, 0));
    }

    /* STEP_FINISH auto-completes, terminal delivered to sink (rejected) */
    vTaskDelay(pdMS_TO_TICKS(500));
    xSemaphoreTake(g_sink.mutex, portMAX_DELAY);
    TEST_ASSERT_GREATER_THAN(0u, g_sink.call_count);
    xSemaphoreGive(g_sink.mutex);

    /* Terminal pending — new submit must be rejected */
    wash_program_t prog2 = make_simple_program(2);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
        wash_executor_submit_program(exec, &prog2, 2));

    /* Snapshot shows terminal pending */
    wash_exec_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_get_snapshot(exec, &snap));
    TEST_ASSERT_TRUE(snap.terminal_pending);

    /* Allow sink to succeed, wait for retry with polling */
    xSemaphoreTake(g_sink.mutex, portMAX_DELAY);
    g_sink.return_value = true;
    uint32_t count_before = g_sink.call_count;
    xSemaphoreGive(g_sink.mutex);

    /* Poll until terminal delivered or timeout */
    bool delivered = false;
    for (int i = 0; i < 40; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
        wash_exec_snapshot_t snap2;
        wash_executor_get_snapshot(exec, &snap2);
        if (!snap2.terminal_pending) {
            delivered = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(delivered);

    /* Terminal should have been retried and succeeded */
    xSemaphoreTake(g_sink.mutex, portMAX_DELAY);
    TEST_ASSERT_GREATER_THAN(count_before, g_sink.call_count);
    xSemaphoreGive(g_sink.mutex);

    /* Reservation released */
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_get_snapshot(exec, &snap));
    TEST_ASSERT_FALSE(snap.terminal_pending);

    /* New submit now allowed */
    wash_program_t prog3 = make_simple_program(3);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(exec, &prog3, 3));

    sink_ctx_deinit(&g_sink);
    TEST_ASSERT_TRUE(safe_cleanup(exec, 10000));
}

/* ---- EXE-W23: start succeeds quickly ---- */
TEST_CASE("executor: start ready success", "[executor][exe]")
{
    fake_adapter_init(&g_adapter, g_adapter_ctx);
    g_cfg.step_timeout_default_ms = 5000;
    g_cfg.reset_timeout_ms = 1000;
    g_cfg.cancel_timeout_ms = 500;
    g_cfg.terminal_retry_ms = 100;
    wash_executor_t *exec = wash_executor_create(&g_cfg, &g_adapter);
    TEST_ASSERT_NOT_NULL(exec);

    int64_t t0 = esp_timer_get_time();
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_start(exec));
    int64_t elapsed_us = esp_timer_get_time() - t0;
    TEST_ASSERT_LESS_THAN(2000000, elapsed_us);  /* < 2s */

    TEST_ASSERT_TRUE(safe_cleanup(exec, 10000));
}

/* ---- EXE-W25: second stop idempotent ---- */
TEST_CASE("executor: second stop idempotent", "[executor][exe]")
{
    fake_adapter_init(&g_adapter, g_adapter_ctx);
    g_cfg.step_timeout_default_ms = 5000;
    g_cfg.reset_timeout_ms = 1000;
    g_cfg.cancel_timeout_ms = 500;
    g_cfg.terminal_retry_ms = 100;
    wash_executor_t *exec = wash_executor_create(&g_cfg, &g_adapter);
    TEST_ASSERT_NOT_NULL(exec);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_start(exec));

    /* First stop */
    int64_t t0 = esp_timer_get_time();
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_stop(exec, 2000));
    int64_t t1 = esp_timer_get_time();

    /* Second stop — idempotent fast return */
    esp_err_t r = wash_executor_stop(exec, 2000);
    int64_t t2 = esp_timer_get_time();

    TEST_ASSERT_EQUAL(ESP_OK, r);
    TEST_ASSERT_LESS_THAN(500000, (t2 - t1));  /* < 500ms */

    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_destroy(exec));
}

/* ---- EXE-W30: 20 round lifecycle heap stable ---- */
TEST_CASE("executor: 20 round lifecycle stable", "[executor][exe]")
{
    size_t heap_before = esp_get_free_heap_size();

    for (int i = 0; i < 20; i++) {
        fake_adapter_init(&g_adapter, g_adapter_ctx);
        g_cfg.step_timeout_default_ms = 1000;
        g_cfg.reset_timeout_ms = 1000;
        g_cfg.cancel_timeout_ms = 1000;
        g_cfg.terminal_retry_ms = 100;
        wash_executor_t *exec = wash_executor_create(&g_cfg, &g_adapter);
        TEST_ASSERT_NOT_NULL(exec);

        test_sink_ctx_t sink;
        sink_ctx_init(&g_sink);
        wash_executor_set_terminal_sink(exec, test_terminal_sink, &g_sink);
        TEST_ASSERT_EQUAL(ESP_OK, wash_executor_start(exec));

        g_prog = make_simple_program(i + 1);
        wash_executor_submit_program(exec, &g_prog, i + 1);

        /* Complete all steps */
        complete_program_steps(exec, g_adapter_ctx, 2);

        /* Wait for terminal delivery */
        xEventGroupWaitBits(g_sink.received, 1, pdTRUE, pdFALSE,
                            pdMS_TO_TICKS(5000));

        TEST_ASSERT_EQUAL(ESP_OK, wash_executor_stop(exec, 2000));
        TEST_ASSERT_EQUAL(ESP_OK, wash_executor_destroy(exec));
        sink_ctx_deinit(&g_sink);
    }

    size_t heap_after = esp_get_free_heap_size();
    TEST_ASSERT_INT_WITHIN(8192, heap_before, heap_after);
}

/* ---- EXE-W03: two executor instances isolated ---- */
TEST_CASE("executor: two instances isolated", "[executor][exe]")
{
    fake_adapter_init(&g_adapter, g_adapter_ctx);
    fake_adapter_init(&g_adapter2, g_adapter_ctx2);

    g_cfg.step_timeout_default_ms = 10000;
    g_cfg.reset_timeout_ms = 1000;
    g_cfg.cancel_timeout_ms = 500;
    g_cfg.terminal_retry_ms = 100;
    wash_executor_t *exec1 = wash_executor_create(&g_cfg, &g_adapter);
    wash_executor_t *exec2 = wash_executor_create(&g_cfg, &g_adapter2);
    TEST_ASSERT_NOT_NULL(exec1);
    TEST_ASSERT_NOT_NULL(exec2);

    sink_ctx_init(&g_sink);
    sink_ctx_init(&g_sink2);
    wash_executor_set_terminal_sink(exec1, test_terminal_sink, &g_sink);
    wash_executor_set_terminal_sink(exec2, test_terminal_sink, &g_sink2);

    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_start(exec1));
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_start(exec2));

    /* Program 1: submit and progress through all steps */
    g_prog = make_simple_program(100);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(exec1, &g_prog, 100));

    /* Wait for exec1 dispatch, then publish terminals for each non-finish step */
    for (int step = 0; step < 2; step++) {
        TEST_ASSERT_TRUE(fake_adapter_wait_dispatch(g_adapter_ctx, step + 1, 3000));
        const fake_dispatch_record_t *rec = fake_adapter_get_dispatch(g_adapter_ctx, step);
        TEST_ASSERT_NOT_NULL(rec);
        terminal_token_t tok = rec->token;
        TEST_ASSERT_EQUAL(ESP_OK,
            wash_executor_publish_terminal(exec1, &tok, sizeof(tok), true, 0));
    }

    /* Wait for exec1 program terminal (STEP_FINISH auto-completes) */
    for (int i = 0; i < 100; i++) {
        xSemaphoreTake(g_sink.mutex, portMAX_DELAY);
        uint32_t c1 = g_sink.call_count;
        xSemaphoreGive(g_sink.mutex);
        if (c1 > 0) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    xSemaphoreTake(g_sink.mutex, portMAX_DELAY);
    TEST_ASSERT_GREATER_THAN(0u, g_sink.call_count);
    TEST_ASSERT_EQUAL(100u, g_sink.last_terminal.program_id);
    xSemaphoreGive(g_sink.mutex);

    /* Verify exec2 adapter has zero dispatches (isolation) */
    TEST_ASSERT_EQUAL(0u, fake_adapter_dispatch_count(g_adapter_ctx2));

    /* Program 2: submit and progress through all steps */
    g_prog = make_simple_program(200);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(exec2, &g_prog, 200));

    for (int step = 0; step < 2; step++) {
        TEST_ASSERT_TRUE(fake_adapter_wait_dispatch(g_adapter_ctx2, step + 1, 3000));
        const fake_dispatch_record_t *rec = fake_adapter_get_dispatch(g_adapter_ctx2, step);
        TEST_ASSERT_NOT_NULL(rec);
        terminal_token_t tok = rec->token;
        TEST_ASSERT_EQUAL(ESP_OK,
            wash_executor_publish_terminal(exec2, &tok, sizeof(tok), true, 0));
    }

    for (int i = 0; i < 100; i++) {
        xSemaphoreTake(g_sink2.mutex, portMAX_DELAY);
        uint32_t c2 = g_sink2.call_count;
        xSemaphoreGive(g_sink2.mutex);
        if (c2 > 0) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    xSemaphoreTake(g_sink2.mutex, portMAX_DELAY);
    TEST_ASSERT_GREATER_THAN(0u, g_sink2.call_count);
    TEST_ASSERT_EQUAL(200u, g_sink2.last_terminal.program_id);
    xSemaphoreGive(g_sink2.mutex);

    /* Verify exec1 adapter still only has2 dispatches (no cross-contamination) */
    TEST_ASSERT_EQUAL(2u, fake_adapter_dispatch_count(g_adapter_ctx));
    TEST_ASSERT_EQUAL(2u, fake_adapter_dispatch_count(g_adapter_ctx2));

    sink_ctx_deinit(&g_sink);
    sink_ctx_deinit(&g_sink2);
    TEST_ASSERT_TRUE(safe_cleanup(exec1, 10000));
    TEST_ASSERT_TRUE(safe_cleanup(exec2, 10000));
}

/* ---- EXE-PROTO-01: per-step dispatch records with correct token ---- */
TEST_CASE("executor: per step dispatch records", "[executor][exe]")
{
    fake_adapter_init(&g_adapter, g_adapter_ctx);
    g_cfg.step_timeout_default_ms = 10000;
    g_cfg.reset_timeout_ms = 1000;
    g_cfg.cancel_timeout_ms = 500;
    g_cfg.terminal_retry_ms = 100;
    wash_executor_t *exec = wash_executor_create(&g_cfg, &g_adapter);
    TEST_ASSERT_NOT_NULL(exec);

    sink_ctx_init(&g_sink);
    wash_executor_set_terminal_sink(exec, test_terminal_sink, &g_sink);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_start(exec));

    g_prog = make_simple_program(1);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(exec, &g_prog, 1));

    /* Process step 0: MOVE_POSITION */
    TEST_ASSERT_TRUE(fake_adapter_wait_dispatch(g_adapter_ctx, 1, 2000));
    const fake_dispatch_record_t *r0 = fake_adapter_get_dispatch(g_adapter_ctx, 0);
    TEST_ASSERT_NOT_NULL(r0);
    TEST_ASSERT_EQUAL(STEP_MOVE_POSITION, r0->step.type);
    TEST_ASSERT_EQUAL(TERMINAL_KIND_NORMAL, r0->token.terminal_kind);
    TEST_ASSERT_EQUAL(1u, r0->token.program_id);
    terminal_token_t t0 = r0->token;
    TEST_ASSERT_EQUAL(ESP_OK,
        wash_executor_publish_terminal(exec, &t0, sizeof(t0), true, 0));

    /* Process step 1: WATER_IN */
    TEST_ASSERT_TRUE(fake_adapter_wait_dispatch(g_adapter_ctx, 2, 2000));
    const fake_dispatch_record_t *r1 = fake_adapter_get_dispatch(g_adapter_ctx, 1);
    TEST_ASSERT_NOT_NULL(r1);
    TEST_ASSERT_EQUAL(STEP_WATER_IN, r1->step.type);
    TEST_ASSERT_EQUAL(TERMINAL_KIND_NORMAL, r1->token.terminal_kind);
    TEST_ASSERT_NOT_EQUAL(r0->token.request_id, r1->token.request_id);
    terminal_token_t t1 = r1->token;
    TEST_ASSERT_EQUAL(ESP_OK,
        wash_executor_publish_terminal(exec, &t1, sizeof(t1), true, 0));

    /* Wait for program terminal (STEP_FINISH auto-completes) */
    xEventGroupWaitBits(g_sink.received, 1, pdTRUE, pdFALSE, pdMS_TO_TICKS(5000));
    xSemaphoreTake(g_sink.mutex, portMAX_DELAY);
    TEST_ASSERT_EQUAL(TERM_RESULT_COMPLETE, g_sink.last_terminal.result);
    xSemaphoreGive(g_sink.mutex);

    sink_ctx_deinit(&g_sink);
    TEST_ASSERT_TRUE(safe_cleanup(exec, 10000));
}

/* ---- EXE-PROTO-02: wrong terminal_kind rejected ---- */
TEST_CASE("executor: wrong terminal kind rejected", "[executor][exe]")
{
    fake_adapter_init(&g_adapter, g_adapter_ctx);
    g_cfg.step_timeout_default_ms = 10000;
    g_cfg.reset_timeout_ms = 1000;
    g_cfg.cancel_timeout_ms = 500;
    g_cfg.terminal_retry_ms = 100;
    wash_executor_t *exec = wash_executor_create(&g_cfg, &g_adapter);
    TEST_ASSERT_NOT_NULL(exec);

    sink_ctx_init(&g_sink);
    wash_executor_set_terminal_sink(exec, test_terminal_sink, &g_sink);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_start(exec));

    g_prog = make_simple_program(1);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(exec, &g_prog, 1));

    TEST_ASSERT_TRUE(fake_adapter_wait_dispatch(g_adapter_ctx, 1, 2000));
    const fake_dispatch_record_t *d0 = fake_adapter_get_dispatch(g_adapter_ctx, 0);
    TEST_ASSERT_NOT_NULL(d0);

    /* Cancel */
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_urgent(exec, false));
    TEST_ASSERT_TRUE(fake_adapter_wait_cancel(g_adapter_ctx, 1, 2000));
    const terminal_token_t *cancel_tok = fake_adapter_get_cancel_token(g_adapter_ctx, 0);
    TEST_ASSERT_NOT_NULL(cancel_tok);

    /* Send wrong terminal_kind (NORMAL instead of CANCEL_ACK) */
    terminal_token_t wrong = *cancel_tok;
    wrong.terminal_kind = TERMINAL_KIND_NORMAL;
    TEST_ASSERT_EQUAL(ESP_OK,
        wash_executor_publish_terminal(exec, &wrong, sizeof(wrong), true, 0));
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Should still be canceling — wrong kind rejected */
    wash_exec_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_get_snapshot(exec, &snap));
    TEST_ASSERT_TRUE(snap.program_active);

    /* Send correct CANCEL_ACK */
    terminal_token_t correct = *cancel_tok;
    TEST_ASSERT_EQUAL(ESP_OK,
        wash_executor_publish_terminal(exec, &correct, sizeof(correct), true, 0));

    /* Now cancel ACK accepted → reset dispatched */
    TEST_ASSERT_TRUE(fake_adapter_wait_reset(g_adapter_ctx, 1, 2000));

    /* Send wrong RESET kind (NORMAL instead of RESET_ACK) */
    const terminal_token_t *reset_tok = fake_adapter_get_reset_token(g_adapter_ctx, 0);
    terminal_token_t wrong_reset = *reset_tok;
    wrong_reset.terminal_kind = TERMINAL_KIND_NORMAL;
    TEST_ASSERT_EQUAL(ESP_OK,
        wash_executor_publish_terminal(exec, &wrong_reset, sizeof(wrong_reset), true, 0));
    vTaskDelay(pdMS_TO_TICKS(100));
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_get_snapshot(exec, &snap));
    TEST_ASSERT_TRUE(snap.program_active);  /* still resetting */

    /* Correct RESET_ACK */
    terminal_token_t correct_reset = *reset_tok;
    TEST_ASSERT_EQUAL(ESP_OK,
        wash_executor_publish_terminal(exec, &correct_reset, sizeof(correct_reset), true, 0));

    /* Now should complete with CANCELED */
    EventBits_t bits = xEventGroupWaitBits(g_sink.received, 1, pdTRUE, pdFALSE,
                                            pdMS_TO_TICKS(5000));
    TEST_ASSERT_TRUE(bits & 1);
    xSemaphoreTake(g_sink.mutex, portMAX_DELAY);
    TEST_ASSERT_EQUAL(TERM_RESULT_CANCELED, g_sink.last_terminal.result);
    xSemaphoreGive(g_sink.mutex);

    sink_ctx_deinit(&g_sink);
    TEST_ASSERT_TRUE(safe_cleanup(exec, 10000));
    ESP_LOGI(TAG, "EXPECTED_FAULT_INJECTION_RECOVERED: step_timeout");
}

/* Force linker to include this TU */
void wash_executor_tests_force_link(void) {}

/* ==== EXE-W-CANCEL-01: cancel ACK success → reset → CANCELED ==== */
TEST_CASE("cancel ACK success triggers reset", "[executor][exe][cancel]")
{
    fake_adapter_init(&g_adapter, g_adapter_ctx);
    g_cfg.step_timeout_default_ms = 10000;
    g_cfg.reset_timeout_ms = 1000;
    g_cfg.cancel_timeout_ms = 500;
    g_cfg.terminal_retry_ms = 100;
    wash_executor_t *exec = wash_executor_create(&g_cfg, &g_adapter);
    TEST_ASSERT_NOT_NULL(exec);
    sink_ctx_init(&g_sink);
    wash_executor_set_terminal_sink(exec, test_terminal_sink, &g_sink);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_start(exec));

    g_prog = make_simple_program(1);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(exec, &g_prog, 1));
    TEST_ASSERT_TRUE(fake_adapter_wait_dispatch(g_adapter_ctx, 1, 2000));

    /* Cancel */
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_urgent(exec, false));
    TEST_ASSERT_TRUE(fake_adapter_wait_cancel(g_adapter_ctx, 1, 2000));

    /* ACK success */
    const terminal_token_t *ct = fake_adapter_get_cancel_token(g_adapter_ctx, 0);
    terminal_token_t ack = *ct;
    TEST_ASSERT_EQUAL(ESP_OK,
        wash_executor_publish_terminal(exec, &ack, sizeof(ack), true, 0));

    /* Should dispatch reset */
    TEST_ASSERT_TRUE(fake_adapter_wait_reset(g_adapter_ctx, 1, 2000));
    const terminal_token_t *rt = fake_adapter_get_reset_token(g_adapter_ctx, 0);
    terminal_token_t rack = *rt;
    TEST_ASSERT_EQUAL(ESP_OK,
        wash_executor_publish_terminal(exec, &rack, sizeof(rack), true, 0));

    /* CANCELED terminal */
    EventBits_t bits = xEventGroupWaitBits(g_sink.received, 1, pdTRUE, pdFALSE,
                                            pdMS_TO_TICKS(5000));
    TEST_ASSERT_TRUE(bits & 1);
    xSemaphoreTake(g_sink.mutex, portMAX_DELAY);
    TEST_ASSERT_EQUAL(TERM_RESULT_CANCELED, g_sink.last_terminal.result);
    xSemaphoreGive(g_sink.mutex);

    sink_ctx_deinit(&g_sink);
    TEST_ASSERT_TRUE(safe_cleanup(exec, 10000));
}

/* ==== EXE-W-CANCEL-02: cancel ACK failure → FAULT, no reset ==== */
TEST_CASE("cancel ACK failure goes to FAULT no reset", "[executor][exe][cancel]")
{
    fake_adapter_init(&g_adapter, g_adapter_ctx);
    g_cfg.step_timeout_default_ms = 10000;
    g_cfg.reset_timeout_ms = 1000;
    g_cfg.cancel_timeout_ms = 500;
    g_cfg.terminal_retry_ms = 100;
    wash_executor_t *exec = wash_executor_create(&g_cfg, &g_adapter);
    TEST_ASSERT_NOT_NULL(exec);
    sink_ctx_init(&g_sink);
    wash_executor_set_terminal_sink(exec, test_terminal_sink, &g_sink);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_start(exec));

    g_prog = make_simple_program(1);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(exec, &g_prog, 1));
    TEST_ASSERT_TRUE(fake_adapter_wait_dispatch(g_adapter_ctx, 1, 2000));

    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_urgent(exec, false));
    TEST_ASSERT_TRUE(fake_adapter_wait_cancel(g_adapter_ctx, 1, 2000));

    /* ACK failure */
    const terminal_token_t *ct = fake_adapter_get_cancel_token(g_adapter_ctx, 0);
    terminal_token_t nack = *ct;
    TEST_ASSERT_EQUAL(ESP_OK,
        wash_executor_publish_terminal(exec, &nack, sizeof(nack), false, 310));

    /* Should NOT dispatch reset */
    vTaskDelay(pdMS_TO_TICKS(500));
    TEST_ASSERT_EQUAL(0u, fake_adapter_reset_count(g_adapter_ctx));

    /* Should get FAULT terminal */
    EventBits_t bits = xEventGroupWaitBits(g_sink.received, 1, pdTRUE, pdFALSE,
                                            pdMS_TO_TICKS(5000));
    TEST_ASSERT_TRUE(bits & 1);
    xSemaphoreTake(g_sink.mutex, portMAX_DELAY);
    TEST_ASSERT_EQUAL(TERM_RESULT_FAULT, g_sink.last_terminal.result);
    TEST_ASSERT_EQUAL(0, g_sink.last_terminal.final_position_confirmed);
    xSemaphoreGive(g_sink.mutex);

    sink_ctx_deinit(&g_sink);
    TEST_ASSERT_TRUE(safe_cleanup(exec, 10000));
}

/* ==== EXE-W-CANCEL-03: cancel timeout → FAULT, no reset ==== */
TEST_CASE("cancel timeout goes to FAULT no reset", "[executor][exe][cancel]")
{
    ESP_LOGI(TAG, "EXPECTED_FAULT_INJECTION_BEGIN: cancel_timeout");
    fake_adapter_init(&g_adapter, g_adapter_ctx);
    g_cfg.step_timeout_default_ms = 10000;
    g_cfg.reset_timeout_ms = 1000;
    g_cfg.cancel_timeout_ms = 200;  /* short cancel timeout */
    g_cfg.terminal_retry_ms = 100;
    wash_executor_t *exec = wash_executor_create(&g_cfg, &g_adapter);
    TEST_ASSERT_NOT_NULL(exec);
    sink_ctx_init(&g_sink);
    wash_executor_set_terminal_sink(exec, test_terminal_sink, &g_sink);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_start(exec));

    g_prog = make_simple_program(1);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(exec, &g_prog, 1));
    TEST_ASSERT_TRUE(fake_adapter_wait_dispatch(g_adapter_ctx, 1, 2000));

    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_urgent(exec, false));
    TEST_ASSERT_TRUE(fake_adapter_wait_cancel(g_adapter_ctx, 1, 2000));

    /* Don't send cancel ACK — let timeout fire */
    vTaskDelay(pdMS_TO_TICKS(600));

    /* Should NOT dispatch reset */
    TEST_ASSERT_EQUAL(0u, fake_adapter_reset_count(g_adapter_ctx));

    /* Should get FAULT terminal */
    EventBits_t bits = xEventGroupWaitBits(g_sink.received, 1, pdTRUE, pdFALSE,
                                            pdMS_TO_TICKS(5000));
    TEST_ASSERT_TRUE(bits & 1);
    xSemaphoreTake(g_sink.mutex, portMAX_DELAY);
    TEST_ASSERT_EQUAL(TERM_RESULT_FAULT, g_sink.last_terminal.result);
    xSemaphoreGive(g_sink.mutex);

    sink_ctx_deinit(&g_sink);
    TEST_ASSERT_TRUE(safe_cleanup(exec, 10000));
    ESP_LOGI(TAG, "EXPECTED_FAULT_INJECTION_RECOVERED: cancel_timeout");
}

/* ==== EXE-W-CANCEL-04: wrong cancel token → no state change ==== */
TEST_CASE("wrong cancel token rejected", "[executor][exe][cancel]")
{
    fake_adapter_init(&g_adapter, g_adapter_ctx);
    g_cfg.step_timeout_default_ms = 10000;
    g_cfg.reset_timeout_ms = 1000;
    g_cfg.cancel_timeout_ms = 500;
    g_cfg.terminal_retry_ms = 100;
    wash_executor_t *exec = wash_executor_create(&g_cfg, &g_adapter);
    TEST_ASSERT_NOT_NULL(exec);
    sink_ctx_init(&g_sink);
    wash_executor_set_terminal_sink(exec, test_terminal_sink, &g_sink);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_start(exec));

    g_prog = make_simple_program(1);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(exec, &g_prog, 1));
    TEST_ASSERT_TRUE(fake_adapter_wait_dispatch(g_adapter_ctx, 1, 2000));

    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_urgent(exec, false));
    TEST_ASSERT_TRUE(fake_adapter_wait_cancel(g_adapter_ctx, 1, 2000));

    /* Send wrong token (different program_id) */
    terminal_token_t wrong;
    memset(&wrong, 0, sizeof(wrong));
    wrong.program_id = 999;
    wrong.terminal_kind = TERMINAL_KIND_CANCEL_ACK;
    TEST_ASSERT_EQUAL(ESP_OK,
        wash_executor_publish_terminal(exec, &wrong, sizeof(wrong), true, 0));
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Should still be in CANCELING — no reset, no fault */
    wash_exec_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_get_snapshot(exec, &snap));
    TEST_ASSERT_TRUE(snap.program_active);
    TEST_ASSERT_EQUAL(0u, fake_adapter_reset_count(g_adapter_ctx));

    /* Send correct ACK */
    const terminal_token_t *ct = fake_adapter_get_cancel_token(g_adapter_ctx, 0);
    terminal_token_t ack = *ct;
    TEST_ASSERT_EQUAL(ESP_OK,
        wash_executor_publish_terminal(exec, &ack, sizeof(ack), true, 0));

    /* Now should proceed to reset */
    TEST_ASSERT_TRUE(fake_adapter_wait_reset(g_adapter_ctx, 1, 2000));
    const terminal_token_t *rt = fake_adapter_get_reset_token(g_adapter_ctx, 0);
    terminal_token_t rack = *rt;
    TEST_ASSERT_EQUAL(ESP_OK,
        wash_executor_publish_terminal(exec, &rack, sizeof(rack), true, 0));

    xEventGroupWaitBits(g_sink.received, 1, pdTRUE, pdFALSE, pdMS_TO_TICKS(5000));
    xSemaphoreTake(g_sink.mutex, portMAX_DELAY);
    TEST_ASSERT_EQUAL(TERM_RESULT_CANCELED, g_sink.last_terminal.result);
    xSemaphoreGive(g_sink.mutex);

    sink_ctx_deinit(&g_sink);
    TEST_ASSERT_TRUE(safe_cleanup(exec, 10000));
}

/* ==== EXE-W-STOP-01: active stop sends cancel ==== */
TEST_CASE("active stop sends cancel", "[executor][exe][stop]")
{
    fake_adapter_init(&g_adapter, g_adapter_ctx);
    g_cfg.step_timeout_default_ms = 10000;
    g_cfg.reset_timeout_ms = 1000;
    g_cfg.cancel_timeout_ms = 500;
    g_cfg.terminal_retry_ms = 100;
    wash_executor_t *exec = wash_executor_create(&g_cfg, &g_adapter);
    TEST_ASSERT_NOT_NULL(exec);
    sink_ctx_init(&g_sink);
    wash_executor_set_terminal_sink(exec, test_terminal_sink, &g_sink);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_start(exec));

    g_prog = make_simple_program(1);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(exec, &g_prog, 1));
    TEST_ASSERT_TRUE(fake_adapter_wait_dispatch(g_adapter_ctx, 1, 2000));

    /* Stop should send cancel, wait for ACK, reset, ACK, terminal */
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_stop(exec, 10000));

    /* Verify cancel was dispatched */
    TEST_ASSERT_GREATER_THAN(0u, fake_adapter_cancel_count(g_adapter_ctx));

    /* Verify terminal was delivered */
    xSemaphoreTake(g_sink.mutex, portMAX_DELAY);
    TEST_ASSERT_GREATER_THAN(0u, g_sink.call_count);
    xSemaphoreGive(g_sink.mutex);

    sink_ctx_deinit(&g_sink);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_destroy(exec));
}

/* ==== EXE-W-STOP-06: second stop idempotent ==== */
TEST_CASE("second stop idempotent fast", "[executor][exe][stop]")
{
    fake_adapter_init(&g_adapter, g_adapter_ctx);
    g_cfg.step_timeout_default_ms = 5000;
    g_cfg.reset_timeout_ms = 1000;
    g_cfg.cancel_timeout_ms = 500;
    g_cfg.terminal_retry_ms = 100;
    wash_executor_t *exec = wash_executor_create(&g_cfg, &g_adapter);
    TEST_ASSERT_NOT_NULL(exec);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_start(exec));

    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_stop(exec, 2000));

    int64_t t0 = esp_timer_get_time();
    esp_err_t r = wash_executor_stop(exec, 2000);
    int64_t elapsed = esp_timer_get_time() - t0;

    TEST_ASSERT_EQUAL(ESP_OK, r);
    TEST_ASSERT_LESS_THAN(100000, elapsed);  /* < 100ms */

    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_destroy(exec));
}

/* ==== EXE-W-STOP-08: destroy before drain rejected ==== */
TEST_CASE("destroy before stop rejected", "[executor][exe][stop]")
{
    ESP_LOGI(TAG, "EXPECTED_FAULT_INJECTION_BEGIN: destroy_before_stop");
    fake_adapter_init(&g_adapter, g_adapter_ctx);
    g_cfg.step_timeout_default_ms = 5000;
    g_cfg.reset_timeout_ms = 1000;
    g_cfg.cancel_timeout_ms = 500;
    g_cfg.terminal_retry_ms = 100;
    wash_executor_t *exec = wash_executor_create(&g_cfg, &g_adapter);
    TEST_ASSERT_NOT_NULL(exec);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_start(exec));

    /* Destroy without stop should fail */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, wash_executor_destroy(exec));

    /* Stop then destroy succeeds */
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_stop(exec, 2000));
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_destroy(exec));
    ESP_LOGI(TAG, "EXPECTED_FAULT_INJECTION_RECOVERED: destroy_before_stop");
}

/* ==== EXE-W-START-01: start succeeds ==== */
TEST_CASE("start succeeds quickly", "[executor][exe][start]")
{
    fake_adapter_init(&g_adapter, g_adapter_ctx);
    g_cfg.step_timeout_default_ms = 5000;
    g_cfg.reset_timeout_ms = 1000;
    g_cfg.cancel_timeout_ms = 500;
    g_cfg.terminal_retry_ms = 100;
    wash_executor_t *exec = wash_executor_create(&g_cfg, &g_adapter);
    TEST_ASSERT_NOT_NULL(exec);

    int64_t t0 = esp_timer_get_time();
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_start(exec));
    int64_t elapsed = esp_timer_get_time() - t0;
    TEST_ASSERT_LESS_THAN(2000000, elapsed);  /* < 2s */

    TEST_ASSERT_TRUE(safe_cleanup(exec, 10000));
}

/* ==== EXE-W-PUMP-01: multi-stage dispatch chaining ==== */
TEST_CASE("multi stage dispatch chaining", "[executor][exe][pump]")
{
    fake_adapter_init(&g_adapter, g_adapter_ctx);
    g_cfg.step_timeout_default_ms = 10000;
    g_cfg.reset_timeout_ms = 1000;
    g_cfg.cancel_timeout_ms = 500;
    g_cfg.terminal_retry_ms = 100;
    wash_executor_t *exec = wash_executor_create(&g_cfg, &g_adapter);
    TEST_ASSERT_NOT_NULL(exec);
    sink_ctx_init(&g_sink);
    wash_executor_set_terminal_sink(exec, test_terminal_sink, &g_sink);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_start(exec));

    /* 3-step program: MOVE → WATER → FINISH */
    g_prog = make_simple_program(1);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(exec, &g_prog, 1));

    /* Progress through all steps */
    for (int i = 0; i < 2; i++) {
        TEST_ASSERT_TRUE(fake_adapter_wait_dispatch(g_adapter_ctx, i + 1, 3000));
        const fake_dispatch_record_t *rec = fake_adapter_get_dispatch(g_adapter_ctx, i);
        terminal_token_t tok = rec->token;
        TEST_ASSERT_EQUAL(ESP_OK,
            wash_executor_publish_terminal(exec, &tok, sizeof(tok), true, 0));
    }

    /* FINISH auto-completes */
    EventBits_t bits = xEventGroupWaitBits(g_sink.received, 1, pdTRUE, pdFALSE,
                                            pdMS_TO_TICKS(5000));
    TEST_ASSERT_TRUE(bits & 1);
    xSemaphoreTake(g_sink.mutex, portMAX_DELAY);
    TEST_ASSERT_EQUAL(TERM_RESULT_COMPLETE, g_sink.last_terminal.result);
    TEST_ASSERT_EQUAL(3u, g_sink.last_terminal.completed_steps);
    xSemaphoreGive(g_sink.mutex);

    /* Verify all dispatches recorded */
    TEST_ASSERT_EQUAL(2u, fake_adapter_dispatch_count(g_adapter_ctx));
    TEST_ASSERT_EQUAL(STEP_MOVE_POSITION,
        fake_adapter_dispatch_step_type(g_adapter_ctx, 0));
    TEST_ASSERT_EQUAL(STEP_WATER_IN,
        fake_adapter_dispatch_step_type(g_adapter_ctx, 1));

    sink_ctx_deinit(&g_sink);
    TEST_ASSERT_TRUE(safe_cleanup(exec, 10000));
}

/* ==== EXE-W-PUMP-02: dispatch reject → FAULT terminal ==== */
TEST_CASE("dispatch reject fault terminal via pump", "[executor][exe][pump]")
{
    fake_adapter_init(&g_adapter, g_adapter_ctx);
    fake_adapter_set_next_accept(g_adapter_ctx, false);
    fake_adapter_set_next_error(g_adapter_ctx, 404);
    g_cfg.step_timeout_default_ms = 5000;
    g_cfg.reset_timeout_ms = 1000;
    g_cfg.cancel_timeout_ms = 500;
    g_cfg.terminal_retry_ms = 100;
    wash_executor_t *exec = wash_executor_create(&g_cfg, &g_adapter);
    TEST_ASSERT_NOT_NULL(exec);
    sink_ctx_init(&g_sink);
    wash_executor_set_terminal_sink(exec, test_terminal_sink, &g_sink);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_start(exec));

    g_prog = make_simple_program(1);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(exec, &g_prog, 1));

    EventBits_t bits = xEventGroupWaitBits(g_sink.received, 1, pdTRUE, pdFALSE,
                                            pdMS_TO_TICKS(5000));
    TEST_ASSERT_TRUE(bits & 1);
    xSemaphoreTake(g_sink.mutex, portMAX_DELAY);
    TEST_ASSERT_EQUAL(TERM_RESULT_FAULT, g_sink.last_terminal.result);
    xSemaphoreGive(g_sink.mutex);

    sink_ctx_deinit(&g_sink);
    TEST_ASSERT_TRUE(safe_cleanup(exec, 10000));
}

/* ---- EXE-PROTO-03: dispatch reject → FAULT terminal ---- */
TEST_CASE("executor: dispatch reject fault terminal", "[executor][exe]")
{
    fake_adapter_init(&g_adapter, g_adapter_ctx);
    fake_adapter_set_next_accept(g_adapter_ctx, false);
    fake_adapter_set_next_error(g_adapter_ctx, 404);

    g_cfg.step_timeout_default_ms = 5000;
    g_cfg.reset_timeout_ms = 1000;
    g_cfg.cancel_timeout_ms = 500;
    g_cfg.terminal_retry_ms = 100;
    wash_executor_t *exec = wash_executor_create(&g_cfg, &g_adapter);
    TEST_ASSERT_NOT_NULL(exec);

    sink_ctx_init(&g_sink);
    wash_executor_set_terminal_sink(exec, test_terminal_sink, &g_sink);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_start(exec));

    g_prog = make_simple_program(1);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(exec, &g_prog, 1));

    /* Dispatch rejected → FAULT terminal */
    EventBits_t bits = xEventGroupWaitBits(g_sink.received, 1, pdTRUE, pdFALSE,
                                            pdMS_TO_TICKS(5000));
    TEST_ASSERT_TRUE(bits & 1);
    xSemaphoreTake(g_sink.mutex, portMAX_DELAY);
    TEST_ASSERT_EQUAL(TERM_RESULT_FAULT, g_sink.last_terminal.result);
    TEST_ASSERT_EQUAL(1u, g_sink.last_terminal.program_id);
    xSemaphoreGive(g_sink.mutex);

    sink_ctx_deinit(&g_sink);
    TEST_ASSERT_TRUE(safe_cleanup(exec, 10000));
}

/* ---- EXE-PROTO-04: multi-program sequential ---- */
TEST_CASE("executor: multi program sequential", "[executor][exe]")
{
    fake_adapter_init(&g_adapter, g_adapter_ctx);
    g_cfg.step_timeout_default_ms = 10000;
    g_cfg.reset_timeout_ms = 1000;
    g_cfg.cancel_timeout_ms = 500;
    g_cfg.terminal_retry_ms = 100;
    wash_executor_t *exec = wash_executor_create(&g_cfg, &g_adapter);
    TEST_ASSERT_NOT_NULL(exec);

    sink_ctx_init(&g_sink);
    wash_executor_set_terminal_sink(exec, test_terminal_sink, &g_sink);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_start(exec));

    /* Program 1 */
    wash_program_t prog1 = make_simple_program(10);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(exec, &prog1, 10));
    for (int i = 0; i < 2; i++) {
        TEST_ASSERT_TRUE(fake_adapter_wait_dispatch(g_adapter_ctx, i + 1, 2000));
        const fake_dispatch_record_t *rec = fake_adapter_get_dispatch(g_adapter_ctx, i);
        terminal_token_t tok = rec->token;
        TEST_ASSERT_EQUAL(ESP_OK,
            wash_executor_publish_terminal(exec, &tok, sizeof(tok), true, 0));
    }
    xEventGroupWaitBits(g_sink.received, 1, pdTRUE, pdFALSE, pdMS_TO_TICKS(5000));
    xSemaphoreTake(g_sink.mutex, portMAX_DELAY);
    TEST_ASSERT_EQUAL(10u, g_sink.last_terminal.program_id);
    xSemaphoreGive(g_sink.mutex);

    /* Wait for reservation to clear */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Program 2 */
    wash_program_t prog2 = make_simple_program(20);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(exec, &prog2, 20));
    for (int i = 2; i < 4; i++) {
        TEST_ASSERT_TRUE(fake_adapter_wait_dispatch(g_adapter_ctx, i + 1, 2000));
        const fake_dispatch_record_t *rec = fake_adapter_get_dispatch(g_adapter_ctx, i);
        terminal_token_t tok = rec->token;
        TEST_ASSERT_EQUAL(ESP_OK,
            wash_executor_publish_terminal(exec, &tok, sizeof(tok), true, 0));
    }
    xEventGroupWaitBits(g_sink.received, 1, pdTRUE, pdFALSE, pdMS_TO_TICKS(5000));
    xSemaphoreTake(g_sink.mutex, portMAX_DELAY);
    TEST_ASSERT_EQUAL(20u, g_sink.last_terminal.program_id);
    xSemaphoreGive(g_sink.mutex);

    TEST_ASSERT_EQUAL(4u, fake_adapter_dispatch_count(g_adapter_ctx));
    TEST_ASSERT_EQUAL(2u, g_sink.call_count);

    sink_ctx_deinit(&g_sink);
    TEST_ASSERT_TRUE(safe_cleanup(exec, 10000));
}

/* ---- EXE-PROTO-05: duplicate terminal rejected ---- */
TEST_CASE("executor: duplicate terminal rejected", "[executor][exe]")
{
    fake_adapter_init(&g_adapter, g_adapter_ctx);
    g_cfg.step_timeout_default_ms = 10000;
    g_cfg.reset_timeout_ms = 1000;
    g_cfg.cancel_timeout_ms = 500;
    g_cfg.terminal_retry_ms = 100;
    wash_executor_t *exec = wash_executor_create(&g_cfg, &g_adapter);
    TEST_ASSERT_NOT_NULL(exec);

    sink_ctx_init(&g_sink);
    wash_executor_set_terminal_sink(exec, test_terminal_sink, &g_sink);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_start(exec));

    g_prog = make_simple_program(1);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(exec, &g_prog, 1));

    /* Process first step */
    TEST_ASSERT_TRUE(fake_adapter_wait_dispatch(g_adapter_ctx, 1, 2000));
    const fake_dispatch_record_t *d0 = fake_adapter_get_dispatch(g_adapter_ctx, 0);
    terminal_token_t tok = d0->token;

    /* First terminal — should advance */
    TEST_ASSERT_EQUAL(ESP_OK,
        wash_executor_publish_terminal(exec, &tok, sizeof(tok), true, 0));

    /* Wait for second dispatch (step 1) */
    TEST_ASSERT_TRUE(fake_adapter_wait_dispatch(g_adapter_ctx, 2, 2000));

    /* Duplicate terminal for step 0 — should be rejected (stale token) */
    TEST_ASSERT_EQUAL(ESP_OK,
        wash_executor_publish_terminal(exec, &tok, sizeof(tok), true, 0));
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Should only have 2 dispatches, not 3 */
    TEST_ASSERT_EQUAL(2u, fake_adapter_dispatch_count(g_adapter_ctx));

    sink_ctx_deinit(&g_sink);
    TEST_ASSERT_TRUE(safe_cleanup(exec, 10000));
}

/* ---- EXE-PROTO-06: step timeout triggers cancel flow ---- */
TEST_CASE("executor: step timeout triggers cancel", "[executor][exe]")
{
    ESP_LOGI(TAG, "EXPECTED_FAULT_INJECTION_BEGIN: step_timeout");
    fake_adapter_init(&g_adapter, g_adapter_ctx);
    g_cfg.step_timeout_default_ms = 200;
    g_cfg.reset_timeout_ms = 1000;
    g_cfg.cancel_timeout_ms = 500;
    g_cfg.terminal_retry_ms = 100;
    wash_executor_t *exec = wash_executor_create(&g_cfg, &g_adapter);
    TEST_ASSERT_NOT_NULL(exec);

    sink_ctx_init(&g_sink);
    wash_executor_set_terminal_sink(exec, test_terminal_sink, &g_sink);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_start(exec));

    /* Program with 0 timeout → uses default (200ms) */
    g_prog = make_simple_program(1);
    g_prog.steps[0].timeout_ms = 0;
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_submit_program(exec, &g_prog, 1));

    /* Wait for dispatch */
    TEST_ASSERT_TRUE(fake_adapter_wait_dispatch(g_adapter_ctx, 1, 2000));

    /* Wait for timeout (200ms) + some margin */
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Cancel should have been dispatched */
    TEST_ASSERT_TRUE(fake_adapter_wait_cancel(g_adapter_ctx, 1, 2000));

    /* Complete cancel flow */
    const terminal_token_t *cancel_tok = fake_adapter_get_cancel_token(g_adapter_ctx, 0);
    terminal_token_t ack = *cancel_tok;
    TEST_ASSERT_EQUAL(ESP_OK,
        wash_executor_publish_terminal(exec, &ack, sizeof(ack), true, 0));

    TEST_ASSERT_TRUE(fake_adapter_wait_reset(g_adapter_ctx, 1, 2000));
    const terminal_token_t *reset_tok = fake_adapter_get_reset_token(g_adapter_ctx, 0);
    terminal_token_t rack = *reset_tok;
    TEST_ASSERT_EQUAL(ESP_OK,
        wash_executor_publish_terminal(exec, &rack, sizeof(rack), true, 0));

    EventBits_t bits = xEventGroupWaitBits(g_sink.received, 1, pdTRUE, pdFALSE,
                                            pdMS_TO_TICKS(5000));
    TEST_ASSERT_TRUE(bits & 1);
    xSemaphoreTake(g_sink.mutex, portMAX_DELAY);
    TEST_ASSERT_EQUAL(TERM_RESULT_CANCELED, g_sink.last_terminal.result);
    xSemaphoreGive(g_sink.mutex);

    sink_ctx_deinit(&g_sink);
    TEST_ASSERT_TRUE(safe_cleanup(exec, 10000));
}
