/*
 * real_hal.c — Real hardware HAL assembly
 * Creates xiaojing_hal_t from actual ESP-IDF drivers.
 *
 * Creation sequence: i2c_bus → mcp23017 → ibt2 → bl50 → fan →
 *                    flow → water → sht → audio → assemble.
 * Each step failure triggers reverse-order cleanup.
 */

#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include "real_hal.h"
#include "i2c_bus_manager.h"
#include "mcp23017_driver.h"
#include "ibt2_driver.h"
#include "bl50_driver.h"
#include "fan_driver.h"
#include "flow_meter.h"
#include "water_level_sensor.h"
#include "sht_sensor.h"
#include "paj7620_adapter.h"
#include "audio_driver.h"
#include "board_config.h"
#include "machine_types.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "real_hal";

struct real_hal_ctx {
    xiaojing_hal_t hal;
    i2c_bus_ctx_t *i2c_bus;
    mcp23017_handle_t *mcp;
    ibt2_handle_t *ibt2;
    bl50_handle_t *bl50;
    fan_handle_t *fan;
    flow_handle_t *flow;
    water_level_handle_t *water;
    sht_handle_t *sht;
    paj7620_handle_t *paj;
    audio_handle_t *audio;
    _Atomic bool audio_playback_canceled;
    SemaphoreHandle_t audio_playback_mutex;
};

/* ---- HAL function implementations ---- */

/* Static context pointer (same pattern as fake_hal) */
static real_hal_ctx_t *s_real_ctx = NULL;

static esp_err_t real_read_mcp(mcp_input_snapshot_t *out)
{
    if (!s_real_ctx || !out) return ESP_ERR_INVALID_STATE;
    return mcp23017_get_input_snapshot(s_real_ctx->mcp, out);
}

static esp_err_t real_set_safe_output(safe_output_t output, bool enable)
{
    if (!s_real_ctx) return ESP_ERR_INVALID_STATE;

    /* Map safe_output_t enum to MCP port/pin */
    mcp_port_t port;
    uint8_t pin;
    switch (output) {
    case SAFE_OUTPUT_TAP_VALVE:     port = MCP_PORT_A; pin = 0; break;
    case SAFE_OUTPUT_TRANSFER_VALVE: port = MCP_PORT_A; pin = 1; break;
    case SAFE_OUTPUT_UV:            port = MCP_PORT_A; pin = 2; break;
    case SAFE_OUTPUT_DRAIN_VALVE:   port = MCP_PORT_A; pin = 3; break;
    case SAFE_OUTPUT_PTC_HEATER:    port = MCP_PORT_A; pin = 7; break;
    case SAFE_OUTPUT_DETERGENT_PUMP: port = MCP_PORT_B; pin = 7; break;
    default: return ESP_ERR_INVALID_ARG;
    }
    return mcp23017_set_output(s_real_ctx->mcp, port, pin, enable);
}

