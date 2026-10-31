#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "machine_types.h"
#include "wash_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * machine_config.h — 校准参数配置
 * 编译默认值 → NVS 版本化覆盖 → 校验 → 无效时恢复默认
 * NVS blob 封装: magic/schema_version/payload_size/crc32/payload
 * ================================================================ */

#define MACHINE_CONFIG_SCHEMA_VERSION  4            /* V4: +benchtst_params */
#define MACHINE_CONFIG_NVS_NAMESPACE   "xiaojing"
#define MACHINE_CONFIG_NVS_MAGIC       0x584A4346  /* "XJCF" */

/* ---- 姿态定位参数 ---- */

typedef struct {
    position_direction_policy_t default_policy;
    uint8_t move_pwm_percent;       /* 5-100 */
    uint8_t approach_pwm_percent;   /* 接近目标时降速 */
    uint32_t debounce_ms;           /* 霍尔消抖，20-200 */
    uint32_t move_timeout_ms;       /* 移动超时 */
    uint32_t brake_ms;              /* 制动时间 */
    bool rpwm_is_cw;                /* IBT-2 方向标定 */
    bool direction_calibrated;
    bool bl50_reverse_dir;          /* BL50 无刷方向翻转：DIR 极性取反，
                                     使软件 CW 命令=物理 CW（相序/机械安装修正） */
} position_params_t;

/* ---- 进水参数 ---- */

typedef struct {
    float pulses_per_liter;          /* 0=未标定, >=0, isfinite */
    uint32_t no_flow_timeout_ms;     /* 无流超时 */
    uint32_t low_flow_window_ms;     /* 低流检测窗口 */
    uint32_t source_batch_target_pulses;  /* 单批目标脉冲 */
    uint32_t source_batch_max_ms;    /* 单批最大时间 */
    uint32_t source_settle_ms;       /* 源箱沉降时间 */
    uint32_t transfer_timeout_ms;    /* 转移超时 */
    uint32_t default_transfer_ms;    /* 默认转移时间 */
    uint16_t max_fill_cycles;        /* 最大批次 */
    uint32_t total_inlet_timeout_ms; /* 进水总超时 */
} water_params_t;

/* ---- 烘干参数 ---- */

typedef struct {
    uint32_t max_total_ms;           /* 累计最大烘干 */
    uint32_t pre_fan_ms;             /* 预吹时间 */
    uint32_t heat_on_max_ms;         /* 单次最大加热 */
    uint32_t heat_off_min_ms;        /* 最小停止加热 */
    uint32_t cooldown_ms;            /* 冷却时间 */
    float heater_cutoff_c;           /* 关断温度, isfinite */
    float heater_resume_c;           /* 恢复温度, isfinite */
    uint8_t fan_percent;             /* 烘干风速 0-100 */
    uint32_t sht_stale_timeout_ms;   /* SHT 过期超时 */
} dry_params_t;

/* ---- 洗衣液参数 ---- */

typedef struct {
    uint32_t demo_duration_ms;       /* Demo 默认投液时间 */
    uint32_t formal_duration_ms;     /* Formal 默认投液时间 */
    uint32_t max_single_ms;          /* 单次最大 */
    float ml_per_second;             /* 0=未标定, >=0, isfinite */
} detergent_params_t;

/* ---- UV 参数 ---- */

typedef struct {
    uint32_t default_duration_ms;    /* 默认 UV 时间 */
    uint32_t max_duration_ms;        /* 单次最大 */
} uv_params_t;

/* ---- 板测诊断参数（V4；仅开发测试入口消费，不改变生产洗涤语义） ---- */

typedef struct {
    uint8_t  bl50_pwm;             /* 搅动电机(BL50)台架脉冲 PWM 0-100 */
    uint8_t  ibt2_pwm;            /* 换位电机(IBT-2)测试 PWM 0-100 */
    uint32_t act_duration_ms;     /* 执行器(泵/阀/热风)板测时长 0-10000ms */
} benchtst_params_t;

/* ---- 排水参数 ---- */

typedef struct {
    uint32_t max_duration_ms;        /* 单次最大排水时间 */
} drain_params_t;

