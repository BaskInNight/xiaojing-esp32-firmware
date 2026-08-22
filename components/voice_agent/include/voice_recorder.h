/*
 * voice_recorder.h — Voice recorder with bounded buffer and VAD
 *
 * Accepts PCM from audio_controller via feed function.
 * Implements VAD, silence detection, and timeout-based termination.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Recording Source ---- */

typedef enum {
    VOICE_RECORDER_SOURCE_WAKE = 0,  /* Wake-triggered recording */
    VOICE_RECORDER_SOURCE_PTT,       /* Push-to-talk recording */
} voice_recorder_source_t;

/* ---- Recording End Reason ---- */

typedef enum {
    VOICE_RECORDER_END_NONE = 0,
    VOICE_RECORDER_END_SPEECH_COMPLETE,  /* VAD detected speech end */
    VOICE_RECORDER_END_SILENCE_TIMEOUT,  /* No speech detected */
    VOICE_RECORDER_END_MAX_DURATION,     /* Max recording duration reached */
    VOICE_RECORDER_END_BUFFER_FULL,      /* Buffer full */
    VOICE_RECORDER_END_USER_CANCEL,      /* User cancelled */
    VOICE_RECORDER_END_I2S_ERROR,        /* I2S read error */
    VOICE_RECORDER_END_OVERRUN,          /* Queue/buffer overrun */
} voice_recorder_end_reason_t;

/* ---- Recording Snapshot ---- */

typedef struct {
    bool active;
    uint32_t session_id;
    voice_recorder_source_t source;
    uint32_t pcm_bytes;             /* Total PCM bytes recorded */
    uint32_t duration_ms;           /* Recording duration */
    bool speech_started;            /* VAD detected speech start */
    bool vad_end;                   /* VAD detected speech end */
    uint16_t peak_amplitude;        /* Peak PCM amplitude */
    uint32_t rms_amplitude;         /* RMS amplitude */
    uint32_t silence_ms;            /* Continuous silence duration */
    uint32_t overruns;              /* Queue/buffer overruns */
    voice_recorder_end_reason_t last_end_reason;
} voice_recorder_snapshot_t;

/* ---- Configuration ---- */

typedef struct {
    uint32_t max_pcm_bytes;         /* Max recording buffer size */
    uint32_t max_duration_ms;       /* Max recording duration (default 10s) */
    uint32_t min_speech_ms;         /* Minimum valid speech length */
    uint32_t vad_timeout_ms;        /* Silence timeout after speech (1.0-1.2s) */
    uint32_t empty_timeout_ms;      /* No-speech timeout (2.5-3.0s) */
    uint32_t sample_rate;           /* Sample rate (16000) */
} voice_recorder_config_t;

/* ---- API ---- */

/**
 * Initialize the voice recorder.
 */
esp_err_t voice_recorder_init(const voice_recorder_config_t *config);

/**
 * Deinitialize the voice recorder.
 */
esp_err_t voice_recorder_deinit(void);

/**
 * Begin a new recording session.
 *
 * @param session_id    Unique session ID
 * @param source        Recording source (WAKE or PTT)
 * @return ESP_OK on success
 */
esp_err_t voice_recorder_begin(uint32_t session_id,
                                voice_recorder_source_t source);

/**
 * Feed PCM samples to the recorder.
 * Called from audio_controller's capture task during DIALOG state.
 * Must be non-blocking (bounded buffer write).
 *
 * @param samples   PCM16 samples
 * @param count     Number of samples
 * @return ESP_OK on success, ESP_ERR_NO_MEM if buffer full
 */
esp_err_t voice_recorder_feed(const int16_t *samples, size_t count);

/**
 * End the recording session.
 *
 * @param reason    End reason
 * @return ESP_OK on success
 */
esp_err_t voice_recorder_end(voice_recorder_end_reason_t reason);

/**
 * Abort the recording session (discard recorded data).
 *
 * @param reason    Abort reason
 * @return ESP_OK on success
 */
esp_err_t voice_recorder_abort(voice_recorder_end_reason_t reason);

/**
 * Check if recording should end based on VAD/timeout.
 * Called periodically from audio_controller or diagnostic task.
 *
 * @return End reason if should end, VOICE_RECORDER_END_NONE if should continue
 */
voice_recorder_end_reason_t voice_recorder_check_termination(void);

/**
 * Get recording snapshot.
 */
esp_err_t voice_recorder_get_snapshot(voice_recorder_snapshot_t *out);

/**
 * Get recorded PCM data (after recording ends).
 * Returns pointer to internal buffer - caller must copy before next begin().
 *
 * @param pcm       Output: pointer to PCM data
 * @param pcm_bytes Output: PCM data size in bytes
 * @return ESP_OK on success
 */
esp_err_t voice_recorder_get_data(const uint8_t **pcm, size_t *pcm_bytes);

/**
 * Check if recorder is active (recording in progress).
 */
bool voice_recorder_is_active(void);

#ifdef __cplusplus
}
#endif
