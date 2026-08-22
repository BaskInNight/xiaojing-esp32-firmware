#include "unity.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "machine_config.h"
#include "voice_frontend.h"
#include "voice_service.h"
#include "wash_executor.h"
#include "wash_executor_adapter.h"

void phase9_voice_tests_force_link(void) { }

#undef TEST_CASE
#define TEST_CASE(name_, desc_) \
    void UNITY_TEST_UID(test_func_) (void); \
    void __attribute__((constructor)) UNITY_TEST_UID(test_reg_helper_) (void) \
    { \
        static test_func test_fn_[] = {&UNITY_TEST_UID(test_func_)}; \
        static test_desc_t UNITY_TEST_UID(test_desc_) = { \
            .name = name_, .desc = desc_, .fn = test_fn_, \
            .file = __FILE__, .line = __LINE__, .test_fn_count = 1, \
            .test_fn_name = NULL, .next = NULL \
        }; \
        unity_testcase_register(&UNITY_TEST_UID(test_desc_)); \
    } \
    void UNITY_TEST_UID(test_func_) (void)

typedef struct {
    uint32_t select_count;
    uint32_t start_count;
    uint32_t stop_count;
    uint32_t record_start_count;
    uint32_t upload_count;
    uint32_t cancel_count;
    uint32_t prompt_count;
    uint32_t speak_count;
    uint32_t route_context_count;
    voice_wake_backend_t selected;
    voice_prompt_id_t last_prompt;
    voice_invocation_source_t last_source;
    voice_route_policy_t last_policy;
    char last_speech[64];
    bool fail_select;
    bool fail_prompt;
    voice_wake_backend_t fail_backend;
} voice_fake_io_t;

typedef struct {
    wash_executor_adapter_t adapter;
    uint8_t adapter_ctx[FAKE_ADAPTER_CTX_SIZE];
    wash_executor_t *exec;
    voice_fake_io_t io;
} voice_fixture_t;

static voice_fixture_t s_fx;

typedef struct {
    bool initialized;
    bool running;
    uint32_t detections;
} frontend_fake_backend_t;

static frontend_fake_backend_t s_fe_edge;
static frontend_fake_backend_t s_fe_sr;
static uint32_t s_fe_callback_count;

static esp_err_t fe_backend_init(frontend_fake_backend_t *backend)
{
    memset(backend, 0, sizeof(*backend));
    backend->initialized = true;
    return ESP_OK;
}

static esp_err_t fe_backend_start(frontend_fake_backend_t *backend)
{
    if (!backend->initialized) return ESP_ERR_INVALID_STATE;
    backend->running = true;
    return ESP_OK;
}

static esp_err_t fe_backend_stop(frontend_fake_backend_t *backend)
{
    backend->running = false;
    return ESP_OK;
}

static esp_err_t fe_backend_deinit(frontend_fake_backend_t *backend)
{
    memset(backend, 0, sizeof(*backend));
    return ESP_OK;
}

static esp_err_t fe_backend_feed(frontend_fake_backend_t *backend,
                                 const int16_t *samples, size_t count)
{
    return backend->running && samples && count ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t fe_backend_snapshot(frontend_fake_backend_t *backend,
                                     voice_wake_backend_type_t type,
                                     voice_wake_snapshot_t *out)
{
    if (!backend->initialized || !out) return ESP_ERR_INVALID_STATE;
    memset(out, 0, sizeof(*out));
    out->backend = type;
    out->state = backend->running
        ? VOICE_WAKE_STATE_LISTENING : VOICE_WAKE_STATE_STOPPED;
    out->detection_count = backend->detections;
    return ESP_OK;
}

static esp_err_t fe_edge_init(const voice_wake_config_t *cfg)
{ (void)cfg; return fe_backend_init(&s_fe_edge); }
static esp_err_t fe_edge_start(void)
{ return fe_backend_start(&s_fe_edge); }
static esp_err_t fe_edge_stop(uint32_t timeout_ms)
{ (void)timeout_ms; return fe_backend_stop(&s_fe_edge); }
static esp_err_t fe_edge_deinit(void)
{ return fe_backend_deinit(&s_fe_edge); }
static esp_err_t fe_edge_feed(const int16_t *samples, size_t count)
{ return fe_backend_feed(&s_fe_edge, samples, count); }
static esp_err_t fe_edge_snapshot(voice_wake_snapshot_t *out)
{ return fe_backend_snapshot(&s_fe_edge, VOICE_WAKE_BACKEND_EDGE_IMPULSE, out); }

static esp_err_t fe_sr_init(const voice_wake_config_t *cfg)
{ (void)cfg; return fe_backend_init(&s_fe_sr); }
static esp_err_t fe_sr_start(void)
{ return fe_backend_start(&s_fe_sr); }
static esp_err_t fe_sr_stop(uint32_t timeout_ms)
{ (void)timeout_ms; return fe_backend_stop(&s_fe_sr); }
static esp_err_t fe_sr_deinit(void)
{ return fe_backend_deinit(&s_fe_sr); }
static esp_err_t fe_sr_feed(const int16_t *samples, size_t count)
{ return fe_backend_feed(&s_fe_sr, samples, count); }
static esp_err_t fe_sr_snapshot(voice_wake_snapshot_t *out)
{ return fe_backend_snapshot(&s_fe_sr, VOICE_WAKE_BACKEND_ESP_SR, out); }

static const voice_wake_backend_ops_t s_fe_edge_ops = {
    .init = fe_edge_init, .start = fe_edge_start, .stop = fe_edge_stop,
    .deinit = fe_edge_deinit, .feed = fe_edge_feed,
    .get_snapshot = fe_edge_snapshot, .name = "test_edge",
};
static const voice_wake_backend_ops_t s_fe_sr_ops = {
    .init = fe_sr_init, .start = fe_sr_start, .stop = fe_sr_stop,
    .deinit = fe_sr_deinit, .feed = fe_sr_feed,
    .get_snapshot = fe_sr_snapshot, .name = "test_esp_sr",
};

static void fe_detection_callback(const voice_wake_detection_t *detection,
                                  void *context)
{
    (void)context;
    if (detection && detection->detected) s_fe_callback_count++;
}

static esp_err_t fake_select(voice_wake_backend_t backend, void *context)
{
    voice_fake_io_t *io = context;
    io->select_count++;
    if (io->fail_select && backend == io->fail_backend)
        return ESP_FAIL;
    io->selected = backend;
    return ESP_OK;
}

static esp_err_t fake_start(void *context)
{
    ((voice_fake_io_t *)context)->start_count++;
    return ESP_OK;
}

static esp_err_t fake_stop(void *context)
{
    ((voice_fake_io_t *)context)->stop_count++;
    return ESP_OK;
}

static esp_err_t fake_record_start(void *context)
{
    ((voice_fake_io_t *)context)->record_start_count++;
    return ESP_OK;
}

static esp_err_t fake_upload(void *context)
{
    ((voice_fake_io_t *)context)->upload_count++;
    return ESP_OK;
}

static esp_err_t fake_cancel(void *context)
{
    ((voice_fake_io_t *)context)->cancel_count++;
    return ESP_OK;
}

static esp_err_t fake_prompt(voice_prompt_id_t prompt, void *context)
{
    voice_fake_io_t *io = context;
    io->prompt_count++;
    io->last_prompt = prompt;
    return io->fail_prompt ? ESP_FAIL : ESP_OK;
}

static esp_err_t fake_speak(const char *text, void *context)
{
    voice_fake_io_t *io = context;
    io->speak_count++;
    snprintf(io->last_speech, sizeof(io->last_speech), "%s", text);
    return ESP_OK;
}

static esp_err_t fake_set_route_context(
    voice_invocation_source_t source,
    voice_route_policy_t policy,
    void *context)
{
    voice_fake_io_t *io = context;
    io->route_context_count++;
    io->last_source = source;
    io->last_policy = policy;
    return ESP_OK;
}

static void fixture_init(void)
{
    memset(&s_fx, 0, sizeof(s_fx));
    voice_service_global_init();
    voice_service_stop();
    voice_service_test_reset();
    fake_adapter_init(&s_fx.adapter, s_fx.adapter_ctx);

    wash_executor_config_t exec_cfg = {
        .step_timeout_default_ms = 5000,
        .reset_timeout_ms = 1000,
        .cancel_timeout_ms = 500,
        .terminal_retry_ms = 50,
    };
    s_fx.exec = wash_executor_create(&exec_cfg, &s_fx.adapter);
    TEST_ASSERT_NOT_NULL(s_fx.exec);
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_start(s_fx.exec));

    machine_config_restore_defaults();
    voice_service_config_t voice_cfg = {
        .executor = s_fx.exec,
        .machine_config = machine_config_get(),
        .default_backend = VOICE_WAKE_BACKEND_EDGE_IMPULSE,
        .select_backend = fake_select,
        .start_frontend = fake_start,
        .stop_frontend = fake_stop,
        .start_recording = fake_record_start,
        .stop_recording_and_upload = fake_upload,
        .cancel_io = fake_cancel,
        .play_prompt = fake_prompt,
        .speak_text = fake_speak,
        .set_route_context = fake_set_route_context,
        .adapter_context = &s_fx.io,
    };
    TEST_ASSERT_EQUAL(ESP_OK, voice_service_init(&voice_cfg));
    TEST_ASSERT_EQUAL(ESP_OK, voice_service_start());
}

static bool finish_active_program(void)
{
    uint32_t handled = 0;
    for (uint32_t tries = 0; tries < 500; tries++) {
        uint32_t count = fake_adapter_dispatch_count(s_fx.adapter_ctx);
        while (handled < count) {
            const fake_dispatch_record_t *record =
                fake_adapter_get_dispatch(s_fx.adapter_ctx, handled);
            if (!record) return false;
            terminal_token_t token = record->token;
            if (wash_executor_publish_terminal(
                    s_fx.exec, &token, sizeof(token), true, 0) != ESP_OK)
                return false;
            handled++;
        }
        wash_exec_snapshot_t snapshot;
        if (wash_executor_get_snapshot(s_fx.exec, &snapshot) != ESP_OK)
            return false;
        if (!snapshot.program_active) return true;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

static void fixture_deinit(void)
{
    if (s_fx.exec) {
        finish_active_program();
    }
    TEST_ASSERT_EQUAL(ESP_OK, voice_service_stop());
    if (s_fx.exec) {
        TEST_ASSERT_EQUAL(ESP_OK, wash_executor_stop(s_fx.exec, 5000));
        TEST_ASSERT_EQUAL(ESP_OK, wash_executor_destroy(s_fx.exec));
        s_fx.exec = NULL;
    }
    voice_service_test_reset();
}

static void drive_to_cloud_response(void)
{
    TEST_ASSERT_EQUAL(ESP_OK,
        voice_service_handle_event(VOICE_EVENT_WAKE, 0));
    TEST_ASSERT_EQUAL(ESP_OK,
        voice_service_handle_event(VOICE_EVENT_UTTERANCE_READY, 0));
    TEST_ASSERT_EQUAL(ESP_OK,
        voice_service_handle_event(VOICE_EVENT_CLOUD_RESPONSE, 0));
}

TEST_CASE("VOICE-P9-01: fake wake cloud planner executor chain",
          "[voice][phase9][group_a]")
{
    fixture_init();
    drive_to_cloud_response();
    const char *json =
        "{\"tool\":\"start_program\",\"program\":\"demo\","
        "\"allow_uv\":false,\"allow_dry\":false}";
    TEST_ASSERT_EQUAL(ESP_OK,
        voice_service_process_tool_json(json, strlen(json)));
    TEST_ASSERT_TRUE(fake_adapter_wait_dispatch(s_fx.adapter_ctx, 1, 3000));

    voice_service_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(ESP_OK, voice_service_get_snapshot(&snapshot));
    TEST_ASSERT_EQUAL(VOICE_STATE_LISTENING, snapshot.state);
    TEST_ASSERT_EQUAL(1, snapshot.accepted_commands);
    TEST_ASSERT_EQUAL(VOICE_PROMPT_ACCEPTED, s_fx.io.last_prompt);
    TEST_ASSERT_EQUAL(1, s_fx.io.record_start_count);
    TEST_ASSERT_EQUAL(1, s_fx.io.upload_count);
    TEST_ASSERT_TRUE(s_fx.io.start_count >= 2);
    fixture_deinit();
}

TEST_CASE("VOICE-P9-02: forbidden hardware tool never dispatches",
          "[voice][phase9][group_a]")
{
    fixture_init();
    drive_to_cloud_response();
    const char *json = "{\"tool\":\"heater_on\"}";
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
        voice_service_process_tool_json(json, strlen(json)));
    TEST_ASSERT_EQUAL(0, fake_adapter_dispatch_count(s_fx.adapter_ctx));
    voice_service_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(ESP_OK, voice_service_get_snapshot(&snapshot));
    TEST_ASSERT_EQUAL(VOICE_STATE_LISTENING, snapshot.state);
    TEST_ASSERT_EQUAL(1, snapshot.rejected_commands);
    TEST_ASSERT_EQUAL(VOICE_PROMPT_ERROR, s_fx.io.last_prompt);
    fixture_deinit();
}

TEST_CASE("VOICE-P9-03: tool JSON rejected outside parsing phase",
          "[voice][phase9][group_a]")
{
    fixture_init();
    const char *json = "{\"tool\":\"status\"}";
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
        voice_service_process_tool_json(json, strlen(json)));
    TEST_ASSERT_EQUAL(0, s_fx.io.prompt_count);
    fixture_deinit();
}

