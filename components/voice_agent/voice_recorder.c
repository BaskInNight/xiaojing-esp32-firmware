/*
 * voice_recorder.c — Voice recorder with bounded buffer and VAD
 *
 * Accepts PCM from audio_controller via feed function.
 * Implements VAD, silence detection, and timeout-based termination.
 */

#include "voice_recorder.h"

#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "voice_recorder";

/* ---- VAD Configuration ---- */

#define VAD_ENERGY_THRESHOLD     500    /* RMS threshold for speech detection */
#define VAD_SPEECH_MIN_FRAMES    3      /* Minimum frames with speech to confirm start */
#define VAD_SILENCE_MIN_FRAMES   50     /* ~1 second at 20ms frames */
#define VAD_EMPTY_MIN_FRAMES     125    /* ~2.5 seconds at 20ms frames */
#define FRAME_DURATION_MS        20     /* Frame duration for VAD */

/* ---- State ---- */

typedef struct {
    voice_recorder_config_t config;
    bool initialized;
    bool active;

    /* Session */
    uint32_t session_id;
    voice_recorder_source_t source;

    /* Buffer (PSRAM) */
    uint8_t *buffer;
    size_t buffer_size;
    size_t pcm_offset;          /* Current write offset */
    size_t pcm_bytes;           /* Total PCM bytes recorded */

    /* VAD state */
    bool speech_started;
    bool vad_end;
    uint32_t speech_frame_count;
    uint32_t silence_frame_count;
    uint32_t empty_frame_count;

    /* Timing */
    int64_t start_time_us;
    uint32_t duration_ms;

    /* Audio stats */
    uint16_t peak_amplitude;
    uint64_t sum_squares;
    uint32_t sample_count;

    /* Error tracking */
    uint32_t overruns;
    voice_recorder_end_reason_t last_end_reason;

    /* Synchronization */
    SemaphoreHandle_t mutex;
} voice_recorder_t;

static voice_recorder_t s_recorder = {0};

/* ---- Helper Functions ---- */

static uint32_t calculate_rms(void)
{
    if (s_recorder.sample_count == 0) return 0;
    double mean_square = (double)s_recorder.sum_squares / s_recorder.sample_count;
    return (uint32_t)sqrt(mean_square);
}

static void update_audio_stats(const int16_t *samples, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        int16_t sample = samples[i];
        uint16_t abs_sample = (uint16_t)(sample < 0 ? -sample : sample);
        if (abs_sample > s_recorder.peak_amplitude) {
            s_recorder.peak_amplitude = abs_sample;
        }
        s_recorder.sum_squares += (uint64_t)sample * sample;
        s_recorder.sample_count++;
    }
}

/* ---- VAD Processing ---- */

static void process_vad_frame(const int16_t *samples, size_t count)
{
    /* Calculate frame RMS */
    uint64_t sum_sq = 0;
    for (size_t i = 0; i < count; i++) {
        sum_sq += (uint64_t)samples[i] * samples[i];
    }
    uint32_t rms = (uint32_t)sqrt((double)sum_sq / count);

    /* Check for speech */
    bool is_speech = (rms > VAD_ENERGY_THRESHOLD);

    if (!s_recorder.speech_started) {
        if (is_speech) {
            s_recorder.speech_frame_count++;
            if (s_recorder.speech_frame_count >= VAD_SPEECH_MIN_FRAMES) {
                s_recorder.speech_started = true;
                ESP_LOGI(TAG, "VAD: speech started (rms=%u)", rms);
            }
        } else {
            s_recorder.speech_frame_count = 0;
            s_recorder.empty_frame_count++;
        }
    } else {
        /* Speech has started - check for end */
        if (!is_speech) {
            s_recorder.silence_frame_count++;
            s_recorder.empty_frame_count = 0;
        } else {
            s_recorder.silence_frame_count = 0;
        }
    }
}

/* ---- API Implementation ---- */

esp_err_t voice_recorder_init(const voice_recorder_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    if (s_recorder.initialized) return ESP_ERR_INVALID_STATE;

    memset(&s_recorder, 0, sizeof(s_recorder));
    memcpy(&s_recorder.config, config, sizeof(*config));

    /* Set defaults */
    if (s_recorder.config.max_duration_ms == 0)
        s_recorder.config.max_duration_ms = 10000;
    if (s_recorder.config.min_speech_ms == 0)
        s_recorder.config.min_speech_ms = 300;
    if (s_recorder.config.vad_timeout_ms == 0)
        s_recorder.config.vad_timeout_ms = 1100;
    if (s_recorder.config.empty_timeout_ms == 0)
        s_recorder.config.empty_timeout_ms = 2500;
    if (s_recorder.config.sample_rate == 0)
        s_recorder.config.sample_rate = 16000;

    /* Allocate buffer in PSRAM */
    size_t buf_size = s_recorder.config.max_pcm_bytes;
    s_recorder.buffer = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!s_recorder.buffer) {
        ESP_LOGE(TAG, "Failed to allocate buffer (%u bytes) in PSRAM", (unsigned)buf_size);
        return ESP_ERR_NO_MEM;
    }
    /* Verify buffer is actually in external RAM */
    if (!esp_ptr_external_ram(s_recorder.buffer)) {
        ESP_LOGE(TAG, "Buffer allocated but not in external RAM! ptr=%p", s_recorder.buffer);
        heap_caps_free(s_recorder.buffer);
        s_recorder.buffer = NULL;
        return ESP_ERR_INVALID_STATE;
    }
    s_recorder.buffer_size = buf_size;
    ESP_LOGI(TAG, "Buffer allocated: %u bytes at %p (PSRAM)", (unsigned)buf_size, s_recorder.buffer);

    s_recorder.mutex = xSemaphoreCreateMutex();
    if (!s_recorder.mutex) {
        heap_caps_free(s_recorder.buffer);
        return ESP_ERR_NO_MEM;
    }

    s_recorder.initialized = true;

    ESP_LOGI(TAG, "Initialized (buf=%u bytes, max_dur=%u ms, vad_timeout=%u ms, empty_timeout=%u ms)",
             (unsigned)buf_size,
             (unsigned)s_recorder.config.max_duration_ms,
             (unsigned)s_recorder.config.vad_timeout_ms,
             (unsigned)s_recorder.config.empty_timeout_ms);

    return ESP_OK;
}

esp_err_t voice_recorder_deinit(void)
{
    if (!s_recorder.initialized) return ESP_ERR_INVALID_STATE;
    if (s_recorder.active) return ESP_ERR_INVALID_STATE;

    if (s_recorder.mutex) {
        vSemaphoreDelete(s_recorder.mutex);
        s_recorder.mutex = NULL;
    }
    if (s_recorder.buffer) {
        heap_caps_free(s_recorder.buffer);
        s_recorder.buffer = NULL;
    }

    s_recorder.initialized = false;
    return ESP_OK;
}

esp_err_t voice_recorder_begin(uint32_t session_id,
                                voice_recorder_source_t source)
{
    if (!s_recorder.initialized) return ESP_ERR_INVALID_STATE;
    if (s_recorder.active) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_recorder.mutex, pdMS_TO_TICKS(100)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    /* Reset session state */
    s_recorder.session_id = session_id;
    s_recorder.source = source;
    s_recorder.pcm_offset = 0;
    s_recorder.pcm_bytes = 0;
    s_recorder.speech_started = false;
    s_recorder.vad_end = false;
    s_recorder.speech_frame_count = 0;
    s_recorder.silence_frame_count = 0;
    s_recorder.empty_frame_count = 0;
    s_recorder.start_time_us = esp_timer_get_time();
    s_recorder.duration_ms = 0;
    s_recorder.peak_amplitude = 0;
    s_recorder.sum_squares = 0;
    s_recorder.sample_count = 0;
    s_recorder.overruns = 0;
    s_recorder.last_end_reason = VOICE_RECORDER_END_NONE;
    s_recorder.active = true;

    xSemaphoreGive(s_recorder.mutex);

    ESP_LOGI(TAG, "Begin session=%"PRIu32" source=%d",
             session_id, (int)source);

    return ESP_OK;
}

esp_err_t voice_recorder_feed(const int16_t *samples, size_t count)
{
    if (!s_recorder.initialized || !s_recorder.active)
        return ESP_ERR_INVALID_STATE;
    if (!samples || count == 0) return ESP_ERR_INVALID_ARG;

    size_t bytes = count * sizeof(int16_t);

    /* Non-blocking check for buffer space */
    if (s_recorder.pcm_offset + bytes > s_recorder.buffer_size) {
        s_recorder.overruns++;
        ESP_LOGW(TAG, "Buffer overrun (offset=%u, need=%u, capacity=%u)",
                 (unsigned)s_recorder.pcm_offset, (unsigned)bytes,
                 (unsigned)s_recorder.buffer_size);
        return ESP_ERR_NO_MEM;
    }

    /* Copy PCM to buffer */
    memcpy(s_recorder.buffer + s_recorder.pcm_offset, samples, bytes);
    s_recorder.pcm_offset += bytes;
    s_recorder.pcm_bytes += bytes;

    /* Update audio stats */
    update_audio_stats(samples, count);

    /* Process VAD (frame-based) */
    size_t frame_samples = s_recorder.config.sample_rate * FRAME_DURATION_MS / 1000;
    if (count >= frame_samples) {
        process_vad_frame(samples, frame_samples);
    }

    /* Update duration */
    int64_t now = esp_timer_get_time();
    s_recorder.duration_ms = (uint32_t)((now - s_recorder.start_time_us) / 1000);

    return ESP_OK;
}

