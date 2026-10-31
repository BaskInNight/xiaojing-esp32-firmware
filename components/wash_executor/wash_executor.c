/*
 * wash_executor.c — FreeRTOS wrapper (Round 1.3)
 *
 * Terminal retry state machine. Deadline model with virtual clock.
 * Iterative command pump. Immediate wake via task notification.
 * Published snapshot. Active stop drain. Idempotent stop. Destroy gate.
 */

#include "wash_executor.h"
#include "wash_executor_core.h"
#include "wash_executor_adapter.h"
#include "machine_status_store.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "wash_exec";

/* STEP_SETTLE 0 时长兜底（recipe 未给时长时的内部等待默认） */
#define WASH_EXEC_INTERNAL_SETTLE_MS     3000U
/* STEP_AUDIT_* 失败/未注册 sink 时的故障码 */
#define WASH_EXEC_ERR_AUDIT_FAIL         250U

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

static wash_exec_test_hooks_t s_test_hooks;

void wash_executor_set_test_hooks(const wash_exec_test_hooks_t *hooks)
{
    if (hooks) {
        s_test_hooks = *hooks;
    } else {
        memset(&s_test_hooks, 0, sizeof(s_test_hooks));
    }
}
#endif /* WASH_EXEC_TEST_HOOKS */

#define CMD_PUMP_MAX_ITERATIONS 16
#define QUEUE_SERVICE_DEPTH 8
#define QUEUE_URGENT_DEPTH 2
#define MAX_DRAIN_PER_WAKE 8

/* ---- Ingress message types ---- */

typedef enum {
    INGRESS_PROGRAM = 0,
    INGRESS_SERVICE_TERMINAL,
    INGRESS_URGENT,
    INGRESS_LOAD_CONFIRM,
    INGRESS_UNLOAD_CONFIRM,
    INGRESS_FAULT_ACK,
    INGRESS_SKIP,
} ingress_type_t;

typedef struct {
    ingress_type_t type;
    uint32_t program_id;
    terminal_token_t token;
    bool success;
    uint32_t error_code;
    bool abort_flag;
} ingress_msg_t;

/* ---- Terminal delivery states ---- */

typedef enum {
    TERM_STATE_IDLE = 0,
    TERM_STATE_PENDING,
    TERM_STATE_IN_FLIGHT,
} terminal_state_t;

/* ---- Event bits ---- */

#define EVT_TASK_READY    (1U << 0)
#define EVT_TASK_DONE     (1U << 1)
#define EVT_JOIN_COMPLETE (1U << 2)

/* ---- Executor instance ---- */

struct wash_executor {
    wash_executor_config_t config;
    wash_executor_adapter_t *adapter;

    /* Core state machine — only modified by executor task */
    wash_executor_core_t core;

    /* Per-instance program storage */
    wash_program_t pending_program;
    wash_program_t active_program;
    bool program_reserved;

    /* External activity blocker: set by diagnostics (motor bench) for two-way
     * mutual exclusion. While true, submit_program rejects new programs. */
    bool external_busy;

    /* Published snapshot — written by task under mutex, read by get_snapshot */
    wash_exec_snapshot_t published_snapshot;

    /* FreeRTOS */
    TaskHandle_t task_handle;
    QueueHandle_t queue_program;
    QueueHandle_t queue_service;
    QueueHandle_t queue_urgent;
    QueueHandle_t queue_stop;
    QueueSetHandle_t queue_set;
    SemaphoreHandle_t mutex;
    EventGroupHandle_t events;

    /* Lifecycle */
    exec_lifecycle_state_t lifecycle;
    uint32_t task_generation;       /* allocated by start() under mutex */
    uint32_t exit_ready_generation; /* written by task on exit_ready */
    uint32_t join_complete_generation; /* written by join owner on complete */
    bool task_done_persistent;
    bool task_exit_ready;
    bool join_claimed;      /* stop owner has claimed but not yet deleted */
    bool join_complete;     /* task deleted, safe to destroy */

    /* Terminal sink */
    program_terminal_sink_fn terminal_sink;
    void *terminal_sink_ctx;

    /* Terminal delivery state machine */
    terminal_state_t terminal_state;
    program_terminal_t pending_terminal;
    uint32_t terminal_attempt_count;
    uint32_t terminal_next_retry_ms;
    bool terminal_needs_delivery;

    /* Deadline tracking */
    uint32_t step_deadline_ms;
    bool step_deadline_active;
    uint32_t cancel_deadline_ms;
    bool cancel_deadline_active;
    uint32_t reset_deadline_ms;
    bool reset_deadline_active;
    bool adapter_in_flight;

    /* Internal steps (STEP_SETTLE / STEP_AUDIT_*): handled inside the
     * wrapper, never dispatched to the adapter. */
    bool internal_step_active;
    wash_executor_audit_fn audit_fn;
    void *audit_ctx;

    uint32_t program_started_ms;
    bool program_started_valid;

    /* Stop coordination */
    bool stop_requested;

    /* Join synchronization — task gives before vTaskDelete, stop waits */
    SemaphoreHandle_t join_sem;
};

/* ---- Forward declarations ---- */

static void process_core_event_and_drain(
    wash_executor_t *exec, const core_event_t *event);
static void process_ingress_msg(wash_executor_t *exec, const ingress_msg_t *msg);
static void attempt_terminal_delivery(wash_executor_t *exec);
static uint32_t check_deadlines(wash_executor_t *exec);
static void publish_snapshot(wash_executor_t *exec);

/* ---- Time helper ---- */

