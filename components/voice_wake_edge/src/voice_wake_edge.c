#include "voice_wake_edge.h"
#include "ei_classifier_wrapper.h"
#include "model_metadata.h"

#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "wake_edge";

/* ---- Configuration ---- */

#define EI_FREQUENCY        16062
#define I2S_FREQUENCY       16000
#define FRAME_SAMPLES       512
#define RESAMPLE_BUFFER     (FRAME_SAMPLES * EI_FREQUENCY / I2S_FREQUENCY + 1)
#define THRESHOLD_DEFAULT   0.78f
#define MARGIN_DEFAULT      0.15f
#define CONSECUTIVE_HITS_DEFAULT 2
#define COOLDOWN_MS_DEFAULT 2000
#define LABEL_XIAO_JING     "xiao_jing"

/* Sliding window configuration:
 * Window size: 1 second at 16kHz = 16000 samples
 * Stride: 500ms at 16kHz = 8000 samples
 * This allows classification every 500ms with 1 second context */
#define WINDOW_SIZE_SAMPLES     16000  /* 1 second */
#define WINDOW_STRIDE_SAMPLES   8000   /* 500ms */

/* ---- State ---- */

typedef struct {
    bool initialized;
    bool running;

    /* Wake strategy parameters (configurable) */
    float threshold;                /* xj >= threshold to count as hit */
    float margin;                   /* xj > noise + margin AND xj > unknown + margin */
    uint32_t consecutive_hits;      /* Number of consecutive hits to trigger wake */
    uint32_t cooldown_ms;
    int64_t last_detection_us;
    uint32_t detection_count;
    uint32_t frame_count;

    /* Consecutive hit tracking */
    uint32_t current_consecutive;

    /* Sliding window buffer in PSRAM */
    int16_t *window_buffer;         /* PSRAM buffer for full window */
    size_t window_fill;             /* Current fill level in samples */
    size_t window_size;             /* Window size (16000) */
    size_t window_stride;           /* Stride for sliding (8000) */
    uint32_t classifier_trigger_count; /* Times classifier was triggered */

    /* Diagnostics */
    uint32_t edge_feed_calls;
    uint32_t edge_input_samples;
    uint32_t resampled_samples;

    /* Edge-specific amplification stats */
    uint32_t edge_amplified_count;
    uint32_t edge_clipped_count;

    /* Classifier timing */
    int64_t classifier_time_min_us;
    int64_t classifier_time_max_us;
    uint64_t classifier_time_sum_us;
    uint32_t classifier_time_count;

    /* Statistics */
    int64_t create_time_us;
    int64_t start_time_us;
    int64_t stop_time_us;
    uint32_t create_duration_ms;
    uint32_t stop_duration_ms;

    SemaphoreHandle_t mutex;
} edge_state_t;

static edge_state_t s_edge = {0};

/* ---- Resampler ---- */

/**
 * Linear interpolation resampler: 16000 Hz -> 16062 Hz
 * Input: samples_in[0..count_in-1]
 * Output: samples_out[0..*count_out-1]
 *
 * This is a simple linear interpolation that preserves the signal
 * characteristics needed for the Edge Impulse model.
 */
static void resample_16k_to_16062(const int16_t *samples_in, size_t count_in,
                                  int16_t *samples_out, size_t *count_out)
{
    if (count_in == 0) {
        *count_out = 0;
        return;
    }

    /* Calculate output count */
    size_t out_count = (size_t)((uint64_t)count_in * EI_FREQUENCY / I2S_FREQUENCY);
    *count_out = out_count;

    /* Linear interpolation */
    for (size_t i = 0; i < out_count; i++) {
        float pos = (float)i * I2S_FREQUENCY / EI_FREQUENCY;
        size_t idx = (size_t)pos;
        float frac = pos - idx;

        if (idx + 1 < count_in) {
            float sample = samples_in[idx] * (1.0f - frac) + samples_in[idx + 1] * frac;
            samples_out[i] = (int16_t)fmaxf(-32768.0f, fminf(32767.0f, sample));
        } else if (idx < count_in) {
            samples_out[i] = samples_in[idx];
        } else {
            samples_out[i] = 0;
        }
    }
}

/* ---- Backend Operations ---- */

