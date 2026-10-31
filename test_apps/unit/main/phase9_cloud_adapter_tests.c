#include <stdatomic.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"
#include "voice_cloud_adapter.h"

typedef struct {
    uint8_t wav[48];
    _Atomic int starts;
    _Atomic int finishes;
    _Atomic int cancels;
    _Atomic int releases;
    _Atomic int transports;
    _Atomic int completions;
    _Atomic int callback_errors;
    _Atomic bool block_transport;
    _Atomic bool transport_canceled;
    voice_cloud_result_t transport_result;
    voice_cloud_result_t completion_result;
    uint32_t completion_token;
    uint32_t release_token;
} p9a_fake_t;

static void p9a_make_wav(uint8_t wav[48])
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

static esp_err_t p9a_record_start(void *context)
{
    p9a_fake_t *fake = context;
    atomic_fetch_add(&fake->starts, 1);
    return ESP_OK;
}

static esp_err_t p9a_record_finish(
    const uint8_t **wav, size_t *length, uint32_t *token, void *context)
{
    p9a_fake_t *fake = context;
    atomic_fetch_add(&fake->finishes, 1);
    *wav = fake->wav;
    *length = sizeof(fake->wav);
    *token = 42;
    return ESP_OK;
}

static esp_err_t p9a_record_cancel(void *context)
{
    p9a_fake_t *fake = context;
    atomic_fetch_add(&fake->cancels, 1);
    return ESP_OK;
}

static void p9a_record_release(uint32_t token, void *context)
{
    p9a_fake_t *fake = context;
    fake->release_token = token;
    if (token != 42) atomic_fetch_add(&fake->callback_errors, 1);
    atomic_fetch_add(&fake->releases, 1);
}

static void p9a_cloud_cancel(void *context)
{
    p9a_fake_t *fake = context;
    atomic_store(&fake->transport_canceled, true);
    atomic_store(&fake->block_transport, false);
}

static voice_cloud_result_t p9a_transport(
    const voice_cloud_transport_request_t *request,
    voice_cloud_transport_response_t *response,
    void *context)
{
    p9a_fake_t *fake = context;
    if (!request || !response) {
        atomic_fetch_add(&fake->callback_errors, 1);
        return VOICE_CLOUD_TRANSPORT_ERROR;
    }
    atomic_fetch_add(&fake->transports, 1);
    while (atomic_load(&fake->block_transport) &&
           !atomic_load(&fake->transport_canceled)) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (atomic_load(&fake->transport_canceled))
        return VOICE_CLOUD_CANCELED;
    if (fake->transport_result != VOICE_CLOUD_OK)
        return fake->transport_result;
    const char *body =
        "{\"function\":{\"arguments\":\"{\\\"domain\\\":\\\"status\\\"," 
        "\\\"transcript\\\":\\\"device status\\\"," 
        "\\\"reply_text\\\":\\\"idle\\\",\\\"confidence_milli\\\":900,"
        "\\\"requires_confirmation\\\":false,\\\"tool\\\":"
        "\\\"status\\\"}\"}}";
    size_t length = strlen(body);
    if (length >= response->body_capacity) {
        atomic_fetch_add(&fake->callback_errors, 1);
        return VOICE_CLOUD_RESPONSE_TOO_LARGE;
    }
    memcpy(response->body, body, length);
    response->body_length = length;
    response->http_status = 200;
    return VOICE_CLOUD_OK;
}

static void p9a_completion(voice_cloud_result_t result,
                           const voice_cloud_route_output_t *output,
                           uint32_t token, void *context)
{
    p9a_fake_t *fake = context;
    fake->completion_result = result;
    fake->completion_token = token;
    if (result == VOICE_CLOUD_OK) {
        if (!output ||
            output->route.domain != VOICE_ROUTE_DOMAIN_STATUS ||
            output->route.tool.tool != VOICE_TOOL_STATUS)
            atomic_fetch_add(&fake->callback_errors, 1);
    }
    atomic_fetch_add(&fake->completions, 1);
}

