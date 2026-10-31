#pragma once

#include "esp_err.h"
#include "xiaojing_hal.h"
#include "machine_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * app_bootstrap.h — 安全初始化入口 (Round 2)
 * ================================================================ */

#define APP_BOOTSTRAP_EVENT_QUEUE_DEPTH  16

/* Forward declare opaque types */
struct wash_executor;
typedef struct wash_executor wash_executor_t;
struct real_adapter_ctx;
typedef struct real_adapter_ctx real_adapter_ctx_t;

typedef struct {
    const xiaojing_hal_t *hal;
    wash_executor_t *executor;
    real_adapter_ctx_t *adapter_ctx;
    bool nvs_ok;
    bool config_ok;
    bool status_store_ok;
    bool safety_mgr_ok;
    bool position_svc_ok;
    bool bl50_svc_ok;
    bool water_svc_ok;
    bool dry_svc_ok;
    bool coupled_hot_air_svc_ok;
    bool drain_svc_ok;
    bool detergent_svc_ok;
    bool uv_svc_ok;
    bool executor_ok;
    bool wifi_station_ok;
    bool wifi_provisioned;
    bool wifi_start_requested;
    esp_err_t wifi_start_error;
    bool voice_credentials_ready;
    bool voice_svc_ok;
    bool display_svc_ok;
    bool gesture_svc_ok;
    bool button_svc_ok;
    bool app_protocol_ok;
    bool ble_transport_ok;
} app_bootstrap_result_t;

esp_err_t app_bootstrap_init(app_bootstrap_result_t *result);
esp_err_t app_bootstrap_deinit(void);

/** Get the shared machine event queue (services publish here). */
void *app_bootstrap_get_event_queue(void);

#ifdef __cplusplus
}
#endif
