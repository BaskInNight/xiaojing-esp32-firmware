/*
 * fake_hal.c — 可控测试 HAL 实现
 * 线程安全，支持虚拟时间、输入脚本、输出历史和故障注入
 * 单实例：fake_hal_create 拒绝第二个实例
 */

#include <string.h>
#include <stdatomic.h>
#include "fake_hal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* ---- 内部上下文 ---- */

struct fake_hal_ctx {
    SemaphoreHandle_t mutex;
    xiaojing_hal_t hal;

    /* 虚拟时间 */
    int64_t virtual_time_ms;
    bool realtime_clock_enabled;
    TickType_t realtime_origin_tick;
    int64_t realtime_origin_ms;

    /* 传感器输入 */
    mcp_input_snapshot_t mcp;
    bool water_full;
    flow_snapshot_t flow;
    sht_sample_t sht;

    /* 输出历史 */
    fake_output_record_t history[FAKE_HAL_HISTORY_CAPACITY];
    size_t history_head;    /* 下一个写入位置 */
    size_t history_count;   /* 当前条目数 */

    /* 脚本 */
    fake_script_entry_t script[FAKE_HAL_SCRIPT_CAPACITY];
    size_t script_count;
    size_t script_index;    /* 下一个待执行 */
    int64_t script_start_ms;

    /* 故障注入 */
    fake_fault_config_t faults;

    /* Button state - C11 atomic for thread-safe polling */
    uint8_t btn_gpio_a;           /* Current button GPIO state (under mutex) */
    _Atomic uint32_t btn_read_count; /* Number of GPIO reads (for test polling) */
    _Atomic uint32_t btn_state_gen;  /* Incremented on state change (for polling) */
};

/* ---- 内部辅助 ---- */

static void record_output(fake_hal_ctx_t *ctx, const fake_output_record_t *rec)
{
    ctx->history[ctx->history_head] = *rec;
    ctx->history_head = (ctx->history_head + 1) % FAKE_HAL_HISTORY_CAPACITY;
    if (ctx->history_count < FAKE_HAL_HISTORY_CAPACITY) {
        ctx->history_count++;
    }
}

/* 在 mutex 已持有时记录紧急停机 */
static void record_emergency_under_lock(fake_hal_ctx_t *ctx)
{
    fake_output_record_t rec = {
        .type = FAKE_OUTPUT_EMERGENCY_SHUTDOWN,
        .timestamp_ms = ctx->virtual_time_ms,
    };
    record_output(ctx, &rec);
}

/* ---- Context-based implementations ---- */

static fake_hal_ctx_t *g_fake_ctx = NULL; /* 单实例 */

static int64_t now_ms_locked(const fake_hal_ctx_t *ctx)
{
    if (!ctx->realtime_clock_enabled) return ctx->virtual_time_ms;
    TickType_t elapsed_ticks = xTaskGetTickCount() - ctx->realtime_origin_tick;
    return ctx->realtime_origin_ms
           + (int64_t)elapsed_ticks * (int64_t)portTICK_PERIOD_MS;
}

static esp_err_t ctx_read_mcp(mcp_input_snapshot_t *out)
{
    fake_hal_ctx_t *ctx = g_fake_ctx;
    if (!ctx || !out) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    esp_err_t fault = ctx->faults.mcp_read_error;
    if (fault != ESP_OK) {
        xSemaphoreGive(ctx->mutex);
        return fault;
    }
    *out = ctx->mcp;
    out->timestamp_ms = now_ms_locked(ctx);
    xSemaphoreGive(ctx->mutex);
    return ESP_OK;
}

