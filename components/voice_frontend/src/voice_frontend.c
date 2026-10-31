#include "voice_frontend.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "voice_frontend";

/* ---- I2S Diagnostic Counters ---- */

typedef struct {
    uint32_t i2s_read_calls;
    uint32_t i2s_read_ok;
    uint32_t i2s_bytes;
    uint32_t i2s_zero_reads;
    uint32_t i2s_errors;
    uint32_t frontend_feed_calls;
    uint32_t frontend_feed_samples;
    esp_err_t last_i2s_error;
} i2s_diag_t;

/* ---- Internal State ---- */

typedef struct {
    voice_frontend_config_t config;
    voice_wake_backend_ops_t *backends[VOICE_WAKE_BACKEND_COUNT];
    voice_wake_backend_type_t current_backend;
    voice_wake_state_t state;
    voice_audio_owner_t audio_owner;
    bool wake_disabled;
    bool initialized;

    /* Synchronization */
    SemaphoreHandle_t mutex;
    EventGroupHandle_t events;

    /* Task */
    TaskHandle_t task_handle;
    bool task_running;

    /* Audio buffer for I2S reads */
    int16_t *pcm_buffer;
    size_t pcm_buffer_samples;

    /* I2S diagnostics */
    i2s_diag_t i2s_diag;

    /* Statistics */
    uint32_t frame_count;
    uint32_t detection_count;          /* Aggregate callbacks across backends */
    uint32_t backend_detection_cursor; /* Current backend-local counter */
    uint32_t sequence;
    int64_t last_detection_us;
    int64_t last_start_us;
    int64_t last_stop_us;
    uint32_t stop_duration_ms;
    uint32_t create_duration_ms;

    /* Wake callback counter */
    uint32_t wake_callback_count;

    /* Detection buffer */
    voice_wake_detection_t last_detection;
} voice_frontend_t;

static voice_frontend_t s_frontend = {0};

/* Event bits */
#define EVENT_STOP_REQUESTED    (1 << 0)
#define EVENT_STOP_COMPLETE     (1 << 1)
#define EVENT_TASK_EXITED       (1 << 2)

/* ---- Helpers ---- */

static void generate_sequence(voice_frontend_t *fe)
{
    fe->sequence++;
    if (fe->sequence == 0) fe->sequence = 1;
}

static bool acquire_audio(voice_frontend_t *fe, voice_audio_owner_t owner)
{
    if (fe->audio_owner != VOICE_AUDIO_OWNER_NONE &&
        fe->audio_owner != owner) {
        ESP_LOGW(TAG, "audio owned by %d, cannot acquire for %d",
                 (int)fe->audio_owner, (int)owner);
        return false;
    }
    fe->audio_owner = owner;
    return true;
}

static void release_audio(voice_frontend_t *fe, voice_audio_owner_t owner)
{
    if (fe->audio_owner == owner) {
        fe->audio_owner = VOICE_AUDIO_OWNER_NONE;
    }
}

/* ---- Audio Feed ---- */

