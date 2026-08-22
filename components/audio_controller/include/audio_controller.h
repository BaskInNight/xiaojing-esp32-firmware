/*
 * audio_controller.h — Single I2S owner with state machine
 *
 * One permanent capture task reads I2S and routes PCM based on state.
 * Edge detection and voice service receive PCM via callbacks.
 * All state transitions are async via event queue.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Audio States ---- */

typedef enum {
    AUDIO_STATE_STOPPED = 0,
    AUDIO_STATE_WAKE,               /* PCM → Edge wake detection */
    AUDIO_STATE_TRANSITION_TO_DIALOG, /* Flushing, about to record */
    AUDIO_STATE_DIALOG,             /* PCM → voice service recorder */
    AUDIO_STATE_TRANSITION_TO_WAKE, /* Clearing recorder, restoring Edge */
    AUDIO_STATE_PTT,                /* PCM → PTT recorder */
    AUDIO_STATE_ERROR_RECOVERY,     /* Resetting to WAKE after error */
    AUDIO_STATE_COUNT
} audio_state_t;

/* ---- Audio Events ---- */

typedef enum {
    AUDIO_EVENT_NONE = 0,
    AUDIO_EVENT_WAKE_DETECTED,      /* Edge detected wake word */
    AUDIO_EVENT_DIALOG_START,       /* Start dialog recording */
    AUDIO_EVENT_DIALOG_TERMINAL,    /* Dialog ended (success/timeout/error) */
    AUDIO_EVENT_PTT_START,          /* User pressed PTT button */
    AUDIO_EVENT_PTT_RELEASE,        /* User released PTT button */
    AUDIO_EVENT_ERROR,              /* Error occurred, recover to WAKE */
    AUDIO_EVENT_STOP,               /* Stop audio controller */
    AUDIO_EVENT_COUNT
} audio_event_type_t;

typedef struct {
    audio_event_type_t type;
    uint32_t generation;            /* Session generation for staleness check */
    esp_err_t error;                /* Error code for AUDIO_EVENT_ERROR */
    int64_t timestamp_us;           /* Event timestamp */
} audio_event_t;

/* ---- PCM Feed Callbacks ---- */

/**
 * Feed PCM samples to wake detection backend (Edge/ESP-SR).
 * Called from capture task context during WAKE state.
 * Must be non-blocking and fast.
 *
 * @param samples   PCM16 samples
 * @param count     Number of samples
 * @param context   User context
 * @return ESP_OK on success
 */
typedef esp_err_t (*audio_wake_feed_t)(
    const int16_t *samples, size_t count, void *context);

/**
 * Feed PCM samples to dialog recorder.
 * Called from capture task context during DIALOG state.
 * Must be non-blocking (bounded queue).
 *
 * @param samples   PCM16 samples
 * @param count     Number of samples
 * @param context   User context
 * @return ESP_OK on success
 */
typedef esp_err_t (*audio_dialog_feed_t)(
    const int16_t *samples, size_t count, void *context);

/**
 * Begin dialog recording session.
 * Called when transitioning to DIALOG state.
 *
 * @param session_id    Session generation ID
 * @param context       User context
 * @return ESP_OK on success
 */
typedef esp_err_t (*audio_dialog_begin_t)(uint32_t session_id, void *context);

/**
 * End dialog recording session.
 * Called when transitioning from DIALOG state.
 *
 * @param session_id    Session generation ID
 * @param context       User context
 * @return ESP_OK on success
 */
typedef esp_err_t (*audio_dialog_end_t)(uint32_t session_id, void *context);

/**
 * Check if dialog should terminate.
 * Called from capture task during DIALOG state.
 * Returns true if dialog should end.
 *
 * @param context   User context
 * @return true if dialog should terminate
 */
typedef bool (*audio_dialog_check_terminate_t)(void *context);

/**
 * Read PCM from I2S hardware.
 * Called from capture task context.
 *
 * @param buffer    Output buffer
 * @param size      Buffer size in bytes
 * @param bytes_read Output: bytes read
 * @param timeout_ms Read timeout
 * @return ESP_OK on success
 */
typedef esp_err_t (*audio_i2s_read_t)(
    void *buffer, size_t size, size_t *bytes_read, uint32_t timeout_ms);

/**
 * Start/stop I2S hardware capture.
 */
typedef esp_err_t (*audio_i2s_control_t)(void);

/* ---- Controller Configuration ---- */

