#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "voice_profile.h"
#include "voice_route.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VOICE_CLOUD_TOOL_JSON_CAPACITY 513U
#define VOICE_CLOUD_DEFAULT_MAX_WAV_BYTES (320U * 1024U + 1024U + 44U)
#define VOICE_CLOUD_DEFAULT_MAX_RESPONSE_BYTES 8192U

typedef enum {
    VOICE_CLOUD_AUTH_API_KEY = 0,
    VOICE_CLOUD_AUTH_BEARER,
} voice_cloud_auth_t;

typedef enum {
    VOICE_CLOUD_REQUEST_TOOL_ONLY = 0,
    VOICE_CLOUD_REQUEST_AUTO_ROUTE,
} voice_cloud_request_kind_t;

typedef enum {
    VOICE_CLOUD_OK = 0,
    VOICE_CLOUD_BAD_ARG,
    VOICE_CLOUD_BAD_CONFIG,
    VOICE_CLOUD_BAD_WAV,
    VOICE_CLOUD_WAV_TOO_LARGE,
    VOICE_CLOUD_NETWORK_UNAVAILABLE,
    VOICE_CLOUD_TRANSPORT_ERROR,
    VOICE_CLOUD_TLS_ERROR,
    VOICE_CLOUD_TIMEOUT,
    VOICE_CLOUD_CANCELED,
    VOICE_CLOUD_HTTP_ERROR,
    VOICE_CLOUD_RESPONSE_TOO_LARGE,
    VOICE_CLOUD_BAD_RESPONSE,
    VOICE_CLOUD_TOOL_TOO_LARGE,
} voice_cloud_result_t;

typedef struct {
    const char *endpoint;
    const char *api_key;
    const char *model;
    voice_cloud_auth_t auth;
    voice_cloud_request_kind_t kind;
    voice_invocation_source_t invocation_source;
    voice_route_policy_t route_policy;
    const uint8_t *wav;
    size_t wav_length;
    uint32_t connect_timeout_ms;
    uint32_t overall_timeout_ms;
    size_t max_response_bytes;
} voice_cloud_transport_request_t;

typedef struct {
    char *body;
    size_t body_capacity;
    size_t body_length;
    int http_status;
} voice_cloud_transport_response_t;

typedef voice_cloud_result_t (*voice_cloud_transport_fn)(
    const voice_cloud_transport_request_t *request,
    voice_cloud_transport_response_t *response,
    void *context);
typedef bool (*voice_cloud_network_ready_fn)(void *context);

typedef struct {
    const char *endpoint;
    const char *api_key;
    const char *model;
    voice_cloud_auth_t auth;
    uint32_t connect_timeout_ms;
    uint32_t overall_timeout_ms;
    size_t max_wav_bytes;
    size_t max_response_bytes;
    voice_cloud_network_ready_fn network_ready;
    void *network_context;
    voice_cloud_transport_fn transport;
    void *transport_context;
} voice_cloud_config_t;

typedef struct {
    char tool_json[VOICE_CLOUD_TOOL_JSON_CAPACITY];
    size_t tool_json_length;
    int http_status;
} voice_cloud_output_t;

bool voice_cloud_config_valid(const voice_cloud_config_t *config);
bool voice_cloud_wav_valid(const uint8_t *wav, size_t length);

/* Extract a bounded tool arguments object from an OpenAI-compatible response.
 * A direct tool JSON object is accepted for deterministic Fake transports. */
voice_cloud_result_t voice_cloud_extract_tool_json(
    const char *response, size_t response_length,
    char *out, size_t out_capacity, size_t *out_length);

/* Extract function arguments without applying the machine-tool grammar.
 * AUTO_ROUTE validates the decoded object with voice_route_parse_json(). */
voice_cloud_result_t voice_cloud_extract_arguments_json(
    const char *response, size_t response_length,
    char *out, size_t out_capacity, size_t *out_length);

voice_cloud_result_t voice_cloud_run(
    const voice_cloud_config_t *config,
    const uint8_t *wav, size_t wav_length,
    char *response_scratch, size_t response_capacity,
    voice_cloud_output_t *out);

typedef struct {
    voice_route_result_t route;
    int http_status;
} voice_cloud_route_output_t;

voice_cloud_result_t voice_cloud_run_auto_route(
    const voice_cloud_config_t *config,
    voice_invocation_source_t invocation_source,
    voice_route_policy_t route_policy,
    const uint8_t *wav, size_t wav_length,
    char *response_scratch, size_t response_capacity,
    voice_cloud_route_output_t *out);

#ifdef __cplusplus
}
#endif
