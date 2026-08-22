#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "machine_config.h"
#include "voice_core.h"
#include "voice_profile.h"
#include "voice_route.h"
#include "voice_tool_parser.h"
#include "wash_executor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VOICE_WAKE_BACKEND_EDGE_IMPULSE = 0,
    VOICE_WAKE_BACKEND_ESP_SR,
} voice_wake_backend_t;

typedef enum {
    VOICE_PROMPT_ACCEPTED = 0,
    VOICE_PROMPT_REJECTED,
    VOICE_PROMPT_ERROR,
    VOICE_PROMPT_BACKEND_EDGE,
    VOICE_PROMPT_BACKEND_ESP_SR,
    VOICE_PROMPT_PTT_READY,
    VOICE_PROMPT_CHAT_THINKING,
    VOICE_PROMPT_NEED_CONFIRMATION,
    VOICE_PROMPT_NETWORK_UNAVAILABLE,
} voice_prompt_id_t;

typedef esp_err_t (*voice_select_backend_fn)(
    voice_wake_backend_t backend, void *context);
typedef esp_err_t (*voice_adapter_action_fn)(void *context);
typedef esp_err_t (*voice_prompt_play_fn)(
    voice_prompt_id_t prompt, void *context);
typedef esp_err_t (*voice_speak_text_fn)(
    const char *utf8_text, void *context);
typedef esp_err_t (*voice_set_route_context_fn)(
    voice_invocation_source_t source,
    voice_route_policy_t policy,
    void *context);

typedef struct {
    wash_executor_t *executor;
    const machine_config_t *machine_config;
    voice_wake_backend_t default_backend;
    voice_select_backend_fn select_backend;
    voice_adapter_action_fn start_frontend;
    voice_adapter_action_fn stop_frontend;
    voice_adapter_action_fn start_recording;
    voice_adapter_action_fn stop_recording_and_upload;
    voice_adapter_action_fn cancel_io;
    voice_prompt_play_fn play_prompt;
    voice_speak_text_fn speak_text;
    voice_set_route_context_fn set_route_context;
    uint16_t min_control_confidence_milli;
    void *adapter_context;
} voice_service_config_t;

typedef struct {
    bool initialized;
    bool running;
    bool backend_switching;
    voice_state_t state;
    voice_wake_backend_t backend;
    voice_invocation_source_t invocation_source;
    voice_route_policy_t route_policy;
    voice_route_domain_t last_route_domain;
    voice_route_decision_t last_route_decision;
    uint32_t generation;
    uint32_t utterance_id;
    uint32_t next_program_id;
    uint32_t accepted_commands;
    uint32_t rejected_commands;
    uint32_t backend_switches;
    voice_parse_result_t last_parse_result;
    esp_err_t last_error;
} voice_service_snapshot_t;

esp_err_t voice_service_global_init(void);
esp_err_t voice_service_init(const voice_service_config_t *config);
esp_err_t voice_service_start(void);
esp_err_t voice_service_stop(void);

/* Drives the deterministic core and executes the resulting adapter action.
 * CLOUD_RESPONSE only moves the core to PARSING; the bounded JSON is then
 * supplied through voice_service_process_tool_json(). */
esp_err_t voice_service_handle_event(voice_event_t event, uint32_t detail);
esp_err_t voice_service_process_tool_json(const char *json, size_t length);
esp_err_t voice_service_process_route_result(
    const voice_route_result_t *route);

/* BTN2 push-to-talk: begin on long-press, finish/upload on release. */
esp_err_t voice_service_ptt_begin(void);
esp_err_t voice_service_ptt_end(void);

/* Wake-word audio is recorded by audio_controller rather than the PTT
 * capture adapter.  These APIs advance the same deterministic service state
 * without starting/stopping a second I2S reader. */
esp_err_t voice_service_external_recording_begin(
    voice_invocation_source_t source, voice_route_policy_t policy);
esp_err_t voice_service_external_upload_begin(void);

/* Safe only while LISTENING and the wash executor is IDLE. Stops recognition,
 * selects the alternate backend, plays a confirmation while recognition is
 * disabled, then restarts. Returns ESP_ERR_NOT_SUPPORTED until a real backend
 * selector is wired. Failure rolls back to the previous backend. */
esp_err_t voice_service_toggle_backend(void);
esp_err_t voice_service_get_snapshot(voice_service_snapshot_t *out);

#ifdef XIAOJING_TESTING
void voice_service_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif
