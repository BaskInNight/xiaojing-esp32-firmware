#include "wifi_station.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "WIFI_STA";

static const char *wifi_disconnect_reason_name(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_UNSPECIFIED: return "UNSPECIFIED";
    case WIFI_REASON_AUTH_EXPIRE: return "AUTH_EXPIRE";
    case WIFI_REASON_AUTH_LEAVE: return "AUTH_LEAVE";
    case WIFI_REASON_ASSOC_EXPIRE: return "ASSOC_EXPIRE";
    case WIFI_REASON_ASSOC_TOOMANY: return "ASSOC_TOOMANY";
    case WIFI_REASON_NOT_AUTHED: return "NOT_AUTHED";
    case WIFI_REASON_NOT_ASSOCED: return "NOT_ASSOCED";
    case WIFI_REASON_ASSOC_LEAVE: return "ASSOC_LEAVE";
    case WIFI_REASON_ASSOC_NOT_AUTHED: return "ASSOC_NOT_AUTHED";
    case WIFI_REASON_DISASSOC_PWRCAP_BAD: return "DISASSOC_PWRCAP_BAD";
    case WIFI_REASON_DISASSOC_SUPCHAN_BAD: return "DISASSOC_SUPCHAN_BAD";
    case WIFI_REASON_IE_INVALID: return "IE_INVALID";
    case WIFI_REASON_MIC_FAILURE: return "MIC_FAILURE";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4WAY_HANDSHAKE_TIMEOUT";
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT: return "GROUP_KEY_UPDATE_TIMEOUT";
    case WIFI_REASON_IE_IN_4WAY_DIFFERS: return "IE_IN_4WAY_DIFFERS";
    case WIFI_REASON_GROUP_CIPHER_INVALID: return "GROUP_CIPHER_INVALID";
    case WIFI_REASON_PAIRWISE_CIPHER_INVALID: return "PAIRWISE_CIPHER_INVALID";
    case WIFI_REASON_AKMP_INVALID: return "AKMP_INVALID";
    case WIFI_REASON_UNSUPP_RSN_IE_VERSION: return "UNSUPP_RSN_IE_VERSION";
    case WIFI_REASON_INVALID_RSN_IE_CAP: return "INVALID_RSN_IE_CAP";
    case WIFI_REASON_802_1X_AUTH_FAILED: return "802_1X_AUTH_FAILED";
    case WIFI_REASON_CIPHER_SUITE_REJECTED: return "CIPHER_SUITE_REJECTED";
    case WIFI_REASON_BEACON_TIMEOUT: return "BEACON_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND: return "NO_AP_FOUND";
    case WIFI_REASON_AUTH_FAIL: return "AUTH_FAIL";
    case WIFI_REASON_ASSOC_FAIL: return "ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_CONNECTION_FAIL: return "CONNECTION_FAIL";
    case WIFI_REASON_AP_TSF_RESET: return "AP_TSF_RESET";
    case WIFI_REASON_ROAMING: return "ROAMING";
    default: return "UNKNOWN";
    }
}

#define WIFI_LOCK_TIMEOUT_MS 100U
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT BIT1
#define WIFI_STOP_BIT BIT2
#define WIFI_EVENT_BITS \
    (WIFI_CONNECTED_BIT | WIFI_FAILED_BIT | WIFI_STOP_BIT)
#define WIFI_START_JOIN_TIMEOUT_MS 5000U
#define WIFI_ASYNC_TASK_STACK 4096U
#define WIFI_ASYNC_TASK_PRIORITY 5U

typedef struct {
    wifi_station_config_t config;
    wifi_station_snapshot_t snapshot;
    esp_netif_t *netif;
    esp_event_handler_instance_t wifi_handler;
    esp_event_handler_instance_t ip_handler;
    bool wifi_initialized;
    bool wifi_started;
    bool handlers_registered;
    bool start_active;
    bool async_task_active;
    TaskHandle_t async_task;
    esp_err_t async_result;
    esp_err_t start_cleanup_error;
    uint32_t retry_count;       /* Current retry sequence count */
    uint32_t retry_backoff_ms;  /* Current backoff delay */
} wifi_station_runtime_t;