TEST_CASE("VOICE-P9-04: BTN backend toggle commits after restart",
          "[voice][phase9][group_a]")
{
    fixture_init();
    TEST_ASSERT_EQUAL(ESP_OK, voice_service_toggle_backend());
    voice_service_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(ESP_OK, voice_service_get_snapshot(&snapshot));
    TEST_ASSERT_EQUAL(VOICE_WAKE_BACKEND_ESP_SR, snapshot.backend);
    TEST_ASSERT_EQUAL(1, snapshot.backend_switches);
    TEST_ASSERT_EQUAL(VOICE_PROMPT_BACKEND_ESP_SR, s_fx.io.last_prompt);
    TEST_ASSERT_EQUAL(VOICE_WAKE_BACKEND_ESP_SR, s_fx.io.selected);
    fixture_deinit();
}

TEST_CASE("VOICE-P9-05: backend select failure rolls back",
          "[voice][phase9][group_a]")
{
    fixture_init();
    s_fx.io.fail_select = true;
    s_fx.io.fail_backend = VOICE_WAKE_BACKEND_ESP_SR;
    TEST_ASSERT_EQUAL(ESP_FAIL, voice_service_toggle_backend());
    voice_service_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(ESP_OK, voice_service_get_snapshot(&snapshot));
    TEST_ASSERT_EQUAL(VOICE_WAKE_BACKEND_EDGE_IMPULSE, snapshot.backend);
    TEST_ASSERT_EQUAL(0, snapshot.backend_switches);
    TEST_ASSERT_EQUAL(VOICE_WAKE_BACKEND_EDGE_IMPULSE, s_fx.io.selected);
    fixture_deinit();
}

TEST_CASE("VOICE-P9-05A: prompt failure does not roll back backend",
          "[voice][phase9][group_a]")
{
    fixture_init();
    s_fx.io.fail_prompt = true;
    TEST_ASSERT_EQUAL(ESP_OK, voice_service_toggle_backend());
    voice_service_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(ESP_OK, voice_service_get_snapshot(&snapshot));
    TEST_ASSERT_EQUAL(VOICE_WAKE_BACKEND_ESP_SR, snapshot.backend);
    TEST_ASSERT_EQUAL(1, snapshot.backend_switches);
    TEST_ASSERT_EQUAL(1, s_fx.io.prompt_count);
    TEST_ASSERT_EQUAL(VOICE_PROMPT_BACKEND_ESP_SR, s_fx.io.last_prompt);
    fixture_deinit();
}

TEST_CASE("VOICE-P9-05B: backend switch rejected while program active",
          "[voice][phase9][group_a][safety]")
{
    fixture_init();
    drive_to_cloud_response();
    const char *json =
        "{\"tool\":\"start_program\",\"program\":\"demo\","
        "\"allow_uv\":false,\"allow_dry\":false}";
    TEST_ASSERT_EQUAL(ESP_OK,
        voice_service_process_tool_json(json, strlen(json)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
        voice_service_toggle_backend());

    voice_service_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(ESP_OK, voice_service_get_snapshot(&snapshot));
    TEST_ASSERT_EQUAL(VOICE_WAKE_BACKEND_EDGE_IMPULSE, snapshot.backend);
    TEST_ASSERT_EQUAL(0, snapshot.backend_switches);
    fixture_deinit();
}

TEST_CASE("VOICE-P9-06: safety fault cancels IO",
          "[voice][phase9][group_a]")
{
    fixture_init();
    TEST_ASSERT_EQUAL(ESP_OK,
        voice_service_handle_event(VOICE_EVENT_SAFETY_FAULT, 19));
    voice_service_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(ESP_OK, voice_service_get_snapshot(&snapshot));
    TEST_ASSERT_EQUAL(VOICE_STATE_ERROR, snapshot.state);
    TEST_ASSERT_EQUAL(1, s_fx.io.cancel_count);
    fixture_deinit();
}

TEST_CASE("VOICE-P9-07: stop is idempotent",
          "[voice][phase9][group_a]")
{
    fixture_init();
    TEST_ASSERT_EQUAL(ESP_OK, voice_service_stop());
    TEST_ASSERT_EQUAL(ESP_OK, voice_service_stop());
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_stop(s_fx.exec, 5000));
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_destroy(s_fx.exec));
    s_fx.exec = NULL;
    voice_service_test_reset();
}

