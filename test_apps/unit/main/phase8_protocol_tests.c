#include "unity.h"
#include "app_protocol_core.h"
#include "app_protocol.h"
#include "ble_frame_assembler.h"
#include "ble_transport.h"
#include "machine_config.h"
#include "machine_status_store.h"
#include "wash_executor_adapter.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

void phase8_protocol_tests_force_link(void) {}

typedef struct {
    uint32_t critical_count;
    uint32_t status_count;
    char last_critical[APP_PROTOCOL_CACHED_REPLY_MAX + 1];
    char last_status[512];
} p8_tx_capture_t;

static wash_executor_t *s_p8_exec;
static wash_executor_adapter_t s_p8_adapter;
static uint8_t s_p8_adapter_ctx[FAKE_ADAPTER_CTX_SIZE];
static p8_tx_capture_t s_p8_tx;
static uint32_t s_p8_wifi_provision_count;
static char s_p8_wifi_ssid[33];
static char s_p8_wifi_password[64];

esp_err_t phase8_protocol_tests_cleanup(void);

#ifdef CONFIG_BLE_TRANSPORT_TEST_HOOKS
static _Atomic uint32_t s_p8_ble_frame_count;
static char s_p8_ble_last_frame[BLE_FRAME_MAX_BYTES + 1U];

static esp_err_t p8_ble_frame_sink(const char *frame, size_t length, void *context)
{
    (void)context;
    if (length >= sizeof(s_p8_ble_last_frame)) return ESP_ERR_INVALID_SIZE;
    memcpy(s_p8_ble_last_frame, frame, length);
    s_p8_ble_last_frame[length] = '\0';
    atomic_fetch_add_explicit(&s_p8_ble_frame_count, 1U, memory_order_release);
    return ESP_OK;
}

static esp_err_t p8_ble_protocol_sink(const char *frame, size_t length,
                                      void *context)
{
    (void)context;
    return app_protocol_process_frame(frame, length);
}

static void p8_ble_connection_sink(bool connected, void *context)
{
    (void)context;
    app_protocol_set_connected(connected);
}

static bool p8_wait_value(uint32_t (*read_value)(void), uint32_t target,
                          uint32_t timeout_ms)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    do {
        if (read_value() >= target) return true;
        vTaskDelay(pdMS_TO_TICKS(5));
    } while ((int32_t)(xTaskGetTickCount() - deadline) < 0);
    return read_value() >= target;
}

static bool p8_ble_only_setup(void)
{
    if (phase8_protocol_tests_cleanup() != ESP_OK) return false;
    memset(s_p8_ble_last_frame, 0, sizeof(s_p8_ble_last_frame));
    atomic_store_explicit(&s_p8_ble_frame_count, 0U, memory_order_release);
    ble_transport_config_t config = {
        .device_name = BLE_TRANSPORT_DEVICE_NAME_DEFAULT,
        .frame_sink = p8_ble_frame_sink,
    };
    return xiaojing_ble_transport_init(&config) == ESP_OK &&
           xiaojing_ble_transport_start() == ESP_OK;
}

static uint32_t p8_wait_heap_quiescent(uint32_t timeout_ms)
{
    uint32_t last = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
    uint32_t stable_samples = 0U;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    do {
        vTaskDelay(pdMS_TO_TICKS(10));
        uint32_t current =
            (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
        if (current == last) {
            if (++stable_samples >= 5U) return current;
        } else {
            last = current;
            stable_samples = 0U;
        }
    } while ((int32_t)(xTaskGetTickCount() - deadline) < 0);
    return last;
}
#endif

static esp_err_t p8_critical_tx(const char *json, size_t length,
                                uint32_t timeout_ms, void *context)
{
    (void)timeout_ms;
    p8_tx_capture_t *tx = context;
    if (length >= sizeof(tx->last_critical)) return ESP_ERR_INVALID_SIZE;
    memcpy(tx->last_critical, json, length);
    tx->last_critical[length] = '\0';
    tx->critical_count++;
    return ESP_OK;
}

static esp_err_t p8_status_tx(const char *json, size_t length,
                              uint32_t timeout_ms, void *context)
{
    (void)timeout_ms;
    p8_tx_capture_t *tx = context;
    if (length >= sizeof(tx->last_status)) return ESP_ERR_INVALID_SIZE;
    memcpy(tx->last_status, json, length);
    tx->last_status[length] = '\0';
    tx->status_count++;
    return ESP_OK;
}

static esp_err_t p8_provision_wifi(
    const char *ssid, const char *password, void *context)
{
    (void)context;
    if (!ssid || !password || ssid[0] == '\0' ||
        strlen(ssid) >= sizeof(s_p8_wifi_ssid) ||
        strlen(password) >= sizeof(s_p8_wifi_password))
        return ESP_ERR_INVALID_ARG;
    snprintf(s_p8_wifi_ssid, sizeof(s_p8_wifi_ssid), "%s", ssid);
    snprintf(s_p8_wifi_password, sizeof(s_p8_wifi_password), "%s", password);
    s_p8_wifi_provision_count++;
    return ESP_OK;
}

esp_err_t phase8_protocol_tests_cleanup(void)
{
    esp_err_t first = ESP_OK;
#ifdef CONFIG_BLE_TRANSPORT_TEST_HOOKS
    esp_err_t ble_err = ble_transport_test_reset();
    if (ble_err != ESP_OK) first = ble_err;
#endif
    esp_err_t protocol_err = app_protocol_stop();
    if (first == ESP_OK && protocol_err != ESP_OK &&
        protocol_err != ESP_ERR_INVALID_STATE) first = protocol_err;
    if (s_p8_exec) {
        esp_err_t err = wash_executor_stop(s_p8_exec, 10000);
        if (first == ESP_OK && err != ESP_OK) first = err;
        if (err == ESP_OK) {
            err = wash_executor_destroy(s_p8_exec);
            if (first == ESP_OK && err != ESP_OK) first = err;
            if (err == ESP_OK) s_p8_exec = NULL;
        }
    }
    return first;
}

static bool p8_runtime_setup(bool legacy_enabled)
{
    if (phase8_protocol_tests_cleanup() != ESP_OK) return false;
    memset(&s_p8_tx, 0, sizeof(s_p8_tx));
    memset(s_p8_wifi_ssid, 0, sizeof(s_p8_wifi_ssid));
    memset(s_p8_wifi_password, 0, sizeof(s_p8_wifi_password));
    s_p8_wifi_provision_count = 0;
    machine_config_restore_defaults();
    if (machine_status_store_init() != ESP_OK ||
        machine_status_store_reset() != ESP_OK) return false;
    fake_adapter_init(&s_p8_adapter, s_p8_adapter_ctx);
    wash_executor_config_t exec_cfg = {
        .step_timeout_default_ms = 5000,
        .reset_timeout_ms = 1000,
        .cancel_timeout_ms = 500,
        .terminal_retry_ms = 50,
    };
    s_p8_exec = wash_executor_create(&exec_cfg, &s_p8_adapter);
    if (!s_p8_exec || wash_executor_start(s_p8_exec) != ESP_OK) return false;
    app_protocol_config_t cfg = {
        .executor = s_p8_exec,
        .machine_config = machine_config_get(),
        .legacy_enabled = legacy_enabled,
        .critical_tx = p8_critical_tx,
        .status_tx = p8_status_tx,
        .provision_wifi = p8_provision_wifi,
        .tx_context = &s_p8_tx,
    };
    return app_protocol_init(&cfg) == ESP_OK && app_protocol_start() == ESP_OK;
}

#ifdef CONFIG_BLE_TRANSPORT_TEST_HOOKS
static bool p8_protocol_ble_start(void)
{
    ble_transport_config_t config = {
        .device_name = BLE_TRANSPORT_DEVICE_NAME_DEFAULT,
        .frame_sink = p8_ble_protocol_sink,
        .connection_sink = p8_ble_connection_sink,
    };
    return xiaojing_ble_transport_init(&config) == ESP_OK &&
           xiaojing_ble_transport_start() == ESP_OK;
}
#endif

TEST_CASE("BLE-P8-01 single and fragmented frames", "[ble][phase8]")
{
    ble_frame_assembler_t a;
    const char *out = NULL;
    size_t out_len = 0;
    const char *json = "{\"v\":1,\"seq\":1,\"cmd\":\"hello\"}";
    ble_frame_assembler_init(&a);
    TEST_ASSERT_EQUAL(BLE_FRAME_NEED_MORE,
        ble_frame_assembler_feed(&a, (const uint8_t *)json, 20, 10, &out, &out_len));
    TEST_ASSERT_EQUAL(BLE_FRAME_COMPLETE,
        ble_frame_assembler_feed(&a, (const uint8_t *)json + 20,
            strlen(json) - 20, 11, &out, &out_len));
    TEST_ASSERT_EQUAL(strlen(json), out_len);
}

TEST_CASE("BLE-P8-02 braces and escapes inside string", "[ble][phase8]")
{
    ble_frame_assembler_t a;
    const char *out = NULL; size_t out_len = 0;
    const char *json = "{\"payload\":\"{x}\\\"y\\\\z\"}";
    ble_frame_assembler_init(&a);
    TEST_ASSERT_EQUAL(BLE_FRAME_COMPLETE,
        ble_frame_assembler_feed(&a, (const uint8_t *)json, strlen(json),
            1, &out, &out_len));
}

TEST_CASE("BLE-P8-03 timeout 3000 boundary", "[ble][phase8]")
{
    ble_frame_assembler_t a; const char *out; size_t out_len;
    ble_frame_assembler_init(&a);
    TEST_ASSERT_EQUAL(BLE_FRAME_NEED_MORE,
        ble_frame_assembler_feed(&a, (const uint8_t *)"{\"v\":", 5, 100,
            &out, &out_len));
    TEST_ASSERT_EQUAL(BLE_FRAME_COMPLETE,
        ble_frame_assembler_feed(&a, (const uint8_t *)"1}", 2, 3100,
            &out, &out_len));
}

TEST_CASE("BLE-P8-04 timeout 3001 rejects", "[ble][phase8]")
{
    ble_frame_assembler_t a; const char *out; size_t out_len;
    ble_frame_assembler_init(&a);
    ble_frame_assembler_feed(&a, (const uint8_t *)"{\"v\":", 5, 100,
        &out, &out_len);
    TEST_ASSERT_EQUAL(BLE_FRAME_ERR_TIMEOUT,
        ble_frame_assembler_feed(&a, (const uint8_t *)"1}", 2, 3101,
            &out, &out_len));
}

TEST_CASE("BLE-P8-05 2049 overflow resets", "[ble][phase8]")
{
    static uint8_t huge[BLE_FRAME_MAX_BYTES + 1];
    memset(huge, ' ', sizeof(huge)); huge[0] = '{';
    ble_frame_assembler_t a; const char *out; size_t out_len;
    ble_frame_assembler_init(&a);
    TEST_ASSERT_EQUAL(BLE_FRAME_ERR_TOO_LARGE,
        ble_frame_assembler_feed(&a, huge, sizeof(huge), 1, &out, &out_len));
    TEST_ASSERT_FALSE(a.active);
    TEST_ASSERT_EQUAL_UINT32(0, a.length);
}

TEST_CASE("BLE-P8-06 all V1 commands parse", "[ble][phase8]")
{
    const char *names[] = {"hello","get_status","start_formal","start_demo",
        "submit_plan","ack_load","ack_unload","skip_uv","abort_reset",
        "ack_fault","provision_wifi","pause","resume"};
    char json[128];
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        snprintf(json, sizeof(json),
            "{\"v\":1,\"seq\":%u,\"type\":\"cmd\",\"cmd\":\"%s\"}",
            (unsigned)i + 1, names[i]);
        app_protocol_message_t msg;
        TEST_ASSERT_EQUAL(APP_PARSE_OK,
            app_protocol_parse_frame(json, strlen(json), &msg));
        TEST_ASSERT_NOT_EQUAL(APP_CMD_INVALID, msg.command);
    }
}

TEST_CASE("BLE-P8-07 version missing and malformed", "[ble][phase8]")
{
    app_protocol_message_t msg;
    const char *bad_v = "{\"v\":2,\"seq\":1,\"cmd\":\"hello\"}";
    const char *missing = "{\"v\":1,\"cmd\":\"hello\"}";
    const char *broken = "{\"v\":1";
    const char *duplicate =
        "{\"v\":1,\"seq\":1,\"seq\":2,\"cmd\":\"hello\"}";
    TEST_ASSERT_EQUAL(APP_PARSE_BAD_VERSION,
        app_protocol_parse_frame(bad_v, strlen(bad_v), &msg));
    TEST_ASSERT_EQUAL(APP_PARSE_MISSING_FIELD,
        app_protocol_parse_frame(missing, strlen(missing), &msg));
    TEST_ASSERT_EQUAL(APP_PARSE_BAD_JSON,
        app_protocol_parse_frame(broken, strlen(broken), &msg));
    TEST_ASSERT_EQUAL(APP_PARSE_BAD_JSON,
        app_protocol_parse_frame(duplicate, strlen(duplicate), &msg));
}

TEST_CASE("BLE-P8-08 raw hardware commands forbidden", "[ble][phase8]")
{
    app_protocol_message_t msg;
    const char *json = "{\"v\":1,\"seq\":1,\"cmd\":\"valve_on\"}";
    TEST_ASSERT_EQUAL(APP_PARSE_FORBIDDEN_CMD,
        app_protocol_parse_frame(json, strlen(json), &msg));
}

TEST_CASE("BLE-P8-09 legacy start stop", "[ble][phase8]")
{
    app_protocol_message_t msg;
    TEST_ASSERT_EQUAL(APP_PARSE_OK, app_protocol_parse_frame("start", 5, &msg));
    TEST_ASSERT_EQUAL(APP_CMD_LEGACY_START, msg.command);
    const char *stop = "{\"cmd\":\"stop\"}";
    TEST_ASSERT_EQUAL(APP_PARSE_OK,
        app_protocol_parse_frame(stop, strlen(stop), &msg));
    TEST_ASSERT_EQUAL(APP_CMD_LEGACY_STOP, msg.command);
}

TEST_CASE("BLE-P8-10 seq duplicate conflict eviction", "[ble][phase8]")
{
    app_seq_cache_t cache; app_seq_cache_init(&cache);
    app_protocol_message_t first = {
        .version = 1, .seq = 1, .command = APP_CMD_START_DEMO, .fingerprint = 11
    };
    TEST_ASSERT_TRUE(app_seq_cache_store(&cache, &first, "ACK", 3));
    const char *reply = NULL; size_t reply_len = 0;
    TEST_ASSERT_EQUAL(APP_SEQ_DUPLICATE,
        app_seq_cache_lookup(&cache, &first, &reply, &reply_len));
    TEST_ASSERT_EQUAL_UINT32(3, reply_len);
    app_protocol_message_t conflict = first; conflict.fingerprint = 12;
    TEST_ASSERT_EQUAL(APP_SEQ_CONFLICT,
        app_seq_cache_lookup(&cache, &conflict, NULL, NULL));
    for (uint32_t i = 2; i <= APP_PROTOCOL_SEQ_CACHE_SIZE + 1; i++) {
        app_protocol_message_t m = {
            .version = 1, .seq = i, .command = APP_CMD_GET_STATUS, .fingerprint = i
        };
        TEST_ASSERT_TRUE(app_seq_cache_store(&cache, &m, "A", 1));
    }
    TEST_ASSERT_EQUAL(APP_SEQ_MISS,
        app_seq_cache_lookup(&cache, &first, NULL, NULL));
}

TEST_CASE("BLE-P8-11 runtime hello capability and status", "[ble][phase8]")
{
    TEST_ASSERT_TRUE(p8_runtime_setup(false));
    app_protocol_set_connected(true);
    const char *frame = "{\"v\":1,\"seq\":10,\"cmd\":\"hello\"}";
    TEST_ASSERT_EQUAL(ESP_OK, app_protocol_process_frame(frame, strlen(frame)));
    TEST_ASSERT_EQUAL_UINT32(2, s_p8_tx.critical_count);
    TEST_ASSERT_EQUAL_UINT32(1, s_p8_tx.status_count);
    TEST_ASSERT_NOT_NULL(strstr(s_p8_tx.last_status, "\"ble_connected\":true"));
}

TEST_CASE("BLE-P8-12 duplicate replay and conflict execute nothing", "[ble][phase8]")
{
    TEST_ASSERT_TRUE(p8_runtime_setup(false));
    const char *hello = "{\"v\":1,\"seq\":11,\"cmd\":\"hello\"}";
    const char *conflict = "{\"v\":1,\"seq\":11,\"cmd\":\"get_status\"}";
    TEST_ASSERT_EQUAL(ESP_OK, app_protocol_process_frame(hello, strlen(hello)));
    TEST_ASSERT_EQUAL(ESP_OK, app_protocol_process_frame(hello, strlen(hello)));
    TEST_ASSERT_EQUAL(ESP_OK, app_protocol_process_frame(conflict, strlen(conflict)));
    app_protocol_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, app_protocol_get_snapshot(&snap));
    TEST_ASSERT_EQUAL_UINT32(1, snap.duplicates_replayed);
    TEST_ASSERT_EQUAL_UINT32(1, snap.sequence_conflicts);
    TEST_ASSERT_NOT_NULL(strstr(s_p8_tx.last_critical, "SEQ_CONFLICT"));
}

TEST_CASE("BLE-P8-13 unsupported and forbidden commands NACK", "[ble][phase8]")
{
    TEST_ASSERT_TRUE(p8_runtime_setup(false));
    const char *pause = "{\"v\":1,\"seq\":12,\"cmd\":\"pause\"}";
    TEST_ASSERT_EQUAL(ESP_OK, app_protocol_process_frame(pause, strlen(pause)));
    TEST_ASSERT_NOT_NULL(strstr(s_p8_tx.last_critical, "NOT_SUPPORTED"));
    const char *raw = "{\"v\":1,\"seq\":13,\"cmd\":\"motor_on\"}";
    TEST_ASSERT_EQUAL(ESP_OK, app_protocol_process_frame(raw, strlen(raw)));
    TEST_ASSERT_NOT_NULL(strstr(s_p8_tx.last_critical, "FORBIDDEN_HARDWARE"));
}

TEST_CASE("BLE-P8-13A WiFi provisioning ACK is deduplicated", "[ble][phase8][phase9]")
{
    TEST_ASSERT_TRUE(p8_runtime_setup(false));
    const char *frame =
        "{\"v\":1,\"seq\":1301,\"cmd\":\"provision_wifi\","
        "\"ssid\":\"XiaoJingLab\",\"password\":\"secret123\"}";
    TEST_ASSERT_EQUAL(
        ESP_OK, app_protocol_process_frame(frame, strlen(frame)));
    TEST_ASSERT_NOT_NULL(
        strstr(s_p8_tx.last_critical, "\"code\":\"WIFI_CONNECTING\""));
    TEST_ASSERT_EQUAL_UINT32(1, s_p8_wifi_provision_count);
    TEST_ASSERT_EQUAL_STRING("XiaoJingLab", s_p8_wifi_ssid);
    TEST_ASSERT_EQUAL_STRING("secret123", s_p8_wifi_password);

    TEST_ASSERT_EQUAL(
        ESP_OK, app_protocol_process_frame(frame, strlen(frame)));
    TEST_ASSERT_EQUAL_UINT32(1, s_p8_wifi_provision_count);
}

TEST_CASE("BLE-P8-13B invalid WiFi payload fails closed", "[ble][phase8][phase9]")
{
    TEST_ASSERT_TRUE(p8_runtime_setup(false));
    const char *missing_ssid =
        "{\"v\":1,\"seq\":1302,\"cmd\":\"provision_wifi\","
        "\"password\":\"secret123\"}";
    TEST_ASSERT_EQUAL(ESP_OK, app_protocol_process_frame(
        missing_ssid, strlen(missing_ssid)));
    TEST_ASSERT_NOT_NULL(
        strstr(s_p8_tx.last_critical, "\"code\":\"BAD_WIFI_PAYLOAD\""));
    TEST_ASSERT_EQUAL_UINT32(0, s_p8_wifi_provision_count);
}

TEST_CASE("BLE-P8-14 custom payload and planner rejection", "[ble][phase8]")
{
    TEST_ASSERT_TRUE(p8_runtime_setup(false));
    const char *missing = "{\"v\":1,\"seq\":14,\"cmd\":\"submit_plan\",\"actions\":[]}";
    TEST_ASSERT_EQUAL(ESP_OK, app_protocol_process_frame(missing, strlen(missing)));
    TEST_ASSERT_NOT_NULL(strstr(s_p8_tx.last_critical, "ACTION_COUNT"));
    const char *rejected = "{\"v\":1,\"seq\":15,\"cmd\":\"submit_plan\","
        "\"actions\":[{\"type\":\"pulsator_wash\",\"duration_ms\":100,\"rounds\":0}]}";
    TEST_ASSERT_EQUAL(ESP_OK, app_protocol_process_frame(rejected, strlen(rejected)));
    TEST_ASSERT_NOT_NULL(strstr(s_p8_tx.last_critical, "PLAN_REJECTED"));
}

TEST_CASE("BLE-P8-15 start demo duplicate dispatches once", "[ble][phase8]")
{
    TEST_ASSERT_TRUE(p8_runtime_setup(false));
    const char *frame = "{\"v\":1,\"seq\":16,\"cmd\":\"start_demo\"}";
    TEST_ASSERT_EQUAL(ESP_OK, app_protocol_process_frame(frame, strlen(frame)));
    TEST_ASSERT_TRUE(fake_adapter_wait_dispatch(s_p8_adapter_ctx, 1, 3000));
    TEST_ASSERT_EQUAL(ESP_OK, app_protocol_process_frame(frame, strlen(frame)));
    vTaskDelay(pdMS_TO_TICKS(50));
    TEST_ASSERT_EQUAL_UINT32(1, fake_adapter_dispatch_count(s_p8_adapter_ctx));
    app_protocol_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, app_protocol_get_snapshot(&snap));
    TEST_ASSERT_EQUAL_UINT32(1, snap.duplicates_replayed);
}

TEST_CASE("BLE-P8-16 legacy gate enforced", "[ble][phase8]")
{
    TEST_ASSERT_TRUE(p8_runtime_setup(false));
    TEST_ASSERT_EQUAL(ESP_OK, app_protocol_process_frame("start", 5));
    TEST_ASSERT_NOT_NULL(strstr(s_p8_tx.last_critical, "LEGACY_DISABLED"));
}

#ifdef CONFIG_BLE_TRANSPORT_TEST_HOOKS

TEST_CASE("BLE-P8-17 RX worker fragmented frame reaches executor", "[ble][phase8][runtime]")
{
    TEST_ASSERT_TRUE(p8_runtime_setup(false));
    TEST_ASSERT_TRUE(p8_protocol_ble_start());
    ble_transport_test_set_link(true, true);

    const char *frame = "{\"v\":1,\"seq\":117,\"cmd\":\"start_demo\"}";
    size_t split = 17U;
    TEST_ASSERT_EQUAL(ESP_OK, ble_transport_test_inject_rx(
        (const uint8_t *)frame, split));
    TEST_ASSERT_EQUAL(ESP_OK, ble_transport_test_inject_rx(
        (const uint8_t *)frame + split, strlen(frame) - split));
    TEST_ASSERT_TRUE(fake_adapter_wait_dispatch(s_p8_adapter_ctx, 1, 3000));

    ble_transport_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, xiaojing_ble_transport_get_snapshot(&snap));
    TEST_ASSERT_EQUAL_UINT32(2, snap.rx_chunks);
    TEST_ASSERT_EQUAL_UINT32(1, snap.rx_frames);
    TEST_ASSERT_GREATER_THAN_UINT32(0, snap.rx_stack_high_water_mark);
    TEST_ASSERT_GREATER_THAN_UINT32(0, snap.tx_stack_high_water_mark);
}

TEST_CASE("BLE-P8-18 critical queue saturation is bounded and counted", "[ble][phase8][runtime]")
{
    TEST_ASSERT_TRUE(p8_ble_only_setup());
    for (uint32_t i = 0; i < 8U; ++i) {
        char payload[16];
        int length = snprintf(payload, sizeof(payload), "critical-%u",
                              (unsigned)i);
        TEST_ASSERT_GREATER_THAN(0, length);
        TEST_ASSERT_EQUAL(ESP_OK, xiaojing_ble_transport_send_critical(
            payload, (size_t)length, 0));
    }
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT,
        xiaojing_ble_transport_send_critical("overflow", 8, 0));
    ble_transport_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, xiaojing_ble_transport_get_snapshot(&snap));
    TEST_ASSERT_EQUAL_UINT32(1, snap.critical_queue_drops);

    ble_transport_test_set_link(true, true);
    TEST_ASSERT_TRUE(p8_wait_value(ble_transport_test_get_delivery_count,
                                   8U, 3000));
}

