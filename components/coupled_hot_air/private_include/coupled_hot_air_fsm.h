#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "machine_types.h"
#include "coupled_hot_air_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * coupled_hot_air_fsm.h — 耦合热风模块状态机（纯 C，无 FreeRTOS/HAL 依赖）
 *
 * 模型：单继电器，风扇+加热丝按厂家规定并联同步启停，无预吹/延迟关风。
 * 硬规则：
 * - 只有 safety_manager_apply(RELAY_ON) 返回 ESP_OK 才进入 RUNNING。
 * - 只有继电器 OFF 确认成功才发 COMPLETE/INTERRUPTED 终态；
 *   OFF 确认失败 → FAULT（off_confirmed=false，fail-closed）。
 * - terminal exactly once；发布后回 IDLE。
 * ================================================================ */

#define CHA_OFF_RETRY_MAX  5   /* STOPPING 内 OFF 重试上限 */

typedef enum {
    CHA_FSM_IDLE = 0,
    CHA_FSM_VALIDATING,
    CHA_FSM_STARTING,
    CHA_FSM_RUNNING,
    CHA_FSM_STOPPING,
    CHA_FSM_COMPLETE,      /* 终态已发，等待回 IDLE */
} cha_fsm_state_t;

typedef enum {
    CHA_OUT_UNKNOWN = 0,
    CHA_OUT_OFF = 1,
    CHA_OUT_ON = 2,
} cha_output_state_t;

typedef enum {
    CHA_ACTION_NONE = 0,
    CHA_ACTION_RELAY_ON,
    CHA_ACTION_RELAY_OFF,
} cha_action_t;

/* 停止原因 → 终态映射 */
typedef enum {
    CHA_STOP_NONE = 0,
    CHA_STOP_DURATION,      /* 到期 → COMPLETE */
    CHA_STOP_CANCELED,      /* → INTERRUPTED */
    CHA_STOP_EMERGENCY,     /* → INTERRUPTED */
    CHA_STOP_FAULT,         /* 安全故障锁存（非急停）→ FAULT */
    CHA_STOP_POSITION,      /* → FAULT */
    CHA_STOP_SHT_INVALID,   /* → FAULT */
    CHA_STOP_SHT_STALE,     /* → FAULT */
    CHA_STOP_OVERTEMP,      /* → FAULT */
    CHA_STOP_COOLDOWN,      /* → FAULT（门禁已挡，兜底） */
    CHA_STOP_APPLY_FAULT,   /* RELAY_ON 失败 → FAULT */
    CHA_STOP_OFF_FAIL,      /* RELAY_OFF 确认失败 → FAULT */
} cha_stop_reason_t;

typedef enum {
    CHA_EVT_TICK = 0,
    CHA_EVT_ACTION_RESULT,
    CHA_EVT_CANCEL,
    CHA_EVT_EMERGENCY,
} cha_fsm_event_type_t;

typedef struct {
    cha_fsm_event_type_t type;
    int64_t now_ms;
    /* TICK */
    drum_position_t position;
    bool position_valid;
    bool position_stable;
    bool position_fresh;
    bool position_motor_moving;
    bool sht_valid;
    bool sht_fresh;
    float temperature_c;
    bool fault_active;
    bool emergency_active;
    uint32_t cooldown_remaining_ms;
    /* ACTION_RESULT */
    cha_action_t action;
    bool action_ok;
    /* CANCEL */
    machine_request_id_t cancel_request_id;
} cha_fsm_event_t;

typedef struct {
    cha_fsm_state_t next_state;
    cha_action_t action;
    coupled_hot_air_terminal_t terminal;   /* != NONE 表示终态 */
    const char *reason;                    /* 静态字符串 */
    bool off_confirmed;                    /* 终态时继电器 OFF 已确认 */
} cha_fsm_output_t;

typedef struct {
    cha_fsm_state_t state;
    machine_request_id_t request_id;
    uint32_t duration_ms;
    int64_t started_at_ms;
    int64_t deadline_ms;
    cha_output_state_t relay_state;
    bool relay_ever_on;
    cha_stop_reason_t stop_reason;
    uint32_t off_attempts;
    coupled_hot_air_terminal_t terminal;
    const char *terminal_reason;
    bool terminal_off_confirmed;    /* 终态时继电器 OFF 已确认（emit_terminal 记录） */
    bool terminal_emitted;
    uint32_t generation;
    uint32_t revision;

    /* 环境（tick 事件刷新） */
    drum_position_t position;
    bool position_valid;
    bool position_stable;
    bool position_fresh;
    bool position_motor_moving;
    bool sht_valid;
    bool sht_fresh;
    float temperature_c;
    bool emergency_active;
    bool fault_active;
    uint32_t cooldown_remaining_ms;
    int64_t tick_ms;

    float cutoff_c;                 /* 温度关断阈值（经 init 注入） */
    cha_action_t pending_action;
} cha_fsm_ctx_t;

/* ---- API ---- */

void cha_fsm_init(cha_fsm_ctx_t *ctx, float cutoff_c);
void cha_fsm_begin_request(cha_fsm_ctx_t *ctx, const coupled_hot_air_request_t *req,
                           int64_t now_ms);
void cha_fsm_tick(cha_fsm_ctx_t *ctx, const cha_fsm_event_t *evt,
                  cha_fsm_output_t *out);
void cha_fsm_mark_terminal_emitted(cha_fsm_ctx_t *ctx);

bool cha_fsm_is_idle(const cha_fsm_ctx_t *ctx);
bool cha_fsm_is_terminal(const cha_fsm_ctx_t *ctx);
const char *cha_fsm_reason_name(cha_stop_reason_t reason);

#ifdef __cplusplus
}
#endif
