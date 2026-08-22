#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "machine_types.h"
#include "voice_tool_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Phase 9 voice policy has three orthogonal axes:
 *
 *   wake backend      - how a hands-free session is opened
 *   invocation source - wake word or BTN2 push-to-talk
 *   route policy      - AUTO intent routing or CHAT_PRIORITY
 *
 * Selecting a wake backend never selects a cloud provider or grants tools.
 */
typedef enum {
    VOICE_WAKE_SELECTION_EDGE_IMPULSE = 0,
    VOICE_WAKE_SELECTION_ESP_SR,
    VOICE_WAKE_SELECTION_COUNT,
} voice_wake_selection_t;

typedef enum {
    VOICE_INVOCATION_WAKE_WORD = 0,
    VOICE_INVOCATION_BUTTON2_PTT,
    VOICE_INVOCATION_COUNT,
} voice_invocation_source_t;

typedef enum {
    VOICE_ROUTE_POLICY_AUTO = 0,
    VOICE_ROUTE_POLICY_CHAT_PRIORITY,
    VOICE_ROUTE_POLICY_COUNT,
} voice_route_policy_t;

typedef struct {
    machine_state_t machine_state;
    bool voice_listening;
    bool command_in_flight;
    bool audio_capture_ready;
    bool edge_impulse_ready;
    bool esp_sr_ready;
} voice_wake_runtime_t;

typedef enum {
    VOICE_WAKE_SWITCH_OK = 0,
    VOICE_WAKE_SWITCH_BAD_ARG,
    VOICE_WAKE_SWITCH_MACHINE_BUSY,
    VOICE_WAKE_SWITCH_VOICE_BUSY,
    VOICE_WAKE_SWITCH_AUDIO_UNAVAILABLE,
    VOICE_WAKE_SWITCH_BACKEND_UNAVAILABLE,
} voice_wake_switch_result_t;

voice_wake_selection_t voice_wake_next(voice_wake_selection_t current);

/*
 * Evaluate BTN1 long-press wake switching. On failure *out is untouched.
 * The actual stop/select/prompt/start transaction remains in voice_service.
 */
voice_wake_switch_result_t voice_wake_request_switch(
    voice_wake_selection_t current,
    const voice_wake_runtime_t *runtime,
    voice_wake_selection_t *out);

voice_route_policy_t voice_invocation_route_policy(
    voice_invocation_source_t source);

/*
 * CHAT_PRIORITY is deliberately read-only in the baseline: STATUS is the
 * only accepted tool. AUTO may use the complete bounded voice tool set.
 */
bool voice_policy_tool_allowed(
    voice_route_policy_t policy,
    voice_tool_t tool);

#ifdef __cplusplus
}
#endif
