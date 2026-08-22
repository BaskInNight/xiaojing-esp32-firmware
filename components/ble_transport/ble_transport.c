#include "ble_transport.h"

#include <stdatomic.h>
#include <string.h>

#include "ble_frame_assembler.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define RX_QUEUE_DEPTH 8U
#define CRITICAL_QUEUE_DEPTH 8U
#define RX_TASK_STACK_BYTES 12288U
#define TX_TASK_STACK_BYTES 4096U
#define RX_TASK_PRIORITY 12U
#define TX_TASK_PRIORITY 11U
#ifdef CONFIG_BLE_TRANSPORT_TEST_HOOKS
#define START_TIMEOUT_MS 250U
#define STOP_TIMEOUT_MS 250U
#else
#define START_TIMEOUT_MS 5000U
#define STOP_TIMEOUT_MS 5000U
#endif

#define EVT_RX_DONE   BIT0
#define EVT_TX_DONE   BIT1
#define EVT_HOST_DONE BIT2
#define EVT_HOST_SYNC BIT3
#define EVT_ALL_DONE  (EVT_RX_DONE | EVT_TX_DONE | EVT_HOST_DONE)

typedef enum {
    BLE_LC_UNINITIALIZED = 0,
    BLE_LC_INITIALIZED,
    BLE_LC_STARTING,
    BLE_LC_RUNNING,
    BLE_LC_STOPPING,
} ble_lifecycle_t;

typedef struct {
    uint16_t length;
    bool secure;
    uint8_t data[BLE_TRANSPORT_RX_CHUNK_MAX];
} rx_chunk_t;

typedef struct {
    uint16_t length;
    char data[BLE_TRANSPORT_MAX_TX_BYTES + 1U];
} tx_message_t;

typedef struct {
    ble_lifecycle_t lifecycle;
    ble_transport_config_t config;
    char device_name[32];
    QueueHandle_t rx_queue;
    QueueHandle_t critical_queue;
    QueueHandle_t status_mailbox;
    EventGroupHandle_t events;
    SemaphoreHandle_t state_mutex;
    TaskHandle_t rx_task;
    TaskHandle_t tx_task;
    bool host_started;
    ble_transport_snapshot_t counters;
    char last_tx[BLE_TRANSPORT_MAX_TX_BYTES + 1U];
    uint16_t last_tx_len;
} ble_runtime_t;

static const char *TAG = "ble_transport";
static ble_runtime_t s_rt;
static StaticSemaphore_t s_lifecycle_mutex_storage;
static SemaphoreHandle_t s_lifecycle_mutex;
static _Atomic bool s_stop_requested;
static _Atomic bool s_connected;
static _Atomic bool s_encrypted;
static _Atomic bool s_subscribed;
static _Atomic bool s_host_synchronized;
static _Atomic uint16_t s_conn_handle;
static _Atomic int s_start_error;

#ifdef CONFIG_BLE_TRANSPORT_TEST_HOOKS
static _Atomic bool s_test_start_sync_suppressed;
static _Atomic bool s_test_exit_barrier;
static _Atomic bool s_test_tx_paused;
static _Atomic uint32_t s_test_stop_failures;
static _Atomic uint32_t s_test_notify_failures;
static _Atomic uint32_t s_test_notify_attempts;
static tx_message_t s_test_deliveries[BLE_TRANSPORT_TEST_MAX_DELIVERIES];
static uint32_t s_test_delivery_count;
#endif

static TickType_t timeout_ticks(uint32_t timeout_ms)
{
    if (timeout_ms == 0U) return 0;
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    return ticks == 0 ? 1 : ticks;
}

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static bool take_mutex(SemaphoreHandle_t mutex, uint32_t timeout_ms)
{
    return mutex && xSemaphoreTake(mutex, timeout_ticks(timeout_ms)) == pdTRUE;
}

static void record_error(esp_err_t err)
{
    if (err == ESP_OK || !s_rt.state_mutex) return;
    if (xSemaphoreTake(s_rt.state_mutex, 0) == pdTRUE) {
        s_rt.counters.last_error = err;
        xSemaphoreGive(s_rt.state_mutex);
    }
}

static void record_stack_high_water(bool rx_task)
{
    uint32_t mark = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    if (!take_mutex(s_rt.state_mutex, 10)) return;
    uint32_t *saved = rx_task ? &s_rt.counters.rx_stack_high_water_mark
                              : &s_rt.counters.tx_stack_high_water_mark;
    if (*saved == 0U || mark < *saved) *saved = mark;
    xSemaphoreGive(s_rt.state_mutex);
}

bool xiaojing_ble_transport_is_supported(void)
{
#if (CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED) || defined(CONFIG_BLE_TRANSPORT_TEST_HOOKS)
    return true;
#else
    return false;
#endif
}

esp_err_t xiaojing_ble_transport_init(const ble_transport_config_t *config)
{
    if (!config || !config->frame_sink) return ESP_ERR_INVALID_ARG;
    if (!s_lifecycle_mutex) {
        s_lifecycle_mutex = xSemaphoreCreateMutexStatic(&s_lifecycle_mutex_storage);
        if (!s_lifecycle_mutex) return ESP_ERR_NO_MEM;
    }
    if (!take_mutex(s_lifecycle_mutex, 1000)) return ESP_ERR_TIMEOUT;
    if (s_rt.lifecycle != BLE_LC_UNINITIALIZED) {
        xSemaphoreGive(s_lifecycle_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_rt, 0, sizeof(s_rt));
    s_rt.config = *config;
    const char *name = config->device_name ? config->device_name
                                           : BLE_TRANSPORT_DEVICE_NAME_DEFAULT;
    size_t name_len = strnlen(name, sizeof(s_rt.device_name));
    if (name_len == 0 || name_len >= sizeof(s_rt.device_name)) {
        xSemaphoreGive(s_lifecycle_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(s_rt.device_name, name, name_len + 1U);

    /* BLE is initialized after Wi-Fi + voice. Keep the sizeable transport
     * storage in PSRAM so the controller can still obtain its DMA buffers
     * from internal RAM. */
    const UBaseType_t queue_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    s_rt.rx_queue = xQueueCreateWithCaps(RX_QUEUE_DEPTH, sizeof(rx_chunk_t),
                                         queue_caps);
    s_rt.critical_queue = xQueueCreateWithCaps(CRITICAL_QUEUE_DEPTH,
                                               sizeof(tx_message_t),
                                               queue_caps);
    s_rt.status_mailbox = xQueueCreateWithCaps(1, sizeof(tx_message_t),
                                               queue_caps);
    s_rt.events = xEventGroupCreate();
    s_rt.state_mutex = xSemaphoreCreateMutex();
    if (!s_rt.rx_queue || !s_rt.critical_queue || !s_rt.status_mailbox ||
        !s_rt.events || !s_rt.state_mutex) {
        if (s_rt.rx_queue) vQueueDelete(s_rt.rx_queue);
        if (s_rt.critical_queue) vQueueDelete(s_rt.critical_queue);
        if (s_rt.status_mailbox) vQueueDelete(s_rt.status_mailbox);
        if (s_rt.events) vEventGroupDelete(s_rt.events);
        if (s_rt.state_mutex) vSemaphoreDelete(s_rt.state_mutex);
        memset(&s_rt, 0, sizeof(s_rt));
        xSemaphoreGive(s_lifecycle_mutex);
        return ESP_ERR_NO_MEM;
    }
    s_rt.lifecycle = BLE_LC_INITIALIZED;
    s_rt.counters.initialized = true;
    atomic_store_explicit(&s_stop_requested, false, memory_order_release);
    atomic_store_explicit(&s_connected, false, memory_order_release);
    atomic_store_explicit(&s_encrypted, false, memory_order_release);
    atomic_store_explicit(&s_subscribed, false, memory_order_release);
    atomic_store_explicit(&s_host_synchronized, false, memory_order_release);
    atomic_store_explicit(&s_conn_handle, UINT16_MAX, memory_order_release);
    atomic_store_explicit(&s_start_error, ESP_OK, memory_order_release);
#ifdef CONFIG_BLE_TRANSPORT_TEST_HOOKS
    atomic_store_explicit(&s_test_notify_attempts, 0U, memory_order_release);
    s_test_delivery_count = 0U;
    memset(s_test_deliveries, 0, sizeof(s_test_deliveries));
#endif
    xSemaphoreGive(s_lifecycle_mutex);
    return ESP_OK;
}

/* NimBLE-specific implementation is compiled only in BT-enabled firmware. */
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static uint8_t s_own_addr_type;
static uint16_t s_tx_value_handle;
static const ble_uuid16_t s_service_uuid =
    BLE_UUID16_INIT(BLE_TRANSPORT_SERVICE_UUID16);
static const ble_uuid16_t s_rx_uuid =
    BLE_UUID16_INIT(BLE_TRANSPORT_RX_UUID16);
static const ble_uuid16_t s_tx_uuid =
    BLE_UUID16_INIT(BLE_TRANSPORT_TX_UUID16);
static const ble_uuid16_t s_secure_rx_uuid =
    BLE_UUID16_INIT(BLE_TRANSPORT_SECURE_RX_UUID16);

static void notify_connection(bool connected)
{
    atomic_store_explicit(&s_connected, connected, memory_order_release);
    if (!connected) {
        atomic_store_explicit(&s_subscribed, false, memory_order_release);
        atomic_store_explicit(&s_encrypted, false, memory_order_release);
        atomic_store_explicit(&s_conn_handle, UINT16_MAX, memory_order_release);
    }
    if (s_rt.config.connection_sink)
        s_rt.config.connection_sink(connected, s_rt.config.context);
}

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)arg;
    bool normal_rx = ble_uuid_cmp(ctxt->chr->uuid, &s_rx_uuid.u) == 0;
    bool secure_rx = ble_uuid_cmp(
        ctxt->chr->uuid, &s_secure_rx_uuid.u) == 0;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR &&
        (normal_rx || secure_rx)) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len == 0 || len > BLE_TRANSPORT_RX_CHUNK_MAX)
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        rx_chunk_t item = { .length = len, .secure = secure_rx };
        uint16_t copied = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, item.data, sizeof(item.data),
                                &copied) != 0 || copied != len)
            return BLE_ATT_ERR_UNLIKELY;
        if (xQueueSend(s_rt.rx_queue, &item, 0) != pdTRUE) {
            if (s_rt.state_mutex && xSemaphoreTake(s_rt.state_mutex, 0) == pdTRUE) {
                s_rt.counters.rx_queue_drops++;
                xSemaphoreGive(s_rt.state_mutex);
            }
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR &&
        attr_handle == s_tx_value_handle) {
        if (!take_mutex(s_rt.state_mutex, 10)) return BLE_ATT_ERR_UNLIKELY;
        int rc = os_mbuf_append(ctxt->om, s_rt.last_tx, s_rt.last_tx_len);
        xSemaphoreGive(s_rt.state_mutex);
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_rx_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &s_tx_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_tx_value_handle,
            },
            {
                .uuid = &s_secure_rx_uuid.u,
                .access_cb = gatt_access,
                /* Demo provisioning remains protected by the physical
                 * button authorization window. Do not request an OS-level
                 * BLE bond: WeChat GATT connections must stay pairless. */
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            { 0 }
        },
    },
    { 0 }
};

static int start_advertising(void);

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            atomic_store_explicit(&s_conn_handle, event->connect.conn_handle,
                                  memory_order_release);
            notify_connection(true);
        } else if (!atomic_load_explicit(&s_stop_requested, memory_order_acquire)) {
            (void)start_advertising();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        notify_connection(false);
        if (!atomic_load_explicit(&s_stop_requested, memory_order_acquire))
            (void)start_advertising();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (!atomic_load_explicit(&s_stop_requested, memory_order_acquire))
            (void)start_advertising();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_value_handle) {
            atomic_store_explicit(&s_subscribed,
                event->subscribe.cur_notify != 0, memory_order_release);
        }
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE: {
        bool encrypted = false;
        struct ble_gap_conn_desc desc;
        if (event->enc_change.status == 0 &&
            ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0)
            encrypted = desc.sec_state.encrypted != 0;
        atomic_store_explicit(
            &s_encrypted, encrypted, memory_order_release);
        return 0;
    }
    default:
        return 0;
    }
}

static void log_ble_heap(const char *step)
{
    ESP_LOGI(TAG, "BLE_HEAP[%s] int_free=%u int_largest=%u int_min=%u",
             step,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

static int start_advertising(void)
{
    static int s_adv_call_count = 0;
    s_adv_call_count++;
    ESP_LOGI(TAG, "start_advertising call #%d", s_adv_call_count);
    log_ble_heap("pre_adv_set_fields");

    /* Legacy advertising: adv data ≤ 31 bytes, scan response ≤ 31 bytes.
     * Strategy: put flags + UUID in adv, put device name in scan response.
     * This keeps both payloads well under the 31-byte limit. */

    /* Advertising data: flags (3) + UUID16 (2+2) = 7 bytes + header (2) = 9 */
    struct ble_hs_adv_fields adv_fields = {0};
    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv_fields.uuids16 = (ble_uuid16_t *)&s_service_uuid;
    adv_fields.num_uuids16 = 1;
    adv_fields.uuids16_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&adv_fields);
    ESP_LOGI(TAG, "adv_set_fields rc=%d", rc);
    log_ble_heap("post_adv_set_fields");
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields FAILED rc=%d", rc);
        return rc;
    }

    /* Scan response: device name only */
    struct ble_hs_adv_fields rsp_fields = {0};
    rsp_fields.name = (uint8_t *)s_rt.device_name;
    rsp_fields.name_len = strlen(s_rt.device_name);
    rsp_fields.name_is_complete = 1;
    ESP_LOGI(TAG, "scan_rsp name_len=%u", (unsigned)rsp_fields.name_len);

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    ESP_LOGI(TAG, "adv_rsp_set_fields rc=%d", rc);
    if (rc != 0) {
        ESP_LOGW(TAG, "scan_rsp set failed rc=%d (non-fatal)", rc);
    }

    log_ble_heap("pre_adv_start");

    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &params, gap_event, NULL);
    ESP_LOGI(TAG, "adv_start rc=%d", rc);
    log_ble_heap("post_adv_start");
    return rc;
}

