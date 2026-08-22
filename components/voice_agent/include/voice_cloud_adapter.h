#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "voice_cloud.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*voice_record_start_fn)(void *context);
typedef esp_err_t (*voice_record_finish_fn)(
    const uint8_t **wav, size_t *wav_length, uint32_t *token, void *context);
typedef esp_err_t (*voice_record_cancel_fn)(void *context);
typedef void (*voice_record_release_fn)(uint32_t token, void *context);
typedef void (*voice_cloud_cancel_fn)(void *context);

/* Optional test/integration override. If NULL, a successful result is routed
 * into voice_service (CLOUD_RESPONSE -> strict AUTO_ROUTE result) and a failure is
 * routed as VOICE_EVENT_FAILURE. The callback is never invoked under an
 * adapter lock. */
typedef void (*voice_cloud_completion_fn)(
    voice_cloud_result_t result,
    const voice_cloud_route_output_t *output,
    uint32_t token,
    void *context);

typedef struct {
    voice_cloud_config_t cloud;
    voice_record_start_fn record_start;
    voice_record_finish_fn record_finish;
    voice_record_cancel_fn record_cancel;
    voice_record_release_fn record_release;
    voice_cloud_cancel_fn cloud_cancel;
    voice_cloud_completion_fn completion;
    void *record_context;
    void *completion_context;
    uint32_t task_stack_bytes;
    uint32_t task_priority;
    int task_core;
} voice_cloud_adapter_config_t;

typedef struct {
    bool initialized;
    bool running;
    bool job_pending;
    bool job_active;
    bool cancel_requested;
    uint32_t generation;
    uint32_t submitted_jobs;
    uint32_t completed_jobs;
    uint32_t canceled_jobs;
    uint32_t rejected_jobs;
    uint32_t active_token;
    voice_invocation_source_t invocation_source;
    voice_route_policy_t route_policy;
    voice_cloud_result_t last_result;
    esp_err_t last_error;
} voice_cloud_adapter_snapshot_t;

esp_err_t voice_cloud_adapter_global_init(void);
esp_err_t voice_cloud_adapter_init(
    const voice_cloud_adapter_config_t *config);
esp_err_t voice_cloud_adapter_start(void);
esp_err_t voice_cloud_adapter_stop(void);

/* Callbacks compatible with voice_service_config_t. */
esp_err_t voice_cloud_adapter_start_recording(void *context);
esp_err_t voice_cloud_adapter_stop_recording_and_upload(void *context);
esp_err_t voice_cloud_adapter_cancel_io(void *context);
esp_err_t voice_cloud_adapter_set_route_context(
    voice_invocation_source_t source,
    voice_route_policy_t policy,
    void *context);

esp_err_t voice_cloud_adapter_get_snapshot(
    voice_cloud_adapter_snapshot_t *out);

#ifdef XIAOJING_TESTING
void voice_cloud_adapter_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif
