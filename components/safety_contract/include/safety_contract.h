#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "machine_types.h"
#include "wash_contract.h"
#include "xiaojing_hal.h"
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * safety_contract.h — 安全管理器公共类型
 * 定义安全操作、决策、快照、输入快照和停机钩子
 * 不依赖任何具体 service 组件
 * ================================================================ */

/* ---- Safety Operation (高风险输出操作) ---- */

typedef enum {
    SAFETY_OP_POSITION_MOTOR = 0,
    SAFETY_OP_BL50,
    SAFETY_OP_SOURCE_INLET_VALVE,
    SAFETY_OP_TRANSFER_VALVE,
    SAFETY_OP_DRAIN_VALVE,
    SAFETY_OP_DETERGENT_PUMP,
    SAFETY_OP_UV,
    SAFETY_OP_FAN,
    SAFETY_OP_PTC_HEATER,
    SAFETY_OP_HOT_AIR_MODULE,   /* 单继电器耦合热风模块（风扇+加热丝并联） */
} safety_operation_t;

#define SAFETY_OP_COUNT  10

/* ---- Interlock Bit Masks (用于 safety_decision_t.interlock_mask) ---- */

#define SAFETY_ILK_NONE                 0U
#define SAFETY_ILK_POSITION_MISMATCH    (1U << 0)   /* 姿态不符 */
#define SAFETY_ILK_POSITION_UNKNOWN     (1U << 1)   /* 姿态未知 */
#define SAFETY_ILK_POSITION_MOVING      (1U << 2)   /* 姿态电机运动中 */
#define SAFETY_ILK_VALVE_CONFLICT       (1U << 3)   /* 阀门互斥 */
#define SAFETY_ILK_MOTOR_RUNNING        (1U << 4)   /* 其他电机运行中 */
#define SAFETY_ILK_FAN_NOT_RUNNING      (1U << 5)   /* 风扇未运行 */
#define SAFETY_ILK_FAN_PREBLOW_SHORT    (1U << 6)   /* 风扇预吹不足 */
#define SAFETY_ILK_SHT_INVALID          (1U << 7)   /* SHT 无效 */
#define SAFETY_ILK_SHT_STALE            (1U << 8)   /* SHT 样本过期 */
#define SAFETY_ILK_OVERTEMP             (1U << 9)   /* 过温 */
#define SAFETY_ILK_CONFIG_DISABLED      (1U << 10)  /* 配置禁用 */
#define SAFETY_ILK_FAULT_ACTIVE         (1U << 11)  /* 故障锁存中 */
#define SAFETY_ILK_EMERGENCY            (1U << 12)  /* 急停中 */
#define SAFETY_ILK_NOT_INITIALIZED      (1U << 13)  /* 未初始化 */
#define SAFETY_ILK_INVALID_PARAM        (1U << 14)  /* 参数非法 */
#define SAFETY_ILK_POSITION_STALE       (1U << 15)  /* 姿态样本过期 */
#define SAFETY_ILK_OUTPUT_UNKNOWN       (1U << 16)  /* MCP 输出状态未知 */
#define SAFETY_ILK_BL50_RUNNING         (1U << 17)  /* BL50 内筒运行中 */
#define SAFETY_ILK_HOT_AIR_COOLDOWN     (1U << 18)  /* 热风模块冷却锁定中 */
#define SAFETY_ILK_HOT_AIR_ALREADY_ON   (1U << 19)  /* 热风模块已处于 ON */

#define SAFETY_ILK_MAX_BIT             19

/* ---- Safety Reject Reason (授权拒绝原因，与 fault code 语义分离) ---- */

typedef enum {
    SAFETY_REJECT_NONE = 0,
    SAFETY_REJECT_POSITION_MISMATCH,
    SAFETY_REJECT_POSITION_UNKNOWN,
    SAFETY_REJECT_POSITION_MOVING,
    SAFETY_REJECT_VALVE_CONFLICT,
    SAFETY_REJECT_MOTOR_RUNNING,
    SAFETY_REJECT_FAN_NOT_RUNNING,
    SAFETY_REJECT_FAN_PREBLOW_SHORT,
    SAFETY_REJECT_SHT_INVALID,
    SAFETY_REJECT_SHT_STALE,
    SAFETY_REJECT_OVERTEMP,
    SAFETY_REJECT_CONFIG_DISABLED,
    SAFETY_REJECT_FAULT_ACTIVE,
    SAFETY_REJECT_EMERGENCY,
    SAFETY_REJECT_NOT_INITIALIZED,
    SAFETY_REJECT_INVALID_PARAM,
    SAFETY_REJECT_PTC_DURATION_EXCEEDED,
    SAFETY_REJECT_POSITION_STALE,
    SAFETY_REJECT_OUTPUT_UNKNOWN,
    SAFETY_REJECT_HOT_AIR_COOLDOWN_ACTIVE,  /* 冷却锁定中 */
    SAFETY_REJECT_ALREADY_ACTIVE,           /* 目标输出已开启 */
} safety_reject_reason_t;