static voice_cloud_adapter_config_t p9a_config(p9a_fake_t *fake)
{
    voice_cloud_adapter_config_t config = {
        .cloud = {
            .endpoint = "https://example.invalid/v1/chat/completions",
            .api_key = "unit-key",
            .model = "mimo-v2.5",
            .connect_timeout_ms = 1000,
            .overall_timeout_ms = 5000,
            .max_wav_bytes = 1024,
            .max_response_bytes = 1024,
            .transport = p9a_transport,
            .transport_context = fake,
        },
        .record_start = p9a_record_start,
        .record_finish = p9a_record_finish,
        .record_cancel = p9a_record_cancel,
        .record_release = p9a_record_release,
        .cloud_cancel = p9a_cloud_cancel,
        .completion = p9a_completion,
        .record_context = fake,
        .completion_context = fake,
        /* Exercise the production default.  The worker traverses route JSON
         * parsing (and TLS in production); 6 KiB is below its documented
         * minimum and deterministically trips the ESP32-S3 stack canary. */
        .task_stack_bytes = 0,
        .task_priority = 5,
        .task_core = 1,
    };
    return config;
}

static void p9a_setup(p9a_fake_t *fake)
{
    memset(fake, 0, sizeof(*fake));
    p9a_make_wav(fake->wav);
    fake->transport_result = VOICE_CLOUD_OK;
    voice_cloud_adapter_test_reset();
    voice_cloud_adapter_config_t config = p9a_config(fake);
    TEST_ASSERT_EQUAL(ESP_OK, voice_cloud_adapter_init(&config));
    TEST_ASSERT_EQUAL(ESP_OK, voice_cloud_adapter_start());
}

static void p9a_teardown(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, voice_cloud_adapter_stop());
    voice_cloud_adapter_test_reset();
}

static bool p9a_wait_atomic(_Atomic int *value, int expected,
                            uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ticks == 0) timeout_ticks = 1;
    do {
        if (atomic_load(value) >= expected) return true;
        /* CONFIG_FREERTOS_HZ may be 100, where pdMS_TO_TICKS(5) is zero.
         * One real tick prevents a tight yield loop from expiring its logical
         * millisecond counter before the worker's 250 ms retry can finish. */
        vTaskDelay(1);
    } while ((xTaskGetTickCount() - start) < timeout_ticks);
    return atomic_load(value) >= expected;
}

TEST_CASE("VOICE-CLOUD-P9A-01 async upload completes once",
          "[voice][phase9][group_b][adapter]")
{
    p9a_fake_t fake;
    p9a_setup(&fake);
    TEST_ASSERT_EQUAL(
        ESP_OK, voice_cloud_adapter_start_recording(NULL));
    TEST_ASSERT_EQUAL(
        ESP_OK, voice_cloud_adapter_stop_recording_and_upload(NULL));
    TEST_ASSERT_TRUE(p9a_wait_atomic(&fake.completions, 1, 2000));
    TEST_ASSERT_EQUAL(1, atomic_load(&fake.starts));
    TEST_ASSERT_EQUAL(1, atomic_load(&fake.finishes));
    TEST_ASSERT_EQUAL(1, atomic_load(&fake.transports));
    TEST_ASSERT_EQUAL(1, atomic_load(&fake.releases));
    TEST_ASSERT_EQUAL_UINT32(42, fake.release_token);
    TEST_ASSERT_EQUAL(0, atomic_load(&fake.callback_errors));
    TEST_ASSERT_EQUAL(VOICE_CLOUD_OK, fake.completion_result);
    TEST_ASSERT_EQUAL_UINT32(42, fake.completion_token);
    voice_cloud_adapter_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(
        ESP_OK, voice_cloud_adapter_get_snapshot(&snapshot));
    TEST_ASSERT_EQUAL_UINT32(1, snapshot.completed_jobs);
    TEST_ASSERT_FALSE(snapshot.job_active);
    p9a_teardown();
}

TEST_CASE("VOICE-CLOUD-P9A-02 busy request is rejected",
          "[voice][phase9][group_b][adapter]")
{
    p9a_fake_t fake;
    p9a_setup(&fake);
    atomic_store(&fake.block_transport, true);
    TEST_ASSERT_EQUAL(
        ESP_OK, voice_cloud_adapter_start_recording(NULL));
    TEST_ASSERT_EQUAL(
        ESP_OK, voice_cloud_adapter_stop_recording_and_upload(NULL));
    TEST_ASSERT_TRUE(p9a_wait_atomic(&fake.transports, 1, 1000));
    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_STATE,
        voice_cloud_adapter_start_recording(NULL));
    TEST_ASSERT_EQUAL(ESP_OK, voice_cloud_adapter_cancel_io(NULL));
    TEST_ASSERT_TRUE(p9a_wait_atomic(&fake.releases, 1, 1000));
    TEST_ASSERT_EQUAL(0, atomic_load(&fake.completions));
    p9a_teardown();
}

