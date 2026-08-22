#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_TRANSPORT_DEVICE_NAME_DEFAULT "JJTP-XIAOJING"
#define BLE_TRANSPORT_MAX_TX_BYTES 1024U
#define BLE_TRANSPORT_RX_CHUNK_MAX 256U
#define BLE_TRANSPORT_SERVICE_UUID16 0xFFF0U
#define BLE_TRANSPORT_RX_UUID16      0xFFF1U
#define BLE_TRANSPORT_TX_UUID16      0xFFF2U
#define BLE_TRANSPORT_SECURE_RX_UUID16 0xFFF3U

typedef esp_err_t (*ble_transport_frame_sink_fn)(const char *frame,
                                                  size_t length,
                                                  void *context);
typedef void (*ble_transport_connection_sink_fn)(bool connected, void *context);

typedef struct {
    const char *device_name;
    ble_transport_frame_sink_fn frame_sink;
    /* Credential frames from FFF3. Writes are pairless; the application
     * layer enforces a physical-presence authorization window. Optional. */
    ble_transport_frame_sink_fn secure_frame_sink;
    ble_transport_connection_sink_fn connection_sink;
    void *context;
} ble_transport_config_t;

typedef struct {
    bool initialized;
    bool running;
    bool host_synchronized;
    bool connected;
    bool encrypted;
    bool subscribed;
    uint16_t connection_handle;
    uint32_t rx_chunks;
    uint32_t rx_frames;
    uint32_t rx_queue_drops;
    uint32_t rx_frame_errors;
    uint32_t frame_sink_errors;
    uint32_t secure_rx_chunks;
    uint32_t secure_rx_frames;
    uint32_t secure_frame_errors;
    uint32_t secure_frame_sink_errors;
    uint32_t tx_messages;
    uint32_t tx_notify_errors;
    uint32_t critical_queue_drops;
    uint32_t rx_stack_high_water_mark;
    uint32_t tx_stack_high_water_mark;
    esp_err_t last_error;
} ble_transport_snapshot_t;

bool xiaojing_ble_transport_is_supported(void);
esp_err_t xiaojing_ble_transport_init(const ble_transport_config_t *config);
esp_err_t xiaojing_ble_transport_start(void);
esp_err_t xiaojing_ble_transport_stop(void);
esp_err_t xiaojing_ble_transport_send_critical(const char *json, size_t length,
                                               uint32_t timeout_ms);
esp_err_t xiaojing_ble_transport_publish_status(const char *json, size_t length);
esp_err_t xiaojing_ble_transport_get_snapshot(ble_transport_snapshot_t *out);

#ifdef CONFIG_BLE_TRANSPORT_TEST_HOOKS

#define BLE_TRANSPORT_TEST_MAX_DELIVERIES 16U

void ble_transport_test_set_start_sync_suppressed(bool suppressed);
void ble_transport_test_set_stop_failures(uint32_t count);
void ble_transport_test_set_notify_failures(uint32_t count);
void ble_transport_test_set_exit_barrier(bool enabled);
void ble_transport_test_set_tx_paused(bool paused);
void ble_transport_test_set_link(bool connected, bool subscribed);
esp_err_t ble_transport_test_inject_rx(const uint8_t *data, size_t length);
esp_err_t ble_transport_test_inject_secure_rx(
    const uint8_t *data, size_t length);
uint32_t ble_transport_test_get_notify_attempts(void);
uint32_t ble_transport_test_get_delivery_count(void);
esp_err_t ble_transport_test_get_delivery(uint32_t index, char *out,
                                          size_t capacity);
esp_err_t ble_transport_test_reset(void);

#endif

#ifdef __cplusplus
}
#endif
