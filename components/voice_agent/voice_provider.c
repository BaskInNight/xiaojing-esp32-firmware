#include "voice_provider.h"

#include <string.h>

static bool present(const char *value)
{
    return value && value[0] != '\0';
}

voice_provider_result_t voice_provider_build_cloud_config(
    const voice_provider_credentials_t *credentials,
    voice_cloud_transport_fn transport,
    void *transport_context,
    voice_cloud_network_ready_fn network_ready,
    void *network_context,
    voice_cloud_config_t *out)
{
    if (!credentials || !out || !transport ||
        credentials->provider < VOICE_PROVIDER_MIMO_DIRECT ||
        credentials->provider >= VOICE_PROVIDER_COUNT) {
        return VOICE_PROVIDER_BAD_ARG;
    }
    if (!present(credentials->api_key))
        return VOICE_PROVIDER_API_KEY_REQUIRED;

    voice_cloud_config_t config;
    memset(&config, 0, sizeof(config));
    config.api_key = credentials->api_key;
    config.transport = transport;
    config.transport_context = transport_context;
    config.network_ready = network_ready;
    config.network_context = network_context;
    /* A normal MiMo upload/response completes in under 10 seconds on the
     * target. Keep each socket operation bounded so a stalled peer can be
     * reconnected once by the adapter instead of leaving the UI thinking for
     * a full minute. */
    config.connect_timeout_ms = 30000;
    config.overall_timeout_ms = 90000;
    config.max_wav_bytes = VOICE_CLOUD_DEFAULT_MAX_WAV_BYTES;
    config.max_response_bytes = VOICE_CLOUD_DEFAULT_MAX_RESPONSE_BYTES;

    if (credentials->provider == VOICE_PROVIDER_MIMO_DIRECT) {
        config.endpoint = present(credentials->endpoint_override)
            ? credentials->endpoint_override : VOICE_MIMO_ENDPOINT;
        config.model = present(credentials->model_override)
            ? credentials->model_override : VOICE_MIMO_DEFAULT_MODEL;
        config.auth = VOICE_CLOUD_AUTH_API_KEY;
    } else {
        config.endpoint = present(credentials->endpoint_override)
            ? credentials->endpoint_override : VOICE_ESPRESSIF_GATEWAY_ENDPOINT;
        config.model = present(credentials->model_override)
            ? credentials->model_override
            : VOICE_ESPRESSIF_GATEWAY_DEFAULT_MODEL;
        config.auth = VOICE_CLOUD_AUTH_BEARER;
    }

    if (!voice_cloud_config_valid(&config))
        return VOICE_PROVIDER_BAD_CONFIG;
    *out = config;
    return VOICE_PROVIDER_OK;
}

bool voice_conversation_registration_ready(
    const voice_conversation_credentials_t *credentials)
{
    return credentials &&
           present(credentials->instance_id) &&
           present(credentials->product_key) &&
           present(credentials->product_secret) &&
           present(credentials->device_name) &&
           present(credentials->bot_id);
}

bool voice_conversation_session_ready(
    const voice_conversation_credentials_t *credentials)
{
    return voice_conversation_registration_ready(credentials) &&
           present(credentials->device_secret);
}
