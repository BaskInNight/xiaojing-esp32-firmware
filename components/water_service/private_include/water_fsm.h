#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "machine_types.h"
#include "water_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * water_fsm.h — 纯 C 状态机 (Round 5.4)
 *
 * commit_result requires pending_action != NONE
 * commit_close_all requires pending_action == CLOSE_ALL
 * success ACTION_RESULT validates committed shadow
 * protocol violation → CLOSE_ALL
 * ================================================================ */

typedef enum {
    WATER_VALVE_UNKNOWN = 0,
    WATER_VALVE_KNOWN_OFF,
    WATER_VALVE_KNOWN_ON,
} water_valve_state_t;

/* ---- FSM Output Action (定义在 event 之前，因 event 引用此类型) ---- */

typedef enum {
    WATER_FSM_ACTION_NONE = 0,
    WATER_FSM_ACTION_OPEN_SOURCE,
    WATER_FSM_ACTION_CLOSE_SOURCE,
    WATER_FSM_ACTION_OPEN_TRANSFER,
    WATER_FSM_ACTION_CLOSE_TRANSFER,
    WATER_FSM_ACTION_CLOSE_ALL,
    WATER_FSM_ACTION_RESET_FLOW,
} water_fsm_action_t;

/* ---- FSM Events ---- */

typedef enum {
    WATER_EVT_NONE = 0,
    WATER_EVT_TICK,
    WATER_EVT_CANCEL,
    WATER_EVT_EMERGENCY,
    WATER_EVT_FLOW_RESET_DONE,
    WATER_EVT_ACTION_RESULT,        /* 执行器动作结果 */
} water_event_type_t;

typedef struct {
    water_event_type_t type;
    int64_t now_ms;
    bool     drum_full;
    bool     water_level_valid;
    uint32_t flow_delta;
    uint32_t flow_total;
    bool     flow_read_ok;
    bool     safety_gpa0_active_ok;
    bool     safety_gpa1_active_ok;
    bool     safety_apply_ok;
    bool     action_result_ok;      /* FLOW_RESET_DONE / ACTION_RESULT: 动作是否成功 */
    water_fsm_action_t action_result_action; /* ACTION_RESULT: 哪个动作的结果 */
    machine_request_id_t cancel_request_id;
} water_event_t;

/* ---- Valve Commit Result ---- */

typedef enum {
    WATER_COMMIT_OK = 0,
    WATER_COMMIT_FAILED,
} water_valve_commit_result_t;

/* ---- Terminal Result ---- */

typedef enum {
    WATER_TERMINAL_NONE = 0,
    WATER_TERMINAL_COMPLETE,
    WATER_TERMINAL_CANCELLED,
    WATER_TERMINAL_FAULT,
} water_terminal_t;

/* ---- FSM Output ---- */

typedef struct {
    water_state_t        next_state;
    water_fsm_action_t   action;
    water_terminal_t     terminal;
    machine_fault_code_t fault_code;
    fault_severity_t     fault_severity;
    bool                 emit_event;
} water_fsm_output_t;

/* ---- Pending Terminal (service 层) ---- */

typedef struct {
    bool                 valid;
    machine_event_t      event;
    uint32_t             attempts;
    esp_err_t            last_error;
} water_pending_terminal_t;

/* ---- FSM Context ---- */

typedef struct {
    water_state_t state;
    machine_request_id_t request_id;
    bool complete_on_drum_full;

    water_fsm_action_t pending_action;  /* 当前等待的 ACTION_RESULT action ID */

    water_service_config_t cfg;
    uint32_t batch_max_ms_effective;

    int64_t request_start_ms;
    int64_t batch_start_ms;
    int64_t window_start_ms;
    int64_t settle_start_ms;
    int64_t transfer_start_ms;
    int64_t last_flow_pulse_ms;

    uint16_t completed_cycles;
    uint32_t current_batch_pulses;
    uint32_t total_flow_pulses;
    uint32_t low_flow_window_pulses;
    uint8_t  flow_io_error_count;

    /* Latest level sample, retained for idle/telemetry snapshots. */
    bool water_level_valid;
    bool drum_water_full;

    water_valve_state_t source_valve;
    water_valve_state_t transfer_valve;

    /* Cancel/emergency 时保存的原始终态，close 结果可能升级 */
    water_terminal_t    pending_terminal_type;
    machine_fault_code_t pending_fault_code;

    uint8_t close_all_shadow_retry_count;  /* bounded retry on shadow mismatch */

    bool terminal_event_emitted;
    machine_fault_t fault;
    uint32_t revision;
} water_fsm_ctx_t;

/* ---- FSM API ---- */

void water_fsm_init(water_fsm_ctx_t *ctx, const water_service_config_t *cfg);

void water_fsm_begin_request(water_fsm_ctx_t *ctx,
                              const water_in_request_t *req,
                              int64_t now_ms);

void water_fsm_tick(water_fsm_ctx_t *ctx,
                     const water_event_t *event,
                     water_fsm_output_t *output);

/* Commit shadow for a single-valve action.
 * Returns ESP_OK on success, ESP_ERR_INVALID_STATE if rejected:
 *   - ctx == NULL or out == NULL
 *   - pending_action == NONE (stale commit)
 *   - action != pending_action (mismatch)
 *   - action is not a valid single-valve action
 * Only modifies shadow and out when returning ESP_OK. */
esp_err_t water_fsm_commit_result(water_fsm_ctx_t *ctx,
                                  water_fsm_action_t action,
                                  water_valve_commit_result_t result,
                                  water_fsm_output_t *output);

/* Per-valve commit for CLOSE_ALL: commits source and transfer results
 * separately. Only valid when pending_action == CLOSE_ALL.
 * Each valve: success → KNOWN_OFF, failure → UNKNOWN.
 * Returns ESP_OK if committed, ESP_ERR_INVALID_STATE if rejected. */
esp_err_t water_fsm_commit_close_all(water_fsm_ctx_t *ctx,
                                     water_valve_commit_result_t source_result,
                                     water_valve_commit_result_t transfer_result,
                                     water_fsm_output_t *output);

/* Initial safe-OFF synchronization commit (HW-FIX-1).  Called once at service
 * start, before any FSM request, after the owning task has explicitly requested
 * both valves OFF through the normal safety/service path.  UNKNOWN is never
 * silently promoted to OFF: each valve becomes KNOWN_OFF only when its OFF
 * apply succeeded; a failed apply leaves it UNKNOWN (fail-closed).
 * Only valid while the FSM is IDLE (no request in flight). */
esp_err_t water_fsm_commit_initial_off(water_fsm_ctx_t *ctx,
                                       water_valve_commit_result_t source_result,
                                       water_valve_commit_result_t transfer_result,
                                       water_fsm_output_t *output);

void water_fsm_cancel(water_fsm_ctx_t *ctx,
                       machine_request_id_t request_id,
                       int64_t now_ms,
                       water_fsm_output_t *output);

void water_fsm_emergency(water_fsm_ctx_t *ctx,
                          int64_t now_ms,
                          water_fsm_output_t *output);

void water_fsm_snapshot(const water_fsm_ctx_t *ctx,
                         water_snapshot_t *out);

void water_fsm_mark_terminal_emitted(water_fsm_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