/* ---- Safety Params (电机/风扇等非布尔操作参数) ---- */

typedef struct {
    uint8_t pwm_percent;   /* 0-100 */
    bool clockwise;        /* 电机方向 */
} safety_motor_params_t;

typedef struct {
    uint8_t percent;       /* 0-100, 0=关闭 */
} safety_fan_params_t;

/* ---- Safety Request ---- */

typedef struct {
    safety_operation_t operation;
    bool enable;
    machine_request_id_t request_id;
    drum_position_t required_position;
    app_source_t source;
    union {
        safety_motor_params_t motor;
        safety_fan_params_t fan;
    } params;
} safety_request_t;

/* ---- Safety Decision (授权结果) ---- */

typedef struct {
    bool allowed;
    safety_reject_reason_t reason;
    machine_fault_code_t fault_code;    /* 仅当 reason 需要锁存故障时非 NONE */
    uint32_t interlock_mask;
} safety_decision_t;

/* ---- MCP Output State (6 路安全输出当前状态) ---- */

typedef struct {
    bool source_inlet_valve;    /* GPA0 */
    bool transfer_valve;        /* GPA1 */
    bool uv;                    /* GPA2 */
    bool drain_valve;           /* GPA3 */
    bool ptc_heater;            /* GPA7 */
    bool detergent_pump;        /* GPB7 */
} safety_mcp_output_state_t;

/* ---- Safety Inputs (纯策略函数的全部输入快照) ---- */

typedef struct {
    /* 姿态 */
    drum_position_t position;
    bool position_stable;
    bool position_motor_moving;
    bool position_sample_valid;         /* 姿态样本是否有效 */
    uint32_t position_sample_ms;        /* 姿态采样时间 (ms), 0=未采样 */

    /* MCP 输出当前状态 */
    safety_mcp_output_state_t mcp_outputs;
    uint32_t mcp_known_mask;            /* 每 bit 对应 safe_output_t, 1=known, 0=unknown */

    /* 风扇 */
    uint8_t fan_percent;
    bool fan_running;
    int64_t fan_running_since_ms;   /* 风扇开始运行的时间，0=未记录 */

    /* SHT */
    sht_sample_t sht_sample;
    bool sht_valid;
    bool sht_fresh;                 /* sample age < stale_timeout */

    /* 水位 */
    bool water_full;
    bool water_level_valid;

    /* BL50 内筒状态 (三态: UNKNOWN/STOPPED/RUNNING) */
    bool bl50_state_known;              /* false=UNKNOWN, true=STOPPED or RUNNING */
    bool bl50_running;                  /* true=RUNNING, false=STOPPED (仅 bl50_state_known=true 时有意义) */

    /* 故障 */
    bool fault_active;
    bool emergency_stop_active;

    /* 配置 */
    bool ptc_enabled_in_config;
    xiaojing_output_mode_t output_mode;

    /* 时间 */
    int64_t now_ms;

    /* PTC 运行状态（由 manager 内部维护） */
    bool ptc_on;
    int64_t ptc_on_since_ms;        /* PTC 开启时间 */

    /* 耦合热风模块运行状态（由 manager 内部维护）。
     * hot_air_on 是命令态（继电器已请求闭合的闩锁），绝不代表物理风量/转速。 */
    bool hot_air_on;
    int64_t hot_air_on_since_ms;    /* 热风模块开启时间 */
    int64_t hot_air_cooldown_until_ms; /* 冷却锁截止时刻；now < 该值 ⇒ COOLDOWN 拒绝 */
} safety_inputs_t;

/* ---- Safety Snapshot ---- */

typedef struct {
    bool fault_active;
    machine_fault_t primary_fault;
    uint64_t active_fault_mask;
    uint64_t allowed_operation_mask;
    bool emergency_stop_active;
    bool water_outputs_safe;
    bool heater_safe;
    uint32_t mcp_known_mask;            /* 诊断: 当前已知输出掩码 */
    uint32_t mcp_unknown_mask;          /* 诊断: 当前未知输出掩码 */
    /* 当前 MCP 输出实际状态（来源：成功写后跟踪 + 启动安全基线写 OFF）。
     * 诊断/UI 用真实 HAL 状态，而不是从 known 位推断。 */
    safety_mcp_output_state_t mcp_outputs;
    bool bl50_state_known;              /* BL50 状态已知 */
    bool bl50_running;                  /* BL50 运行中 */
    /* 耦合热风模块（命令态，非物理风量证明） */
    bool hot_air_on;
    int64_t hot_air_cooldown_until_ms;  /* 冷却锁截止；0 或过去 = 无锁 */
    uint32_t revision;
} safety_snapshot_t;

/* ---- Stop Hook (非阻塞停机回调) ---- */

typedef esp_err_t (*safety_stop_hook_t)(void *context);

#ifdef __cplusplus
}
#endif
