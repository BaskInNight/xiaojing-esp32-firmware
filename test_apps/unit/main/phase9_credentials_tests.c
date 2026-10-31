#include <stdatomic.h>
#include <string.h>

#include "ble_transport.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "unity.h"
#include "voice_credentials.h"

static _Atomic uint32_t s_normal_frames;
static _Atomic uint32_t s_secure_frames;

static bool memory_contains(
    const void *memory, size_t memory_length,
    const void *needle, size_t needle_length)
{
    if (!memory || !needle || needle_length == 0U ||
        needle_length > memory_length)
        return false;
    const uint8_t *bytes = memory;
    for (size_t i = 0; i + needle_length <= memory_length; i++) {
        if (memcmp(bytes + i, needle, needle_length) == 0) return true;
    }
    return false;
}

static esp_err_t credential_test_normal_sink(
    const char *frame, size_t length, void *context)
{
    (void)frame;
    (void)length;
    (void)context;
    atomic_fetch_add_explicit(&s_normal_frames, 1U, memory_order_relaxed);
    return ESP_OK;
}

static esp_err_t credential_test_secure_sink(
    const char *frame, size_t length, void *context)
{
    (void)frame;
    (void)length;
    (void)context;
    atomic_fetch_add_explicit(&s_secure_frames, 1U, memory_order_relaxed);
    return ESP_OK;
}

static void credential_test_ready(void)
{
    esp_err_t err = nvs_flash_init();
    TEST_ASSERT_TRUE(err == ESP_OK || err == ESP_ERR_INVALID_STATE);
    TEST_ASSERT_EQUAL(ESP_OK, voice_credentials_global_init());
    TEST_ASSERT_EQUAL(ESP_OK, voice_credentials_test_reset());
}

static voice_credentials_value_t mimo_credentials(void)
{
    voice_credentials_value_t value;
    memset(&value, 0, sizeof(value));
    value.provider = VOICE_PROVIDER_MIMO_DIRECT;
    strcpy(value.api_key, "unit-test-mimo-key");
    strcpy(value.model, "mimo-v2.5");
    return value;
}

TEST_CASE("VOICE-CRED-P9C-01 provider and bounds validation",
          "[voice][phase9][credentials]")
{
    voice_credentials_value_t value = mimo_credentials();
    TEST_ASSERT_TRUE(voice_credentials_value_valid(&value));
    value.provider = VOICE_PROVIDER_COUNT;
    TEST_ASSERT_FALSE(voice_credentials_value_valid(&value));
    value = mimo_credentials();
    value.api_key[0] = '\0';
    TEST_ASSERT_FALSE(voice_credentials_value_valid(&value));
    value = mimo_credentials();
    strcpy(value.api_key, "bad key with spaces");
    TEST_ASSERT_FALSE(voice_credentials_value_valid(&value));
}

TEST_CASE("VOICE-CRED-P9C-02 save requires physical window",
          "[voice][phase9][credentials][security]")
{
    credential_test_ready();
    voice_credentials_value_t value = mimo_credentials();
    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_STATE, voice_credentials_save(&value));
}

TEST_CASE("VOICE-CRED-P9C-03 NVS roundtrip and snapshot redaction",
          "[voice][phase9][credentials][nvs]")
{
    credential_test_ready();
    voice_credentials_value_t value = mimo_credentials();
    TEST_ASSERT_EQUAL(
        ESP_OK, voice_credentials_open_provisioning_window(1000));
    TEST_ASSERT_EQUAL(ESP_OK, voice_credentials_save(&value));
    voice_credentials_value_t loaded;
    memset(&loaded, 0, sizeof(loaded));
    TEST_ASSERT_EQUAL(ESP_OK, voice_credentials_load(&loaded));
    TEST_ASSERT_EQUAL(value.provider, loaded.provider);
    TEST_ASSERT_EQUAL_STRING(value.api_key, loaded.api_key);
    TEST_ASSERT_EQUAL_STRING(value.model, loaded.model);
    voice_credentials_snapshot_t snapshot;
    memset(&snapshot, 0xa5, sizeof(snapshot));
    TEST_ASSERT_EQUAL(ESP_OK, voice_credentials_get_snapshot(&snapshot));
    TEST_ASSERT_TRUE(snapshot.stored);
    TEST_ASSERT_TRUE(snapshot.has_api_key);
    TEST_ASSERT_EQUAL_STRING("mimo-v2.5", snapshot.model);
    TEST_ASSERT_FALSE(memory_contains(
        &snapshot, sizeof(snapshot), "unit-test-mimo-key",
        strlen("unit-test-mimo-key")));
    memset(&loaded, 0, sizeof(loaded));
}

TEST_CASE("VOICE-CRED-P9C-04 secure frame rejects absent authorization",
          "[voice][phase9][credentials][security]")
{
    credential_test_ready();
    const char *frame =
        "{\"v\":1,\"seq\":41,\"cmd\":\"configure_voice\"," 
        "\"provider\":\"mimo_direct\",\"api_key\":\"secret-41\"," 
        "\"model\":\"mimo-v2.5\"}";
    voice_credentials_command_result_t result;
    TEST_ASSERT_EQUAL(
        ESP_OK, voice_credentials_process_secure_frame(
                    frame, strlen(frame), &result));
    TEST_ASSERT_FALSE(result.ok);
    TEST_ASSERT_EQUAL_UINT32(41, result.sequence);
    TEST_ASSERT_EQUAL_STRING("PHYSICAL_AUTH_REQUIRED", result.code);
}

