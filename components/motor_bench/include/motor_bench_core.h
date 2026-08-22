/*
 * motor_bench_core.h — 电机空载台架 纯 C 核心（无 FreeRTOS/HAL/服务依赖）
 *
 * 诊断模式专用「电机空载台架测试」：
 *   1. pulsator_motor_self_test    主洗涤/波轮独立自检（单步短脉冲）
 *   2. drum_motor_self_test        滚筒/BLDC 独立自检（单步短脉冲）
 *   3. motor_hall_bench_sequence   人工霍尔流程台架（多步交互）
 *
 * 安全语义（与生产洗涤流程同源）：
 *   - 每步所需桶位由生产契约 bl50_action_to_position() 派生（非硬编码猜测）；
 *   - 每步仅在用户手动摆放到目标位置且稳定后，经用户显式确认，
 *     才允许提交一次低能量短脉冲（低档 ≤300ms）到真实 bl50_service；
 *   - 每步脉冲后必须确认 BL50 回 IDLE 且 PWM=0（输出确认关闭）才推进；
 *   - 错误位置/多霍尔矛盾/位置超时/快照不可用/输出确认失败均 fail-closed，
 *     不推进步骤；start/cancel/timeout/exception/BLE/emergency 全输出 OFF。
 *
 * 本文件为 host-testable 纯逻辑：门禁 + 多步 FSM。运行时组件见
 * motor_bench_service.h。生产默认洗涤订单绝不调用本测试。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "machine_types.h"
#include "bl50_service.h"   /* bl50_action_t（类型共享，单一来源） */

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * 台架测试类型
 * ================================================================ */

typedef enum {
    MOTOR_BENCH_TYPE_NONE = 0,
    MOTOR_BENCH_TYPE_PULSATOR,        /* 主洗涤/波轮独立自检（单步） */
    MOTOR_BENCH_TYPE_DRUM,            /* 滚筒/BLDC 独立自检（单步） */
    MOTOR_BENCH_TYPE_HALL_SEQUENCE,   /* 人工霍尔流程台架（多步交互） */
    MOTOR_BENCH_TYPE_PULSATOR_BEHAVIOR, /* 波轮行为诊断：确认后按生产波轮
                                        逻辑转-刹-反向交替 ~15s（空载目视） */
} motor_bench_type_t;

const char *motor_bench_type_name(motor_bench_type_t type);
bool motor_bench_type_from_name(const char *name, motor_bench_type_t *out);

/* ================================================================
 * 台架程序（有序步骤）
 * ================================================================ */

#define MOTOR_BENCH_MAX_STEPS 8U

typedef struct {
    bl50_action_t action;             /* 生产动作（PULSATOR/DRUM/SPIN/HOME） */
    drum_position_t required_position; /* 生产位置语义（bl50_action_to_position） */
} motor_bench_step_t;

/* 默认程序构建：把类型展开为动作序列，再用位置解析器派生所需位置。
 * resolver 由服务层传入生产契约 bl50_service_action_to_position()；
 * 禁止在 core 内硬编码位置表。失败（类型无效/步数超限）返回 false。 */
typedef drum_position_t (*motor_bench_position_resolver_t)(bl50_action_t action);
bool motor_bench_build_default_program(motor_bench_type_t type,
                                       motor_bench_position_resolver_t resolver,
                                       motor_bench_step_t *out_steps,
                                       uint8_t *out_count);

/* 步数（单步自检=1，人工霍尔台架=3） */
uint8_t motor_bench_type_step_count(motor_bench_type_t type);

/* ================================================================
 * 门禁（fail-closed）
 * ================================================================ */