/* ---- 综合配置 ---- */

typedef struct {
    uint32_t schema_version;         /* 必须等于 MACHINE_CONFIG_SCHEMA_VERSION */
    position_params_t position;
    water_params_t water;
    dry_params_t dry;
    detergent_params_t detergent;
    uv_params_t uv;
    drain_params_t drain;
    benchtst_params_t benchtst;     /* V4+ */
} machine_config_t;

/* ---- NVS Blob 封装 ---- */

typedef struct {
    uint32_t magic;                  /* MACHINE_CONFIG_NVS_MAGIC */
    uint32_t schema_version;
    uint32_t payload_size;           /* sizeof(machine_config_t) */
    uint32_t crc32;                  /* CRC32 of payload bytes */
    machine_config_t payload;
} machine_config_nvs_blob_t;

/* ---- API ---- */

/* 初始化配置：从 NVS 加载，版本不匹配或 CRC 错误时恢复默认 */
esp_err_t machine_config_init(void);

/* 获取当前配置（指针有效直到下次 init/save） */
const machine_config_t *machine_config_get(void);

/* 运行时方向标定（开发/板级诊断）：置 direction_calibrated=true 并按
 * rpwm_is_cw 记录 PWM 通道方向。仅修改内存配置；调用方需随后调用
 * machine_config_save_to_nvs() 持久化。BL50 服务持实时配置指针，
 * 标定后下一命令立即生效（无需重启）。 */
esp_err_t machine_config_set_direction_calibration(bool rpwm_is_cw);

/* 设置 BL50 无刷方向翻转（开发/板级诊断）：置 bl50_reverse_dir，仅修改
 * 内存配置；调用方需随后 machine_config_save_to_nvs()/request_persist()。
 * BL50 驱动实时读取（下次 run 生效）。 */
esp_err_t machine_config_set_bl50_direction_reverse(bool reverse);

/* 设置板测诊断参数（V4）：bl50_pwm / ibt2_pwm / act_duration_ms。
 * 仅改内存，调用方需随后 request_persist() 持久化。参数会在下次
 * 台架脉冲 / 换向 / 换位 / 执行器自检时立即生效。返回值：
 * - ESP_OK：全部字段已写入；
 * - ESP_ERR_INVALID_ARG：任一字段超范围。 */
esp_err_t machine_config_set_benchtst(const benchtst_params_t *params);

/* 仅恢复板测诊断参数默认值，不得修改方向标定、生产参数或其他配置。 */
esp_err_t machine_config_reset_benchtst_defaults(void);

/* 请求异步持久化当前配置到 NVS。可在任意任务上下文调用（含 PSRAM 栈的
 * BLE rx 任务——该任务直接做 nvs/flash 写会触发 cache 禁用断言崩溃）；
 * 真正的 flash 写由内部 RAM 栈的专用任务执行。 */
esp_err_t machine_config_request_persist(void);

/* 将当前配置保存到 NVS */
esp_err_t machine_config_save_to_nvs(void);

/* 恢复编译默认值（不自动保存到 NVS） */
void machine_config_restore_defaults(void);

/* 校验参数上下限，返回 true 表示全部合法 */
bool machine_config_validate(const machine_config_t *cfg);

/* 应用方向标定到配置（纯 C，无 NVS/FreeRTOS 依赖，固件与 host test 共享）：
 * 置 direction_calibrated=true 并按 rpwm_is_cw 记录 PWM 通道方向。
 * 不保存 NVS（调用方负责 machine_config_save_to_nvs()）。 */
void machine_config_apply_direction_calibration(machine_config_t *cfg,
                                                bool rpwm_is_cw);

/* 应用 BL50 无刷方向翻转（纯 C）：置 bl50_reverse_dir，使软件 CW 命令
 * =物理 CW。不保存 NVS。 */
void machine_config_apply_bl50_direction_reverse(machine_config_t *cfg,
                                                 bool reverse);

/* 仅将 benchtst 子结构恢复为编译默认值（纯 C，供固件与 host test 共享）。 */
void machine_config_apply_benchtst_defaults(machine_config_t *cfg);

#ifdef __cplusplus
}
#endif