TEST_CASE("VOICE-CRED-P9C-05 secure frame saves and closes window",
          "[voice][phase9][credentials][security][nvs]")
{
    credential_test_ready();
    TEST_ASSERT_EQUAL(
        ESP_OK, voice_credentials_open_provisioning_window(1000));
    const char *frame =
        "{\"v\":1,\"seq\":42,\"cmd\":\"configure_voice\"," 
        "\"provider\":\"mimo_direct\",\"api_key\":\"secret-42\"," 
        "\"model\":\"mimo-v2.5\"}";
    voice_credentials_command_result_t result;
    TEST_ASSERT_EQUAL(
        ESP_OK, voice_credentials_process_secure_frame(
                    frame, strlen(frame), &result));
    TEST_ASSERT_TRUE(result.ok);
    TEST_ASSERT_TRUE(result.restart_required);
    TEST_ASSERT_EQUAL_STRING("VOICE_CONFIG_SAVED", result.code);
    TEST_ASSERT_FALSE(voice_credentials_provisioning_window_is_open());
    voice_credentials_value_t loaded;
    TEST_ASSERT_EQUAL(ESP_OK, voice_credentials_load(&loaded));
    TEST_ASSERT_EQUAL_STRING("secret-42", loaded.api_key);
    memset(&loaded, 0, sizeof(loaded));
}

TEST_CASE("VOICE-CRED-P9C-06 unknown provider rejected without commit",
          "[voice][phase9][credentials][security]")
{
    credential_test_ready();
    TEST_ASSERT_EQUAL(
        ESP_OK, voice_credentials_open_provisioning_window(1000));
    const char *frame =
        "{\"v\":1,\"seq\":43,\"cmd\":\"configure_voice\"," 
        "\"provider\":\"arbitrary_url\",\"api_key\":\"secret-43\"}";
    voice_credentials_command_result_t result;
    TEST_ASSERT_EQUAL(
        ESP_OK, voice_credentials_process_secure_frame(
                    frame, strlen(frame), &result));
    TEST_ASSERT_FALSE(result.ok);
    TEST_ASSERT_EQUAL_STRING("BAD_PROVIDER", result.code);
    voice_credentials_value_t loaded;
    memset(&loaded, 0x5a, sizeof(loaded));
    voice_credentials_value_t before = loaded;
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND,
                      voice_credentials_load(&loaded));
    TEST_ASSERT_EQUAL_MEMORY(&before, &loaded, sizeof(loaded));
}

TEST_CASE("VOICE-CRED-P9C-07 erase requires authorization",
          "[voice][phase9][credentials][security][nvs]")
{
    credential_test_ready();
    voice_credentials_value_t value = mimo_credentials();
    TEST_ASSERT_EQUAL(
        ESP_OK, voice_credentials_open_provisioning_window(1000));
    TEST_ASSERT_EQUAL(ESP_OK, voice_credentials_save(&value));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, voice_credentials_erase());
    TEST_ASSERT_EQUAL(
        ESP_OK, voice_credentials_open_provisioning_window(1000));
    TEST_ASSERT_EQUAL(ESP_OK, voice_credentials_erase());
    voice_credentials_value_t loaded;
    TEST_ASSERT_EQUAL(
        ESP_ERR_NVS_NOT_FOUND, voice_credentials_load(&loaded));
}

TEST_CASE("VOICE-CRED-P9C-08 BLE secure assembler is isolated",
          "[voice][phase9][credentials][ble]")
{
    (void)xiaojing_ble_transport_stop();
    (void)ble_transport_test_reset();
    atomic_store_explicit(&s_normal_frames, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_secure_frames, 0U, memory_order_relaxed);
    ble_transport_config_t config = {
        .device_name = "JJTP-CRED-TEST",
        .frame_sink = credential_test_normal_sink,
        .secure_frame_sink = credential_test_secure_sink,
    };
    TEST_ASSERT_EQUAL(ESP_OK, xiaojing_ble_transport_init(&config));
    TEST_ASSERT_EQUAL(ESP_OK, xiaojing_ble_transport_start());
    const char *normal = "{\"v\":1}";
    const char *secure_a = "{\"v\":1,\"api_";
    const char *secure_b = "key\":\"redacted\"}";
    TEST_ASSERT_EQUAL(ESP_OK, ble_transport_test_inject_rx(
        (const uint8_t *)normal, strlen(normal)));
    TEST_ASSERT_EQUAL(ESP_OK, ble_transport_test_inject_secure_rx(
        (const uint8_t *)secure_a, strlen(secure_a)));
    TEST_ASSERT_EQUAL(ESP_OK, ble_transport_test_inject_secure_rx(
        (const uint8_t *)secure_b, strlen(secure_b)));
    for (int i = 0; i < 100 &&
         (atomic_load_explicit(&s_normal_frames, memory_order_relaxed) != 1U ||
          atomic_load_explicit(&s_secure_frames, memory_order_relaxed) != 1U); i++)
        vTaskDelay(pdMS_TO_TICKS(5));
    TEST_ASSERT_EQUAL_UINT32(
        1, atomic_load_explicit(&s_normal_frames, memory_order_relaxed));
    TEST_ASSERT_EQUAL_UINT32(
        1, atomic_load_explicit(&s_secure_frames, memory_order_relaxed));
    ble_transport_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(
        ESP_OK, xiaojing_ble_transport_get_snapshot(&snapshot));
    TEST_ASSERT_EQUAL_UINT32(1, snapshot.rx_frames);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot.secure_rx_frames);
    TEST_ASSERT_EQUAL(ESP_OK, xiaojing_ble_transport_stop());
    TEST_ASSERT_EQUAL(ESP_OK, ble_transport_test_reset());
}

void phase9_credentials_tests_force_link(void)
{
}
