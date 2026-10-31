#include "voice_core.h"

#include <string.h>

static uint32_t next_nonzero(uint32_t value)
{
    value++;
    return value == 0 ? 1 : value;
}

static voice_transition_t reject(const voice_core_t *core)
{
    voice_transition_t out = {
        .action = VOICE_ACTION_NONE,
        .state = core ? core->state : VOICE_STATE_DISABLED,
        .accepted = false,
    };
    return out;
}

static voice_transition_t accept(voice_core_t *core, voice_state_t state,
                                 voice_action_t action)
{
    core->state = state;
    voice_transition_t out = {
        .action = action,
        .state = state,
        .accepted = true,
    };
    return out;
}

void voice_core_init(voice_core_t *core)
{
    if (!core) return;
    memset(core, 0, sizeof(*core));
    core->state = VOICE_STATE_DISABLED;
}

voice_transition_t voice_core_process(voice_core_t *core,
                                      voice_event_t event,
                                      uint32_t detail)
{
    if (!core) return reject(NULL);

    if (event == VOICE_EVENT_DISABLE) {
        core->generation = next_nonzero(core->generation);
        core->terminal_emitted = false;
        return accept(core, VOICE_STATE_DISABLED, VOICE_ACTION_CANCEL_ALL);
    }
    if (event == VOICE_EVENT_SAFETY_FAULT) {
        core->last_error = detail;
        core->terminal_emitted = false;
        return accept(core, VOICE_STATE_ERROR, VOICE_ACTION_CANCEL_ALL);
    }
    if (event == VOICE_EVENT_CANCEL) {
        core->generation = next_nonzero(core->generation);
        core->terminal_emitted = false;
        return accept(core, VOICE_STATE_IDLE, VOICE_ACTION_CANCEL_ALL);
    }
    if (event == VOICE_EVENT_FAILURE || event == VOICE_EVENT_TIMEOUT) {
        core->last_error = detail;
        core->terminal_emitted = false;
        return accept(core, VOICE_STATE_ERROR, VOICE_ACTION_PLAY_ERROR);
    }

    switch (core->state) {
    case VOICE_STATE_DISABLED:
        if (event == VOICE_EVENT_ENABLE) {
            core->generation = next_nonzero(core->generation);
            core->terminal_emitted = false;
            return accept(core, VOICE_STATE_IDLE,
                          VOICE_ACTION_START_FRONTEND);
        }
        break;
    case VOICE_STATE_IDLE:
        if (event == VOICE_EVENT_FRONTEND_READY) {
            return accept(core, VOICE_STATE_LISTENING, VOICE_ACTION_NONE);
        }
        break;
    case VOICE_STATE_LISTENING:
        if (event == VOICE_EVENT_WAKE ||
            event == VOICE_EVENT_PTT_BEGIN) {
            core->utterance_id = next_nonzero(core->utterance_id);
            core->terminal_emitted = false;
            return accept(core, VOICE_STATE_WAKE_DETECTED,
                          VOICE_ACTION_START_RECORDING);
        }
        break;
    case VOICE_STATE_WAKE_DETECTED:
        if (event == VOICE_EVENT_RECORDING_STARTED) {
            return accept(core, VOICE_STATE_RECORDING, VOICE_ACTION_NONE);
        }
        break;
    case VOICE_STATE_RECORDING:
        if (event == VOICE_EVENT_UTTERANCE_READY ||
            event == VOICE_EVENT_PTT_END) {
            return accept(core, VOICE_STATE_UPLOADING,
                          VOICE_ACTION_STOP_AND_UPLOAD);
        }
        break;
    case VOICE_STATE_UPLOADING:
        if (event == VOICE_EVENT_UPLOAD_STARTED) {
            return accept(core, VOICE_STATE_WAITING_CLOUD,
                          VOICE_ACTION_NONE);
        }
        break;
    case VOICE_STATE_WAITING_CLOUD:
        /* A physical PTT press is an explicit barge-in.  The service layer
         * cancels the in-flight cloud job before starting the new capture. */
        if (event == VOICE_EVENT_PTT_BEGIN) {
            core->utterance_id = next_nonzero(core->utterance_id);
            core->terminal_emitted = false;
            return accept(core, VOICE_STATE_WAKE_DETECTED,
                          VOICE_ACTION_START_RECORDING);
        }
        if (event == VOICE_EVENT_CLOUD_RESPONSE) {
            return accept(core, VOICE_STATE_PARSING,
                          VOICE_ACTION_PARSE_RESPONSE);
        }
        break;
    case VOICE_STATE_PARSING:
        if (event == VOICE_EVENT_PTT_BEGIN) {
            core->utterance_id = next_nonzero(core->utterance_id);
            core->terminal_emitted = false;
            return accept(core, VOICE_STATE_WAKE_DETECTED,
                          VOICE_ACTION_START_RECORDING);
        }
        if (event == VOICE_EVENT_TOOL_PARSED) {
            return accept(core, VOICE_STATE_SUBMITTING,
                          VOICE_ACTION_SUBMIT_TOOL);
        }
        break;
    case VOICE_STATE_SUBMITTING:
        if (event == VOICE_EVENT_PTT_BEGIN) {
            core->utterance_id = next_nonzero(core->utterance_id);
            core->terminal_emitted = false;
            return accept(core, VOICE_STATE_WAKE_DETECTED,
                          VOICE_ACTION_START_RECORDING);
        }
        if (event == VOICE_EVENT_TOOL_ACCEPTED) {
            core->terminal_emitted = true;
            return accept(core, VOICE_STATE_SPEAKING,
                          VOICE_ACTION_PLAY_ACCEPTED);
        }
        if (event == VOICE_EVENT_TOOL_REJECTED) {
            core->terminal_emitted = true;
            return accept(core, VOICE_STATE_SPEAKING,
                          VOICE_ACTION_PLAY_REJECTED);
        }
        break;
    case VOICE_STATE_SPEAKING:
        /* Text is already committed before optional TTS presentation.  A
         * physical PTT press must be able to barge in instead of forcing the
         * user to wait for synthesis plus playback. */
        if (event == VOICE_EVENT_PTT_BEGIN) {
            core->utterance_id = next_nonzero(core->utterance_id);
            core->terminal_emitted = false;
            return accept(core, VOICE_STATE_WAKE_DETECTED,
                          VOICE_ACTION_START_RECORDING);
        }
        if (event == VOICE_EVENT_PLAYBACK_DONE) {
            core->terminal_emitted = false;
            return accept(core, VOICE_STATE_IDLE,
                          VOICE_ACTION_START_FRONTEND);
        }
        break;
    case VOICE_STATE_ERROR:
        /* An error must never permanently lock out the voice UI.  A new
         * wake word or the physical PTT button is a fresh invocation and can
         * start a new recording even if the previous error prompt failed. */
        if (event == VOICE_EVENT_WAKE ||
            event == VOICE_EVENT_PTT_BEGIN) {
            core->utterance_id = next_nonzero(core->utterance_id);
            core->terminal_emitted = false;
            return accept(core, VOICE_STATE_WAKE_DETECTED,
                          VOICE_ACTION_START_RECORDING);
        }
        if (event == VOICE_EVENT_PLAYBACK_DONE) {
            core->terminal_emitted = false;
            return accept(core, VOICE_STATE_IDLE,
                          VOICE_ACTION_START_FRONTEND);
        }
        break;
    default:
        break;
    }
    return reject(core);
}
