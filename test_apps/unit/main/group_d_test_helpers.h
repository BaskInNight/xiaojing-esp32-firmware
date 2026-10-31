/*
 * group_d_test_helpers.h — Round 1.6 shared test infrastructure
 *
 * Thread-safe sinks with event history, MCP register mock with fault modes,
 * deterministic polling helpers. MCP owner tracking in group_d_test_support.c.
 * No TEST_ASSERT in cleanup paths.
 */

#pragma once

#include <string.h>
#include <stdatomic.h>
#include "unity.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "mcp23017_driver.h"
#include "button_service.h"
#include "button_debounce.h"
#include "group_d_test_support.h"
#include "fake_hal.h"

/* ---- Constants ---- */
#define BTN1_RAW_PRESSED  ((uint8_t)0xEF)
#define BTN2_RAW_PRESSED  ((uint8_t)0xDF)
#define BTN3_RAW_PRESSED  ((uint8_t)0xBF)
#define ALL_RELEASED       ((uint8_t)0xFF)

/* ================================================================
 * I2C fault mode enum
 * ================================================================ */

typedef enum {
    MCP_FAULT_NONE = 0,
    MCP_FAULT_NEXT,
    MCP_FAULT_ON_NTH,
    MCP_FAULT_NEXT_N,
    MCP_FAULT_UNTIL_CLEARED,
} mcp_fault_mode_t;

/* ================================================================
 * MCP23017 Register Mock Backend
 * Thread-safe: all register access protected by mcp_reg_mock_t.mtx.
 * ================================================================ */

#define MCP_REG_COUNT  22

typedef struct {
    uint8_t regs[MCP_REG_COUNT];
    SemaphoreHandle_t mtx;
    StaticSemaphore_t mtx_storage;
    SemaphoreHandle_t int_sem;
    StaticSemaphore_t int_sem_storage;
    bool has_int_sem;
    mcp_fault_mode_t read_fault_mode;
    mcp_fault_mode_t write_fault_mode;
    esp_err_t        read_fault_error;
    esp_err_t        write_fault_error;
    uint32_t         read_fault_param;
    uint32_t         write_fault_param;
    uint32_t         read_fault_succeed_count;
    uint32_t         write_fault_succeed_count;
    struct { uint8_t reg; bool is_write; uint8_t value; } trace[64];
    int trace_count;
    int trace_capacity;
    _Atomic uint32_t gpio_read_gen;
    _Atomic uint32_t read_attempt_count;
    _Atomic uint32_t write_attempt_count;
    _Atomic uint32_t failure_count;
    _Atomic uint32_t recovery_count;
} mcp_reg_mock_t;

static mcp_reg_mock_t s_mcp_mock;

static void mcp_mock_init(mcp_reg_mock_t *m, bool with_int_sem)
{
    memset(m, 0, sizeof(*m));
    m->trace_capacity = 64;
    m->mtx = xSemaphoreCreateMutexStatic(&m->mtx_storage);
    m->regs[0x00] = 0xFF;
    m->regs[0x01] = 0xFF;
    /* R1.7: C11 atomic_init after memset */
    atomic_init(&m->gpio_read_gen, 0);
    atomic_init(&m->read_attempt_count, 0);
    atomic_init(&m->write_attempt_count, 0);
    atomic_init(&m->failure_count, 0);
    atomic_init(&m->recovery_count, 0);
    if (with_int_sem) {
        m->int_sem = xSemaphoreCreateBinaryStatic(&m->int_sem_storage);
        m->has_int_sem = (m->int_sem != NULL);
    }
}

static void mcp_mock_reset_regs(mcp_reg_mock_t *m)
{
    xSemaphoreTake(m->mtx, portMAX_DELAY);
    memset(m->regs, 0, sizeof(m->regs));
    m->regs[0x00] = 0xFF;
    m->regs[0x01] = 0xFF;
    m->read_fault_mode = MCP_FAULT_NONE;
    m->write_fault_mode = MCP_FAULT_NONE;
    m->read_fault_error = ESP_OK;
    m->write_fault_error = ESP_OK;
    m->read_fault_param = 0;
    m->write_fault_param = 0;
    m->read_fault_succeed_count = 0;
    m->write_fault_succeed_count = 0;
    m->trace_count = 0;
    atomic_store(&m->gpio_read_gen, 0);
    atomic_store(&m->read_attempt_count, 0);
    atomic_store(&m->write_attempt_count, 0);
    atomic_store(&m->failure_count, 0);
    atomic_store(&m->recovery_count, 0);
    xSemaphoreGive(m->mtx);
}

static void mcp_mock_preset(mcp_reg_mock_t *m, uint8_t reg, uint8_t value)
{
    if (reg < MCP_REG_COUNT) {
        xSemaphoreTake(m->mtx, portMAX_DELAY);
        m->regs[reg] = value;
        xSemaphoreGive(m->mtx);
    }
}

static void mcp_mock_set_gpio(mcp_reg_mock_t *m, uint8_t gpio_a)
{
    mcp_mock_preset(m, 0x12, gpio_a);
}

static void mcp_mock_trigger_interrupt(mcp_reg_mock_t *m)
{
    if (m->has_int_sem && m->int_sem)
        xSemaphoreGive(m->int_sem);
}

static void mcp_mock_set_intfa(mcp_reg_mock_t *m, uint8_t flags)
{
    mcp_mock_preset(m, 0x0E, flags);
}

static void mcp_mock_set_read_fault(mcp_reg_mock_t *m, esp_err_t err,
                                     mcp_fault_mode_t mode, uint32_t param)
{
    xSemaphoreTake(m->mtx, portMAX_DELAY);
    m->read_fault_mode = mode;
    m->read_fault_error = err;
    m->read_fault_param = param;
    m->read_fault_succeed_count = 0;
    xSemaphoreGive(m->mtx);
}

static void mcp_mock_set_write_fault(mcp_reg_mock_t *m, esp_err_t err,
                                      mcp_fault_mode_t mode, uint32_t param)
{
    xSemaphoreTake(m->mtx, portMAX_DELAY);
    m->write_fault_mode = mode;
    m->write_fault_error = err;
    m->write_fault_param = param;
    m->write_fault_succeed_count = 0;
    xSemaphoreGive(m->mtx);
}

static void mcp_mock_set_fault(mcp_reg_mock_t *m, esp_err_t err, int countdown)
{
    if (countdown <= 0) {
        mcp_mock_set_read_fault(m, err, MCP_FAULT_NONE, 0);
        mcp_mock_set_write_fault(m, err, MCP_FAULT_NONE, 0);
    } else if (countdown == 1) {
        mcp_mock_set_read_fault(m, err, MCP_FAULT_NEXT, 1);
        mcp_mock_set_write_fault(m, err, MCP_FAULT_NEXT, 1);
    } else {
        mcp_mock_set_read_fault(m, err, MCP_FAULT_ON_NTH, (uint32_t)countdown);
        mcp_mock_set_write_fault(m, err, MCP_FAULT_ON_NTH, (uint32_t)countdown);
    }
}

static void mcp_mock_clear_fault(mcp_reg_mock_t *m)
{
    xSemaphoreTake(m->mtx, portMAX_DELAY);
    if (m->read_fault_mode != MCP_FAULT_NONE || m->write_fault_mode != MCP_FAULT_NONE)
        atomic_fetch_add(&m->recovery_count, 1);
    m->read_fault_mode = MCP_FAULT_NONE;
    m->write_fault_mode = MCP_FAULT_NONE;
    m->read_fault_error = ESP_OK;
    m->write_fault_error = ESP_OK;
    m->read_fault_param = 0;
    m->write_fault_param = 0;
    m->read_fault_succeed_count = 0;
    m->write_fault_succeed_count = 0;
    xSemaphoreGive(m->mtx);
}

static void mcp_mock_clear_trace(mcp_reg_mock_t *m)
{
    xSemaphoreTake(m->mtx, portMAX_DELAY);
    m->trace_count = 0;
    xSemaphoreGive(m->mtx);
}

static uint32_t mcp_mock_get_gen(mcp_reg_mock_t *m)
{
    return atomic_load(&m->gpio_read_gen);
}

static esp_err_t mcp_mock_eval_fault(mcp_reg_mock_t *m, bool is_write)
{
    mcp_fault_mode_t mode = is_write ? m->write_fault_mode : m->read_fault_mode;
    esp_err_t err = is_write ? m->write_fault_error : m->read_fault_error;
    uint32_t *param = is_write ? &m->write_fault_param : &m->read_fault_param;
    uint32_t *succ = is_write ? &m->write_fault_succeed_count : &m->read_fault_succeed_count;

    if (mode == MCP_FAULT_NONE) return ESP_OK;

    if (mode == MCP_FAULT_NEXT || mode == MCP_FAULT_NEXT_N) {
        if (*param > 0) {
            (*param)--;
            atomic_fetch_add(&m->failure_count, 1);
            if (*param == 0) {
                if (is_write) { m->write_fault_mode = MCP_FAULT_NONE; m->write_fault_error = ESP_OK; }
                else { m->read_fault_mode = MCP_FAULT_NONE; m->read_fault_error = ESP_OK; }
            }
            return err;
        }
        return ESP_OK;
    }

    if (mode == MCP_FAULT_ON_NTH) {
        (*succ)++;
        uint32_t n = is_write ? m->write_fault_param : m->read_fault_param;
        if (*succ == n) {
            atomic_fetch_add(&m->failure_count, 1);
            if (is_write) { m->write_fault_mode = MCP_FAULT_NONE; m->write_fault_error = ESP_OK; }
            else { m->read_fault_mode = MCP_FAULT_NONE; m->read_fault_error = ESP_OK; }
            return err;
        }
        return ESP_OK;
    }

    if (mode == MCP_FAULT_UNTIL_CLEARED) {
        atomic_fetch_add(&m->failure_count, 1);
        return err;
    }

    return ESP_OK;
}

static esp_err_t mcp_mock_fault_fn(uint8_t reg, bool is_write,
                                    uint8_t *data, void *user_data)
{
    mcp_reg_mock_t *m = (mcp_reg_mock_t *)user_data;
    xSemaphoreTake(m->mtx, portMAX_DELAY);

    if (m->trace_count < m->trace_capacity) {
        m->trace[m->trace_count].reg = reg;
        m->trace[m->trace_count].is_write = is_write;
        m->trace[m->trace_count].value = is_write ? *data :
                                          (reg < MCP_REG_COUNT ? m->regs[reg] : 0);
        m->trace_count++;
    }

    if (is_write) atomic_fetch_add(&m->write_attempt_count, 1);
    else atomic_fetch_add(&m->read_attempt_count, 1);

    esp_err_t fault = mcp_mock_eval_fault(m, is_write);
    if (fault != ESP_OK) {
        xSemaphoreGive(m->mtx);
        return fault;
    }

    if (is_write) {
        if (reg < MCP_REG_COUNT) m->regs[reg] = *data;
    } else {
        if (reg < MCP_REG_COUNT) {
            *data = m->regs[reg];
            if (reg == 0x12) atomic_fetch_add(&m->gpio_read_gen, 1);
            if (reg == 0x10) m->regs[0x0E] = 0;
            if (reg == 0x11) m->regs[0x0F] = 0;
        } else {
            *data = 0;
        }
    }

    xSemaphoreGive(m->mtx);
    return ESP_OK;
}

/* ================================================================
 * MCP creation — delegates to group_d_test_support for owner tracking
 * ================================================================ */

static mcp23017_handle_t *create_mock_mcp_with_int(void)
{
    mcp23017_handle_t *h = group_d_test_create_mock_mcp();
    if (!h) return NULL;
    mcp_mock_init(&s_mcp_mock, true);
    mcp23017_test_set_i2c_fault(h, mcp_mock_fault_fn, &s_mcp_mock);
    return h;
}

static mcp23017_handle_t *create_mock_mcp(void)
{
    return create_mock_mcp_with_int();
}

/* ================================================================
 * Thread-safe test sink with event history
 * ================================================================ */

typedef struct {
    EventGroupHandle_t eg;
    EventBits_t done_bit;
    _Atomic uint32_t count;
    _Atomic esp_err_t force_error;
    bool reentrant_check;
    bool reentrant_snapshot_ok;
    SemaphoreHandle_t mtx;
    StaticSemaphore_t mtx_storage;
    button_sink_event_t last_event;
    /* R1.6: event history for D33 etc. */
    button_sink_event_t history[GROUP_D_EVENT_HISTORY_CAP];
    int history_count;
    /* R1.8: callback barrier for deterministic test synchronization */
    SemaphoreHandle_t cb_barrier;
} test_sink_ctx_t;

static void test_sink_init(test_sink_ctx_t *s)
{
    memset(s, 0, sizeof(*s));
    s->mtx = xSemaphoreCreateMutexStatic(&s->mtx_storage);
    /* R1.7: C11 atomic_init after memset */
    atomic_init(&s->count, 0);
    atomic_init(&s->force_error, ESP_OK);
}

static uint32_t test_sink_get_count(test_sink_ctx_t *s)
{
    return atomic_load(&s->count);
}

static void test_sink_set_error(test_sink_ctx_t *s, esp_err_t err)
{
    atomic_store(&s->force_error, err);
}