TEST_CASE("BLE-P8-19 notify failure retains critical FIFO until success", "[ble][phase8][runtime]")
{
    TEST_ASSERT_TRUE(p8_ble_only_setup());
    ble_transport_test_set_link(true, true);
    ble_transport_test_set_notify_failures(3);
    TEST_ASSERT_EQUAL(ESP_OK,
        xiaojing_ble_transport_send_critical("A", 1, 0));
    TEST_ASSERT_EQUAL(ESP_OK,
        xiaojing_ble_transport_send_critical("B", 1, 0));
    TEST_ASSERT_TRUE(p8_wait_value(ble_transport_test_get_delivery_count,
                                   2U, 3000));
    TEST_ASSERT_EQUAL_UINT32(5, ble_transport_test_get_notify_attempts());
    char first[8] = {0};
    char second[8] = {0};
    TEST_ASSERT_EQUAL(ESP_OK,
        ble_transport_test_get_delivery(0, first, sizeof(first)));
    TEST_ASSERT_EQUAL(ESP_OK,
        ble_transport_test_get_delivery(1, second, sizeof(second)));
    TEST_ASSERT_EQUAL_STRING("A", first);
    TEST_ASSERT_EQUAL_STRING("B", second);
    ble_transport_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, xiaojing_ble_transport_get_snapshot(&snap));
    TEST_ASSERT_EQUAL_UINT32(3, snap.tx_notify_errors);
    TEST_ASSERT_EQUAL_UINT32(2, snap.tx_messages);
}

TEST_CASE("BLE-P8-20 critical overtakes pending status mailbox", "[ble][phase8][runtime]")
{
    TEST_ASSERT_EQUAL(ESP_OK, phase8_protocol_tests_cleanup());
    memset(s_p8_ble_last_frame, 0, sizeof(s_p8_ble_last_frame));
    atomic_store_explicit(&s_p8_ble_frame_count, 0U, memory_order_release);
    ble_transport_config_t config = {
        .device_name = BLE_TRANSPORT_DEVICE_NAME_DEFAULT,
        .frame_sink = p8_ble_frame_sink,
    };
    TEST_ASSERT_EQUAL(ESP_OK, xiaojing_ble_transport_init(&config));
    TEST_ASSERT_EQUAL(ESP_OK, xiaojing_ble_transport_start());
    TEST_ASSERT_EQUAL(ESP_OK,
        xiaojing_ble_transport_publish_status("STATUS", 6));
    TEST_ASSERT_EQUAL(ESP_OK,
        xiaojing_ble_transport_send_critical("CRITICAL", 8, 0));
    /* While disconnected the TX task cannot dequeue either message.  Making
     * the link ready after both are queued gives a deterministic priority
     * observation without pausing a high-priority worker during start(). */
    ble_transport_test_set_link(true, true);
    TEST_ASSERT_TRUE(p8_wait_value(ble_transport_test_get_delivery_count,
                                   2U, 3000));
    char first[16] = {0};
    char second[16] = {0};
    TEST_ASSERT_EQUAL(ESP_OK,
        ble_transport_test_get_delivery(0, first, sizeof(first)));
    TEST_ASSERT_EQUAL(ESP_OK,
        ble_transport_test_get_delivery(1, second, sizeof(second)));
    TEST_ASSERT_EQUAL_STRING("CRITICAL", first);
    TEST_ASSERT_EQUAL_STRING("STATUS", second);
}

TEST_CASE("BLE-P8-21 start sync timeout rolls back all resources", "[ble][phase8][runtime]")
{
    TEST_ASSERT_EQUAL(ESP_OK, phase8_protocol_tests_cleanup());
    ble_transport_test_set_start_sync_suppressed(true);
    ble_transport_config_t config = {
        .device_name = BLE_TRANSPORT_DEVICE_NAME_DEFAULT,
        .frame_sink = p8_ble_frame_sink,
    };
    TEST_ASSERT_EQUAL(ESP_OK, xiaojing_ble_transport_init(&config));
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, xiaojing_ble_transport_start());
    ble_transport_test_set_start_sync_suppressed(false);
    TEST_ASSERT_EQUAL(ESP_OK, xiaojing_ble_transport_init(&config));
    TEST_ASSERT_EQUAL(ESP_OK, xiaojing_ble_transport_start());
    TEST_ASSERT_EQUAL(ESP_OK, xiaojing_ble_transport_stop());
}

TEST_CASE("BLE-P8-22 host stop failure preserves resources for retry", "[ble][phase8][runtime]")
{
    TEST_ASSERT_TRUE(p8_ble_only_setup());
    ble_transport_test_set_stop_failures(1);
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, xiaojing_ble_transport_stop());
    ble_transport_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, xiaojing_ble_transport_get_snapshot(&snap));
    TEST_ASSERT_TRUE(snap.initialized);
    TEST_ASSERT_FALSE(snap.running);
    TEST_ASSERT_EQUAL(ESP_FAIL, snap.last_error);
    TEST_ASSERT_EQUAL(ESP_OK, xiaojing_ble_transport_stop());
}