typedef enum {
    MOTOR_BENCH_GATE_OK = 0,
    MOTOR_BENCH_GATE_EXECUTOR_BUSY,        /* 正式洗涤程序运行中 */
    MOTOR_BENCH_GATE_SELF_TEST_BUSY,       /* 其他自检/板测运行中 */
    MOTOR_BENCH_GATE_FAULT_ACTIVE,
    MOTOR_BENCH_GATE_EMERGENCY_ACTIVE,
    MOTOR_BENCH_GATE_DIRECTION_NOT_CALIBRATED, /* BL50 方向未完成实物标定 */
    MOTOR_BENCH_GATE_POSITION_UNKNOWN,
    MOTOR_BENCH_GATE_POSITION_STALE,
    MOTOR_BENCH_GATE_POSITION_UNSTABLE,
    MOTOR_BENCH_GATE_MOTOR_MOVING,         /* 姿态电机仍运动 */
    MOTOR_BENCH_GATE_POSITION_MISMATCH,    /* 桶位 != 所需位置（错误霍尔） */
    MOTOR_BENCH_GATE_HALL_CONFLICT,        /* 多霍尔同时触发 */
    MOTOR_BENCH_GATE_OUTPUT_STATE_UNKNOWN, /* 输出快照未知/未同步 */
    MOTOR_BENCH_GATE_FORBIDDEN_OUTPUT_ON,  /* 进水/转移/排水/蠕动/UV/热风 输出 ON */
    MOTOR_BENCH_GATE_BL50_BUSY,            /* BL50 非 IDLE 或 PWM>0 */
    MOTOR_BENCH_GATE_POSITION_SERVICE_BUSY,
    MOTOR_BENCH_GATE_SERVICE_BUSY,         /* water/detergent/drain/dry/hot_air busy */
    MOTOR_BENCH_GATE_REQUEST_ID_INVALID,
    MOTOR_BENCH_GATE_SNAPSHOT_UNAVAILABLE, /* 任一关键服务快照不可用 */
} motor_bench_gate_result_t;

typedef struct {
    motor_bench_type_t type;
    drum_position_t required_pos;    /* 当前步所需位置 */
    /* 位置视图 */
    bool pos_valid;
    bool pos_fresh;
    bool pos_stable;
    bool pos_moving;                 /* 姿态电机运动 */
    drum_position_t pos;
    uint8_t raw_hall_mask;           /* 未防抖原始霍尔掩码（bit0-4=五路） */
    uint8_t stable_hall_mask;        /* 防抖后霍尔掩码 */
    bool hall_conflict;              /* 多霍尔同时触发 */
    /* 安全 */
    bool fault_active;
    bool emergency_active;
    bool direction_calibrated;
    /* 业务互斥 */
    bool exec_idle;
    bool self_test_busy;
    /* 电机 */
    bool bl50_idle;
    uint8_t bl50_pwm;
    bool bl50_fault;                 /* BL50 快照 state == FAULT（FSM 脉冲/关断确认用） */
    bool position_service_idle;
    bool position_moving;            /* position_service 内部运动标志 */
    /* 输出确认（禁止输出全部 KNOWN_OFF 且互斥服务空闲） */
    bool forbidden_outputs_off;      /* 水阀/转移/排水/蠕动/UV/热风/烘干 全 OFF */
    bool water_service_idle;
    bool detergent_service_idle;
    bool drain_service_idle;
    bool dry_service_idle;
    bool hot_air_service_idle;
    bool water_safe_off_synced;
    bool request_id_ok;
    bool snapshot_fail;              /* fail-closed：任一关键快照读取失败 */
} motor_bench_gate_input_t;

motor_bench_gate_result_t motor_bench_gate_check(const motor_bench_gate_input_t *in);
const char *motor_bench_gate_code(motor_bench_gate_result_t result);

/* ================================================================
 * 禁止输出确认（host-testable 纯逻辑，fail-closed）
 * ================================================================ */

/* forbidden_outputs_off 的聚合子句。任一子句 false（输出 ON 或状态未知）
 * → 整体 false。基值语义必须为 true（若从 false 起 AND，结果恒 false，
 * 台架将永远无法受理 —— 曾作为 P0 缺陷真实发生）。
 * uv_clause_applicable=false（fault/emergency 激活时）跳过 UV 子句。 */
typedef struct {
    bool water_valves_off;     /* 进水阀 + 转移阀均 KNOWN_OFF */
    bool detergent_off;        /* 蠕动泵输出 KNOWN_OFF */
    bool drain_off;            /* 排水泵输出 KNOWN_OFF */
    bool dry_off;              /* 烘干风扇 + 加热器均 KNOWN_OFF */
    bool hot_air_off;          /* 耦合热风 KNOWN_OFF 且已确认关闭 */
    bool uv_clause_applicable; /* fault/emergency 激活时不应用 UV 子句 */
    bool uv_off;               /* UV MCP 输出 KNOWN_OFF */
} motor_bench_forbidden_clauses_t;

bool motor_bench_forbidden_outputs_off(const motor_bench_forbidden_clauses_t *clauses);

/* self_test_busy 的聚合（host-testable，契约固定）：
 * 台架的"自检忙"仅来自**外部**自检活动（UV 自检 / 执行器板测 任一运行）。
 * 台架自身 test_active 刻意**不参与**：受理后 test_active 置 true，若计入则
 * 门禁每 tick 以 SELF_TEST_BUSY 自锁，台架"一受理即 FAULT"（P0：板测发现，
 * 台架完全无法运行）。重入保护由服务层 request() 顶部的 test_active 检查独占。
 * 参数名即契约：本函数签名**不接受**台架自身活动标志。 */
bool motor_bench_external_self_test_busy(bool uv_selftest_busy,
                                         bool actuator_selftest_busy);

/* ================================================================
 * 多步 FSM（纯逻辑，host-testable）
 * ================================================================ */

typedef enum {
    MOTOR_BENCH_STATE_INACTIVE = 0,
    MOTOR_BENCH_STATE_PRECHECK,        /* 受理后全量门禁复核 */
    MOTOR_BENCH_STATE_WAIT_POSITION,   /* 等待用户手动摆放桶位 */
    MOTOR_BENCH_STATE_WAIT_CONFIRM,    /* 位置就位，等待用户确认脉冲 */
    MOTOR_BENCH_STATE_PULSING,         /* BL50 短脉冲运行中 */
    MOTOR_BENCH_STATE_OFF_CONFIRM,     /* 脉冲后确认 BL50 已 OFF */
    MOTOR_BENCH_STATE_COMPLETE,
    MOTOR_BENCH_STATE_REJECTED,
    MOTOR_BENCH_STATE_INTERRUPTED,
    MOTOR_BENCH_STATE_TIMEOUT,
    MOTOR_BENCH_STATE_FAULT,
} motor_bench_state_t;

const char *motor_bench_state_name(motor_bench_state_t state);

typedef enum {
    MOTOR_BENCH_TERMINAL_NONE = 0,
    MOTOR_BENCH_TERMINAL_COMPLETE,
    MOTOR_BENCH_TERMINAL_REJECTED,
    MOTOR_BENCH_TERMINAL_INTERRUPTED,
    MOTOR_BENCH_TERMINAL_TIMEOUT,
    MOTOR_BENCH_TERMINAL_FAULT,
} motor_bench_terminal_t;

typedef struct {
    motor_bench_type_t type;
    motor_bench_step_t steps[MOTOR_BENCH_MAX_STEPS];
    uint8_t step_count;
    uint8_t current_step;          /* 0-based */
    motor_bench_state_t state;
    motor_bench_terminal_t terminal;
    machine_request_id_t request_id;
    const char *last_code;         /* 拒绝/等待原因/状态码（只读展示） */
    bool output_confirmed_off;
    /* 内部时间戳（wrap-safe uint32） */
    uint32_t step_enter_ms;        /* 当前步进入时刻（位置等待/确认超时） */
    uint32_t pulse_issued_ms;      /* 当前步脉冲提交时刻 */
    /* 内部标志 */
    bool pulse_issued;             /* 当前步已提交脉冲 */
    bool pulse_seen_running;       /* BL50 已被观察到非 IDLE（受理确认） */
    bool confirm_consumed;         /* 当前步用户确认已消费 */
} motor_bench_fsm_ctx_t;

