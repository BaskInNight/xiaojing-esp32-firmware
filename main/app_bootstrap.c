/*
 * app_bootstrap.c — 安全初始化顺序 (Round 2.1)
 *
 * Four-phase shutdown: button→quiesce→services→executor
 * Button service wired to executor via HAL (not MCP)
 * FAKE mode: button_service enabled via fake HAL
 */

#include <stdio.h>
#include <inttypes.h>
#include <stdatomic.h>
#include "app_bootstrap.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "machine_config.h"
#include "machine_status_store.h"
#include "fake_hal.h"
#include "real_hal.h"
#include "board_config.h"
#include "position_service.h"
#include "position_swing.h"
#include "safety_manager.h"
#include "bl50_service.h"
#include "water_service.h"
#include "dry_service.h"
#include "coupled_hot_air_service.h"
#include "drain_service.h"
#include "detergent_service.h"
#include "uv_service.h"
#include "uv_self_test.h"
#include "actuator_self_test.h"
#include "motor_bench_service.h"
#include "button_service.h"
#include "program_control.h"
#include "program_control_core.h"
#include "wash_executor.h"
#include "wash_executor_bootstrap.h"
#include "voice_service.h"
#include "voice_cloud_adapter.h"
#include "voice_cloud_transport_idf.h"
#include "voice_tts_idf.h"
#include "voice_capture_idf.h"
#include "voice_provider.h"
#include "voice_prompt_audio.h"
#include "voice_credentials.h"
#include "voice_frontend.h"
#include "voice_wake_edge.h"
#include "voice_wake_esp_sr.h"
#include "ei_classifier_wrapper.h"
#include "audio_controller.h"
#include "voice_recorder.h"
#include "wifi_station.h"
#include "audio_driver.h"
#include "display_service.h"
#include "gesture_service.h"
#include "gesture_page_map.h"
#include "turbidity_sensor.h"
#include "app_protocol.h"
#include "ble_transport.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

#if defined(__has_include)
#if __has_include("voice_secrets.h")
#include "voice_secrets.h"
#endif
#endif

#ifndef XIAOJING_VOICE_PROVIDER
#define XIAOJING_VOICE_PROVIDER VOICE_PROVIDER_MIMO_DIRECT
#endif
#ifndef XIAOJING_VOICE_API_KEY
#define XIAOJING_VOICE_API_KEY ""
#endif
#ifndef XIAOJING_VOICE_MODEL
#define XIAOJING_VOICE_MODEL ""
#endif
#ifndef XIAOJING_VOICE_PRESET_REVISION
#define XIAOJING_VOICE_PRESET_REVISION 0U
#endif
#ifndef XIAOJING_DEFAULT_WIFI_SSID
#define XIAOJING_DEFAULT_WIFI_SSID ""
#endif
#ifndef XIAOJING_DEFAULT_WIFI_PASSWORD
#define XIAOJING_DEFAULT_WIFI_PASSWORD ""
#endif

/* Initial field constant from the installed flow-meter specification:
 * F[Hz] = 98 * Q[L/min], therefore 98 * 60 = 5880 pulses/L.  It is used
 * only for live telemetry when an older NVS config has not been calibrated. */
#define FLOW_TELEMETRY_REFERENCE_PPL 5880U

/* A/B/C test mode selection:
 * Define XIAOJING_TEST_MODE=1|2|3 in CMakeLists.txt or command line.
 * 1: BLE + Edge + WiFi (default)
 * 2: BLE + WiFi (no Edge)
 * 3: WiFi only (no BLE, no Edge)
 */
#ifndef XIAOJING_TEST_MODE
#define XIAOJING_TEST_MODE 1
#endif

/* Phase 9 unattended hardware exercise.  This is enabled only by the
 * dedicated overnight test build; normal firmware remains passive. */
#ifndef XIAOJING_PHASE9_AUTOTEST
#define XIAOJING_PHASE9_AUTOTEST 0
#endif

static const char *TAG = "bootstrap";

/* Forward declarations */
static void log_heap(const char *tag);

/* Diagnostic flags: set by compile-time test mode */
#if XIAOJING_TEST_MODE == 3
static bool s_diag_disable_ble = true;
static bool s_diag_disable_edge = true;
#elif XIAOJING_TEST_MODE == 2
static bool s_diag_disable_ble = false;
static bool s_diag_disable_edge = true;
#else /* 1 */
static bool s_diag_disable_ble = false;
static bool s_diag_disable_edge = false;
#endif

static fake_hal_ctx_t *s_fake_hal = NULL;
static real_hal_ctx_t *s_real_hal = NULL;
static const xiaojing_hal_t *s_hal = NULL;
static bool s_protocol_active = false;
static bool s_ble_active = false;
static bool s_wifi_active = false;
static bool s_voice_capture_active = false;
static bool s_voice_cloud_active = false;
static bool s_voice_service_active = false;
static bool s_display_active = false;
static bool s_gesture_active = false;
/* Telemetry-only ADC reader.  It is deliberately not part of xiaojing_hal_t:
 * no wash safety transition consumes an uncalibrated turbidity value. */
static turbidity_handle_t *s_turbidity_sensor = NULL;
static audio_handle_t *s_voice_audio = NULL;
static xiaojing_hal_t s_voice_audio_hal;
static voice_cloud_idf_context_t s_voice_cloud_context;
static voice_credentials_value_t s_effective_voice_credentials;
static _Atomic bool s_credential_restart_scheduled;
static _Atomic bool s_ble_connected;
static _Atomic int s_requested_wake_backend = VOICE_WAKE_BACKEND_EDGE_IMPULSE;
static _Atomic int s_display_wifi_applied;
static _Atomic int s_display_ble_applied;
/* Voice backend switch outcome pulse for the display (0=idle, 1=switching,
 * 2=success, 3=failure, 4=timeout).  Set by the switch path, consumed by
 * publish_display_telemetry. */
static _Atomic int s_backend_switch_result;
static _Atomic int s_backend_switch_target;
static StaticTimer_t s_connectivity_timer_storage;
static TimerHandle_t s_connectivity_timer;
static TaskHandle_t s_display_telemetry_task;
static StaticSemaphore_t s_display_telemetry_done_storage;
static SemaphoreHandle_t s_display_telemetry_done;
static _Atomic bool s_display_telemetry_stop;

#if XIAOJING_PHASE9_AUTOTEST
static void phase9_autotest_task(void *arg);
#endif

#define DISPLAY_TELEMETRY_STACK_SIZE 4096U
/* Keep telemetry responsive to physical inputs without turning the UI task
 * into a high-frequency renderer.  Water-level sampling itself is 50 ms. */
#define DISPLAY_TELEMETRY_PERIOD_MS  250U

/* TS-300B provisional transfer model.  Its analogue output is divided by
 * 1.5 before GPIO5.  This restores sensor-side voltage, normalizes it to the
 * vendor's 25 C reference with the approximate -10 mV/C slope, then applies
 * the vendor curve.  A future two-point liquid calibration replaces this. */
#define TURBIDITY_ADC_FULL_SCALE        4095.0f
#define TURBIDITY_ADC_REFERENCE_V       3.3f
#define TURBIDITY_DIVIDER_RESTORE       1.5f
#define TURBIDITY_REFERENCE_TEMP_C      25.0f
#define TURBIDITY_TEMP_COEFF_V_PER_C    0.010f

static float turbidity_estimate_ntu(int raw, float temperature_c,
                                    bool temperature_valid)
{
    float adc_voltage = ((float)raw * TURBIDITY_ADC_REFERENCE_V) /
                        TURBIDITY_ADC_FULL_SCALE;
    float sensor_voltage = adc_voltage * TURBIDITY_DIVIDER_RESTORE;
    float measured_temp = temperature_valid ? temperature_c
                                             : TURBIDITY_REFERENCE_TEMP_C;
    float voltage_25c = sensor_voltage +
        TURBIDITY_TEMP_COEFF_V_PER_C *
        (measured_temp - TURBIDITY_REFERENCE_TEMP_C);
    float estimate = -1120.4f * voltage_25c * voltage_25c +
                     5742.3f * voltage_25c - 4352.9f;
    if (estimate < 0.0f) return 0.0f;
    if (estimate > 1000.0f) return 1000.0f;
    return estimate;
}

#define APP_RUNTIME_NVS_NAMESPACE "xj_runtime"
#define APP_NVS_WAKE_BACKEND_KEY   "wake_backend"
#define APP_NVS_WIFI_ORIGIN_KEY    "wifi_origin"
#define APP_NVS_WIFI_PRESET_REV_KEY "wifi_preset"
#define APP_NVS_VOICE_PRESET_REV_KEY "voice_preset"
#define WIFI_ORIGIN_FIRMWARE       1U
#define WIFI_ORIGIN_USER           2U
#define WIFI_PRESET_REVISION       2U
#define WIFI_FORCE_SAME_SSID_REVISION 2U

/* Forward declaration — defined after task_complete_worker. */
static void poll_wash_completion(void);

/* Task completion presentation layer.
 * Uses a dedicated worker task + bounded queue instead of Timer callback
 * to avoid blocking the Timer Service with PCM playback. */
typedef struct {
    uint32_t program_id;
    uint32_t generation;
} task_complete_event_t;

#define TASK_COMPLETE_QUEUE_DEPTH 4
static QueueHandle_t s_task_complete_queue = NULL;
static TaskHandle_t s_task_complete_task = NULL;
static bool s_task_complete_task_running = false;

/* Exactly-once tracking: key = (program_id, generation). */
static uint32_t s_tc_last_program_id = 0;
static uint32_t s_tc_last_generation = 0;

static esp_err_t sync_display_connectivity(bool force)
{
    if (!s_display_active) return ESP_ERR_INVALID_STATE;
    bool wifi = s_wifi_active && xiaojing_wifi_is_connected();
    bool ble = atomic_load_explicit(&s_ble_connected, memory_order_acquire);
    int old_wifi = atomic_load_explicit(
        &s_display_wifi_applied, memory_order_acquire);
    int old_ble = atomic_load_explicit(
        &s_display_ble_applied, memory_order_acquire);
    /* The display model can be reset/rebuilt independently of bootstrap
     * (for example after an LCD resource recovery).  In that case the
     * bootstrap cache still contains the old value and a cache-only check
     * would leave the freshly-created screen stuck at OFFLINE.  Verify the
     * actual model before skipping the write. */
    if (!force && old_wifi == (int)wifi && old_ble == (int)ble) {
        if (display_service_connectivity_matches(wifi, ble))
            return ESP_OK;
    }
    esp_err_t err = display_service_set_connectivity(wifi, ble);
    if (err == ESP_OK) {
        atomic_store_explicit(&s_display_wifi_applied, (int)wifi,
                              memory_order_release);
        atomic_store_explicit(&s_display_ble_applied, (int)ble,
                              memory_order_release);
        ESP_LOGI(TAG, "Display connectivity applied: wifi=%u ble=%u",
                 (unsigned)wifi, (unsigned)ble);
    }
    return err;
}

static void connectivity_timer_callback(TimerHandle_t timer)
{
    (void)timer;
    esp_err_t err = sync_display_connectivity(false);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        ESP_LOGW(TAG, "Display connectivity reconcile failed: 0x%x", err);
    /* Poll wash completion — only publishes to queue, never plays audio. */
    poll_wash_completion();
    /* Refresh program control (executor snapshot → arbiter refresh). */
    (void)program_control_refresh();
}

/* ---- Diagnostic Task (non-blocking) ---- */

/* Atomic diagnostic snapshot - filled by various subsystems */
typedef struct {
    _Atomic uint32_t i2s_read_ok;
    _Atomic uint32_t i2s_bytes;
    _Atomic uint32_t pcm_to_wake;
    _Atomic uint32_t pcm_to_dialog;
    _Atomic uint32_t wake_events;
    _Atomic uint32_t dialog_terminals;
    _Atomic uint32_t ei_classifier_calls;
    _Atomic uint32_t ei_threshold_hits;
} diag_snapshot_t;

static diag_snapshot_t s_diag_snapshot = {0};
static TaskHandle_t s_diag_task_handle = NULL;
static SemaphoreHandle_t s_diag_done_sem = NULL;  /* TASK_DONE signal */
static StaticSemaphore_t s_diag_done_sem_storage;
static _Atomic bool s_diag_stop_requested = false;
static StaticTimer_t s_diag_notify_timer_storage;
static TimerHandle_t s_diag_notify_timer = NULL;
static SemaphoreHandle_t s_diag_lifecycle_lock = NULL;  /* Serializes start/stop */
static StaticSemaphore_t s_diag_lifecycle_lock_storage;

/* Forward declaration */
static void update_diag_snapshot(void);

/* Non-blocking timer callback - just notifies the diagnostic task */
static void diag_notify_timer_callback(TimerHandle_t timer)
{
    (void)timer;
    if (atomic_load_explicit(&s_diag_stop_requested, memory_order_acquire))
        return;
    /* Update snapshot from audio controller (fast, no mutex) */
    update_diag_snapshot();
    /* Notify diagnostic task to log */
    TaskHandle_t handle = s_diag_task_handle;
    if (handle) {
        xTaskNotifyGive(handle);
    }
}

static void diagnostic_task(void *arg)
{
    (void)arg;

    while (!atomic_load_explicit(&s_diag_stop_requested, memory_order_acquire)) {
        /* The periodic software timer is the sole cadence source.  A second
         * 60-second task timeout races the timer at the boundary and can emit
         * duplicate snapshots.  stop() also wakes this wait explicitly. */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (atomic_load_explicit(&s_diag_stop_requested, memory_order_acquire))
            break;

        /* Read atomic snapshots - no mutex needed */
        uint32_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        uint32_t int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        uint32_t i2s_ok = atomic_load_explicit(&s_diag_snapshot.i2s_read_ok, memory_order_relaxed);
        uint32_t i2s_bytes = atomic_load_explicit(&s_diag_snapshot.i2s_bytes, memory_order_relaxed);
        uint32_t wake_pcm = atomic_load_explicit(&s_diag_snapshot.pcm_to_wake, memory_order_relaxed);
        uint32_t dialog_pcm = atomic_load_explicit(&s_diag_snapshot.pcm_to_dialog, memory_order_relaxed);
        uint32_t wake_ev = atomic_load_explicit(&s_diag_snapshot.wake_events, memory_order_relaxed);
        uint32_t term_ev = atomic_load_explicit(&s_diag_snapshot.dialog_terminals, memory_order_relaxed);
        uint32_t ei_calls = atomic_load_explicit(&s_diag_snapshot.ei_classifier_calls, memory_order_relaxed);
        uint32_t ei_hits = atomic_load_explicit(&s_diag_snapshot.ei_threshold_hits, memory_order_relaxed);
        audio_state_t state = audio_controller_get_state();
        uint32_t gen = audio_controller_get_generation();

        /* Single ASCII log line */
        ESP_LOGI(TAG, "D: h=%"PRIu32" L=%"PRIu32
                 " s=%c g=%"PRIu32
                 " i2s=%"PRIu32" iB=%"PRIu32
                 " w=%"PRIu32" d=%"PRIu32
                 " we=%"PRIu32" te=%"PRIu32
                 " ei=%"PRIu32"/%"PRIu32,
                 int_free, int_largest,
                 state == AUDIO_STATE_WAKE ? 'W' :
                 state == AUDIO_STATE_DIALOG ? 'D' :
                 state == AUDIO_STATE_TRANSITION_TO_DIALOG ? 'T' :
                 state == AUDIO_STATE_TRANSITION_TO_WAKE ? 'R' : '?',
                 gen,
                 i2s_ok, i2s_bytes,
                 wake_pcm, dialog_pcm,
                 wake_ev, term_ev,
                 ei_calls, ei_hits);
    }

    /* Signal TASK_DONE before exiting */
    ESP_LOGI(TAG, "Diagnostic task exiting");
    if (s_diag_done_sem) {
        xSemaphoreGive(s_diag_done_sem);
    }
    vTaskDeleteWithCaps(NULL);
}

/* Update diagnostic snapshot from audio controller */
static void update_diag_snapshot(void)
{
    audio_controller_diag_t ac_diag;
    audio_controller_get_diag(&ac_diag);

    atomic_store_explicit(&s_diag_snapshot.i2s_read_ok, ac_diag.i2s_read_ok, memory_order_relaxed);
    atomic_store_explicit(&s_diag_snapshot.i2s_bytes, ac_diag.i2s_bytes, memory_order_relaxed);
    atomic_store_explicit(&s_diag_snapshot.pcm_to_wake, ac_diag.pcm_to_wake, memory_order_relaxed);
    atomic_store_explicit(&s_diag_snapshot.pcm_to_dialog, ac_diag.pcm_to_dialog, memory_order_relaxed);
    atomic_store_explicit(&s_diag_snapshot.wake_events, ac_diag.wake_events, memory_order_relaxed);
    atomic_store_explicit(&s_diag_snapshot.dialog_terminals, ac_diag.dialog_terminals, memory_order_relaxed);

    uint32_t ei_calls = 0, ei_hits = 0;
    ei_classifier_get_diagnostics(&ei_calls, &ei_hits, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    atomic_store_explicit(&s_diag_snapshot.ei_classifier_calls, ei_calls, memory_order_relaxed);
    atomic_store_explicit(&s_diag_snapshot.ei_threshold_hits, ei_hits, memory_order_relaxed);
}

static esp_err_t start_diagnostic_task(void)
{
    /* Serialize with stop via lifecycle lock */
    if (!s_diag_lifecycle_lock) {
        s_diag_lifecycle_lock = xSemaphoreCreateMutexStatic(&s_diag_lifecycle_lock_storage);
    }
    xSemaphoreTake(s_diag_lifecycle_lock, portMAX_DELAY);

    if (s_diag_task_handle) {
        xSemaphoreGive(s_diag_lifecycle_lock);
        return ESP_OK;
    }

    /* Reset state */
    atomic_store_explicit(&s_diag_stop_requested, false, memory_order_release);

    /* Create TASK_DONE semaphore (binary, starts empty) */
    s_diag_done_sem = xSemaphoreCreateBinaryStatic(&s_diag_done_sem_storage);
    if (!s_diag_done_sem) {
        xSemaphoreGive(s_diag_lifecycle_lock);
        return ESP_ERR_NO_MEM;
    }

    /* Diagnostics are intentionally non-real-time.  Create the task in PSRAM
     * so late bootstrap does not require a 12 KiB contiguous internal block
     * after Wi-Fi, BLE, LVGL and the audio DMA pool are resident. */
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        diagnostic_task, "diag_task", 3072, NULL, 2,
        &s_diag_task_handle, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS) {
        s_diag_task_handle = NULL;
        xSemaphoreGive(s_diag_lifecycle_lock);
        return ESP_ERR_NO_MEM;
    }

    /* Start notify timer (every 60 seconds) */
    s_diag_notify_timer = xTimerCreateStatic(
        "diag_notify", pdMS_TO_TICKS(60000), pdTRUE, NULL,
        diag_notify_timer_callback, &s_diag_notify_timer_storage);
    if (s_diag_notify_timer) {
        xTimerStart(s_diag_notify_timer, 0);
    }

    ESP_LOGI(TAG, "Diagnostic task started (60s interval)");
    xSemaphoreGive(s_diag_lifecycle_lock);
    return ESP_OK;
}

/* Stop diagnostic task with proper lifecycle:
 * 1. Set stop_requested (atomic)
 * 2. Stop timer
 * 3. Notify task to wake
 * 4. Wait on TASK_DONE semaphore (bounded)
 * 5. On success: clear handle, return OK
 * 6. On timeout: preserve resources, return TIMEOUT (retry-safe) */
static esp_err_t stop_diagnostic_task(void)
{
    if (!s_diag_lifecycle_lock) return ESP_OK;

    xSemaphoreTake(s_diag_lifecycle_lock, portMAX_DELAY);

    if (!s_diag_task_handle) {
        xSemaphoreGive(s_diag_lifecycle_lock);
        return ESP_OK;
    }

    /* 1. Signal stop */
    atomic_store_explicit(&s_diag_stop_requested, true, memory_order_release);

    /* 2. Stop timer first */
    if (s_diag_notify_timer) {
        xTimerStop(s_diag_notify_timer, pdMS_TO_TICKS(100));
        xTimerDelete(s_diag_notify_timer, pdMS_TO_TICKS(100));
        s_diag_notify_timer = NULL;
    }

    /* 3. Notify task to wake up and exit */
    xTaskNotifyGive(s_diag_task_handle);

    /* 4. Wait on TASK_DONE semaphore (bounded 2 seconds) */
    if (s_diag_done_sem) {
        if (xSemaphoreTake(s_diag_done_sem, pdMS_TO_TICKS(2000)) == pdTRUE) {
            /* 5. Task exited cleanly */
            s_diag_task_handle = NULL;
            ESP_LOGI(TAG, "Diagnostic task stopped");
            xSemaphoreGive(s_diag_lifecycle_lock);
            return ESP_OK;
        }
    }

    /* 6. Timeout: preserve resources, return TIMEOUT
     *    Second call can retry. Do NOT clear handle. */
    ESP_LOGW(TAG, "Diagnostic task stop timeout (task may still be running)");
    xSemaphoreGive(s_diag_lifecycle_lock);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t start_connectivity_timer(void)
{
    if (s_connectivity_timer) return ESP_OK;
    atomic_store_explicit(&s_display_wifi_applied, -1,
                          memory_order_release);
    atomic_store_explicit(&s_display_ble_applied, -1,
                          memory_order_release);
    s_connectivity_timer = xTimerCreateStatic(
        "ui_connectivity", pdMS_TO_TICKS(1000), pdTRUE, NULL,
        connectivity_timer_callback, &s_connectivity_timer_storage);
    if (!s_connectivity_timer) return ESP_ERR_NO_MEM;
    if (xTimerStart(s_connectivity_timer, pdMS_TO_TICKS(100)) != pdPASS) {
        (void)xTimerDelete(s_connectivity_timer, pdMS_TO_TICKS(100));
        s_connectivity_timer = NULL;
        return ESP_ERR_TIMEOUT;
    }
    return sync_display_connectivity(true);
}

static void stop_connectivity_timer(void)
{
    TimerHandle_t timer = s_connectivity_timer;
    if (!timer) return;
    (void)xTimerStop(timer, pdMS_TO_TICKS(100));
    (void)xTimerDelete(timer, pdMS_TO_TICKS(100));
    s_connectivity_timer = NULL;
}

static const char *wash_step_name(wash_step_type_t step)
{
    static const char *const names[] = {
        "等待放入", "BL50归位", "姿态切换", "进水", "洗涤剂",
        "波轮洗", "滚筒洗", "排水", "脱水", "烘干",
        "等待取出", "UV消毒", "程序完成"
    };
    return (unsigned)step < (sizeof(names) / sizeof(names[0]))
        ? names[step] : "待机";
}

static uint8_t tri_state_from_output(int state)
{
    return state == 2 ? 2U : state == 1 ? 1U : 0U;
}

/* Output truth fusion for the HMI: the service snapshot is authoritative when
 * it reports a confirmed ON/OFF.  When the service reports UNKNOWN, fall back
 * to the safety-managed real MCP output state (boot baseline wrote OFF +
 * successful-write tracking) — a genuine HAL snapshot, never an inference from
 * the known bit.  If even the MCP state is unknown, stay UNKNOWN (fail-closed). */
static uint8_t fuse_output_state(uint8_t service_state,
                                 uint32_t mcp_unknown_mask,
                                 safe_output_t output, bool mcp_on)
{
    if (service_state == 2U || service_state == 1U) return service_state;
    bool mcp_known = (mcp_unknown_mask & (1U << (int)output)) == 0U;
    if (!mcp_known) return 0U;
    return mcp_on ? 2U : 1U;
}

static uint8_t display_machine_state_from_executor(wash_exec_state_t state)
{
    switch (state) {
    case WASH_EXEC_STATE_IDLE:        return (uint8_t)MACHINE_STATE_IDLE;
    case WASH_EXEC_STATE_WAIT_LOAD:   return (uint8_t)MACHINE_STATE_WAITING_LOAD;
    case WASH_EXEC_STATE_RUNNING:     return (uint8_t)MACHINE_STATE_RUNNING;
    case WASH_EXEC_STATE_WAIT_UNLOAD: return (uint8_t)MACHINE_STATE_WAITING_UNLOAD;
    case WASH_EXEC_STATE_UV:          return (uint8_t)MACHINE_STATE_UV;
    case WASH_EXEC_STATE_RESETTING:   return (uint8_t)MACHINE_STATE_RESETTING;
    case WASH_EXEC_STATE_FAULT:       return (uint8_t)MACHINE_STATE_FAULT;
    default:                          return (uint8_t)MACHINE_STATE_BOOTING;
    }
}

static void publish_display_telemetry(void)
{
    static uint32_t sensor_log_divider;
    if (!s_display_active) return;
    display_telemetry_t telemetry;
    memset(&telemetry, 0, sizeof(telemetry));
    telemetry.position_degrees = -1;
    telemetry.target_position_degrees = -1;
    telemetry.wake_backend = DISPLAY_WAKE_EDGE_IMPULSE;
    telemetry.internal_free_bytes = heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    machine_status_t status;
    if (machine_status_store_get(&status) == ESP_OK) {
        telemetry.machine_state = (uint8_t)status.state;
        telemetry.wash_phase = (uint8_t)status.phase;
        telemetry.position_degrees = (int16_t)status.position;
        telemetry.target_position_degrees = (int16_t)status.target_position;
        telemetry.current_step = status.current_step;
        telemetry.total_steps = status.total_steps;
        telemetry.elapsed_ms = status.elapsed_ms;
        telemetry.remaining_ms = status.remaining_ms;
        telemetry.water_ml = status.estimated_water_ml;
        telemetry.flow_pulses = status.flow_pulses;
        telemetry.temperature_c = status.temperature_c;
        telemetry.humidity_rh = status.humidity_rh;
        telemetry.environment_valid = status.sht_valid;
        telemetry.water_full = status.water_full;
        telemetry.fan_percent = status.fan_percent;
        telemetry.heater_state = status.heater_on ? 2U : 1U;
        telemetry.uv_state = status.uv_on ? 2U : 1U;
        telemetry.detergent_state = status.detergent_pump_on ? 2U : 1U;
        telemetry.fault_code = (uint16_t)status.fault.code;
    }

    wash_executor_t *exec = wash_exec_bootstrap_get_executor();
    wash_exec_snapshot_t exec_snapshot;
    bool exec_valid = exec &&
        wash_executor_get_snapshot(exec, &exec_snapshot) == ESP_OK;
    if (exec_valid) {
        telemetry.machine_state = display_machine_state_from_executor(
            exec_snapshot.state);
        telemetry.current_step = exec_snapshot.completed_steps;
        telemetry.total_steps = exec_snapshot.total_steps;
    }

    safety_snapshot_t safety;
    bool safety_snap_ok = false;
    if (safety_manager_get_snapshot(&safety) == ESP_OK) {
        safety_snap_ok = true;
        telemetry.safety_fault = safety.fault_active;
        telemetry.emergency_stop = safety.emergency_stop_active;
        telemetry.mcp_unknown_mask = safety.mcp_unknown_mask;
        if (safety.fault_active)
            telemetry.fault_code = (uint16_t)safety.primary_fault.code;
    }

    position_snapshot_t position;
    if (position_service_get_snapshot(&position) == ESP_OK) {
        telemetry.position_degrees = (int16_t)position.current;
        telemetry.target_position_degrees = (int16_t)position.target;
        telemetry.position_pwm = position.pwm_percent;
        telemetry.hall_raw_mask = position.raw_hall_mask;
        telemetry.hall_stable_mask = position.stable_hall_mask;
        /* 快照可读即"有效"：没有磁铁触发是正常的 OFF（空心），不是 UNKNOWN。
         * UNKNOWN 只在快照缺失/读取失败时出现。 */
        telemetry.hall_valid = true;
        telemetry.hall_fault = position.state == POSITION_STATE_FAULT;
        telemetry.bucket_aligned = position.bucket_aligned;
    }

    /* PAJ7620 is owned by gesture_service.  Expose availability separately
     * from the last gesture event so the telemetry page can distinguish
     * "sensor not started" from an idle sensor. */
    telemetry.gesture_available = s_gesture_active;

    water_snapshot_t water;
    if (water_service_get_snapshot(&water) == ESP_OK) {
        telemetry.source_valve_state = water.source_valve_unknown
            ? 0U : water.source_valve_on ? 2U : 1U;
        telemetry.transfer_valve_state = water.transfer_valve_unknown
            ? 0U : water.transfer_valve_on ? 2U : 1U;
        telemetry.water_full = water.drum_water_full;
        telemetry.water_level_valid = water.water_level_valid;
        /* Show the raw PCNT count: manual flow testing happens while the
         * water FSM is idle, when the job-local total intentionally stays 0. */
        telemetry.flow_pulses = water.raw_flow_pulses;
        const float configured_ppl = machine_config_get()->water.pulses_per_liter;
        const uint32_t telemetry_ppl = (configured_ppl > 0.0f)
            ? (uint32_t)(configured_ppl + 0.5f) : FLOW_TELEMETRY_REFERENCE_PPL;
        /* Round to nearest millilitre.  Use 64 bit so a long-lived PCNT
         * count cannot overflow before its division by pulses-per-litre. */
        telemetry.water_ml = (uint32_t)(((uint64_t)water.raw_flow_pulses * 1000U +
                                          (telemetry_ppl / 2U)) / telemetry_ppl);
    }

    if (s_turbidity_sensor) {
        turbidity_reading_t turbidity = {0};
        if (turbidity_sensor_read(s_turbidity_sensor, &turbidity) == ESP_OK) {
            telemetry.turbidity_raw = (uint16_t)turbidity.raw;
            telemetry.turbidity_valid = true;
        }
    }

    dry_snapshot_t dry;
    if (dry_service_get_snapshot(&dry) == ESP_OK) {
        telemetry.fan_percent = dry.fan_percent;
        telemetry.fan_state = tri_state_from_output(dry.fan_output_state);
        telemetry.heater_state = tri_state_from_output(dry.heater_output_state);
        if (dry.sht_valid) {
            telemetry.temperature_c = dry.temperature_c;
            telemetry.humidity_rh = dry.humidity_percent;
            telemetry.environment_valid = dry.sht_fresh;
        }
    }

    /* 耦合热风模块（单继电器，风扇+加热丝并联同步启停）：命令态 tri-state +
     * 冷却锁定倒计时。绝不把命令态当作物理性风量证明。 */
    coupled_hot_air_snapshot_t cha;
    if (coupled_hot_air_service_get_snapshot(&cha) == ESP_OK) {
        if (cha.output_known) {
            telemetry.hot_air_state =
                cha.output_confirmed_on ? 2U : 1U;
        } else {
            telemetry.hot_air_state = 0U;
        }
        telemetry.hot_air_cooldown_ms = cha.cooldown_remaining_ms;
    }

    /* The dry FSM normally supplies the environmental sample.  Keep the UI
     * observable immediately after deferred SHT30 startup as well.  The SHT
     * driver serializes its own I2C access, so this fallback is safe beside
     * the dry-service reader. */
    if (!telemetry.environment_valid && s_hal && s_hal->read_sht) {
        sht_sample_t ui_sht = {0};
        if (s_hal->read_sht(&ui_sht) == ESP_OK && ui_sht.valid) {
            telemetry.temperature_c = ui_sht.temperature_c;
            telemetry.humidity_rh = ui_sht.humidity_rh;
            telemetry.environment_valid = true;
        }
    }

    if (telemetry.turbidity_valid) {
        telemetry.turbidity_ntu = turbidity_estimate_ntu(
            (int)telemetry.turbidity_raw, telemetry.temperature_c,
            telemetry.environment_valid);
    }

    drain_snapshot_t drain;
    if (drain_service_get_snapshot(&drain) == ESP_OK)
        telemetry.drain_state = tri_state_from_output(drain.output_state);
    detergent_snapshot_t detergent;
    if (detergent_service_get_snapshot(&detergent) == ESP_OK)
        telemetry.detergent_state = tri_state_from_output(detergent.output_state);
    uv_snapshot_t uv;
    if (uv_service_get_snapshot(&uv) == ESP_OK)
        telemetry.uv_state = tri_state_from_output(uv.output_state);
    bl50_snapshot_t bl50;
    if (bl50_service_get_snapshot(&bl50) == ESP_OK)
        telemetry.bl50_pwm = bl50.current_pwm_percent;

    /* 输出真值融合：服务快照权威；服务 UNKNOWN 时用 safety 管理的真实 MCP
     * 输出状态（启动基线写 OFF + 成功写跟踪），绝不从 known 位推断 OFF。
     * 这样待机时 IN/TR 在快照正常下显示空心 OFF 而非长期黄色 UNKNOWN。 */
    if (safety_snap_ok) {
        telemetry.source_valve_state = fuse_output_state(
            telemetry.source_valve_state, safety.mcp_unknown_mask,
            SAFE_OUTPUT_TAP_VALVE, safety.mcp_outputs.source_inlet_valve);
        telemetry.transfer_valve_state = fuse_output_state(
            telemetry.transfer_valve_state, safety.mcp_unknown_mask,
            SAFE_OUTPUT_TRANSFER_VALVE, safety.mcp_outputs.transfer_valve);
        telemetry.drain_state = fuse_output_state(
            telemetry.drain_state, safety.mcp_unknown_mask,
            SAFE_OUTPUT_DRAIN_VALVE, safety.mcp_outputs.drain_valve);
        telemetry.detergent_state = fuse_output_state(
            telemetry.detergent_state, safety.mcp_unknown_mask,
            SAFE_OUTPUT_DETERGENT_PUMP, safety.mcp_outputs.detergent_pump);
        telemetry.uv_state = fuse_output_state(
            telemetry.uv_state, safety.mcp_unknown_mask,
            SAFE_OUTPUT_UV, safety.mcp_outputs.uv);
        telemetry.heater_state = fuse_output_state(
            telemetry.heater_state, safety.mcp_unknown_mask,
            SAFE_OUTPUT_PTC_HEATER, safety.mcp_outputs.ptc_heater);
        telemetry.hot_air_state = fuse_output_state(
            telemetry.hot_air_state, safety.mcp_unknown_mask,
            SAFE_OUTPUT_PTC_HEATER, safety.mcp_outputs.ptc_heater);
    }

    /* 固件支持的输出掩码：无 PTC 且无热风模块的板卡 HOT AIR 位不支持，显示
     * 灰色 "--" 而非 UNKNOWN。bit0-4=IN/TR/DR/DT/UV, bit5=HOT AIR
     * （PTC 或耦合热风模块启用即支持）, bit6=FAN（恒支持）。 */
    telemetry.output_supported_mask = 0x5FU; /* IN/TR/DR/DT/UV/FAN */
    if (board_config_is_ptc_enabled() ||
        board_config_is_hot_air_module_enabled())
        telemetry.output_supported_mask |= (1U << DISPLAY_OUTPUT_HT);

    /* Keep a low-rate, ASCII-only sensor trace for long-run validation. */
    if (++sensor_log_divider >= 60U) {
        sensor_log_divider = 0U;
        ESP_LOGI(TAG, "UI_SENSOR water=%u/%u sht=%u t=%.1f rh=%.1f hall=0x%02x/0x%02x",
                 telemetry.water_level_valid ? 1U : 0U,
                 telemetry.water_full ? 1U : 0U,
                 telemetry.environment_valid ? 1U : 0U,
                 (double)telemetry.temperature_c,
                 (double)telemetry.humidity_rh,
                 telemetry.hall_raw_mask,
                 telemetry.hall_stable_mask);
    }

    /* Wi-Fi link state from the live runtime (never a stale connect cache).
     * 先采集 Wi-Fi 局部快照，供同一帧内的云端状态推断使用，避免出现
     * "Wi-Fi 已连接但云端仍显示 OFFLINE"的跨帧不一致。 */
    wifi_station_snapshot_t wsnap;
    uint8_t wifi_state_now = DISPLAY_WIFI_OFF;
    if (s_wifi_active && xiaojing_wifi_get_snapshot(&wsnap) == ESP_OK) {
        telemetry.wifi_rssi = 0x7fffffff; /* unknown */
        if (wsnap.state == WIFI_STATION_CONNECTING) {
            wifi_state_now = DISPLAY_WIFI_CONNECTING;
        } else if (wsnap.state == WIFI_STATION_CONNECTED) {
            wifi_state_now = wsnap.ip_address != 0U
                ? DISPLAY_WIFI_IP : DISPLAY_WIFI_CONNECTING;
        } else if (wsnap.state == WIFI_STATION_FAILED) {
            wifi_state_now = DISPLAY_WIFI_FAILED;
        } else {
            wifi_state_now = DISPLAY_WIFI_OFF;
        }
    }
    telemetry.wifi_state = wifi_state_now;

    voice_service_snapshot_t voice;
    if (s_voice_service_active && voice_service_get_snapshot(&voice) == ESP_OK) {
        telemetry.wake_backend = voice.backend == VOICE_WAKE_BACKEND_ESP_SR
            ? DISPLAY_WAKE_ESP_SR : DISPLAY_WAKE_EDGE_IMPULSE;
        telemetry.backend_switching = voice.backend_switching;
        /* 云端状态：无真实 cloud readiness API，Wi-Fi 有 IP 只证明网络可达，
         * 绝不伪报"云端 READY"。映射逻辑见 display_cloud_from_wifi()。 */
        telemetry.voice_cloud_state = display_cloud_from_wifi(wifi_state_now);
    }

    /* BLE link state from the transport snapshot: off / advertising /
     * connected.  VOICE_SUSPENDED and RECOVER_FAILED have no runtime source
     * on this bench build and are left at OFF. */
    ble_transport_snapshot_t bsnap;
    if (xiaojing_ble_transport_get_snapshot(&bsnap) == ESP_OK) {
        if (!bsnap.running) {
            telemetry.ble_state = DISPLAY_BLE_OFF;
        } else if (bsnap.connected) {
            telemetry.ble_state = DISPLAY_BLE_CONNECTED;
        } else {
            telemetry.ble_state = DISPLAY_BLE_ADVERTISING;
        }
    }

    /* Voice backend switch outcome pulse. */
    telemetry.backend_switch_result = (uint8_t)atomic_load_explicit(
        &s_backend_switch_result, memory_order_acquire);
    telemetry.backend_switch_target = (uint8_t)atomic_load_explicit(
        &s_backend_switch_target, memory_order_acquire);

    uint8_t progress = 0U;
    if (telemetry.total_steps > 0U) {
        uint32_t calculated = ((uint32_t)telemetry.current_step * 100U) /
                              telemetry.total_steps;
        progress = calculated > 100U ? 100U : (uint8_t)calculated;
    }
    const char *step = exec_valid && exec_snapshot.program_active
        ? wash_step_name(exec_snapshot.current_step_type) : "待机";
    (void)display_service_set_wash(
        exec_valid && exec_snapshot.program_active, progress, step);
    (void)display_service_set_telemetry(&telemetry);
}

static void display_telemetry_task(void *context)
{
    (void)context;
    while (!atomic_load_explicit(&s_display_telemetry_stop,
                                 memory_order_acquire)) {
        publish_display_telemetry();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(DISPLAY_TELEMETRY_PERIOD_MS));
    }
    if (s_display_telemetry_done) xSemaphoreGive(s_display_telemetry_done);
    vTaskDeleteWithCaps(NULL);
}