static esp_err_t ctx_set_safe(safe_output_t output, bool enable)
{
    fake_hal_ctx_t *ctx = g_fake_ctx;
    if (!ctx) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    /* 故障注入: trigger_emergency_on_next_output */
    if (ctx->faults.trigger_emergency_on_next_output) {
        record_emergency_under_lock(ctx);
        ctx->faults.trigger_emergency_on_next_output = false;
        xSemaphoreGive(ctx->mutex);
        return ESP_OK;
    }
    esp_err_t fault = ctx->faults.mcp_write_error;
    if (fault != ESP_OK) {
        xSemaphoreGive(ctx->mutex);
        return fault;
    }
    fake_output_record_t rec = {
        .type = FAKE_OUTPUT_SET_SAFE,
        .timestamp_ms = ctx->virtual_time_ms,
    };
    rec.data.safe.output = output;
    rec.data.safe.enable = enable;
    record_output(ctx, &rec);
    xSemaphoreGive(ctx->mutex);
    return ESP_OK;
}

static esp_err_t ctx_set_ibt2(ibt2_command_t command)
{
    fake_hal_ctx_t *ctx = g_fake_ctx;
    if (!ctx) return ESP_ERR_INVALID_STATE;
    if (command.pwm_percent > 100) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    if (ctx->faults.trigger_emergency_on_next_output) {
        record_emergency_under_lock(ctx);
        ctx->faults.trigger_emergency_on_next_output = false;
        xSemaphoreGive(ctx->mutex);
        return ESP_OK;
    }
    esp_err_t fault = ctx->faults.ibt2_error;
    if (fault != ESP_OK) {
        xSemaphoreGive(ctx->mutex);
        return fault;
    }
    fake_output_record_t rec = {
        .type = FAKE_OUTPUT_SET_IBT2,
        .timestamp_ms = ctx->virtual_time_ms,
    };
    rec.data.ibt2.cmd = command;
    record_output(ctx, &rec);
    xSemaphoreGive(ctx->mutex);
    return ESP_OK;
}

static esp_err_t ctx_stop_ibt2(void)
{
    fake_hal_ctx_t *ctx = g_fake_ctx;
    if (!ctx) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    if (ctx->faults.trigger_emergency_on_next_output) {
        record_emergency_under_lock(ctx);
        ctx->faults.trigger_emergency_on_next_output = false;
        xSemaphoreGive(ctx->mutex);
        return ESP_OK;
    }
    esp_err_t fault = ctx->faults.stop_ibt2_error;
    if (fault != ESP_OK) {
        xSemaphoreGive(ctx->mutex);
        return fault;
    }
    fake_output_record_t rec = {
        .type = FAKE_OUTPUT_STOP_IBT2,
        .timestamp_ms = ctx->virtual_time_ms,
    };
    record_output(ctx, &rec);
    xSemaphoreGive(ctx->mutex);
    return ESP_OK;
}

static esp_err_t ctx_set_bl50(bl50_command_t command)
{
    fake_hal_ctx_t *ctx = g_fake_ctx;
    if (!ctx) return ESP_ERR_INVALID_STATE;
    if (command.pwm_percent > 100) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    if (ctx->faults.trigger_emergency_on_next_output) {
        record_emergency_under_lock(ctx);
        ctx->faults.trigger_emergency_on_next_output = false;
        xSemaphoreGive(ctx->mutex);
        return ESP_OK;
    }
    esp_err_t fault = ctx->faults.bl50_error;
    if (fault != ESP_OK) {
        xSemaphoreGive(ctx->mutex);
        return fault;
    }
    fake_output_record_t rec = {
        .type = FAKE_OUTPUT_SET_BL50,
        .timestamp_ms = ctx->virtual_time_ms,
    };
    rec.data.bl50.cmd = command;
    record_output(ctx, &rec);
    xSemaphoreGive(ctx->mutex);
    return ESP_OK;
}

static esp_err_t ctx_stop_bl50(void)
{
    fake_hal_ctx_t *ctx = g_fake_ctx;
    if (!ctx) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    if (ctx->faults.trigger_emergency_on_next_output) {
        record_emergency_under_lock(ctx);
        ctx->faults.trigger_emergency_on_next_output = false;
        xSemaphoreGive(ctx->mutex);
        return ESP_OK;
    }
    if (ctx->faults.stop_bl50_error != ESP_OK) {
        esp_err_t err = ctx->faults.stop_bl50_error;
        xSemaphoreGive(ctx->mutex);
        return err;
    }
    fake_output_record_t rec = {
        .type = FAKE_OUTPUT_STOP_BL50,
        .timestamp_ms = ctx->virtual_time_ms,
    };
    record_output(ctx, &rec);
    xSemaphoreGive(ctx->mutex);
    return ESP_OK;
}

