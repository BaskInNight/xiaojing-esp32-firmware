/*
 * program_control_core.h — 程序控制 纯 C 模型（host-testable）
 *
 * 不含 FreeRTOS/HAL/NVS/GPIO，无静态可变状态，可重入、确定性。
 * 供运行时服务（program_control_service.c）与 host 测试共用同一份权威逻辑：
 *   - DEFAULT_DEMO_V1 26 阶段规范表 + 编译函数（生成 wash_program_t）；
 *   - 输出审计纯谓词（UV-off / 全危险输出 safe-off，UNKNOWN 视为失败）；
 *   - 多输入仲裁（EMERGENCY > FAULT > STOP > START > backend switch）；
 *   - BTN1 短按→START / 长按→后端切换（互斥）纯决策；
 *   - 手势 UP→START / DOWN→STOP / 左右→翻页（冷却 + 重武装 + 尾串抑制）。
 *
 * 安全约束（贯穿）：绝不自动给电机/热风通电；审计失败 fail-closed 进入故障；
 * UNKNOWN 输出一律视为不满足「确认 OFF」；编译计划位置可达性校验。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "machine_types.h"
#include "wash_contract.h"
#include "machine_config.h"
#include "wash_planner.h"   /* planner_report_t / planner_result_t */
#include "xiaojing_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * DEFAULT_DEMO_V1 26 阶段规范
 *
 * 26 个演示阶段（规范表，测试断言其精确数量与顺序）。编译函数会把阶段
 * 编译为可执行 wash_program_t：在服务步骤的位置门禁与当前位置不一致时
 * 插入系统 MOVE 步骤（WASH_STEP_FLAG_SYSTEM_INSERTED，与 wash_planner
 * 的插入语义一致）。因此「规范阶段数=26」与「编译后步骤数≥26」并不矛盾。
 *
 * 位置契约（与生产服务同源，禁止硬编码猜测）：
 *   UV→DRUM_POS_0 / BL50 HOME→45° / PULSATOR→0° / DRUM→90° / SPIN→180°
 *   WATER+DETERGENT→DRUM_POS_0 / DRAIN→DRUM_POS_180 / DRY→DRUM_POS_270
 * 编译起点位置默认 DRUM_POS_45（任何已完成的洗涤/复位后的标准停放位）。
 * ================================================================ */

#define DEFAULT_DEMO_PHASE_COUNT         26U
#define DEFAULT_DEMO_START_POSITION      DRUM_POS_45

/* 演示缩放时长（ms）。服务类步骤 0=交给对应服务/配置默认。 */
#define DEFAULT_DEMO_UV_MS               5000U
#define DEFAULT_DEMO_PRECHECK_MS         0U      /* HOME：服务默认 */
#define DEFAULT_DEMO_WATER_MS            0U      /* 脉冲计量驱动 */
#define DEFAULT_DEMO_DETERGENT_MS        3000U
#define DEFAULT_DEMO_PREWASH_MS          8000U
#define DEFAULT_DEMO_SETTLE_MS           3000U
#define DEFAULT_DEMO_DRUM_MS             10000U
#define DEFAULT_DEMO_DRAIN_MS            8000U
#define DEFAULT_DEMO_SPIN_RAMP_MS        3000U
#define DEFAULT_DEMO_SPIN_HOLD_MS        6000U
#define DEFAULT_DEMO_SPIN_RAMP_DOWN_MS   3000U
#define DEFAULT_DEMO_HOT_AIR_MS          15000U
#define DEFAULT_DEMO_COOL_TUMBLE_MS      10000U

/* 内部步骤超时兜底（executor 内部处理 SETTLE/AUDIT，本常量为 0 时长兜底） */
#define DEFAULT_DEMO_SETTLE_FALLBACK_MS  3000U

/* ---- 阶段枚举（1..26，顺序即规范顺序） ---- */

typedef enum {
    DEMO_PHASE_ORDER_ACCEPT    = 1,
    DEMO_PHASE_UV              = 2,
    DEMO_PHASE_PRECHECK        = 3,
    DEMO_PHASE_POS_0           = 4,
    DEMO_PHASE_WATER           = 5,
    DEMO_PHASE_DETERGENT       = 6,
    DEMO_PHASE_PREWASH         = 7,
    DEMO_PHASE_SETTLE_1        = 8,
    DEMO_PHASE_POS_90          = 9,
    DEMO_PHASE_DRUM            = 10,
    DEMO_PHASE_SETTLE_2        = 11,
    DEMO_PHASE_POS_180         = 12,
    DEMO_PHASE_DRAIN           = 13,
    DEMO_PHASE_SPIN_RAMP       = 14,
    DEMO_PHASE_SPIN_HOLD       = 15,
    DEMO_PHASE_SPIN_RAMP_DOWN  = 16,
    DEMO_PHASE_SETTLE_3        = 17,
    DEMO_PHASE_POS_270         = 18,
    DEMO_PHASE_HOT_AIR         = 19,
    DEMO_PHASE_COOL_TUMBLE     = 20,
    DEMO_PHASE_SETTLE_4        = 21,
    DEMO_PHASE_POS_45          = 22,
    DEMO_PHASE_UV_OFF_AUDIT    = 23,
    DEMO_PHASE_SAFE_OFF_AUDIT  = 24,
    DEMO_PHASE_WAIT_UNLOAD     = 25,
    DEMO_PHASE_COMPLETE        = 26,
} default_demo_phase_id_t;

typedef struct {
    uint8_t phase;                 /* 1..26 = DEMO_PHASE_* */
    wash_step_type_t step_type;
    drum_position_t pos;           /* 该阶段执行时所需的桶位（内部步骤=当前位置） */
    uint32_t duration_ms;          /* 0=服务/配置默认 */
    wash_intensity_t intensity;
    uint32_t flags;                /* WASH_STEP_FLAG_* */
} default_demo_phase_t;

/* 规范阶段表（长度恒为 DEFAULT_DEMO_PHASE_COUNT）。返回静态 const 表。 */
const default_demo_phase_t *default_demo_phase_table(void);

/* 编译 DEFAULT_DEMO_V1 → wash_program_t。
 * initial_position = 编译起点假设位置（默认 DEFAULT_DEMO_START_POSITION）。
 * 位置门禁不满足时插入系统 MOVE 步骤。step_count 为编译后总步骤数
 * （= 26 + 系统 MOVE 数），steps_inserted 报告系统插入数。 */
planner_report_t default_demo_build_program(
    const machine_config_t *config,
    uint32_t program_id,
    drum_position_t initial_position,
    wash_program_t *out_program);

/* 编译后程序位置可达性不变量：沿程序追踪桶位，断言每个非 MOVE 步骤的
 * required_position 与前一 MOVE 到达位置一致（位置门禁步骤必有前置移动）。
 * UNKNOWN 位置门禁步骤（如 WAIT_LOAD）跳过。返回 false 表示不变量被破坏。 */
bool default_demo_validate_position_trace(
    const wash_program_t *program,
    drum_position_t initial_position);

/* ================================================================
 * 输出审计纯谓词（fail-closed）
 *
 * 输出状态三态：0=UNKNOWN, 1=OFF(已确认), 2=ON。
 * 任何 UNKNOWN 都视为不满足「确认 OFF」→ 审计失败（禁止把 UNKNOWN 显示为 OFF）。
 * ================================================================ */

typedef struct {
    int uv;                /* UV 灯 */
    int heater;            /* 热风 PTC */
    int fan;               /* 热风风机 */
    int tap_valve;         /* 进水源阀 */
    int transfer_valve;    /* 进水转移阀 */
    int detergent_pump;    /* 洗衣液泵 */
    int drain_valve;       /* 排水阀 */
    bool bl50_busy;        /* BL50 电机任一动作进行中 */
    bool position_moving;  /* IBT-2 换位电机移动中 */
} pc_output_audit_t;

/* UV-off 审计：UV 输出必须为「已确认 OFF」且 UV 服务不忙。 */
bool pc_audit_uv_off(const pc_output_audit_t *a);

/* 全危险输出 safe-off 审计：所有输出均为「已确认 OFF」，且两个电机都不忙。 */
bool pc_audit_safe_off(const pc_output_audit_t *a);

/* ================================================================
 * 多输入仲裁（EMERGENCY > FAULT > ABORT > STOP > START > backend switch）
 * ================================================================ */

typedef enum {
    PC_REQ_EMERGENCY = 0,      /* 最高优先级：急停闩锁 */
    PC_REQ_ABORT,              /* 立即中止（比 STOP 强，不闩锁 safety） */
    PC_REQ_STOP,               /* 停止当前程序 */
    PC_REQ_START_DEMO,         /* 启动 DEFAULT_DEMO_V1 */
    PC_REQ_BACKEND_SWITCH,     /* 最低：语音后端切换（与洗涤状态正交） */
} pc_request_t;

