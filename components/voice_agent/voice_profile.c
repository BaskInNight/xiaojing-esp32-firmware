#include "voice_profile.h"

static bool wake_selection_valid(voice_wake_selection_t selection)
{
    return selection >= VOICE_WAKE_SELECTION_EDGE_IMPULSE &&
           selection < VOICE_WAKE_SELECTION_COUNT;
}

voice_wake_selection_t voice_wake_next(voice_wake_selection_t current)
{
    return current == VOICE_WAKE_SELECTION_EDGE_IMPULSE
        ? VOICE_WAKE_SELECTION_ESP_SR
        : VOICE_WAKE_SELECTION_EDGE_IMPULSE;
}

voice_wake_switch_result_t voice_wake_request_switch(
    voice_wake_selection_t current,
    const voice_wake_runtime_t *runtime,
    voice_wake_selection_t *out)
{
    if (!runtime || !out || !wake_selection_valid(current))
        return VOICE_WAKE_SWITCH_BAD_ARG;
    if (runtime->machine_state != MACHINE_STATE_IDLE)
        return VOICE_WAKE_SWITCH_MACHINE_BUSY;
    if (!runtime->voice_listening || runtime->command_in_flight)
        return VOICE_WAKE_SWITCH_VOICE_BUSY;
    if (!runtime->audio_capture_ready)
        return VOICE_WAKE_SWITCH_AUDIO_UNAVAILABLE;

    voice_wake_selection_t target = voice_wake_next(current);
    bool ready = target == VOICE_WAKE_SELECTION_EDGE_IMPULSE
        ? runtime->edge_impulse_ready
        : runtime->esp_sr_ready;
    if (!ready) return VOICE_WAKE_SWITCH_BACKEND_UNAVAILABLE;

    *out = target;
    return VOICE_WAKE_SWITCH_OK;
}

voice_route_policy_t voice_invocation_route_policy(
    voice_invocation_source_t source)
{
    return source == VOICE_INVOCATION_BUTTON2_PTT
        ? VOICE_ROUTE_POLICY_CHAT_PRIORITY
        : VOICE_ROUTE_POLICY_AUTO;
}

bool voice_policy_tool_allowed(
    voice_route_policy_t policy,
    voice_tool_t tool)
{
    if (policy < VOICE_ROUTE_POLICY_AUTO ||
        policy >= VOICE_ROUTE_POLICY_COUNT ||
        tool <= VOICE_TOOL_NONE || tool > VOICE_TOOL_ACK_FAULT) {
        return false;
    }
    if (policy == VOICE_ROUTE_POLICY_CHAT_PRIORITY)
        return tool == VOICE_TOOL_STATUS;
    return true;
}