static esp_err_t ctx_set_fan(uint8_t percent)
{
    fake_hal_ctx_t *ctx = g_fake_ctx;
    if (!ctx) return ESP_ERR_INVALID_STATE;
    if (percent > 100) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    if (ctx->faults.trigger_emergency_on_next_output) {
        record_emergency_under_lock(ctx);
        ctx->faults.trigger_emergency_on_next_output = false;
        xSemaphoreGive(ctx->mutex);
        return ESP_OK;
    }
    fake_output_record_t rec = {
        .type = FAKE_OUTPUT_SET_FAN,
        .timestamp_ms = ctx->virtual_time_ms,
    };
    rec.data.fan.percent = percent;
    record_output(ctx, &rec);
    xSemaphoreGive(ctx->mutex);
    return ESP_OK;
}

static esp_err_t ctx_read_water_level(bool *full)
{
    fake_hal_ctx_t *ctx = g_fake_ctx;
    if (!ctx || !full) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    esp_err_t fault = ctx->faults.water_level_error;
    if (fault != ESP_OK) {
        xSemaphoreGive(ctx->mutex);
        return fault;
    }
    *full = ctx->water_full;
    xSemaphoreGive(ctx->mutex);
    return ESP_OK;
}

static esp_err_t ctx_read_sht(sht_sample_t *out)
{
    fake_hal_ctx_t *ctx = g_fake_ctx;
    if (!ctx || !out) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    esp_err_t fault = ctx->faults.sht_error;
    if (fault != ESP_OK) {
        xSemaphoreGive(ctx->mutex);
        return fault;
    }
    *out = ctx->sht;
    out->age_ms = (uint32_t)(ctx->virtual_time_ms - out->timestamp_ms);
    xSemaphoreGive(ctx->mutex);
    return ESP_OK;
}

static esp_err_t ctx_read_flow(flow_snapshot_t *out)
{
    fake_hal_ctx_t *ctx = g_fake_ctx;
    if (!ctx || !out) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    esp_err_t fault = ctx->faults.flow_error;
    if (fault != ESP_OK) {
        xSemaphoreGive(ctx->mutex);
        return fault;
    }
    *out = ctx->flow;
    out->timestamp_ms = ctx->virtual_time_ms;
    xSemaphoreGive(ctx->mutex);
    return ESP_OK;
}

static esp_err_t ctx_reset_flow(void)
{
    fake_hal_ctx_t *ctx = g_fake_ctx;
    if (!ctx) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    ctx->flow.pulses = 0;
    ctx->flow.delta_pulses = 0;
    xSemaphoreGive(ctx->mutex);
    return ESP_OK;
}

static int64_t ctx_now_ms(void)
{
    fake_hal_ctx_t *ctx = g_fake_ctx;
    if (!ctx) return 0;

    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    int64_t t = now_ms_locked(ctx);
    xSemaphoreGive(ctx->mutex);
    return t;
}

static esp_err_t ctx_emergency_shutdown(void)
{
    fake_hal_ctx_t *ctx = g_fake_ctx;
    if (!ctx) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    record_emergency_under_lock(ctx);
    xSemaphoreGive(ctx->mutex);
    return ESP_OK;
}

static esp_err_t ctx_configure_btn_irq(void)
{
    fake_hal_ctx_t *ctx = g_fake_ctx;
    if (!ctx) return ESP_ERR_INVALID_STATE;
    /* No-op in fake HAL - button state is always available */
    return ESP_OK;
}