TEST_CASE("BLE-P8-23 worker exit timeout preserves resources for retry", "[ble][phase8][runtime]")
{
    TEST_ASSERT_TRUE(p8_ble_only_setup());
    ble_transport_test_set_exit_barrier(true);
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, xiaojing_ble_transport_stop());
    ble_transport_snapshot_t snap;
    TEST_ASSERT_EQUAL(ESP_OK, xiaojing_ble_transport_get_snapshot(&snap));
    TEST_ASSERT_TRUE(snap.initialized);
    TEST_ASSERT_FALSE(snap.running);
    ble_transport_test_set_exit_barrier(false);
    TEST_ASSERT_EQUAL(ESP_OK, xiaojing_ble_transport_stop());
}

TEST_CASE("BLE-P8-24 disconnect does not cancel active program", "[ble][phase8][runtime]")
{
    TEST_ASSERT_TRUE(p8_runtime_setup(false));
    TEST_ASSERT_TRUE(p8_protocol_ble_start());
    ble_transport_test_set_link(true, true);
    const char *frame = "{\"v\":1,\"seq\":124,\"cmd\":\"start_demo\"}";
    TEST_ASSERT_EQUAL(ESP_OK, ble_transport_test_inject_rx(
        (const uint8_t *)frame, strlen(frame)));
    TEST_ASSERT_TRUE(fake_adapter_wait_dispatch(s_p8_adapter_ctx, 1, 3000));
    wash_exec_snapshot_t before;
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_get_snapshot(s_p8_exec, &before));
    TEST_ASSERT_TRUE(before.program_active);

    ble_transport_test_set_link(false, false);
    vTaskDelay(pdMS_TO_TICKS(50));
    wash_exec_snapshot_t after;
    TEST_ASSERT_EQUAL(ESP_OK, wash_executor_get_snapshot(s_p8_exec, &after));
    TEST_ASSERT_TRUE(after.program_active);
    TEST_ASSERT_EQUAL_UINT32(before.program_id, after.program_id);
    TEST_ASSERT_EQUAL_UINT32(before.current_request_id,
                             after.current_request_id);
    machine_status_t status;
    TEST_ASSERT_EQUAL(ESP_OK, machine_status_store_get(&status));
    TEST_ASSERT_FALSE(status.ble_connected);
}

TEST_CASE("BLE-P8-25 lifecycle heap stable and worker stacks measured", "[ble][phase8][runtime]")
{
    TEST_ASSERT_TRUE(p8_ble_only_setup());
    ble_transport_snapshot_t warm;
    TEST_ASSERT_EQUAL(ESP_OK, xiaojing_ble_transport_get_snapshot(&warm));
    TEST_ASSERT_GREATER_THAN_UINT32(0, warm.rx_stack_high_water_mark);
    TEST_ASSERT_GREATER_THAN_UINT32(0, warm.tx_stack_high_water_mark);
    TEST_ASSERT_EQUAL(ESP_OK, ble_transport_test_reset());
    uint32_t baseline = p8_wait_heap_quiescent(1000);

    for (uint32_t i = 0; i < 20U; ++i) {
        TEST_ASSERT_TRUE(p8_ble_only_setup());
        ble_transport_test_set_link(true, true);
        TEST_ASSERT_EQUAL(ESP_OK,
            xiaojing_ble_transport_send_critical("cycle", 5, 0));
        TEST_ASSERT_TRUE(p8_wait_value(ble_transport_test_get_delivery_count,
                                       1U, 1000));
        ble_transport_snapshot_t snap;
        TEST_ASSERT_EQUAL(ESP_OK, xiaojing_ble_transport_get_snapshot(&snap));
        TEST_ASSERT_GREATER_THAN_UINT32(0, snap.rx_stack_high_water_mark);
        TEST_ASSERT_GREATER_THAN_UINT32(0, snap.tx_stack_high_water_mark);
        TEST_ASSERT_EQUAL(ESP_OK, ble_transport_test_reset());
    }

    uint32_t after = p8_wait_heap_quiescent(2000);
    uint32_t delta = baseline > after ? baseline - after : after - baseline;
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(512U, delta);
}

#endif
