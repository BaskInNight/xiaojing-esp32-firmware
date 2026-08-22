#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "machine_types.h"
#include "dry_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * dry_fsm.h — 烘干状态机（纯 C，无 FreeRTOS/HAL 依赖）
 * ================================================================ */

/* ---- FSM States ---- */

typedef enum {
    DRY_FSM_UNINITIALIZED = 0,
    DRY_FSM_IDLE,
    DRY_FSM_VALIDATING,
    DRY_FSM_ENSURE_PTC_OFF,     /* baseline: confirm PTC off before starting */
    DRY_FSM_ENSURE_FAN_OFF,     /* baseline: confirm fan off before starting */
    DRY_FSM_STARTING_FAN,
    DRY_FSM_PRE_FAN,
    DRY_FSM_HEATING,
    DRY_FSM_HEAT_REST,
    DRY_FSM_FINISHING,          /* normal: heater off confirmed, now cooldown */
    DRY_FSM_FAN_OFF_WAIT,       /* waiting for FAN_OFF confirmation */
    DRY_FSM_COMPLETE,
    DRY_FSM_CANCELING,
    DRY_FSM_FAULT,
} dry_fsm_state_t;

/* ---- Shutdown Reason ---- */

typedef enum {
    DRY_SHUTDOWN_NONE = 0,
    DRY_SHUTDOWN_NORMAL,
    DRY_SHUTDOWN_CANCELED,
    DRY_SHUTDOWN_FAULT,
    DRY_SHUTDOWN_EMERGENCY,
} dry_shutdown_reason_t;

/* ---- FSM Actions ---- */

typedef enum {
    DRY_ACTION_NONE = 0,
    DRY_ACTION_FAN_ON,
    DRY_ACTION_FAN_OFF,
    DRY_ACTION_HEATER_ON,
    DRY_ACTION_HEATER_OFF,
} dry_fsm_action_t;

#define DRY_ACTION_COUNT 5

/* ---- Output Shadow (tri-state) ---- */

typedef enum {
    DRY_OUTPUT_UNKNOWN = 0,
    DRY_OUTPUT_KNOWN_OFF,
    DRY_OUTPUT_KNOWN_ON,
} dry_output_state_t;

/* ---- FSM Events ---- */

typedef enum {
    DRY_EVT_TICK = 0,
    DRY_EVT_ACTION_RESULT,
    DRY_EVT_CANCEL,
    DRY_EVT_EMERGENCY,
} dry_fsm_event_type_t;

typedef struct {
    dry_fsm_event_type_t type;
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
    float humidity_percent;
    bool fault_active;
    bool emergency_active;

    /* ACTION_RESULT */
    dry_fsm_action_t action_result_action;
    bool action_result_ok;

    /* CANCEL */
    machine_request_id_t cancel_request_id;
} dry_fsm_event_t;

/* ---- FSM Output ---- */

typedef struct {
    dry_fsm_state_t next_state;
    dry_fsm_action_t action;
    dry_terminal_t terminal;
    machine_fault_code_t fault_code;
    fault_severity_t fault_severity;
    bool emit_event;
} dry_fsm_output_t;

/* ---- Action result tracking ---- */

typedef struct {
    bool valid;
    dry_fsm_action_t action;
    bool success;
} dry_action_result_t;

/* ---- FSM Context ---- */

typedef struct {
    /* Config */
    uint32_t pre_fan_ms;
    uint32_t heat_on_max_ms;
    uint32_t heat_off_min_ms;
    uint32_t cooldown_ms;
    uint32_t max_total_ms;
    float heater_cutoff_c;
    float heater_resume_c;
    uint8_t fan_percent;

    /* State */
    dry_fsm_state_t state;
    machine_request_id_t request_id;
    uint32_t duration_ms;
    bool heater_requested;

    /* Shutdown tracking */
    dry_shutdown_reason_t shutdown_reason;

    /* Pending action */
    dry_fsm_action_t pending_action;
    uint32_t action_seq;

    /* Shadow state (tri-state) */
    dry_output_state_t fan_state;
    dry_output_state_t heater_state;

    /* Whether heater was ever confirmed ON during this request */
    bool heater_ever_confirmed_on;

    /* Position */
    drum_position_t position;
    bool position_valid;
    bool position_stable;
    bool position_fresh;
    bool position_motor_moving;

    /* SHT */
    bool sht_valid;
    bool sht_fresh;
    float temperature_c;
    float humidity_percent;

    /* Timing */
    int64_t state_enter_ms;
    int64_t tick_ms;
    int64_t fan_on_since_ms;
    int64_t ptc_on_since_ms;
    int64_t ptc_off_since_ms;
    int64_t heating_start_ms;
    int64_t heating_total_ms;
    int64_t current_heat_cycle_ms;
    int64_t cooldown_start_ms;
    uint32_t cooldown_remaining_ms;

    /* PTC OFF retry */
    uint8_t ptc_off_retry_count;

    /* Terminal */
    bool terminal_event_emitted;
    dry_terminal_t terminal;        /* 保留终态（发布后回 IDLE 仍有效） */
    dry_fault_code_t fault_code;
    machine_fault_code_t m_fault_code;
    fault_severity_t fault_severity;

    /* Last action result */
    dry_action_result_t last_result;

    /* Revision */
    uint32_t revision;
} dry_fsm_ctx_t;

/* ---- FSM API ---- */

void dry_fsm_init(dry_fsm_ctx_t *ctx, const dry_params_t *config);
void dry_fsm_begin_request(dry_fsm_ctx_t *ctx, const dry_request_t *req,
                           int64_t now_ms);
void dry_fsm_tick(dry_fsm_ctx_t *ctx, const dry_fsm_event_t *evt,
                  dry_fsm_output_t *out);
void dry_fsm_mark_terminal_emitted(dry_fsm_ctx_t *ctx);

/* 初始安全 OFF 同步（fan/heater 板测前置）：仅在 IDLE 时提交风扇/加热器影子
 * 状态——成功 → KNOWN_OFF，失败 → UNKNOWN（fail-closed）。UNKNOWN 绝不当作
 * OFF。fan_state/heater_state 在 dry_fsm_init 为 UNKNOWN，须显式同步。 */
esp_err_t dry_fsm_commit_initial_off(dry_fsm_ctx_t *ctx,
                                     bool fan_off_ok, bool heater_off_ok);

/* 快照 terminal_pending 的确定性推导：存在已产生但尚未发布的终态。纯函数。 */
bool dry_fsm_terminal_pending(const dry_fsm_ctx_t *ctx);

/* ---- State query helpers ---- */

static inline bool dry_fsm_is_idle(const dry_fsm_ctx_t *ctx) {
    return ctx->state == DRY_FSM_IDLE;
}

static inline bool dry_fsm_is_terminal(const dry_fsm_ctx_t *ctx) {
    return ctx->state == DRY_FSM_COMPLETE || ctx->state == DRY_FSM_FAULT;
}

static inline bool dry_fsm_has_pending_action(const dry_fsm_ctx_t *ctx) {
    return ctx->pending_action != DRY_ACTION_NONE;
}

static inline bool dry_fsm_fan_is_on(const dry_fsm_ctx_t *ctx) {
    return ctx->fan_state == DRY_OUTPUT_KNOWN_ON;
}

static inline bool dry_fsm_heater_is_on(const dry_fsm_ctx_t *ctx) {
    return ctx->heater_state == DRY_OUTPUT_KNOWN_ON;
}

/* SAFE only when both are explicitly KNOWN_OFF */
static inline bool dry_fsm_outputs_safe(const dry_fsm_ctx_t *ctx) {
    return ctx->fan_state == DRY_OUTPUT_KNOWN_OFF &&
           ctx->heater_state == DRY_OUTPUT_KNOWN_OFF;
}

#ifdef __cplusplus
}
#endif
