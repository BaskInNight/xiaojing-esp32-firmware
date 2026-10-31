#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VOICE_STATE_DISABLED = 0,
    VOICE_STATE_IDLE,
    VOICE_STATE_LISTENING,
    VOICE_STATE_WAKE_DETECTED,
    VOICE_STATE_RECORDING,
    VOICE_STATE_UPLOADING,
    VOICE_STATE_WAITING_CLOUD,
    VOICE_STATE_PARSING,
    VOICE_STATE_SUBMITTING,
    VOICE_STATE_SPEAKING,
    VOICE_STATE_ERROR,
} voice_state_t;

typedef enum {
    VOICE_EVENT_ENABLE = 0,
    VOICE_EVENT_DISABLE,
    VOICE_EVENT_FRONTEND_READY,
    VOICE_EVENT_WAKE,
    VOICE_EVENT_PTT_BEGIN,
    VOICE_EVENT_PTT_END,
    VOICE_EVENT_RECORDING_STARTED,
    VOICE_EVENT_UTTERANCE_READY,
    VOICE_EVENT_UPLOAD_STARTED,
    VOICE_EVENT_CLOUD_RESPONSE,
    VOICE_EVENT_TOOL_PARSED,
    VOICE_EVENT_TOOL_ACCEPTED,
    VOICE_EVENT_TOOL_REJECTED,
    VOICE_EVENT_PLAYBACK_DONE,
    VOICE_EVENT_FAILURE,
    VOICE_EVENT_CANCEL,
    VOICE_EVENT_SAFETY_FAULT,
    VOICE_EVENT_TIMEOUT,
} voice_event_t;

typedef enum {
    VOICE_ACTION_NONE = 0,
    VOICE_ACTION_START_FRONTEND,
    VOICE_ACTION_STOP_FRONTEND,
    VOICE_ACTION_START_RECORDING,
    VOICE_ACTION_STOP_AND_UPLOAD,
    VOICE_ACTION_PARSE_RESPONSE,
    VOICE_ACTION_SUBMIT_TOOL,
    VOICE_ACTION_PLAY_ACCEPTED,
    VOICE_ACTION_PLAY_REJECTED,
    VOICE_ACTION_PLAY_ERROR,
    VOICE_ACTION_CANCEL_ALL,
} voice_action_t;

typedef struct {
    voice_state_t state;
    uint32_t generation;
    uint32_t utterance_id;
    uint32_t last_error;
    bool terminal_emitted;
} voice_core_t;

typedef struct {
    voice_action_t action;
    voice_state_t state;
    bool accepted;
} voice_transition_t;

void voice_core_init(voice_core_t *core);
voice_transition_t voice_core_process(
    voice_core_t *core,
    voice_event_t event,
    uint32_t detail);

#ifdef __cplusplus
}
#endif
