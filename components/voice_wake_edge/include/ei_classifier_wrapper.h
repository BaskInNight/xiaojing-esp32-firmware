/* Edge Impulse C wrapper for ESP-IDF
 * Provides C-callable interface to the C++ Edge Impulse classifier. */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Classifier Result ---- */

typedef struct {
    float score;                    /* xiao_jing score */
    float noise_score;              /* noise score */
    float unknown_score;            /* unknown score */
    const char *label;
    bool detected;          /* true if xiao_jing detected above threshold */
    int64_t timing_dsp_ms;
    int64_t timing_inference_ms;
} ei_classifier_result_t;

/* ---- API ---- */

/**
 * Initialize the Edge Impulse classifier.
 * Must be called once before ei_classify_audio().
 */
esp_err_t ei_classifier_init(void);

/**
 * Classify a buffer of 16-bit PCM audio samples.
 * The samples are resampled from sample_rate to EI_CLASSIFIER_FREQUENCY
 * internally if needed.
 *
 * @param samples       Pointer to PCM16 samples
 * @param count         Number of samples
 * @param sample_rate   Sample rate of input (e.g. 16000)
 * @param result        Output result
 * @return ESP_OK on success
 */
esp_err_t ei_classify_audio(const int16_t *samples, size_t count,
                            uint32_t sample_rate,
                            ei_classifier_result_t *result);

/**
 * Get the configured detection threshold.
 */
float ei_classifier_get_threshold(void);

/**
 * Set the detection threshold.
 */
void ei_classifier_set_threshold(float threshold);

/**
 * Deinitialize the classifier.
 */
void ei_classifier_deinit(void);

/**
 * Get diagnostic counters.
 * Any output pointer can be NULL to skip that value.
 */
void ei_classifier_get_diagnostics(
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
    uint32_t *model_window_required);

/**
 * Log diagnostic counters to ESP_LOG.
 */
void ei_classifier_log_diagnostics(void);

#ifdef __cplusplus
}
#endif
