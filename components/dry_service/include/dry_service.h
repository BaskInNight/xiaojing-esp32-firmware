#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "machine_types.h"
#include "machine_config.h"
#include "xiaojing_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * dry_service.h — 烘干服务公共 API
 *
 * 纯 C 接口。状态机在 dry_fsm.h，不在此暴露。
 * 生命周期: global_init -> init -> start -> run_async/cancel -> stop
 * ================================================================ */

/* ---- States ---- */

typedef enum {
    DRY_STATE_UNINITIALIZED = 0,
    DRY_STATE_IDLE,
    DRY_STATE_VALIDATING,
    DRY_STATE_ENSURE_PTC_OFF,
    DRY_STATE_ENSURE_FAN_OFF,
    DRY_STATE_STARTING_FAN,
    DRY_STATE_PRE_FAN,
    DRY_STATE_HEATING,
    DRY_STATE_HEAT_REST,
    DRY_STATE_FINISHING,
    DRY_STATE_FAN_OFF_WAIT,
    DRY_STATE_COMPLETE,
    DRY_STATE_CANCELING,
    DRY_STATE_FAULT,
} dry_state_t;

/* ---- Terminal Reason ---- */

typedef enum {
    DRY_TERMINAL_NONE = 0,
    DRY_TERMINAL_COMPLETE,
    DRY_TERMINAL_CANCELED,
    DRY_TERMINAL_FAULT,
} dry_terminal_t;

/* ---- Fault Codes (dry-service specific detail) ---- */

typedef enum {
    DRY_FAULT_NONE = 0,
    DRY_FAULT_FAN_APPLY,
    DRY_FAULT_PTC_APPLY,
    DRY_FAULT_SHT_INVALID,
    DRY_FAULT_SHT_STALE,
    DRY_FAULT_SHT_IO,
    DRY_FAULT_POSITION_LOST,
    DRY_FAULT_OVERTEMP,
    DRY_FAULT_DURATION_EXCEEDED,
    DRY_FAULT_PTC_OFF_FAILED,
    DRY_FAULT_INTERNAL,
} dry_fault_code_t;

/* ---- Request ---- */

typedef struct {
    machine_request_id_t request_id;
    uint32_t duration_ms;
    bool heater_requested;
} dry_request_t;

/* ---- Snapshot ---- */

typedef struct {
    /* FSM state */
    dry_state_t state;
    machine_request_id_t request_id;
    uint32_t elapsed_ms;
    uint32_t requested_duration_ms;

    /* Position */
    drum_position_t position;
    bool position_valid;
    bool position_stable;
    bool position_fresh;

    /* Fan */
    uint8_t fan_percent;
    bool fan_on;

    /* Heater */
    bool heater_requested;
    bool heater_on;

    /* Output state (tri-state for diagnostics) */
    int fan_output_state;       /* dry_output_state_t: 0=UNKNOWN, 1=OFF, 2=ON */
    int heater_output_state;    /* dry_output_state_t: 0=UNKNOWN, 1=OFF, 2=ON */

    /* SHT */
    bool sht_valid;
    bool sht_fresh;
    float temperature_c;
    float humidity_percent;

    /* Timing */
    uint32_t heat_cycle_elapsed_ms;
    uint32_t cooldown_remaining_ms;

    /* Terminal */
    bool terminal_pending;
    dry_terminal_t terminal;        /* 保留终态（发布后回 IDLE 仍有效） */
    dry_fault_code_t fault;
    /* 初始安全 OFF 同步：dry 任务启动时经 safety 请求 fan/heater OFF 后置位。
     * false = 同步未完成（fan/heater 板测门禁拒绝）。 */
    bool outputs_synced;
    uint32_t revision;
} dry_snapshot_t;

/* ---- Lifecycle ---- */

/**
 * @brief Create global mutex. Call once per process, idempotent.
 */
esp_err_t dry_service_global_init(void);

/**
 * @brief Allocate resources, validate config/hal. Must be LC_UNINITIALIZED.
 */
esp_err_t dry_service_init(const dry_params_t *config,
                           const xiaojing_hal_t *hal,
                           machine_event_sink_t event_sink);

/**
 * @brief Create task, register stop hook. Must be LC_INITIALIZED.
 */
esp_err_t dry_service_start(void);

/**
 * @brief Signal task exit, wait for join, unregister hook, free resources.
 *         Idempotent. Safe to call at any lifecycle state.
 */
esp_err_t dry_service_stop(void);

/* ---- Commands ---- */

/**
 * @brief Submit dry request. Exactly one active at a time.
 *         request_id must be valid, duration_ms > 0 and <= config.max_total_ms.
 *         PTC disabled + heater_requested => ESP_ERR_NOT_SUPPORTED.
 */
esp_err_t dry_service_run_async(const dry_request_t *request);

/**
 * @brief Cancel active request by ID. Wrong/late ID is ignored.
 */
esp_err_t dry_service_cancel(machine_request_id_t request_id);

/**
 * @brief Emergency: PTC OFF immediately, notify task.
 */
esp_err_t dry_service_emergency_stop(void);

/* ---- Query ---- */

/**
 * @brief Copy current snapshot under mutex. 10ms timeout.
 */
esp_err_t dry_service_get_snapshot(dry_snapshot_t *out);

#ifdef DRY_SERVICE_TEST_HOOKS

/* ---- Test Hook Points ---- */

typedef enum {
    DRY_HOOK_PRE_FAN_APPLY = 0,
    DRY_HOOK_PRE_PTC_APPLY,
    DRY_HOOK_COUNT,
} dry_hook_point_t;

typedef void (*dry_hook_fn)(dry_hook_point_t where, void *user_data);

void  dry_service_set_hook(dry_hook_point_t where, dry_hook_fn fn, void *user_data);
void *dry_service_get_barrier(dry_hook_point_t where);

/* Test introspection */
esp_err_t dry_service_test_fail_next_hook_unregister(esp_err_t error);
bool      dry_service_test_is_task_handle_null(void);
bool      dry_service_test_is_stop_hook_registered(void);
uint32_t  dry_service_test_get_notify_attempt_count(void);
bool      dry_service_test_resources_alive(void);

#endif /* DRY_SERVICE_TEST_HOOKS */

#ifdef __cplusplus
}
#endif