static esp_err_t start_display_telemetry(void)
{
    if (s_display_telemetry_task) return ESP_OK;
    if (!s_display_telemetry_done) {
        s_display_telemetry_done = xSemaphoreCreateBinaryStatic(
            &s_display_telemetry_done_storage);
        if (!s_display_telemetry_done) return ESP_ERR_NO_MEM;
    }
    while (xSemaphoreTake(s_display_telemetry_done, 0) == pdTRUE) {}
    atomic_store_explicit(&s_display_telemetry_stop, false,
                          memory_order_release);
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        display_telemetry_task, "ui_telemetry", DISPLAY_TELEMETRY_STACK_SIZE,
        NULL, 2, &s_display_telemetry_task, 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t stop_display_telemetry(void)
{
    TaskHandle_t task = s_display_telemetry_task;
    if (!task) return ESP_OK;
    atomic_store_explicit(&s_display_telemetry_stop, true,
                          memory_order_release);
    xTaskNotifyGive(task);
    if (!s_display_telemetry_done ||
        xSemaphoreTake(s_display_telemetry_done,
                       pdMS_TO_TICKS(2000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    s_display_telemetry_task = NULL;
    return ESP_OK;
}

static esp_err_t runtime_nvs_set_u8(const char *key, uint8_t value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(
        APP_RUNTIME_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, key, value);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t runtime_nvs_get_u8(const char *key, uint8_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(
        APP_RUNTIME_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    err = nvs_get_u8(handle, key, out);
    nvs_close(handle);
    return err;
}

static voice_wake_backend_t load_wake_backend(void)
{
    uint8_t stored = (uint8_t)VOICE_WAKE_BACKEND_EDGE_IMPULSE;
    esp_err_t err = runtime_nvs_get_u8(APP_NVS_WAKE_BACKEND_KEY, &stored);
    if (err == ESP_OK && stored <= (uint8_t)VOICE_WAKE_BACKEND_ESP_SR)
        return (voice_wake_backend_t)stored;
    return VOICE_WAKE_BACKEND_EDGE_IMPULSE;
}

static StaticSemaphore_t s_backend_nvs_request_storage;
static StaticSemaphore_t s_backend_nvs_done_storage;
static StaticSemaphore_t s_backend_nvs_lock_storage;
static SemaphoreHandle_t s_backend_nvs_request;
static SemaphoreHandle_t s_backend_nvs_done;
static SemaphoreHandle_t s_backend_nvs_lock;
static TaskHandle_t s_backend_nvs_task_handle;
static uint8_t s_backend_nvs_value;
static esp_err_t s_backend_nvs_result;

static void backend_nvs_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_backend_nvs_request, portMAX_DELAY) != pdTRUE)
            continue;
        s_backend_nvs_result = runtime_nvs_set_u8(
            APP_NVS_WAKE_BACKEND_KEY, s_backend_nvs_value);
        xSemaphoreGive(s_backend_nvs_done);
    }
}

static esp_err_t start_backend_nvs_worker(void)
{
    if (s_backend_nvs_task_handle) return ESP_OK;
    s_backend_nvs_request = xSemaphoreCreateBinaryStatic(
        &s_backend_nvs_request_storage);
    s_backend_nvs_done = xSemaphoreCreateBinaryStatic(
        &s_backend_nvs_done_storage);
    s_backend_nvs_lock = xSemaphoreCreateMutexStatic(
        &s_backend_nvs_lock_storage);
    if (!s_backend_nvs_request || !s_backend_nvs_done ||
        !s_backend_nvs_lock) return ESP_ERR_NO_MEM;
    /* Flash/NVS disables caches temporarily, so keep this worker's stack in
     * internal RAM and allocate it once while boot has ample contiguous heap. */
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        backend_nvs_task, "wake_nvs", 2048U, NULL, 5,
        &s_backend_nvs_task_handle, 1,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        s_backend_nvs_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "wake backend NVS worker started (internal stack)");
    return ESP_OK;
}

static esp_err_t save_wake_backend(voice_wake_backend_t backend)
{
    if (backend != VOICE_WAKE_BACKEND_EDGE_IMPULSE &&
        backend != VOICE_WAKE_BACKEND_ESP_SR)
        return ESP_ERR_INVALID_ARG;
    if (!s_backend_nvs_task_handle || !s_backend_nvs_lock)
        return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_backend_nvs_lock, pdMS_TO_TICKS(3000U)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    while (xSemaphoreTake(s_backend_nvs_done, 0) == pdTRUE) { }
    s_backend_nvs_value = (uint8_t)backend;
    s_backend_nvs_result = ESP_FAIL;
    xSemaphoreGive(s_backend_nvs_request);
    TickType_t wait_ticks = pdMS_TO_TICKS(3000);
    if (wait_ticks == 0) wait_ticks = 1;
    esp_err_t result = xSemaphoreTake(s_backend_nvs_done, wait_ticks) == pdTRUE
        ? s_backend_nvs_result : ESP_ERR_TIMEOUT;
    xSemaphoreGive(s_backend_nvs_lock);
    return result;
}

/* Seed or migrate firmware-owned demo credentials.  BLE-provisioned user
 * credentials are marked separately and are never overwritten at boot. */
static esp_err_t seed_demo_credentials_if_absent(void)
{
    wifi_station_credentials_t wifi;
    memset(&wifi, 0, sizeof(wifi));
    esp_err_t err = xiaojing_wifi_load_credentials(
        WIFI_STATION_DEFAULT_NAMESPACE, &wifi);
    uint8_t wifi_origin = 0U;
    esp_err_t origin_err = runtime_nvs_get_u8(
        APP_NVS_WIFI_ORIGIN_KEY, &wifi_origin);
    uint8_t preset_revision = 0U;
    (void)runtime_nvs_get_u8(APP_NVS_WIFI_PRESET_REV_KEY,
                             &preset_revision);
    bool no_credentials = err == ESP_ERR_NVS_NOT_FOUND;
    bool same_default_ssid = err == ESP_OK &&
        XIAOJING_DEFAULT_WIFI_SSID[0] &&
        strcmp(wifi.ssid, XIAOJING_DEFAULT_WIFI_SSID) == 0;
    bool legacy_firmware_preset = err == ESP_OK &&
        origin_err == ESP_ERR_NVS_NOT_FOUND &&
        same_default_ssid;
    bool update_firmware_preset = err == ESP_OK &&
        wifi_origin == WIFI_ORIGIN_FIRMWARE &&
        (!same_default_ssid || preset_revision < WIFI_PRESET_REVISION);
    /* Rev 2 is the single migration that repairs the stale JINGJIE password
     * already present on development boards, even if an older miniapp write
     * marked it as user-owned.  Later firmware revisions must not overwrite
     * a user-provisioned password. */
    bool one_time_preset_upgrade = same_default_ssid &&
        preset_revision < WIFI_FORCE_SAME_SSID_REVISION;
    if ((no_credentials || legacy_firmware_preset || update_firmware_preset ||
         one_time_preset_upgrade) &&
        XIAOJING_DEFAULT_WIFI_SSID[0]) {
        size_t ssid_len = strlen(XIAOJING_DEFAULT_WIFI_SSID);
        size_t pass_len = strlen(XIAOJING_DEFAULT_WIFI_PASSWORD);
        if (ssid_len >= sizeof(wifi.ssid) || pass_len >= sizeof(wifi.password))
            return ESP_ERR_INVALID_SIZE;
        memcpy(wifi.ssid, XIAOJING_DEFAULT_WIFI_SSID, ssid_len + 1U);
        memcpy(wifi.password, XIAOJING_DEFAULT_WIFI_PASSWORD, pass_len + 1U);
        err = xiaojing_wifi_save_credentials(
            WIFI_STATION_DEFAULT_NAMESPACE, &wifi);
        if (err != ESP_OK) {
            memset(&wifi, 0, sizeof(wifi));
            return err;
        }
        err = runtime_nvs_set_u8(
            APP_NVS_WIFI_ORIGIN_KEY, WIFI_ORIGIN_FIRMWARE);
        if (err == ESP_OK)
            err = runtime_nvs_set_u8(APP_NVS_WIFI_PRESET_REV_KEY,
                                     WIFI_PRESET_REVISION);
        if (err != ESP_OK) {
            memset(&wifi, 0, sizeof(wifi));
            return err;
        }
        ESP_LOGI(TAG, "Applied firmware Wi-Fi preset (SSID=%s, password_len=%u)",
                 wifi.ssid, (unsigned)pass_len);
    } else if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Existing Wi-Fi preset unreadable (0x%x); not overwritten",
                 err);
    }
    memset(&wifi, 0, sizeof(wifi));

    voice_credentials_value_t voice;
    memset(&voice, 0, sizeof(voice));
    err = voice_credentials_load(&voice);
    memset(&voice, 0, sizeof(voice));
    uint8_t voice_preset_revision = 0U;
    (void)runtime_nvs_get_u8(APP_NVS_VOICE_PRESET_REV_KEY,
                             &voice_preset_revision);
    bool seed_or_upgrade_voice = XIAOJING_VOICE_API_KEY[0] &&
        (err == ESP_ERR_NVS_NOT_FOUND ||
         voice_preset_revision < XIAOJING_VOICE_PRESET_REVISION);
    ESP_LOGI(TAG,
             "Voice preset check: stored=%u firmware=%u credential_state=0x%x key_len=%u",
             (unsigned)voice_preset_revision,
             (unsigned)XIAOJING_VOICE_PRESET_REVISION, (unsigned)err,
             (unsigned)strlen(XIAOJING_VOICE_API_KEY));
    if (seed_or_upgrade_voice) {
        voice.provider = XIAOJING_VOICE_PROVIDER;
        size_t key_len = strlen(XIAOJING_VOICE_API_KEY);
        size_t model_len = strlen(XIAOJING_VOICE_MODEL);
        if (key_len >= sizeof(voice.api_key) ||
            model_len >= sizeof(voice.model))
            return ESP_ERR_INVALID_SIZE;
        memcpy(voice.api_key, XIAOJING_VOICE_API_KEY, key_len + 1U);
        memcpy(voice.model, XIAOJING_VOICE_MODEL, model_len + 1U);
        err = voice_credentials_open_provisioning_window(1000U);
        if (err == ESP_OK) err = voice_credentials_save(&voice);
        if (err == ESP_OK) {
            err = runtime_nvs_set_u8(APP_NVS_VOICE_PRESET_REV_KEY,
                                     XIAOJING_VOICE_PRESET_REVISION);
        }
        memset(&voice, 0, sizeof(voice));
        if (err != ESP_OK) return err;
        ESP_LOGI(TAG, "Applied demo MiMo credential preset revision=%u",
                 (unsigned)XIAOJING_VOICE_PRESET_REVISION);
    } else if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Existing voice preset unreadable (0x%x); not overwritten",
                 err);
    }
    return ESP_OK;
}

static esp_err_t voice_audio_start_capture(void)
{
    return s_voice_audio ? audio_driver_start_rx(s_voice_audio)
                         : ESP_ERR_INVALID_STATE;
}

static esp_err_t voice_audio_stop_capture(void)
{
    return s_voice_audio ? audio_driver_stop_rx(s_voice_audio)
                         : ESP_ERR_INVALID_STATE;
}

static esp_err_t voice_audio_read_pcm(void *buffer, size_t size,
                                      size_t *bytes_read,
                                      uint32_t timeout_ms)
{
    return s_voice_audio
        ? audio_driver_read(
              s_voice_audio, buffer, size, bytes_read, timeout_ms)
        : ESP_ERR_INVALID_STATE;
}

static void resync_voice_capture_after_playback(void)
{
    if (!s_voice_audio || !audio_driver_is_input_enabled(s_voice_audio))
        return;
    esp_err_t err = audio_driver_resync_rx(s_voice_audio);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "voice capture RX resync after playback failed: 0x%x",
                 (unsigned)err);
}

static esp_err_t create_voice_audio_hal_if_needed(
    const xiaojing_hal_t **out)
{
    if (!out || !s_hal) return ESP_ERR_INVALID_ARG;
    /* REAL playback owns I2S0. Keep microphone capture on a persistent I2S1
     * channel so each PTT cycle does not destroy/recreate DMA descriptors.
     * Fake/test HALs remain injectable through the existing interface. */
    if (!s_real_hal &&
        s_hal->audio_start_capture && s_hal->audio_stop_capture &&
        s_hal->audio_read_pcm) {
        *out = s_hal;
        return ESP_OK;
    }
    if (s_voice_audio) {
        *out = &s_voice_audio_hal;
        return ESP_OK;
    }
    audio_config_t audio_config = AUDIO_DEFAULT_CONFIG();
    s_voice_audio = audio_driver_init_capture_only(&audio_config);
    if (!s_voice_audio) return ESP_ERR_NOT_SUPPORTED;
    memset(&s_voice_audio_hal, 0, sizeof(s_voice_audio_hal));
    s_voice_audio_hal.audio_start_capture = voice_audio_start_capture;
    s_voice_audio_hal.audio_stop_capture = voice_audio_stop_capture;
    s_voice_audio_hal.audio_read_pcm = voice_audio_read_pcm;
    *out = &s_voice_audio_hal;
    return ESP_OK;
}

static void destroy_voice_audio_hal(void)
{
    if (!s_voice_audio) return;
    audio_driver_destroy(s_voice_audio);
    s_voice_audio = NULL;
    memset(&s_voice_audio_hal, 0, sizeof(s_voice_audio_hal));
}

static bool voice_network_ready(void *context)
{
    (void)context;
    return xiaojing_wifi_is_connected();
}

static void voice_cloud_cancel_adapter(void *context)
{
    voice_cloud_idf_cancel((voice_cloud_idf_context_t *)context);
}

/* The cloud adapter's built-in completion only advances voice_service.  The
 * display needs both fields from the same immutable result before routing can
 * invoke speak_text().  Keeping this in the completion callback also avoids a
 * second shared transcript buffer and the associated cross-task race. */
static void voice_cloud_ui_completion(
    voice_cloud_result_t result,
    const voice_cloud_route_output_t *output,
    uint32_t token,
    void *context)
{
    (void)token;
    (void)context;
    if (result != VOICE_CLOUD_OK) {
        (void)voice_service_handle_event(
            VOICE_EVENT_FAILURE, (uint32_t)result);
        return;
    }
    if (!output) {
        (void)voice_service_handle_event(
            VOICE_EVENT_FAILURE, (uint32_t)VOICE_CLOUD_BAD_RESPONSE);
        return;
    }

    /* Arm this reply's optional TTS before publishing the text.  A PTT press
     * after the text appears cancels synthesis/playback for this reply. */
    if (s_real_hal)
        (void)real_hal_audio_prepare_playback(s_real_hal);

    if (s_display_active) {
        esp_err_t display_err = display_service_set_voice_text(
            output->route.transcript, output->route.reply_text);
        if (display_err != ESP_OK) {
            ESP_LOGW(TAG, "voice transcript display failed: 0x%x",
                     (unsigned)display_err);
        }
    }
    ESP_LOGI(TAG, "voice UI result committed: transcript_bytes=%u reply_bytes=%u",
             (unsigned)strlen(output->route.transcript),
             (unsigned)strlen(output->route.reply_text));

    esp_err_t event_err = voice_service_handle_event(
        VOICE_EVENT_CLOUD_RESPONSE, 0);
    if (event_err != ESP_OK) {
        ESP_LOGW(TAG, "cloud response state transition failed: 0x%x",
                 (unsigned)event_err);
        if (s_display_active)
            (void)display_service_set_error("语音状态异常，请重试");
        (void)voice_service_handle_event(
            VOICE_EVENT_FAILURE, (uint32_t)event_err);
        return;
    }

    esp_err_t route_err = voice_service_process_route_result(&output->route);
    if (route_err != ESP_OK) {
        ESP_LOGW(TAG, "cloud route processing failed: 0x%x",
                 (unsigned)route_err);
        if (s_display_active)
            (void)display_service_set_error("语音处理失败，请重试");
        (void)voice_service_handle_event(
            VOICE_EVENT_FAILURE, (uint32_t)route_err);
    }
}

static esp_err_t voice_placeholder_prompt(
    voice_prompt_id_t prompt, void *context)
{
    (void)context;
    ESP_LOGI(TAG, "voice prompt placeholder id=%d", (int)prompt);
    if (s_display_active) {
        if (prompt == VOICE_PROMPT_ERROR ||
            prompt == VOICE_PROMPT_NETWORK_UNAVAILABLE) {
            (void)display_service_set_error(
                prompt == VOICE_PROMPT_NETWORK_UNAVAILABLE
                    ? "网络不可用，请检查WiFi"
                    : "语音处理失败，请重试");
        } else if (prompt == VOICE_PROMPT_CHAT_THINKING) {
            (void)display_service_set_voice_state(DISPLAY_VOICE_THINKING);
        }
    }
    if (!s_real_hal) return ESP_OK;

    voice_prompt_pcm_t local_prompt;
    if (voice_prompt_audio_get(prompt, &local_prompt)) {
        uint32_t timeout_ms = local_prompt.duration_ms + 2000U;
        esp_err_t local_err = real_hal_audio_play_pcm16(
            s_real_hal, local_prompt.pcm, local_prompt.pcm_bytes,
            local_prompt.sample_rate, timeout_ms);
        resync_voice_capture_after_playback();
        if (local_err == ESP_OK) {
            ESP_LOGI(TAG, "embedded voice prompt played id=%d bytes=%u",
                     (int)prompt, (unsigned)local_prompt.pcm_bytes);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "embedded prompt failed id=%d err=0x%x; using tone",
                 (int)prompt, (unsigned)local_err);
    }
    uint16_t frequency = prompt == VOICE_PROMPT_ERROR ? 240U :
                         prompt == VOICE_PROMPT_REJECTED ? 360U :
                         prompt == VOICE_PROMPT_NETWORK_UNAVAILABLE ? 300U :
                         prompt == VOICE_PROMPT_CHAT_THINKING ? 760U : 620U;
    esp_err_t tone_err = real_hal_audio_play_tone(
        s_real_hal, frequency, 120U, 5000);
    resync_voice_capture_after_playback();
    return tone_err;
}

/* ---- Voice Frontend Integration ---- */