static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
static StaticEventGroup_t s_events_storage;
static EventGroupHandle_t s_events;
static StaticSemaphore_t s_start_done_storage;
static SemaphoreHandle_t s_start_done;
static StaticSemaphore_t s_async_done_storage;
static SemaphoreHandle_t s_async_done;
static wifi_station_runtime_t s_rt;
static _Atomic bool s_connected;
static _Atomic bool s_stop_requested;
static _Atomic int s_global_state;

enum {
    GLOBAL_UNINITIALIZED = 0,
    GLOBAL_INITIALIZING,
    GLOBAL_READY,
    GLOBAL_FAILED,
};

static bool take_lock(void)
{
    if (!s_lock) return false;
    TickType_t ticks = pdMS_TO_TICKS(WIFI_LOCK_TIMEOUT_MS);
    if (ticks == 0) ticks = 1;
    return xSemaphoreTake(s_lock, ticks) == pdTRUE;
}

static bool valid_namespace(const char *value)
{
    if (!value) return false;
    size_t length = strnlen(value, 16);
    if (length == 0 || length >= 16) return false;
    for (size_t i = 0; i < length; i++) {
        char c = value[i];
        if (!((c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    }
    return true;
}

static bool valid_text(const char *value, size_t capacity,
                       size_t minimum, size_t maximum)
{
    if (!value) return false;
    size_t length = strnlen(value, capacity);
    if (length < minimum || length > maximum || length >= capacity)
        return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

bool xiaojing_wifi_credentials_valid(
    const wifi_station_credentials_t *credentials)
{
    if (!credentials ||
        !valid_text(credentials->ssid, sizeof(credentials->ssid), 1, 32))
        return false;
    size_t password_length = strnlen(
        credentials->password, sizeof(credentials->password));
    if (password_length == 0) return true;
    return valid_text(
        credentials->password, sizeof(credentials->password), 8, 63);
}

esp_err_t xiaojing_wifi_save_credentials(
    const char *nvs_namespace,
    const wifi_station_credentials_t *credentials)
{
    if (!valid_namespace(nvs_namespace) ||
        !xiaojing_wifi_credentials_valid(credentials))
        return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t error = nvs_open(nvs_namespace, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    error = nvs_set_str(handle, "ssid", credentials->ssid);
    if (error == ESP_OK)
        error = nvs_set_str(handle, "password", credentials->password);
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error;
}

esp_err_t xiaojing_wifi_load_credentials(
    const char *nvs_namespace,
    wifi_station_credentials_t *out)
{
    if (!valid_namespace(nvs_namespace) || !out)
        return ESP_ERR_INVALID_ARG;
    wifi_station_credentials_t value;
    memset(&value, 0, sizeof(value));
    nvs_handle_t handle;
    esp_err_t error = nvs_open(nvs_namespace, NVS_READONLY, &handle);
    if (error != ESP_OK) return error;
    size_t ssid_size = sizeof(value.ssid);
    size_t password_size = sizeof(value.password);
    error = nvs_get_str(handle, "ssid", value.ssid, &ssid_size);
    if (error == ESP_OK)
        error = nvs_get_str(
            handle, "password", value.password, &password_size);
    nvs_close(handle);
    if (error != ESP_OK) return error;
    if (!xiaojing_wifi_credentials_valid(&value))
        return ESP_ERR_INVALID_STATE;
    *out = value;
    memset(&value, 0, sizeof(value));
    return ESP_OK;
}

esp_err_t xiaojing_wifi_erase_credentials(const char *nvs_namespace)
{
    if (!valid_namespace(nvs_namespace)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t error = nvs_open(nvs_namespace, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    error = nvs_erase_all(handle);
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error;
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;
    if (base != WIFI_EVENT) return;
    if (id == WIFI_EVENT_STA_START) {
        if (take_lock()) {
            s_rt.snapshot.connect_attempts++;
            xSemaphoreGive(s_lock);
        }
        (void)esp_wifi_connect();
        return;
    }
    if (id != WIFI_EVENT_STA_DISCONNECTED) return;

    wifi_event_sta_disconnected_t *event = data;
    uint8_t reason = event ? event->reason : 0;
    const char *reason_name = wifi_disconnect_reason_name(reason);
    /* Stage 1 diagnostic: log reason, retry state, RSSI and channel.
     * SSID/password are never logged. */
    wifi_ap_record_t ap_info;
    int8_t rssi = 0;
    uint8_t channel = 0;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
        channel = ap_info.primary;
    }
    ESP_LOGI(TAG, "DISCONNECTED reason=%u reason_name=%s "
             "RSSI=%d ch=%u",
             (unsigned)reason, reason_name, (int)rssi, (unsigned)channel);

    bool retry = false;
    uint32_t this_retry = 0;
    uint32_t backoff_ms = 0;
    wifi_station_connection_sink_t sink = NULL;
    void *sink_context = NULL;
    if (take_lock()) {
        s_rt.snapshot.disconnects++;
        s_rt.snapshot.last_disconnect_reason = (int32_t)reason;
        atomic_store_explicit(&s_connected, false, memory_order_release);
        s_rt.snapshot.connected = false;
        if (s_rt.snapshot.state == WIFI_STATION_CONNECTING) {
            s_rt.snapshot.connect_attempts++;
            s_rt.retry_count++;
            this_retry = s_rt.retry_count;
            /* Exponential backoff with a bounded retry burst.  Once the
             * configured burst is exhausted, keep the station in CONNECTING
             * and start another burst after a quiet interval.  A mains
             * powered appliance must recover from a transient AP/auth fault
             * without requiring a reboot or BLE provisioning cycle. */
            if (s_rt.retry_count > s_rt.config.max_retries) {
                s_rt.retry_count = 1;
                s_rt.retry_backoff_ms = 10000;
                this_retry = s_rt.retry_count;
            } else if (s_rt.retry_backoff_ms == 0) {
                s_rt.retry_backoff_ms = 500;
            } else {
                s_rt.retry_backoff_ms *= 2;
                if (s_rt.retry_backoff_ms > 5000)
                    s_rt.retry_backoff_ms = 5000;
            }
            backoff_ms = s_rt.retry_backoff_ms;
            retry = true;
        } else if (s_rt.snapshot.state != WIFI_STATION_STOPPING) {
            s_rt.snapshot.state = WIFI_STATION_FAILED;
            s_rt.snapshot.last_error = ESP_ERR_TIMEOUT;
        }
        sink = s_rt.config.connection_sink;
        sink_context = s_rt.config.connection_context;
        ESP_LOGI(TAG, "retry=%"PRIu32" connect_attempts=%"PRIu32
                 " max_retries=%"PRIu32" backoff=%"PRIu32"ms",
                 this_retry,
                 s_rt.snapshot.connect_attempts,
                 s_rt.config.max_retries,
                 backoff_ms);
        xSemaphoreGive(s_lock);
    }
    if (sink) sink(false, sink_context);
    if (retry) {
        if (backoff_ms > 0)
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        (void)esp_wifi_connect();
    } else {
        xEventGroupSetBits(s_events, WIFI_FAILED_BIT);
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base,
                             int32_t id, void *data)
{
    (void)arg;
    if (base != IP_EVENT || id != IP_EVENT_STA_GOT_IP) return;
    ip_event_got_ip_t *event = data;
    wifi_station_connection_sink_t sink = NULL;
    void *sink_context = NULL;
    if (take_lock()) {
        s_rt.snapshot.state = WIFI_STATION_CONNECTED;
        s_rt.snapshot.connected = true;
        s_rt.snapshot.last_error = ESP_OK;
        s_rt.snapshot.ip_address = event ? event->ip_info.ip.addr : 0;
        atomic_store_explicit(&s_connected, true, memory_order_release);
        /* Clear retry state on successful connection */
        s_rt.retry_count = 0;
        s_rt.retry_backoff_ms = 0;
        sink = s_rt.config.connection_sink;
        sink_context = s_rt.config.connection_context;
        xSemaphoreGive(s_lock);
    }
    if (sink) sink(true, sink_context);
    xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
}

esp_err_t xiaojing_wifi_global_init(void)
{
    int state = atomic_load_explicit(
        &s_global_state, memory_order_acquire);
    if (state == GLOBAL_READY) return ESP_OK;
    if (state == GLOBAL_FAILED) return ESP_ERR_NO_MEM;
    int expected = GLOBAL_UNINITIALIZED;
    if (atomic_compare_exchange_strong_explicit(
            &s_global_state, &expected, GLOBAL_INITIALIZING,
            memory_order_acq_rel, memory_order_acquire)) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
        s_events = xEventGroupCreateStatic(&s_events_storage);
        s_start_done = xSemaphoreCreateBinaryStatic(&s_start_done_storage);
        s_async_done = xSemaphoreCreateBinaryStatic(&s_async_done_storage);
        bool ready = s_lock && s_events && s_start_done && s_async_done;
        atomic_store_explicit(
            &s_global_state, ready ? GLOBAL_READY : GLOBAL_FAILED,
            memory_order_release);
        return ready ? ESP_OK : ESP_ERR_NO_MEM;
    }
    for (uint32_t i = 0; i < 1000; i++) {
        state = atomic_load_explicit(
            &s_global_state, memory_order_acquire);
        if (state == GLOBAL_READY) return ESP_OK;
        if (state == GLOBAL_FAILED) return ESP_ERR_NO_MEM;
        taskYIELD();
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t xiaojing_wifi_init(const wifi_station_config_t *config)
{
    if (!config || !valid_namespace(config->nvs_namespace) ||
        config->connect_timeout_ms < 1000 ||
        config->connect_timeout_ms > 120000 ||
        config->max_retries > 20)
        return ESP_ERR_INVALID_ARG;
    esp_err_t error = xiaojing_wifi_global_init();
    if (error != ESP_OK) return error;
    if (!take_lock()) return ESP_ERR_TIMEOUT;
    if (s_rt.snapshot.state != WIFI_STATION_UNINITIALIZED) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_rt, 0, sizeof(s_rt));
    s_rt.config = *config;
    s_rt.snapshot.state = WIFI_STATION_INITIALIZED;
    wifi_station_credentials_t credentials;
    s_rt.snapshot.provisioned =
        xiaojing_wifi_load_credentials(
            config->nvs_namespace, &credentials) == ESP_OK;
    memset(&credentials, 0, sizeof(credentials));
    atomic_store_explicit(&s_connected, false, memory_order_release);
    atomic_store_explicit(
        &s_stop_requested, false, memory_order_release);
    xEventGroupClearBits(s_events, WIFI_EVENT_BITS);
    (void)xSemaphoreTake(s_start_done, 0);
    (void)xSemaphoreTake(s_async_done, 0);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static esp_err_t cleanup_wifi(void)
{
    esp_err_t first = ESP_OK;
    if (s_rt.wifi_started) {
        esp_err_t error = esp_wifi_stop();
        if (first == ESP_OK) first = error;
        if (error == ESP_OK) s_rt.wifi_started = false;
    }
    if (s_rt.handlers_registered) {
        esp_err_t wifi_error = esp_event_handler_instance_unregister(
            WIFI_EVENT, ESP_EVENT_ANY_ID, s_rt.wifi_handler);
        if (first == ESP_OK) first = wifi_error;
        esp_err_t ip_error = esp_event_handler_instance_unregister(
            IP_EVENT, IP_EVENT_STA_GOT_IP, s_rt.ip_handler);
        if (first == ESP_OK) first = ip_error;
        if (wifi_error == ESP_OK && ip_error == ESP_OK)
            s_rt.handlers_registered = false;
    }
    if (s_rt.wifi_initialized) {
        esp_err_t error = esp_wifi_deinit();
        if (first == ESP_OK) first = error;
        if (error == ESP_OK) s_rt.wifi_initialized = false;
    }
    if (s_rt.netif && first == ESP_OK) {
        esp_netif_destroy(s_rt.netif);
        s_rt.netif = NULL;
    }
    return first;
}

esp_err_t xiaojing_wifi_start(void)
{
    if (!take_lock()) return ESP_ERR_INVALID_STATE;
    if (s_rt.snapshot.state != WIFI_STATION_INITIALIZED) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    wifi_station_config_t config = s_rt.config;
    xSemaphoreGive(s_lock);

    wifi_station_credentials_t credentials;
    esp_err_t error = xiaojing_wifi_load_credentials(
        config.nvs_namespace, &credentials);
    if (error != ESP_OK) return error;

    if (!take_lock()) {
        memset(&credentials, 0, sizeof(credentials));
        return ESP_ERR_TIMEOUT;
    }
    if (s_rt.snapshot.state != WIFI_STATION_INITIALIZED) {
        xSemaphoreGive(s_lock);
        memset(&credentials, 0, sizeof(credentials));
        return ESP_ERR_INVALID_STATE;
    }
    s_rt.snapshot.state = WIFI_STATION_CONNECTING;
    s_rt.snapshot.connect_attempts = 0;
    s_rt.snapshot.last_error = ESP_OK;
    s_rt.start_active = true;
    s_rt.start_cleanup_error = ESP_OK;
    atomic_store_explicit(
        &s_stop_requested, false, memory_order_release);
    xEventGroupClearBits(s_events, WIFI_EVENT_BITS);
    (void)xSemaphoreTake(s_start_done, 0);
    xSemaphoreGive(s_lock);

    error = esp_netif_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) goto fail;
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) goto fail;
    s_rt.netif = esp_netif_create_default_wifi_sta();
    if (!s_rt.netif) {
        error = ESP_ERR_NO_MEM;
        goto fail;
    }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    error = esp_wifi_init(&init);
    if (error != ESP_OK) goto fail;
    s_rt.wifi_initialized = true;
    error = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler,
        NULL, &s_rt.wifi_handler);
    if (error != ESP_OK) goto fail;
    error = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler,
        NULL, &s_rt.ip_handler);
    if (error != ESP_OK) {
        (void)esp_event_handler_instance_unregister(
            WIFI_EVENT, ESP_EVENT_ANY_ID, s_rt.wifi_handler);
        goto fail;
    }
    s_rt.handlers_registered = true;

    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config));
    memcpy(wifi_config.sta.ssid, credentials.ssid,
           strlen(credentials.ssid));
    memcpy(wifi_config.sta.password, credentials.password,
           strlen(credentials.password));
    wifi_config.sta.threshold.authmode =
        credentials.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    memset(&credentials, 0, sizeof(credentials));

    error = esp_wifi_set_mode(WIFI_MODE_STA);
    if (error == ESP_OK)
        error = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    memset(&wifi_config, 0, sizeof(wifi_config));
    if (error != ESP_OK) goto fail;

    error = esp_wifi_start();
    if (error != ESP_OK) goto fail;
    s_rt.wifi_started = true;
    /* This controller is mains powered. Disabling modem power save avoids
     * long TLS upload stalls while BLE remains active during voice requests. */
    error = esp_wifi_set_ps(WIFI_PS_NONE);
    if (error != ESP_OK) goto fail;
    TickType_t wait = pdMS_TO_TICKS(config.connect_timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(
        s_events, WIFI_EVENT_BITS, pdFALSE, pdFALSE, wait);
    if ((bits & WIFI_STOP_BIT) != 0 ||
        atomic_load_explicit(
            &s_stop_requested, memory_order_acquire)) {
        error = ESP_ERR_INVALID_STATE;
    } else if ((bits & WIFI_CONNECTED_BIT) != 0) {
        if (take_lock()) {
            s_rt.start_active = false;
            xSemaphoreGive(s_lock);
        }
        xSemaphoreGive(s_start_done);
        return ESP_OK;
    } else {
        error = ESP_ERR_TIMEOUT;
    }

fail:
    memset(&credentials, 0, sizeof(credentials));
    if (take_lock()) {
        s_rt.snapshot.state = WIFI_STATION_FAILED;
        s_rt.snapshot.connected = false;
        s_rt.snapshot.last_error = error;
        atomic_store_explicit(&s_connected, false, memory_order_release);
        xSemaphoreGive(s_lock);
    }
    esp_err_t cleanup_error = cleanup_wifi();
    if (error == ESP_OK) error = cleanup_error;
    if (take_lock()) {
        s_rt.start_active = false;
        s_rt.start_cleanup_error = cleanup_error;
        xSemaphoreGive(s_lock);
    }
    xSemaphoreGive(s_start_done);
    return error;
}

static void wifi_async_start_task(void *context)
{
    (void)context;
    esp_err_t result = xiaojing_wifi_start();
    if (take_lock()) {
        s_rt.async_result = result;
        s_rt.async_task_active = false;
        s_rt.async_task = NULL;
        xSemaphoreGive(s_lock);
    }
    /*
     * No shared object is accessed after this signal. The only remaining
     * operation is self-deletion, so stop() may safely continue cleanup.
     */
    xSemaphoreGive(s_async_done);
    vTaskDelete(NULL);
}

esp_err_t xiaojing_wifi_start_async(void)
{
    if (!s_lock || !s_async_done) return ESP_ERR_INVALID_STATE;

    /* Fail unprovisioned devices before allocating a task. */
    if (!take_lock()) return ESP_ERR_TIMEOUT;
    if (s_rt.snapshot.state != WIFI_STATION_INITIALIZED ||
        s_rt.async_task_active || s_rt.start_active) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    wifi_station_config_t config = s_rt.config;
    xSemaphoreGive(s_lock);

    wifi_station_credentials_t credentials;
    esp_err_t error = xiaojing_wifi_load_credentials(
        config.nvs_namespace, &credentials);
    memset(&credentials, 0, sizeof(credentials));
    if (error != ESP_OK) return error;

    if (!take_lock()) return ESP_ERR_TIMEOUT;
    if (s_rt.snapshot.state != WIFI_STATION_INITIALIZED ||
        s_rt.async_task_active || s_rt.start_active) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    (void)xSemaphoreTake(s_async_done, 0);
    s_rt.async_task_active = true;
    s_rt.async_result = ESP_ERR_INVALID_STATE;
    BaseType_t created = xTaskCreate(
        wifi_async_start_task, "xj_wifi_start",
        WIFI_ASYNC_TASK_STACK, NULL, WIFI_ASYNC_TASK_PRIORITY,
        &s_rt.async_task);
    if (created != pdPASS) {
        s_rt.async_task_active = false;
        s_rt.async_task = NULL;
        s_rt.async_result = ESP_ERR_NO_MEM;
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t xiaojing_wifi_provision_and_start(
    const wifi_station_credentials_t *credentials)
{
    if (!xiaojing_wifi_credentials_valid(credentials))
        return ESP_ERR_INVALID_ARG;
    if (!take_lock()) return ESP_ERR_INVALID_STATE;
    if (s_rt.snapshot.state != WIFI_STATION_INITIALIZED ||
        s_rt.async_task_active || s_rt.start_active) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    wifi_station_config_t config = s_rt.config;
    xSemaphoreGive(s_lock);

    esp_err_t error = xiaojing_wifi_save_credentials(
        config.nvs_namespace, credentials);
    if (error != ESP_OK) return error;

    if (!take_lock()) return ESP_ERR_TIMEOUT;
    if (s_rt.snapshot.state != WIFI_STATION_INITIALIZED ||
        s_rt.async_task_active || s_rt.start_active) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_rt.snapshot.provisioned = true;
    xSemaphoreGive(s_lock);
    return xiaojing_wifi_start_async();
}

esp_err_t xiaojing_wifi_stop(void)
{
    if (!s_lock) return ESP_OK;
    if (!take_lock()) return ESP_ERR_TIMEOUT;
    if (s_rt.snapshot.state == WIFI_STATION_UNINITIALIZED) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    bool start_active = s_rt.start_active;
    bool async_task_active = s_rt.async_task_active;
    s_rt.snapshot.state = WIFI_STATION_STOPPING;
    atomic_store_explicit(
        &s_stop_requested, true, memory_order_release);
    atomic_store_explicit(&s_connected, false, memory_order_release);
    xSemaphoreGive(s_lock);

    esp_err_t error = ESP_OK;
    if (async_task_active) {
        xEventGroupSetBits(s_events, WIFI_STOP_BIT);
        if (xSemaphoreTake(
                s_async_done,
                pdMS_TO_TICKS(WIFI_START_JOIN_TIMEOUT_MS)) != pdTRUE)
            return ESP_ERR_TIMEOUT;
        /* stop() is the join owner. Commit the joined state even if the
         * worker's best-effort bookkeeping lock timed out. */
        if (!take_lock()) return ESP_ERR_TIMEOUT;
        s_rt.async_task_active = false;
        s_rt.async_task = NULL;
        xSemaphoreGive(s_lock);
    } else if (start_active) {
        xEventGroupSetBits(s_events, WIFI_STOP_BIT);
        if (xSemaphoreTake(
                s_start_done,
                pdMS_TO_TICKS(WIFI_START_JOIN_TIMEOUT_MS)) != pdTRUE)
            return ESP_ERR_TIMEOUT;
    }
    /*
     * Retry cleanup even when the start path already attempted it. This
     * makes stop() the single owner that proves all WiFi resources are gone.
     */
    error = cleanup_wifi();
    if (error != ESP_OK) return error;
    if (!take_lock()) return ESP_ERR_TIMEOUT;
    s_rt.snapshot.state = WIFI_STATION_UNINITIALIZED;
    s_rt.snapshot.connected = false;
    s_rt.snapshot.ip_address = 0;
    xEventGroupClearBits(s_events, WIFI_EVENT_BITS);
    (void)xSemaphoreTake(s_start_done, 0);
    (void)xSemaphoreTake(s_async_done, 0);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool xiaojing_wifi_is_connected(void)
{
    return atomic_load_explicit(&s_connected, memory_order_acquire);
}

esp_err_t xiaojing_wifi_get_snapshot(wifi_station_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!take_lock()) return ESP_ERR_INVALID_STATE;
    wifi_station_snapshot_t value = s_rt.snapshot;
    value.connected = atomic_load_explicit(
        &s_connected, memory_order_acquire);
    *out = value;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t xiaojing_wifi_dump_config(void)
{
    wifi_station_credentials_t credentials;
    memset(&credentials, 0, sizeof(credentials));
    if (!take_lock()) return ESP_ERR_INVALID_STATE;
    bool provisioned = s_rt.snapshot.provisioned;
    const char *ns = s_rt.config.nvs_namespace;
    xSemaphoreGive(s_lock);

    if (!provisioned) {
        ESP_LOGI(TAG, "WIFI_CONFIG: provisioned=0 (no credentials)");
        return ESP_OK;
    }

    esp_err_t error = xiaojing_wifi_load_credentials(ns, &credentials);
    if (error != ESP_OK) {
        ESP_LOGI(TAG, "WIFI_CONFIG: load_failed error=0x%x", error);
        return error;
    }

    size_t ssid_len = strlen(credentials.ssid);
    size_t password_len = strlen(credentials.password);
    bool has_leading_trailing_space = false;
    bool has_cr_lf = false;

    if (ssid_len > 0) {
        if (credentials.ssid[0] == ' ' ||
            credentials.ssid[ssid_len - 1] == ' ')
            has_leading_trailing_space = true;
        for (size_t i = 0; i < ssid_len; i++) {
            if (credentials.ssid[i] == '\r' || credentials.ssid[i] == '\n') {
                has_cr_lf = true;
                break;
            }
        }
    }
    if (password_len > 0) {
        if (credentials.password[0] == ' ' ||
            credentials.password[password_len - 1] == ' ')
            has_leading_trailing_space = true;
        for (size_t i = 0; i < password_len; i++) {
            if (credentials.password[i] == '\r' ||
                credentials.password[i] == '\n') {
                has_cr_lf = true;
                break;
            }
        }
    }

    ESP_LOGI(TAG, "WIFI_CONFIG: ssid=\"%s\" ssid_len=%u password_len=%u "
             "leading_or_trailing_space=%u contains_cr_lf=%u config_source=NVS",
             credentials.ssid, (unsigned)ssid_len, (unsigned)password_len,
             has_leading_trailing_space ? 1 : 0, has_cr_lf ? 1 : 0);

    memset(&credentials, 0, sizeof(credentials));
    return ESP_OK;
}

esp_err_t xiaojing_wifi_scan_for_ap(const char *target_ssid,
                                    int *out_channel,
                                    int *out_rssi,
                                    int *out_authmode)
{
    if (!target_ssid) return ESP_ERR_INVALID_ARG;

    wifi_scan_config_t scan_config;
    memset(&scan_config, 0, sizeof(scan_config));
    scan_config.show_hidden = true;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_config.scan_time.active.min = 100;
    scan_config.scan_time.active.max = 300;

    ESP_LOGI(TAG, "SCAN: starting...");
    esp_err_t error = esp_wifi_scan_start(&scan_config, true);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "SCAN: failed error=0x%x", error);
        return error;
    }

    uint16_t ap_count = 0;
    error = esp_wifi_scan_get_ap_num(&ap_count);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "SCAN: get_ap_num failed error=0x%x", error);
        return error;
    }

    ESP_LOGI(TAG, "SCAN: found %u APs", (unsigned)ap_count);

    wifi_ap_record_t *ap_list = NULL;
    if (ap_count > 0) {
        ap_list = malloc(sizeof(wifi_ap_record_t) * ap_count);
        if (!ap_list) return ESP_ERR_NO_MEM;
        error = esp_wifi_scan_get_ap_records(&ap_count, ap_list);
        if (error != ESP_OK) {
            free(ap_list);
            return error;
        }
    }

    bool found = false;
    for (uint16_t i = 0; i < ap_count; i++) {
        if (strcmp((const char *)ap_list[i].ssid, target_ssid) == 0) {
            found = true;
            if (out_channel) *out_channel = ap_list[i].primary;
            if (out_rssi) *out_rssi = ap_list[i].rssi;
            if (out_authmode) *out_authmode = (int)ap_list[i].authmode;
            ESP_LOGI(TAG, "JINGJIE: found=1 channel=%d RSSI=%d authmode=%d",
                     ap_list[i].primary, ap_list[i].rssi,
                     (int)ap_list[i].authmode);
            break;
        }
    }

    if (!found) {
        ESP_LOGI(TAG, "JINGJIE: found=0");
        if (out_channel) *out_channel = 0;
        if (out_rssi) *out_rssi = 0;
        if (out_authmode) *out_authmode = 0;
    }

    free(ap_list);
    return ESP_OK;
}

#ifdef XIAOJING_TESTING
void xiaojing_wifi_test_reset(void)
{
    if (!s_lock) return;
    if (xiaojing_wifi_stop() != ESP_OK) return;
    if (!take_lock()) return;
    memset(&s_rt, 0, sizeof(s_rt));
    atomic_store_explicit(&s_connected, false, memory_order_release);
    atomic_store_explicit(
        &s_stop_requested, false, memory_order_release);
    xEventGroupClearBits(s_events, WIFI_EVENT_BITS);
    (void)xSemaphoreTake(s_start_done, 0);
    (void)xSemaphoreTake(s_async_done, 0);
    xSemaphoreGive(s_lock);
}
#endif
