#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "machine_types.h"
#include "uv_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * uv_fsm.h — UV 杀菌状态机（纯 C，无 FreeRTOS/HAL 依赖）
 * 所有时间使用传入 now_ms，tick wrap-safe
 * ================================================================ */

/* ---- FSM States ---- */

typedef enum {
    UV_FSM_IDLE = 0,
    UV_FSM_VALIDATING,
    UV_FSM_RUNNING,
    UV_FSM_STOPPING,
    UV_FSM_COMPLETE,
    UV_FSM_CANCELING,
    UV_FSM_SKIPPING,
    UV_FSM_FAULT,
} uv_fsm_state_t;

/* ---- FSM Actions ---- */

typedef enum {
    UV_ACTION_NONE = 0,
    UV_ACTION_LAMP_ON,
    UV_ACTION_LAMP_OFF,
} uv_fsm_action_t;

/* ---- FSM Events ---- */

typedef enum {
    UV_EVT_TICK = 0,
    UV_EVT_ACTION_RESULT,
    UV_EVT_CANCEL,
    UV_EVT_SKIP,
    UV_EVT_EMERGENCY,
} uv_fsm_event_type_t;

typedef struct {
    uv_fsm_event_type_t type;
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
    uv_fsm_action_t action_result_action;
    bool action_result_ok;

    /* CANCEL/SKIP */
    machine_request_id_t cancel_request_id;
} uv_fsm_event_t;

/* ---- Output Shadow ---- */

typedef enum {
    UV_OUTPUT_UNKNOWN = 0,
    UV_OUTPUT_KNOWN_OFF,
    UV_OUTPUT_KNOWN_ON,
} uv_output_state_t;

/* ---- FSM Output ---- */

typedef struct {
    uv_fsm_state_t next_state;
    uv_fsm_action_t action;
    uv_terminal_t terminal;
    machine_fault_code_t fault_code;
    fault_severity_t fault_severity;
    bool emit_event;
} uv_fsm_output_t;

/* ---- FSM Context ---- */

typedef struct {
    uint32_t max_duration_ms;

    uv_fsm_state_t state;
    machine_request_id_t request_id;
    uint32_t duration_ms;

    uv_output_state_t output_state;
    uint32_t state_enter_ms;
    uint32_t tick_ms;

    uv_fsm_action_t pending_action;
    bool terminal_event_emitted;
    uv_terminal_t terminal;
    machine_fault_code_t fault_code;
    fault_severity_t fault_severity;
} uv_fsm_ctx_t;

/* ---- API ---- */

void uv_fsm_init(uv_fsm_ctx_t *ctx, const uv_params_t *config);
esp_err_t uv_fsm_begin_request(uv_fsm_ctx_t *ctx, const uv_request_t *req,
                                 uint32_t max_duration_ms, uint32_t now_ms);
void uv_fsm_tick(uv_fsm_ctx_t *ctx, const uv_fsm_event_t *evt,
                  uv_fsm_output_t *out);
void uv_fsm_mark_terminal_emitted(uv_fsm_ctx_t *ctx);

/* 内部 uv_fsm_state_t (IDLE=0) → 公开 uv_state_t (IDLE=1, ...) 的显式映射。
 * 两个枚举值域刻意错位，禁止依赖数值相等。穷举每个内部状态；未知映射为
 * 安全的 FAULT，绝不误报 IDLE。纯函数，无依赖，host 可测。 */
uv_state_t uv_fsm_state_to_public(uv_fsm_state_t fsm);

/* 内部输出三态 → 公开 int 三态（0=UNKNOWN,1=KNOWN_OFF,2=KNOWN_ON）。
 * 显式映射，禁止依赖内部枚举数值。纯函数，host 可测。 */
int uv_fsm_output_to_public(uv_output_state_t out);

/* 服务快照 terminal_pending 的确定性推导：存在已产生但尚未发布（mark
 * terminal emitted）的终态。纯函数，host 可测。 */
bool uv_fsm_terminal_pending(const uv_fsm_ctx_t *ctx);

static inline bool uv_fsm_is_idle(const uv_fsm_ctx_t *ctx) {
    return ctx->state == UV_FSM_IDLE;
}

static inline bool uv_fsm_is_terminal(const uv_fsm_ctx_t *ctx) {
    return ctx->state == UV_FSM_COMPLETE || ctx->state == UV_FSM_FAULT;
}

static inline bool uv_fsm_has_pending_action(const uv_fsm_ctx_t *ctx) {
    return ctx->pending_action != UV_ACTION_NONE;
}

#ifdef __cplusplus
}
#endif