static esp_err_t edge_init(const voice_wake_config_t *config)
{
    if (s_edge.initialized) return ESP_ERR_INVALID_STATE;
    if (!config) return ESP_ERR_INVALID_ARG;

    memset(&s_edge, 0, sizeof(s_edge));

    /* Wake strategy parameters */
    s_edge.threshold = config->threshold > 0 ? config->threshold : THRESHOLD_DEFAULT;
    s_edge.margin = MARGIN_DEFAULT;
    s_edge.consecutive_hits = 1;  /* Single hit for now, consecutive=2 causes too many resets */
    s_edge.cooldown_ms = config->cooldown_ms > 0 ? config->cooldown_ms : COOLDOWN_MS_DEFAULT;
    s_edge.current_consecutive = 0;

    s_edge.mutex = xSemaphoreCreateMutex();
    if (!s_edge.mutex) return ESP_ERR_NO_MEM;

    /* Initialize the Edge Impulse classifier */
    esp_err_t ei_err = ei_classifier_init();
    if (ei_err != ESP_OK) {
        ESP_LOGE(TAG, "classifier init failed: 0x%x", (unsigned)ei_err);
        vSemaphoreDelete(s_edge.mutex);
        return ei_err;
    }

    /* Allocate sliding window buffer in PSRAM
     * Window size: 1 second at 16kHz = 16000 samples
     * Stride: 500ms at 16kHz = 8000 samples */
    s_edge.window_size = WINDOW_SIZE_SAMPLES;
    s_edge.window_stride = WINDOW_STRIDE_SAMPLES;
    size_t window_bytes = s_edge.window_size * sizeof(int16_t);
    s_edge.window_buffer = heap_caps_malloc(window_bytes, MALLOC_CAP_SPIRAM);
    if (!s_edge.window_buffer) {
        ESP_LOGE(TAG, "Failed to allocate window buffer (%u bytes in PSRAM)",
                 (unsigned)window_bytes);
        ei_classifier_deinit();
        vSemaphoreDelete(s_edge.mutex);
        return ESP_ERR_NO_MEM;
    }
    memset(s_edge.window_buffer, 0, window_bytes);
    s_edge.window_fill = 0;

    /* Initialize classifier timing stats */
    s_edge.classifier_time_min_us = INT64_MAX;
    s_edge.classifier_time_max_us = 0;

    s_edge.create_time_us = esp_timer_get_time();
    s_edge.initialized = true;

    /* Log model constants */
    ESP_LOGI(TAG, "MODEL_CONSTANTS:");
    ESP_LOGI(TAG, "  EI_CLASSIFIER_FREQUENCY=%d", EI_CLASSIFIER_FREQUENCY);
    ESP_LOGI(TAG, "  EI_CLASSIFIER_RAW_SAMPLE_COUNT=%d", EI_CLASSIFIER_RAW_SAMPLE_COUNT);
    ESP_LOGI(TAG, "  EI_CLASSIFIER_INTERVAL_MS=%.6f", (double)EI_CLASSIFIER_INTERVAL_MS);
    ESP_LOGI(TAG, "  EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE=%d", EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE);
    ESP_LOGI(TAG, "  EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME=%d", EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME);
    ESP_LOGI(TAG, "  window_size=%u samples (1s)", (unsigned)s_edge.window_size);
    ESP_LOGI(TAG, "  window_stride=%u samples (500ms)", (unsigned)s_edge.window_stride);

    ESP_LOGI(TAG, "initialized (threshold=%.2f, margin=%.2f, consecutive=%u, "
             "cooldown=%u ms, window=%u samples, stride=%u samples)",
             s_edge.threshold, s_edge.margin, (unsigned)s_edge.consecutive_hits,
             (unsigned)s_edge.cooldown_ms,
             (unsigned)s_edge.window_size, (unsigned)s_edge.window_stride);

    return ESP_OK;
}

static esp_err_t edge_start(void)
{
    if (!s_edge.initialized) return ESP_ERR_INVALID_STATE;
    if (s_edge.running) return ESP_OK;

    xSemaphoreTake(s_edge.mutex, portMAX_DELAY);

    s_edge.running = true;
    s_edge.frame_count = 0;
    s_edge.detection_count = 0;
    s_edge.window_fill = 0;
    s_edge.start_time_us = esp_timer_get_time();

    xSemaphoreGive(s_edge.mutex);

    ESP_LOGI(TAG, "started");
    return ESP_OK;
}

static esp_err_t edge_stop(uint32_t timeout_ms)
{
    if (!s_edge.initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_edge.mutex, portMAX_DELAY);

    int64_t t0 = esp_timer_get_time();
    s_edge.running = false;
    s_edge.stop_time_us = esp_timer_get_time();
    s_edge.stop_duration_ms = (uint32_t)((s_edge.stop_time_us - t0) / 1000);

    xSemaphoreGive(s_edge.mutex);

    ESP_LOGI(TAG, "stopped (took %u ms)", (unsigned)s_edge.stop_duration_ms);
    return ESP_OK;
}

static esp_err_t edge_deinit(void)
{
    if (!s_edge.initialized) return ESP_ERR_INVALID_STATE;
    if (s_edge.running) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_edge.mutex, portMAX_DELAY);

    ei_classifier_deinit();

    if (s_edge.window_buffer) {
        heap_caps_free(s_edge.window_buffer);
        s_edge.window_buffer = NULL;
    }

    s_edge.initialized = false;
    SemaphoreHandle_t mutex = s_edge.mutex;
    s_edge.mutex = NULL;
    xSemaphoreGive(mutex);
    vSemaphoreDelete(mutex);

    ESP_LOGI(TAG, "deinitialized");
    return ESP_OK;
}

static esp_err_t edge_feed(const int16_t *samples, size_t count)
{
    if (!s_edge.initialized || !s_edge.running) return ESP_ERR_INVALID_STATE;
    if (!samples || count == 0) return ESP_ERR_INVALID_ARG;
    if (!s_edge.window_buffer) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_edge.mutex, portMAX_DELAY);

    s_edge.frame_count++;
    s_edge.edge_feed_calls++;
    s_edge.edge_input_samples += count;

    /* Check cooldown */
    int64_t now = esp_timer_get_time();
    if (s_edge.last_detection_us > 0) {
        int64_t elapsed = now - s_edge.last_detection_us;
        if (elapsed < (int64_t)s_edge.cooldown_ms * 1000) {
            xSemaphoreGive(s_edge.mutex);
            return ESP_OK;
        }
    }

    /* Sliding window: accumulate samples with Edge-specific 8x gain
     * Old project does (raw>>14)<<3 = *8 after shift.
     * audio_driver outputs neutral >>14 PCM, we amplify here. */
    size_t remaining = count;
    size_t src_offset = 0;

    while (remaining > 0) {
        /* Calculate how many samples we can add to window */
        size_t space = s_edge.window_size - s_edge.window_fill;
        size_t to_copy = (remaining < space) ? remaining : space;

        if (to_copy > 0) {
            /* Apply 8x gain with safe int64 multiply and saturate */
            for (size_t j = 0; j < to_copy; j++) {
                int16_t in_sample = samples[src_offset + j];
                int64_t amplified = (int64_t)in_sample * 8;
                if (amplified > INT16_MAX) amplified = INT16_MAX;
                if (amplified < INT16_MIN) amplified = INT16_MIN;
                s_edge.window_buffer[s_edge.window_fill + j] = (int16_t)amplified;

                /* Track clipping stats */
                s_edge.edge_amplified_count++;
                if (amplified >= INT16_MAX || amplified <= INT16_MIN) {
                    s_edge.edge_clipped_count++;
                }
            }
            s_edge.window_fill += to_copy;
            src_offset += to_copy;
            remaining -= to_copy;
        }

        /* If window is full, run classifier and slide */
        if (s_edge.window_fill >= s_edge.window_size) {
            s_edge.classifier_trigger_count++;

            /* Measure classifier time */
            int64_t t0 = esp_timer_get_time();

            ei_classifier_result_t result;
            esp_err_t err = ei_classify_audio(
                s_edge.window_buffer, s_edge.window_fill, I2S_FREQUENCY, &result);

            int64_t t1 = esp_timer_get_time();
            int64_t elapsed_us = t1 - t0;

            /* Update timing stats */
            if (elapsed_us < s_edge.classifier_time_min_us)
                s_edge.classifier_time_min_us = elapsed_us;
            if (elapsed_us > s_edge.classifier_time_max_us)
                s_edge.classifier_time_max_us = elapsed_us;
            s_edge.classifier_time_sum_us += (uint64_t)elapsed_us;
            s_edge.classifier_time_count++;

            /* Normal S3 inference is about 200-270 ms with Wi-Fi/BLE active.
             * Only report a genuine scheduling/compute anomaly. */
            if (elapsed_us > 500000) {
                ESP_LOGW(TAG, "Classifier slow: %"PRId64" ms", elapsed_us / 1000);
            }

            /* Slide window: move stride samples, keep remainder */
            size_t keep = s_edge.window_size - s_edge.window_stride;
            memmove(s_edge.window_buffer,
                    s_edge.window_buffer + s_edge.window_stride,
                    keep * sizeof(int16_t));
            s_edge.window_fill = keep;
            s_edge.resampled_samples += s_edge.window_stride;

            if (err != ESP_OK) {
                ESP_LOGW(TAG, "classifier error: 0x%x", (unsigned)err);
                xSemaphoreGive(s_edge.mutex);
                return err;
            }

            /* Check for xiao_jing detection with threshold, margin and consecutive hits */
            if (result.detected) {
                /* Check threshold: xj >= wake_threshold (0.80) */
                bool threshold_pass = (result.score >= s_edge.threshold);

                /* Check margin: xj > noise + margin AND xj > unknown + margin */
                bool margin_pass = (result.score > result.noise_score + s_edge.margin) &&
                                   (result.score > result.unknown_score + s_edge.margin);

                /* Log candidate hit */
                ESP_LOGI(TAG, "CANDIDATE: xj=%.3f noise=%.3f unknown=%.3f "
                         "margin_n=%.3f margin_u=%.3f consecutive=%u/%u %s %s",
                         result.score, result.noise_score, result.unknown_score,
                         result.score - result.noise_score,
                         result.score - result.unknown_score,
                         (unsigned)s_edge.current_consecutive + 1,
                         (unsigned)s_edge.consecutive_hits,
                         threshold_pass ? "THRESHOLD_OK" : "THRESHOLD_FAIL",
                         margin_pass ? "MARGIN_OK" : "MARGIN_FAIL");

                if (threshold_pass && margin_pass) {
                    s_edge.current_consecutive++;
                    if (s_edge.current_consecutive >= s_edge.consecutive_hits) {
                        /* Wake detection accepted */
                        s_edge.detection_count++;
                        s_edge.last_detection_us = now;
                        s_edge.current_consecutive = 0;
                        ESP_LOGI(TAG, "DETECTED xiao_jing score=%.3f noise=%.3f unknown=%.3f "
                                 "(count=%u) ACCEPTED",
                                 result.score, result.noise_score, result.unknown_score,
                                 (unsigned)s_edge.detection_count);
                    }
                } else {
                    /* Threshold or margin failed, reset consecutive */
                    s_edge.current_consecutive = 0;
                    if (!threshold_pass) {
                        ESP_LOGI(TAG, "REJECTED xj=%.3f < threshold=%.2f",
                                 result.score, s_edge.threshold);
                    } else {
                        ESP_LOGI(TAG, "REJECTED xj=%.3f noise=%.3f unknown=%.3f margin_fail",
                                 result.score, result.noise_score, result.unknown_score);
                    }
                }
            } else {
                /* Below threshold, reset consecutive */
                s_edge.current_consecutive = 0;
            }
        }
    }

    xSemaphoreGive(s_edge.mutex);
    return ESP_OK;
}

