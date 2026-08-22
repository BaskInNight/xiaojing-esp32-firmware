#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_STATION_SSID_CAPACITY 33U
#define WIFI_STATION_PASSWORD_CAPACITY 64U
#define WIFI_STATION_DEFAULT_NAMESPACE "xj_wifi"

typedef struct {
    char ssid[WIFI_STATION_SSID_CAPACITY];
    char password[WIFI_STATION_PASSWORD_CAPACITY];
} wifi_station_credentials_t;

typedef enum {
    WIFI_STATION_UNINITIALIZED = 0,
    WIFI_STATION_INITIALIZED,
    WIFI_STATION_CONNECTING,
    WIFI_STATION_CONNECTED,
    WIFI_STATION_FAILED,
    WIFI_STATION_STOPPING,
} wifi_station_state_t;

typedef void (*wifi_station_connection_sink_t)(
    bool connected, void *context);

typedef struct {
    const char *nvs_namespace;
    uint32_t connect_timeout_ms;
    uint32_t max_retries;
    /* Optional non-blocking observer. Called after the internal state lock is
     * released on IP acquisition and station disconnection. */
    wifi_station_connection_sink_t connection_sink;
    void *connection_context;
} wifi_station_config_t;

typedef struct {
    wifi_station_state_t state;
    bool provisioned;
    bool connected;
    uint32_t connect_attempts;
    uint32_t disconnects;
    int32_t last_disconnect_reason;
    esp_err_t last_error;
    uint32_t ip_address;
} wifi_station_snapshot_t;

bool xiaojing_wifi_credentials_valid(
    const wifi_station_credentials_t *credentials);
esp_err_t xiaojing_wifi_save_credentials(
    const char *nvs_namespace,
    const wifi_station_credentials_t *credentials);
esp_err_t xiaojing_wifi_load_credentials(
    const char *nvs_namespace,
    wifi_station_credentials_t *out);
esp_err_t xiaojing_wifi_erase_credentials(const char *nvs_namespace);
/* Save credentials for the initialized station and start connection on its
 * owned task. Intended for a trusted local provisioning transport such as
 * the XiaoJing BLE application protocol. */
esp_err_t xiaojing_wifi_provision_and_start(
    const wifi_station_credentials_t *credentials);

esp_err_t xiaojing_wifi_global_init(void);
esp_err_t xiaojing_wifi_init(const wifi_station_config_t *config);
esp_err_t xiaojing_wifi_start(void);
/* Starts the synchronous connection procedure on an owned FreeRTOS task.
 * Returns after the task has been created, not after an IP is acquired.
 * xiaojing_wifi_stop() cancels and joins that task before cleanup. */
esp_err_t xiaojing_wifi_start_async(void);
esp_err_t xiaojing_wifi_stop(void);
bool xiaojing_wifi_is_connected(void);
esp_err_t xiaojing_wifi_get_snapshot(wifi_station_snapshot_t *out);

/* Dump WiFi config without exposing password */
esp_err_t xiaojing_wifi_dump_config(void);

/* Scan for specific AP and report channel, RSSI, authmode */
esp_err_t xiaojing_wifi_scan_for_ap(const char *target_ssid,
                                    int *out_channel,
                                    int *out_rssi,
                                    int *out_authmode);

#ifdef XIAOJING_TESTING
void xiaojing_wifi_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif
