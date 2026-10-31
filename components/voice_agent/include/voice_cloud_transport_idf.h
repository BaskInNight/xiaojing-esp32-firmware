#pragma once

#include <stdatomic.h>

#include "voice_cloud.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    _Atomic bool cancel_requested;
} voice_cloud_idf_context_t;

void voice_cloud_idf_context_init(voice_cloud_idf_context_t *context);
void voice_cloud_idf_cancel(voice_cloud_idf_context_t *context);

/* Original open/write/fetch transport (streaming, manual). */
voice_cloud_result_t voice_cloud_idf_transport(
    const voice_cloud_transport_request_t *request,
    voice_cloud_transport_response_t *response,
    void *context);

/* set_post_field() + perform() transport (buffered, standard). */
voice_cloud_result_t voice_cloud_idf_transport_perform(
    const voice_cloud_transport_request_t *request,
    voice_cloud_transport_response_t *response,
    void *context);

#if VOICE_CLOUD_DIAGNOSTIC_MODE
/* Start auto-transport diagnostic task (runs on boot).
 * If mimo_endpoint and mimo_key are non-NULL, also test against MiMo. */
void voice_cloud_diag_start_ex(const char *mimo_endpoint,
                               const char *mimo_key);
/* Start with default LAN-only test (no MiMo). */
void voice_cloud_diag_start(void);
#endif

#ifdef __cplusplus
}
#endif
