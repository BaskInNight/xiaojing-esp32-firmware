/*
 * xiao jing ESP-IDF firmware -- application entry point.
 */

#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "board_config.h"
#include "app_bootstrap.h"

static const char *TAG = "xiaojing";

void app_main(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "XiaoJing ESP-IDF Firmware");
    ESP_LOGI(TAG, "  Version    : Phase 9 Round 2.2");
    ESP_LOGI(TAG, "  Chip       : %s", CONFIG_IDF_TARGET);
    ESP_LOGI(TAG, "  Cores      : %d", chip_info.cores);
    ESP_LOGI(TAG, "  Revision   : %d.%d",
             chip_info.revision / 100, chip_info.revision % 100);
    ESP_LOGI(TAG, "  Free heap  : %"PRIu32" bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "  Output mode: %s", board_config_get_output_mode_str());
    ESP_LOGI(TAG, "  PTC        : %s",
             board_config_is_ptc_enabled() ? "ENABLED" : "disabled");
    ESP_LOGI(TAG, "========================================");

    if (board_config_get_output_mode() != XIAOJING_MODE_FAKE) {
        ESP_LOGW(TAG, "Non-FAKE mode: real hardware outputs active!");
    }

    app_bootstrap_result_t boot_result;
    esp_err_t err = app_bootstrap_init(&boot_result);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Bootstrap failed: 0x%x", err);
        ESP_LOGE(TAG, "System halted in safe state.");
        return;
    }

    ESP_LOGI(TAG, "Bootstrap complete:");
    ESP_LOGI(TAG, "  NVS        : %s", boot_result.nvs_ok ? "OK" : "FAIL");
    ESP_LOGI(TAG, "  Config     : %s", boot_result.config_ok ? "OK" : "FAIL");
    ESP_LOGI(TAG, "  StatusStore: %s",
             boot_result.status_store_ok ? "OK" : "FAIL");
    ESP_LOGI(TAG, "  HAL        : %s", boot_result.hal ? "ready" : "NONE");
    ESP_LOGI(TAG, "  SafetyMgr  : %s",
             boot_result.safety_mgr_ok ? "OK" : "FAIL");
    ESP_LOGI(TAG, "  PositionSvc: %s",
             boot_result.position_svc_ok ? "OK" : "FAIL");
    ESP_LOGI(TAG, "  WaterSvc   : %s",
             boot_result.water_svc_ok ? "OK" : "FAIL");
    ESP_LOGI(TAG, "  DrySvc     : %s",
             boot_result.dry_svc_ok ? "OK" : "FAIL");
    ESP_LOGI(TAG, "  DrainSvc   : %s",
             boot_result.drain_svc_ok ? "OK" : "FAIL");
    ESP_LOGI(TAG, "  Detergent  : %s",
             boot_result.detergent_svc_ok ? "OK" : "FAIL");
    ESP_LOGI(TAG, "  UVSvc      : %s",
             boot_result.uv_svc_ok ? "OK" : "FAIL");
    ESP_LOGI(TAG, "  Executor   : %s",
             boot_result.executor_ok ? "OK" : "FAIL");
    ESP_LOGI(TAG, "  WiFi       : %s",
             board_config_is_voice_enabled() ?
             (boot_result.wifi_station_ok ?
              (boot_result.wifi_provisioned ?
               (boot_result.wifi_start_requested ?
                "CONNECTING" : "START FAILED") :
               "UNPROVISIONED") : "FAIL") :
             "DISABLED");
    ESP_LOGI(TAG, "  BLE        : %s",
             boot_result.ble_transport_ok ? "OK" : "DISABLED");
    ESP_LOGI(TAG, "  VoiceSvc   : %s",
             board_config_is_voice_enabled() ?
             (boot_result.voice_svc_ok ? "OK" :
              (boot_result.voice_credentials_ready ?
               "START FAILED" : "DORMANT (ADD API KEY)")) :
             "DISABLED");
    ESP_LOGI(TAG, "  ButtonSvc  : %s",
             boot_result.button_svc_ok ? "OK" : "SKIP");
    ESP_LOGI(TAG, "Bootstrap OK.");
}