static bool test_sink_get_last(test_sink_ctx_t *s, button_sink_event_t *out)
{
    bool ok = false;
    if (xSemaphoreTake(s->mtx, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (atomic_load(&s->count) > 0) {
            *out = s->last_event;
            ok = true;
        }
        xSemaphoreGive(s->mtx);
    }
    return ok;
}

/* Sink callback — records event, history, sets done bit */
static esp_err_t test_normal_sink_fn(const button_sink_event_t *ev,
                                      uint32_t timeout_ms, void *ctx)
{
    test_sink_ctx_t *s = (test_sink_ctx_t *)ctx;
    if (s->reentrant_check) {
        button_service_snapshot_t snap;
        esp_err_t snap_err = button_service_get_snapshot(&snap);
        if (snap_err == ESP_OK) s->reentrant_snapshot_ok = true;
    }
    esp_err_t err = atomic_load(&s->force_error);
    if (err != ESP_OK) return err;
    if (xSemaphoreTake(s->mtx, pdMS_TO_TICKS(10)) == pdTRUE) {
        s->last_event = *ev;
        if (s->history_count < GROUP_D_EVENT_HISTORY_CAP)
            s->history[s->history_count++] = *ev;
        xSemaphoreGive(s->mtx);
    }
    atomic_fetch_add(&s->count, 1);
    if (s->eg) xEventGroupSetBits(s->eg, s->done_bit);
    return ESP_OK;
}

static esp_err_t test_urgent_sink_fn(const button_sink_event_t *ev,
                                      uint32_t timeout_ms, void *ctx)
{
    test_sink_ctx_t *s = (test_sink_ctx_t *)ctx;
    if (s->reentrant_check) {
        button_service_snapshot_t snap;
        esp_err_t snap_err = button_service_get_snapshot(&snap);
        if (snap_err == ESP_OK) s->reentrant_snapshot_ok = true;
    }
    /* R1.8: Callback barrier — blocks until test releases.
     * Allows test to inject stale completions while callback is in-flight. */
    if (s->cb_barrier) {
        xSemaphoreTake(s->cb_barrier, portMAX_DELAY);
    }
    esp_err_t err = atomic_load(&s->force_error);
    if (err != ESP_OK) return err;
    if (xSemaphoreTake(s->mtx, pdMS_TO_TICKS(10)) == pdTRUE) {
        s->last_event = *ev;
        if (s->history_count < GROUP_D_EVENT_HISTORY_CAP)
            s->history[s->history_count++] = *ev;
        xSemaphoreGive(s->mtx);
    }
    atomic_fetch_add(&s->count, 1);
    if (s->eg) xEventGroupSetBits(s->eg, s->done_bit);
    return ESP_OK;
}

static button_service_config_t make_btn_config(
    const xiaojing_hal_t *hal,
    test_sink_ctx_t *normal, test_sink_ctx_t *urgent,
    uint32_t debounce_ms, uint32_t poll_ms)
{
    button_service_config_t cfg = {0};
    cfg.hal = hal;
    cfg.debounce_ms = debounce_ms;
    cfg.poll_interval_ms = poll_ms;
    cfg.normal_sink = (button_event_sink_t){test_normal_sink_fn, normal};
    cfg.urgent_sink = (button_urgent_sink_t){test_urgent_sink_fn, urgent};
    return cfg;
}

/* R1.6: setup checks poison, teardown saves error and poisons */
static esp_err_t btn_setup(void)
{
    if (group_d_test_is_poisoned()) {
        TEST_FAIL_MESSAGE("Isolation poisoned: previous test cleanup failed");
        return ESP_ERR_INVALID_STATE;
    }
    /* Destroy any leaked fake HAL singleton from a previous failed test.
     * Must stop button service first — it uses the HAL. */
    fake_hal_ctx_t *leaked = fake_hal_get_singleton();
    if (leaked) {
        esp_err_t err = group_d_test_global_cleanup();
        if (err != ESP_OK) {
            /* Service cleanup failed — HAL still in use, can't proceed */
            group_d_test_set_poisoned(true);
            TEST_FAIL_MESSAGE("Leaked singleton cleanup failed");
            return err;
        }
        fake_hal_destroy(leaked);
    }
    return ESP_OK;
}

static esp_err_t btn_teardown(void)
{
    /* 1. Stop button service FIRST — it uses the HAL */
    esp_err_t err = group_d_test_global_cleanup();
    if (err != ESP_OK) {
        /* Service cleanup failed — HAL may still be in use, don't destroy */
        group_d_test_set_poisoned(true);
        return err;
    }
    /* 2. Only destroy HAL after service confirmed stopped */
    fake_hal_destroy(fake_hal_get_singleton());
    return ESP_OK;
}

/* ================================================================
 * Deterministic polling helpers
 * ================================================================ */

typedef bool (*snapshot_pred_fn)(const button_service_snapshot_t *snap, void *ctx);

static bool wait_for_gpio_gen(mcp_reg_mock_t *m, uint32_t target, uint32_t timeout_ms)
{
    uint32_t t0 = xTaskGetTickCount() * portTICK_PERIOD_MS;
    while (mcp_mock_get_gen(m) < target) {
        vTaskDelay(pdMS_TO_TICKS(5));
        if ((xTaskGetTickCount() * portTICK_PERIOD_MS - t0) > timeout_ms)
            return false;
    }
    return true;
}

/* R1.8: wait_for_btn_read_count — deterministic polling via fake_hal read counter */
static bool wait_for_btn_read_count(fake_hal_ctx_t *fh, uint32_t target, uint32_t timeout_ms)
{
    uint32_t t0 = xTaskGetTickCount() * portTICK_PERIOD_MS;
    while (fake_hal_get_btn_read_count(fh) < target) {
        vTaskDelay(pdMS_TO_TICKS(5));
        if ((xTaskGetTickCount() * portTICK_PERIOD_MS - t0) > timeout_ms)
            return false;
    }
    return true;
}

static bool wait_for_normal_count(test_sink_ctx_t *s, uint32_t target, uint32_t timeout_ms)
{
    uint32_t t0 = xTaskGetTickCount() * portTICK_PERIOD_MS;
    while (test_sink_get_count(s) < target) {
        vTaskDelay(pdMS_TO_TICKS(5));
        if ((xTaskGetTickCount() * portTICK_PERIOD_MS - t0) > timeout_ms)
            return false;
    }
    return true;
}

static bool wait_for_urgent_count(test_sink_ctx_t *s, uint32_t target, uint32_t timeout_ms)
{
    uint32_t t0 = xTaskGetTickCount() * portTICK_PERIOD_MS;
    while (test_sink_get_count(s) < target) {
        vTaskDelay(pdMS_TO_TICKS(5));
        if ((xTaskGetTickCount() * portTICK_PERIOD_MS - t0) > timeout_ms)
            return false;
    }
    return true;
}

static bool wait_for_snapshot_predicate(snapshot_pred_fn pred, void *ctx, uint32_t timeout_ms)
{
    uint32_t t0 = xTaskGetTickCount() * portTICK_PERIOD_MS;
    button_service_snapshot_t snap;
    while (true) {
        if (button_service_get_snapshot(&snap) == ESP_OK && pred(&snap, ctx))
            return true;
        vTaskDelay(pdMS_TO_TICKS(5));
        if ((xTaskGetTickCount() * portTICK_PERIOD_MS - t0) > timeout_ms)
            return false;
    }
}

static bool wait_for_i2c_error_count(uint32_t target, uint32_t timeout_ms)
{
    uint32_t t0 = xTaskGetTickCount() * portTICK_PERIOD_MS;
    button_service_snapshot_t snap;
    while (true) {
        if (button_service_get_snapshot(&snap) == ESP_OK && snap.i2c_read_errors >= target)
            return true;
        vTaskDelay(pdMS_TO_TICKS(5));
        if ((xTaskGetTickCount() * portTICK_PERIOD_MS - t0) > timeout_ms)
            return false;
    }
}

static bool wait_for_join_commit_count(uint32_t target, uint32_t timeout_ms)
{
    uint32_t t0 = xTaskGetTickCount() * portTICK_PERIOD_MS;
    button_service_snapshot_t snap;
    while (true) {
        if (button_service_get_snapshot(&snap) == ESP_OK && snap.join_commit_count >= target)
            return true;
        vTaskDelay(pdMS_TO_TICKS(5));
        if ((xTaskGetTickCount() * portTICK_PERIOD_MS - t0) > timeout_ms)
            return false;
    }
}

static bool wait_for_task_done(uint32_t timeout_ms)
{
    uint32_t t0 = xTaskGetTickCount() * portTICK_PERIOD_MS;
    while (true) {
        if (button_service_test_is_task_done())
            return true;
        vTaskDelay(pdMS_TO_TICKS(5));
        if ((xTaskGetTickCount() * portTICK_PERIOD_MS - t0) > timeout_ms)
            return false;
    }
}
