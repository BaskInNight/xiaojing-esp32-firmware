#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "wash_contract.h"
#include "machine_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Executor states (wrapper lifecycle) ---- */

typedef enum {
    WASH_EXEC_STATE_IDLE        = 0,
    WASH_EXEC_STATE_WAIT_LOAD   = 1,
    WASH_EXEC_STATE_RUNNING     = 2,
    WASH_EXEC_STATE_WAIT_UNLOAD = 3,
    WASH_EXEC_STATE_UV          = 4,
    WASH_EXEC_STATE_RESETTING   = 5,
    WASH_EXEC_STATE_FAULT       = 6,
} wash_exec_state_t;

typedef enum {
    WASH_EXEC_EVENT_NONE              = 0,
    WASH_EXEC_EVENT_PROGRAM_ACCEPTED  = 1,
    WASH_EXEC_EVENT_STEP_DISPATCHED   = 2,
    WASH_EXEC_EVENT_STEP_TERMINAL     = 3,
    WASH_EXEC_EVENT_STEP_TIMEOUT      = 4,
    WASH_EXEC_EVENT_STEP_SKIPPED      = 5,
    WASH_EXEC_EVENT_CANCELLED         = 6,
    WASH_EXEC_EVENT_ABORTED           = 7,
    WASH_EXEC_EVENT_PROGRAM_COMPLETE  = 8,
    WASH_EXEC_EVENT_FAULT_ENTERED     = 9,
    WASH_EXEC_EVENT_RESET_COMPLETE    = 10,
} wash_exec_event_t;

/* ---- Program terminal sink ---- */

typedef struct {
    uint32_t program_id;
    uint8_t  result;       /* TERM_RESULT_COMPLETE/CANCELED/ABORTED/FAULT */
    uint32_t error_code;
    bool     final_position_confirmed;
    uint16_t completed_steps;
} program_terminal_t;

/* ---- Terminal sink callback ---- */

typedef bool (*program_terminal_sink_fn)(
    const program_terminal_t *terminal,
    void *ctx);

/* ---- Internal-step audit sink ---- */

/* STEP_SETTLE / STEP_AUDIT_UV_OFF / STEP_AUDIT_SAFE_OFF 是「内部步骤」：
 * 不经 real_adapter 派发到服务，由 executor 包装层内部处理。
 *   - STEP_SETTLE：定时无输出等待，duration_ms 到期自动完成；
 *   - STEP_AUDIT_*：dispatch 时调用本 sink 校验。返回 true → 立即完成；
 *     返回 false 或未注册 sink → fail-closed 进入故障（绝不静默放行）。
 * 由 app_bootstrap 注册（program_control 提供实现）。 */
typedef bool (*wash_executor_audit_fn)(wash_step_type_t step_type, void *ctx);

/* ---- Snapshot ---- */

typedef struct {
    wash_exec_state_t state;
    wash_exec_event_t last_event;
    uint32_t program_id;
    uint16_t current_step_id;
    uint16_t current_request_id;
    uint16_t completed_steps;
    uint16_t total_steps;
    uint32_t generation;
    bool program_active;
    bool terminal_pending;
    uint32_t terminal_attempt_count;
    bool adapter_in_flight;
    wash_step_type_t current_step_type;
    bool current_step_skippable;
} wash_exec_snapshot_t;

/* ---- Configuration ---- */

typedef struct {
    uint32_t step_timeout_default_ms;
    uint32_t reset_timeout_ms;
    uint32_t cancel_timeout_ms;
    uint32_t terminal_retry_ms;
    uint32_t (*get_now_ms)(void *ctx);
    void *time_ctx;
} wash_executor_config_t;

/* ---- Lifecycle states ---- */

typedef enum {
    EXEC_LIFECYCLE_UNINITIALIZED = 0,
    EXEC_LIFECYCLE_INITIALIZED,
    EXEC_LIFECYCLE_STARTING,
    EXEC_LIFECYCLE_RUNNING,
    EXEC_LIFECYCLE_QUIESCING,
    EXEC_LIFECYCLE_STOPPING,
    EXEC_LIFECYCLE_STOPPED,
    EXEC_LIFECYCLE_FAILED,
} exec_lifecycle_state_t;

/* ---- Executor instance ---- */
typedef struct wash_executor wash_executor_t;

/* ---- Forward declare adapter ---- */
typedef struct wash_executor_adapter wash_executor_adapter_t;