static void wake_detection_callback(const voice_wake_detection_t *det,
                                    void *context)
{
    (void)context;
    if (!det || !det->detected) return;
    ESP_LOGI(TAG, "wake detected: score=%.2f seq=%u",
             det->score, (unsigned)det->sequence);

    /* Non-blocking: send wake event to audio controller */
    esp_err_t err = audio_controller_notify_wake();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to notify wake: 0x%x", err);
        return;
    }
    err = voice_service_external_recording_begin(
        VOICE_INVOCATION_WAKE_WORD, VOICE_ROUTE_POLICY_AUTO);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to enter wake recording state: 0x%x", err);
        return;
    }
    if (s_display_active) {
        (void)display_service_show_page(DISPLAY_PAGE_VOICE);
        (void)display_service_set_voice_state(DISPLAY_VOICE_LISTENING);
    }
}

/* Wrapper for voice_frontend_feed to match audio_wake_feed_t signature */
static esp_err_t wake_feed_wrapper(const int16_t *samples, size_t count,
                                    void *context)
{
    (void)context;
    return voice_frontend_feed(samples, count);
}

/* Wrapper for voice_recorder_feed to match audio_dialog_feed_t signature */
static esp_err_t dialog_feed_wrapper(const int16_t *samples, size_t count,
                                      void *context)
{
    (void)context;
    return voice_recorder_feed(samples, count);
}

/* Wrapper for voice_recorder_begin */
static esp_err_t dialog_begin_wrapper(uint32_t session_id, void *context)
{
    (void)context;
    return voice_recorder_begin(session_id, VOICE_RECORDER_SOURCE_WAKE);
}

/* Wrapper for voice_recorder_end */
static esp_err_t dialog_end_wrapper(uint32_t session_id, void *context)
{
    (void)context;
    (void)session_id;
    esp_err_t err = voice_recorder_end(
        VOICE_RECORDER_END_SPEECH_COMPLETE);
    if (err != ESP_OK) return err;

    const uint8_t *pcm = NULL;
    size_t pcm_bytes = 0;
    err = voice_recorder_get_data(&pcm, &pcm_bytes);
    if (err == ESP_OK)
        err = voice_capture_idf_import_pcm(pcm, pcm_bytes);
    if (err == ESP_OK) {
        err = voice_cloud_adapter_set_route_context(
            VOICE_INVOCATION_WAKE_WORD, VOICE_ROUTE_POLICY_AUTO, NULL);
    }
    if (err == ESP_OK)
        err = voice_service_external_upload_begin();
    if (err == ESP_OK)
        err = voice_cloud_adapter_stop_recording_and_upload(NULL);
    if (err != ESP_OK) {
        (void)voice_capture_idf_cancel(NULL);
        ESP_LOGE(TAG, "wake recording cloud submit failed: %s (0x%x)",
                 esp_err_to_name(err), (unsigned)err);
        (void)voice_service_handle_event(
            VOICE_EVENT_FAILURE, (uint32_t)err);
        return err;
    }
    if (s_display_active)
        (void)display_service_set_voice_state(DISPLAY_VOICE_THINKING);
    ESP_LOGI(TAG, "wake recording submitted to cloud worker: pcm=%u",
             (unsigned)pcm_bytes);
    return ESP_OK;
}

/* Wrapper for voice_recorder_check_termination */
static bool dialog_check_terminate_wrapper(void *context)
{
    (void)context;
    voice_recorder_end_reason_t reason = voice_recorder_check_termination();
    return reason != VOICE_RECORDER_END_NONE;
}

static esp_err_t bootstrap_select_backend(voice_wake_backend_t backend,
                                          void *context)
{
    (void)context;
    voice_wake_backend_type_t type =
        (backend == VOICE_WAKE_BACKEND_ESP_SR)
            ? VOICE_WAKE_BACKEND_ESP_SR
            : VOICE_WAKE_BACKEND_EDGE_IMPULSE;

    /* Selection and activation are deliberately separate.  During BTN1
     * switching voice_service stops recognition, selects the target, plays
     * the confirmation prompt, then starts recognition again. */
    voice_wake_snapshot_t snap;
    esp_err_t err = voice_frontend_get_snapshot(&snap);
    if (err == ESP_OK && snap.state == VOICE_WAKE_STATE_LISTENING) {
        atomic_store_explicit(&s_backend_switch_result,
                              DISPLAY_BACKEND_SWITCHING,
                              memory_order_release);
        atomic_store_explicit(&s_backend_switch_target, (int)type,
                              memory_order_release);
        err = voice_frontend_switch_backend(type, 3000);
        if (err == ESP_ERR_TIMEOUT) {
            atomic_store_explicit(&s_backend_switch_result,
                                  DISPLAY_BACKEND_TIMEOUT,
                                  memory_order_release);
        } else if (err != ESP_OK) {
            atomic_store_explicit(&s_backend_switch_result,
                                  DISPLAY_BACKEND_FAILURE,
                                  memory_order_release);
        }
    } else if (err == ESP_OK &&
               (snap.state == VOICE_WAKE_STATE_INITIALIZED ||
                snap.state == VOICE_WAKE_STATE_STOPPED ||
                snap.state == VOICE_WAKE_STATE_FAULT)) {
        atomic_store_explicit(&s_requested_wake_backend, (int)type,
                              memory_order_release);
        atomic_store_explicit(&s_backend_switch_target, (int)type,
                              memory_order_release);
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        atomic_store_explicit(&s_requested_wake_backend, (int)type,
                              memory_order_release);
        atomic_store_explicit(&s_backend_switch_target, (int)type,
                              memory_order_release);
        ESP_LOGI(TAG, "wake backend selected: %s",
                 type == VOICE_WAKE_BACKEND_ESP_SR ? "ESP-SR" : "Edge Impulse");
    }
    return err;
}

static esp_err_t bootstrap_start_frontend(void *context)
{
    (void)context;
    voice_wake_backend_type_t selected = (voice_wake_backend_type_t)
        atomic_load_explicit(&s_requested_wake_backend, memory_order_acquire);
    if (selected != VOICE_WAKE_BACKEND_EDGE_IMPULSE &&
        selected != VOICE_WAKE_BACKEND_ESP_SR)
        selected = VOICE_WAKE_BACKEND_EDGE_IMPULSE;
    esp_err_t err = voice_frontend_start_backend(selected);
    ESP_LOGI(TAG, "bootstrap_start_frontend: start_backend(%d) = 0x%x",
             (int)selected, (unsigned)err);
    return err;
}

static esp_err_t bootstrap_stop_frontend(void *context)
{
    (void)context;
    return voice_frontend_stop(2000);
}

/* Heap profiling helper — logs internal and PSRAM state at key points. */
static void log_heap(const char *tag)
{
    ESP_LOGI(TAG, "HEAP[%s] int_free=%u int_largest=%u int_min=%u psram=%u",
             tag,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

/* ---- Task Completion Presentation Worker ----
 *
 * Dedicated task that consumes task_complete_event_t from a bounded queue
 * and plays the task_done prompt.  Never runs in Timer Service context.
 *
 * Exactly-once: keyed on (program_id, generation).
 * Queue full: event is dropped with a warning (producer is the wash executor
 *             which cannot block on presentation).
 * Playback failure: logged, does not affect wash COMPLETE terminal state.
 */

static void task_complete_worker(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "task_complete_worker started");
    task_complete_event_t ev;

    while (s_task_complete_task_running) {
        if (xQueueReceive(s_task_complete_queue, &ev,
                          pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;
        }

        /* Exactly-once guard */
        if (ev.program_id == s_tc_last_program_id &&
            ev.generation == s_tc_last_generation) {
            ESP_LOGD(TAG, "task_done duplicate ignored prog=%u gen=%u",
                     (unsigned)ev.program_id, (unsigned)ev.generation);
            continue;
        }
        s_tc_last_program_id = ev.program_id;
        s_tc_last_generation = ev.generation;

        ESP_LOGI(TAG, "task_done prompt: program=%u generation=%u",
                 (unsigned)ev.program_id, (unsigned)ev.generation);

        /* Play prompt (runs in task context, not Timer Service) */
        if (!s_real_hal) continue;
        voice_prompt_pcm_t pcm;
        if (voice_prompt_audio_get_task_done(&pcm)) {
            esp_err_t err = real_hal_audio_play_pcm16(
                s_real_hal, pcm.pcm, pcm.pcm_bytes,
                pcm.sample_rate, pcm.duration_ms + 2000U);
            resync_voice_capture_after_playback();
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "task_done played bytes=%u",
                         (unsigned)pcm.pcm_bytes);
            } else {
                ESP_LOGW(TAG, "task_done playback failed err=0x%x "
                         "(wash COMPLETE unaffected)", (unsigned)err);
            }
        } else {
            ESP_LOGW(TAG, "task_done WAV not available");
        }
    }

    ESP_LOGI(TAG, "task_complete_worker exiting");
    s_task_complete_task = NULL;
    vTaskDelete(NULL);
}

/* Publish a task completion event.  Called from the wash executor snapshot
 * poll path (non-blocking).  If the queue is full the event is dropped
 * with a warning — the wash executor must never block on presentation. */
static void publish_task_complete(uint32_t program_id, uint32_t generation)
{
    if (!s_task_complete_queue) return;
    task_complete_event_t ev = { .program_id = program_id,
                                 .generation = generation };
    if (xQueueSend(s_task_complete_queue, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "task_complete queue full, dropping event "
                 "prog=%u gen=%u", (unsigned)program_id,
                 (unsigned)generation);
    }
}

static esp_err_t start_task_complete_worker(void)
{
    if (s_task_complete_task) return ESP_OK;
    s_task_complete_queue = xQueueCreate(TASK_COMPLETE_QUEUE_DEPTH,
                                         sizeof(task_complete_event_t));
    if (!s_task_complete_queue) return ESP_ERR_NO_MEM;
    s_task_complete_task_running = true;
    BaseType_t r = xTaskCreatePinnedToCore(
        task_complete_worker, "task_done_w", 4096, NULL, 2,
        &s_task_complete_task, 1);
    if (r != pdPASS) {
        s_task_complete_task_running = false;
        vQueueDelete(s_task_complete_queue);
        s_task_complete_queue = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* Poll wash executor for PROGRAM_COMPLETE.  Runs from the connectivity
 * timer callback (which already runs every 1 s).  Only publishes —
 * never plays audio here. */
static void poll_wash_completion(void)
{
    wash_executor_t *exec = wash_exec_bootstrap_get_executor();
    if (!exec) return;
    wash_exec_snapshot_t snap;
    if (wash_executor_get_snapshot(exec, &snap) != ESP_OK) return;
    if (!snap.program_active &&
        snap.last_event == WASH_EXEC_EVENT_PROGRAM_COMPLETE &&
        snap.program_id != 0) {
        publish_task_complete(snap.program_id, snap.generation);
    }
}

static esp_err_t voice_cloud_speech(
    const char *utf8_text, void *context)
{
    (void)context;
    if (!utf8_text) return ESP_ERR_INVALID_ARG;
    /* voice_cloud_ui_completion already committed transcript + reply as one
     * display update. Do not erase either field on a TTS-side failure. */
    if (s_effective_voice_credentials.provider !=
            VOICE_PROVIDER_MIMO_DIRECT ||
        s_effective_voice_credentials.api_key[0] == '\0') {
        ESP_LOGW(TAG, "dynamic TTS unavailable for active provider");
        return ESP_OK;
    }

    voice_tts_audio_t audio;
    esp_err_t err = voice_tts_idf_synthesize(
        VOICE_MIMO_ENDPOINT, s_effective_voice_credentials.api_key,
        utf8_text, &audio);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TTS synthesis failed: %s (0x%x); text reply retained",
                 esp_err_to_name(err), (unsigned)err);
        /* Keep the already committed reply visible even when the optional
         * speech channel is unavailable.  This also bumps the display
         * revision in case LVGL was busy during the HTTP failure. */
        if (s_display_active)
            (void)display_service_set_voice_state(DISPLAY_VOICE_REPLY);
        return ESP_OK;
    }

    if (s_real_hal) {
        uint32_t timeout_ms = audio.duration_ms + 3000U;
        if (timeout_ms < 5000U) timeout_ms = 5000U;
        err = real_hal_audio_play_pcm16(
            s_real_hal, audio.pcm, audio.pcm_bytes,
            audio.sample_rate, timeout_ms);
        resync_voice_capture_after_playback();
    } else {
        ESP_LOGI(TAG, "TTS software path ready; no audio HAL for playback");
        err = ESP_OK;
    }
    voice_tts_idf_release(&audio);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TTS playback failed: %s (0x%x); text reply retained",
                 esp_err_to_name(err), (unsigned)err);
    }
    /* Audio is a degradable presentation channel. A valid cloud/text reply
     * remains a successful voice transaction even when the amplifier fails. */
    return ESP_OK;
}

/* ---- Event queue (shared: services publish, adapter consumer reads) ---- */

static QueueHandle_t s_event_queue = NULL;