typedef struct {
    uint32_t now_ms;
    motor_bench_gate_result_t gate;   /* 服务层按当前步所需位置计算的门禁结果 */
    bool bl50_idle;
    uint8_t bl50_pwm;
    bool bl50_fault;                  /* BL50 快照 state == FAULT */
    bool submit_failed;               /* 脉冲提交失败（服务层捕获 run_async 错误） */
    bool forbidden_outputs_off;       /* 禁止输出全 OFF 不变式（每 tick 复核） */
    bool user_confirm_pulse;          /* 用户显式确认本步脉冲 */
    bool cancel_requested;            /* 用户取消/abort */
    bool emergency;                   /* BLE 断线/急停闩锁 */
} motor_bench_fsm_input_t;

typedef struct {
    bool submit_pulse;                /* 服务层提交一步脉冲到 bl50_service */
    bool request_all_off;             /* 服务层请求全输出 OFF（终态前） */
    bool terminal;                    /* 终态事件 exactly-once */
    motor_bench_terminal_t terminal_kind;
    const char *terminal_code;
} motor_bench_fsm_output_t;

/* ---- 常量（低能量短脉冲）---- */

/* ZS-X11B 有霍尔台架单方向测试时长。BL50 FSM 每 100ms 仅升 5%，因此
 * 1500ms 在 60% 目标下约有 1200ms 消耗在软启动斜坡，只剩约 300ms
 * 稳态时间。板测延长至 5s，以便轮毂电机完成起动并观察相序。 */
#define MOTOR_BENCH_PULSE_DURATION_MS       5000U
/* 波轮脉冲 PWM 对齐生产 PULSATOR 档（wash_executor BL50_PWM_DEFAULT_PULSATOR=40）。
 * 早期 15% 扭矩不足以驱动空载波轮可见转动，无法目视验证方向；40%/300ms
 * 仍是低能量短脉冲（时长上限即能量约束），且由用户逐次手动确认触发。 */
#define MOTOR_BENCH_PULSE_PWM_PERCENT       40U    /* 低档（=生产 PULSATOR 档） */
#define MOTOR_BENCH_POSITION_WAIT_TIMEOUT_MS 120000U /* 每步等待位置上限（用户手动） */
#define MOTOR_BENCH_CONFIRM_TIMEOUT_MS      20000U  /* 位置就位后等待确认上限 */
#define MOTOR_BENCH_PULSE_TIMEOUT_MS        8000U   /* 5s运行 + 减速/停止完成宽限 */
#define MOTOR_BENCH_PULSE_ACCEPT_GRACE_MS   300U    /* 提交后 BL50 受理宽限 */
#define MOTOR_BENCH_OFF_CONFIRM_TIMEOUT_MS  2000U   /* BL50 回 IDLE 后确认 OFF 宽限 */

/* 波轮行为诊断（pulsator_behavior）：确认后按生产波轮逻辑转-刹-反向交替。
 * 时长/换向间隔对齐生产 PULSATOR 参数；空载 + 用户目视 + fail-closed。 */
#define MOTOR_BENCH_BEHAVIOR_DURATION_MS     15000U  /* 总运行时长 ~15s */
#define MOTOR_BENCH_BEHAVIOR_DIR_INTERVAL_MS 2000U   /* 每 2s 换向（含刹车/反转等待） */
#define MOTOR_BENCH_BEHAVIOR_TIMEOUT_MS      20000U  /* 行为模式 PULSING 等待上限 */

/* ---- FSM API ---- */

void motor_bench_fsm_init(motor_bench_fsm_ctx_t *ctx,
                          motor_bench_type_t type,
                          const motor_bench_step_t *steps,
                          uint8_t step_count,
                          machine_request_id_t rid);

/* 一步 tick。读 ctx + in，写 out。ctx 原地推进。
 * 终态 exactly-once：terminal==true 只置位一次，之后 tick 为空操作。 */
void motor_bench_fsm_tick(motor_bench_fsm_ctx_t *ctx,
                          const motor_bench_fsm_input_t *in,
                          motor_bench_fsm_output_t *out);

/* 供 host 测试/服务层：判断 FSM 是否已到终态 */
bool motor_bench_fsm_is_terminal(const motor_bench_fsm_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