/**
 * Create an executor instance with an adapter. Does not start the task.
 * Adapter is caller-owned. Executor does NOT free the adapter on destroy.
 * The adapter must remain valid until destroy() returns.
 */
wash_executor_t *wash_executor_create(
    const wash_executor_config_t *config,
    wash_executor_adapter_t *adapter);

/**
 * Set the program terminal sink. Must be called before start() or
 * while lifecycle is INITIALIZED/STOPPED.
 */
void wash_executor_set_terminal_sink(
    wash_executor_t *exec,
    program_terminal_sink_fn sink,
    void *sink_ctx);

/**
 * Set the internal-step audit sink (STEP_AUDIT_UV_OFF / STEP_AUDIT_SAFE_OFF).
 * Call before start() or while lifecycle is INITIALIZED/STOPPED.
 * If unset, audit steps fail closed (program faults).
 */
void wash_executor_set_audit_sink(
    wash_executor_t *exec,
    wash_executor_audit_fn fn,
    void *ctx);

esp_err_t wash_executor_start(wash_executor_t *exec);

/**
 * Submit a program. Deep-copies into executor instance memory.
 * Caller may free/reuse immediately. Returns ESP_ERR_INVALID_STATE if busy.
 */
esp_err_t wash_executor_submit_program(
    wash_executor_t *exec,
    const wash_program_t *program,
    uint32_t program_id);

/**
 * Publish a service terminal. Token must match the original dispatch token.
 */
esp_err_t wash_executor_publish_terminal(
    wash_executor_t *exec,
    const void *token_buf,
    uint32_t token_size,
    bool success,
    uint32_t error_code);

esp_err_t wash_executor_submit_urgent(wash_executor_t *exec, bool abort);

esp_err_t wash_executor_confirm_load(wash_executor_t *exec);
esp_err_t wash_executor_confirm_unload(wash_executor_t *exec);
esp_err_t wash_executor_ack_fault(wash_executor_t *exec);

/* Only an ACTIVE UV step is skippable.  The executor requests UV service
 * cancellation and advances only after the OFF acknowledgement succeeds. */
esp_err_t wash_executor_skip_current_step(wash_executor_t *exec);

esp_err_t wash_executor_get_snapshot(
    wash_executor_t *exec,
    wash_exec_snapshot_t *out);

/**
 * Set an external activity blocker. While true, wash_executor_submit_program()
 * rejects new programs (returns ESP_ERR_INVALID_STATE). Used by diagnostics
 * (e.g. motor bench) to enforce two-way mutual exclusion with the production
 * wash: the bench already requires exec_idle to start; this closes the reverse
 * direction so a formal wash can never start while a bench session is active.
 * Default false. Thread-safe (takes the executor mutex).
 */
void wash_executor_set_external_busy(wash_executor_t *exec, bool busy);

/**
 * Return whether a program is currently reserved but not yet active
 * (submitted via submit_program, pending task activation). Live read under
 * the executor mutex — the published snapshot lags because it is task-published.
 * Used by diagnostics to avoid admitting during the reserve→activate window.
 */
bool wash_executor_program_reserved(wash_executor_t *exec);

esp_err_t wash_executor_quiesce(wash_executor_t *exec);
esp_err_t wash_executor_stop(wash_executor_t *exec, uint32_t timeout_ms);

/**
 * Destroy executor. Must be stopped first. Returns ESP_ERR_INVALID_STATE
 * if task is still running or terminal delivery is in-flight.
 * Does NOT free the adapter (caller-owned).
 */
esp_err_t wash_executor_destroy(wash_executor_t *exec);

/* ---- Test hooks (Unit test only) ---- */

#ifdef WASH_EXEC_TEST_HOOKS
typedef void (*wash_exec_hook_fn)(void *ctx);

typedef struct {
    wash_exec_hook_fn task_ready_barrier;
    void *task_ready_ctx;
    wash_exec_hook_fn task_exit_barrier;
    void *task_exit_ctx;
    wash_exec_hook_fn join_owner_barrier;
    void *join_owner_ctx;
} wash_exec_test_hooks_t;

/**
 * Set test hooks for deterministic barrier control. Pass NULL to clear.
 * Only available when WASH_EXEC_TEST_HOOKS is defined.
 */
void wash_executor_set_test_hooks(const wash_exec_test_hooks_t *hooks);
#endif

#ifdef __cplusplus
}
#endif