typedef enum {
    PC_STATE_IDLE = 0,
    PC_STATE_STARTING,         /* START 已受理，程序保留/启动中 */
    PC_STATE_RUNNING,          /* 程序运行中 */
    PC_STATE_STOPPING,         /* STOP 已受理，取消进行中 */
    PC_STATE_FAULT,            /* 程序故障，等待用户处理（仅 EMERGENCY 可通过） */
    PC_STATE_EMERGENCY,        /* 急停闩锁 */
} pc_state_t;

typedef enum {
    PC_ACTION_NONE = 0,
    PC_ACTION_START_DEMO,
    PC_ACTION_STOP_PROGRAM,    /* executor cancel（submit_urgent(false)） */
    PC_ACTION_ABORT_PROGRAM,   /* executor abort（submit_urgent(true)） */
    PC_ACTION_EMERGENCY_STOP,  /* safety_manager 急停 + executor abort */
    PC_ACTION_BACKEND_SWITCH,
} pc_action_t;

typedef struct {
    /* 内部闩锁状态（控制平面单线程更新） */
    pc_state_t state;
    /* 外部真实（运行时每拍从 executor / safety_manager 快照回填） */
    bool program_active;       /* executor 有程序运行/保留 */
    bool fault_active;         /* executor 处于 FAULT，等待 ack */
    bool emergency_latched;    /* safety_manager 急停闩锁 */
    bool external_busy;        /* 诊断组件（motor_bench 等）持有 executor */
} pc_arbiter_t;

/* 依当前状态 + 外部真实 + 请求优先级裁决。返回动作并更新内部闩锁。
 * 纯函数：无 IO，仅修改 *a 内部字段。 */
pc_action_t pc_arbitrate(pc_arbiter_t *a, pc_request_t req);

/* 控制平面每拍以外部真实为准修正闩锁：EMERGENCY/FAULT/RUNNING 主导；
 * 程序非激活时 STARTING/STOPPING 塌缩回 IDLE。仅改 *a 内部字段。 */
void pc_arbiter_refresh(pc_arbiter_t *a);

/* ================================================================
 * BTN1 短按 / 长按 纯决策（互斥）
 *
 * 长按抑制释放时的 CLICK（button_debounce 已保证互斥）；此处为最终权威：
 * 长按事件 → 后端切换（语音激活时），短按事件 → START。二者互斥。
 * ================================================================ */

typedef enum {
    PC_BTN1_EV_NONE = 0,
    PC_BTN1_EV_CLICK,          /* 短按 */
    PC_BTN1_EV_LONG_PRESS,     /* 长按（到达长按阈值） */
    PC_BTN1_EV_LONG_RELEASE,   /* 长按释放（不产生 CLICK） */
} pc_btn1_event_t;

typedef enum {
    PC_BTN1_ACTION_NONE = 0,
    PC_BTN1_ACTION_START,      /* 短按 → START */
    PC_BTN1_ACTION_BACKEND,    /* 长按 → 语音后端切换 */
} pc_btn1_action_t;

pc_btn1_action_t pc_btn1_decide(pc_btn1_event_t ev, bool voice_active);

/* ================================================================
 * 手势 UP→START / DOWN→STOP / 左右→翻页
 * 冷却 + 重武装 + 尾串抑制
 * ================================================================ */

typedef enum {
    PC_GESTURE_ACTION_NONE = 0,
    PC_GESTURE_ACTION_START,      /* UP */
    PC_GESTURE_ACTION_STOP,       /* DOWN */
    PC_GESTURE_ACTION_PAGE_PREV,  /* LEFT / BACKWARD */
    PC_GESTURE_ACTION_PAGE_NEXT,  /* RIGHT / FORWARD */
} pc_gesture_action_t;

typedef struct {
    bool armed;                /* true=下一个可动作手势可触发 */
    uint32_t cooldown_until_ms;/* 冷却截止（now_ms >= 该值才可重武装/触发） */
} pc_gesture_state_t;

/* 步进一次手势状态机。
 * - NONE：传感器空闲；冷却过后重武装。
 * - 可动作手势：未武装或在冷却内 → 抑制（尾串）；否则触发映射动作并进入
 *   冷却（armed=false, cooldown_until=now+cooldown_ms）。
 * CLOCKWISE/COUNTER_CW 不映射动作。 */
pc_gesture_action_t pc_gesture_step(pc_gesture_state_t *s,
                                    hal_gesture_t gesture,
                                    uint32_t now_ms,
                                    uint32_t cooldown_ms);

#ifdef __cplusplus
}
#endif
