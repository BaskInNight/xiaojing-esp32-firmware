#include <stdatomic.h>
#include <string.h>

#include "unity.h"
#include "voice_cloud.h"
#include "voice_cloud_transport_idf.h"

typedef struct {
    voice_cloud_result_t result;
    int status;
    const char *body;
    int calls;
} p9_cloud_fake_t;

static bool p9_network_ready(void *context)
{
    return context && *(const bool *)context;
}

static voice_cloud_result_t p9_cloud_transport(
    const voice_cloud_transport_request_t *request,
    voice_cloud_transport_response_t *response,
    void *context)
{
    p9_cloud_fake_t *fake = (p9_cloud_fake_t *)context;
    TEST_ASSERT_NOT_NULL(request);
    TEST_ASSERT_NOT_NULL(response);
    TEST_ASSERT_NOT_NULL(fake);
    fake->calls++;
    response->http_status = fake->status;
    if (fake->result != VOICE_CLOUD_OK) return fake->result;
    size_t length = fake->body ? strlen(fake->body) : 0;
    if (length >= response->body_capacity)
        return VOICE_CLOUD_RESPONSE_TOO_LARGE;
    if (length) memcpy(response->body, fake->body, length);
    response->body_length = length;
    return VOICE_CLOUD_OK;
}

static void p9_make_wav(uint8_t wav[48])
{
    static const uint8_t header[44] = {
        'R','I','F','F', 40,0,0,0, 'W','A','V','E',
        'f','m','t',' ', 16,0,0,0,
        1,0, 1,0, 0x80,0x3e,0,0, 0x00,0x7d,0,0,
        2,0, 16,0, 'd','a','t','a', 4,0,0,0,
    };
    memcpy(wav, header, sizeof(header));
    memset(wav + sizeof(header), 0, 4);
}

static voice_cloud_config_t p9_config(p9_cloud_fake_t *fake)
{
    voice_cloud_config_t config = {
        .endpoint =
            "https://token-plan-cn.xiaomimimo.com/v1/chat/completions",
        .api_key = "unit-test-key",
        .model = "mimo-v2.5",
        .connect_timeout_ms = 5000,
        .overall_timeout_ms = 30000,
        .max_wav_bytes = VOICE_CLOUD_DEFAULT_MAX_WAV_BYTES,
        .max_response_bytes = 1024,
        .transport = p9_cloud_transport,
        .transport_context = fake,
    };
    return config;
}

TEST_CASE("VOICE-CLOUD-P9-01 secure config contract",
          "[voice][phase9][group_b]")
{
    p9_cloud_fake_t fake = {0};
    voice_cloud_config_t config = p9_config(&fake);
    TEST_ASSERT_TRUE(voice_cloud_config_valid(&config));
    config.endpoint = "http://example.invalid/v1";
    TEST_ASSERT_FALSE(voice_cloud_config_valid(&config));
    config = p9_config(&fake);
    config.api_key = "key\r\nInjected: yes";
    TEST_ASSERT_FALSE(voice_cloud_config_valid(&config));
}

TEST_CASE("VOICE-CLOUD-P9-02 WAV format contract",
          "[voice][phase9][group_b]")
{
    uint8_t wav[48];
    p9_make_wav(wav);
    TEST_ASSERT_TRUE(voice_cloud_wav_valid(wav, sizeof(wav)));
    wav[34] = 8;
    TEST_ASSERT_FALSE(voice_cloud_wav_valid(wav, sizeof(wav)));
}

TEST_CASE("VOICE-CLOUD-P9-03 provider arguments extraction",
          "[voice][phase9][group_b]")
{
    const char *body =
        "{\"choices\":[{\"message\":{\"tool_calls\":[{\"function\":{"
        "\"arguments\":\"{\\\"tool\\\":\\\"start_program\\\","
        "\\\"program\\\":\\\"formal\\\"}\"}}]}}]}";
    char out[VOICE_CLOUD_TOOL_JSON_CAPACITY];
    size_t length = 0;
    TEST_ASSERT_EQUAL(
        VOICE_CLOUD_OK,
        voice_cloud_extract_tool_json(
            body, strlen(body), out, sizeof(out), &length));
    TEST_ASSERT_EQUAL_STRING(
        "{\"tool\":\"start_program\",\"program\":\"formal\"}", out);
}

TEST_CASE("VOICE-CLOUD-P9-04 forbidden response blocked",
          "[voice][phase9][group_b][safety]")
{
    const char *body =
        "{\"function\":{\"arguments\":\"{\\\"tool\\\":"
        "\\\"motor_on\\\"}\"}}";
    char out[VOICE_CLOUD_TOOL_JSON_CAPACITY];
    size_t length = 0;
    TEST_ASSERT_EQUAL(
        VOICE_CLOUD_BAD_RESPONSE,
        voice_cloud_extract_tool_json(
            body, strlen(body), out, sizeof(out), &length));
}

