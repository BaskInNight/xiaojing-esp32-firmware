#include <string.h>

#include "display_service.h"
#include "unity.h"

TEST_CASE("DISPLAY-P9-01 model lifecycle works without hardware",
          "[display][phase9]")
{
    TEST_ASSERT_EQUAL(ESP_OK, display_service_init());
    display_service_test_reset();
    TEST_ASSERT_EQUAL(ESP_OK, display_service_start());
    display_model_t model;
    TEST_ASSERT_EQUAL(ESP_OK, display_service_get_model(&model));
    TEST_ASSERT_EQUAL(DISPLAY_VOICE_IDLE, model.voice_state);
    TEST_ASSERT_EQUAL(ESP_OK, display_service_stop());
}

TEST_CASE("DISPLAY-P9-02 voice reply is copied into owned model",
          "[display][phase9]")
{
    TEST_ASSERT_EQUAL(ESP_OK, display_service_init());
    display_service_test_reset();
    TEST_ASSERT_EQUAL(ESP_OK,
                      display_service_set_voice_text("start standard wash",
                                                     "OK, starting now"));
    display_model_t model;
    TEST_ASSERT_EQUAL(ESP_OK, display_service_get_model(&model));
    TEST_ASSERT_EQUAL(DISPLAY_VOICE_REPLY, model.voice_state);
    TEST_ASSERT_EQUAL_STRING("start standard wash", model.transcript);
    TEST_ASSERT_EQUAL_STRING("OK, starting now", model.reply);
}

TEST_CASE("DISPLAY-P9-03 invalid progress is rejected atomically",
          "[display][phase9]")
{
    TEST_ASSERT_EQUAL(ESP_OK, display_service_init());
    display_service_test_reset();
    display_model_t before, after;
    TEST_ASSERT_EQUAL(ESP_OK, display_service_get_model(&before));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      display_service_set_wash(true, 101U, "water inlet"));
    TEST_ASSERT_EQUAL(ESP_OK, display_service_get_model(&after));
    TEST_ASSERT_EQUAL_MEMORY(&before, &after, sizeof(before));
}

TEST_CASE("DISPLAY-P9-04 long reply is bounded and terminated",
          "[display][phase9]")
{
    TEST_ASSERT_EQUAL(ESP_OK, display_service_init());
    display_service_test_reset();
    char long_text[800];
    memset(long_text, 'A', sizeof(long_text));
    long_text[sizeof(long_text) - 1U] = '\0';
    TEST_ASSERT_EQUAL(ESP_OK, display_service_set_voice_text("", long_text));
    display_model_t model;
    TEST_ASSERT_EQUAL(ESP_OK, display_service_get_model(&model));
    TEST_ASSERT_EQUAL(sizeof(model.reply) - 1U, strlen(model.reply));
    TEST_ASSERT_EQUAL('\0', model.reply[sizeof(model.reply) - 1U]);
}

TEST_CASE("DISPLAY-P9-05 error state is observable",
          "[display][phase9]")
{
    TEST_ASSERT_EQUAL(ESP_OK, display_service_init());
    display_service_test_reset();
    TEST_ASSERT_EQUAL(ESP_OK,
                      display_service_set_error("network unavailable"));
    display_model_t model;
    TEST_ASSERT_EQUAL(ESP_OK, display_service_get_model(&model));
    TEST_ASSERT_EQUAL(DISPLAY_VOICE_ERROR, model.voice_state);
    TEST_ASSERT_EQUAL_STRING("network unavailable", model.error);
}

TEST_CASE("DISPLAY-P9-06 telemetry is copied into owned model",
          "[display][phase9][telemetry]")
{
    TEST_ASSERT_EQUAL(ESP_OK, display_service_init());
    display_service_test_reset();
    display_telemetry_t telemetry = {
        .machine_state = 3,
        .position_degrees = 90,
        .target_position_degrees = 180,
        .temperature_c = 28.5f,
        .humidity_rh = 61.0f,
        .environment_valid = true,
        .drain_state = 2,
        .uv_state = 1,
        .wake_backend = DISPLAY_WAKE_ESP_SR,
        .internal_free_bytes = 32768,
    };
    TEST_ASSERT_EQUAL(ESP_OK, display_service_set_telemetry(&telemetry));
    memset(&telemetry, 0, sizeof(telemetry));
    display_model_t model;
    TEST_ASSERT_EQUAL(ESP_OK, display_service_get_model(&model));
    TEST_ASSERT_EQUAL(90, model.telemetry.position_degrees);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 28.5f,
                             model.telemetry.temperature_c);
    TEST_ASSERT_EQUAL(2, model.telemetry.drain_state);
    TEST_ASSERT_EQUAL(DISPLAY_WAKE_ESP_SR,
                      model.telemetry.wake_backend);
    TEST_ASSERT_EQUAL(32768, model.telemetry.internal_free_bytes);
}

TEST_CASE("DISPLAY-P9-07 invalid backend rejects telemetry atomically",
          "[display][phase9][telemetry]")
{
    TEST_ASSERT_EQUAL(ESP_OK, display_service_init());
    display_service_test_reset();
    display_model_t before, after;
    TEST_ASSERT_EQUAL(ESP_OK, display_service_get_model(&before));
    display_telemetry_t telemetry = {
        .wake_backend = (display_wake_backend_t)99,
    };
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      display_service_set_telemetry(&telemetry));
    TEST_ASSERT_EQUAL(ESP_OK, display_service_get_model(&after));
    TEST_ASSERT_EQUAL_MEMORY(&before, &after, sizeof(before));
}
