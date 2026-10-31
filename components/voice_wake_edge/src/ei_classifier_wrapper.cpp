/* Edge Impulse C++ implementation for ESP-IDF
 * Wraps the C++ Edge Impulse SDK with a C-callable interface. */

#include "ei_classifier_wrapper.h"

#include <cmath>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* Arduino compatibility - provide before SDK includes */
#define ARDUINO_H
#include "Arduino_compat.h"

/* Edge Impulse SDK includes */
#include "model_metadata.h"
#include "ei_run_classifier.h"

static const char *TAG = "ei_classifier";

/* ---- State ---- */

static bool s_initialized = false;
static float s_threshold = EI_CLASSIFIER_THRESHOLD;
static SemaphoreHandle_t s_mutex = NULL;

/* Resample buffer in PSRAM to preserve internal RAM */
static int16_t *s_resampled = NULL;

/* Diagnostic counters */
static uint32_t s_classifier_calls = 0;
static uint32_t s_threshold_hits = 0;
static float s_last_xiao_jing_score = 0.0f;
static float s_last_noise_score = 0.0f;
static float s_last_unknown_score = 0.0f;
static uint32_t s_internal_free_min = UINT32_MAX;
static uint32_t s_largest_block_min = UINT32_MAX;

/* Sampling counters */
static uint32_t s_source_samples_in = 0;
static uint32_t s_resampled_samples_out = 0;
static uint32_t s_model_window_fill = 0;
static uint32_t s_model_window_required = 0;

/* ---- Signal callback for run_classifier ---- */

static const int16_t *s_signal_buffer = NULL;
static size_t s_signal_count = 0;

static int get_signal_data(size_t offset, size_t length, float *out_ptr)
{
    if (!s_signal_buffer || offset + length > s_signal_count) {
        return EIDSP_OUT_OF_BOUNDS;
    }
    for (size_t i = 0; i < length; i++) {
        out_ptr[i] = (float)s_signal_buffer[offset + i];
    }
    return EIDSP_OK;
}

/* ---- Stream-based Resampler (16000 -> 16062 Hz) ---- */

/* Resampler state - maintains phase between calls */
typedef struct {
    double phase;           /* Current phase accumulator (0.0 to 1.0) */
    int16_t prev_sample;    /* Previous input sample for interpolation */
    bool initialized;       /* Whether prev_sample is valid */
    uint32_t total_in;      /* Total input samples processed */
    uint32_t total_out;     /* Total output samples produced */
} resampler_state_t;

static resampler_state_t s_resampler = {0};

/* Reset resampler state (call when starting a new window) */
static void resampler_reset(void)
{
    s_resampler.phase = 0.0;
    s_resampler.prev_sample = 0;
    s_resampler.initialized = false;
    /* Don't reset total counters - they accumulate across windows */
}

/* Stream-based resample: 16000 Hz -> 16062 Hz
 * Maintains phase between calls to avoid drift.
 * Returns number of output samples produced. */
static size_t resample_stream(const int16_t *in, size_t in_count,
                               int16_t *out, size_t out_capacity)
{
    if (in_count == 0) return 0;

    /* Ratio: output_rate / input_rate = 16062 / 16000 */
    const double ratio = (double)EI_CLASSIFIER_FREQUENCY / 16000.0;
    size_t out_idx = 0;

    for (size_t i = 0; i < in_count && out_idx < out_capacity; i++) {
        int16_t cur = in[i];
        int16_t prev = s_resampler.initialized ? s_resampler.prev_sample : cur;

        /* Generate output samples while phase advances */
        while (s_resampler.phase < 1.0 && out_idx < out_capacity) {
            /* Linear interpolation */
            float frac = (float)s_resampler.phase;
            float sample = prev * (1.0f - frac) + cur * frac;
            out[out_idx++] = (int16_t)fmaxf(-32768.0f, fminf(32767.0f, sample));
            s_resampler.phase += 1.0 / ratio;
        }

        /* Advance to next input sample */
        s_resampler.phase -= 1.0;
        s_resampler.prev_sample = cur;
        s_resampler.initialized = true;
    }

    s_resampler.total_in += (uint32_t)in_count;
    s_resampler.total_out += (uint32_t)out_idx;

    return out_idx;
}

/* ---- C API Implementation ---- */

extern "C" esp_err_t ei_classifier_init(void)
{
    if (s_initialized) return ESP_OK;

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    /* Allocate resample buffer in PSRAM */
    size_t resample_size = (EI_CLASSIFIER_RAW_SAMPLE_COUNT + 16) * sizeof(int16_t);
    s_resampled = (int16_t *)heap_caps_malloc(resample_size, MALLOC_CAP_SPIRAM);
    if (!s_resampled) {
        ESP_LOGE(TAG, "Failed to allocate resample buffer (%u bytes in PSRAM)",
                 (unsigned)resample_size);
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Resample buffer: %u bytes at %p (%s)",
             (unsigned)resample_size, s_resampled,
             esp_ptr_external_ram(s_resampled) ? "PSRAM" : "INTERNAL");

    s_threshold = EI_CLASSIFIER_THRESHOLD;
    s_initialized = true;

    ESP_LOGI(TAG, "Edge Impulse classifier initialized");
    ESP_LOGI(TAG, "  project: %s (id=%u)", EI_CLASSIFIER_PROJECT_NAME,
             (unsigned)EI_CLASSIFIER_PROJECT_ID);
    ESP_LOGI(TAG, "  frequency: %u Hz", (unsigned)EI_CLASSIFIER_FREQUENCY);
    ESP_LOGI(TAG, "  sample_count: %u", (unsigned)EI_CLASSIFIER_RAW_SAMPLE_COUNT);
    ESP_LOGI(TAG, "  labels: noise, unknown, xiao_jing");
    ESP_LOGI(TAG, "  threshold: %.2f", s_threshold);
    ESP_LOGI(TAG, "  engine: TFLite (compiled/EON)");

    return ESP_OK;
}

