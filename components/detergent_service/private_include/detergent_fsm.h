#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "machine_types.h"
#include "detergent_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * detergent_fsm.h — 洗衣液投放状态机（纯 C，无 FreeRTOS/HAL 依赖）
 * 所有时间使用传入 now_ms，tick wrap-safe
 * ================================================================ */

/* ---- FSM States ---- */

typedef enum {
    DETERGENT_FSM_IDLE = 0,
    DETERGENT_FSM_VALIDATING,
    DETERGENT_FSM_RUNNING,
    DETERGENT_FSM_STOPPING,
    DETERGENT_FSM_COMPLETE,
    DETERGENT_FSM_CANCELING,
    DETERGENT_FSM_FAULT,
} detergent_fsm_state_t;

/* ---- FSM Actions ---- */

typedef enum {
    DETERGENT_ACTION_NONE = 0,
    DETERGENT_ACTION_PUMP_ON,
    DETERGENT_ACTION_PUMP_OFF,
} detergent_fsm_action_t;

/* ---- FSM Events ---- */

typedef enum {
    DETERGENT_EVT_TICK = 0,
    DETERGENT_EVT_ACTION_RESULT,
    DETERGENT_EVT_CANCEL,
    DETERGENT_EVT_EMERGENCY,
} detergent_fsm_event_type_t;

typedef struct {
    detergent_fsm_event_type_t type;
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
    detergent_fsm_action_t action_result_action;
    bool action_result_ok;

    /* CANCEL */
    machine_request_id_t cancel_request_id;
} detergent_fsm_event_t;

/* ---- Output Shadow ---- */

typedef enum {
    DETERGENT_OUTPUT_UNKNOWN = 0,
    DETERGENT_OUTPUT_KNOWN_OFF,
    DETERGENT_OUTPUT_KNOWN_ON,
} detergent_output_state_t;

/* ---- FSM Output ---- */

typedef struct {
    detergent_fsm_state_t next_state;
    detergent_fsm_action_t action;
    detergent_terminal_t terminal;
    machine_fault_code_t fault_code;
    fault_severity_t fault_severity;
    bool emit_event;
} detergent_fsm_output_t;

/* ---- FSM Context ---- */

typedef struct {
    uint32_t max_single_ms;

    detergent_fsm_state_t state;
    machine_request_id_t request_id;
    uint32_t duration_ms;

    detergent_output_state_t output_state;
    uint32_t state_enter_ms;
    uint32_t tick_ms;

    detergent_fsm_action_t pending_action;
    bool terminal_event_emitted;
    detergent_terminal_t terminal;
    machine_fault_code_t fault_code;
    fault_severity_t fault_severity;
} detergent_fsm_ctx_t;

/* ---- API ---- */

void detergent_fsm_init(detergent_fsm_ctx_t *ctx, const detergent_params_t *config);
esp_err_t detergent_fsm_begin_request(detergent_fsm_ctx_t *ctx,
                                        const detergent_request_t *req,
                                        uint32_t max_single_ms, uint32_t now_ms);
void detergent_fsm_tick(detergent_fsm_ctx_t *ctx, const detergent_fsm_event_t *evt,
                         detergent_fsm_output_t *out);
void detergent_fsm_mark_terminal_emitted(detergent_fsm_ctx_t *ctx);

/* 内部 detergent_fsm_state_t (IDLE=0) → 公开 detergent_state_t (IDLE=1, ...)
 * 的显式映射。纯函数，无依赖，host 可测。 */
detergent_state_t detergent_fsm_state_to_public(detergent_fsm_state_t fsm);

/* 内部输出三态 → 公开 int 三态（0=UNKNOWN,1=KNOWN_OFF,2=KNOWN_ON）。
 * 显式映射，禁止依赖内部枚举数值与公开约定巧合相等。纯函数，host 可测。 */
int detergent_fsm_output_to_public(detergent_output_state_t out);

/* 服务快照 terminal_pending 的确定性推导：存在已产生但尚未发布（mark
 * terminal emitted）的终态。纯函数，host 可测；快照不得依赖未初始化字段。 */
bool detergent_fsm_terminal_pending(const detergent_fsm_ctx_t *ctx);

static inline bool detergent_fsm_is_idle(const detergent_fsm_ctx_t *ctx) {
    return ctx->state == DETERGENT_FSM_IDLE;
}

static inline bool detergent_fsm_is_terminal(const detergent_fsm_ctx_t *ctx) {
    return ctx->state == DETERGENT_FSM_COMPLETE || ctx->state == DETERGENT_FSM_FAULT;
}

static inline bool detergent_fsm_has_pending_action(const detergent_fsm_ctx_t *ctx) {
    return ctx->pending_action != DETERGENT_ACTION_NONE;
}

#ifdef __cplusplus
}
#endif