TEST_CASE("VOICE-CLOUD-P9-05 fake WAV to tool chain",
          "[voice][phase9][group_b]")
{
    const char *body =
        "{\"function\":{\"arguments\":\"{\\\"tool\\\":"
        "\\\"status\\\"}\"}}";
    p9_cloud_fake_t fake = {
        .result = VOICE_CLOUD_OK, .status = 200, .body = body};
    voice_cloud_config_t config = p9_config(&fake);
    uint8_t wav[48];
    char scratch[1024];
    voice_cloud_output_t output;
    p9_make_wav(wav);
    TEST_ASSERT_EQUAL(
        VOICE_CLOUD_OK,
        voice_cloud_run(&config, wav, sizeof(wav), scratch,
                        sizeof(scratch), &output));
    TEST_ASSERT_EQUAL(1, fake.calls);
    TEST_ASSERT_EQUAL(200, output.http_status);
    TEST_ASSERT_EQUAL_STRING("{\"tool\":\"status\"}", output.tool_json);
}

TEST_CASE("VOICE-CLOUD-P9-06 HTTP errors cannot dispatch",
          "[voice][phase9][group_b]")
{
    p9_cloud_fake_t fake = {
        .result = VOICE_CLOUD_OK, .status = 503,
        .body = "{\"error\":\"unavailable\"}"};
    voice_cloud_config_t config = p9_config(&fake);
    uint8_t wav[48];
    char scratch[1024];
    voice_cloud_output_t output;
    p9_make_wav(wav);
    TEST_ASSERT_EQUAL(
        VOICE_CLOUD_HTTP_ERROR,
        voice_cloud_run(&config, wav, sizeof(wav), scratch,
                        sizeof(scratch), &output));
    TEST_ASSERT_EQUAL(503, output.http_status);
}

TEST_CASE("VOICE-CLOUD-P9-07 transport failures preserved",
          "[voice][phase9][group_b]")
{
    const voice_cloud_result_t errors[] = {
        VOICE_CLOUD_TLS_ERROR,
        VOICE_CLOUD_TIMEOUT,
        VOICE_CLOUD_CANCELED,
        VOICE_CLOUD_RESPONSE_TOO_LARGE,
    };
    uint8_t wav[48];
    char scratch[1024];
    voice_cloud_output_t output;
    p9_make_wav(wav);
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        p9_cloud_fake_t fake = {.result = errors[i]};
        voice_cloud_config_t config = p9_config(&fake);
        TEST_ASSERT_EQUAL(
            errors[i],
            voice_cloud_run(&config, wav, sizeof(wav), scratch,
                            sizeof(scratch), &output));
    }
}

TEST_CASE("VOICE-CLOUD-P9-08 IDF cancel context is atomic",
          "[voice][phase9][group_b]")
{
    voice_cloud_idf_context_t context;
    voice_cloud_idf_context_init(&context);
    TEST_ASSERT_FALSE(atomic_load_explicit(
        &context.cancel_requested, memory_order_acquire));
    voice_cloud_idf_cancel(&context);
    TEST_ASSERT_TRUE(atomic_load_explicit(
        &context.cancel_requested, memory_order_acquire));
}

TEST_CASE("VOICE-CLOUD-P9-09 offline rejected before transport",
          "[voice][phase9][group_b][network]")
{
    const char *body =
        "{\"function\":{\"arguments\":\"{\\\"tool\\\":"
        "\\\"status\\\"}\"}}";
    p9_cloud_fake_t fake = {
        .result = VOICE_CLOUD_OK, .status = 200, .body = body};
    voice_cloud_config_t config = p9_config(&fake);
    bool ready = false;
    config.network_ready = p9_network_ready;
    config.network_context = &ready;
    uint8_t wav[48];
    char scratch[1024];
    voice_cloud_output_t output;
    p9_make_wav(wav);
    TEST_ASSERT_EQUAL(
        VOICE_CLOUD_NETWORK_UNAVAILABLE,
        voice_cloud_run(&config, wav, sizeof(wav), scratch,
                        sizeof(scratch), &output));
    TEST_ASSERT_EQUAL(0, fake.calls);
    ready = true;
    TEST_ASSERT_EQUAL(
        VOICE_CLOUD_OK,
        voice_cloud_run(&config, wav, sizeof(wav), scratch,
                        sizeof(scratch), &output));
    TEST_ASSERT_EQUAL(1, fake.calls);
}

void phase9_cloud_tests_force_link(void)
{
}
