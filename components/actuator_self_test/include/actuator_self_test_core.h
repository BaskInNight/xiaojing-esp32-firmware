#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "machine_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * actuator_self_test_core.h — 执行器板测核心（纯 C，无 FreeRTOS/服务依赖）
 *
 * 板级诊断命令 actuator_self_test 的安全门禁、固定时长、request_id 分配
 * 与终态语义。三个目标：
 *   source_valve    — 有压进水阀 (GPA0, SAFETY_OP_SOURCE_INLET_VALVE)
 *   transfer_valve  — 零压转移阀 (GPA1, SAFETY_OP_TRANSFER_VALVE)
 *   detergent_pump  — 洗涤剂蠕动泵 (GPB7, SAFETY_OP_DETERGENT_PUMP)
 * 时长由固件固定，客户端不可覆盖；到期由设备端（对应 service）自动 OFF。
 * 运行时接线见 actuator_self_test.h。
 * ================================================================ */

#define ACT_SELF_TEST_SOURCE_VALVE_DURATION_MS   1500U
#define ACT_SELF_TEST_TRANSFER_VALVE_DURATION_MS 1500U
#define ACT_SELF_TEST_DETERGENT_PUMP_DURATION_MS 800U
#define ACT_SELF_TEST_DRAIN_PUMP_DURATION_MS     2000U
#define ACT_SELF_TEST_FAN_DURATION_MS            5000U
#define ACT_SELF_TEST_HOT_AIR_DURATION_MS        1000U   /* 首轮实测默认 1s */
#define ACT_SELF_TEST_HOT_AIR_MAX_DURATION_MS    3000U   /* 硬上限 */

/* ---- 目标 ---- */

typedef enum {
    ACT_TARGET_NONE = 0,
    ACT_TARGET_SOURCE_VALVE,
    ACT_TARGET_TRANSFER_VALVE,
    ACT_TARGET_DETERGENT_PUMP,
    ACT_TARGET_DRAIN_PUMP,
    ACT_TARGET_FAN,
    ACT_TARGET_HOT_AIR_COUPLED,   /* 单继电器耦合热风模块（GPA7 继电器） */
    ACT_TARGET_COUNT,
} actuator_self_test_target_t;

/* ---- 状态（向状态订阅端暴露） ---- */

typedef enum {
    ACT_STATE_INACTIVE = 0,    /* 无板测 */
    ACT_STATE_CHECKING,        /* 已受理，等待服务校验/点灯确认 */
    ACT_STATE_ACCEPTED,        /* 已受理待运行 */
    ACT_STATE_RUNNING,         /* 输出已开 */
    ACT_STATE_COMPLETE,        /* 正常完成，输出已确认 OFF */
    ACT_STATE_REJECTED,        /* 门禁拒绝 */
    ACT_STATE_INTERRUPTED,     /* 被中断（紧急/断线） */
    ACT_STATE_TIMEOUT,         /* 超时终态 */
    ACT_STATE_FAULT,           /* 故障（含 OFF 失败） */
    ACT_STATE_UNKNOWN,         /* 状态未知 */
} actuator_self_test_state_t;

/* ---- 终态 ---- */

typedef enum {
    ACT_TERMINAL_NONE = 0,
    ACT_TERMINAL_COMPLETE,
    ACT_TERMINAL_REJECTED,
    ACT_TERMINAL_INTERRUPTED,
    ACT_TERMINAL_TIMEOUT,
    ACT_TERMINAL_FAULT,
    ACT_TERMINAL_UNKNOWN,
} actuator_self_test_terminal_t;

/* ---- 门禁结果（机器可读拒绝码） ---- */