static const char *end_reason_str(voice_recorder_end_reason_t reason)
{
    switch (reason) {
    case VOICE_RECORDER_END_NONE: return "NONE";
    case VOICE_RECORDER_END_SPEECH_COMPLETE: return "VAD_SILENCE";
    case VOICE_RECORDER_END_SILENCE_TIMEOUT: return "EMPTY_TIMEOUT";
    case VOICE_RECORDER_END_MAX_DURATION: return "MAX_DURATION";
    case VOICE_RECORDER_END_BUFFER_FULL: return "BUFFER_FULL";
    case VOICE_RECORDER_END_USER_CANCEL: return "USER_CANCEL";
    case VOICE_RECORDER_END_I2S_ERROR: return "I2S_ERROR";
    case VOICE_RECORDER_END_OVERRUN: return "OVERRUN";
    default: return "UNKNOWN";
    }
}

esp_err_t voice_recorder_end(voice_recorder_end_reason_t reason)
{
    if (!s_recorder.initialized || !s_recorder.active)
        return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_recorder.mutex, pdMS_TO_TICKS(100)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    s_recorder.active = false;
    s_recorder.last_end_reason = reason;
    s_recorder.vad_end = true;

    ESP_LOGI(TAG, "End session=%"PRIu32" reason=%s pcm=%u dur=%u ms peak=%u rms=%u",
             s_recorder.session_id, end_reason_str(reason),
             (unsigned)s_recorder.pcm_bytes,
             (unsigned)s_recorder.duration_ms,
             (unsigned)s_recorder.peak_amplitude,
             (unsigned)calculate_rms());

    xSemaphoreGive(s_recorder.mutex);
    return ESP_OK;
}

esp_err_t voice_recorder_abort(voice_recorder_end_reason_t reason)
{
    if (!s_recorder.initialized) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_recorder.mutex, pdMS_TO_TICKS(100)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    s_recorder.active = false;
    s_recorder.last_end_reason = reason;
    s_recorder.pcm_bytes = 0;
    s_recorder.pcm_offset = 0;

    ESP_LOGI(TAG, "Abort session=%"PRIu32" reason=%s",
             s_recorder.session_id, end_reason_str(reason));

    xSemaphoreGive(s_recorder.mutex);
    return ESP_OK;
}

voice_recorder_end_reason_t voice_recorder_check_termination(void)
{
    if (!s_recorder.active) return VOICE_RECORDER_END_NONE;

    /* Check max duration */
    if (s_recorder.duration_ms >= s_recorder.config.max_duration_ms) {
        return VOICE_RECORDER_END_MAX_DURATION;
    }

    /* Check buffer full */
    if (s_recorder.pcm_offset >= s_recorder.buffer_size) {
        return VOICE_RECORDER_END_BUFFER_FULL;
    }

    /* Check overrun */
    if (s_recorder.overruns > 0) {
        return VOICE_RECORDER_END_OVERRUN;
    }

    /* Check VAD-based termination */
    if (s_recorder.speech_started) {
        /* Speech has started - check for silence timeout */
        uint32_t silence_ms = s_recorder.silence_frame_count * FRAME_DURATION_MS;
        if (silence_ms >= s_recorder.config.vad_timeout_ms) {
            return VOICE_RECORDER_END_SPEECH_COMPLETE;
        }
    } else {
        /* No speech yet - check empty timeout */
        uint32_t empty_ms = s_recorder.empty_frame_count * FRAME_DURATION_MS;
        if (empty_ms >= s_recorder.config.empty_timeout_ms) {
            return VOICE_RECORDER_END_SILENCE_TIMEOUT;
        }
    }

    return VOICE_RECORDER_END_NONE;
}

esp_err_t voice_recorder_get_snapshot(voice_recorder_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    if (xSemaphoreTake(s_recorder.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        out->active = s_recorder.active;
        out->session_id = s_recorder.session_id;
        out->source = s_recorder.source;
        out->pcm_bytes = s_recorder.pcm_bytes;
        out->duration_ms = s_recorder.duration_ms;
        out->speech_started = s_recorder.speech_started;
        out->vad_end = s_recorder.vad_end;
        out->peak_amplitude = s_recorder.peak_amplitude;
        out->rms_amplitude = calculate_rms();
        out->silence_ms = s_recorder.silence_frame_count * FRAME_DURATION_MS;
        out->overruns = s_recorder.overruns;
        out->last_end_reason = s_recorder.last_end_reason;
        xSemaphoreGive(s_recorder.mutex);
    }

    return ESP_OK;
}

esp_err_t voice_recorder_get_data(const uint8_t **pcm, size_t *pcm_bytes)
{
    if (!pcm || !pcm_bytes) return ESP_ERR_INVALID_ARG;
    if (s_recorder.active) return ESP_ERR_INVALID_STATE;

    *pcm = s_recorder.buffer;
    *pcm_bytes = s_recorder.pcm_bytes;
    return ESP_OK;
}

bool voice_recorder_is_active(void)
{
    return s_recorder.active;
}