esp_err_t voice_frontend_feed(const int16_t *samples, size_t count)
{
    if (!s_frontend.initialized) return ESP_ERR_INVALID_STATE;
    if (!samples || count == 0) return ESP_ERR_INVALID_ARG;

    /* The audio controller owns the capture task, while this mutex owns the
     * backend lifetime.  Holding it across feed/snapshot prevents a BTN1
     * switch from destroying a model that is still classifying a frame. */
    if (xSemaphoreTake(s_frontend.mutex, portMAX_DELAY) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    if (s_frontend.current_backend == VOICE_WAKE_BACKEND_NONE ||
        (s_frontend.state != VOICE_WAKE_STATE_LISTENING &&
         s_frontend.state != VOICE_WAKE_STATE_STOPPING) ||
        s_frontend.wake_disabled) {
        xSemaphoreGive(s_frontend.mutex);
        return ESP_OK;
    }

    voice_wake_backend_ops_t *ops = s_frontend.backends[s_frontend.current_backend];
    if (!ops || !ops->feed) {
        xSemaphoreGive(s_frontend.mutex);
        return ESP_ERR_INVALID_STATE;
    }

    /* Feed to backend */
    s_frontend.i2s_diag.frontend_feed_calls++;
    s_frontend.i2s_diag.frontend_feed_samples += count;
    s_frontend.frame_count++;

    esp_err_t err = ops->feed(samples, count);
    if (err != ESP_OK) {
        xSemaphoreGive(s_frontend.mutex);
        return err;
    }

    /* Check for wake detection */
    bool notify = false;
    voice_wake_detection_t det = {0};
    voice_wake_detection_cb_t callback = NULL;
    void *callback_context = NULL;
    voice_wake_snapshot_t snap;
    if (ops->get_snapshot && ops->get_snapshot(&snap) == ESP_OK) {
        if (snap.detection_count > s_frontend.backend_detection_cursor) {
            s_frontend.backend_detection_cursor = snap.detection_count;
            s_frontend.detection_count++;
            s_frontend.last_detection_us = esp_timer_get_time();
            generate_sequence(&s_frontend);

            if (s_frontend.config.detection_callback) {
                s_frontend.wake_callback_count++;
                det = (voice_wake_detection_t) {
                    .detected = true,
                    .score = snap.backend == VOICE_WAKE_BACKEND_ESP_SR
                        ? 1.0f : 0.0f,
                    .sequence = s_frontend.sequence,
                    .timestamp_us = s_frontend.last_detection_us,
                };
                callback = s_frontend.config.detection_callback;
                callback_context = s_frontend.config.detection_context;
                notify = true;
                ESP_LOGI(TAG, "WAKE_CALLBACK count=%"PRIu32,
                         s_frontend.wake_callback_count);
            }
        }
    }

    xSemaphoreGive(s_frontend.mutex);
    if (notify) callback(&det, callback_context);
    return ESP_OK;
}

/* ---- API Implementation ---- */

esp_err_t voice_frontend_init(const voice_frontend_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    if (s_frontend.initialized) return ESP_ERR_INVALID_STATE;

    memset(&s_frontend, 0, sizeof(s_frontend));
    memcpy(&s_frontend.config, config, sizeof(*config));

    s_frontend.mutex = xSemaphoreCreateMutex();
    if (!s_frontend.mutex) return ESP_ERR_NO_MEM;

    s_frontend.events = xEventGroupCreate();
    if (!s_frontend.events) {
        vSemaphoreDelete(s_frontend.mutex);
        return ESP_ERR_NO_MEM;
    }

    s_frontend.state = VOICE_WAKE_STATE_INITIALIZED;
    s_frontend.current_backend = VOICE_WAKE_BACKEND_NONE;
    s_frontend.audio_owner = VOICE_AUDIO_OWNER_NONE;
    s_frontend.initialized = true;

    ESP_LOGI(TAG, "initialized (no I2S owner - PCM fed externally)");
    return ESP_OK;
}

esp_err_t voice_frontend_register_backend(voice_wake_backend_type_t type,
                                          const voice_wake_backend_ops_t *ops)
{
    if (type == VOICE_WAKE_BACKEND_NONE || type >= VOICE_WAKE_BACKEND_COUNT)
        return ESP_ERR_INVALID_ARG;
    if (!ops) return ESP_ERR_INVALID_ARG;
    if (!s_frontend.initialized || !s_frontend.mutex)
        return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_frontend.mutex, portMAX_DELAY) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    if (s_frontend.current_backend != VOICE_WAKE_BACKEND_NONE ||
        (s_frontend.state != VOICE_WAKE_STATE_INITIALIZED &&
         s_frontend.state != VOICE_WAKE_STATE_STOPPED)) {
        xSemaphoreGive(s_frontend.mutex);
        return ESP_ERR_INVALID_STATE;
    }

    s_frontend.backends[type] = (voice_wake_backend_ops_t *)ops;
    xSemaphoreGive(s_frontend.mutex);
    ESP_LOGI(TAG, "registered backend: %s", ops->name ? ops->name : "unknown");
    return ESP_OK;
}