typedef enum {
    ACT_GATE_OK = 0,
    ACT_GATE_EXECUTOR_BUSY,        /* EXECUTOR_BUSY */
    ACT_GATE_SELF_TEST_BUSY,       /* SELF_TEST_BUSY */
    ACT_GATE_FAULT_ACTIVE,         /* FAULT_ACTIVE */
    ACT_GATE_EMERGENCY_ACTIVE,     /* EMERGENCY_ACTIVE */
    ACT_GATE_POSITION_UNKNOWN,     /* POSITION_UNKNOWN */
    ACT_GATE_POSITION_STALE,       /* POSITION_STALE */
    ACT_GATE_POSITION_UNSTABLE,    /* POSITION_UNSTABLE */
    ACT_GATE_MOTOR_MOVING,         /* MOTOR_MOVING */
    ACT_GATE_POSITION_NOT_ZERO,    /* POSITION_NOT_ZERO */
    ACT_GATE_OUTPUT_STATE_UNKNOWN, /* OUTPUT_STATE_UNKNOWN */
    ACT_GATE_OUTPUT_ALREADY_ON,    /* OUTPUT_ALREADY_ON */
    ACT_GATE_CONFLICT_OUTPUT_ON,   /* CONFLICT_OUTPUT_ON */
    ACT_GATE_SERVICE_BUSY,         /* SERVICE_BUSY */
    ACT_GATE_REQUEST_ID_INVALID,   /* REQUEST_ID_INVALID */
    ACT_GATE_WATER_SNAPSHOT_UNAVAILABLE,      /* WATER_SNAPSHOT_UNAVAILABLE */
    ACT_GATE_DETERGENT_SNAPSHOT_UNAVAILABLE,  /* DETERGENT_SNAPSHOT_UNAVAILABLE */
    ACT_GATE_DRAIN_SNAPSHOT_UNAVAILABLE,      /* DRAIN_SNAPSHOT_UNAVAILABLE */
    ACT_GATE_DRY_SNAPSHOT_UNAVAILABLE,        /* DRY_SNAPSHOT_UNAVAILABLE */
    ACT_GATE_HOT_AIR_SNAPSHOT_UNAVAILABLE,    /* HOT_AIR_SNAPSHOT_UNAVAILABLE */
    ACT_GATE_HOT_AIR_COOLDOWN_ACTIVE,         /* HOT_AIR_COOLDOWN_ACTIVE */
    ACT_GATE_HOT_AIR_TEMP_TOO_HIGH,           /* HOT_AIR_TEMP_TOO_HIGH */
} actuator_self_test_gate_result_t;

/* ---- 门禁输入（由运行时从各服务快照采集） ---- */

typedef struct {
    actuator_self_test_target_t target;  /* 正在校验的目标 */
    bool exec_idle;              /* executor IDLE 且无程序/无保留 terminal */
    bool self_test_busy;         /* 本组件或 UV 自检正在运行 */
    bool fault_active;           /* safety fault 锁存 */
    bool emergency_active;       /* emergency 激活 */
    bool pos_valid;              /* 位置样本存在 */
    bool pos_fresh;              /* 位置样本新鲜 */
    bool pos_stable;             /* 位置稳定 */
    bool pos_moving;             /* 姿态电机运动 */
    drum_position_t pos;         /* 当前位置 */
    drum_position_t required_pos;/* 目标所需桶位（source/transfer/det=0, drain=180, fan=270） */
    bool mcp_target_known;       /* 目标 MCP 输出 known（fan 用 dry fan_state!=UNKNOWN） */
    bool water_safe_off_synced;  /* water 初始安全 OFF 同步已到终态（HW-FIX-1） */
    bool target_confirmed_off;   /* 目标输出确认 OFF */
    bool conflict_confirmed_off; /* 互斥输出确认 OFF（source↔transfer、二者↔drain） */
    bool water_service_idle;     /* water_service FSM 空闲 */
    bool detergent_service_idle; /* detergent_service 空闲 */
    bool drain_service_idle;     /* drain_service FSM 空闲 */
    bool dry_service_idle;       /* dry_service FSM 空闲（fan/heater 路径） */
    bool fan_confirmed_off;      /* 风扇输出 KNOWN_OFF */
    bool heater_confirmed_off;   /* 加热器输出 KNOWN_OFF */
    bool sht_valid_fresh;        /* 温度样本有效且新鲜（fan/heater 前置） */
    bool sht_below_cutoff;       /* 温度低于安全阈值（fan/heater 前置） */
    /* 耦合热风模块（GPA7 继电器，风扇+加热丝并联同步启停） */
    bool hot_air_service_idle;   /* coupled_hot_air_service 空闲且无保留 terminal */
    bool hot_air_confirmed_off;  /* 继电器输出 KNOWN_OFF（命令态） */
    bool hot_air_cooldown_active;/* 冷却锁定中（HOT_AIR_COOLDOWN_ACTIVE） */
    bool request_id_ok;          /* request_id 非 0 且单调 */
    actuator_self_test_gate_result_t snapshot_fail; /* 所需服务快照不可用时置为对应
                                       UNAVAILABLE 拒绝码（P1-2，fail-closed）；
                                       ACT_GATE_OK 表示无快照失败。 */
} actuator_self_test_gate_input_t;