static esp_err_t bootstrap_event_publish(const machine_event_t *event,
                                          uint32_t timeout_ms, void *context) {
    (void)context;
    if (!s_event_queue) return ESP_ERR_INVALID_STATE;
    TickType_t ticks = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
    return (xQueueSend(s_event_queue, event, ticks) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

static machine_event_sink_t make_event_sink(void) {
    return (machine_event_sink_t){ .publish = bootstrap_event_publish, .context = NULL };
}

static esp_err_t protocol_tx_critical(const char *json, size_t length,
                                      uint32_t timeout_ms, void *context) {
    (void)context;
    return xiaojing_ble_transport_send_critical(json, length, timeout_ms);
}

static esp_err_t protocol_tx_status(const char *json, size_t length,
                                    uint32_t timeout_ms, void *context) {
    (void)timeout_ms; (void)context;
    return xiaojing_ble_transport_publish_status(json, length);
}

/* UV 灯安全自检（开发/板级诊断）。委托给 uv_self_test 组件：12 项安全门禁
 * + uv_service_run_async(3000ms)。非阻塞，异步提交。 */
static esp_err_t protocol_uv_self_test(const char **out_code,
                                       uint32_t *out_duration_ms) {
    return uv_self_test_request(out_code, out_duration_ms);
}

static uint8_t protocol_uv_self_test_state(void) {
    return uv_self_test_state_code();
}

/* 执行器板测（开发/板级诊断）。委托给 actuator_self_test 组件：15 项安全
 * 门禁 + water_service 单阀入口 / detergent_service_run_async。非阻塞。 */
static esp_err_t protocol_actuator_self_test(const char *target,
                                             const char **out_code,
                                             uint32_t *out_duration_ms) {
    actuator_self_test_target_t t;
    if (!actuator_self_test_target_from_name(target, &t)) {
        *out_code = "UNKNOWN_TARGET";
        *out_duration_ms = 0;
        return ESP_OK;
    }
    return actuator_self_test_request(t, out_code, out_duration_ms);
}

static bool protocol_actuator_self_test_snapshot(
    app_protocol_actuator_snapshot_t *out)
{
    if (!out) return false;
    actuator_self_test_snapshot_t snap;
    coupled_hot_air_snapshot_t cha;
    if (actuator_self_test_get_snapshot(&snap) != ESP_OK) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    out->state = (uint8_t)snap.state;
    out->target = (uint8_t)snap.target;
    out->request_id = snap.request_id;
    out->duration_ms = snap.requested_duration_ms;
    out->output_confirmed_off = snap.output_confirmed_off;
    out->last_code = snap.last_code;
    /* 热风冷却剩余：以 coupled_hot_air_service 权威快照为准（客户端展示
     * 倒计时，且不受板测状态机是否空闲影响）。 */
    out->hot_air_cooldown_ms = 0;
    if (coupled_hot_air_service_get_snapshot(&cha) == ESP_OK) {
        out->hot_air_cooldown_ms = cha.cooldown_remaining_ms;
    }
    return true;
}

/* 电机空载台架（开发/板级诊断）。委托给 motor_bench_service：执行器空闲
 * 门禁 + capability 门控；绝不自动让电机通电 —— 每步由用户显式 confirm
 * 后才提交一次低能量短脉冲（低档 ≤300ms），随后确认 BL50 回 IDLE 且
 * PWM=0 才推进。非阻塞，异步执行。 */
static esp_err_t protocol_motor_bench_start(const char *type_name,
                                            const char **out_code) {
    /* 用 motor_bench_core 的单一类型映射源（含 pulsator_behavior），
     * 不再维护硬编码字符串链（漏加新类型会返回 BAD_TYPE）。 */
    motor_bench_type_t type;
    if (!motor_bench_type_from_name(type_name, &type)) {
        *out_code = "BAD_TYPE";
        return ESP_OK;
    }
    return motor_bench_service_request(type, out_code);
}

/* 换位电机（IBT-2 有刷位置电机）板测诊断：target_name 为角度名
 * "0"/"45"/"90"/"180"/"270"，驱动 position_service 把滚筒转到目标角度。
 * 门禁 fail-closed：emergency/fault/禁止输出/BL50 非空闲/位置服务忙 → 拒绝。
 * PWM：由 machine_config.benchtst.ibt2_pwm 决定（滑杆/NVS 可调，默认 80）。 */
static esp_err_t protocol_position_move_test(const char *target_name,
                                             const char **out_code) {
    drum_position_t target;
    if (strcmp(target_name, "0") == 0)         target = DRUM_POS_0;
    else if (strcmp(target_name, "45") == 0)   target = DRUM_POS_45;
    else if (strcmp(target_name, "90") == 0)   target = DRUM_POS_90;
    else if (strcmp(target_name, "180") == 0)  target = DRUM_POS_180;
    else if (strcmp(target_name, "270") == 0)  target = DRUM_POS_270;
    else { *out_code = "BAD_POSITION"; return ESP_OK; }

    safety_snapshot_t safety;
    if (safety_manager_get_snapshot(&safety) != ESP_OK) {
        *out_code = "SNAPSHOT_UNAVAILABLE"; return ESP_OK;
    }
    if (safety.emergency_stop_active) { *out_code = "EMERGENCY_ACTIVE"; return ESP_OK; }
    if (safety.fault_active)          { *out_code = "FAULT_ACTIVE"; return ESP_OK; }

    bl50_snapshot_t bl50;
    if (bl50_service_get_snapshot(&bl50) != ESP_OK) {
        *out_code = "BL50_UNAVAILABLE"; return ESP_OK;
    }
    if (bl50.state != BL50_SVC_STATE_IDLE || bl50.current_pwm_percent != 0U) {
        *out_code = "BL50_BUSY"; return ESP_OK;
    }

    position_snapshot_t pos;
    if (position_service_get_snapshot(&pos) != ESP_OK) {
        *out_code = "POSITION_UNAVAILABLE"; return ESP_OK;
    }
    if (pos.state != POSITION_STATE_IDLE_KNOWN &&
        pos.state != POSITION_STATE_IDLE_UNKNOWN) {
        *out_code = "POSITION_BUSY"; return ESP_OK;
    }

    static uint32_t s_pos_test_req_id;
    /* 板测诊断 PWM：优先 benchtst.ibt2_pwm（NVS/滑杆可调），否则用
     * 编译默认 80%（角度切换实测通过值）。生产洗涤 position move 不受
     * 影响（仍读 NVS move_pwm_percent）。 */
    uint8_t bench_pwm = 80U;
    const machine_config_t *mcfg = machine_config_get();
    if (mcfg && mcfg->benchtst.ibt2_pwm > 0U &&
        mcfg->benchtst.ibt2_pwm <= 100U) {
        bench_pwm = mcfg->benchtst.ibt2_pwm;
    }
    position_move_request_t req = {
        .request_id = machine_request_id_next(s_pos_test_req_id),
        .target = target,
        .direction_policy = POSITION_DIR_PREFER_CW,
        .pwm_percent = bench_pwm,
        .timeout_ms = 0,     /* 使用配置默认 move_timeout_ms */
    };
    s_pos_test_req_id = req.request_id;
    esp_err_t err = position_service_move_async(&req);
    if (err != ESP_OK) { *out_code = "POSITION_MOVE_REJECTED"; return ESP_OK; }
    *out_code = "POSITION_MOVE_ACCEPTED";
    return ESP_OK;
}

static esp_err_t protocol_motor_bench_confirm(const char **out_code) {
    esp_err_t err = motor_bench_service_confirm_pulse();
    *out_code = "OK";
    return err;
}

static esp_err_t protocol_motor_bench_cancel(const char **out_code) {
    esp_err_t err = motor_bench_service_cancel();
    *out_code = "OK";
    return err;
}

static bool protocol_motor_bench_snapshot(
    app_protocol_motor_bench_snapshot_t *out)
{
    if (!out) return false;
    motor_bench_snapshot_t snap;
    if (motor_bench_service_get_snapshot(&snap) != ESP_OK) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    out->state = (uint8_t)snap.state;
    out->type = (uint8_t)snap.type;
    out->request_id = snap.request_id;
    out->current_step = snap.current_step;
    out->step_count = snap.step_count;
    out->required_position = (int32_t)snap.required_position;
    out->current_action = (uint8_t)snap.current_action;
    out->position_ready = snap.position_ready;
    out->terminal = (uint8_t)snap.terminal;
    out->output_confirmed_off = snap.output_confirmed_off;
    out->elapsed_ms = snap.elapsed_ms;
    out->last_code = snap.last_code;
    return true;
}

/* 连续换向（position swing）展示台诊断。委托 position_swing_service。
 * 门禁 fail-closed：无故障/急停/其他自检/洗涤运行、位置已标定。 */
static esp_err_t protocol_position_swing_test(
    uint32_t rounds, uint32_t duration_ms, const char **out_code)
{
    position_swing_request_t req = { .rounds = rounds,
                                     .duration_ms = duration_ms };
    return position_swing_service_request(&req, out_code);
}

static esp_err_t protocol_position_swing_cancel(const char **out_code)
{
    return position_swing_service_cancel(out_code);
}

static bool protocol_position_swing_snapshot(
    app_protocol_position_swing_snapshot_t *out)
{
    if (!out) return false;
    position_swing_snapshot_t snap;
    if (position_swing_service_get_snapshot(&snap) != ESP_OK) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    out->state = (uint32_t)snap.state;
    out->terminal = (uint32_t)snap.terminal;
    out->request_id = snap.request_id;
    out->segment_index = snap.segment_index;
    out->dir_cw = snap.dir_cw ? 1U : 0U;
    out->off = snap.output_confirmed_off ? 1U : 0U;
    out->fl = snap.dir_flipped ? 1U : 0U;
    out->elapsed_ms = snap.elapsed_ms;
    out->emergency = snap.emergency ? 1U : 0U;
    out->rounds = snap.total_rounds;
    out->dwell_ms = snap.dwell_ms;
    out->last_code = "";
    return true;
}

static esp_err_t ble_frame_sink(const char *frame, size_t length, void *context) {
    (void)context;
    return app_protocol_process_frame(frame, length);
}

static void credential_restart_task(void *context)
{
    (void)context;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static void schedule_credential_restart(void)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(
            &s_credential_restart_scheduled, &expected, true,
            memory_order_acq_rel, memory_order_acquire))
        return;
#if defined(XIAOJING_TESTING)
    ESP_LOGI(TAG, "credential restart scheduled (suppressed in tests)");
#else
    if (xTaskCreate(credential_restart_task, "voice_cfg_reboot", 2048,
                    NULL, 4, NULL) != pdPASS) {
        atomic_store_explicit(
            &s_credential_restart_scheduled, false, memory_order_release);
        ESP_LOGE(TAG, "failed to schedule credential restart");
    }
#endif
}

static esp_err_t ble_secure_frame_sink(
    const char *frame, size_t length, void *context)
{
    (void)context;
    voice_credentials_command_result_t result;
    esp_err_t err = voice_credentials_process_secure_frame(
        frame, length, &result);
    if (err != ESP_OK) return err;
    char reply[192];
    int written = snprintf(
        reply, sizeof(reply),
        "{\"v\":1,\"type\":\"credential_ack\",\"seq\":%" PRIu32
        ",\"ok\":%s,\"code\":\"%s\",\"restart_required\":%s}",
        result.sequence, result.ok ? "true" : "false", result.code,
        result.restart_required ? "true" : "false");
    if (written < 0 || (size_t)written >= sizeof(reply))
        return ESP_ERR_INVALID_SIZE;
    err = xiaojing_ble_transport_send_critical(
        reply, (size_t)written, 100U);
    if (err == ESP_OK && result.ok && result.restart_required)
        schedule_credential_restart();
    return err;
}

static void ble_connection_sink(bool connected, void *context) {
    (void)context;
    atomic_store_explicit(&s_ble_connected, connected, memory_order_release);
    app_protocol_set_connected(connected);
    if (connected) {
        log_heap("ble_connected");
    } else {
        /* BLE 断线：对诊断自检采取严格策略，立即紧急关灯/关阀/关泵。
         * emergency 失败必须记录：输出仍由 service + safety_manager 关断，
         * 但调用方不能静默吞掉错误（见 actuator/uv emergency 闩锁语义）。 */
        esp_err_t uve = uv_self_test_emergency();
        if (uve != ESP_OK)
            ESP_LOGE(TAG, "BLE disconnect: uv emergency failed (0x%x)", uve);
        esp_err_t ace = actuator_self_test_emergency();
        if (ace != ESP_OK)
            ESP_LOGE(TAG, "BLE disconnect: actuator emergency failed (0x%x)",
                     ace);
        esp_err_t mbe = motor_bench_service_emergency();
        if (mbe != ESP_OK)
            ESP_LOGE(TAG, "BLE disconnect: motor_bench emergency failed (0x%x)",
                     mbe);
        log_heap("ble_disconnected");
    }
    if (s_display_active) (void)sync_display_connectivity(true);
}

static void wifi_connection_sink(bool connected, void *context)
{
    (void)context;
    if (connected) {
        wifi_station_snapshot_t snap;
        if (xiaojing_wifi_get_snapshot(&snap) == ESP_OK) {
            ESP_LOGI(TAG, "GOT_IP: attempts=%"PRIu32
                     " disconnects=%"PRIu32
                     " reason=%"PRId32,
                     snap.connect_attempts,
                     snap.disconnects,
                     snap.last_disconnect_reason);
            ESP_LOGI(TAG, "GOT_IP: ip=%lu.%lu.%lu.%lu",
                     (unsigned long)(snap.ip_address & 0xFF),
                     (unsigned long)((snap.ip_address >> 8) & 0xFF),
                     (unsigned long)((snap.ip_address >> 16) & 0xFF),
                     (unsigned long)((snap.ip_address >> 24) & 0xFF));
        }
        log_heap("got_ip");
    } else {
        ESP_LOGI(TAG, "WIFI_DISCONNECT");
    }
    if (!s_display_active) return;
    esp_err_t display_err = sync_display_connectivity(true);
    if (display_err != ESP_OK)
        ESP_LOGW(TAG, "Immediate Wi-Fi display update failed: 0x%x",
                 display_err);
    if (connected)
        (void)display_service_show_page(DISPLAY_PAGE_OVERVIEW);
}

static esp_err_t protocol_provision_wifi(
    const char *ssid, const char *password, void *context)
{
    (void)context;
    if (!s_wifi_active || !ssid || !password)
        return ESP_ERR_INVALID_STATE;
    wifi_station_credentials_t credentials;
    memset(&credentials, 0, sizeof(credentials));
    size_t ssid_length = strnlen(ssid, sizeof(credentials.ssid));
    size_t password_length =
        strnlen(password, sizeof(credentials.password));
    if (ssid_length >= sizeof(credentials.ssid) ||
        password_length >= sizeof(credentials.password))
        return ESP_ERR_INVALID_ARG;
    memcpy(credentials.ssid, ssid, ssid_length);
    memcpy(credentials.password, password, password_length);
    if (!xiaojing_wifi_credentials_valid(&credentials)) {
        memset(&credentials, 0, sizeof(credentials));
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error =
        xiaojing_wifi_provision_and_start(&credentials);
    /* start_async may fail after the credential commit.  Mark user ownership
     * whenever NVS contains the requested value, independent of connection. */
    wifi_station_credentials_t stored;
    memset(&stored, 0, sizeof(stored));
    esp_err_t load_error = xiaojing_wifi_load_credentials(
        WIFI_STATION_DEFAULT_NAMESPACE, &stored);
    if (load_error == ESP_OK &&
        strcmp(stored.ssid, credentials.ssid) == 0 &&
        strcmp(stored.password, credentials.password) == 0) {
        esp_err_t marker_error = runtime_nvs_set_u8(
            APP_NVS_WIFI_ORIGIN_KEY, WIFI_ORIGIN_USER);
        if (error == ESP_OK) error = marker_error;
    }
    memset(&stored, 0, sizeof(stored));
    memset(&credentials, 0, sizeof(credentials));
    return error;
}

void *app_bootstrap_get_event_queue(void) {
    return (void *)s_event_queue;
}

/* ---- Button → Executor sink callbacks ---- */

/* ================================================================
 * program_control 注入回调（急停 / 后端切换 / 审计 provider）
 * ================================================================ */

static bool pc_is_voice_active(void)
{
    return s_voice_service_active;
}

/* 急停：走 safety_manager 全局闩锁（全危险输出关）。 */
static esp_err_t pc_emergency_cb(void *ctx)
{
    (void)ctx;
    return safety_manager_emergency_stop();
}

/* 语音后端切换（从原 BTN1 长按逻辑提取；经 program_control 仲裁后调用）。 */
static esp_err_t pc_backend_switch_cb(void *ctx)
{
    (void)ctx;
    if (!s_voice_service_active) return ESP_ERR_NOT_SUPPORTED;
    voice_service_snapshot_t before;
    esp_err_t switch_err = voice_service_get_snapshot(&before);
    if (switch_err != ESP_OK) return switch_err;
    voice_wake_backend_t target = before.backend ==
        VOICE_WAKE_BACKEND_EDGE_IMPULSE ? VOICE_WAKE_BACKEND_ESP_SR :
                                          VOICE_WAKE_BACKEND_EDGE_IMPULSE;
    switch_err = save_wake_backend(target);
    if (switch_err == ESP_OK)
        switch_err = voice_service_toggle_backend();
    if (switch_err != ESP_OK) {
        (void)save_wake_backend(before.backend);
    } else {
        ESP_LOGI(TAG, "wake backend persisted: %s",
                 target == VOICE_WAKE_BACKEND_ESP_SR ?
                     "ESP-SR" : "Edge Impulse");
    }
    return switch_err;
}

/* 输出审计 provider：从各服务快照读取危险输出三态 + 电机忙态。
 * 任一快照读取失败 → 相应字段保持 UNKNOWN(0) → 审计失败（fail-closed）。 */
static esp_err_t pc_audit_provider(pc_output_audit_t *out, void *ctx)
{
    (void)ctx;
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    uv_snapshot_t uv;
    if (uv_service_get_snapshot(&uv) == ESP_OK) out->uv = uv.output_state;

    dry_snapshot_t dry;
    if (dry_service_get_snapshot(&dry) == ESP_OK) {
        out->heater = dry.heater_output_state;
        out->fan = dry.fan_output_state;
    }

    water_snapshot_t water;
    if (water_service_get_snapshot(&water) == ESP_OK) {
        out->tap_valve = water.source_valve_unknown
            ? 0 : (water.source_valve_on ? 2 : 1);
        out->transfer_valve = water.transfer_valve_unknown
            ? 0 : (water.transfer_valve_on ? 2 : 1);
    }

    detergent_snapshot_t det;
    if (detergent_service_get_snapshot(&det) == ESP_OK)
        out->detergent_pump = det.output_state;

    drain_snapshot_t drain;
    if (drain_service_get_snapshot(&drain) == ESP_OK)
        out->drain_valve = drain.output_state;

    bl50_snapshot_t bl;
    if (bl50_service_get_snapshot(&bl) == ESP_OK) {
        out->bl50_busy = !(bl.state == BL50_SVC_STATE_IDLE ||
                           bl.state == BL50_SVC_STATE_COMPLETE ||
                           bl.state == BL50_SVC_STATE_FAULT);
    }

    position_snapshot_t pos;
    if (position_service_get_snapshot(&pos) == ESP_OK) {
        out->position_moving = (pos.state == POSITION_STATE_MOVING ||
                                pos.state == POSITION_STATE_BRAKING ||
                                pos.state == POSITION_STATE_STOPPING);
    }
    return ESP_OK;
}

static esp_err_t button_normal_sink(const button_sink_event_t *event,
                                     uint32_t timeout_ms, void *context) {
    (void)timeout_ms; (void)context;
    ESP_LOGI(TAG, "button event: id=%u type=%u seq=%" PRIu32,
             (unsigned)event->button_id, (unsigned)event->event_type,
             event->sequence);
    if (event->button_id == BUTTON_ID_1 &&
        event->event_type == BUTTON_EVENT_LONG_PRESS) {
        /* BTN1 长按 → 经 program_control 仲裁 → 语音后端切换。
         * 长按抑制释放时的 CLICK（互斥），不会误触 START。 */
        pc_btn1_action_t action = PC_BTN1_ACTION_NONE;
        return program_control_btn1(PC_BTN1_EV_LONG_PRESS, &action);
    }
    if (event->button_id == BUTTON_ID_2 &&
        event->event_type == BUTTON_EVENT_LONG_PRESS) {
        if (s_display_active) {
            (void)display_service_show_page(DISPLAY_PAGE_VOICE);
        }
        if (s_real_hal) {
            (void)real_hal_audio_cancel_playback(s_real_hal);
            /* Playback writes are 320-byte chunks, so 20 ms is enough for
             * the previous producer to observe cancellation and release TX. */
            vTaskDelay(pdMS_TO_TICKS(20));
            esp_err_t tone_err = real_hal_audio_play_tone(
                s_real_hal, 660U, 100U, 5000);
            /* The ready tone is presentation-only.  A missing/busy
             * amplifier must never prevent the PTT state transition. */
            if (tone_err != ESP_OK) {
                ESP_LOGW(TAG, "PTT ready tone failed (0x%x); continuing",
                         (unsigned)tone_err);
            }
        }
        if (!s_voice_service_active) {
            ESP_LOGE(TAG, "PTT begin rejected: voice service inactive");
            return ESP_ERR_NOT_SUPPORTED;
        }
        esp_err_t ptt_err = voice_service_ptt_begin();
        if (ptt_err != ESP_OK) {
            ESP_LOGE(TAG, "PTT begin failed: %s (0x%x)",
                     esp_err_to_name(ptt_err), (unsigned)ptt_err);
            if (s_display_active)
                (void)display_service_set_error("语音启动失败，请重试");
        } else {
            if (s_display_active)
                (void)display_service_set_voice_state(
                    DISPLAY_VOICE_LISTENING);
            ESP_LOGI(TAG, "PTT recording started");
        }
        return ptt_err;
    }
    if (event->button_id == BUTTON_ID_2 &&
        event->event_type == BUTTON_EVENT_LONG_PRESS_RELEASE) {
        if (s_display_active) {
            (void)display_service_show_page(DISPLAY_PAGE_VOICE);
        }
        if (!s_voice_service_active) {
            ESP_LOGE(TAG, "PTT end rejected: voice service inactive");
            return ESP_ERR_NOT_SUPPORTED;
        }
        esp_err_t ptt_err = voice_service_ptt_end();
        if (ptt_err != ESP_OK) {
            ESP_LOGE(TAG, "PTT end failed: %s (0x%x)",
                     esp_err_to_name(ptt_err), (unsigned)ptt_err);
        } else {
            if (s_display_active)
                (void)display_service_set_voice_state(
                    DISPLAY_VOICE_THINKING);
            ESP_LOGI(TAG, "PTT recording submitted to cloud worker");
        }
        return ptt_err;
    }
    if (event->event_type == BUTTON_EVENT_CLICK) {
        /* BTN1 短按 → START；BTN2 短按 → STOP（与旧 cancel 语义一致）。
         * 均经 program_control 仲裁后访问 executor。 */
        if (event->button_id == BUTTON_ID_1) {
            pc_btn1_action_t ba = PC_BTN1_ACTION_NONE;
            return program_control_btn1(PC_BTN1_EV_CLICK, &ba);
        }
        pc_action_t action = PC_ACTION_NONE;
        return program_control_request(PC_REQ_STOP, &action);
    }
    return ESP_OK;
}

#if XIAOJING_PHASE9_AUTOTEST
#ifndef PHASE9_AUTOTEST_CYCLES
#define PHASE9_AUTOTEST_CYCLES 20U
#endif
static void phase9_autotest_task(void *arg)
{
    (void)arg;
    /* Let Wi-Fi/BLE/display and the capture task settle before exercising
     * the shared I2S pins. */
    vTaskDelay(pdMS_TO_TICKS(8000U));

    for (uint32_t cycle = 1U; cycle <= PHASE9_AUTOTEST_CYCLES; ++cycle) {
        button_sink_event_t ev = {
            .button_id = BUTTON_ID_1,
            .event_type = BUTTON_EVENT_LONG_PRESS,
            .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000),
            .sequence = cycle,
            .stable_mask = 0U,
        };
        esp_err_t switch_err = button_normal_sink(&ev, 3000U, NULL);
        ESP_LOGI(TAG, "PHASE9_AUTOTEST switch[%u] err=0x%x",
                 (unsigned)cycle, (unsigned)switch_err);

        /* The backend switch plays its confirmation prompt.  Add a short
         * deterministic tone so TX->RX recovery is exercised even when a
         * prompt asset is unavailable. */
        vTaskDelay(pdMS_TO_TICKS(1500U));
        esp_err_t tone_err = s_real_hal
            ? real_hal_audio_play_tone(s_real_hal, 440U, 120U, 1200)
            : ESP_ERR_INVALID_STATE;
        ESP_LOGI(TAG, "PHASE9_AUTOTEST tone[%u] err=0x%x",
                 (unsigned)cycle, (unsigned)tone_err);

        /* Capture start is the RX-DMA recovery boundary. */
        vTaskDelay(pdMS_TO_TICKS(250U));
        esp_err_t rx_err = s_voice_audio
            ? audio_driver_start_rx(s_voice_audio)
            : ESP_ERR_INVALID_STATE;
        ESP_LOGI(TAG, "PHASE9_AUTOTEST rx_restart[%u] err=0x%x",
                 (unsigned)cycle, (unsigned)rx_err);
        vTaskDelay(pdMS_TO_TICKS(2500U));
    }
    ESP_LOGI(TAG, "PHASE9_AUTOTEST COMPLETE");
    vTaskDelete(NULL);
}
#endif

static esp_err_t button_urgent_sink(const button_sink_event_t *event,
                                      uint32_t timeout_ms, void *context) {
    (void)timeout_ms; (void)context;
    pc_action_t action = PC_ACTION_NONE;
    if (event->event_type == BUTTON_EVENT_LONG_PRESS) {
        /* BTN3 长按 → 经 program_control → 急停（safety_manager 闩锁） */
        return program_control_request(PC_REQ_EMERGENCY, &action);
    }
    if (event->event_type == BUTTON_EVENT_CLICK) {
        /* BTN3 短按 → 经 program_control → STOP */
        return program_control_request(PC_REQ_STOP, &action);
    }
    return ESP_OK;
}

static esp_err_t gesture_display_sink(hal_gesture_t gesture, void *context)
{
    (void)context;
    /* UP→START / DOWN→STOP 经 program_control 执行；左右→翻页仍走显示。
     * 动作已含冷却 + 尾串抑制（program_control 纯模型）。 */
    pc_gesture_action_t action = PC_GESTURE_ACTION_NONE;
    esp_err_t err = program_control_gesture(gesture, &action);
    if (err != ESP_OK) return err;
    switch (action) {
    case PC_GESTURE_ACTION_PAGE_NEXT:
        if (!s_display_active) return ESP_ERR_INVALID_STATE;
        ESP_LOGI(TAG, "gesture %u -> next display page", (unsigned)gesture);
        return display_service_step_page(1);
    case PC_GESTURE_ACTION_PAGE_PREV:
        if (!s_display_active) return ESP_ERR_INVALID_STATE;
        ESP_LOGI(TAG, "gesture %u -> previous display page", (unsigned)gesture);
        return display_service_step_page(-1);
    case PC_GESTURE_ACTION_START:
        ESP_LOGI(TAG, "gesture %u -> START demo", (unsigned)gesture);
        return ESP_OK;   /* 已由 program_control 提交 DEFAULT_DEMO_V1 */
    case PC_GESTURE_ACTION_STOP:
        ESP_LOGI(TAG, "gesture %u -> STOP program", (unsigned)gesture);
        return ESP_OK;
    default:
        ESP_LOGI(TAG, "gesture %u observed (no control action)",
                 (unsigned)gesture);
        return ESP_OK;
    }
}

/* ---- NVS ---- */

static esp_err_t init_nvs(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    return err;
}

static void destroy_hal(void) {
    if (s_turbidity_sensor) {
        turbidity_sensor_destroy(s_turbidity_sensor);
        s_turbidity_sensor = NULL;
    }
    if (s_fake_hal) { fake_hal_destroy(s_fake_hal); s_fake_hal = NULL; s_hal = NULL; }
    if (s_real_hal) { real_hal_destroy(s_real_hal); s_real_hal = NULL; s_hal = NULL; }
}

/* ---- Rollback state (init vs started) ---- */

typedef struct {
    bool nvs, config, status_store, event_queue;
    bool hal;
    bool global_position, global_water, global_dry, global_drain;
    bool global_detergent, global_uv, global_button;
    bool global_voice;
    bool global_coupled_hot_air;
    bool display_init, display_started;
    bool gesture_init, gesture_started;
    bool wifi_init, wifi_started;
    bool voice_audio_init, voice_capture_init;
    bool voice_cloud_init, voice_cloud_started;
    bool safety_init, safety_started;
    bool position_init, position_started;
    bool bl50_init, bl50_started;
    bool water_init, water_started;
    bool dry_init, dry_started;
    bool coupled_hot_air_init, coupled_hot_air_started;
    bool drain_init, drain_started;
    bool detergent_init, detergent_started;
    bool uv_init, uv_started;
    bool uv_self_test_init, uv_self_test_started;
    bool actuator_self_test_init, actuator_self_test_started;
    bool motor_bench_init, motor_bench_started;
    bool position_swing_started;
    bool program_control_started;
    bool button_init, button_started;
    bool adapter_consumer_started;
    bool executor_started;
    bool voice_initialized;
    bool voice_started;
    bool protocol_initialized;
    bool protocol_started;
    bool ble_initialized;
    bool ble_started;
} init_state_t;

/* ================================================================
 * Init — with goto rollback
 * Order: NVS→Config→Store→Global→EventQ→HAL→Safety→Position→BL50→
 *        Water→Dry→Drain→Detergent→UV→Button→Executor
 * ================================================================ */

esp_err_t app_bootstrap_init(app_bootstrap_result_t *result) {
    if (!result) return ESP_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));
    s_voice_service_active = false;

    init_state_t st = {0};
    esp_err_t err;

    log_heap("boot_start");

    err = init_nvs();
    if (err != ESP_OK) goto fail;
    st.nvs = true; result->nvs_ok = true;

    /* Reserve the internal-stack NVS worker before BLE/Wi-Fi/voice consume
     * the contiguous internal heap.  BTN1 backend persistence then remains
     * available even while ESP-SR is active. */
    err = start_backend_nvs_worker();
    if (err != ESP_OK) goto fail;

    /* Log test mode */
