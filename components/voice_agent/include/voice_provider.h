#pragma once

#include <stdbool.h>

#include "voice_cloud.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VOICE_MIMO_ENDPOINT \
    "https://api.xiaomimimo.com/v1/chat/completions"
#define VOICE_MIMO_DEFAULT_MODEL "mimo-v2.5"
#define VOICE_ESPRESSIF_GATEWAY_ENDPOINT \
    "https://ai-gateway.vei.volces.com/v1/chat/completions"
#define VOICE_ESPRESSIF_GATEWAY_DEFAULT_MODEL "mimo-v2.5"
#define VOICE_VOLC_CONVERSATION_ENDPOINT \
    "wss://ai-gateway.vei.volces.com/v1/realtime"

typedef enum {
    VOICE_PROVIDER_MIMO_DIRECT = 0,
    VOICE_PROVIDER_ESPRESSIF_AI_GATEWAY,
    VOICE_PROVIDER_COUNT,
} voice_provider_t;

typedef enum {
    VOICE_PROVIDER_OK = 0,
    VOICE_PROVIDER_BAD_ARG,
    VOICE_PROVIDER_API_KEY_REQUIRED,
    VOICE_PROVIDER_MODEL_REQUIRED,
    VOICE_PROVIDER_BAD_CONFIG,
} voice_provider_result_t;

typedef struct {
    voice_provider_t provider;
    const char *api_key;
    /* Optional for both providers. Both default to mimo-v2.5. AI Gateway
     * projects that expose a different routed alias may override it, but
     * the only secret/credential remains api_key. */
    const char *model_override;
    const char *endpoint_override;
} voice_provider_credentials_t;

voice_provider_result_t voice_provider_build_cloud_config(
    const voice_provider_credentials_t *credentials,
    voice_cloud_transport_fn transport,
    void *transport_context,
    voice_cloud_network_ready_fn network_ready,
    void *network_context,
    voice_cloud_config_t *out);

/*
 * The Volcengine hardware conversational-agent WebSocket is a separate
 * product/protocol. It does not reuse the AI Gateway API key.
 */
typedef struct {
    const char *instance_id;
    const char *product_key;
    const char *product_secret;
    const char *device_name;
    const char *device_secret;
    const char *bot_id;
} voice_conversation_credentials_t;

bool voice_conversation_registration_ready(
    const voice_conversation_credentials_t *credentials);
bool voice_conversation_session_ready(
    const voice_conversation_credentials_t *credentials);

#ifdef __cplusplus
}
#endif