TEST_CASE("VOICE-P9-08: backend toggle rejected while recording",
          "[voice][phase9][group_a]")
{
    fixture_init();
    TEST_ASSERT_EQUAL(ESP_OK,
        voice_service_handle_event(VOICE_EVENT_WAKE, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
        voice_service_toggle_backend());
    voice_service_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(ESP_OK, voice_service_get_snapshot(&snapshot));
    TEST_ASSERT_EQUAL(VOICE_WAKE_BACKEND_EDGE_IMPULSE, snapshot.backend);
    TEST_ASSERT_EQUAL(0, snapshot.backend_switches);
    fixture_deinit();
}

TEST_CASE("VOICE-P9-09: BTN2 PTT freezes chat-priority context",
          "[voice][phase9][group_b][ptt]")
{
    fixture_init();
    TEST_ASSERT_EQUAL(ESP_OK, voice_service_ptt_begin());
    TEST_ASSERT_EQUAL(ESP_OK, voice_service_ptt_end());
    voice_service_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(ESP_OK, voice_service_get_snapshot(&snapshot));
    TEST_ASSERT_EQUAL(
        VOICE_INVOCATION_BUTTON2_PTT, snapshot.invocation_source);
    TEST_ASSERT_EQUAL(
        VOICE_ROUTE_POLICY_CHAT_PRIORITY, snapshot.route_policy);
    TEST_ASSERT_EQUAL(1, s_fx.io.route_context_count);
    TEST_ASSERT_EQUAL(
        VOICE_INVOCATION_BUTTON2_PTT, s_fx.io.last_source);
    TEST_ASSERT_EQUAL(
        VOICE_ROUTE_POLICY_CHAT_PRIORITY, s_fx.io.last_policy);
    fixture_deinit();
}

TEST_CASE("VOICE-P9-10: BTN2 chat response never dispatches",
          "[voice][phase9][group_b][ptt][route]")
{
    fixture_init();
    TEST_ASSERT_EQUAL(ESP_OK, voice_service_ptt_begin());
    TEST_ASSERT_EQUAL(ESP_OK, voice_service_ptt_end());
    TEST_ASSERT_EQUAL(
        ESP_OK, voice_service_handle_event(VOICE_EVENT_CLOUD_RESPONSE, 0));
    voice_route_result_t route = {
        .domain = VOICE_ROUTE_DOMAIN_CHAT,
        .confidence_milli = 980,
    };
    snprintf(route.reply_text, sizeof(route.reply_text), "你好，我在。");
    TEST_ASSERT_EQUAL(
        ESP_OK, voice_service_process_route_result(&route));
    TEST_ASSERT_EQUAL(0, fake_adapter_dispatch_count(s_fx.adapter_ctx));
    TEST_ASSERT_EQUAL(1, s_fx.io.speak_count);
    TEST_ASSERT_EQUAL_STRING("你好，我在。", s_fx.io.last_speech);
    voice_service_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(ESP_OK, voice_service_get_snapshot(&snapshot));
    TEST_ASSERT_EQUAL(VOICE_STATE_LISTENING, snapshot.state);
    fixture_deinit();
}

TEST_CASE("VOICE-P9-11: BTN2 mutating request requires confirmation",
          "[voice][phase9][group_b][ptt][safety]")
{
    fixture_init();
    TEST_ASSERT_EQUAL(ESP_OK, voice_service_ptt_begin());
    TEST_ASSERT_EQUAL(ESP_OK, voice_service_ptt_end());
    TEST_ASSERT_EQUAL(
        ESP_OK, voice_service_handle_event(VOICE_EVENT_CLOUD_RESPONSE, 0));
    voice_route_result_t route = {
        .domain = VOICE_ROUTE_DOMAIN_CONTROL,
        .confidence_milli = 1000,
        .has_tool = true,
        .tool = {
            .tool = VOICE_TOOL_START_PROGRAM,
            .program_kind = WASH_PROGRAM_DEMO,
            .allow_uv = true,
            .allow_dry = true,
        },
    };
    TEST_ASSERT_EQUAL(
        ESP_OK, voice_service_process_route_result(&route));
    TEST_ASSERT_EQUAL(0, fake_adapter_dispatch_count(s_fx.adapter_ctx));
    TEST_ASSERT_EQUAL(
        VOICE_PROMPT_NEED_CONFIRMATION, s_fx.io.last_prompt);
    voice_service_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(ESP_OK, voice_service_get_snapshot(&snapshot));
    TEST_ASSERT_EQUAL(
        VOICE_ROUTE_DECISION_REQUIRE_CONFIRMATION,
        snapshot.last_route_decision);
    fixture_deinit();
}

TEST_CASE("VOICE-P9-12: wake AUTO route reaches planner executor",
          "[voice][phase9][group_b][auto_route]")
{
    fixture_init();
    drive_to_cloud_response();
    voice_route_result_t route = {
        .domain = VOICE_ROUTE_DOMAIN_CONTROL,
        .confidence_milli = 930,
        .has_tool = true,
        .tool = {
            .tool = VOICE_TOOL_START_PROGRAM,
            .program_kind = WASH_PROGRAM_DEMO,
            .allow_uv = false,
            .allow_dry = false,
        },
    };
    TEST_ASSERT_EQUAL(
        ESP_OK, voice_service_process_route_result(&route));
    TEST_ASSERT_TRUE(
        fake_adapter_wait_dispatch(s_fx.adapter_ctx, 1, 3000));
    voice_service_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(ESP_OK, voice_service_get_snapshot(&snapshot));
    TEST_ASSERT_EQUAL(VOICE_ROUTE_POLICY_AUTO, snapshot.route_policy);
    TEST_ASSERT_EQUAL(
        VOICE_ROUTE_DECISION_EXECUTE_TOOL,
        snapshot.last_route_decision);
    fixture_deinit();
}

TEST_CASE("VOICE-P9-13: first detection survives backend counter reset",
          "[voice][phase9][frontend][backend_switch]")
{
    memset(&s_fe_edge, 0, sizeof(s_fe_edge));
    memset(&s_fe_sr, 0, sizeof(s_fe_sr));
    s_fe_callback_count = 0;
    voice_frontend_config_t config = {
        .default_backend = VOICE_WAKE_BACKEND_EDGE_IMPULSE,
        .detection_callback = fe_detection_callback,
        .i2s_sample_rate = 16000,
        .frame_samples = 16,
    };
    int16_t pcm[16] = {0};

    TEST_ASSERT_EQUAL(ESP_OK, voice_frontend_init(&config));
    TEST_ASSERT_EQUAL(ESP_OK, voice_frontend_register_backend(
        VOICE_WAKE_BACKEND_EDGE_IMPULSE, &s_fe_edge_ops));
    TEST_ASSERT_EQUAL(ESP_OK, voice_frontend_register_backend(
        VOICE_WAKE_BACKEND_ESP_SR, &s_fe_sr_ops));
    TEST_ASSERT_EQUAL(ESP_OK, voice_frontend_start_backend(
        VOICE_WAKE_BACKEND_EDGE_IMPULSE));

    s_fe_edge.detections = 1;
    TEST_ASSERT_EQUAL(ESP_OK, voice_frontend_feed(pcm, 16));
    s_fe_edge.detections = 2;
    TEST_ASSERT_EQUAL(ESP_OK, voice_frontend_feed(pcm, 16));
    TEST_ASSERT_EQUAL_UINT32(2, s_fe_callback_count);

    TEST_ASSERT_EQUAL(ESP_OK, voice_frontend_switch_backend(
        VOICE_WAKE_BACKEND_ESP_SR, 1000));
    s_fe_sr.detections = 1;
    TEST_ASSERT_EQUAL(ESP_OK, voice_frontend_feed(pcm, 16));

    voice_wake_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(ESP_OK, voice_frontend_get_snapshot(&snapshot));
    uint32_t callback_count = s_fe_callback_count;
    uint32_t aggregate_count = snapshot.detection_count;
    voice_wake_backend_type_t backend = snapshot.backend;
    TEST_ASSERT_EQUAL(ESP_OK, voice_frontend_stop(1000));
    TEST_ASSERT_EQUAL(ESP_OK, voice_frontend_deinit());

    TEST_ASSERT_EQUAL_UINT32(3, callback_count);
    TEST_ASSERT_EQUAL_UINT32(3, aggregate_count);
    TEST_ASSERT_EQUAL(VOICE_WAKE_BACKEND_ESP_SR, backend);
}