extern "C" esp_err_t ei_classify_audio(const int16_t *samples, size_t count,
                                        uint32_t sample_rate,
                                        ei_classifier_result_t *result)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!samples || !result || count == 0) return ESP_ERR_INVALID_ARG;
    if (!s_resampled) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    memset(result, 0, sizeof(*result));

    /* Log input samples for diagnostics */
    s_source_samples_in += (uint32_t)count;

    /* Resample if needed.  When the source rate is within 1% of the model's
     * native frequency, skip resampling entirely and pad with zeros instead.
     * The old Arduino project fed 16000 Hz audio directly to a 16062 Hz model
     * without resampling and it worked; the resampler's linear interpolation
     * can introduce micro-distortions that degrade the quantized INT8 model. */
    const int16_t *classify_samples;
    size_t classify_count;
    double rate_ratio = (double)sample_rate / (double)EI_CLASSIFIER_FREQUENCY;
    bool skip_resample = (rate_ratio > 0.99 && rate_ratio < 1.01);

    if (!skip_resample && sample_rate != EI_CLASSIFIER_FREQUENCY) {
        /* Reset resampler for each window to ensure consistent phase */
        resampler_reset();

        /* Stream-based resample from sample_rate to EI_CLASSIFIER_FREQUENCY */
        classify_count = resample_stream(
            samples, count, s_resampled, EI_CLASSIFIER_RAW_SAMPLE_COUNT);
        classify_samples = s_resampled;

        s_resampled_samples_out += (uint32_t)classify_count;
        s_model_window_fill = (uint32_t)classify_count;
        s_model_window_required = EI_CLASSIFIER_RAW_SAMPLE_COUNT;
    } else {
        /* No resampling: copy input directly and pad with zeros if needed */
        if (count <= EI_CLASSIFIER_RAW_SAMPLE_COUNT) {
            memcpy(s_resampled, samples, count * sizeof(int16_t));
            if (count < EI_CLASSIFIER_RAW_SAMPLE_COUNT) {
                memset(s_resampled + count, 0,
                       (EI_CLASSIFIER_RAW_SAMPLE_COUNT - count) * sizeof(int16_t));
            }
        } else {
            memcpy(s_resampled, samples,
                   EI_CLASSIFIER_RAW_SAMPLE_COUNT * sizeof(int16_t));
        }
        classify_samples = s_resampled;
        classify_count = EI_CLASSIFIER_RAW_SAMPLE_COUNT;
        s_resampled_samples_out += (uint32_t)classify_count;
        s_model_window_fill = (uint32_t)count;
        s_model_window_required = EI_CLASSIFIER_RAW_SAMPLE_COUNT;
    }

    /* Assert model sample count before running classifier */
    if (classify_count != EI_CLASSIFIER_RAW_SAMPLE_COUNT) {
        ESP_LOGE(TAG, "ASSERTION FAILED: classify_count=%u != EI_CLASSIFIER_RAW_SAMPLE_COUNT=%u",
                 (unsigned)classify_count, (unsigned)EI_CLASSIFIER_RAW_SAMPLE_COUNT);
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Pad or truncate to EI_CLASSIFIER_RAW_SAMPLE_COUNT */
    if (classify_count < EI_CLASSIFIER_RAW_SAMPLE_COUNT) {
        memset(s_resampled + classify_count, 0,
               (EI_CLASSIFIER_RAW_SAMPLE_COUNT - classify_count) * sizeof(int16_t));
        classify_count = EI_CLASSIFIER_RAW_SAMPLE_COUNT;
        classify_samples = s_resampled;
    } else if (classify_count > EI_CLASSIFIER_RAW_SAMPLE_COUNT) {
        classify_count = EI_CLASSIFIER_RAW_SAMPLE_COUNT;
    }

    /* Set up signal */
    s_signal_buffer = classify_samples;
    s_signal_count = classify_count;

    signal_t signal;
    signal.total_length = classify_count;
    signal.get_data = get_signal_data;

    /* Run classifier */
    ei_impulse_result_t ei_result;
    int64_t t0 = esp_timer_get_time();
    EI_IMPULSE_ERROR err = run_classifier(&signal, &ei_result, false);
    int64_t elapsed_us = esp_timer_get_time() - t0;
    int64_t elapsed_ms = elapsed_us / 1000;

    s_signal_buffer = NULL;

    /* A normal S3 inference takes roughly 140-220 ms.  Logging every window
     * floods the UART and perturbs timing; retain only the abnormal-latency
     * signal while the periodic diagnostic reports aggregate progress. */
    if (elapsed_ms > 500) {
        ESP_LOGW(TAG, "classifier unusually slow: %lld ms", elapsed_ms);
    }

    if (err != EI_IMPULSE_OK) {
        ESP_LOGW(TAG, "classifier error: %d", (int)err);
        xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }

    /* Find xiao_jing label */
    float xiao_jing_score = 0.0f;
    float noise_score = 0.0f;
    float unknown_score = 0.0f;
    for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
        if (strcmp(ei_result.classification[i].label, "xiao_jing") == 0) {
            xiao_jing_score = ei_result.classification[i].value;
        } else if (strcmp(ei_result.classification[i].label, "noise") == 0) {
            noise_score = ei_result.classification[i].value;
        } else if (strcmp(ei_result.classification[i].label, "unknown") == 0) {
            unknown_score = ei_result.classification[i].value;
        }
    }

    result->score = xiao_jing_score;
    result->noise_score = noise_score;
    result->unknown_score = unknown_score;
    result->label = "xiao_jing";
    result->detected = (xiao_jing_score >= s_threshold);
    result->timing_dsp_ms = ei_result.timing.dsp;
    result->timing_inference_ms = ei_result.timing.classification;

    /* Update diagnostic counters */
    s_classifier_calls++;
    s_last_xiao_jing_score = xiao_jing_score;
    s_last_noise_score = noise_score;
    s_last_unknown_score = unknown_score;
    if (xiao_jing_score >= s_threshold) {
        s_threshold_hits++;
        ESP_LOGI(TAG, "THRESHOLD_HIT: score=%.3f threshold=%.2f count=%" PRIu32,
                 xiao_jing_score, s_threshold, s_threshold_hits);
    }

    /* Track heap minimums */
    uint32_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (int_free < s_internal_free_min) s_internal_free_min = int_free;
    if (int_largest < s_largest_block_min) s_largest_block_min = int_largest;

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

extern "C" float ei_classifier_get_threshold(void)
{
    return s_threshold;
}

extern "C" void ei_classifier_set_threshold(float threshold)
{
    s_threshold = threshold;
}

extern "C" void ei_classifier_deinit(void)
{
    if (!s_initialized) return;
    if (s_resampled) {
        heap_caps_free(s_resampled);
        s_resampled = NULL;
    }
    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
    s_initialized = false;
    ESP_LOGI(TAG, "classifier deinitialized");
}

extern "C" void ei_classifier_get_diagnostics(
    uint32_t *classifier_calls,
    uint32_t *threshold_hits,
    float *last_xiao_jing_score,
    float *last_noise_score,
    float *last_unknown_score,
    uint32_t *internal_free_min,
    uint32_t *largest_block_min,
    uint32_t *source_samples_in,
    uint32_t *resampled_samples_out,
    uint32_t *model_window_fill,
    uint32_t *model_window_required)
{
    if (classifier_calls) *classifier_calls = s_classifier_calls;
    if (threshold_hits) *threshold_hits = s_threshold_hits;
    if (last_xiao_jing_score) *last_xiao_jing_score = s_last_xiao_jing_score;
    if (last_noise_score) *last_noise_score = s_last_noise_score;
    if (last_unknown_score) *last_unknown_score = s_last_unknown_score;
    if (internal_free_min) *internal_free_min = s_internal_free_min;
    if (largest_block_min) *largest_block_min = s_largest_block_min;
    if (source_samples_in) *source_samples_in = s_source_samples_in;
    if (resampled_samples_out) *resampled_samples_out = s_resampled_samples_out;
    if (model_window_fill) *model_window_fill = s_model_window_fill;
    if (model_window_required) *model_window_required = s_model_window_required;
}

extern "C" void ei_classifier_log_diagnostics(void)
{
    ESP_LOGI(TAG, "DIAG: classifier_calls=%" PRIu32 " threshold_hits=%" PRIu32,
             s_classifier_calls, s_threshold_hits);
    ESP_LOGI(TAG, "DIAG: scores: xj=%.3f n=%.3f u=%.3f",
             s_last_xiao_jing_score, s_last_noise_score, s_last_unknown_score);
    ESP_LOGI(TAG, "DIAG: source_in=%" PRIu32 " resampled_out=%" PRIu32
             " window=%" PRIu32 "/%" PRIu32,
             s_source_samples_in, s_resampled_samples_out,
             s_model_window_fill, s_model_window_required);
    ESP_LOGI(TAG, "DIAG: heap_min: int_free=%" PRIu32 " int_largest=%" PRIu32,
             s_internal_free_min, s_largest_block_min);
    ESP_LOGI(TAG, "DIAG: resampler: total_in=%" PRIu32 " total_out=%" PRIu32
             " ratio=%.6f",
             s_resampler.total_in, s_resampler.total_out,
             s_resampler.total_in > 0 ?
                 (double)s_resampler.total_out / s_resampler.total_in : 0.0);
}