#if XIAOJING_TEST_MODE == 3
    ESP_LOGI(TAG, "TEST_MODE=3 (WiFi only, no BLE, no Edge)");
#elif XIAOJING_TEST_MODE == 2
    ESP_LOGI(TAG, "TEST_MODE=2 (BLE + WiFi, no Edge)");
#else
    ESP_LOGI(TAG, "TEST_MODE=1 (BLE + Edge + WiFi)");
#endif

    err = voice_credentials_global_init();
    if (err != ESP_OK) goto fail;

    err = seed_demo_credentials_if_absent();
    if (err != ESP_OK) goto fail;

    err = machine_config_init();
    if (err != ESP_OK || !machine_config_validate(machine_config_get())) { err = ESP_ERR_INVALID_STATE; goto fail; }
    st.config = true; result->config_ok = true;

    err = machine_status_store_init();
    if (err != ESP_OK) goto fail;
    st.status_store = true; result->status_store_ok = true;

    err = position_service_global_init();
    if (err != ESP_OK) goto fail;
    st.global_position = true;
    err = water_service_global_init();
    if (err != ESP_OK) goto fail;
    st.global_water = true;
    err = dry_service_global_init();
    if (err != ESP_OK) goto fail;
    st.global_dry = true;
    err = coupled_hot_air_service_global_init();
    if (err != ESP_OK) goto fail;
    st.global_coupled_hot_air = true;
    err = drain_service_global_init();
    if (err != ESP_OK) goto fail;
    st.global_drain = true;
    err = detergent_service_global_init();
    if (err != ESP_OK) goto fail;
    st.global_detergent = true;
    err = uv_service_global_init();
    if (err != ESP_OK) goto fail;
    st.global_uv = true;
    err = button_service_global_init();
    if (err != ESP_OK) goto fail;
    st.global_button = true;
    err = voice_service_global_init();
    if (err != ESP_OK) goto fail;
    st.global_voice = true;

    /* Initialize the display model early, but defer LCD/LVGL allocation
     * until Wi-Fi and BLE have reserved their contiguous internal memory. */
    err = display_service_init();
    if (err != ESP_OK) goto fail;
    st.display_init = true;
    err = display_service_prepare();
    if (err != ESP_OK) goto fail;

    s_event_queue = xQueueCreate(APP_BOOTSTRAP_EVENT_QUEUE_DEPTH, sizeof(machine_event_t));
    if (!s_event_queue) { err = ESP_ERR_NO_MEM; goto fail; }
    st.event_queue = true;

    /* HAL */
    if (board_config_get_output_mode() == XIAOJING_MODE_FAKE) {
        s_fake_hal = fake_hal_create();
        if (!s_fake_hal) { err = ESP_ERR_NO_MEM; goto fail; }
#if defined(CONFIG_XIAOJING_FAKE_RUNTIME_SIM)
        /*
         * Interactive FAKE firmware needs a moving clock and released
         * active-low inputs. Unit tests do not enable this Kconfig option
         * and retain full manual control over virtual time and sensors.
         */
        fake_hal_enable_realtime_clock(s_fake_hal, 1000);
        mcp_input_snapshot_t mcp = {
            .gpio_a = 0xFF,
            .gpio_b = (uint8_t)(0xFF
                & ~(1U << XIAOJING_MCP_GPB1_HALL_45)
                & ~(1U << XIAOJING_MCP_GPB5_HALL_BLDC)),
        };
        fake_hal_set_mcp(s_fake_hal, &mcp);
        fake_hal_set_button_state(s_fake_hal, 0xFF);
        sht_sample_t sht = {
            .temperature_c = 25.0f,
            .humidity_rh = 50.0f,
            .valid = true,
            .timestamp_ms = 1000,
            .age_ms = 0,
        };
        fake_hal_set_sht(s_fake_hal, &sht);
#endif
        s_hal = fake_hal_get_interface(s_fake_hal);
    } else {
        s_real_hal = real_hal_create();
        if (!s_real_hal) { err = ESP_ERR_NOT_SUPPORTED; goto fail; }
        err = real_hal_set_ibt2_calibration(s_real_hal,
            machine_config_get()->position.direction_calibrated,
            machine_config_get()->position.rpwm_is_cw);
        if (err != ESP_OK) { destroy_hal(); goto fail; }
        s_hal = real_hal_get_interface(s_real_hal);

        /* Audio output diagnostics are explicit operations.  Normal boot must
         * not block voice capture, Wi-Fi, BLE, or safety startup. */
    }
    result->hal = s_hal;
    st.hal = true;

    /* Safety manager */
    {
        safety_manager_config_t sm_cfg = {
            .hal = s_hal, .config = machine_config_get(),
            .output_mode = board_config_get_output_mode(),
            .ptc_enabled = board_config_is_ptc_enabled(),
            .hot_air_module_enabled = board_config_is_hot_air_module_enabled(),
            .hot_air_cooldown_ms = COUPLED_HOT_AIR_COOLDOWN_MS,
            .board_identity_confirmed = board_config_is_board_identity_confirmed(),
        };
        err = safety_manager_init(&sm_cfg);
        if (err != ESP_OK) goto fail;
        st.safety_init = true;
        err = safety_manager_start();
        if (err != ESP_OK) goto fail;
        st.safety_started = true; result->safety_mgr_ok = true;
    }

    /* Position service */
    {
        const position_params_t *pp = &machine_config_get()->position;
        position_service_config_t ps_cfg = {
            .default_policy = pp->default_policy, .move_pwm_percent = pp->move_pwm_percent,
            .approach_pwm_percent = pp->approach_pwm_percent, .debounce_ms = pp->debounce_ms,
            .move_timeout_ms = pp->move_timeout_ms, .brake_ms = pp->brake_ms,
            .rpwm_is_cw = pp->rpwm_is_cw, .direction_calibrated = pp->direction_calibrated,
        };
        err = position_service_init(&ps_cfg, s_hal, make_event_sink());
        if (err != ESP_OK) goto fail;
        st.position_init = true;
        err = position_service_start();
        if (err != ESP_OK) goto fail;
        st.position_started = true; result->position_svc_ok = true;
    }

    /* BL50 service */
    {
        bl50_service_config_t b50_cfg = {
            .hal = s_hal, .config = machine_config_get(),
            .event_sink = make_event_sink(),
        };
        err = bl50_service_init(&b50_cfg);
        if (err != ESP_OK) goto fail;
        st.bl50_init = true;
        err = bl50_service_start();
        if (err != ESP_OK) goto fail;
        st.bl50_started = true; result->bl50_svc_ok = true;
    }

    /* Water service */
    {
        const water_params_t *wp = &machine_config_get()->water;
        water_service_config_t ws_cfg = {
            .pulses_per_liter = wp->pulses_per_liter, .no_flow_timeout_ms = wp->no_flow_timeout_ms,
            .low_flow_window_ms = wp->low_flow_window_ms, .low_flow_min_pulses = 0,
            .source_batch_target_pulses = wp->source_batch_target_pulses,
            .source_batch_max_ms = wp->source_batch_max_ms, .source_settle_ms = wp->source_settle_ms,
            .transfer_timeout_ms = wp->transfer_timeout_ms, .default_transfer_ms = wp->default_transfer_ms,
            .max_fill_cycles = wp->max_fill_cycles, .total_inlet_timeout_ms = wp->total_inlet_timeout_ms,
        };
        err = water_service_init(&ws_cfg, s_hal, make_event_sink());
        if (err != ESP_OK) goto fail;
        st.water_init = true;
        err = water_service_start();
        if (err != ESP_OK) goto fail;
        st.water_started = true; result->water_svc_ok = true;
    }

    /* Dry service */
    {
        const dry_params_t *dp = &machine_config_get()->dry;
        err = dry_service_init(dp, s_hal, make_event_sink());
        if (err != ESP_OK) goto fail;
        st.dry_init = true;
        err = dry_service_start();
        if (err != ESP_OK) goto fail;
        st.dry_started = true; result->dry_svc_ok = true;
    }

    /* Coupled hot-air module service (single relay, fan+heater parallel) */
    {
        err = coupled_hot_air_service_init(s_hal, make_event_sink());
        if (err != ESP_OK) goto fail;
        st.coupled_hot_air_init = true;
        err = coupled_hot_air_service_start();
        if (err != ESP_OK) goto fail;
        st.coupled_hot_air_started = true;
        result->coupled_hot_air_svc_ok = true;
    }

    /* Drain service */
    {
        const drain_params_t *drp = &machine_config_get()->drain;
        err = drain_service_init(drp, s_hal, make_event_sink());
        if (err != ESP_OK) goto fail;
        st.drain_init = true;
        err = drain_service_start();
        if (err != ESP_OK) goto fail;
        st.drain_started = true; result->drain_svc_ok = true;
    }

    /* Detergent service */
    {
        const detergent_params_t *dtp = &machine_config_get()->detergent;
        err = detergent_service_init(dtp, s_hal, make_event_sink());
        if (err != ESP_OK) goto fail;
        st.detergent_init = true;
        err = detergent_service_start();
        if (err != ESP_OK) goto fail;
        st.detergent_started = true; result->detergent_svc_ok = true;
    }

    /* UV service */
    {
        const uv_params_t *uvp = &machine_config_get()->uv;
        err = uv_service_init(uvp, s_hal, make_event_sink());
        if (err != ESP_OK) goto fail;
        st.uv_init = true;
        err = uv_service_start();
        if (err != ESP_OK) goto fail;
        st.uv_started = true; result->uv_svc_ok = true;
    }

    /* Button service — uses HAL (not MCP), works in both FAKE and REAL mode.
     * Button is the local cancel/urgent entry point — init/start failure is fatal. */
    /* Executor must exist before any local or remote producer starts. */
    {
        err = wash_exec_bootstrap_create((void *)s_event_queue);
        if (err != ESP_OK) goto fail;
        result->executor = wash_exec_bootstrap_get_executor();
        result->adapter_ctx = wash_exec_bootstrap_get_adapter_ctx();
        st.adapter_consumer_started = true;
        st.executor_started = true; result->executor_ok = true;

        /* Start task completion prompt timer */
        esp_err_t tc_err = start_task_complete_worker();
        if (tc_err != ESP_OK)
            ESP_LOGW(TAG, "Task completion timer start failed: 0x%x", tc_err);
    }

    /* UV self-test (development/bench diagnostic). Requires the executor for
     * the machine-IDLE gate; started after executor creation so the gate can
     * never race a half-initialized executor. */
    {
        uv_self_test_config_t st_cfg = { .executor = result->executor };
        err = uv_self_test_init(&st_cfg);
        if (err != ESP_OK) goto fail;
        st.uv_self_test_init = true;
        err = uv_self_test_start();
        if (err != ESP_OK) goto fail;
        st.uv_self_test_started = true;
    }

    /* Actuator board test (source/transfer valve, detergent pump). Same
     * machine-IDLE gate; all real outputs go through safety_manager via the
     * owning services.  Device-side auto OFF is guaranteed by those services. */
    {
        actuator_self_test_config_t at_cfg = { .executor = result->executor };
        err = actuator_self_test_init(&at_cfg);
        if (err != ESP_OK) goto fail;
        st.actuator_self_test_init = true;
        err = actuator_self_test_start();
        if (err != ESP_OK) goto fail;
        st.actuator_self_test_started = true;
    }

    /* Motor bench (pulsator/drum no-load bench test, diagnostic only).
     * Never auto-energizes: each step waits for the user to place the tub at
     * the production-contract position and explicitly confirm one ≤300ms
     * low-gear pulse.  Position motor (IBT-2) is never driven here. */
    {
        motor_bench_service_config_t mb_cfg = { .executor = result->executor };
        err = motor_bench_service_global_init();
        if (err != ESP_OK) goto fail;
        err = motor_bench_service_init(&mb_cfg);
        if (err != ESP_OK) goto fail;
        st.motor_bench_init = true;
        err = motor_bench_service_start();
        if (err != ESP_OK) goto fail;
        st.motor_bench_started = true;
    }

    /* Position swing（连续换向展示台，诊断）。IBT-2 直接驱动仅诊断，
     * 门禁复用 executor/自检/位置校准，运行期间锁死洗涤提交。 */
    {
        position_swing_service_config_t sw_cfg = {
            .executor = result->executor,
        };
        err = position_swing_global_init();
        if (err != ESP_OK) goto fail;
        err = position_swing_service_init(&sw_cfg);
        if (err != ESP_OK) goto fail;
        err = position_swing_set_hal(s_hal);
        if (err != ESP_OK) goto fail;
        st.position_swing_started = true;
        err = position_swing_service_start();
        if (err != ESP_OK) goto fail;
    }

    /* Program control（DEFAULT_DEMO_V1 多输入仲裁）。在所有服务启动后、
     * 按钮/手势/协议接入前初始化：审计 provider 依赖各服务快照，按钮/手势
     * 依赖本服务已完成初始化。 */
    {
        program_control_service_config_t pc_cfg = {
            .executor = result->executor,
            .get_config = machine_config_get,
            .is_voice_active = pc_is_voice_active,
            .gesture_cooldown_ms = 1000,
            .emergency = pc_emergency_cb,
            .emergency_ctx = NULL,
            .backend = pc_backend_switch_cb,
            .backend_ctx = NULL,
            .audit_provider = pc_audit_provider,
            .audit_ctx = NULL,
        };
        err = program_control_global_init();
        if (err != ESP_OK) goto fail;
        err = program_control_service_init(&pc_cfg);
        if (err != ESP_OK) goto fail;
        err = program_control_service_start();
        if (err != ESP_OK) goto fail;
        st.program_control_started = true;
    }

    /* Phase 9 voice chain. Direct MiMo is the default; AI Gateway remains an
     * optional runtime-selectable provider.  Both share the same local
     * AUTO_ROUTE authorization and never receive raw actuator authority. */
    if (board_config_is_voice_enabled()) {
        const xiaojing_hal_t *voice_capture_hal = NULL;
        wifi_station_config_t wifi_cfg = {
            .nvs_namespace = WIFI_STATION_DEFAULT_NAMESPACE,
            /* Association can take longer while NimBLE and ESP-SR are
             * reserving radio/heap resources.  Keep the Wi-Fi driver alive
             * long enough for the AP handshake instead of letting the async
             * starter tear it down at the first transient timeout. */
            .connect_timeout_ms = 120000,
            .max_retries = 5,
            .connection_sink = wifi_connection_sink,
            .connection_context = NULL,
        };
        err = xiaojing_wifi_init(&wifi_cfg);
        if (err != ESP_OK) goto fail;
        st.wifi_init = true;
        s_wifi_active = true;
        result->wifi_station_ok = true;

        /* I2S DMA descriptors must come from contiguous internal RAM.  Reserve
         * the real capture channel before BLE/Wi-Fi fragment that heap, then
         * disable it while retaining the channel and DMA allocation.  The
         * audio controller later re-enables this persistent I2S1 channel. */
        err = create_voice_audio_hal_if_needed(&voice_capture_hal);
        if (err != ESP_OK) goto fail;
        st.voice_audio_init = s_voice_audio != NULL;
        if (s_voice_audio) {
            err = voice_audio_start_capture();
            if (err != ESP_OK) goto fail;
            err = voice_audio_stop_capture();
            if (err != ESP_OK) goto fail;
            log_heap("after_audio_dma_prepare");
        }

        /* Start LVGL after the capture DMA reservation but before BLE/Wi-Fi
         * and AFE allocations.  This preserves a contiguous DMA block for
         * INMP441 while also leaving enough internal heap for LVGL's TCB. */
        if (!s_display_active) {
            esp_err_t display_start_err = display_service_start();
            if (display_start_err == ESP_OK) {
                st.display_started = true;
                s_display_active = true;
                result->display_svc_ok = true;
                ESP_LOGI(TAG, "Display/LVGL started after audio DMA reserve");
            } else {
                ESP_LOGW(TAG, "Display start unavailable (0x%x)",
                         (unsigned)display_start_err);
            }
        }
        /* Do not start the telemetry task while the radio/voice stacks are
         * still being brought up.  Its task/queue bookkeeping consumes the
         * last small internal-heap fragments on ESP32-S3 and can make a
         * later non-essential init fail, which then rolls the whole bootstrap
         * back (and leaves the display blank).  LVGL already has its own
         * refresh task; telemetry is started lazily in the post-radio block
         * below, where failure is non-fatal. */

        /* Start Wi-Fi before BLE.  esp_wifi_start() allocates the static RX/TX
         * pools from internal RAM; doing it after NimBLE has fragmented that
         * heap can leave only one of the two required RX buffers available.
         * The connection is asynchronous, so this does not block bootstrap.
         * BLE is started immediately afterwards and keeps its own PSRAM-backed
         * host allocations where possible. */
        wifi_station_snapshot_t wifi_snapshot;
        err = xiaojing_wifi_get_snapshot(&wifi_snapshot);
        if (err != ESP_OK) goto fail;
        result->wifi_provisioned = wifi_snapshot.provisioned;
        result->wifi_start_error = ESP_OK;
        if (wifi_snapshot.provisioned) {
            err = xiaojing_wifi_start_async();
            result->wifi_start_error = err;
            if (err == ESP_OK) {
                st.wifi_started = true;
                result->wifi_start_requested = true;
            } else {
                ESP_LOGW(TAG,
                         "Wi-Fi async start failed (0x%x); BLE/local control remain available",
                         err);
            }
        } else {
            ESP_LOGW(TAG,
                     "Wi-Fi credentials absent; BLE/local control remain available");
        }
        log_heap("after_wifi_driver_start");

        /* BLE controller init follows Wi-Fi so both subsystems get their
         * required internal buffers while the heap is still contiguous. */
        #if 0 /* BLE starts after the voice/AFE chain below. */
        if (xiaojing_ble_transport_is_supported() && !s_diag_disable_ble) {
            log_heap("before_ble");
            ble_transport_config_t ble_cfg = {
                .device_name = BLE_TRANSPORT_DEVICE_NAME_DEFAULT,
                .frame_sink = ble_frame_sink,
                .secure_frame_sink = ble_secure_frame_sink,
                .connection_sink = ble_connection_sink,
                .context = NULL,
            };
            err = xiaojing_ble_transport_init(&ble_cfg);
            if (err != ESP_OK) goto fail;
            st.ble_initialized = true;
            /* Start BLE now — nimble_port_init() allocates the DMA block
             * before WiFi fragments the heap. */
            err = xiaojing_ble_transport_start();
            if (err != ESP_OK) goto fail;
            st.ble_started = true;
            s_ble_active = true;
            result->ble_transport_ok = true;
            log_heap("after_ble");
        } else if (s_diag_disable_ble) {
            ESP_LOGI(TAG, "BLE DISABLED by diagnostic flag");
        }

        log_heap("after_ble");
        #endif

        voice_cloud_idf_context_init(&s_voice_cloud_context);
        memset(&s_effective_voice_credentials, 0,
               sizeof(s_effective_voice_credentials));
        esp_err_t stored_credentials_error = voice_credentials_load(
            &s_effective_voice_credentials);
        if (stored_credentials_error != ESP_OK) {
            if (stored_credentials_error != ESP_ERR_NVS_NOT_FOUND) {
                ESP_LOGW(TAG,
                         "Stored voice credentials are unusable (0x%x); "
                         "voice remains configurable and local control continues",
                         stored_credentials_error);
            }
            memset(&s_effective_voice_credentials, 0,
                   sizeof(s_effective_voice_credentials));
            s_effective_voice_credentials.provider = XIAOJING_VOICE_PROVIDER;
            size_t key_length = strlen(XIAOJING_VOICE_API_KEY);
            size_t model_length = strlen(XIAOJING_VOICE_MODEL);
            if (key_length >= sizeof(s_effective_voice_credentials.api_key) ||
                model_length >= sizeof(s_effective_voice_credentials.model)) {
                err = ESP_ERR_INVALID_SIZE;
                goto fail;
            }
            memcpy(s_effective_voice_credentials.api_key,
                   XIAOJING_VOICE_API_KEY, key_length + 1U);
            memcpy(s_effective_voice_credentials.model,
                   XIAOJING_VOICE_MODEL, model_length + 1U);
        }
        voice_provider_credentials_t credentials = {
            .provider = s_effective_voice_credentials.provider,
            .api_key = s_effective_voice_credentials.api_key,
            .model_override = s_effective_voice_credentials.model,
        };
        voice_cloud_config_t cloud_cfg;
        voice_provider_result_t provider_result =
            voice_provider_build_cloud_config(
                &credentials, voice_cloud_idf_transport,
                &s_voice_cloud_context, voice_network_ready, NULL,
                &cloud_cfg);
        result->voice_credentials_ready =
            provider_result == VOICE_PROVIDER_OK;
        if (provider_result == VOICE_PROVIDER_API_KEY_REQUIRED) {
            ESP_LOGW(TAG,
                     "Voice cloud dormant: provision a provider API key");
            (void)voice_credentials_open_provisioning_window(300000U);
            ESP_LOGW(TAG,
                     "First-time encrypted credential window open for 5 minutes");
        } else if (provider_result != VOICE_PROVIDER_OK) {
            ESP_LOGE(TAG, "voice provider config rejected (%d)",
                     (int)provider_result);
            err = ESP_ERR_INVALID_STATE;
            goto fail;
        }
        if (result->voice_credentials_ready) {
            err = voice_capture_idf_init(
                voice_capture_hal, VOICE_CAPTURE_DEFAULT_PCM_BYTES);
            if (err != ESP_OK) goto fail;
            st.voice_capture_init = true;
            s_voice_capture_active = true;

            voice_cloud_adapter_config_t cloud_adapter_cfg = {
                .cloud = cloud_cfg,
                .record_start = voice_capture_idf_start,
                .record_finish = voice_capture_idf_finish,
                .record_cancel = voice_capture_idf_cancel,
                .record_release = voice_capture_idf_release,
                .cloud_cancel = voice_cloud_cancel_adapter,
                .completion = voice_cloud_ui_completion,
                .record_context = NULL,
                .task_stack_bytes = 32768,
                .task_priority = 5,
                .task_core = 0,
            };
            err = voice_cloud_adapter_init(&cloud_adapter_cfg);
            if (err != ESP_OK) goto fail;
            st.voice_cloud_init = true;
            err = voice_cloud_adapter_start();
            if (err != ESP_OK) goto fail;
            st.voice_cloud_started = true;
            s_voice_cloud_active = true;

            log_heap("before_voice_frontend");

            /* Initialize voice frontend with wake backends.
             * Backends are lazy-init: only the selected backend allocates
             * resources when start_backend() is called. */
            voice_frontend_config_t fe_cfg = {
                .default_backend = s_diag_disable_edge ?
                    VOICE_WAKE_BACKEND_ESP_SR : VOICE_WAKE_BACKEND_EDGE_IMPULSE,
                .detection_callback = wake_detection_callback,
                .detection_context = NULL,
                .i2s_sample_rate = 16000,
                .frame_samples = 512,
                .stack_size = 4096,
                .task_core = 1,
                .task_priority = 4,
            };
            err = voice_frontend_init(&fe_cfg);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "voice_frontend init failed: 0x%x", err);
            } else {
                if (!s_diag_disable_edge) {
                    voice_frontend_register_backend(
                        VOICE_WAKE_BACKEND_EDGE_IMPULSE,
                        voice_wake_edge_get_ops());
                } else {
                    ESP_LOGI(TAG, "Edge Impulse DISABLED by diagnostic flag");
                }
                voice_frontend_register_backend(
                    VOICE_WAKE_BACKEND_ESP_SR,
                    voice_wake_esp_sr_get_ops());
                ESP_LOGI(TAG, "voice_frontend initialized with %s",
                         s_diag_disable_edge ? "1 backend (ESP-SR only)" :
                         "2 backends");
            }

            log_heap("after_voice_frontend_register");

            /* Initialize voice recorder
             * Buffer size = sample_rate * channels * bytes_per_sample * max_duration_ms / 1000
             * 16000 * 1 * 2 * 10000 / 1000 = 320000 bytes
             * Add 1024 bytes margin for safety */
            voice_recorder_config_t rec_cfg = {
                .max_pcm_bytes = 320U * 1024U + 1024U,  /* ~321KB for 10s at 16kHz/16bit/mono */
                .max_duration_ms = 10000,
                .min_speech_ms = 300,
                .vad_timeout_ms = 1100,
                .empty_timeout_ms = 2500,
                .sample_rate = 16000,
            };
            err = voice_recorder_init(&rec_cfg);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "voice_recorder_init failed: 0x%x", err);
                goto fail;
            }
            ESP_LOGI(TAG, "Voice recorder initialized");

            /* Initialize audio controller (single I2S owner) */
            audio_controller_config_t ac_cfg = {
                .i2s_read = voice_audio_read_pcm,
                .i2s_start = voice_audio_start_capture,
                .i2s_stop = voice_audio_stop_capture,
                .wake_feed = wake_feed_wrapper,
                .wake_context = NULL,
                .dialog_feed = dialog_feed_wrapper,
                .dialog_context = NULL,
                .dialog_begin = dialog_begin_wrapper,
                .dialog_begin_context = NULL,
                .dialog_end = dialog_end_wrapper,
                .dialog_end_context = NULL,
                .dialog_check_terminate = dialog_check_terminate_wrapper,
                .dialog_check_terminate_context = NULL,
                .frame_samples = 512,
                .event_queue_depth = 8,
                .wake_cooldown_ms = 2000,
                .stack_size = 4096,
                .task_core = 1,
                .task_priority = 5,
            };
            err = audio_controller_init(&ac_cfg);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "audio_controller_init failed: 0x%x", err);
                goto fail;
            }
            err = audio_controller_start();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "audio_controller_start failed: 0x%x", err);
                goto fail;
            }
            ESP_LOGI(TAG, "Audio controller started");

            voice_wake_backend_t default_wake_backend = load_wake_backend();
            if (s_diag_disable_edge)
                default_wake_backend = VOICE_WAKE_BACKEND_ESP_SR;
            atomic_store_explicit(&s_requested_wake_backend,
                                  (int)default_wake_backend,
                                  memory_order_release);
            voice_service_config_t voice_cfg = {
                .executor = result->executor,
                .machine_config = machine_config_get(),
                .default_backend = default_wake_backend,
                .select_backend = bootstrap_select_backend,
                .start_frontend = bootstrap_start_frontend,
                .stop_frontend = bootstrap_stop_frontend,
                .start_recording =
                    voice_cloud_adapter_start_recording,
                .stop_recording_and_upload =
                    voice_cloud_adapter_stop_recording_and_upload,
                .cancel_io = voice_cloud_adapter_cancel_io,
                .play_prompt = voice_placeholder_prompt,
                .speak_text = voice_cloud_speech,
                .set_route_context =
                    voice_cloud_adapter_set_route_context,
                .min_control_confidence_milli = 750,
            };
            err = voice_service_init(&voice_cfg);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "voice_service_init failed: 0x%x", (unsigned)err);
                goto fail;
            }
            st.voice_initialized = true;
            err = voice_service_start();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "voice_service_start failed: 0x%x", (unsigned)err);
                goto fail;
            }
            st.voice_started = true;
            s_voice_service_active = true;
            result->voice_svc_ok = true;

            log_heap("after_voice_service_start");

            /* BLE starts after the voice/AFE chain.  Wi-Fi has already
             * reserved its static buffers and ESP-SR prefers PSRAM, so this
             * ordering preserves enough internal RAM for audio and NimBLE. */
            if (xiaojing_ble_transport_is_supported() && !s_diag_disable_ble) {
                log_heap("before_ble");
                ble_transport_config_t ble_cfg = {
                    .device_name = BLE_TRANSPORT_DEVICE_NAME_DEFAULT,
                    .frame_sink = ble_frame_sink,
                    .secure_frame_sink = ble_secure_frame_sink,
                    .connection_sink = ble_connection_sink,
                    .context = NULL,
                };
                err = xiaojing_ble_transport_init(&ble_cfg);
                if (err != ESP_OK) goto fail;
                st.ble_initialized = true;
                err = xiaojing_ble_transport_start();
                if (err != ESP_OK) goto fail;
                st.ble_started = true;
                s_ble_active = true;
                result->ble_transport_ok = true;
                log_heap("after_ble");
            } else if (s_diag_disable_ble) {
                ESP_LOGI(TAG, "BLE DISABLED by diagnostic flag");
            }