static esp_err_t real_set_ibt2(ibt2_command_t command)
{
    if (!s_real_ctx) return ESP_ERR_INVALID_STATE;
    switch (command.command) {
    case IBT2_CMD_STOP:
        return ibt2_driver_stop(s_real_ctx->ibt2);
    case IBT2_CMD_CW:
        return ibt2_driver_run_cw(s_real_ctx->ibt2, command.pwm_percent);
    case IBT2_CMD_CCW:
        return ibt2_driver_run_ccw(s_real_ctx->ibt2, command.pwm_percent);
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

static esp_err_t real_stop_ibt2(void)
{
    if (!s_real_ctx) return ESP_ERR_INVALID_STATE;
    return ibt2_driver_stop(s_real_ctx->ibt2);
}

static esp_err_t real_set_bl50(bl50_command_t command)
{
    if (!s_real_ctx) return ESP_ERR_INVALID_STATE;
    switch (command.command) {
    case BL50_CMD_STOP:
        return bl50_driver_stop(s_real_ctx->bl50);
    case BL50_CMD_RUN_CW:
        return bl50_driver_run_cw(s_real_ctx->bl50, command.pwm_percent);
    case BL50_CMD_RUN_CCW:
        return bl50_driver_run_ccw(s_real_ctx->bl50, command.pwm_percent);
    case BL50_CMD_BRAKE:
        return bl50_driver_brake(s_real_ctx->bl50);
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

static esp_err_t real_stop_bl50(void)
{
    if (!s_real_ctx) return ESP_ERR_INVALID_STATE;
    return bl50_driver_stop(s_real_ctx->bl50);
}

static esp_err_t real_set_fan(uint8_t percent)
{
    if (!s_real_ctx) return ESP_ERR_INVALID_STATE;
    return fan_driver_set_percent(s_real_ctx->fan, percent);
}

static esp_err_t real_read_water_level(bool *full)
{
    if (!s_real_ctx || !full) return ESP_ERR_INVALID_STATE;
    return water_level_sensor_read(s_real_ctx->water, full);
}

static esp_err_t real_read_sht(sht_sample_t *out)
{
    if (!s_real_ctx || !out) return ESP_ERR_INVALID_STATE;
    return sht_sensor_read(s_real_ctx->sht, out);
}

static esp_err_t real_read_flow(flow_snapshot_t *out)
{
    if (!s_real_ctx || !out) return ESP_ERR_INVALID_STATE;
    return flow_meter_read(s_real_ctx->flow, out);
}

static esp_err_t real_reset_flow(void)
{
    if (!s_real_ctx) return ESP_ERR_INVALID_STATE;
    return flow_meter_reset(s_real_ctx->flow);
}

static int64_t real_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static esp_err_t real_emergency_shutdown(void)
{
    if (!s_real_ctx) return ESP_ERR_INVALID_STATE;

    esp_err_t first_err = ESP_OK;

    /* 1. Close ALL MCP outputs (valves, UV, drain, pump, PTC OFF).
     *    PTC OFF is always allowed even if PTC was never enabled.
     *    Best-effort: continue even if one I2C write fails. */
    if (s_real_ctx->mcp) {
        esp_err_t err = mcp23017_close_all_outputs(s_real_ctx->mcp);
        if (err != ESP_OK && first_err == ESP_OK) {
            first_err = err;
        }
    }

    /* 2. Stop all motors */
    if (s_real_ctx->ibt2) {
        esp_err_t err = ibt2_driver_stop(s_real_ctx->ibt2);
        if (err != ESP_OK && first_err == ESP_OK) first_err = err;
    }
    if (s_real_ctx->bl50) {
        esp_err_t err = bl50_driver_stop(s_real_ctx->bl50);
        if (err != ESP_OK && first_err == ESP_OK) first_err = err;
    }

    /* 3. Stop fan */
    if (s_real_ctx->fan) {
        esp_err_t err = fan_driver_set_percent(s_real_ctx->fan, 0);
        if (err != ESP_OK && first_err == ESP_OK) first_err = err;
    }

    /* 4. Mute audio */
    if (s_real_ctx->audio) {
        esp_err_t err = audio_driver_disable_output(s_real_ctx->audio);
        if (err != ESP_OK && first_err == ESP_OK) first_err = err;
    }

    if (first_err == ESP_OK) {
        ESP_LOGW(TAG, "Emergency shutdown: all outputs closed");
    } else {
        ESP_LOGE(TAG, "Emergency shutdown: some outputs failed (first err=0x%x)",
                 first_err);
    }
    return first_err;
}

/* ---- Button HAL (delegates to MCP) ---- */

#define BTN_PIN_MASK  0x70  /* GPA4/5/6 */

static esp_err_t real_configure_btn_irq(void)
{
    if (!s_real_ctx || !s_real_ctx->mcp) return ESP_ERR_INVALID_STATE;
    return mcp23017_configure_button_interrupts(s_real_ctx->mcp, BTN_PIN_MASK, true);
}

static esp_err_t real_disable_btn_irq(void)
{
    if (!s_real_ctx || !s_real_ctx->mcp) return ESP_ERR_INVALID_STATE;
    return mcp23017_disable_button_interrupts(s_real_ctx->mcp, BTN_PIN_MASK);
}

static esp_err_t real_wait_btn_irq(uint32_t timeout_ms)
{
    if (!s_real_ctx || !s_real_ctx->mcp) return ESP_ERR_INVALID_STATE;
    return mcp23017_wait_interrupt(s_real_ctx->mcp, timeout_ms);
}

static esp_err_t real_read_btn_gpio(button_gpio_snapshot_t *out)
{
    if (!s_real_ctx || !s_real_ctx->mcp || !out) return ESP_ERR_INVALID_STATE;
    uint8_t gpio_a = 0, gpio_b = 0;
    esp_err_t err = mcp23017_read_gpio(s_real_ctx->mcp, &gpio_a, &gpio_b);
    if (err != ESP_OK) return err;
    out->gpio_a = gpio_a;
    out->intcap_a = 0;  /* Not needed for debounce */
    out->timestamp_ms = real_now_ms();
    return ESP_OK;
}

static esp_err_t real_read_gesture(hal_gesture_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = HAL_GESTURE_NONE;
    if (!s_real_ctx || !s_real_ctx->paj) return ESP_ERR_NOT_SUPPORTED;
    paj_gesture_event_t event;
    esp_err_t err = paj7620_read_gesture(s_real_ctx->paj, &event);
    if (err != ESP_OK || !event.valid) return err;
    switch (event.gesture) {
    case PAJ_GESTURE_RIGHT:      *out = HAL_GESTURE_RIGHT; break;
    case PAJ_GESTURE_LEFT:       *out = HAL_GESTURE_LEFT; break;
    case PAJ_GESTURE_UP:         *out = HAL_GESTURE_UP; break;
    case PAJ_GESTURE_DOWN:       *out = HAL_GESTURE_DOWN; break;
    case PAJ_GESTURE_FORWARD:    *out = HAL_GESTURE_FORWARD; break;
    case PAJ_GESTURE_BACKWARD:   *out = HAL_GESTURE_BACKWARD; break;
    case PAJ_GESTURE_CLOCKWISE:  *out = HAL_GESTURE_CLOCKWISE; break;
    case PAJ_GESTURE_COUNTER_CW: *out = HAL_GESTURE_COUNTER_CW; break;
    default: break;
    }
    return ESP_OK;
}

static esp_err_t real_audio_start_capture(void)
{
    if (!s_real_ctx || !s_real_ctx->audio) return ESP_ERR_INVALID_STATE;
    return audio_driver_start_rx(s_real_ctx->audio);
}

static esp_err_t real_audio_stop_capture(void)
{
    if (!s_real_ctx || !s_real_ctx->audio) return ESP_ERR_INVALID_STATE;
    return audio_driver_stop_rx(s_real_ctx->audio);
}

static esp_err_t real_audio_read_pcm(void *buffer, size_t size,
                                     size_t *bytes_read,
                                     uint32_t timeout_ms)
{
    if (!s_real_ctx || !s_real_ctx->audio) return ESP_ERR_INVALID_STATE;
    return audio_driver_read(
        s_real_ctx->audio, buffer, size, bytes_read, timeout_ms);
}

/* ---- Lifecycle ---- */

real_hal_ctx_t *real_hal_create(void)
{
    if (s_real_ctx) {
        ESP_LOGW(TAG, "Real HAL already created (singleton)");
        return NULL;
    }

    real_hal_ctx_t *ctx = calloc(1, sizeof(real_hal_ctx_t));
    if (!ctx) return NULL;
    atomic_init(&ctx->audio_playback_canceled, false);
    ctx->audio_playback_mutex = xSemaphoreCreateMutex();
    if (!ctx->audio_playback_mutex) {
        free(ctx);
        return NULL;
    }

    /* Step 1: I2C bus */
    i2c_bus_config_t i2c_cfg = {
        .sda_pin = XIAOJING_GPIO_I2C_SDA,
        .scl_pin = XIAOJING_GPIO_I2C_SCL,
        .clk_speed_hz = 100000,
        .max_retries = 3,
        .retry_delay_ms = 10,
        .xfer_timeout_ms = 100,
        .internal_pullup = true,
    };
    esp_err_t err = i2c_bus_manager_init(&i2c_cfg, &ctx->i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: 0x%x", err);
        goto fail;
    }

    /* Step 2: MCP23017 */
    mcp23017_config_t mcp_cfg = MCP23017_DEFAULT_CONFIG();
    ctx->mcp = mcp23017_init(ctx->i2c_bus, &mcp_cfg);
    if (!ctx->mcp) {
        ESP_LOGE(TAG, "MCP23017 init failed");
        goto fail;
    }

    /* Step 3: IBT-2 */
    ibt2_driver_config_t ibt2_cfg = IBT2_DEFAULT_CONFIG();
    /* Direction calibration loaded from machine_config in Phase 3+ */
    ctx->ibt2 = ibt2_driver_init(&ibt2_cfg);
    if (!ctx->ibt2) {
        ESP_LOGE(TAG, "IBT-2 init failed");
        goto fail;
    }

    /* Step 4: BL50 */
    ctx->bl50 = bl50_driver_init();
    if (!ctx->bl50) {
        ESP_LOGE(TAG, "BL50 init failed");
        goto fail;
    }

    /* Step 5: Fan */
    ctx->fan = fan_driver_init();
    if (!ctx->fan) {
        ESP_LOGE(TAG, "Fan init failed");
        goto fail;
    }

    /* Step 6: Flow meter */
    ctx->flow = flow_meter_init();
    if (!ctx->flow) {
        ESP_LOGE(TAG, "Flow meter init failed");
        goto fail;
    }

    /* Step 7: Water level */
    ctx->water = water_level_sensor_init();
    if (!ctx->water) {
        ESP_LOGE(TAG, "Water level init failed");
        goto fail;
    }

    /* Step 8: SHT */
    if (false) {
        ESP_LOGW(TAG, "SHT30 not found — continuing without it");
        /* Non-fatal: SHT may not be connected during development */
    }

    /* Step 9: Audio. PAJ7620 is intentionally deferred until after BLE and
     * display startup because an early optional probe fragments the scarce
     * contiguous internal heap required by the BLE controller. */
    audio_config_t audio_cfg = AUDIO_DEFAULT_CONFIG();
    ctx->audio = audio_driver_init(&audio_cfg);
    if (!ctx->audio) {
        ESP_LOGW(TAG, "Audio init failed — continuing without it");
        /* Non-fatal: audio not critical for washing operation */
    }

    /* Step 10: Assemble function table.
     * NOTE: PAJ7620 (gesture) and turbidity_sensor are NOT in xiaojing_hal_t.
     * They have no corresponding HAL function and are accessed directly as
     * independent components by their consumers (gesture service, diagnostics). */
    ctx->hal.read_mcp_inputs     = real_read_mcp;
    ctx->hal.set_safe_output     = real_set_safe_output;
    ctx->hal.set_ibt2            = real_set_ibt2;
    ctx->hal.stop_ibt2           = real_stop_ibt2;
    ctx->hal.set_bl50            = real_set_bl50;
    ctx->hal.stop_bl50           = real_stop_bl50;
    ctx->hal.set_fan_percent     = real_set_fan;
    ctx->hal.read_water_level    = real_read_water_level;
    ctx->hal.read_sht            = real_read_sht;
    ctx->hal.read_flow           = real_read_flow;
    ctx->hal.reset_flow_counter  = real_reset_flow;
    ctx->hal.now_ms              = real_now_ms;
    ctx->hal.emergency_shutdown  = real_emergency_shutdown;
    ctx->hal.configure_button_interrupts = real_configure_btn_irq;
    ctx->hal.disable_button_interrupts   = real_disable_btn_irq;
    ctx->hal.wait_button_interrupt       = real_wait_btn_irq;
    ctx->hal.read_button_gpio            = real_read_btn_gpio;
    ctx->hal.read_gesture                = real_read_gesture;
    ctx->hal.audio_start_capture         = real_audio_start_capture;
    ctx->hal.audio_stop_capture          = real_audio_stop_capture;
    ctx->hal.audio_read_pcm              = real_audio_read_pcm;

    s_real_ctx = ctx;
    ESP_LOGI(TAG, "Real HAL created successfully");
    return ctx;

fail:
    real_hal_destroy(ctx);
    return NULL;
}

const xiaojing_hal_t *real_hal_get_interface(real_hal_ctx_t *ctx)
{
    return ctx ? &ctx->hal : NULL;
}

bool real_hal_gesture_available(real_hal_ctx_t *ctx)
{
    return ctx && ctx->paj;
}

esp_err_t real_hal_start_gesture(real_hal_ctx_t *ctx)
{
    if (!ctx || !ctx->i2c_bus) return ESP_ERR_INVALID_ARG;
    if (ctx->paj) return ESP_OK;
    paj7620_config_t paj_cfg = PAJ7620_DEFAULT_CONFIG();
    ctx->paj = paj7620_init(ctx->i2c_bus, &paj_cfg);
    if (!ctx->paj) {
        ESP_LOGW(TAG, "PAJ7620 unavailable - gesture paging disabled");
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t real_hal_start_sht(real_hal_ctx_t *ctx)
{
    if (!ctx || !ctx->i2c_bus) return ESP_ERR_INVALID_ARG;
    if (ctx->sht) return ESP_OK;
    sht_sensor_config_t sht_cfg = SHT30_DEFAULT_CONFIG();
    ctx->sht = sht_sensor_init(ctx->i2c_bus, &sht_cfg);
    if (!ctx->sht) {
        ESP_LOGW(TAG,
                 "SHT30 unavailable - temperature safety remains fail-closed");
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t real_hal_set_ibt2_calibration(real_hal_ctx_t *ctx,
                                        bool calibrated, bool rpwm_is_cw)
{
    if (!ctx || !ctx->ibt2) return ESP_ERR_INVALID_STATE;
    return ibt2_driver_set_calibration(ctx->ibt2, calibrated, rpwm_is_cw);
}

esp_err_t real_hal_audio_play_tone(real_hal_ctx_t *ctx,
                                   uint16_t frequency_hz,
                                   uint16_t duration_ms,
                                   int16_t amplitude)
{
    if (!ctx || !ctx->audio || frequency_hz < 100U ||
        frequency_hz > 4000U || duration_ms == 0U ||
        duration_ms > 30000U || amplitude <= 0)
        return ESP_ERR_INVALID_ARG;

    if (!ctx->audio_playback_mutex ||
        xSemaphoreTake(ctx->audio_playback_mutex, pdMS_TO_TICKS(1000U)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    esp_err_t err = audio_driver_enable_output(ctx->audio);
    if (err != ESP_OK) {
        xSemaphoreGive(ctx->audio_playback_mutex);
        return err;
    }

    /* 16 kHz signed mono square wave, generated in bounded stack chunks. */
    int16_t pcm[160];
    uint32_t phase = 0U;
    const uint32_t phase_step = (uint32_t)frequency_hz * 2U;
    uint32_t samples_left = (uint32_t)duration_ms * 16U;
    size_t total_written = 0U;
    while (samples_left > 0U && err == ESP_OK) {
        size_t count = samples_left < 160U ? samples_left : 160U;
        for (size_t i = 0; i < count; i++) {
            pcm[i] = phase < 16000U ? amplitude : (int16_t)-amplitude;
            phase += phase_step;
            if (phase >= 32000U) phase -= 32000U;
        }
        size_t written = 0U;
        err = audio_driver_write(ctx->audio, pcm,
                                 count * sizeof(pcm[0]), &written, 100U);
        if (err == ESP_OK && written != count * sizeof(pcm[0]))
            err = ESP_ERR_TIMEOUT;
        total_written += written;
        samples_left -= (uint32_t)count;
    }
    ESP_LOGI(TAG, "tone %uHz/%ums: pcm_written=%u err=0x%x",
             (unsigned)frequency_hz, (unsigned)duration_ms,
             (unsigned)total_written, (unsigned)err);
    memset(pcm, 0, sizeof(pcm));
    esp_err_t stop_err = audio_driver_disable_output(ctx->audio);
    esp_err_t result = err != ESP_OK ? err : stop_err;
    xSemaphoreGive(ctx->audio_playback_mutex);
    return result;
}

esp_err_t real_hal_audio_play_pcm16(real_hal_ctx_t *ctx,
                                    const int16_t *pcm,
                                    size_t pcm_bytes,
                                    uint32_t sample_rate,
                                    uint32_t timeout_ms)
{
    if (!ctx || !ctx->audio || !pcm || pcm_bytes == 0U ||
        (pcm_bytes & 1U) != 0U ||
        (sample_rate != 16000U && sample_rate != 24000U) ||
        timeout_ms == 0U)
        return ESP_ERR_INVALID_ARG;

    if (!ctx->audio_playback_mutex ||
        xSemaphoreTake(ctx->audio_playback_mutex, pdMS_TO_TICKS(1000U)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    if (atomic_load_explicit(&ctx->audio_playback_canceled,
                             memory_order_acquire))
    {
        xSemaphoreGive(ctx->audio_playback_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = audio_driver_set_sample_rate(
        ctx->audio, (int)sample_rate);
    if (err == ESP_OK) err = audio_driver_enable_output(ctx->audio);
    size_t written = 0U;
    const size_t chunk_bytes = 320U;
    int64_t deadline_us = esp_timer_get_time() +
                          (int64_t)timeout_ms * 1000;
    while (err == ESP_OK && written < pcm_bytes) {
        if (atomic_load_explicit(&ctx->audio_playback_canceled,
                                 memory_order_acquire)) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        size_t amount = pcm_bytes - written;
        if (amount > chunk_bytes) amount = chunk_bytes;
        uint32_t chunk_timeout_ms = (uint32_t)(remaining_us / 1000);
        if (chunk_timeout_ms == 0U) chunk_timeout_ms = 1U;
        if (chunk_timeout_ms > 100U) chunk_timeout_ms = 100U;
        size_t chunk_written = 0U;
        err = audio_driver_write(ctx->audio,
                                 (const uint8_t *)pcm + written,
                                 amount, &chunk_written,
                                 chunk_timeout_ms);
        written += chunk_written;
        if (err == ESP_OK && chunk_written != amount)
            err = ESP_ERR_TIMEOUT;
    }
    esp_err_t stop_err = audio_driver_disable_output(ctx->audio);
    esp_err_t restore_err = audio_driver_set_sample_rate(ctx->audio, 16000);
    ESP_LOGI(TAG, "PCM16 playback rate=%u bytes=%u/%u err=0x%x",
             (unsigned)sample_rate, (unsigned)written,
             (unsigned)pcm_bytes, (unsigned)err);
    esp_err_t result = err != ESP_OK ? err :
                       (stop_err != ESP_OK ? stop_err : restore_err);
    xSemaphoreGive(ctx->audio_playback_mutex);
    return result;
}

esp_err_t real_hal_audio_prepare_playback(real_hal_ctx_t *ctx)
{
    if (!ctx || !ctx->audio) return ESP_ERR_INVALID_STATE;
    atomic_store_explicit(&ctx->audio_playback_canceled, false,
                          memory_order_release);
    return ESP_OK;
}

esp_err_t real_hal_audio_cancel_playback(real_hal_ctx_t *ctx)
{
    if (!ctx || !ctx->audio) return ESP_ERR_INVALID_STATE;
    atomic_store_explicit(&ctx->audio_playback_canceled, true,
                          memory_order_release);
    return ESP_OK;
}

void real_hal_destroy(real_hal_ctx_t *ctx)
{
    if (!ctx) return;

    atomic_store_explicit(&ctx->audio_playback_canceled, true,
                          memory_order_release);
    if (ctx->audio_playback_mutex)
        (void)xSemaphoreTake(ctx->audio_playback_mutex, portMAX_DELAY);

    /* Emergency stop: close all MCP outputs + stop motors + mute */
    if (ctx->mcp) mcp23017_close_all_outputs(ctx->mcp);
    if (ctx->ibt2) ibt2_driver_stop(ctx->ibt2);
    if (ctx->bl50) bl50_driver_stop(ctx->bl50);
    if (ctx->fan) fan_driver_set_percent(ctx->fan, 0);
    if (ctx->audio) audio_driver_disable_output(ctx->audio);

    /* Destroy in reverse order */
    if (ctx->audio) audio_driver_destroy(ctx->audio);
    if (ctx->paj) paj7620_destroy(ctx->paj);
    if (ctx->sht) sht_sensor_destroy(ctx->sht);
    if (ctx->water) water_level_sensor_destroy(ctx->water);
    if (ctx->flow) flow_meter_destroy(ctx->flow);
    if (ctx->fan) fan_driver_destroy(ctx->fan);
    if (ctx->bl50) bl50_driver_destroy(ctx->bl50);
    if (ctx->ibt2) ibt2_driver_destroy(ctx->ibt2);
    if (ctx->mcp) mcp23017_destroy(ctx->mcp);
    if (ctx->i2c_bus) i2c_bus_manager_destroy(ctx->i2c_bus);

    if (ctx->audio_playback_mutex) {
        xSemaphoreGive(ctx->audio_playback_mutex);
        vSemaphoreDelete(ctx->audio_playback_mutex);
        ctx->audio_playback_mutex = NULL;
    }

    if (s_real_ctx == ctx) s_real_ctx = NULL;
    free(ctx);
}

/* ---- Button HAL (delegates to MCP) ---- */
