#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "voice_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Reuse voice_wake_backend_t from voice_service.h ---- */
typedef voice_wake_backend_t voice_wake_backend_type_t;

/* NONE sentinel for "no backend active" */
#define VOICE_WAKE_BACKEND_NONE     ((voice_wake_backend_type_t)-1)
#define VOICE_WAKE_BACKEND_COUNT    2

/* ---- Audio Owner ---- */

typedef enum {
    VOICE_AUDIO_OWNER_NONE = 0,
    VOICE_AUDIO_OWNER_WAKE_EDGE,
    VOICE_AUDIO_OWNER_WAKE_ESP_SR,
    VOICE_AUDIO_OWNER_PTT_CAPTURE,
    VOICE_AUDIO_OWNER_TTS_PLAYBACK,
} voice_audio_owner_t;

/* ---- Wake Frontend State ---- */

typedef enum {
    VOICE_WAKE_STATE_UNINITIALIZED = 0,
    VOICE_WAKE_STATE_INITIALIZED,
    VOICE_WAKE_STATE_STARTING,
    VOICE_WAKE_STATE_LISTENING,
    VOICE_WAKE_STATE_TRIGGERED,
    VOICE_WAKE_STATE_STOPPING,
    VOICE_WAKE_STATE_STOPPED,
    VOICE_WAKE_STATE_FAULT,
} voice_wake_state_t;

/* ---- Wake Detection Result ---- */

typedef struct {
    bool detected;
    float score;           /* Edge Impulse: 0.0-1.0, ESP-SR: word index */
    uint32_t sequence;     /* Monotonic, skips 0 */
    int64_t timestamp_us;  /* Detection time */
} voice_wake_detection_t;

/* ---- Wake Snapshot (atomic read) ---- */

typedef struct {
    voice_wake_backend_type_t backend;
    voice_wake_state_t state;
    voice_audio_owner_t audio_owner;
    uint32_t frame_count;
    uint32_t detection_count;
    uint32_t sequence;
    int64_t last_detection_us;
    int64_t last_start_us;
    int64_t last_stop_us;
    uint32_t stop_duration_ms;
    uint32_t create_duration_ms;
    int64_t timestamp_us;
} voice_wake_snapshot_t;

/* ---- Wake Backend Configuration ---- */

typedef struct {
    uint32_t sample_rate;       /* 16000 Hz */
    uint32_t frame_samples;    /* Samples per frame (e.g., 512) */
    float threshold;            /* Detection threshold (0.0-1.0) */
    uint32_t cooldown_ms;       /* Minimum time between detections */
    void *model_data;           /* Backend-specific model data */
    size_t model_size;          /* Model data size */
} voice_wake_config_t;

/* ---- Wake Backend Operations ---- */

typedef struct {
    esp_err_t (*init)(const voice_wake_config_t *config);
    esp_err_t (*start)(void);
    esp_err_t (*stop)(uint32_t timeout_ms);
    esp_err_t (*deinit)(void);
    esp_err_t (*feed)(const int16_t *samples, size_t count);
    esp_err_t (*get_snapshot)(voice_wake_snapshot_t *out);
    const char *name;
} voice_wake_backend_ops_t;

/* ---- Wake Detection Callback ---- */

typedef void (*voice_wake_detection_cb_t)(const voice_wake_detection_t *detection,
                                          void *context);

/* ---- Frontend Configuration ---- */

typedef struct {
    voice_wake_backend_type_t default_backend;
    voice_wake_detection_cb_t detection_callback;
    void *detection_context;
    uint32_t i2s_sample_rate;   /* 16000 Hz */
    uint32_t frame_samples;    /* Samples per frame (e.g. 512) */
    uint32_t stack_size;       /* Task stack size */
    uint8_t task_core;         /* Task core affinity */
    uint8_t task_priority;     /* Task priority */
} voice_frontend_config_t;

/* ---- Frontend API ---- */

/**
 * Initialize the voice frontend with specified configuration.
 * Does not start any backend.
 */
esp_err_t voice_frontend_init(const voice_frontend_config_t *config);

/**
 * Start the specified wake backend.
 * Stops current backend if different.
 * Acquires audio ownership.
 */
esp_err_t voice_frontend_start_backend(voice_wake_backend_type_t backend);

/**
 * Stop the current wake backend.
 * Releases audio ownership.
 * Waits for task to stop (deterministic join).
 */
esp_err_t voice_frontend_stop(uint32_t timeout_ms);

/**
 * Feed PCM samples to the frontend for wake detection.
 * Called from audio_controller's capture task during WAKE state.
 * Must be non-blocking.
 *
 * @param samples   PCM16 samples
 * @param count     Number of samples
 * @return ESP_OK on success
 */
esp_err_t voice_frontend_feed(const int16_t *samples, size_t count);

/**
 * Deinitialize the frontend and release all resources.
 * Must be stopped first.
 */
esp_err_t voice_frontend_deinit(void);

/**
 * Get atomic snapshot of frontend state.
 */
esp_err_t voice_frontend_get_snapshot(voice_wake_snapshot_t *out);

/**
 * Get current audio owner.
 */
voice_audio_owner_t voice_frontend_get_audio_owner(void);

/**
 * Check if frontend is in a state that allows backend switch.
 */
bool voice_frontend_can_switch_backend(void);

/**
 * Switch to a different backend (stop current, start new).
 * Only allowed when state is LISTENING and no other operations in progress.
 */
esp_err_t voice_frontend_switch_backend(voice_wake_backend_type_t new_backend,
                                        uint32_t timeout_ms);

/**
 * Temporarily disable wake detection (e.g., during TTS playback).
 * Does not release audio ownership.
 */
esp_err_t voice_frontend_disable_wake(void);

/**
 * Re-enable wake detection after disable.
 */
esp_err_t voice_frontend_enable_wake(void);

/**
 * Register a wake backend implementation.
 * Must be called after voice_frontend_init() and before a backend is started.
 */
esp_err_t voice_frontend_register_backend(voice_wake_backend_type_t type,
                                          const voice_wake_backend_ops_t *ops);

/**
 * Get the currently registered backend type.
 */
voice_wake_backend_type_t voice_frontend_get_current_backend(void);

/**
 * Get the wake callback count.
 */
uint32_t voice_frontend_get_wake_callback_count(void);

/**
 * Get I2S diagnostic counters.
 * Any output pointer can be NULL to skip that value.
 */
void voice_frontend_get_i2s_diag(
    uint32_t *i2s_read_calls,
    uint32_t *i2s_read_ok,
    uint32_t *i2s_bytes,
    uint32_t *i2s_zero_reads,
    uint32_t *i2s_errors,
    uint32_t *frontend_feed_calls,
    uint32_t *frontend_feed_samples);

/**
 * Log diagnostic counters to ESP_LOG.
 */
void voice_frontend_log_diagnostics(void);

#ifdef __cplusplus
}
#endif