static void host_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc == 0) rc = start_advertising();
    /* Advertising failure (e.g. BLE_ERR_MEM_CAPACITY) is non-fatal.
     * The device won't be visible but the transport can still operate.
     * Advertising will be retried on connect/disconnect events. */
    if (rc != 0) {
        ESP_LOGW(TAG, "advertising failed rc=%d (non-fatal, will retry)", rc);
    }
    atomic_store_explicit(&s_start_error, ESP_OK,
                          memory_order_release);
    atomic_store_explicit(&s_host_synchronized, true, memory_order_release);
    xEventGroupSetBits(s_rt.events, EVT_HOST_SYNC);
}

static void host_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE reset reason=%d", reason);
    atomic_store_explicit(&s_host_synchronized, false, memory_order_release);
    notify_connection(false);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    esp_err_t err = nimble_port_deinit();
    if (err != ESP_OK) record_error(err);
    xEventGroupSetBits(s_rt.events, EVT_HOST_DONE);
    nimble_port_freertos_deinit();
}

static esp_err_t send_notification(const tx_message_t *message)
{
    uint16_t conn = atomic_load_explicit(&s_conn_handle, memory_order_acquire);
    if (conn == UINT16_MAX ||
        !atomic_load_explicit(&s_connected, memory_order_acquire) ||
        !atomic_load_explicit(&s_subscribed, memory_order_acquire))
        return ESP_ERR_INVALID_STATE;

    uint16_t mtu = ble_att_mtu(conn);
    size_t chunk_max = mtu > 3 ? (size_t)(mtu - 3U) : 20U;
    for (size_t offset = 0; offset < message->length; offset += chunk_max) {
        size_t chunk_len = message->length - offset;
        if (chunk_len > chunk_max) chunk_len = chunk_max;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(message->data + offset,
                                                   chunk_len);
        if (!om) return ESP_ERR_NO_MEM;
        if (ble_gatts_notify_custom(conn, s_tx_value_handle, om) != 0)
            return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t nimble_start(void)
{
    log_ble_heap("pre_nimble_port_init");
    esp_err_t err = nimble_port_init();
    ESP_LOGI(TAG, "nimble_port_init err=0x%x", (unsigned)err);
    log_ble_heap("post_nimble_port_init");
    if (err != ESP_OK) return err;

    /* Pairless GATT for WeChat Mini Program compatibility. Credential
     * mutation is authorized at the application layer by physical presence. */
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_sc = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_our_key_dist = 0;
    ble_hs_cfg.sm_their_key_dist = 0;
    ble_hs_cfg.reset_cb = host_reset;
    ble_hs_cfg.sync_cb = host_sync;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_LOGI(TAG, "gap/gatt init done");
    log_ble_heap("post_gap_gatt_init");

    int rc = ble_gatts_count_cfg(s_services);
    ESP_LOGI(TAG, "gatts_count rc=%d", rc);
    if (rc == 0) rc = ble_gatts_add_svcs(s_services);
    ESP_LOGI(TAG, "gatts_add_svcs rc=%d", rc);
    if (rc == 0) rc = ble_svc_gap_device_name_set(s_rt.device_name);
    ESP_LOGI(TAG, "gap_device_name_set rc=%d", rc);
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble_start service registration failed rc=%d", rc);
        (void)nimble_port_deinit();
        return ESP_FAIL;
    }
    log_ble_heap("post_service_reg");
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "host_task started");
    return ESP_OK;
}

static esp_err_t nimble_request_stop(void)
{
    return nimble_port_stop() == 0 ? ESP_OK : ESP_FAIL;
}

static bool nimble_has_host_task(void) { return true; }

#elif defined(CONFIG_BLE_TRANSPORT_TEST_HOOKS)

static bool consume_test_failure(_Atomic uint32_t *remaining)
{
    uint32_t value = atomic_load_explicit(remaining, memory_order_acquire);
    while (value > 0U) {
        if (atomic_compare_exchange_weak_explicit(
                remaining, &value, value - 1U,
                memory_order_acq_rel, memory_order_acquire)) return true;
    }
    return false;
}

static void notify_connection(bool connected)
{
    atomic_store_explicit(&s_connected, connected, memory_order_release);
    if (!connected) {
        atomic_store_explicit(&s_subscribed, false, memory_order_release);
        atomic_store_explicit(&s_conn_handle, UINT16_MAX, memory_order_release);
    }
    if (s_rt.config.connection_sink)
        s_rt.config.connection_sink(connected, s_rt.config.context);
}

static esp_err_t send_notification(const tx_message_t *message)
{
    atomic_fetch_add_explicit(&s_test_notify_attempts, 1U, memory_order_acq_rel);
    if (consume_test_failure(&s_test_notify_failures)) return ESP_FAIL;
    if (!take_mutex(s_rt.state_mutex, 100)) return ESP_ERR_TIMEOUT;
    if (s_test_delivery_count < BLE_TRANSPORT_TEST_MAX_DELIVERIES) {
        s_test_deliveries[s_test_delivery_count++] = *message;
    }
    xSemaphoreGive(s_rt.state_mutex);
    return ESP_OK;
}