#if VOICE_CLOUD_DIAGNOSTIC_MODE
            voice_cloud_diag_start_ex(
                VOICE_MIMO_ENDPOINT,
                s_effective_voice_credentials.api_key[0]
                    ? s_effective_voice_credentials.api_key : NULL);
#endif
        }
    }

    {
        button_service_config_t btn_cfg = {
            .debounce_ms = 30,
            .poll_interval_ms = 200,
            .long_press_ms = 2000,
            .long_press_mask = board_config_is_voice_enabled() ?
                (uint8_t)((1U << BUTTON_RAW_BIT_BTN1) |
                          (1U << BUTTON_RAW_BIT_BTN2) |
                          (1U << BUTTON_RAW_BIT_BTN3)) : 0,
            .long_press_ms_by_button = {
                [BUTTON_ID_1] = BTN_LONG_PRESS_DEFAULT_MS,
                [BUTTON_ID_2] = BTN2_PTT_LONG_PRESS_MS,
            },
            .hal = s_hal,
            .enable_internal_pullup = true,
            .normal_sink = { .publish = button_normal_sink, .context = NULL },
            .urgent_sink = { .publish = button_urgent_sink, .context = NULL },
        };
        err = button_service_init(&btn_cfg);
        if (err != ESP_OK) goto fail;
        st.button_init = true;
        err = button_service_start();
        if (err != ESP_OK) goto fail;
        st.button_started = true; result->button_svc_ok = true;
    }

    /* Protocol owns planning/routing; BLE owns framing and GATT only. */
    {
        app_protocol_config_t protocol_cfg = {
            .executor = result->executor,
            .machine_config = machine_config_get(),
            .legacy_enabled = board_config_is_legacy_ble_enabled(),
            .critical_tx = protocol_tx_critical,
            .status_tx = protocol_tx_status,
            .provision_wifi = board_config_is_voice_enabled()
                ? protocol_provision_wifi : NULL,
            .uv_self_test = protocol_uv_self_test,
            .uv_self_test_state = protocol_uv_self_test_state,
            .actuator_self_test = protocol_actuator_self_test,
            .actuator_self_test_snapshot = protocol_actuator_self_test_snapshot,
            .motor_bench_start = protocol_motor_bench_start,
            .motor_bench_confirm = protocol_motor_bench_confirm,
            .motor_bench_cancel = protocol_motor_bench_cancel,
            .position_move_test = protocol_position_move_test,
            .position_swing_test = protocol_position_swing_test,
            .position_swing_cancel = protocol_position_swing_cancel,
            .position_swing_snapshot = protocol_position_swing_snapshot,
            .motor_bench_snapshot = protocol_motor_bench_snapshot,
            .provision_context = NULL,
            .tx_context = NULL,
        };
        err = app_protocol_init(&protocol_cfg);
        if (err != ESP_OK) goto fail;
        st.protocol_initialized = true;
        err = app_protocol_start();
        if (err != ESP_OK) goto fail;
        st.protocol_started = true;
        s_protocol_active = true;
        result->app_protocol_ok = true;
    }

    /* Start the observer UI after the radio stacks. ESP32-S3 BLE needs a
     * sizeable contiguous internal allocation during HCI initialization;
     * starting LVGL first can fragment that region when Wi-Fi is enabled. */
    err = display_service_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Display unavailable (0x%x); control and radio services remain active",
                 err);
    } else {
        st.display_started = true;
        s_display_active = true;
        result->display_svc_ok = true;
        s_turbidity_sensor = turbidity_sensor_init();
        if (!s_turbidity_sensor) {
            ESP_LOGW(TAG, "Turbidity ADC unavailable; telemetry will show N/A");
        }
        esp_err_t connectivity_err = start_connectivity_timer();
        if (connectivity_err != ESP_OK)
            ESP_LOGW(TAG, "Connectivity reconciliation unavailable: 0x%x",
                     connectivity_err);
        esp_err_t telemetry_err = start_display_telemetry();
        if (telemetry_err != ESP_OK)
            ESP_LOGW(TAG, "Display telemetry unavailable: 0x%x",
                     telemetry_err);

        /* Start diagnostic task (non-blocking, 60s interval) */
        esp_err_t diag_err = start_diagnostic_task();
        if (diag_err != ESP_OK)
            ESP_LOGW(TAG, "Diagnostic task unavailable: 0x%x", diag_err);

        esp_err_t sht_err = s_real_hal
            ? real_hal_start_sht(s_real_hal) : ESP_ERR_NOT_SUPPORTED;
        if (s_real_hal && sht_err != ESP_OK)
            ESP_LOGW(TAG, "SHT30 deferred startup failed (0x%x)", sht_err);

        esp_err_t gesture_hw_err = s_real_hal
            ? real_hal_start_gesture(s_real_hal) : ESP_ERR_NOT_SUPPORTED;
        if (gesture_hw_err == ESP_OK &&
            real_hal_gesture_available(s_real_hal)) {
            gesture_service_config_t gesture_cfg = {
                .hal = s_hal,
                .sink = gesture_display_sink,
                .sink_context = NULL,
                .poll_interval_ms = 50U,
                /* A hand-return can produce an opposite PAJ7620 event a few
                 * hundred milliseconds after a swipe.  Keep a short paging
                 * lock to consume that rebound without making rapid paging
                 * feel unresponsive; button/voice controls stay independent. */
                .cooldown_ms = 700U,
            };
            esp_err_t gesture_err = gesture_service_global_init();
            if (gesture_err == ESP_OK) {
                gesture_err = gesture_service_init(&gesture_cfg);
                if (gesture_err == ESP_OK) st.gesture_init = true;
            }
            if (gesture_err == ESP_OK) {
                gesture_err = gesture_service_start();
                if (gesture_err == ESP_OK) {
                    st.gesture_started = true;
                    s_gesture_active = true;
                    result->gesture_svc_ok = true;
                }
            }
            if (gesture_err != ESP_OK)
                ESP_LOGW(TAG, "Gesture paging unavailable (0x%x)", gesture_err);
        } else if (s_real_hal) {
            ESP_LOGW(TAG, "Gesture hardware unavailable (0x%x)",
                     gesture_hw_err);
        }
    }

    log_heap("bootstrap_complete");