static esp_err_t ctx_disable_btn_irq(void)
{
    fake_hal_ctx_t *ctx = g_fake_ctx;
    if (!ctx) return ESP_ERR_INVALID_STATE;
    /* No-op in fake HAL - button state is always available */
    return ESP_OK;
}

static esp_err_t ctx_wait_btn_irq(uint32_t timeout_ms)
{
    fake_hal_ctx_t *ctx = g_fake_ctx;
    if (!ctx) return ESP_ERR_INVALID_STATE;

    /* Always wait the full timeout. The debounce engine needs the full
     * period to elapse before confirming a press/release. Returning early
     * on state change causes the debounce to reset instead of confirm. */
    if (timeout_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(timeout_ms));
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t ctx_read_btn_gpio(button_gpio_snapshot_t *out)
{
    fake_hal_ctx_t *ctx = g_fake_ctx;
    if (!ctx || !out) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    esp_err_t fault = ctx->faults.btn_read_error;
    if (fault != ESP_OK) {
        xSemaphoreGive(ctx->mutex);
        return fault;
    }
    out->gpio_a = ctx->btn_gpio_a;
    out->intcap_a = 0;
    out->timestamp_ms = ctx->virtual_time_ms;
    atomic_fetch_add_explicit(&ctx->btn_read_count, 1, memory_order_relaxed);
    xSemaphoreGive(ctx->mutex);
    return ESP_OK;
}

static esp_err_t ctx_read_gesture(hal_gesture_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = HAL_GESTURE_NONE;
    return ESP_OK;
}

/* ---- 公共 API ---- */

fake_hal_ctx_t *fake_hal_create(void)
{
    /* 单实例：拒绝第二个实例 */
    if (g_fake_ctx != NULL) {
        return NULL;
    }

    fake_hal_ctx_t *ctx = calloc(1, sizeof(fake_hal_ctx_t));
    if (!ctx) return NULL;

    ctx->mutex = xSemaphoreCreateMutex();
    if (!ctx->mutex) {
        free(ctx);
        return NULL;
    }

    /* C11 atomic initialization */
    atomic_init(&ctx->btn_read_count, 0);
    atomic_init(&ctx->btn_state_gen, 0);

    /* 默认 SHT 无效 */
    ctx->sht.valid = false;

    /* 装配 HAL 函数表 */
    ctx->hal.read_mcp_inputs     = ctx_read_mcp;
    ctx->hal.set_safe_output     = ctx_set_safe;
    ctx->hal.set_ibt2            = ctx_set_ibt2;
    ctx->hal.stop_ibt2           = ctx_stop_ibt2;
    ctx->hal.set_bl50            = ctx_set_bl50;
    ctx->hal.stop_bl50           = ctx_stop_bl50;
    ctx->hal.set_fan_percent     = ctx_set_fan;
    ctx->hal.read_water_level    = ctx_read_water_level;
    ctx->hal.read_sht            = ctx_read_sht;
    ctx->hal.read_flow           = ctx_read_flow;
    ctx->hal.reset_flow_counter  = ctx_reset_flow;
    ctx->hal.now_ms              = ctx_now_ms;
    ctx->hal.emergency_shutdown  = ctx_emergency_shutdown;
    ctx->hal.configure_button_interrupts = ctx_configure_btn_irq;
    ctx->hal.disable_button_interrupts   = ctx_disable_btn_irq;
    ctx->hal.wait_button_interrupt       = ctx_wait_btn_irq;
    ctx->hal.read_button_gpio            = ctx_read_btn_gpio;
    ctx->hal.read_gesture                = ctx_read_gesture;

    g_fake_ctx = ctx;
    return ctx;
}

const xiaojing_hal_t *fake_hal_get_interface(fake_hal_ctx_t *ctx)
{
    return ctx ? &ctx->hal : NULL;
}

void fake_hal_destroy(fake_hal_ctx_t *ctx)
{
    if (!ctx) return;
    if (g_fake_ctx == ctx) g_fake_ctx = NULL;
    if (ctx->mutex) vSemaphoreDelete(ctx->mutex);
    free(ctx);
}

fake_hal_ctx_t *fake_hal_get_singleton(void)
{
    return g_fake_ctx;
}

/* ---- 虚拟时间 ---- */

void fake_hal_set_time(fake_hal_ctx_t *ctx, int64_t time_ms)
{
    if (!ctx) return;
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    ctx->virtual_time_ms = time_ms;
    if (ctx->realtime_clock_enabled) {
        ctx->realtime_origin_ms = time_ms;
        ctx->realtime_origin_tick = xTaskGetTickCount();
    }
    xSemaphoreGive(ctx->mutex);
}

void fake_hal_enable_realtime_clock(fake_hal_ctx_t *ctx, int64_t start_ms)
{
    if (!ctx) return;
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    ctx->virtual_time_ms = start_ms;
    ctx->realtime_origin_ms = start_ms;
    ctx->realtime_origin_tick = xTaskGetTickCount();
    ctx->realtime_clock_enabled = true;
    xSemaphoreGive(ctx->mutex);
}

void fake_hal_advance_time(fake_hal_ctx_t *ctx, int64_t delta_ms)
{
    if (!ctx) return;
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    ctx->virtual_time_ms += delta_ms;
    if (ctx->realtime_clock_enabled) {
        ctx->realtime_origin_ms += delta_ms;
    }
    xSemaphoreGive(ctx->mutex);
}

int64_t fake_hal_get_time(fake_hal_ctx_t *ctx)
{
    if (!ctx) return 0;
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    int64_t t = now_ms_locked(ctx);
    xSemaphoreGive(ctx->mutex);
    return t;
}

/* ---- 输入控制 ---- */

void fake_hal_set_mcp(fake_hal_ctx_t *ctx, const mcp_input_snapshot_t *mcp)
{
    if (!ctx || !mcp) return;
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    ctx->mcp = *mcp;
    xSemaphoreGive(ctx->mutex);
}

void fake_hal_set_water_level(fake_hal_ctx_t *ctx, bool full)
{
    if (!ctx) return;
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    ctx->water_full = full;
    xSemaphoreGive(ctx->mutex);
}

void fake_hal_set_flow(fake_hal_ctx_t *ctx, const flow_snapshot_t *flow)
{
    if (!ctx || !flow) return;
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    ctx->flow = *flow;
    xSemaphoreGive(ctx->mutex);
}

void fake_hal_set_sht(fake_hal_ctx_t *ctx, const sht_sample_t *sht)
{
    if (!ctx || !sht) return;
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    ctx->sht = *sht;
    xSemaphoreGive(ctx->mutex);
}

void fake_hal_set_button_state(fake_hal_ctx_t *ctx, uint8_t gpio_a_mask)
{
    if (!ctx) return;
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    ctx->btn_gpio_a = gpio_a_mask;
    atomic_fetch_add_explicit(&ctx->btn_state_gen, 1, memory_order_release);
    xSemaphoreGive(ctx->mutex);
}

uint32_t fake_hal_get_btn_read_count(fake_hal_ctx_t *ctx)
{
    if (!ctx) return 0;
    return atomic_load_explicit(&ctx->btn_read_count, memory_order_acquire);
}

/* ---- 脚本系统 ---- */

esp_err_t fake_hal_load_script(fake_hal_ctx_t *ctx,
                                const fake_script_entry_t *entries,
                                size_t count)
{
    if (!ctx || !entries) return ESP_ERR_INVALID_ARG;
    if (count > FAKE_HAL_SCRIPT_CAPACITY) return ESP_ERR_NO_MEM;

    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    memcpy(ctx->script, entries, count * sizeof(fake_script_entry_t));
    ctx->script_count = count;
    ctx->script_index = 0;
    ctx->script_start_ms = ctx->virtual_time_ms;
    xSemaphoreGive(ctx->mutex);
    return ESP_OK;
}

void fake_hal_tick_script(fake_hal_ctx_t *ctx)
{
    if (!ctx) return;

    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    while (ctx->script_index < ctx->script_count) {
        fake_script_entry_t *entry = &ctx->script[ctx->script_index];
        int64_t trigger_at = ctx->script_start_ms + entry->trigger_after_ms;
        if (ctx->virtual_time_ms < trigger_at) {
            break;
        }
        switch (entry->type) {
        case FAKE_INPUT_MCP:
            ctx->mcp = entry->data.mcp;
            break;
        case FAKE_INPUT_WATER_LEVEL:
            ctx->water_full = entry->data.water_level.full;
            break;
        case FAKE_INPUT_FLOW:
            ctx->flow = entry->data.flow;
            break;
        case FAKE_INPUT_SHT:
            ctx->sht = entry->data.sht;
            break;
        }
        ctx->script_index++;
    }
    xSemaphoreGive(ctx->mutex);
}

void fake_hal_reset_script(fake_hal_ctx_t *ctx)
{
    if (!ctx) return;
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    ctx->script_index = 0;
    ctx->script_start_ms = ctx->virtual_time_ms;
    xSemaphoreGive(ctx->mutex);
}

/* ---- 输出历史 ---- */

size_t fake_hal_get_history_count(fake_hal_ctx_t *ctx)
{
    if (!ctx) return 0;
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    size_t count = ctx->history_count;
    xSemaphoreGive(ctx->mutex);
    return count;
}

size_t fake_hal_get_history(fake_hal_ctx_t *ctx,
                             fake_output_record_t *out,
                             size_t max_count)
{
    if (!ctx || !out || max_count == 0) return 0;

    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    size_t count = ctx->history_count;
    if (max_count < count) count = max_count;

    /* 从最旧的开始复制 */
    size_t start;
    if (ctx->history_count < FAKE_HAL_HISTORY_CAPACITY) {
        start = 0;
    } else {
        start = ctx->history_head; /* head 指向最旧的下一条 */
    }
    for (size_t i = 0; i < count; i++) {
        out[i] = ctx->history[(start + i) % FAKE_HAL_HISTORY_CAPACITY];
    }
    xSemaphoreGive(ctx->mutex);
    return count;
}

void fake_hal_clear_history(fake_hal_ctx_t *ctx)
{
    if (!ctx) return;
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    ctx->history_head = 0;
    ctx->history_count = 0;
    xSemaphoreGive(ctx->mutex);
}

size_t fake_hal_count_output_type(fake_hal_ctx_t *ctx, fake_output_type_t type)
{
    if (!ctx) return 0;

    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    size_t count = 0;
    size_t start;
    if (ctx->history_count < FAKE_HAL_HISTORY_CAPACITY) {
        start = 0;
    } else {
        start = ctx->history_head;
    }
    for (size_t i = 0; i < ctx->history_count; i++) {
        if (ctx->history[(start + i) % FAKE_HAL_HISTORY_CAPACITY].type == type) {
            count++;
        }
    }
    xSemaphoreGive(ctx->mutex);
    return count;
}

/* ---- 故障注入 ---- */

void fake_hal_set_faults(fake_hal_ctx_t *ctx, const fake_fault_config_t *faults)
{
    if (!ctx || !faults) return;
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    ctx->faults = *faults;
    xSemaphoreGive(ctx->mutex);
}

void fake_hal_clear_faults(fake_hal_ctx_t *ctx)
{
    if (!ctx) return;
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    memset(&ctx->faults, 0, sizeof(ctx->faults));
    xSemaphoreGive(ctx->mutex);
}

/* ---- 快照 ---- */

void fake_hal_get_snapshot(fake_hal_ctx_t *ctx, fake_hal_state_snapshot_t *out)
{
    if (!ctx || !out) return;
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    out->mcp = ctx->mcp;
    out->water_full = ctx->water_full;
    out->flow = ctx->flow;
    out->sht = ctx->sht;
    out->virtual_time_ms = now_ms_locked(ctx);
    xSemaphoreGive(ctx->mutex);
}