static esp_err_t nimble_start(void)
{
    if (!atomic_load_explicit(&s_test_start_sync_suppressed,
                              memory_order_acquire)) {
        atomic_store_explicit(&s_start_error, ESP_OK, memory_order_release);
        atomic_store_explicit(&s_host_synchronized, true, memory_order_release);
        xEventGroupSetBits(s_rt.events, EVT_HOST_SYNC);
    }
    return ESP_OK;
}

static esp_err_t nimble_request_stop(void)
{
    if (consume_test_failure(&s_test_stop_failures)) return ESP_FAIL;
    xEventGroupSetBits(s_rt.events, EVT_HOST_DONE);
    return ESP_OK;
}

static bool nimble_has_host_task(void) { return true; }

#else

static void notify_connection(bool connected) { (void)connected; }
static esp_err_t send_notification(const tx_message_t *message)
{
    (void)message;
    return ESP_ERR_NOT_SUPPORTED;
}
static esp_err_t nimble_start(void) { return ESP_ERR_NOT_SUPPORTED; }
static esp_err_t nimble_request_stop(void) { return ESP_OK; }
static bool nimble_has_host_task(void) { return false; }

#endif

/* Worker tasks and the public lifecycle API follow below. */
static void rx_task_main(void *arg)
{
    (void)arg;
    ble_frame_assembler_t assembler;
    ble_frame_assembler_t secure_assembler;
    ble_frame_assembler_init(&assembler);
    ble_frame_assembler_init(&secure_assembler);
    record_stack_high_water(true);
    while (!atomic_load_explicit(&s_stop_requested, memory_order_acquire)) {
        rx_chunk_t item;
        if (xQueueReceive(s_rt.rx_queue, &item, pdMS_TO_TICKS(100)) != pdTRUE)
            continue;
        if (atomic_load_explicit(&s_stop_requested, memory_order_acquire)) break;

        const char *frame = NULL;
        size_t frame_len = 0;
        ble_frame_assembler_t *selected =
            item.secure ? &secure_assembler : &assembler;
        ble_frame_result_t result = ble_frame_assembler_feed(
            selected, item.data, item.length, now_ms(), &frame, &frame_len);
        if (take_mutex(s_rt.state_mutex, 100)) {
            if (item.secure) {
                s_rt.counters.secure_rx_chunks++;
                if (result == BLE_FRAME_COMPLETE)
                    s_rt.counters.secure_rx_frames++;
                else if (result != BLE_FRAME_NEED_MORE)
                    s_rt.counters.secure_frame_errors++;
            } else {
                s_rt.counters.rx_chunks++;
                if (result == BLE_FRAME_COMPLETE) s_rt.counters.rx_frames++;
                else if (result != BLE_FRAME_NEED_MORE)
                    s_rt.counters.rx_frame_errors++;
            }
            xSemaphoreGive(s_rt.state_mutex);
        }
        if (result == BLE_FRAME_COMPLETE) {
            ble_transport_frame_sink_fn sink = item.secure
                ? s_rt.config.secure_frame_sink : s_rt.config.frame_sink;
            esp_err_t err = sink
                ? sink(frame, frame_len, s_rt.config.context)
                : ESP_ERR_NOT_SUPPORTED;
            if (err != ESP_OK && take_mutex(s_rt.state_mutex, 100)) {
                if (item.secure)
                    s_rt.counters.secure_frame_sink_errors++;
                else
                    s_rt.counters.frame_sink_errors++;
                s_rt.counters.last_error = err;
                xSemaphoreGive(s_rt.state_mutex);
            }
            ble_frame_assembler_reset(selected);
        }
        record_stack_high_water(true);
    }
#ifdef CONFIG_BLE_TRANSPORT_TEST_HOOKS
    while (atomic_load_explicit(&s_test_exit_barrier, memory_order_acquire))
        vTaskDelay(1);
#endif
    record_stack_high_water(true);
    xEventGroupSetBits(s_rt.events, EVT_RX_DONE);
    vTaskDelete(NULL);
}

