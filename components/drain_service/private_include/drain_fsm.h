#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "machine_types.h"
#include "drain_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * drain_fsm.h — 排水状态机（纯 C，无 FreeRTOS/HAL 依赖）
 * 所有时间使用传入 now_ms，tick wrap-safe
 * ================================================================ */

/* ---- FSM States ---- */

typedef enum {
    DRAIN_FSM_IDLE = 0,
    DRAIN_FSM_VALIDATING,
    DRAIN_FSM_RUNNING,
    DRAIN_FSM_STOPPING,
    DRAIN_FSM_COMPLETE,
    DRAIN_FSM_CANCELING,
    DRAIN_FSM_FAULT,
} drain_fsm_state_t;

/* ---- FSM Actions ---- */

typedef enum {
    DRAIN_ACTION_NONE = 0,
    DRAIN_ACTION_VALVE_ON,
    DRAIN_ACTION_VALVE_OFF,
} drain_fsm_action_t;

/* ---- FSM Events ---- */

typedef enum {
    DRAIN_EVT_TICK = 0,
    DRAIN_EVT_ACTION_RESULT,
    DRAIN_EVT_CANCEL,
    DRAIN_EVT_EMERGENCY,
} drain_fsm_event_type_t;

typedef struct {
    drain_fsm_event_type_t type;
    uint32_t now_ms;

    /* TICK */
    drum_position_t position;
    bool position_valid;
    bool position_stable;
    bool position_fresh;
    bool position_motor_moving;
    bool fault_active;
    bool emergency_active;

    /* ACTION_RESULT */
    drain_fsm_action_t action_result_action;
    bool action_result_ok;

    /* CANCEL */
    machine_request_id_t cancel_request_id;
} drain_fsm_event_t;

/* ---- Output Shadow ---- */

typedef enum {
    DRAIN_OUTPUT_UNKNOWN = 0,
    DRAIN_OUTPUT_KNOWN_OFF,
    DRAIN_OUTPUT_KNOWN_ON,
} drain_output_state_t;

/* ---- FSM Output ---- */

typedef struct {
    drain_fsm_state_t next_state;
    drain_fsm_action_t action;
    drain_terminal_t terminal;
    machine_fault_code_t fault_code;
    fault_severity_t fault_severity;
    bool emit_event;
} drain_fsm_output_t;

/* ---- FSM Context ---- */

typedef struct {
    uint32_t max_duration_ms;

    drain_fsm_state_t state;
    machine_request_id_t request_id;
    uint32_t duration_ms;

    drain_output_state_t output_state;
    uint32_t state_enter_ms;
    uint32_t tick_ms;

    drain_fsm_action_t pending_action;
    bool terminal_event_emitted;
    drain_terminal_t terminal;
    machine_fault_code_t fault_code;
    fault_severity_t fault_severity;
} drain_fsm_ctx_t;

/* ---- API ---- */

void drain_fsm_init(drain_fsm_ctx_t *ctx, const drain_params_t *config);
esp_err_t drain_fsm_begin_request(drain_fsm_ctx_t *ctx, const drain_request_t *req,
                                    uint32_t max_duration_ms, uint32_t now_ms);
void drain_fsm_tick(drain_fsm_ctx_t *ctx, const drain_fsm_event_t *evt,
                     drain_fsm_output_t *out);
void drain_fsm_mark_terminal_emitted(drain_fsm_ctx_t *ctx);

/* 内部 drain_fsm_state_t (IDLE=0) → 公开 drain_state_t (IDLE=1, ...) 的显式
 * 映射。禁止依赖数值相等（IDLE 偏移）。未知映射为安全 FAULT。纯函数。 */
drain_state_t drain_fsm_state_to_public(drain_fsm_state_t fsm);

/* 内部输出三态 → 公开 int 三态（0=UNKNOWN,1=KNOWN_OFF,2=KNOWN_ON）。纯函数。 */
int drain_fsm_output_to_public(drain_output_state_t out);

/* 快照 terminal_pending 的确定性推导：存在已产生但尚未发布的终态。纯函数。 */
bool drain_fsm_terminal_pending(const drain_fsm_ctx_t *ctx);

static inline bool drain_fsm_is_idle(const drain_fsm_ctx_t *ctx) {
    return ctx->state == DRAIN_FSM_IDLE;
}

static inline bool drain_fsm_is_terminal(const drain_fsm_ctx_t *ctx) {
    return ctx->state == DRAIN_FSM_COMPLETE || ctx->state == DRAIN_FSM_FAULT;
}

static inline bool drain_fsm_has_pending_action(const drain_fsm_ctx_t *ctx) {
    return ctx->pending_action != DRAIN_ACTION_NONE;
}

#ifdef __cplusplus
}
#endif
