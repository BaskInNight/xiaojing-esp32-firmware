#pragma once

#include "voice_frontend.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Get the Edge Impulse wake backend operations.
 * Returns a pointer to the static ops structure.
 */
const voice_wake_backend_ops_t *voice_wake_edge_get_ops(void);

/**
 * Initialize the Edge Impulse backend.
 * Must be called before registering with voice_frontend.
 */
esp_err_t voice_wake_edge_init(void);

/**
 * Get Edge Impulse diagnostic counters.
 * Any output pointer can be NULL to skip that value.
 */
void voice_wake_edge_get_diag(
    uint32_t *edge_feed_calls,
    uint32_t *edge_input_samples,
    uint32_t *resampled_samples,
    uint32_t *window_fill,
    uint32_t *window_size,
    uint32_t *classifier_trigger_count);

/**
 * Get Edge-specific amplification stats.
 * Any output pointer can be NULL to skip that value.
 */
void voice_wake_edge_get_amp_stats(
    uint32_t *amplified_count,
    uint32_t *clipped_count);

/**
 * Get classifier timing statistics.
 * Any output pointer can be NULL to skip that value.
 */
void voice_wake_edge_get_classifier_timing(
    uint32_t *time_min_ms,
    uint32_t *time_max_ms,
    uint32_t *time_avg_ms,
    uint32_t *call_count);

/**
 * Log Edge Impulse diagnostic counters to ESP_LOG.
 */
void voice_wake_edge_log_diag(void);

#ifdef __cplusplus
}
#endif