static void tx_task_main(void *arg)
{
    (void)arg;
    tx_message_t message;
    bool message_valid = false;
    bool message_critical = false;
    record_stack_high_water(false);
    while (!atomic_load_explicit(&s_stop_requested, memory_order_acquire)) {
#ifdef CONFIG_BLE_TRANSPORT_TEST_HOOKS
        while (atomic_load_explicit(&s_test_tx_paused, memory_order_acquire) &&
               !atomic_load_explicit(&s_stop_requested, memory_order_acquire))
            vTaskDelay(1);
#endif
        if (!atomic_load_explicit(&s_connected, memory_order_acquire) ||
            !atomic_load_explicit(&s_subscribed, memory_order_acquire)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (!message_valid) {
            message_critical =
                xQueueReceive(s_rt.critical_queue, &message, 0) == pdTRUE;
            if (!message_critical &&
                xQueueReceive(s_rt.status_mailbox, &message, 0) != pdTRUE) {
                /* Never block on the lower-priority mailbox: a critical reply
                 * may arrive while that receive is sleeping and must not be
                 * overtaken by a later status publication. */
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            message_valid = true;
        }
        esp_err_t err = send_notification(&message);
        if (err != ESP_OK && message_critical &&
            !atomic_load_explicit(&s_stop_requested, memory_order_acquire)) {
            /* Keep ownership locally until delivery succeeds. Requeueing here
             * has a race: another producer can fill the slot opened by the
             * receive, causing a silent loss of this older critical message. */
            message_valid = true;
        } else {
            message_valid = false;
        }
        if (take_mutex(s_rt.state_mutex, 100)) {
            if (err == ESP_OK) {
                s_rt.counters.tx_messages++;
                memcpy(s_rt.last_tx, message.data, message.length + 1U);
                s_rt.last_tx_len = message.length;
            } else {
                s_rt.counters.tx_notify_errors++;
                s_rt.counters.last_error = err;
            }
            xSemaphoreGive(s_rt.state_mutex);
        }
        record_stack_high_water(false);
        if (err != ESP_OK) vTaskDelay(pdMS_TO_TICKS(50));
    }
#ifdef CONFIG_BLE_TRANSPORT_TEST_HOOKS
    while (atomic_load_explicit(&s_test_exit_barrier, memory_order_acquire))
        vTaskDelay(1);
#endif
    record_stack_high_water(false);
    xEventGroupSetBits(s_rt.events, EVT_TX_DONE);
    vTaskDelete(NULL);
}

esp_err_t xiaojing_ble_transport_start(void)
{
    if (!xiaojing_ble_transport_is_supported()) return ESP_ERR_NOT_SUPPORTED;
    if (!take_mutex(s_lifecycle_mutex, 1000)) return ESP_ERR_TIMEOUT;
    if (s_rt.lifecycle != BLE_LC_INITIALIZED) {
        xSemaphoreGive(s_lifecycle_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_rt.lifecycle = BLE_LC_STARTING;
    xEventGroupClearBits(s_rt.events, EVT_ALL_DONE | EVT_HOST_SYNC);
    atomic_store_explicit(&s_stop_requested, false, memory_order_release);
    xSemaphoreGive(s_lifecycle_mutex);

    if (xTaskCreatePinnedToCoreWithCaps(
            rx_task_main, "ble_rx", RX_TASK_STACK_BYTES, NULL,
            RX_TASK_PRIORITY, &s_rt.rx_task, 0,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        record_error(ESP_ERR_NO_MEM);
        (void)xiaojing_ble_transport_stop();
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCoreWithCaps(
            tx_task_main, "ble_tx", TX_TASK_STACK_BYTES, NULL,
            TX_TASK_PRIORITY, &s_rt.tx_task, 0,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        record_error(ESP_ERR_NO_MEM);
        (void)xiaojing_ble_transport_stop();
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = nimble_start();
    if (err != ESP_OK) {
        record_error(err);
        (void)xiaojing_ble_transport_stop();
        return err;
    }
    s_rt.host_started = nimble_has_host_task();
    EventBits_t bits = xEventGroupWaitBits(s_rt.events, EVT_HOST_SYNC,
        pdFALSE, pdTRUE, pdMS_TO_TICKS(START_TIMEOUT_MS));
    err = (bits & EVT_HOST_SYNC) ?
        (esp_err_t)atomic_load_explicit(&s_start_error, memory_order_acquire) :
        ESP_ERR_TIMEOUT;
    if (err != ESP_OK) {
        record_error(err);
        (void)xiaojing_ble_transport_stop();
        return err;
    }
    if (!take_mutex(s_lifecycle_mutex, 1000)) return ESP_ERR_TIMEOUT;
    if (s_rt.lifecycle != BLE_LC_STARTING) {
        xSemaphoreGive(s_lifecycle_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_rt.lifecycle = BLE_LC_RUNNING;
    if (take_mutex(s_rt.state_mutex, 100)) {
        s_rt.counters.running = true;
        xSemaphoreGive(s_rt.state_mutex);
    }
    xSemaphoreGive(s_lifecycle_mutex);
    ESP_LOGI(TAG, "running as %s", s_rt.device_name);
    return ESP_OK;
}

static esp_err_t queue_tx(QueueHandle_t queue, const char *json,
                          size_t length, uint32_t timeout_ms, bool overwrite)
{
    if (!json || length == 0 || length > BLE_TRANSPORT_MAX_TX_BYTES)
        return ESP_ERR_INVALID_ARG;
    if (!take_mutex(s_lifecycle_mutex, 1000)) return ESP_ERR_TIMEOUT;
    if (s_rt.lifecycle != BLE_LC_RUNNING || !queue) {
        xSemaphoreGive(s_lifecycle_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    tx_message_t message = { .length = (uint16_t)length };
    memcpy(message.data, json, length);
    message.data[length] = '\0';
    BaseType_t ok = overwrite ? xQueueOverwrite(queue, &message) :
                                xQueueSend(queue, &message,
                                           timeout_ticks(timeout_ms));
    if (ok != pdTRUE && !overwrite && take_mutex(s_rt.state_mutex, 100)) {
        s_rt.counters.critical_queue_drops++;
        xSemaphoreGive(s_rt.state_mutex);
    }
    xSemaphoreGive(s_lifecycle_mutex);
    return ok == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t xiaojing_ble_transport_send_critical(const char *json, size_t length,
                                               uint32_t timeout_ms)
{
    return queue_tx(s_rt.critical_queue, json, length, timeout_ms, false);
}

esp_err_t xiaojing_ble_transport_publish_status(const char *json, size_t length)
{
    return queue_tx(s_rt.status_mailbox, json, length, 0, true);
}

esp_err_t xiaojing_ble_transport_stop(void)
{
    if (!s_lifecycle_mutex) return ESP_ERR_INVALID_STATE;
    if (!take_mutex(s_lifecycle_mutex, 1000)) return ESP_ERR_TIMEOUT;
    if (s_rt.lifecycle == BLE_LC_UNINITIALIZED) {
        xSemaphoreGive(s_lifecycle_mutex);
        return ESP_OK;
    }
    bool was_active = s_rt.lifecycle != BLE_LC_INITIALIZED;
    s_rt.lifecycle = BLE_LC_STOPPING;
    atomic_store_explicit(&s_stop_requested, true, memory_order_release);
    if (s_rt.rx_queue) {
        rx_chunk_t wake = {0};
        (void)xQueueSend(s_rt.rx_queue, &wake, 0);
    }
    bool has_rx = s_rt.rx_task != NULL;
    bool has_tx = s_rt.tx_task != NULL;
    bool has_host = s_rt.host_started;
    if (was_active && has_host) {
        esp_err_t host_stop_err = nimble_request_stop();
        if (host_stop_err != ESP_OK) record_error(host_stop_err);
    }
    xSemaphoreGive(s_lifecycle_mutex);

    EventBits_t need = 0;
    if (has_rx) need |= EVT_RX_DONE;
    if (has_tx) need |= EVT_TX_DONE;
    if (has_host) need |= EVT_HOST_DONE;
    if (need != 0) {
        EventBits_t bits = xEventGroupWaitBits(s_rt.events, need, pdFALSE, pdTRUE,
                                               pdMS_TO_TICKS(STOP_TIMEOUT_MS));
        if ((bits & need) != need) return ESP_ERR_TIMEOUT;
    }

    if (!take_mutex(s_lifecycle_mutex, 1000)) return ESP_ERR_TIMEOUT;
    notify_connection(false);
    QueueHandle_t rx = s_rt.rx_queue;
    QueueHandle_t critical = s_rt.critical_queue;
    QueueHandle_t status = s_rt.status_mailbox;
    EventGroupHandle_t events = s_rt.events;
    SemaphoreHandle_t state = s_rt.state_mutex;
    memset(&s_rt, 0, sizeof(s_rt));
    vQueueDelete(rx);
    vQueueDelete(critical);
    vQueueDelete(status);
    vEventGroupDelete(events);
    vSemaphoreDelete(state);
    xSemaphoreGive(s_lifecycle_mutex);
    return ESP_OK;
}

esp_err_t xiaojing_ble_transport_get_snapshot(ble_transport_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_lifecycle_mutex || !take_mutex(s_lifecycle_mutex, 1000))
        return ESP_ERR_INVALID_STATE;
    if (!s_rt.state_mutex || !take_mutex(s_rt.state_mutex, 100)) {
        xSemaphoreGive(s_lifecycle_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    ble_transport_snapshot_t tmp = s_rt.counters;
    tmp.initialized = s_rt.lifecycle != BLE_LC_UNINITIALIZED;
    tmp.running = s_rt.lifecycle == BLE_LC_RUNNING;
    tmp.host_synchronized = atomic_load_explicit(&s_host_synchronized,
                                                 memory_order_acquire);
    tmp.connected = atomic_load_explicit(&s_connected, memory_order_acquire);
    tmp.encrypted = atomic_load_explicit(&s_encrypted, memory_order_acquire);
    tmp.subscribed = atomic_load_explicit(&s_subscribed, memory_order_acquire);
    tmp.connection_handle = atomic_load_explicit(&s_conn_handle,
                                                 memory_order_acquire);
    xSemaphoreGive(s_rt.state_mutex);
    xSemaphoreGive(s_lifecycle_mutex);
    *out = tmp;
    return ESP_OK;
}

#ifdef CONFIG_BLE_TRANSPORT_TEST_HOOKS

void ble_transport_test_set_start_sync_suppressed(bool suppressed)
{
    atomic_store_explicit(&s_test_start_sync_suppressed, suppressed,
                          memory_order_release);
}

void ble_transport_test_set_stop_failures(uint32_t count)
{
    atomic_store_explicit(&s_test_stop_failures, count, memory_order_release);
}

void ble_transport_test_set_notify_failures(uint32_t count)
{
    atomic_store_explicit(&s_test_notify_failures, count, memory_order_release);
}

void ble_transport_test_set_exit_barrier(bool enabled)
{
    atomic_store_explicit(&s_test_exit_barrier, enabled, memory_order_release);
}

void ble_transport_test_set_tx_paused(bool paused)
{
    atomic_store_explicit(&s_test_tx_paused, paused, memory_order_release);
}

void ble_transport_test_set_link(bool connected, bool subscribed)
{
    if (connected) {
        atomic_store_explicit(&s_conn_handle, 1U, memory_order_release);
        notify_connection(true);
        atomic_store_explicit(&s_subscribed, subscribed, memory_order_release);
    } else {
        notify_connection(false);
    }
}

static esp_err_t test_inject_rx(
    const uint8_t *data, size_t length, bool secure)
{
    if (!data || length == 0U || length > BLE_TRANSPORT_RX_CHUNK_MAX)
        return ESP_ERR_INVALID_ARG;
    if (!take_mutex(s_lifecycle_mutex, 1000)) return ESP_ERR_TIMEOUT;
    if (s_rt.lifecycle != BLE_LC_RUNNING || !s_rt.rx_queue) {
        xSemaphoreGive(s_lifecycle_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    rx_chunk_t item = {
        .length = (uint16_t)length,
        .secure = secure,
    };
    memcpy(item.data, data, length);
    BaseType_t ok = xQueueSend(s_rt.rx_queue, &item, 0);
    xSemaphoreGive(s_lifecycle_mutex);
    return ok == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t ble_transport_test_inject_rx(const uint8_t *data, size_t length)
{
    return test_inject_rx(data, length, false);
}

esp_err_t ble_transport_test_inject_secure_rx(
    const uint8_t *data, size_t length)
{
    return test_inject_rx(data, length, true);
}

uint32_t ble_transport_test_get_notify_attempts(void)
{
    return atomic_load_explicit(&s_test_notify_attempts, memory_order_acquire);
}

uint32_t ble_transport_test_get_delivery_count(void)
{
    uint32_t count = 0U;
    if (take_mutex(s_lifecycle_mutex, 1000)) {
        if (s_rt.state_mutex && take_mutex(s_rt.state_mutex, 100)) {
            count = s_test_delivery_count;
            xSemaphoreGive(s_rt.state_mutex);
        }
        xSemaphoreGive(s_lifecycle_mutex);
    }
    return count;
}

esp_err_t ble_transport_test_get_delivery(uint32_t index, char *out,
                                          size_t capacity)
{
    if (!out || capacity == 0U) return ESP_ERR_INVALID_ARG;
    if (!take_mutex(s_lifecycle_mutex, 1000)) return ESP_ERR_TIMEOUT;
    if (!s_rt.state_mutex || !take_mutex(s_rt.state_mutex, 100)) {
        xSemaphoreGive(s_lifecycle_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (index < s_test_delivery_count) {
        const tx_message_t *message = &s_test_deliveries[index];
        if ((size_t)message->length + 1U <= capacity) {
            memcpy(out, message->data, message->length + 1U);
            err = ESP_OK;
        } else {
            err = ESP_ERR_INVALID_SIZE;
        }
    }
    xSemaphoreGive(s_rt.state_mutex);
    xSemaphoreGive(s_lifecycle_mutex);
    return err;
}

esp_err_t ble_transport_test_reset(void)
{
    atomic_store_explicit(&s_test_exit_barrier, false, memory_order_release);
    atomic_store_explicit(&s_test_tx_paused, false, memory_order_release);
    atomic_store_explicit(&s_test_start_sync_suppressed, false,
                          memory_order_release);
    atomic_store_explicit(&s_test_stop_failures, 0U, memory_order_release);
    atomic_store_explicit(&s_test_notify_failures, 0U, memory_order_release);
    ble_transport_test_set_link(false, false);
    esp_err_t err = xiaojing_ble_transport_stop();
    if (err == ESP_ERR_INVALID_STATE) err = ESP_OK;
    if (err != ESP_OK) return err;
    atomic_store_explicit(&s_test_notify_attempts, 0U, memory_order_release);
    s_test_delivery_count = 0U;
    memset(s_test_deliveries, 0, sizeof(s_test_deliveries));
    return ESP_OK;
}

#endif