/* ---- 请求 ---- */

typedef struct {
    actuator_self_test_target_t target;
    machine_request_id_t request_id;
    uint32_t duration_ms;        /* 固件固定，客户端不可覆盖 */
} actuator_self_test_request_t;

/* ---- 纯函数 ---- */

/* 目标 → 固定时长（ms）。非法目标返回 0。 */
uint32_t actuator_self_test_duration_ms(actuator_self_test_target_t target);

/* 目标 → 协议字符串（"source_valve" 等）。 */
const char *actuator_self_test_target_name(actuator_self_test_target_t target);

/* 协议字符串 → 目标。未知返回 false。 */
bool actuator_self_test_target_from_name(const char *name,
                                         actuator_self_test_target_t *out);

/* 纯门禁判定：返回 ACT_GATE_OK 或具体拒绝码（无副作用）。 */
actuator_self_test_gate_result_t actuator_self_test_gate_check(
    const actuator_self_test_gate_input_t *in);

/* 拒绝码 → machine-readable 字符串（供 BLE ack code）。 */
const char *actuator_self_test_gate_code(actuator_self_test_gate_result_t result);

/* 单调 request_id 分配：非 0、wrap 后跳过 0。 */
machine_request_id_t actuator_self_test_next_request_id(machine_request_id_t current);

/* 状态 → 字符串（供日志/状态消息）。 */
const char *actuator_self_test_state_name(actuator_self_test_state_t state);

/* ---- fail-closed OFF 证明 ---- */

typedef struct {
    bool water_snapshot_ok;     /* water_service_get_snapshot 成功 */
    bool source_off;            /* source_valve_on == false */
    bool source_unknown;        /* source_valve_unknown == false */
    bool transfer_off;          /* transfer_valve_on == false */
    bool transfer_unknown;      /* transfer_valve_unknown == false */
    bool detergent_snapshot_ok; /* detergent_service_get_snapshot 成功 */
    bool detergent_off;         /* output_state == KNOWN_OFF */
    bool drain_snapshot_ok;     /* drain_service_get_snapshot 成功 */
    bool drain_off;             /* drain output_state == KNOWN_OFF */
    bool dry_snapshot_ok;       /* dry_service_get_snapshot 成功 */
    bool fan_off;               /* fan_output_state == KNOWN_OFF */
    bool heater_off;            /* heater_output_state == KNOWN_OFF */
    bool hot_air_snapshot_ok;   /* coupled_hot_air_service_get_snapshot 成功 */
    bool hot_air_off;           /* coupled 继电器 output_confirmed_off == true */
} actuator_off_proof_t;

/* Fail-closed OFF 确认：仅当两个快照都成功且所有输出明确 KNOWN_OFF 才返回
 * true。任何快照失败/超时或输出 UNKNOWN 都返回 false（"无法证明 OFF"），
 * stop 不得把"无法证明 OFF"当作"已关闭"。 */
bool actuator_outputs_confirmed_off(const actuator_off_proof_t *p);

#ifdef __cplusplus
}
#endif