typedef struct {
    audio_i2s_read_t i2s_read;          /* I2S read function */
    audio_i2s_control_t i2s_start;      /* Start I2S capture */
    audio_i2s_control_t i2s_stop;       /* Stop I2S capture */
    audio_wake_feed_t wake_feed;        /* Feed to wake backend */
    void *wake_context;                 /* Context for wake_feed */
    audio_dialog_feed_t dialog_feed;    /* Feed to dialog recorder */
    void *dialog_context;               /* Context for dialog_feed */
    audio_dialog_begin_t dialog_begin;  /* Begin dialog session */
    void *dialog_begin_context;         /* Context for dialog_begin */
    audio_dialog_end_t dialog_end;      /* End dialog session */
    void *dialog_end_context;           /* Context for dialog_end */
    audio_dialog_check_terminate_t dialog_check_terminate;  /* Check if dialog should end */
    void *dialog_check_terminate_context;  /* Context for check_terminate */
    uint32_t frame_samples;             /* Samples per frame (e.g. 512) */
    uint32_t event_queue_depth;         /* Event queue depth (e.g. 8) */
    uint32_t wake_cooldown_ms;          /* Cooldown after wake detection */
    uint32_t stack_size;                /* Capture task stack size */
    uint8_t task_core;                  /* Capture task core affinity */
    uint8_t task_priority;              /* Capture task priority */
} audio_controller_config_t;

/* ---- Diagnostic Counters ---- */

typedef struct {
    audio_state_t state;
    uint32_t generation;
    uint32_t i2s_read_calls;
    uint32_t i2s_read_ok;
    uint32_t i2s_bytes;
    uint32_t i2s_errors;
    uint32_t pcm_to_wake;               /* Samples routed to wake */
    uint32_t pcm_to_dialog;             /* Samples routed to dialog */
    uint32_t wake_events;               /* Wake detection events */
    uint32_t dialog_starts;             /* Dialog start events */
    uint32_t dialog_terminals;          /* Dialog terminal events */
    uint32_t wake_resumes;              /* Transitions back to WAKE */
    uint32_t stale_events_rejected;     /* Events with wrong generation */
    uint32_t queue_drops;               /* Events dropped (queue full) */
    uint32_t transition_errors;         /* State transition errors */
    esp_err_t last_transition_error;    /* Last transition error */
    int64_t last_wake_detection_us;     /* Timestamp of last wake */
    int64_t last_dialog_start_us;       /* Timestamp of last dialog start */
    int64_t last_dialog_terminal_us;    /* Timestamp of last dialog end */
    int64_t last_wake_resume_us;        /* Timestamp of last wake resume */

    /* I2S read interval tracking */
    int64_t i2s_read_interval_min_us;
    int64_t i2s_read_interval_max_us;
    uint64_t i2s_read_interval_sum_us;
    uint32_t i2s_read_interval_count;

    /* Classifier interval tracking (from wake_feed calls) */
    int64_t classifier_interval_min_us;
    int64_t classifier_interval_max_us;
    uint64_t classifier_interval_sum_us;
    uint32_t classifier_interval_count;
} audio_controller_diag_t;

/* ---- API ---- */

/**
 * Initialize the audio controller.
 * Must be called once before start.
 */
esp_err_t audio_controller_init(const audio_controller_config_t *config);

/**
 * Start the audio controller.
 * Begins I2S capture and routes PCM to WAKE state.
 */
esp_err_t audio_controller_start(void);

/**
 * Stop the audio controller.
 * Stops I2S capture and transitions to STOPPED state.
 */
esp_err_t audio_controller_stop(void);

/**
 * Send an event to the audio controller.
 * Non-blocking: returns ESP_ERR_TIMEOUT if queue is full.
 *
 * @param event Event to send
 * @return ESP_OK on success, ESP_ERR_TIMEOUT if queue full
 */
esp_err_t audio_controller_send_event(const audio_event_t *event);

/**
 * Get current diagnostic counters.
 */
void audio_controller_get_diag(audio_controller_diag_t *out);

/**
 * Log diagnostic counters to ESP_LOG.
 */
void audio_controller_log_diag(void);

/**
 * Get current audio state.
 */
audio_state_t audio_controller_get_state(void);

/**
 * Get current generation.
 */
uint32_t audio_controller_get_generation(void);

/**
 * Send a wake detection event (convenience function).
 * Non-blocking.
 */
esp_err_t audio_controller_notify_wake(void);

/**
 * Send a dialog terminal event (convenience function).
 * Non-blocking.
 */
esp_err_t audio_controller_notify_dialog_terminal(void);

#ifdef __cplusplus
}
#endif