TEST_CASE("VOICE-CLOUD-P9A-03 invalid WAV releases ownership",
          "[voice][phase9][group_b][adapter]")
{
    p9a_fake_t fake;
    p9a_setup(&fake);
    fake.wav[0] = 'X';
    TEST_ASSERT_EQUAL(
        ESP_OK, voice_cloud_adapter_start_recording(NULL));
    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_SIZE,
        voice_cloud_adapter_stop_recording_and_upload(NULL));
    TEST_ASSERT_EQUAL(1, atomic_load(&fake.releases));
    TEST_ASSERT_EQUAL(0, atomic_load(&fake.transports));
    p9a_teardown();
}

TEST_CASE("VOICE-CLOUD-P9A-04 transport error reaches completion",
          "[voice][phase9][group_b][adapter]")
{
    p9a_fake_t fake;
    p9a_setup(&fake);
    fake.transport_result = VOICE_CLOUD_TLS_ERROR;
    TEST_ASSERT_EQUAL(
        ESP_OK, voice_cloud_adapter_start_recording(NULL));
    TEST_ASSERT_EQUAL(
        ESP_OK, voice_cloud_adapter_stop_recording_and_upload(NULL));
    TEST_ASSERT_TRUE(p9a_wait_atomic(&fake.completions, 1, 1000));
    TEST_ASSERT_EQUAL(VOICE_CLOUD_TLS_ERROR, fake.completion_result);
    TEST_ASSERT_EQUAL(1, atomic_load(&fake.releases));
    p9a_teardown();
}

TEST_CASE("VOICE-CLOUD-P9A-05 stop cancels and joins active upload",
          "[voice][phase9][group_b][adapter][lifecycle]")
{
    p9a_fake_t fake;
    p9a_setup(&fake);
    atomic_store(&fake.block_transport, true);
    TEST_ASSERT_EQUAL(
        ESP_OK, voice_cloud_adapter_start_recording(NULL));
    TEST_ASSERT_EQUAL(
        ESP_OK, voice_cloud_adapter_stop_recording_and_upload(NULL));
    TEST_ASSERT_TRUE(p9a_wait_atomic(&fake.transports, 1, 1000));
    TEST_ASSERT_EQUAL(ESP_OK, voice_cloud_adapter_stop());
    TEST_ASSERT_EQUAL(1, atomic_load(&fake.releases));
    TEST_ASSERT_EQUAL(0, atomic_load(&fake.completions));
    voice_cloud_adapter_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(
        ESP_OK, voice_cloud_adapter_get_snapshot(&snapshot));
    TEST_ASSERT_FALSE(snapshot.initialized);
    TEST_ASSERT_FALSE(snapshot.running);
    voice_cloud_adapter_test_reset();
}

TEST_CASE("VOICE-CLOUD-P9A-06 canceled generation drops stale result",
          "[voice][phase9][group_b][adapter][race]")
{
    p9a_fake_t fake;
    p9a_setup(&fake);
    atomic_store(&fake.block_transport, true);
    TEST_ASSERT_EQUAL(
        ESP_OK, voice_cloud_adapter_start_recording(NULL));
    TEST_ASSERT_EQUAL(
        ESP_OK, voice_cloud_adapter_stop_recording_and_upload(NULL));
    TEST_ASSERT_TRUE(p9a_wait_atomic(&fake.transports, 1, 1000));
    TEST_ASSERT_EQUAL(ESP_OK, voice_cloud_adapter_cancel_io(NULL));
    TEST_ASSERT_TRUE(p9a_wait_atomic(&fake.releases, 1, 1000));
    vTaskDelay(pdMS_TO_TICKS(20));
    TEST_ASSERT_EQUAL(0, atomic_load(&fake.completions));
    voice_cloud_adapter_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(
        ESP_OK, voice_cloud_adapter_get_snapshot(&snapshot));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1, snapshot.canceled_jobs);
    p9a_teardown();
}

void phase9_cloud_adapter_tests_force_link(void)
{
}
