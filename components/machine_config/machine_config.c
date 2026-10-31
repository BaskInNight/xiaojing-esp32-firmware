/*
 * machine_config.c — NVS 版本化配置管理
 * 编译默认值 → NVS blob (magic/schema/crc32) 覆盖 → 校验 → 无效时恢复默认
 * 校验函数在 machine_config_validate.c（纯 C，与 host test 共享）
 */

#include <string.h>
#include <stdatomic.h>
#include "machine_config.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "machine_config";

static machine_config_t s_config;
static bool s_initialized = false;

/* 异步持久化任务：把 NVS 保存放到内部 RAM 栈的任务上执行。BLE rx 等
 * PSRAM 栈任务直接做 NVS/flash 写会触发 cache 禁用断言崩溃
 * （spi_flash_disable_interrupts_caches_and_other_cpu），所以配置写入
 * 内存后，把 flash 持久化委托给本任务。 */
#define CONFIG_PERSIST_TASK_STACK 8192U
static TaskHandle_t s_persist_task;
static _Atomic bool s_save_pending;
static void config_persist_task_main(void *arg);

/* ---- 编译默认值 ---- */

static const machine_config_t s_default_config = {
    .schema_version = MACHINE_CONFIG_SCHEMA_VERSION,
    .position = {
        .default_policy      = POSITION_DIR_PREFER_CW,
        .move_pwm_percent    = 30,
        .approach_pwm_percent = 15,
        .debounce_ms         = 20,
        .move_timeout_ms     = 30000,
        .brake_ms            = 500,
        .rpwm_is_cw          = true,
        .direction_calibrated = false,
        .bl50_reverse_dir    = false,
    },
    .water = {
        /* Vendor specification: F[Hz] = 98 * Q[L/min] -> 5880 pulses/L.
         * This is an initial field value; a measured-volume calibration may
         * subsequently replace it in NVS. */
        .pulses_per_liter           = 5880.0f,
        .no_flow_timeout_ms         = 5000,
        .low_flow_window_ms         = 3000,
        .source_batch_target_pulses = 500,
        .source_batch_max_ms        = 30000,
        .source_settle_ms           = 2000,
        .transfer_timeout_ms        = 30000,
        .default_transfer_ms        = 15000,
        .max_fill_cycles            = 10,
        .total_inlet_timeout_ms     = 600000,
    },
    .dry = {
        .max_total_ms          = 1800000,   /* 30min */
        .pre_fan_ms            = 5000,
        .heat_on_max_ms        = 60000,
        .heat_off_min_ms       = 60000,
        .cooldown_ms           = 60000,
        .heater_cutoff_c       = 55.0f,
        .heater_resume_c       = 45.0f,
        .fan_percent           = 80,
        .sht_stale_timeout_ms  = 10000,
    },
    .detergent = {
        .demo_duration_ms   = 3000,
        .formal_duration_ms = 6000,
        .max_single_ms      = 10000,
        .ml_per_second      = 0.0f,
    },
    .uv = {
        .default_duration_ms = 600000,   /* 10min */
        .max_duration_ms     = 1800000,  /* 30min */
    },
    .drain = {
        .max_duration_ms = 120000,       /* 2min */
    },
    .benchtst = {
        .bl50_pwm       = 40,            /* 搅动电机台架脉冲 PWM（对齐生产 PULSATOR 档） */
        .ibt2_pwm      = 80U,           /* 换位电机测试 PWM（角度切换接连续换向） */
        .act_duration_ms = 1500U,       /* 执行器自检默认时长（蠕动泵 0.8s→1.5s 等） */
    },
};

/* ---- NVS 存取 (带 magic/schema/crc32 封装) ---- */

static esp_err_t load_from_nvs(machine_config_t *out)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(MACHINE_CONFIG_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "NVS open failed (0x%x), using defaults", err);
        return err;
    }

    machine_config_nvs_blob_t blob;
    size_t size = sizeof(blob);
    err = nvs_get_blob(handle, "cfg_blob", &blob, &size);
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGI(TAG, "NVS blob read failed (0x%x), using defaults", err);
        return err;
    }

    if (size != sizeof(blob)) {
        ESP_LOGW(TAG, "NVS blob size mismatch: got %u, expected %u",
                 (unsigned)size, (unsigned)sizeof(blob));
        return ESP_ERR_INVALID_SIZE;
    }

    if (blob.magic != MACHINE_CONFIG_NVS_MAGIC) {
        ESP_LOGW(TAG, "NVS magic mismatch: 0x%08"PRIx32, blob.magic);
        return ESP_ERR_INVALID_VERSION;
    }

    if (blob.schema_version != MACHINE_CONFIG_SCHEMA_VERSION) {
        ESP_LOGW(TAG, "NVS schema mismatch: got %"PRIu32", expected %d",
                 blob.schema_version, MACHINE_CONFIG_SCHEMA_VERSION);
        return ESP_ERR_INVALID_VERSION;
    }

    if (blob.payload_size != sizeof(machine_config_t)) {
        ESP_LOGW(TAG, "NVS payload size mismatch: %"PRIu32" vs %u",
                 blob.payload_size, (unsigned)sizeof(machine_config_t));
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t expected_crc = esp_rom_crc32_le(0, (const uint8_t *)&blob.payload, sizeof(machine_config_t));
    if (blob.crc32 != expected_crc) {
        ESP_LOGW(TAG, "NVS CRC mismatch: stored 0x%08"PRIx32" vs computed 0x%08"PRIx32,
                 blob.crc32, expected_crc);
        return ESP_ERR_INVALID_CRC;
    }

    /* Cross-check: payload's own schema_version must match outer blob */
    if (blob.payload.schema_version != blob.schema_version) {
        ESP_LOGW(TAG, "NVS payload schema mismatch: payload %"PRIu32" vs blob %"PRIu32,
                 blob.payload.schema_version, blob.schema_version);
        return ESP_ERR_INVALID_VERSION;
    }

    *out = blob.payload;
    return ESP_OK;
}

static esp_err_t save_to_nvs(const machine_config_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(MACHINE_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open for write failed: 0x%x", err);
        return err;
    }

    machine_config_nvs_blob_t blob = {
        .magic = MACHINE_CONFIG_NVS_MAGIC,
        .schema_version = MACHINE_CONFIG_SCHEMA_VERSION,
        .payload_size = sizeof(machine_config_t),
        .payload = *cfg,
    };
    blob.crc32 = esp_rom_crc32_le(0, (const uint8_t *)&blob.payload, sizeof(machine_config_t));

    err = nvs_set_blob(handle, "cfg_blob", &blob, sizeof(blob));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS write failed: 0x%x", err);
        nvs_close(handle);
        return err;
    }

    err = nvs_commit(handle);
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed: 0x%x", err);
    }
    return err;
}

/* ---- 公共 API ---- */

esp_err_t machine_config_init(void)
{
    machine_config_t loaded;
    esp_err_t err = load_from_nvs(&loaded);

    if (err == ESP_OK && machine_config_validate(&loaded)) {
        /* Config schema 3 shipped with the 30 ms factory Hall debounce.  It
         * is safe to migrate that untouched factory value to the more
         * responsive 20 ms default while preserving any user-selected value
         * in the supported 20..200 ms range. */
        if (loaded.position.debounce_ms == 30U) {
            loaded.position.debounce_ms = 20U;
            (void)save_to_nvs(&loaded);
            ESP_LOGI(TAG, "Migrated factory Hall debounce 30ms -> 20ms");
        }
        ESP_LOGI(TAG, "Loaded valid config from NVS (schema %"PRIu32")", loaded.schema_version);
        s_config = loaded;
    } else {
        ESP_LOGW(TAG, "NVS config invalid or missing (0x%x), restoring defaults", err);
        s_config = s_default_config;
        err = save_to_nvs(&s_config);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save defaults to NVS: 0x%x", err);
        }
    }

    s_initialized = true;

    /* 异步持久化任务（内部 RAM 栈）：负责 NVS/flash 写，供 BLE rx 等
     * PSRAM 栈任务委托配置保存。栈 8KB 内部 RAM 足够 NVS blob 写。 */
    BaseType_t created = xTaskCreate(config_persist_task_main,
                                     "cfg_persist", CONFIG_PERSIST_TASK_STACK,
                                     NULL, 5, &s_persist_task);
    if (created != pdPASS) {
        ESP_LOGW(TAG, "Failed to create persist task; runtime config saves unavailable");
        s_persist_task = NULL;
    }
    atomic_store_explicit(&s_save_pending, false, memory_order_release);

    return ESP_OK;
}