static esp_err_t start_backend_locked(voice_wake_backend_type_t backend,
                                      uint32_t stop_timeout_ms)
{
    /* Check if already running this backend */
    if (s_frontend.current_backend == backend &&
        s_frontend.state == VOICE_WAKE_STATE_LISTENING) {
        return ESP_OK;
    }

    /* Any previously selected backend must be fully stopped/deinitialized
     * before init.  This also covers a rollback to the same backend after a
     * failed switch: the frontend may be in FAULT with its backend still
     * initialized, and calling init() directly would return INVALID_STATE. */
    if (s_frontend.current_backend != VOICE_WAKE_BACKEND_NONE &&
        (s_frontend.current_backend != backend ||
         s_frontend.state != VOICE_WAKE_STATE_LISTENING)) {
        /* Deinit old backend */
        voice_wake_backend_ops_t *old_ops = s_frontend.backends[s_frontend.current_backend];
        esp_err_t err = old_ops && old_ops->stop
            ? old_ops->stop(stop_timeout_ms) : ESP_OK;
        if (err != ESP_OK) return err;
        err = old_ops && old_ops->deinit ? old_ops->deinit() : ESP_OK;
        if (err != ESP_OK) return err;
        release_audio(&s_frontend, voice_frontend_get_audio_owner());
        s_frontend.current_backend = VOICE_WAKE_BACKEND_NONE;
    }

    /* Init and start new backend */
    voice_wake_backend_ops_t *ops = s_frontend.backends[backend];
    if (!ops) return ESP_ERR_NOT_FOUND;

    int64_t t0 = esp_timer_get_time();
    s_frontend.state = VOICE_WAKE_STATE_STARTING;

    /* Acquire audio ownership */
    voice_audio_owner_t owner = (backend == VOICE_WAKE_BACKEND_EDGE_IMPULSE)
                                    ? VOICE_AUDIO_OWNER_WAKE_EDGE
                                    : VOICE_AUDIO_OWNER_WAKE_ESP_SR;
    if (!acquire_audio(&s_frontend, owner)) {
        s_frontend.state = VOICE_WAKE_STATE_FAULT;
        return ESP_ERR_INVALID_STATE;
    }

    /* Init backend */
    voice_wake_config_t config = {
        .sample_rate = s_frontend.config.i2s_sample_rate,
        .frame_samples = s_frontend.config.frame_samples,
        /* Live acoustic evidence: true "xiao jing xiao jing" reached 0.781
         * while the 10-minute negative maximum was 0.746.  Keep the 0.15
         * class margin and accept at 0.78 to avoid this boundary false reject. */
        .threshold = 0.78f,
        .cooldown_ms = 2000,
    };
    esp_err_t err = ops->init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "backend init failed: %s err=0x%x",
                 ops->name ? ops->name : "unknown", (unsigned)err);
        release_audio(&s_frontend, owner);
        s_frontend.state = VOICE_WAKE_STATE_FAULT;
        return err;
    }

    /* Start backend */
    err = ops->start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "backend start failed: %s err=0x%x",
                 ops->name ? ops->name : "unknown", (unsigned)err);
        ops->deinit();
        release_audio(&s_frontend, owner);
        s_frontend.state = VOICE_WAKE_STATE_FAULT;
        return err;
    }

    s_frontend.current_backend = backend;
    /* Backend counters restart from zero on every lazy init.  Keep the
     * aggregate count for diagnostics, but reset the comparison cursor so
     * the first detection after a BTN1 switch is never suppressed. */
    s_frontend.backend_detection_cursor = 0;
    s_frontend.state = VOICE_WAKE_STATE_LISTENING;
    s_frontend.last_start_us = esp_timer_get_time();
    s_frontend.create_duration_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);

    ESP_LOGI(TAG, "started backend: %s (took %u ms)",
             ops->name ? ops->name : "unknown",
             (unsigned)s_frontend.create_duration_ms);

    return ESP_OK;
}

esp_err_t voice_frontend_start_backend(voice_wake_backend_type_t backend)
{
    if (!s_frontend.initialized) return ESP_ERR_INVALID_STATE;
    if (backend == VOICE_WAKE_BACKEND_NONE || backend >= VOICE_WAKE_BACKEND_COUNT)
        return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_frontend.mutex, portMAX_DELAY) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    esp_err_t err = start_backend_locked(backend, 1000U);
    xSemaphoreGive(s_frontend.mutex);
    return err;
}

esp_err_t voice_frontend_stop(uint32_t timeout_ms)
{
    if (!s_frontend.initialized) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_frontend.mutex, portMAX_DELAY) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    if (s_frontend.state == VOICE_WAKE_STATE_STOPPED ||
        s_frontend.state == VOICE_WAKE_STATE_UNINITIALIZED) {
        xSemaphoreGive(s_frontend.mutex);
        return ESP_OK;
    }

    int64_t t0 = esp_timer_get_time();
    s_frontend.state = VOICE_WAKE_STATE_STOPPING;

    /* Do not hold the frontend mutex while waiting for a backend task.  The
     * ESP-SR fetch task and the audio capture task both need this mutex to
     * complete the final feed/stop handshake. */
    voice_wake_backend_type_t backend = s_frontend.current_backend;
    voice_audio_owner_t owner = s_frontend.audio_owner;
    voice_wake_backend_ops_t *ops =
        (backend != VOICE_WAKE_BACKEND_NONE &&
         backend < VOICE_WAKE_BACKEND_COUNT)
            ? s_frontend.backends[backend] : NULL;
    xSemaphoreGive(s_frontend.mutex);

    esp_err_t err = ESP_OK;
    if (ops) {
        err = ops->stop ? ops->stop(timeout_ms) : ESP_OK;
        if (err == ESP_OK && ops->deinit) err = ops->deinit();
    }

    if (xSemaphoreTake(s_frontend.mutex, portMAX_DELAY) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    if (err != ESP_OK) {
        s_frontend.state = VOICE_WAKE_STATE_FAULT;
        xSemaphoreGive(s_frontend.mutex);
        return err;
    }

    release_audio(&s_frontend, owner);
    s_frontend.current_backend = VOICE_WAKE_BACKEND_NONE;
    s_frontend.state = VOICE_WAKE_STATE_STOPPED;
    s_frontend.last_stop_us = esp_timer_get_time();
    s_frontend.stop_duration_ms =
        (uint32_t)((s_frontend.last_stop_us - t0) / 1000);

    xSemaphoreGive(s_frontend.mutex);

    ESP_LOGI(TAG, "stopped (took %u ms)", (unsigned)s_frontend.stop_duration_ms);
    return ESP_OK;
}

esp_err_t voice_frontend_deinit(void)
{
    if (!s_frontend.initialized) return ESP_ERR_INVALID_STATE;

    /* Stop first */
    esp_err_t err = voice_frontend_stop(2000);
    if (err != ESP_OK) return err;

    xSemaphoreTake(s_frontend.mutex, portMAX_DELAY);

    if (s_frontend.pcm_buffer) {
        heap_caps_free(s_frontend.pcm_buffer);
        s_frontend.pcm_buffer = NULL;
    }
    if (s_frontend.events) {
        vEventGroupDelete(s_frontend.events);
        s_frontend.events = NULL;
    }
    s_frontend.state = VOICE_WAKE_STATE_UNINITIALIZED;
    s_frontend.initialized = false;
    SemaphoreHandle_t mutex = s_frontend.mutex;
    s_frontend.mutex = NULL;
    xSemaphoreGive(mutex);
    vSemaphoreDelete(mutex);

    ESP_LOGI(TAG, "deinitialized");
    return ESP_OK;
}

esp_err_t voice_frontend_get_snapshot(voice_wake_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_frontend.initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_frontend.mutex, portMAX_DELAY);

    out->backend = s_frontend.current_backend;
    out->state = s_frontend.state;
    out->audio_owner = s_frontend.audio_owner;
    out->frame_count = s_frontend.frame_count;
    out->detection_count = s_frontend.detection_count;
    out->sequence = s_frontend.sequence;
    out->last_detection_us = s_frontend.last_detection_us;
    out->last_start_us = s_frontend.last_start_us;
    out->last_stop_us = s_frontend.last_stop_us;
    out->stop_duration_ms = s_frontend.stop_duration_ms;
    out->create_duration_ms = s_frontend.create_duration_ms;
    out->timestamp_us = esp_timer_get_time();

    xSemaphoreGive(s_frontend.mutex);
    return ESP_OK;
}

voice_audio_owner_t voice_frontend_get_audio_owner(void)
{
    return s_frontend.audio_owner;
}

bool voice_frontend_can_switch_backend(void)
{
    if (!s_frontend.initialized) return false;
    if (xSemaphoreTake(s_frontend.mutex, portMAX_DELAY) != pdTRUE) return false;
    bool can_switch = s_frontend.state == VOICE_WAKE_STATE_LISTENING &&
                      s_frontend.current_backend != VOICE_WAKE_BACKEND_NONE;
    xSemaphoreGive(s_frontend.mutex);
    return can_switch;
}

esp_err_t voice_frontend_switch_backend(voice_wake_backend_type_t new_backend,
                                        uint32_t timeout_ms)
{
    if (!s_frontend.initialized) return ESP_ERR_INVALID_STATE;
    if (new_backend == VOICE_WAKE_BACKEND_NONE ||
        new_backend >= VOICE_WAKE_BACKEND_COUNT)
        return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_frontend.mutex, portMAX_DELAY) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    if (s_frontend.state != VOICE_WAKE_STATE_LISTENING ||
        s_frontend.current_backend == VOICE_WAKE_BACKEND_NONE) {
        xSemaphoreGive(s_frontend.mutex);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = start_backend_locked(new_backend, timeout_ms);
    xSemaphoreGive(s_frontend.mutex);
    return err;
}

esp_err_t voice_frontend_disable_wake(void)
{
    if (!s_frontend.initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_frontend.mutex, portMAX_DELAY);
    s_frontend.wake_disabled = true;
    xSemaphoreGive(s_frontend.mutex);

    ESP_LOGI(TAG, "wake disabled");
    return ESP_OK;
}

esp_err_t voice_frontend_enable_wake(void)
{
    if (!s_frontend.initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_frontend.mutex, portMAX_DELAY);
    s_frontend.wake_disabled = false;
    xSemaphoreGive(s_frontend.mutex);

    ESP_LOGI(TAG, "wake enabled");
    return ESP_OK;
}

voice_wake_backend_type_t voice_frontend_get_current_backend(void)
{
    return s_frontend.current_backend;
}

uint32_t voice_frontend_get_wake_callback_count(void)
{
    return s_frontend.wake_callback_count;
}

void voice_frontend_get_i2s_diag(
    uint32_t *i2s_read_calls,
    uint32_t *i2s_read_ok,
    uint32_t *i2s_bytes,
    uint32_t *i2s_zero_reads,
    uint32_t *i2s_errors,
    uint32_t *frontend_feed_calls,
    uint32_t *frontend_feed_samples)
{
    if (i2s_read_calls) *i2s_read_calls = s_frontend.i2s_diag.i2s_read_calls;
    if (i2s_read_ok) *i2s_read_ok = s_frontend.i2s_diag.i2s_read_ok;
    if (i2s_bytes) *i2s_bytes = s_frontend.i2s_diag.i2s_bytes;
    if (i2s_zero_reads) *i2s_zero_reads = s_frontend.i2s_diag.i2s_zero_reads;
    if (i2s_errors) *i2s_errors = s_frontend.i2s_diag.i2s_errors;
    if (frontend_feed_calls) *frontend_feed_calls = s_frontend.i2s_diag.frontend_feed_calls;
    if (frontend_feed_samples) *frontend_feed_samples = s_frontend.i2s_diag.frontend_feed_samples;
}

void voice_frontend_log_diagnostics(void)
{
    ESP_LOGI(TAG, "DIAG: I2S: read_calls=%"PRIu32" read_ok=%"PRIu32
             " bytes=%"PRIu32" zero=%"PRIu32" errors=%"PRIu32,
             s_frontend.i2s_diag.i2s_read_calls,
             s_frontend.i2s_diag.i2s_read_ok,
             s_frontend.i2s_diag.i2s_bytes,
             s_frontend.i2s_diag.i2s_zero_reads,
             s_frontend.i2s_diag.i2s_errors);
    ESP_LOGI(TAG, "DIAG: FEED: calls=%"PRIu32" samples=%"PRIu32
             " wake_cb=%"PRIu32" detections=%"PRIu32,
             s_frontend.i2s_diag.frontend_feed_calls,
             s_frontend.i2s_diag.frontend_feed_samples,
             s_frontend.wake_callback_count,
             s_frontend.detection_count);
    ESP_LOGI(TAG, "DIAG: STATE: state=%d backend=%d owner=%d",
             (int)s_frontend.state, (int)s_frontend.current_backend,
             (int)s_frontend.audio_owner);
}