static uint32_t get_now_ms(wash_executor_t *exec)
{
    if (exec->config.get_now_ms) {
        return exec->config.get_now_ms(exec->config.time_ctx);
    }
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static uint32_t time_remaining(uint32_t now, uint32_t deadline)
{
    /* Wrap-safe: if deadline has passed, remaining is 0 */
    uint32_t elapsed = now - deadline;  /* wraps correctly for unsigned */
    if (elapsed <= 0x7FFFFFFFU) return 0;  /* deadline passed */
    return deadline - now;
}

/* ---- Snapshot publishing (task-only, under mutex) ---- */

static wash_exec_state_t map_core_phase(core_phase_t phase)
{
    switch (phase) {
    case CORE_PHASE_IDLE:        return WASH_EXEC_STATE_IDLE;
    case CORE_PHASE_DISPATCHING: return WASH_EXEC_STATE_RUNNING;
    case CORE_PHASE_ACTIVE:      return WASH_EXEC_STATE_RUNNING;
    case CORE_PHASE_CANCELING:   return WASH_EXEC_STATE_RUNNING;
    case CORE_PHASE_RESETTING:   return WASH_EXEC_STATE_RESETTING;
    case CORE_PHASE_WAIT_LOAD:   return WASH_EXEC_STATE_WAIT_LOAD;
    case CORE_PHASE_WAIT_UNLOAD: return WASH_EXEC_STATE_WAIT_UNLOAD;
    case CORE_PHASE_FAULT:       return WASH_EXEC_STATE_FAULT;
    default:                     return WASH_EXEC_STATE_IDLE;
    }
}

static wash_phase_t map_step_phase(const wash_step_t *step)
{
    if (!step) return WASH_PHASE_IDLE;
    switch (step->type) {
    case STEP_MOVE_POSITION: return WASH_PHASE_MOVE_POSITION;
    case STEP_HOME_BL50: return WASH_PHASE_HOME_BL50;
    case STEP_WATER_IN: return WASH_PHASE_WATER_SOURCE_FILL;
    case STEP_DETERGENT: return WASH_PHASE_DETERGENT;
    case STEP_PULSATOR_WASH: return WASH_PHASE_PULSATOR_WASH;
    case STEP_DRUM_WASH: return WASH_PHASE_DRUM_WASH;
    case STEP_DRAIN: return WASH_PHASE_DRAIN;
    case STEP_SPIN: return WASH_PHASE_SPIN;
    case STEP_DRY: return WASH_PHASE_DRY;
    case STEP_UV: return WASH_PHASE_UV;
    case STEP_FINISH: return WASH_PHASE_DONE;
    default: return WASH_PHASE_IDLE;
    }
}

static machine_state_t map_machine_state(
    const wash_executor_core_t *core, const wash_step_t *step)
{
    if (core->phase == CORE_PHASE_FAULT) return MACHINE_STATE_FAULT;
    if (core->phase == CORE_PHASE_RESETTING) return MACHINE_STATE_RESETTING;
    if (core->phase == CORE_PHASE_WAIT_LOAD) return MACHINE_STATE_WAITING_LOAD;
    if (core->phase == CORE_PHASE_WAIT_UNLOAD) return MACHINE_STATE_WAITING_UNLOAD;
    if ((core->phase == CORE_PHASE_ACTIVE ||
         core->phase == CORE_PHASE_DISPATCHING) &&
        step && step->type == STEP_UV) {
        return MACHINE_STATE_UV;
    }
    if (core->program_active) return MACHINE_STATE_RUNNING;
    return MACHINE_STATE_IDLE;
}

static void publish_snapshot(wash_executor_t *exec)
{
    wash_exec_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    snap.state = map_core_phase(exec->core.phase);
    snap.last_event = exec->core.last_event;
    snap.program_id = exec->core.program_id;
    snap.current_step_id = exec->core.token_valid
                           ? exec->core.active_token.step_id : 0;
    snap.current_request_id = exec->core.token_valid
                              ? exec->core.active_token.request_id : 0;
    snap.completed_steps = exec->core.completed_steps;
    snap.total_steps = exec->core.total_steps;
    snap.generation = exec->core.generation;
    snap.program_active = exec->core.program_active;
    snap.terminal_pending = (exec->terminal_state != TERM_STATE_IDLE);
    snap.terminal_attempt_count = exec->terminal_attempt_count;
    snap.adapter_in_flight = exec->adapter_in_flight;
    const wash_step_t *step = core_get_current_step(&exec->core);
    snap.current_step_type = step ? step->type : STEP_FINISH;
    snap.current_step_skippable = core_can_skip_current_step(&exec->core);

    xSemaphoreTake(exec->mutex, portMAX_DELAY);
    exec->published_snapshot = snap;
    xSemaphoreGive(exec->mutex);

    machine_execution_status_t status;
    memset(&status, 0, sizeof(status));
    status.state = map_machine_state(&exec->core, step);
    status.phase = map_step_phase(step);
    if (!exec->core.program_active &&
        exec->core.last_event == WASH_EXEC_EVENT_PROGRAM_COMPLETE) {
        status.phase = WASH_PHASE_DONE;
    }
    status.program_id = exec->core.program_id;
    status.current_step = snap.current_step_id;
    status.total_steps = snap.total_steps;
    status.progress_percent = snap.total_steps > 0
        ? (uint8_t)(((uint32_t)snap.completed_steps * 100U) / snap.total_steps)
        : 0;
    if (exec->program_started_valid) {
        uint32_t elapsed = get_now_ms(exec) - exec->program_started_ms;
        uint32_t total = exec->active_program.estimated_total_ms;
        status.elapsed_ms = total > 0 && elapsed > total ? total : elapsed;
        status.remaining_ms = total > status.elapsed_ms
            ? total - status.elapsed_ms : 0;
    }
    status.target_position = step ? step->required_position : DRUM_POS_UNKNOWN;
    /*
     * Real service terminals carry machine_fault_code_t in core.fault_code.
     * Preserve that identity instead of reporting the misleading
     * "FAULT_INTERNAL/detail=<real code>" pair. Executor-owned codes use
     * the documented 100/200/300/400 ranges and remain FAULT_INTERNAL.
     */
    if (!exec->core.fault_active) {
        status.fault.code = FAULT_NONE;
    } else if (exec->core.fault_code > (uint32_t)FAULT_NONE &&
               exec->core.fault_code <= (uint32_t)FAULT_INTERNAL) {
        status.fault.code = (machine_fault_code_t)exec->core.fault_code;
    } else {
        status.fault.code = FAULT_INTERNAL;
    }
    status.fault.severity = exec->core.fault_active
        ? FAULT_SEVERITY_RECOVERABLE : FAULT_SEVERITY_WARNING;
    status.fault.detail =
        (status.fault.code == FAULT_INTERNAL) ? exec->core.fault_code : 0;
    (void)machine_status_store_update_execution(&status);
}

/* ---- Wake helper — sends to stop queue in QueueSet ---- */

static void wake_executor(wash_executor_t *exec)
{
    if (exec->queue_stop) {
        uint8_t dummy = 0;
        xQueueSend(exec->queue_stop, &dummy, 0);
    }
}

/* ---- Active stop drain: called by task when stop_requested ---- */

static void perform_active_stop(wash_executor_t *exec)
{
    core_phase_t phase = exec->core.phase;

    /* If no program active or already idle, nothing to cancel */
    if (!exec->core.program_active && exec->terminal_state == TERM_STATE_IDLE) {
        return;
    }

    /* If in ACTIVE or DISPATCHING, inject cancel */
    if (phase == CORE_PHASE_ACTIVE || phase == CORE_PHASE_DISPATCHING) {
        core_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = CORE_EVENT_CANCEL;
        process_core_event_and_drain(exec, &ev);
        publish_snapshot(exec);

        /* Wait for cancel ACK (with deadline-driven timeout) */
        uint32_t cancel_deadline = get_now_ms(exec) + exec->config.cancel_timeout_ms;
        while (exec->core.phase == CORE_PHASE_CANCELING) {
            uint32_t now = get_now_ms(exec);
            if (time_remaining(now, cancel_deadline) == 0) break;

            /* Process any pending service terminals */
            ingress_msg_t msg;
            if (xQueueReceive(exec->queue_service, &msg, pdMS_TO_TICKS(10)) == pdTRUE) {
                if (msg.type == INGRESS_SERVICE_TERMINAL) {
                    core_event_t tev;
                    memset(&tev, 0, sizeof(tev));
                    tev.type = CORE_EVENT_SERVICE_TERMINAL;
                    tev.token = msg.token;
                    tev.success = msg.success;
                    tev.error_code = msg.error_code;
                    process_core_event_and_drain(exec, &tev);
                    publish_snapshot(exec);
                }
            }
            /* Check deadlines (handles cancel timeout) */
            check_deadlines(exec);
        }
    }

    /* If in RESETTING (cancel succeeded), wait for reset ACK */
    if (exec->core.phase == CORE_PHASE_RESETTING) {
        uint32_t reset_deadline = get_now_ms(exec) + exec->config.reset_timeout_ms;
        while (exec->core.phase == CORE_PHASE_RESETTING) {
            uint32_t now = get_now_ms(exec);
            if (time_remaining(now, reset_deadline) == 0) break;

            ingress_msg_t msg;
            if (xQueueReceive(exec->queue_service, &msg, pdMS_TO_TICKS(10)) == pdTRUE) {
                if (msg.type == INGRESS_SERVICE_TERMINAL) {
                    core_event_t tev;
                    memset(&tev, 0, sizeof(tev));
                    tev.type = CORE_EVENT_SERVICE_TERMINAL;
                    tev.token = msg.token;
                    tev.success = msg.success;
                    tev.error_code = msg.error_code;
                    process_core_event_and_drain(exec, &tev);
                    publish_snapshot(exec);
                }
            }
            check_deadlines(exec);
        }
    }

    /* If a terminal is pending, deliver it (unbounded — task stays alive
     * until terminal delivered. stop() timeout handles caller abort.) */
    while (exec->terminal_state != TERM_STATE_IDLE) {
        attempt_terminal_delivery(exec);
        if (exec->terminal_state != TERM_STATE_IDLE) {
            /* Wait for terminal_retry_ms before retrying */
            uint32_t retry_ms = exec->config.terminal_retry_ms > 0
                                ? exec->config.terminal_retry_ms : 100;
            vTaskDelay(pdMS_TO_TICKS(retry_ms));
        }
    }
}

/* ---- Command pump: non-recursive FIFO ---- */

typedef struct {
    core_event_t events[CMD_PUMP_MAX_ITERATIONS];
    uint32_t ev_count;
    uint32_t ev_head;
} event_fifo_t;

static void fifo_push_event(event_fifo_t *f, const core_event_t *ev)
{
    if (f->ev_count < CMD_PUMP_MAX_ITERATIONS) {
        uint32_t idx = (f->ev_head + f->ev_count) % CMD_PUMP_MAX_ITERATIONS;
        f->events[idx] = *ev;
        f->ev_count++;
    }
}

static bool fifo_pop_event(event_fifo_t *f, core_event_t *out)
{
    if (f->ev_count == 0) return false;
    *out = f->events[f->ev_head];
    f->ev_head = (f->ev_head + 1) % CMD_PUMP_MAX_ITERATIONS;
    f->ev_count--;
    return true;
}

/* Process a single command. May push events to the FIFO. */
static void execute_single_cmd(
    wash_executor_t *exec,
    const core_cmd_t *cmd,
    event_fifo_t *fifo)
{
    switch (cmd->type) {
    case CORE_CMD_DISPATCH_STEP: {
        if (!exec->adapter || !exec->adapter->dispatch_step) break;
        const wash_step_t *step = core_get_current_step(&exec->core);
        if (!step) break;

        core_event_t de;
        memset(&de, 0, sizeof(de));

        /* 内部步骤（STEP_SETTLE / STEP_AUDIT_*）：不经 real_adapter 派发。
         * 由本包装层定时/门禁完成。 */
        if (step->type == STEP_SETTLE) {
            uint32_t dur = step->duration_ms > 0
                ? step->duration_ms : WASH_EXEC_INTERNAL_SETTLE_MS;
            exec->internal_step_active = true;
            exec->step_deadline_ms = get_now_ms(exec) + dur;
            exec->step_deadline_active = true;
            de.type = CORE_EVENT_STEP_DISPATCH_OK;
            fifo_push_event(fifo, &de);
            break;
        }
        if (step->type == STEP_AUDIT_UV_OFF ||
            step->type == STEP_AUDIT_SAFE_OFF) {
            bool pass = exec->audit_fn
                ? exec->audit_fn(step->type, exec->audit_ctx) : false;
            if (pass) {
                /* 审计通过：下一 deadline tick 立即发布成功终态 */
                exec->internal_step_active = true;
                exec->step_deadline_ms = get_now_ms(exec);
                exec->step_deadline_active = true;
                de.type = CORE_EVENT_STEP_DISPATCH_OK;
            } else {
                /* 审计失败/未注册 sink → fail-closed 进入故障 */
                de.type = CORE_EVENT_STEP_DISPATCH_FAIL;
                de.error_code = WASH_EXEC_ERR_AUDIT_FAIL;
            }
            fifo_push_event(fifo, &de);
            break;
        }

        exec->adapter_in_flight = true;
        adapter_dispatch_result_t r = exec->adapter->dispatch_step(
            exec->adapter->ctx, step, &cmd->token, sizeof(cmd->token));
        exec->adapter_in_flight = false;

        uint32_t timeout = step->timeout_ms > 0
            ? step->timeout_ms : exec->config.step_timeout_default_ms;
        if (timeout > 0) {
            exec->step_deadline_ms = get_now_ms(exec) + timeout;
            exec->step_deadline_active = true;
        }

        de.type = r.accepted ? CORE_EVENT_STEP_DISPATCH_OK
                             : CORE_EVENT_STEP_DISPATCH_FAIL;
        fifo_push_event(fifo, &de);
        break;
    }
    case CORE_CMD_CANCEL_ACTIVE_STEP: {
        if (exec->internal_step_active) {
            /* 内部步骤取消：无服务可取消，直接成功并清除定时器 */
            exec->internal_step_active = false;
            exec->step_deadline_active = false;
            core_event_t cancel_ok;
            memset(&cancel_ok, 0, sizeof(cancel_ok));
            cancel_ok.type = CORE_EVENT_SERVICE_TERMINAL;
            cancel_ok.token = cmd->token;   /* cancel_token（CANCEL_ACK） */
            cancel_ok.success = true;
            fifo_push_event(fifo, &cancel_ok);
            break;
        }
        bool accepted = false;
        if (exec->adapter && exec->adapter->cancel_step) {
            accepted = exec->adapter->cancel_step(
                exec->adapter->ctx, &cmd->token, sizeof(cmd->token));
        }
        if (!accepted) {
            core_event_t cancel_fail;
            memset(&cancel_fail, 0, sizeof(cancel_fail));
            cancel_fail.type = CORE_EVENT_SERVICE_TERMINAL;
            cancel_fail.token = cmd->token;
            cancel_fail.success = false;
            cancel_fail.error_code = 300;
            fifo_push_event(fifo, &cancel_fail);
            exec->cancel_deadline_active = false;
        } else if (exec->config.cancel_timeout_ms > 0) {
            exec->cancel_deadline_ms = get_now_ms(exec) + exec->config.cancel_timeout_ms;
            exec->cancel_deadline_active = true;
        }
        break;
    }
    case CORE_CMD_REQUEST_FINAL_RESET: {
        if (exec->adapter && exec->adapter->dispatch_final_reset) {
            exec->adapter_in_flight = true;
            adapter_dispatch_result_t r = exec->adapter->dispatch_final_reset(
                exec->adapter->ctx, &cmd->token, sizeof(cmd->token));
            exec->adapter_in_flight = false;
            if (!r.accepted) {
                core_event_t re;
                memset(&re, 0, sizeof(re));
                re.type = CORE_EVENT_SERVICE_TERMINAL;
                re.token = cmd->token;
                re.success = false;
                re.error_code = r.error_code > 0 ? r.error_code : 500;
                fifo_push_event(fifo, &re);
            } else {
                if (exec->config.reset_timeout_ms > 0) {
                    exec->reset_deadline_ms = get_now_ms(exec) + exec->config.reset_timeout_ms;
                    exec->reset_deadline_active = true;
                }
            }
        } else {
            core_event_t re;
            memset(&re, 0, sizeof(re));
            re.type = CORE_EVENT_SERVICE_TERMINAL;
            re.token = cmd->token;
            re.success = true;
            fifo_push_event(fifo, &re);
        }
        break;
    }
    case CORE_CMD_PUBLISH_PROGRAM_TERMINAL: {
        memset(&exec->pending_terminal, 0, sizeof(exec->pending_terminal));
        exec->pending_terminal.program_id = cmd->program_id;
        exec->pending_terminal.result = cmd->terminal_result;
        exec->pending_terminal.error_code = cmd->terminal_error;
        /* Use explicit evidence from core, not result-based derivation */
        exec->pending_terminal.final_position_confirmed =
            cmd->final_position_confirmed;
        exec->pending_terminal.completed_steps = exec->core.completed_steps;
        exec->terminal_state = TERM_STATE_PENDING;
        exec->terminal_attempt_count = 0;
        exec->terminal_needs_delivery = true;
        exec->terminal_next_retry_ms = 0;

        /* Clear all step/cancel/reset deadlines on program terminal */
        exec->step_deadline_active = false;
        exec->cancel_deadline_active = false;
        exec->reset_deadline_active = false;
        /* Defensive: no internal step may remain latched across a terminal */
        exec->internal_step_active = false;
        break;
    }
    case CORE_CMD_ENTER_FAULT:
        ESP_LOGE(TAG, "FAULT: code=%lu result=%u",
                 (unsigned long)cmd->terminal_error, cmd->terminal_result);
        break;
    case CORE_CMD_RETURN_IDLE:
        ESP_LOGI(TAG, "returned to IDLE");
        break;
    default:
        break;
    }
}

/* Main pump: non-recursive, bounded by CMD_PUMP_MAX_ITERATIONS */
static void process_core_event_and_drain(
    wash_executor_t *exec,
    const core_event_t *event)
{
    event_fifo_t fifo;
    memset(&fifo, 0, sizeof(fifo));
    fifo_push_event(&fifo, event);

    uint32_t total_processed = 0;
    core_event_t cur;

    while (fifo_pop_event(&fifo, &cur) && total_processed < CMD_PUMP_MAX_ITERATIONS) {
        core_cmd_t cmds[CMD_PUMP_MAX_ITERATIONS];
        uint32_t count = core_process_event(
            &exec->core, &cur, cmds, CMD_PUMP_MAX_ITERATIONS);

        for (uint32_t i = 0; i < count && i < CMD_PUMP_MAX_ITERATIONS; i++) {
            execute_single_cmd(exec, &cmds[i], &fifo);
        }
        total_processed++;
    }

    if (total_processed >= CMD_PUMP_MAX_ITERATIONS) {
        ESP_LOGE(TAG, "command pump overflow — injecting FAULT via core event");
        /* Inject dispatch-fail event through core state machine.
         * This works from DISPATCHING and ACTIVE phases.
         * If not in those phases, directly set fault state. */
        if (exec->core.phase == CORE_PHASE_DISPATCHING ||
            exec->core.phase == CORE_PHASE_ACTIVE) {
            core_event_t fault_ev;
            memset(&fault_ev, 0, sizeof(fault_ev));
            fault_ev.type = CORE_EVENT_STEP_DISPATCH_FAIL;
            fault_ev.error_code = 999;
            event_fifo_t overflow_fifo;
            memset(&overflow_fifo, 0, sizeof(overflow_fifo));
            core_cmd_t fault_cmds[4];
            uint32_t fault_count = core_process_event(
                &exec->core, &fault_ev, fault_cmds, 4);
            for (uint32_t i = 0; i < fault_count; i++) {
                execute_single_cmd(exec, &fault_cmds[i], &overflow_fifo);
            }
        } else {
            /* Core not in a dispatchable phase — direct fault */
            exec->core.fault_active = true;
            exec->core.fault_code = 999;
            if (exec->core.program_active) {
                exec->core.program_active = false;
                memset(&exec->pending_terminal, 0, sizeof(exec->pending_terminal));
                exec->pending_terminal.program_id = exec->core.program_id;
                exec->pending_terminal.result = TERM_RESULT_FAULT;
                exec->pending_terminal.error_code = 999;
                exec->pending_terminal.final_position_confirmed = false;
                exec->terminal_state = TERM_STATE_PENDING;
                exec->terminal_attempt_count = 0;
                exec->terminal_needs_delivery = true;
                exec->terminal_next_retry_ms = 0;
            }
        }
    }
}

/* ---- Terminal delivery attempt ---- */

static void attempt_terminal_delivery(wash_executor_t *exec)
{
    if (exec->terminal_state != TERM_STATE_PENDING) return;
    if (!exec->terminal_needs_delivery) return;

    uint32_t now = get_now_ms(exec);
    uint32_t retry_ms = exec->config.terminal_retry_ms > 0
                        ? exec->config.terminal_retry_ms : 100;

    /* Check retry deadline */
    if (exec->terminal_attempt_count > 0 &&
        time_remaining(now, exec->terminal_next_retry_ms) > 0) {
        return;  /* not yet time to retry */
    }

    exec->terminal_state = TERM_STATE_IN_FLIGHT;
    bool delivered = false;

    if (exec->terminal_sink && exec->terminal_sink_ctx) {
        delivered = exec->terminal_sink(&exec->pending_terminal,
                                        exec->terminal_sink_ctx);
    } else {
        /* No sink configured — treat as delivered */
        delivered = true;
    }

    exec->terminal_attempt_count++;

    if (delivered) {
        /* Success — release reservation */
        exec->terminal_state = TERM_STATE_IDLE;
        exec->terminal_needs_delivery = false;
        xSemaphoreTake(exec->mutex, portMAX_DELAY);
        exec->program_reserved = false;
        xSemaphoreGive(exec->mutex);
    } else {
        /* Failed — schedule retry */
        exec->terminal_state = TERM_STATE_PENDING;
        exec->terminal_next_retry_ms = now + retry_ms;
    }
}

/* ---- Deadline check ---- */

static uint32_t check_deadlines(wash_executor_t *exec)
{
    uint32_t now = get_now_ms(exec);
    uint32_t min_wait_ms = 1000;  /* default max wait */

    /* Step deadline */
    if (exec->step_deadline_active) {
        if (time_remaining(now, exec->step_deadline_ms) == 0) {
            exec->step_deadline_active = false;
            core_event_t ev;
            memset(&ev, 0, sizeof(ev));
            if (exec->internal_step_active) {
                /* 内部步骤到期 = 成功终态（SETTLE 时长到 / AUDIT 立即通过） */
                exec->internal_step_active = false;
                ev.type = CORE_EVENT_SERVICE_TERMINAL;
                ev.success = true;
                if (exec->core.token_valid) {
                    ev.token = exec->core.active_token;
                }
            } else {
                ev.type = CORE_EVENT_STEP_TIMEOUT;
            }
            process_core_event_and_drain(exec, &ev);
            publish_snapshot(exec);
        } else {
            uint32_t remaining = exec->step_deadline_ms - now;
            if (remaining < min_wait_ms) min_wait_ms = remaining;
        }
    }

    /* Cancel deadline */
    if (exec->cancel_deadline_active) {
        if (time_remaining(now, exec->cancel_deadline_ms) == 0) {
            exec->cancel_deadline_active = false;
            /* Cancel timeout → fault */
            core_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = CORE_EVENT_SERVICE_TERMINAL;
            ev.success = false;
            ev.error_code = 600;
            if (exec->core.cancel_token_valid) {
                ev.token = exec->core.cancel_token;
                ev.token.terminal_kind = TERMINAL_KIND_CANCEL_ACK;
            }
            process_core_event_and_drain(exec, &ev);
            publish_snapshot(exec);
        } else {
            uint32_t remaining = exec->cancel_deadline_ms - now;
            if (remaining < min_wait_ms) min_wait_ms = remaining;
        }
    }

    /* Reset deadline */
    if (exec->reset_deadline_active) {
        if (time_remaining(now, exec->reset_deadline_ms) == 0) {
            exec->reset_deadline_active = false;
            core_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = CORE_EVENT_SERVICE_TERMINAL;
            ev.success = false;
            ev.error_code = 700;
            if (exec->core.reset_token_valid) {
                ev.token = exec->core.reset_token;
                ev.token.terminal_kind = TERMINAL_KIND_RESET_ACK;
            }
            process_core_event_and_drain(exec, &ev);
            publish_snapshot(exec);
        } else {
            uint32_t remaining = exec->reset_deadline_ms - now;
            if (remaining < min_wait_ms) min_wait_ms = remaining;
        }
    }

    /* Terminal retry */
    if (exec->terminal_state == TERM_STATE_PENDING) {
        if (exec->terminal_attempt_count > 0) {
            if (time_remaining(now, exec->terminal_next_retry_ms) == 0) {
                /* Will be handled by attempt_terminal_delivery */
                min_wait_ms = 0;
            } else {
                uint32_t remaining = exec->terminal_next_retry_ms - now;
                if (remaining < min_wait_ms) min_wait_ms = remaining;
            }
        } else {
            min_wait_ms = 0;  /* immediate first attempt */
        }
    }

    return min_wait_ms;
}

/* ---- Ingress message processing (extracted from executor task) ---- */

static void process_ingress_msg(wash_executor_t *exec, const ingress_msg_t *msg)
{
    switch (msg->type) {
    case INGRESS_PROGRAM: {
        /* Reject if terminal pending (reservation held) */
        if (exec->terminal_state != TERM_STATE_IDLE) {
            /* Re-enqueue not possible — drop and log */
            ESP_LOGW(TAG, "program rejected: terminal pending");
            xSemaphoreTake(exec->mutex, portMAX_DELAY);
            exec->program_reserved = false;
            xSemaphoreGive(exec->mutex);
            break;
        }
        xSemaphoreTake(exec->mutex, portMAX_DELAY);
        exec->active_program = exec->pending_program;
        exec->program_reserved = true;
        xSemaphoreGive(exec->mutex);

        exec->core.program = &exec->active_program;
        exec->core.total_steps = (uint16_t)exec->active_program.step_count;
        exec->program_started_ms = get_now_ms(exec);
        exec->program_started_valid = true;

        core_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = CORE_EVENT_PROGRAM_ACCEPTED;
        ev.token.program_id = msg->program_id;
        process_core_event_and_drain(exec, &ev);
        publish_snapshot(exec);
        break;
    }
    case INGRESS_SERVICE_TERMINAL: {
        /* Save old phase/token state before processing */
        core_phase_t old_phase = exec->core.phase;
        bool was_cancel_token = exec->core.cancel_token_valid;
        bool was_reset_token = exec->core.reset_token_valid;

        core_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = CORE_EVENT_SERVICE_TERMINAL;
        ev.token = msg->token;
        ev.success = msg->success;
        ev.error_code = msg->error_code;
        process_core_event_and_drain(exec, &ev);

        /* Clear matching deadline based on OLD state */
        if (old_phase == CORE_PHASE_ACTIVE &&
            msg->token.terminal_kind == TERMINAL_KIND_NORMAL) {
            exec->step_deadline_active = false;
        }
        if (old_phase == CORE_PHASE_DISPATCHING &&
            !msg->success) {
            exec->step_deadline_active = false;
        }
        if (was_cancel_token &&
            msg->token.terminal_kind == TERMINAL_KIND_CANCEL_ACK) {
            exec->cancel_deadline_active = false;
        }
        if (was_reset_token &&
            msg->token.terminal_kind == TERMINAL_KIND_RESET_ACK) {
            exec->reset_deadline_active = false;
        }
        publish_snapshot(exec);
        break;
    }
    case INGRESS_URGENT: {
        core_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = msg->abort_flag ? CORE_EVENT_ABORT : CORE_EVENT_CANCEL;
        process_core_event_and_drain(exec, &ev);
        publish_snapshot(exec);
        break;
    }
    case INGRESS_LOAD_CONFIRM: {
        core_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = CORE_EVENT_LOAD_CONFIRMED;
        process_core_event_and_drain(exec, &ev);
        publish_snapshot(exec);
        break;
    }
    case INGRESS_UNLOAD_CONFIRM: {
        core_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = CORE_EVENT_UNLOAD_CONFIRMED;
        process_core_event_and_drain(exec, &ev);
        publish_snapshot(exec);
        break;
    }
    case INGRESS_FAULT_ACK: {
        core_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = CORE_EVENT_FAULT_ACK;
        process_core_event_and_drain(exec, &ev);
        publish_snapshot(exec);
        break;
    }
    case INGRESS_SKIP: {
        core_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = CORE_EVENT_SKIP_STEP;
        process_core_event_and_drain(exec, &ev);
        publish_snapshot(exec);
        break;
    }
    default:
        break;
    }
}

/* ---- Executor task ---- */

static void executor_task(void *arg)
{
    wash_executor_t *exec = (wash_executor_t *)arg;

    xSemaphoreTake(exec->mutex, portMAX_DELAY);
    /* Generation already set by start() — do NOT re-increment */
    exec->lifecycle = EXEC_LIFECYCLE_RUNNING;
    exec->task_done_persistent = false;
    xSemaphoreGive(exec->mutex);

    xEventGroupSetBits(exec->events, EVT_TASK_READY);

#ifdef WASH_EXEC_TEST_HOOKS
    if (s_test_hooks.task_ready_barrier)
        s_test_hooks.task_ready_barrier(s_test_hooks.task_ready_ctx);
#endif

    while (true) {
        /* Check stop flag */
        bool should_stop;
        xSemaphoreTake(exec->mutex, portMAX_DELAY);
        should_stop = exec->stop_requested;
        xSemaphoreGive(exec->mutex);

        if (should_stop) {
            /* Active stop: cancel running program, flush terminal */
            perform_active_stop(exec);
            break;
        }

        /* Attempt terminal delivery if pending */
        attempt_terminal_delivery(exec);

        /* Check deadlines */
        uint32_t deadline_wait = check_deadlines(exec);

        /* Calculate wait time for QueueSet */
        TickType_t wait_ticks;
        if (exec->terminal_state == TERM_STATE_PENDING &&
            exec->terminal_attempt_count == 0) {
            wait_ticks = 0;  /* immediate delivery attempt */
        } else if (deadline_wait == 0) {
            wait_ticks = 0;
        } else {
            wait_ticks = pdMS_TO_TICKS(deadline_wait);
            if (wait_ticks == 0) wait_ticks = 1;
        }

        /* Use QueueSet to block until any queue has data */
        QueueHandle_t active_queue = xQueueSelectFromSet(
            exec->queue_set, wait_ticks);

        if (active_queue) {
            /* Drain stop queue — just a wake signal, discard the byte */
            if (active_queue == exec->queue_stop) {
                uint8_t dummy;
                xQueueReceive(exec->queue_stop, &dummy, 0);
            }

            /* Bounded drain: process up to MAX_DRAIN_PER_WAKE messages */
            int drain_count = 0;
            ingress_msg_t msg;

            /* First, drain the activated queue (if it's a message queue) */
            if (active_queue != exec->queue_stop &&
                xQueueReceive(active_queue, &msg, 0) == pdTRUE) {
                process_ingress_msg(exec, &msg);
                drain_count++;

                /* Then drain remaining from ALL queues in priority order */
                while (drain_count < MAX_DRAIN_PER_WAKE) {
                    bool got = false;
                    if (xQueueReceive(exec->queue_urgent, &msg, 0) == pdTRUE) {
                        process_ingress_msg(exec, &msg);
                        got = true;
                    } else if (xQueueReceive(exec->queue_service, &msg, 0) == pdTRUE) {
                        process_ingress_msg(exec, &msg);
                        got = true;
                    } else if (xQueueReceive(exec->queue_program, &msg, 0) == pdTRUE) {
                        process_ingress_msg(exec, &msg);
                        got = true;
                    }
                    if (!got) break;
                    drain_count++;
                }
            } else if (active_queue == exec->queue_stop) {
                /* Wake from stop — drain any pending message queues */
                while (drain_count < MAX_DRAIN_PER_WAKE) {
                    bool got = false;
                    if (xQueueReceive(exec->queue_urgent, &msg, 0) == pdTRUE) {
                        process_ingress_msg(exec, &msg);
                        got = true;
                    } else if (xQueueReceive(exec->queue_service, &msg, 0) == pdTRUE) {
                        process_ingress_msg(exec, &msg);
                        got = true;
                    } else if (xQueueReceive(exec->queue_program, &msg, 0) == pdTRUE) {
                        process_ingress_msg(exec, &msg);
                        got = true;
                    }
                    if (!got) break;
                    drain_count++;
                }
            }
        }

        publish_snapshot(exec);
    }

    /* Three-state join: task_exit_ready → join_claimed → join_complete.
     * Task sets exit_ready under mutex, signals, then suspends.
     * Stop owner claims join, deletes task, sets join_complete. */

#ifdef WASH_EXEC_TEST_HOOKS
    if (s_test_hooks.task_exit_barrier)
        s_test_hooks.task_exit_barrier(s_test_hooks.task_exit_ctx);
#endif

    xSemaphoreTake(exec->mutex, portMAX_DELAY);
    exec->task_exit_ready = true;
    exec->exit_ready_generation = exec->task_generation;
    exec->task_done_persistent = true;
    /* Do NOT set lifecycle to STOPPED here — stop owner does that after delete */
    xSemaphoreGive(exec->mutex);

    xEventGroupSetBits(exec->events, EVT_TASK_DONE);
    xSemaphoreGive(exec->join_sem);
    vTaskSuspend(NULL);
}

/* ---- Public API ---- */

wash_executor_t *wash_executor_create(
    const wash_executor_config_t *config,
    wash_executor_adapter_t *adapter)
{
    wash_executor_t *exec = calloc(1, sizeof(wash_executor_t));
    if (!exec) return NULL;

    if (config) {
        exec->config = *config;
    } else {
        exec->config.step_timeout_default_ms = 60000;
        exec->config.reset_timeout_ms = 30000;
        exec->config.cancel_timeout_ms = 30000;
        exec->config.terminal_retry_ms = 1000;
    }

    /* Apply defaults for unset values */
    if (exec->config.step_timeout_default_ms == 0)
        exec->config.step_timeout_default_ms = 60000;
    if (exec->config.reset_timeout_ms == 0)
        exec->config.reset_timeout_ms = 30000;
    if (exec->config.cancel_timeout_ms == 0)
        exec->config.cancel_timeout_ms = 30000;
    if (exec->config.terminal_retry_ms == 0)
        exec->config.terminal_retry_ms = 1000;

    exec->adapter = adapter;
    core_init(&exec->core);

    exec->queue_program = xQueueCreate(1, sizeof(ingress_msg_t));
    exec->queue_service = xQueueCreate(QUEUE_SERVICE_DEPTH, sizeof(ingress_msg_t));
    exec->queue_urgent = xQueueCreate(QUEUE_URGENT_DEPTH, sizeof(ingress_msg_t));
    exec->queue_stop = xQueueCreate(1, sizeof(uint8_t));
    exec->mutex = xSemaphoreCreateMutex();
    exec->events = xEventGroupCreate();
    exec->join_sem = xSemaphoreCreateBinary();

    if (!exec->queue_program || !exec->queue_service ||
        !exec->queue_urgent || !exec->queue_stop ||
        !exec->mutex || !exec->events || !exec->join_sem) {
        ESP_LOGE(TAG, "resource creation failed");
        if (exec->queue_program) vQueueDelete(exec->queue_program);
        if (exec->queue_service) vQueueDelete(exec->queue_service);
        if (exec->queue_urgent) vQueueDelete(exec->queue_urgent);
        if (exec->queue_stop) vQueueDelete(exec->queue_stop);
        if (exec->mutex) vSemaphoreDelete(exec->mutex);
        if (exec->events) vEventGroupDelete(exec->events);
        if (exec->join_sem) vSemaphoreDelete(exec->join_sem);
        free(exec);
        return NULL;
    }

    /* Create QueueSet: capacity = sum of all member queue depths */
    QueueSetHandle_t qset = xQueueCreateSet(
        QUEUE_URGENT_DEPTH + QUEUE_SERVICE_DEPTH + 1 + 1);
    if (!qset) {
        ESP_LOGE(TAG, "queue set creation failed");
        vQueueDelete(exec->queue_program);
        vQueueDelete(exec->queue_service);
        vQueueDelete(exec->queue_urgent);
        vQueueDelete(exec->queue_stop);
        vSemaphoreDelete(exec->mutex);
        vEventGroupDelete(exec->events);
        vSemaphoreDelete(exec->join_sem);
        free(exec);
        return NULL;
    }

    if (xQueueAddToSet(exec->queue_urgent, qset) != pdPASS ||
        xQueueAddToSet(exec->queue_service, qset) != pdPASS ||
        xQueueAddToSet(exec->queue_program, qset) != pdPASS ||
        xQueueAddToSet(exec->queue_stop, qset) != pdPASS) {
        ESP_LOGE(TAG, "queue add-to-set failed");
        vQueueDelete(qset);
        vQueueDelete(exec->queue_program);
        vQueueDelete(exec->queue_service);
        vQueueDelete(exec->queue_urgent);
        vQueueDelete(exec->queue_stop);
        vSemaphoreDelete(exec->mutex);
        vEventGroupDelete(exec->events);
        vSemaphoreDelete(exec->join_sem);
        free(exec);
        return NULL;
    }
    exec->queue_set = qset;

    exec->lifecycle = EXEC_LIFECYCLE_INITIALIZED;
    return exec;
}

void wash_executor_set_terminal_sink(
    wash_executor_t *exec,
    program_terminal_sink_fn sink,
    void *sink_ctx)
{
    if (!exec) return;
    exec->terminal_sink = sink;
    exec->terminal_sink_ctx = sink_ctx;
}

void wash_executor_set_audit_sink(
    wash_executor_t *exec,
    wash_executor_audit_fn fn,
    void *ctx)
{
    if (!exec) return;
    exec->audit_fn = fn;
    exec->audit_ctx = ctx;
}

esp_err_t wash_executor_start(wash_executor_t *exec)
{
    if (!exec) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(exec->mutex, portMAX_DELAY);
    if (exec->lifecycle != EXEC_LIFECYCLE_INITIALIZED &&
        exec->lifecycle != EXEC_LIFECYCLE_STOPPED) {
        xSemaphoreGive(exec->mutex);
        return ESP_ERR_INVALID_STATE;
    }
    exec->lifecycle = EXEC_LIFECYCLE_STARTING;
    exec->task_done_persistent = false;
    exec->task_exit_ready = false;
    exec->exit_ready_generation = 0;
    exec->join_claimed = false;
    exec->join_complete = false;
    exec->join_complete_generation = 0;
    exec->stop_requested = false;
    /* Generation: wrap-skip-0, single increment per start */
    exec->task_generation++;
    if (exec->task_generation == 0) exec->task_generation = 1;
    xSemaphoreGive(exec->mutex);

    xEventGroupClearBits(exec->events, EVT_TASK_READY | EVT_TASK_DONE | EVT_JOIN_COMPLETE);

    BaseType_t ret = xTaskCreate(
        executor_task, "wash_exec", 4096, exec, 5, &exec->task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        xSemaphoreTake(exec->mutex, portMAX_DELAY);
        exec->lifecycle = EXEC_LIFECYCLE_FAILED;
        xSemaphoreGive(exec->mutex);
        return ESP_FAIL;
    }

    EventBits_t bits = xEventGroupWaitBits(
        exec->events, EVT_TASK_READY | EVT_TASK_DONE,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(2000));

    if (bits & EVT_TASK_DONE) {
        ESP_LOGE(TAG, "task died on start");
        /* Task has exited — consume join_sem, delete suspended task, set join_complete */
        xSemaphoreTake(exec->join_sem, pdMS_TO_TICKS(1000));
        xSemaphoreTake(exec->mutex, portMAX_DELAY);
        TaskHandle_t saved = exec->task_handle;
        exec->join_claimed = true;
        xSemaphoreGive(exec->mutex);
        if (saved) vTaskDelete(saved);
        xSemaphoreTake(exec->mutex, portMAX_DELAY);
        exec->task_handle = NULL;
        exec->join_claimed = false;
        exec->join_complete = true;
        exec->lifecycle = EXEC_LIFECYCLE_STOPPED;
        xSemaphoreGive(exec->mutex);
        xEventGroupSetBits(exec->events, EVT_JOIN_COMPLETE);
        return ESP_FAIL;
    }

    if (!(bits & EVT_TASK_READY)) {
        ESP_LOGE(TAG, "task start timeout");
        /* Set stop request and wake task */
        xSemaphoreTake(exec->mutex, portMAX_DELAY);
        exec->lifecycle = EXEC_LIFECYCLE_STOPPING;
        exec->stop_requested = true;
        xSemaphoreGive(exec->mutex);
        wake_executor(exec);

        /* Wait for join_sem, then use same join-owner protocol */
        BaseType_t done_ok = xSemaphoreTake(
            exec->join_sem, pdMS_TO_TICKS(3000));
        if (done_ok == pdTRUE) {
            xSemaphoreTake(exec->mutex, portMAX_DELAY);
            if (exec->task_exit_ready && !exec->join_claimed) {
                TaskHandle_t saved = exec->task_handle;
                exec->join_claimed = true;
                xSemaphoreGive(exec->mutex);
                if (saved) vTaskDelete(saved);
                xSemaphoreTake(exec->mutex, portMAX_DELAY);
                exec->task_handle = NULL;
                exec->join_claimed = false;
                exec->join_complete = true;
                exec->lifecycle = EXEC_LIFECYCLE_STOPPED;
            }
            xSemaphoreGive(exec->mutex);
            xEventGroupSetBits(exec->events, EVT_JOIN_COMPLETE);
        }
        /* If join_sem timeout: lifecycle stays STOPPING, task_handle preserved.
         * Subsequent wash_executor_stop() can retry. */
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t wash_executor_submit_program(
    wash_executor_t *exec,
    const wash_program_t *program,
    uint32_t program_id)
{
    if (!exec || !program) return ESP_ERR_INVALID_ARG;
    if (program->step_count > WASH_PROGRAM_MAX_STEPS) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(exec->mutex, portMAX_DELAY);
    bool ok = (exec->lifecycle == EXEC_LIFECYCLE_RUNNING)
           && !exec->program_reserved
           && !exec->external_busy;   /* P1-2: diagnostics hold the wash lock */
    if (ok) {
        exec->pending_program = *program;
        exec->program_reserved = true;
    }
    xSemaphoreGive(exec->mutex);

    if (!ok) return ESP_ERR_INVALID_STATE;

    ingress_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = INGRESS_PROGRAM;
    msg.program_id = program_id;

    if (xQueueSend(exec->queue_program, &msg, 0) != pdTRUE) {
        xSemaphoreTake(exec->mutex, portMAX_DELAY);
        exec->program_reserved = false;
        xSemaphoreGive(exec->mutex);
        return ESP_ERR_INVALID_STATE;
    }

    wake_executor(exec);
    return ESP_OK;
}

esp_err_t wash_executor_publish_terminal(
    wash_executor_t *exec,
    const void *token_buf,
    uint32_t token_size,
    bool success,
    uint32_t error_code)
{
    if (!exec || !token_buf || token_size < sizeof(terminal_token_t))
        return ESP_ERR_INVALID_ARG;

    ingress_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = INGRESS_SERVICE_TERMINAL;
    memcpy(&msg.token, token_buf, sizeof(terminal_token_t));
    msg.success = success;
    msg.error_code = error_code;

    if (xQueueSend(exec->queue_service, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    wake_executor(exec);
    return ESP_OK;
}

esp_err_t wash_executor_submit_urgent(wash_executor_t *exec, bool abort)
{
    if (!exec) return ESP_ERR_INVALID_ARG;

    ingress_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = INGRESS_URGENT;
    msg.abort_flag = abort;

    if (xQueueSend(exec->queue_urgent, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    wake_executor(exec);
    return ESP_OK;
}

esp_err_t wash_executor_confirm_load(wash_executor_t *exec)
{
    if (!exec) return ESP_ERR_INVALID_ARG;
    ingress_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = INGRESS_LOAD_CONFIRM;
    if (xQueueSend(exec->queue_service, &msg, pdMS_TO_TICKS(100)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    wake_executor(exec);
    return ESP_OK;
}

esp_err_t wash_executor_confirm_unload(wash_executor_t *exec)
{
    if (!exec) return ESP_ERR_INVALID_ARG;
    ingress_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = INGRESS_UNLOAD_CONFIRM;
    if (xQueueSend(exec->queue_service, &msg, pdMS_TO_TICKS(100)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    wake_executor(exec);
    return ESP_OK;
}

esp_err_t wash_executor_ack_fault(wash_executor_t *exec)
{
    if (!exec) return ESP_ERR_INVALID_ARG;
    ingress_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = INGRESS_FAULT_ACK;
    if (xQueueSend(exec->queue_urgent, &msg, pdMS_TO_TICKS(100)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    wake_executor(exec);
    return ESP_OK;
}

esp_err_t wash_executor_skip_current_step(wash_executor_t *exec)
{
    if (!exec) return ESP_ERR_INVALID_ARG;

    if (xSemaphoreTake(exec->mutex, pdMS_TO_TICKS(100)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    bool allowed = exec->lifecycle == EXEC_LIFECYCLE_RUNNING &&
                   exec->published_snapshot.current_step_skippable;
    xSemaphoreGive(exec->mutex);
    if (!allowed) return ESP_ERR_INVALID_STATE;

    ingress_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = INGRESS_SKIP;
    if (xQueueSend(exec->queue_urgent, &msg, pdMS_TO_TICKS(100)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    wake_executor(exec);
    return ESP_OK;
}

esp_err_t wash_executor_get_snapshot(
    wash_executor_t *exec,
    wash_exec_snapshot_t *out)
{
    if (!exec || !out) return ESP_ERR_INVALID_ARG;

    if (xSemaphoreTake(exec->mutex, pdMS_TO_TICKS(100)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    *out = exec->published_snapshot;

    xSemaphoreGive(exec->mutex);
    return ESP_OK;
}

void wash_executor_set_external_busy(wash_executor_t *exec, bool busy)
{
    if (!exec) return;
    /* Mutex-protected so submit_program's gate check is race-free with
     * motor_bench's request() setting/clearing the lock. */
    xSemaphoreTake(exec->mutex, portMAX_DELAY);
    exec->external_busy = busy;
    xSemaphoreGive(exec->mutex);
}

bool wash_executor_program_reserved(wash_executor_t *exec)
{
    if (!exec) return false;
    /* Live read under the mutex: the published snapshot is task-published and
     * lags the reserve→activate window, so diagnostics read this to avoid
     * admitting while a program is reserved but not yet active. */
    xSemaphoreTake(exec->mutex, portMAX_DELAY);
    bool reserved = exec->program_reserved;
    xSemaphoreGive(exec->mutex);
    return reserved;
}

esp_err_t wash_executor_quiesce(wash_executor_t *exec)
{
    if (!exec) return ESP_ERR_INVALID_ARG;
    /* Quiesce is an ingress event */
    xSemaphoreTake(exec->mutex, portMAX_DELAY);
    exec->lifecycle = EXEC_LIFECYCLE_QUIESCING;
    xSemaphoreGive(exec->mutex);
    return ESP_OK;
}

esp_err_t wash_executor_stop(wash_executor_t *exec, uint32_t timeout_ms)
{
    if (!exec) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(exec->mutex, portMAX_DELAY);
    exec_lifecycle_state_t state = exec->lifecycle;

    if (state == EXEC_LIFECYCLE_STOPPED && exec->join_complete) {
        /* Already stopped and joined — idempotent fast return */
        xSemaphoreGive(exec->mutex);
        return ESP_OK;
    }

    if (state != EXEC_LIFECYCLE_RUNNING &&
        state != EXEC_LIFECYCLE_QUIESCING &&
        state != EXEC_LIFECYCLE_STOPPING) {
        xSemaphoreGive(exec->mutex);
        return ESP_ERR_INVALID_STATE;
    }

    exec->lifecycle = EXEC_LIFECYCLE_STOPPING;
    exec->stop_requested = true;
    xSemaphoreGive(exec->mutex);

    uint32_t stop_start_tick = xTaskGetTickCount();

    /* Wake task to process stop */
    wake_executor(exec);

    /* Try to become the join owner (consume binary semaphore) */
    BaseType_t ok = xSemaphoreTake(exec->join_sem, pdMS_TO_TICKS(timeout_ms));

    if (ok == pdTRUE) {
        /* We got join_sem — try to become the delete owner */
        xSemaphoreTake(exec->mutex, portMAX_DELAY);
        uint32_t expected_gen = exec->task_generation;
        if (exec->task_exit_ready && !exec->join_claimed
            && exec->exit_ready_generation == expected_gen) {
            /* We are the join owner — generation matches */
            TaskHandle_t saved_handle = exec->task_handle;
            exec->join_claimed = true;
            xSemaphoreGive(exec->mutex);

            /* Delete the suspended task outside mutex */
#ifdef WASH_EXEC_TEST_HOOKS
            if (s_test_hooks.join_owner_barrier)
                s_test_hooks.join_owner_barrier(s_test_hooks.join_owner_ctx);
#endif
            if (saved_handle) {
                vTaskDelete(saved_handle);
            }

            /* Mark join complete under mutex */
            xSemaphoreTake(exec->mutex, portMAX_DELAY);
            exec->task_handle = NULL;
            exec->join_claimed = false;
            exec->join_complete = true;
            exec->join_complete_generation = expected_gen;
            exec->lifecycle = EXEC_LIFECYCLE_STOPPED;
            xSemaphoreGive(exec->mutex);
            xEventGroupSetBits(exec->events, EVT_JOIN_COMPLETE);
            return ESP_OK;
        }
        /* Generation mismatch or someone else claimed.
         * Release mutex and wait for join_complete. */
        xSemaphoreGive(exec->mutex);
    } else {
        ESP_LOGW(TAG, "stop: join_sem timeout, waiting for join_complete");
    }

    /* We are NOT the delete owner. Wait for join_complete via EventGroup.
     * Use remaining time from caller's timeout budget. */
    TickType_t elapsed_ticks = xTaskGetTickCount() - stop_start_tick;
    TickType_t total_ticks = pdMS_TO_TICKS(timeout_ms);
    TickType_t remaining_ticks = (elapsed_ticks < total_ticks)
                                 ? (total_ticks - elapsed_ticks) : 1;
    EventBits_t jbits = xEventGroupWaitBits(
        exec->events, EVT_JOIN_COMPLETE, pdFALSE, pdFALSE,
        remaining_ticks);

    if (jbits & EVT_JOIN_COMPLETE) {
        xSemaphoreTake(exec->mutex, portMAX_DELAY);
        bool ok_state = exec->join_complete && !exec->join_claimed
                        && exec->task_handle == NULL;
        xSemaphoreGive(exec->mutex);
        return ok_state ? ESP_OK : ESP_FAIL;
    }

    ESP_LOGW(TAG, "stop timeout — resources preserved for retry");
    return ESP_ERR_TIMEOUT;
}

esp_err_t wash_executor_destroy(wash_executor_t *exec)
{
    if (!exec) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(exec->mutex, portMAX_DELAY);
    bool complete = exec->join_complete;
    bool claiming = exec->join_claimed;
    bool running = (exec->lifecycle == EXEC_LIFECYCLE_RUNNING ||
                    exec->lifecycle == EXEC_LIFECYCLE_STARTING ||
                    exec->lifecycle == EXEC_LIFECYCLE_QUIESCING ||
                    exec->lifecycle == EXEC_LIFECYCLE_STOPPING);
    bool terminal_active = (exec->terminal_state != TERM_STATE_IDLE);
    bool adapter_busy = exec->adapter_in_flight;
    xSemaphoreGive(exec->mutex);

    if (claiming) {
        ESP_LOGE(TAG, "destroy: join in progress");
        return ESP_ERR_INVALID_STATE;
    }
    if (running && !complete) {
        ESP_LOGE(TAG, "destroy: task still running");
        return ESP_ERR_INVALID_STATE;
    }
    if (terminal_active) {
        ESP_LOGE(TAG, "destroy: terminal delivery active");
        return ESP_ERR_INVALID_STATE;
    }
    if (adapter_busy) {
        ESP_LOGE(TAG, "destroy: adapter callback in-flight");
        return ESP_ERR_INVALID_STATE;
    }

    /* Remove queues from set before deleting */
    if (exec->queue_set) {
        xQueueRemoveFromSet(exec->queue_urgent, exec->queue_set);
        xQueueRemoveFromSet(exec->queue_service, exec->queue_set);
        xQueueRemoveFromSet(exec->queue_program, exec->queue_set);
        xQueueRemoveFromSet(exec->queue_stop, exec->queue_set);
        vQueueDelete(exec->queue_set);
    }
    if (exec->queue_program) vQueueDelete(exec->queue_program);
    if (exec->queue_service) vQueueDelete(exec->queue_service);
    if (exec->queue_urgent) vQueueDelete(exec->queue_urgent);
    if (exec->queue_stop) vQueueDelete(exec->queue_stop);
    if (exec->mutex) vSemaphoreDelete(exec->mutex);
    if (exec->events) vEventGroupDelete(exec->events);
    if (exec->join_sem) vSemaphoreDelete(exec->join_sem);

    free(exec);
    return ESP_OK;
}