const machine_config_t *machine_config_get(void)
{
    /*
     * Config is immutable after machine_config_init().
     * Callers must not invoke machine_config_restore_defaults() while
     * any service holds a stale pointer. Phase 1 assumes single-threaded
     * bootstrap; Phase 2+ will add a mutex if runtime restore is needed.
     */
    if (!s_initialized) {
        return &s_default_config;
    }
    return &s_config;
}

esp_err_t machine_config_save_to_nvs(void)
{
    if (!machine_config_validate(&s_config)) {
        ESP_LOGE(TAG, "Refusing to save invalid config");
        return ESP_ERR_INVALID_STATE;
    }
    return save_to_nvs(&s_config);
}

esp_err_t machine_config_set_direction_calibration(bool rpwm_is_cw)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    machine_config_apply_direction_calibration(&s_config, rpwm_is_cw);
    ESP_LOGI(TAG, "Direction calibration set: calibrated=yes rpwm_is_cw=%s",
             rpwm_is_cw ? "yes" : "no");
    return ESP_OK;
}

esp_err_t machine_config_set_bl50_direction_reverse(bool reverse)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    machine_config_apply_bl50_direction_reverse(&s_config, reverse);
    ESP_LOGI(TAG, "BL50 direction reverse set: reverse=%s",
             reverse ? "yes" : "no");
    return ESP_OK;
}

/* 设置板测诊断参数（V4）。参数范围先整体校验，全合法才写入（杜绝半写）。
 * 只改内存，调用方需随后 machine_config_request_persist() 提交到 NVS。 */
esp_err_t machine_config_set_benchtst(const benchtst_params_t *params)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!params) return ESP_ERR_INVALID_ARG;
    if (params->bl50_pwm > 100 || params->ibt2_pwm > 100 ||
        (params->bl50_pwm < 1 && params->bl50_pwm != 0) ||
        (params->ibt2_pwm < 1 && params->ibt2_pwm != 0) ||
        params->act_duration_ms == 0 ||
        params->act_duration_ms > 10000U) {
        return ESP_ERR_INVALID_ARG;
    }
    s_config.benchtst = *params;
    ESP_LOGI(TAG, "benchtst prefs set: bl50=%u ibt2=%u act_dur=%"PRIu32,
             (unsigned)s_config.benchtst.bl50_pwm,
             (unsigned)s_config.benchtst.ibt2_pwm,
             s_config.benchtst.act_duration_ms);
    return ESP_OK;
}

esp_err_t machine_config_reset_benchtst_defaults(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    machine_config_apply_benchtst_defaults(&s_config);
    ESP_LOGI(TAG, "benchtst prefs reset only: bl50=%u ibt2=%u act_dur=%" PRIu32
             " calibration_preserved=%s bl50_reverse_preserved=%s",
             (unsigned)s_config.benchtst.bl50_pwm,
             (unsigned)s_config.benchtst.ibt2_pwm,
             s_config.benchtst.act_duration_ms,
             s_config.position.direction_calibrated ? "yes" : "no",
             s_config.position.bl50_reverse_dir ? "yes" : "no");
    return ESP_OK;
}

static void config_persist_task_main(void *arg)
{
    (void)arg;
    while (1) {
        uint32_t notify = 0;
        xTaskNotifyWait(0, UINT32_MAX, &notify, portMAX_DELAY);
        if (!atomic_exchange_explicit(&s_save_pending, false,
                                      memory_order_acq_rel)) {
            continue;
        }
        esp_err_t err = machine_config_save_to_nvs();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Async config persist failed: 0x%x", err);
        } else {
            ESP_LOGI(TAG, "Config persisted to NVS");
        }
    }
}

/* 请求异步持久化当前配置到 NVS。调用方可来自任意任务（包括 PSRAM 栈的
 * BLE rx）；真正的 nvs/flash 写在本任务（内部 RAM 栈）执行，避免 cache
 * 禁用断言崩溃。 */
esp_err_t machine_config_request_persist(void)
{
    if (!s_initialized || !s_persist_task) {
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store_explicit(&s_save_pending, true, memory_order_release);
    BaseType_t ok = xTaskNotify(s_persist_task, 1, eSetBits);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}

void machine_config_restore_defaults(void)
{
    s_config = s_default_config;
    ESP_LOGI(TAG, "Config restored to compile defaults");
}