static esp_err_t edge_get_snapshot(voice_wake_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_edge.initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_edge.mutex, portMAX_DELAY);

    out->backend = VOICE_WAKE_BACKEND_EDGE_IMPULSE;
    out->state = s_edge.running ? VOICE_WAKE_STATE_LISTENING : VOICE_WAKE_STATE_STOPPED;
    out->audio_owner = s_edge.running ? VOICE_AUDIO_OWNER_WAKE_EDGE : VOICE_AUDIO_OWNER_NONE;
    out->frame_count = s_edge.frame_count;
    out->detection_count = s_edge.detection_count;
    out->sequence = s_edge.detection_count; /* Simplified */
    out->last_detection_us = s_edge.last_detection_us;
    out->last_start_us = s_edge.start_time_us;
    out->last_stop_us = s_edge.stop_time_us;
    out->stop_duration_ms = s_edge.stop_duration_ms;
    out->create_duration_ms = s_edge.create_duration_ms;
    out->timestamp_us = esp_timer_get_time();

    xSemaphoreGive(s_edge.mutex);
    return ESP_OK;
}

/* ---- Static Operations Table ---- */

static const voice_wake_backend_ops_t s_edge_ops = {
    .init = edge_init,
    .start = edge_start,
    .stop = edge_stop,
    .deinit = edge_deinit,
    .feed = edge_feed,
    .get_snapshot = edge_get_snapshot,
    .name = "edge_impulse_xiao_jing",
};

/* ---- Public API ---- */

const voice_wake_backend_ops_t *voice_wake_edge_get_ops(void)
{
    return &s_edge_ops;
}

esp_err_t voice_wake_edge_init(void)
{
    ESP_LOGI(TAG, "Edge Impulse wake backend ready");
    return ESP_OK;
}

void voice_wake_edge_get_diag(
    uint32_t *edge_feed_calls,
    uint32_t *edge_input_samples,
    uint32_t *resampled_samples,
    uint32_t *window_fill,
    uint32_t *window_size,
    uint32_t *classifier_trigger_count)
{
    if (edge_feed_calls) *edge_feed_calls = s_edge.edge_feed_calls;
    if (edge_input_samples) *edge_input_samples = s_edge.edge_input_samples;
    if (resampled_samples) *resampled_samples = s_edge.resampled_samples;
    if (window_fill) *window_fill = (uint32_t)s_edge.window_fill;
    if (window_size) *window_size = (uint32_t)s_edge.window_size;
    if (classifier_trigger_count) *classifier_trigger_count = s_edge.classifier_trigger_count;
}

void voice_wake_edge_get_amp_stats(
    uint32_t *amplified_count,
    uint32_t *clipped_count)
{
    if (amplified_count) *amplified_count = s_edge.edge_amplified_count;
    if (clipped_count) *clipped_count = s_edge.edge_clipped_count;
}

void voice_wake_edge_get_classifier_timing(
    uint32_t *time_min_ms,
    uint32_t *time_max_ms,
    uint32_t *time_avg_ms,
    uint32_t *call_count)
{
    if (time_min_ms) *time_min_ms = (s_edge.classifier_time_count > 0) ?
        (uint32_t)(s_edge.classifier_time_min_us / 1000) : 0;
    if (time_max_ms) *time_max_ms = (s_edge.classifier_time_count > 0) ?
        (uint32_t)(s_edge.classifier_time_max_us / 1000) : 0;
    if (time_avg_ms) *time_avg_ms = (s_edge.classifier_time_count > 0) ?
        (uint32_t)(s_edge.classifier_time_sum_us / s_edge.classifier_time_count / 1000) : 0;
    if (call_count) *call_count = s_edge.classifier_time_count;
}

void voice_wake_edge_log_diag(void)
{
    ESP_LOGI(TAG, "DIAG: feed_calls=%"PRIu32" input_samples=%"PRIu32
             " resampled=%"PRIu32,
             s_edge.edge_feed_calls, s_edge.edge_input_samples,
             s_edge.resampled_samples);
    ESP_LOGI(TAG, "DIAG: window=%u/%u classifier_triggers=%"PRIu32
             " stride=%u",
             (unsigned)s_edge.window_fill, (unsigned)s_edge.window_size,
             s_edge.classifier_trigger_count, (unsigned)s_edge.window_stride);
    ESP_LOGI(TAG, "DIAG: edge_amplified=%"PRIu32" edge_clipped=%"PRIu32
             " clip_ratio=%.2f%%",
             s_edge.edge_amplified_count, s_edge.edge_clipped_count,
             s_edge.edge_amplified_count > 0 ?
                 100.0 * s_edge.edge_clipped_count / s_edge.edge_amplified_count : 0.0);

    if (s_edge.classifier_time_count > 0) {
        ESP_LOGI(TAG, "DIAG: classifier_time min=%"PRIu32" max=%"PRIu32" avg=%"PRIu32" ms (n=%"PRIu32")",
                 (uint32_t)(s_edge.classifier_time_min_us / 1000),
                 (uint32_t)(s_edge.classifier_time_max_us / 1000),
                 (uint32_t)(s_edge.classifier_time_sum_us / s_edge.classifier_time_count / 1000),
                 s_edge.classifier_time_count);
    }
}