#if XIAOJING_PHASE9_AUTOTEST
    {
        BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
            phase9_autotest_task, "phase9_auto", 4096U, NULL, 2, NULL, 0,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_LOGI(TAG, "PHASE9_AUTOTEST task=%s",
                 created == pdPASS ? "started" : "create_failed");
    }
#endif
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "Bootstrap failed (0x%x), rolling back...", err);
    /* Stop diagnostic task first (if running) */
    stop_diagnostic_task();
#if CONFIG_XIAOJING_DISPLAY_ENABLED
    stop_connectivity_timer();
    if (s_display_active) {
        (void)display_service_set_error(
            "启动失败：请检查 MCP23017 / I2C");
    }
#endif
    /*
     * Four-phase rollback (reverse of init):
     *   Phase 1: button stop (producer)
     *   Phase 2: executor quiesce (reject new, keep consuming)
     *   Phase 3: services stop UV→Detergent→Drain→Dry→Water→BL50→Position
     *   Phase 4: executor stop/join → adapter consumer → safety → queue → HAL
     */
    bool rollback_halted = false;
    esp_err_t first_stop_err = ESP_OK;   /* 记录首个关断错误，不覆盖 */
#if CONFIG_XIAOJING_DISPLAY_ENABLED
    esp_err_t telemetry_stop_err = stop_display_telemetry();
    if (telemetry_stop_err != ESP_OK) {
        ESP_LOGE(TAG, "Rollback: display telemetry stop failed (0x%x)",
                 telemetry_stop_err);
        rollback_halted = true;
    }
#endif

    /* Phase 0: stop remote producer before protocol/executor dependencies. */
    if (st.ble_initialized) {
        esp_err_t s = xiaojing_ble_transport_stop();
        if (s != ESP_OK) {
            ESP_LOGE(TAG, "Rollback: BLE stop failed (0x%x)", s);
            rollback_halted = true;
        } else {
            s_ble_active = false;
        }
    }
    if (!rollback_halted && st.protocol_initialized) {
        esp_err_t s = app_protocol_stop();
        if (s != ESP_OK) {
            ESP_LOGE(TAG, "Rollback: protocol stop failed (0x%x)", s);
            rollback_halted = true;
        } else {
            s_protocol_active = false;
        }
    }
    if (!rollback_halted && st.gesture_init) {
        esp_err_t s = gesture_service_stop();
        if (s != ESP_OK) {
            ESP_LOGE(TAG, "Rollback: gesture stop failed (0x%x)", s);
            rollback_halted = true;
        } else {
            s_gesture_active = false;
        }
    }
    if (!rollback_halted && st.display_init) {
        esp_err_t s = display_service_stop();
        if (s != ESP_OK) {
            ESP_LOGE(TAG, "Rollback: display stop failed (0x%x)", s);
            rollback_halted = true;
        } else {
            s_display_active = false;
        }
    }

    /* Phase 1: Stop producer */
    if (!rollback_halted && st.button_started) {
        esp_err_t s = button_service_stop();
        if (s != ESP_OK) {
            ESP_LOGE(TAG, "Rollback: button stop failed (0x%x)", s);
            rollback_halted = true;
        }
    }
    if (!rollback_halted && st.program_control_started) {
        esp_err_t s = program_control_service_stop();
        if (s != ESP_OK) {
            ESP_LOGE(TAG, "Rollback: program_control stop failed (0x%x)", s);
            rollback_halted = true;
        }
    }
    if (!rollback_halted && st.voice_initialized) {
        /* Stop frontend first (releases audio ownership) */
        voice_frontend_stop(2000);
        esp_err_t s = voice_service_stop();
        if (s != ESP_OK) {
            ESP_LOGE(TAG, "Rollback: voice stop failed (0x%x)", s);
            rollback_halted = true;
        } else {
            s_voice_service_active = false;
        }
    }
    if (!rollback_halted && st.voice_cloud_init) {
        esp_err_t s = voice_cloud_adapter_stop();
        if (s != ESP_OK) {
            ESP_LOGE(TAG, "Rollback: voice cloud stop failed (0x%x)", s);
            rollback_halted = true;
        } else {
            s_voice_cloud_active = false;
        }
    }
    if (!rollback_halted && st.voice_capture_init) {
        esp_err_t s = voice_capture_idf_deinit();
        if (s != ESP_OK) {
            ESP_LOGE(TAG, "Rollback: voice capture deinit failed (0x%x)", s);
            rollback_halted = true;
        } else {
            s_voice_capture_active = false;
        }
    }
    if (!rollback_halted && st.voice_audio_init) {
        destroy_voice_audio_hal();
    }
    if (!rollback_halted && st.wifi_init) {
        esp_err_t s = xiaojing_wifi_stop();
        if (s != ESP_OK) {
            ESP_LOGE(TAG, "Rollback: Wi-Fi stop failed (0x%x)", s);
            rollback_halted = true;
        } else {
            s_wifi_active = false;
        }
    }

    /* Phase 2: Quiesce executor */
    if (st.executor_started) {
        wash_exec_bootstrap_quiesce();
    }

    /* Phase 3: Stop services (executor still consuming terminals).
     * 诊断自检 stop 失败时禁止继续销毁其依赖服务：活任务不得访问已销毁的
     * water/detergent/uv/queue/mutex。记录首个关断错误，不覆盖。 */
    if (!rollback_halted && st.uv_self_test_started) {
        esp_err_t se = uv_self_test_emergency();
        if (se == ESP_OK) se = uv_self_test_stop();
        if (se != ESP_OK) {
            if (first_stop_err == ESP_OK) first_stop_err = se;
            ESP_LOGE(TAG, "Rollback: uv self-test stop failed (0x%x), "
                     "preserving dependencies", se);
            rollback_halted = true;
        }
    }
    if (!rollback_halted && st.actuator_self_test_started) {
        esp_err_t se = actuator_self_test_emergency();
        if (se == ESP_OK) se = actuator_self_test_stop();
        if (se != ESP_OK) {
            if (first_stop_err == ESP_OK) first_stop_err = se;
            ESP_LOGE(TAG, "Rollback: actuator self-test stop failed (0x%x), "
                     "preserving dependencies", se);
            rollback_halted = true;
        }
    }
    if (!rollback_halted && st.motor_bench_started) {
        esp_err_t se = motor_bench_service_emergency();
        if (se == ESP_OK) se = motor_bench_service_stop();
        if (se != ESP_OK) {
            if (first_stop_err == ESP_OK) first_stop_err = se;
            ESP_LOGE(TAG, "Rollback: motor_bench stop failed (0x%x), "
                     "preserving dependencies", se);
            rollback_halted = true;
        }
    }
    if (!rollback_halted && st.uv_init) {
        esp_err_t s = uv_service_stop();
        if (s != ESP_OK) { ESP_LOGE(TAG, "Rollback: uv stop failed (0x%x)", s); rollback_halted = true; }
    }
    if (!rollback_halted && st.detergent_init) {
        esp_err_t s = detergent_service_stop();
        if (s != ESP_OK) { ESP_LOGE(TAG, "Rollback: detergent stop failed (0x%x)", s); rollback_halted = true; }
    }
    if (!rollback_halted && st.drain_init) {
        esp_err_t s = drain_service_stop();
        if (s != ESP_OK) { ESP_LOGE(TAG, "Rollback: drain stop failed (0x%x)", s); rollback_halted = true; }
    }
    if (!rollback_halted && st.coupled_hot_air_init) {
        esp_err_t s = coupled_hot_air_service_stop();
        if (s != ESP_OK) { ESP_LOGE(TAG, "Rollback: coupled hot-air stop failed (0x%x)", s); rollback_halted = true; }
    }
    if (!rollback_halted && st.dry_init) {
        esp_err_t s = dry_service_stop();
        if (s != ESP_OK) { ESP_LOGE(TAG, "Rollback: dry stop failed (0x%x)", s); rollback_halted = true; }
    }
    if (!rollback_halted && st.water_init) {
        esp_err_t s = water_service_stop();
        if (s != ESP_OK) { ESP_LOGE(TAG, "Rollback: water stop failed (0x%x)", s); rollback_halted = true; }
    }
    if (!rollback_halted && st.bl50_init) {
        esp_err_t s = bl50_service_stop();
        if (s != ESP_OK) { ESP_LOGE(TAG, "Rollback: bl50 stop failed (0x%x)", s); rollback_halted = true; }
    }
    if (!rollback_halted && st.position_init) {
        esp_err_t s = position_service_stop();
        if (s != ESP_OK) { ESP_LOGE(TAG, "Rollback: position stop failed (0x%x)", s); rollback_halted = true; }
    }

    /* Phase 4: Executor stop/join → adapter → safety → queue → HAL */
    if (!rollback_halted && st.executor_started) {
        esp_err_t es = wash_exec_bootstrap_stop(5000);
        if (es != ESP_OK) {
            ESP_LOGE(TAG, "Rollback: executor stop failed (0x%x)", es);
            rollback_halted = true;
        }
    }
    if (!rollback_halted) {
        esp_err_t bs = wash_exec_bootstrap_destroy();
        if (bs != ESP_OK) {
            ESP_LOGE(TAG, "Rollback: executor/adapter destroy failed (0x%x)", bs);
            rollback_halted = true;
        }
    }
    if (!rollback_halted && st.safety_init) {
        esp_err_t s = safety_manager_stop();
        if (s != ESP_OK) {
            ESP_LOGE(TAG, "Rollback: safety stop failed (0x%x)", s);
            rollback_halted = true;
        }
    }
    if (!rollback_halted) {
        if (st.event_queue) {
            machine_event_t drain;
            while (xQueueReceive(s_event_queue, &drain, 0) == pdTRUE) {}
            vQueueDelete(s_event_queue); s_event_queue = NULL;
        }
        if (st.hal) destroy_hal();
    }
    if (rollback_halted) {
        ESP_LOGE(TAG, "Rollback halted: resources preserved for later deinit");
    }
    result->hal = NULL;
    result->executor = NULL;
    result->adapter_ctx = NULL;
    return err;
}

/* ================================================================
 * Deinit — four-phase shutdown
 *
 * Phase 1: button stop (producer stops)
 * Phase 2: executor quiesce (reject new programs)
 * Phase 3: services stop (executor consumes terminals)
 * Phase 4: executor stop/join → adapter → safety → queue → HAL
 * ================================================================ */

esp_err_t app_bootstrap_deinit(void) {
    /* Stop task completion worker first to prevent UAF.
     * Order: signal stop → real join → drain queue → delete queue.
     * worker 的 xQueueReceive 超时 1s < 本轮询 1.5s，确保其在删队列前已
     * 离开 receive；但播放（real_hal_audio_play_pcm16）可能更长。若超时
     * 未退出，保留资源供重试，禁止在活任务上强删队列。 */
    if (s_task_complete_task_running) {
        s_task_complete_task_running = false;
        bool worker_exited = false;
        for (int i = 0; i < 15; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
            if (s_task_complete_task == NULL) { worker_exited = true; break; }
        }
        if (!worker_exited) {
            ESP_LOGE(TAG, "deinit: task_done worker still running; "
                     "preserving queue/task for retry");
            return ESP_ERR_TIMEOUT;
        }
    }
    if (s_task_complete_queue) {
        task_complete_event_t drain;
        while (xQueueReceive(s_task_complete_queue, &drain, 0) == pdTRUE) {}
        vQueueDelete(s_task_complete_queue);
        s_task_complete_queue = NULL;
    }

    /* Phase 0: stop BLE producer, then protocol routing. */
    if (s_ble_active) {
        esp_err_t ble_err = xiaojing_ble_transport_stop();
        if (ble_err != ESP_OK) {
            ESP_LOGE(TAG, "deinit: BLE stop failed (0x%x), preserving resources",
                     ble_err);
            return ble_err;
        }
        s_ble_active = false;
    }
    if (s_protocol_active) {
        esp_err_t protocol_err = app_protocol_stop();
        if (protocol_err != ESP_OK) {
            ESP_LOGE(TAG, "deinit: protocol stop failed (0x%x), preserving resources",
                     protocol_err);
            return protocol_err;
        }
        s_protocol_active = false;
    }

    if (s_gesture_active) {
        esp_err_t gesture_err = gesture_service_stop();
        if (gesture_err != ESP_OK) {
            ESP_LOGE(TAG,
                     "deinit: gesture stop failed (0x%x), preserving resources",
                     gesture_err);
            return gesture_err;
        }
        s_gesture_active = false;
    }

    stop_connectivity_timer();
    esp_err_t telemetry_err = stop_display_telemetry();
    if (telemetry_err != ESP_OK) {
        ESP_LOGE(TAG,
                 "deinit: display telemetry stop failed (0x%x), preserving resources",
                 telemetry_err);
        return telemetry_err;
    }
    if (s_display_active) {
        esp_err_t display_err = display_service_stop();
        if (display_err != ESP_OK) {
            ESP_LOGE(TAG,
                     "deinit: display stop failed (0x%x), preserving resources",
                     display_err);
            return display_err;
        }
        s_display_active = false;
    }

    /* Phase 1: Stop button producer */
    esp_err_t btn_err = button_service_stop();
    if (btn_err != ESP_OK) {
        ESP_LOGE(TAG, "deinit: button stop failed (0x%x), preserving resources", btn_err);
        return btn_err;
    }
    (void)program_control_service_stop();

    /* Voice is also an executor producer; stop it before quiescing. */
    esp_err_t voice_err = voice_service_stop();
    if (voice_err != ESP_OK) {
        ESP_LOGE(TAG, "deinit: voice stop failed (0x%x), preserving resources",
                 voice_err);
        return voice_err;
    }
    s_voice_service_active = false;
    if (s_voice_cloud_active) {
        esp_err_t cloud_err = voice_cloud_adapter_stop();
        if (cloud_err != ESP_OK) {
            ESP_LOGE(TAG,
                     "deinit: voice cloud stop failed (0x%x), preserving resources",
                     cloud_err);
            return cloud_err;
        }
        s_voice_cloud_active = false;
    }
    if (s_voice_capture_active) {
        esp_err_t capture_err = voice_capture_idf_deinit();
        if (capture_err != ESP_OK) {
            ESP_LOGE(TAG,
                     "deinit: voice capture deinit failed (0x%x), preserving resources",
                     capture_err);
            return capture_err;
        }
        s_voice_capture_active = false;
    }
    destroy_voice_audio_hal();
    if (s_wifi_active) {
        esp_err_t wifi_err = xiaojing_wifi_stop();
        if (wifi_err != ESP_OK) {
            ESP_LOGE(TAG,
                     "deinit: Wi-Fi stop failed (0x%x), preserving resources",
                     wifi_err);
            return wifi_err;
        }
        s_wifi_active = false;
    }

    /* Phase 2: Quiesce executor (reject new programs, keep consuming) */
    wash_exec_bootstrap_quiesce();

    /* UV self-test: force lamp OFF and stop before services stop, so teardown
     * never tears down HAL/MCP while UV could still be lit.  stop 失败时保留
     * 全部依赖资源并返回：禁止活任务访问已销毁的 service/queue/mutex。 */
    esp_err_t uv_st_stop = uv_self_test_emergency();
    if (uv_st_stop == ESP_OK) uv_st_stop = uv_self_test_stop();
    if (uv_st_stop != ESP_OK) {
        ESP_LOGE(TAG, "deinit: uv self-test stop failed (0x%x), "
                 "preserving resources", uv_st_stop);
        return uv_st_stop;
    }
    esp_err_t act_st_stop = actuator_self_test_emergency();
    if (act_st_stop == ESP_OK) act_st_stop = actuator_self_test_stop();
    if (act_st_stop != ESP_OK) {
        ESP_LOGE(TAG, "deinit: actuator self-test stop failed (0x%x), "
                 "preserving resources", act_st_stop);
        return act_st_stop;
    }
    esp_err_t mb_st_stop = motor_bench_service_emergency();
    if (mb_st_stop == ESP_OK) mb_st_stop = motor_bench_service_stop();
    if (mb_st_stop != ESP_OK) {
        ESP_LOGE(TAG, "deinit: motor_bench stop failed (0x%x), "
                 "preserving resources", mb_st_stop);
        return mb_st_stop;
    }

    /* Phase 3: Stop services (executor still consuming service terminals) */
    esp_err_t uv_err = uv_service_stop();
    if (uv_err != ESP_OK) {
        ESP_LOGE(TAG, "deinit: uv stop failed (0x%x), preserving resources", uv_err);
        return uv_err;
    }

    esp_err_t dt_err = detergent_service_stop();
    if (dt_err != ESP_OK) {
        ESP_LOGE(TAG, "deinit: detergent stop failed (0x%x), preserving resources", dt_err);
        return dt_err;
    }

    esp_err_t dr_err = drain_service_stop();
    if (dr_err != ESP_OK) {
        ESP_LOGE(TAG, "deinit: drain stop failed (0x%x), preserving resources", dr_err);
        return dr_err;
    }

    esp_err_t ds_err = dry_service_stop();
    if (ds_err != ESP_OK) {
        ESP_LOGE(TAG, "deinit: dry stop failed (0x%x), preserving resources", ds_err);
        return ds_err;
    }

    esp_err_t cha_err = coupled_hot_air_service_stop();
    if (cha_err != ESP_OK) {
        ESP_LOGE(TAG, "deinit: coupled hot-air stop failed (0x%x), preserving resources", cha_err);
        return cha_err;
    }

    esp_err_t ws_err = water_service_stop();
    if (ws_err != ESP_OK) {
        ESP_LOGE(TAG, "deinit: water stop failed (0x%x), preserving resources", ws_err);
        return ws_err;
    }

    esp_err_t bs_err = bl50_service_stop();
    if (bs_err != ESP_OK) {
        ESP_LOGE(TAG, "deinit: bl50 stop failed (0x%x), preserving resources", bs_err);
        return bs_err;
    }

    esp_err_t ps_err = position_service_stop();
    if (ps_err != ESP_OK) {
        ESP_LOGE(TAG, "deinit: position stop failed (0x%x), preserving resources", ps_err);
        return ps_err;
    }

    /* Phase 4: Executor stop/join → adapter → safety → queue → HAL */
    esp_err_t es = wash_exec_bootstrap_stop(5000);
    if (es != ESP_OK) {
        ESP_LOGE(TAG, "deinit: executor stop failed (0x%x), preserving resources", es);
        return es;
    }
    esp_err_t destroy_err = wash_exec_bootstrap_destroy();
    if (destroy_err != ESP_OK) {
        ESP_LOGE(TAG, "deinit: executor/adapter destroy failed (0x%x), preserving resources",
                 destroy_err);
        return destroy_err;
    }

    esp_err_t safety_err = safety_manager_stop();
    if (safety_err != ESP_OK) {
        ESP_LOGE(TAG, "deinit: safety stop failed (0x%x), preserving queue/HAL",
                 safety_err);
        return safety_err;
    }

    if (s_event_queue) {
        machine_event_t drain;
        while (xQueueReceive(s_event_queue, &drain, 0) == pdTRUE) {}
        vQueueDelete(s_event_queue); s_event_queue = NULL;
    }
    destroy_hal();

    return ESP_OK;
}
