/*
 * drain_service.c — 排水服务层 (Round 4.2.1)
 *
 * R4.2.1 changes vs R4.2:
 *  - Mailbox: portMUX critical section (never loses OFF result)
 *  - Task pause/resume test hooks for deterministic testing
 *  - stop() returns ESP_ERR_TIMEOUT when task not joined (callers must check)
 *  - abort_cleanup returns ESP_ERR_TIMEOUT when task alive (no masking)
 *  - removed mailbox_mtx, mailbox_mtx_storage
 * ================================================================ */

#include "drain_service.h"
#include "drain_fsm.h"
#include "safety_contract.h"
#include "safety_manager.h"
#include "esp_log.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

static const char *TAG = "drain_svc";

#define TASK_STACK_SIZE         4096
#define TASK_PRIORITY           15
#define TASK_CORE               1
#define CMD_QUEUE_DEPTH         4
#define TICK_INTERVAL_MS        50
#define STOP_TIMEOUT_MS         3000
#define SNAPSHOT_MUTEX_TIMEOUT  pdMS_TO_TICKS(10)
#define EVENT_PUBLISH_TIMEOUT   100
#define MAX_CHAIN_ACTIONS       8
#define TERM_RETRY_MAX          8

typedef enum {
    LC_UNINITIALIZED = 0,
    LC_INITIALIZED,
    LC_STARTING,
    LC_RUNNING,
    LC_STOPPING,
} lifecycle_t;

typedef enum { CMD_RUN = 0, CMD_CANCEL, CMD_EMERGENCY } cmd_type_t;
typedef struct { cmd_type_t type; drain_request_t request; } drain_cmd_t;

/* Emergency mailbox — protected by portMUX critical section.
 * Merge protocol: keep first non-OK error. */
typedef struct {
    bool pending;
    esp_err_t first_off_error;
} emergency_mailbox_t;

typedef struct { bool valid; machine_event_t event; } pending_terminal_t;

typedef struct {
    drain_params_t cfg;
    const xiaojing_hal_t *hal;
    machine_event_sink_t event_sink;
    drain_fsm_ctx_t fsm;
    lifecycle_t lifecycle;

    SemaphoreHandle_t snapshot_mtx;
    QueueHandle_t cmd_queue;
    SemaphoreHandle_t task_done_sem;
    StaticSemaphore_t task_done_storage;

    bool hook_registered;
    bool task_exited;
    bool task_joined;
    bool cleanup_pending;
    esp_err_t task_exit_result;
    esp_err_t hook_unregister_error;
    esp_err_t terminal_publish_error;

    _Atomic bool stop_requested;
    _Atomic bool request_reserved;
    _Atomic machine_request_id_t reserved_request_id;

    emergency_mailbox_t emerg_mailbox;
    volatile uint32_t emerg_mailbox_consumed;   /* test hook: task consumed mailbox */

    pending_terminal_t pending_term;

#ifdef DRAIN_SERVICE_TEST_HOOKS
    volatile bool task_pause_requested;
    SemaphoreHandle_t task_pause_sem;
    StaticSemaphore_t task_pause_storage;
#endif
} drain_ctx_t;

static drain_ctx_t s_rt;
static portMUX_TYPE s_emerg_mailbox_mux = portMUX_INITIALIZER_UNLOCKED;

static SemaphoreHandle_t s_lc_mtx = NULL;
static StaticSemaphore_t s_lc_mtx_storage;
static bool s_mtx_inited = false;

static SemaphoreHandle_t s_stop_mtx = NULL;
static StaticSemaphore_t s_stop_mtx_storage;
static bool s_stop_mtx_inited = false;

esp_err_t drain_service_global_init(void)
{
    if (!s_mtx_inited) {
        s_lc_mtx = xSemaphoreCreateMutexStatic(&s_lc_mtx_storage);
        s_mtx_inited = true;
    }
    if (!s_stop_mtx_inited) {
        s_stop_mtx = xSemaphoreCreateMutexStatic(&s_stop_mtx_storage);
        s_stop_mtx_inited = true;
    }
    return (s_lc_mtx && s_stop_mtx) ? ESP_OK : ESP_ERR_NO_MEM;
}

__attribute__((constructor))
static void drain_service_auto_init(void) { drain_service_global_init(); }

/* ---- Safety helpers ---- */

static esp_err_t apply_valve_off(void)
{
    safety_request_t req = {
        .operation = SAFETY_OP_DRAIN_VALVE, .enable = false,
        .request_id = s_rt.fsm.request_id,
        .required_position = DRUM_POS_UNKNOWN, .source = APP_SOURCE_SYSTEM,
    };
    return safety_manager_apply(&req);
}

static esp_err_t apply_valve_on(void)
{
    safety_request_t req = {
        .operation = SAFETY_OP_DRAIN_VALVE, .enable = true,
        .request_id = s_rt.fsm.request_id,
        .required_position = DRUM_POS_180, .source = APP_SOURCE_SYSTEM,
    };
    return safety_manager_apply(&req);
}

static esp_err_t execute_fsm_action(drain_fsm_action_t action)
{
    switch (action) {
    case DRAIN_ACTION_VALVE_ON:  return apply_valve_on();
    case DRAIN_ACTION_VALVE_OFF: return apply_valve_off();
    default: return ESP_OK;
    }
}

static void get_position_info(bool *valid, bool *stable, bool *fresh,
                                drum_position_t *pos, bool *motor_moving)
{
    safety_position_view_t view;
    if (safety_manager_get_position_view(&view) == ESP_OK) {
        *valid = view.sample_valid && view.fresh; *stable = view.stable;
        *fresh = view.fresh; *pos = view.position; *motor_moving = view.motor_moving;
    } else {
        *valid = false; *stable = false; *fresh = false;
        *pos = DRUM_POS_UNKNOWN; *motor_moving = false;
    }
}

static void build_terminal_event(const drain_fsm_output_t *out, machine_event_t *event)
{
    memset(event, 0, sizeof(*event));
    event->type = MACHINE_EVENT_DRAIN_DONE;
    event->request_id = s_rt.fsm.request_id;
    switch (out->terminal) {
    case DRAIN_TERMINAL_COMPLETE: event->result = SERVICE_RESULT_OK; break;
    case DRAIN_TERMINAL_CANCELED: event->result = SERVICE_RESULT_CANCELED; break;
    case DRAIN_TERMINAL_TIMEOUT:  event->result = SERVICE_RESULT_TIMEOUT; break;
    case DRAIN_TERMINAL_REJECTED: event->result = SERVICE_RESULT_REJECTED; break;
    case DRAIN_TERMINAL_FAULT:    event->result = SERVICE_RESULT_FAULT; break;
    default: event->result = SERVICE_RESULT_FAULT; break;
    }
    if (out->fault_code != FAULT_NONE) {
        event->fault.code = out->fault_code;
        event->fault.severity = out->fault_severity;
    }
}

static bool try_publish_terminal(void)
{
    if (!s_rt.pending_term.valid) return true;
    if (!s_rt.event_sink.publish) return false;
    esp_err_t err = s_rt.event_sink.publish(&s_rt.pending_term.event,
                                             EVENT_PUBLISH_TIMEOUT,
                                             s_rt.event_sink.context);
    if (err == ESP_OK) {
        s_rt.pending_term.valid = false;
        drain_fsm_mark_terminal_emitted(&s_rt.fsm);
        return true;
    }
    return false;
}

/* Wrapper for cleanup context — returns esp_err_t for stop() contract */
static esp_err_t try_publish_pending_term(void)
{
    if (!s_rt.pending_term.valid) return ESP_OK;
    if (try_publish_terminal()) return ESP_OK;
    return ESP_ERR_TIMEOUT;
}

static esp_err_t drain_stop_hook(void *context)
{
    (void)context;
    return drain_service_emergency_stop();
}

/* ---- Unified FSM output processor ---- */

static void process_fsm_output(drain_fsm_output_t *out, uint32_t now_ms)
{
    int chain = 0;
    while (out->action != DRAIN_ACTION_NONE && chain < MAX_CHAIN_ACTIONS) {
        esp_err_t err = execute_fsm_action(out->action);
        drain_fsm_event_t res_evt = {
            .type = DRAIN_EVT_ACTION_RESULT, .now_ms = now_ms,
            .action_result_action = out->action, .action_result_ok = (err == ESP_OK),
        };
        drain_fsm_tick(&s_rt.fsm, &res_evt, out);
        chain++;
    }
    if (chain >= MAX_CHAIN_ACTIONS && out->action != DRAIN_ACTION_NONE) {
        ESP_LOGE(TAG, "chain limit exceeded, forcing FAULT");
        drain_fsm_event_t emerg = { .type = DRAIN_EVT_EMERGENCY, .now_ms = now_ms };
        drain_fsm_tick(&s_rt.fsm, &emerg, out);
        if (out->action != DRAIN_ACTION_NONE) {
            esp_err_t err = execute_fsm_action(out->action);
            drain_fsm_event_t res_evt = {
                .type = DRAIN_EVT_ACTION_RESULT, .now_ms = now_ms,
                .action_result_action = out->action, .action_result_ok = (err == ESP_OK),
            };
            drain_fsm_tick(&s_rt.fsm, &res_evt, out);
        }
    }
    if (out->emit_event && !s_rt.fsm.terminal_event_emitted) {
        build_terminal_event(out, &s_rt.pending_term.event);
        s_rt.pending_term.valid = true;
    }
    if (try_publish_terminal()) {
        if (s_rt.fsm.terminal_event_emitted &&
            (s_rt.fsm.state == DRAIN_FSM_COMPLETE || s_rt.fsm.state == DRAIN_FSM_FAULT)) {
            s_rt.fsm.state = DRAIN_FSM_IDLE;
            atomic_store(&s_rt.request_reserved, false);
            atomic_store(&s_rt.reserved_request_id, MACHINE_REQUEST_ID_INVALID);
        }
    } else {
        s_rt.terminal_publish_error = ESP_ERR_INVALID_STATE;
    }
}

/* ---- Task ---- */

static void drain_task(void *arg)
{
    (void)arg;
    esp_err_t baseline_err = apply_valve_off();
    if (baseline_err != ESP_OK) ESP_LOGW(TAG, "baseline valve OFF failed: 0x%x", baseline_err);

    TickType_t last_wake = xTaskGetTickCount();

    while (!atomic_load(&s_rt.stop_requested)) {
#ifdef DRAIN_SERVICE_TEST_HOOKS
        /* Test hook: pause task at controlled point */
        if (s_rt.task_pause_requested) {
            xSemaphoreTake(s_rt.task_pause_sem, portMAX_DELAY);
        }
#endif

        /* Emergency mailbox — portMUX critical section (never blocks) */
        taskENTER_CRITICAL(&s_emerg_mailbox_mux);
        if (s_rt.emerg_mailbox.pending) {
            esp_err_t off_result = s_rt.emerg_mailbox.first_off_error;
            s_rt.emerg_mailbox.pending = false;
            taskEXIT_CRITICAL(&s_emerg_mailbox_mux);
            s_rt.emerg_mailbox_consumed++;
            if (off_result != ESP_OK) {
                s_rt.fsm.output_state = DRAIN_OUTPUT_UNKNOWN;
                safety_manager_report_fault(FAULT_MCP_IO, FAULT_SEVERITY_LATCHED, (uint32_t)off_result);
            } else { s_rt.fsm.output_state = DRAIN_OUTPUT_KNOWN_OFF; }
            s_rt.fsm.state = DRAIN_FSM_COMPLETE;
            s_rt.fsm.terminal = DRAIN_TERMINAL_FAULT;
            s_rt.fsm.fault_code = FAULT_INTERNAL;
            s_rt.fsm.fault_severity = FAULT_SEVERITY_RECOVERABLE;
            drain_fsm_output_t term_out = {
                .emit_event = true, .terminal = DRAIN_TERMINAL_FAULT,
                .fault_code = FAULT_INTERNAL, .fault_severity = FAULT_SEVERITY_RECOVERABLE,
            };
            build_terminal_event(&term_out, &s_rt.pending_term.event);
            s_rt.pending_term.valid = true;
            try_publish_terminal();
            if (s_rt.fsm.terminal_event_emitted) {
                s_rt.fsm.state = DRAIN_FSM_IDLE;
                atomic_store(&s_rt.request_reserved, false);
                atomic_store(&s_rt.reserved_request_id, MACHINE_REQUEST_ID_INVALID);
            }
        } else {
            taskEXIT_CRITICAL(&s_emerg_mailbox_mux);
        }

        if (s_rt.pending_term.valid) {
            try_publish_terminal();
            if (s_rt.fsm.terminal_event_emitted &&
                (s_rt.fsm.state == DRAIN_FSM_COMPLETE || s_rt.fsm.state == DRAIN_FSM_FAULT)) {
                s_rt.fsm.state = DRAIN_FSM_IDLE;
                atomic_store(&s_rt.request_reserved, false);
                atomic_store(&s_rt.reserved_request_id, MACHINE_REQUEST_ID_INVALID);
            }
        }

        /* Command queue */
        drain_cmd_t cmd;
        while (xQueueReceive(s_rt.cmd_queue, &cmd, 0) == pdTRUE) {
            if (cmd.type == CMD_RUN) {
                uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
                esp_err_t err = drain_fsm_begin_request(&s_rt.fsm, &cmd.request,
                                                         s_rt.cfg.max_duration_ms, now_ms);
                if (err != ESP_OK) {
                    machine_event_t rej = {0};
                    rej.type = MACHINE_EVENT_DRAIN_DONE;
                    rej.request_id = cmd.request.request_id;
                    rej.result = SERVICE_RESULT_REJECTED;
                    if (s_rt.event_sink.publish)
                        s_rt.event_sink.publish(&rej, EVENT_PUBLISH_TIMEOUT, s_rt.event_sink.context);
                    atomic_store(&s_rt.request_reserved, false);
                    atomic_store(&s_rt.reserved_request_id, MACHINE_REQUEST_ID_INVALID);
                }
            } else if (cmd.type == CMD_CANCEL) {
                uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
                drain_fsm_event_t evt = { .type = DRAIN_EVT_CANCEL, .now_ms = now_ms,
                    .cancel_request_id = cmd.request.request_id };
                drain_fsm_output_t out;
                drain_fsm_tick(&s_rt.fsm, &evt, &out);
                process_fsm_output(&out, now_ms);
            } else if (cmd.type == CMD_EMERGENCY) {
                uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
                drain_fsm_event_t evt = { .type = DRAIN_EVT_EMERGENCY, .now_ms = now_ms };
                drain_fsm_output_t out;
                drain_fsm_tick(&s_rt.fsm, &evt, &out);
                process_fsm_output(&out, now_ms);
            }
        }

        /* Active validation */
        bool pos_valid, pos_stable, pos_fresh, motor_moving;
        drum_position_t pos;
        get_position_info(&pos_valid, &pos_stable, &pos_fresh, &pos, &motor_moving);
        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

        if (s_rt.fsm.state == DRAIN_FSM_RUNNING) {
            bool active_allowed = false;
            safety_reject_reason_t active_reason = SAFETY_REJECT_NONE;
            esp_err_t val_err = safety_manager_validate_active(
                SAFETY_OP_DRAIN_VALVE, DRUM_POS_180, &active_allowed, &active_reason);
            if (val_err != ESP_OK || !active_allowed) {
                drain_fsm_event_t emerg = { .type = DRAIN_EVT_EMERGENCY, .now_ms = now_ms };
                drain_fsm_output_t emerg_out;
                drain_fsm_tick(&s_rt.fsm, &emerg, &emerg_out);
                process_fsm_output(&emerg_out, now_ms);
                vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TICK_INTERVAL_MS));
                continue;
            }
        }

        if (!s_rt.pending_term.valid) {
            drain_fsm_event_t tick_evt = {
                .type = DRAIN_EVT_TICK, .now_ms = now_ms,
                .position = pos, .position_valid = pos_valid,
                .position_stable = pos_stable, .position_fresh = pos_fresh,
                .position_motor_moving = motor_moving,
            };
            drain_fsm_output_t out;
            drain_fsm_tick(&s_rt.fsm, &tick_evt, &out);
            process_fsm_output(&out, now_ms);
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TICK_INTERVAL_MS));
    }

    /* ---- Stop drain ---- */
    {
        int stop_drain_retries = 0;
        while (s_rt.fsm.state != DRAIN_FSM_IDLE && stop_drain_retries < TERM_RETRY_MAX) {
            if (s_rt.pending_term.valid) {
                try_publish_terminal();
                if (s_rt.fsm.terminal_event_emitted &&
                    (s_rt.fsm.state == DRAIN_FSM_COMPLETE || s_rt.fsm.state == DRAIN_FSM_FAULT)) {
                    s_rt.fsm.state = DRAIN_FSM_IDLE;
                    atomic_store(&s_rt.request_reserved, false);
                    atomic_store(&s_rt.reserved_request_id, MACHINE_REQUEST_ID_INVALID);
                }
                if (s_rt.fsm.state != DRAIN_FSM_IDLE) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                    stop_drain_retries++;  /* Count retries even on continue */
                    continue;
                }
                break;
            }
            drain_fsm_event_t emerg = {
                .type = DRAIN_EVT_EMERGENCY,
                .now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS),
            };
            drain_fsm_output_t emerg_out;
            drain_fsm_tick(&s_rt.fsm, &emerg, &emerg_out);
            process_fsm_output(&emerg_out, emerg.now_ms);
            vTaskDelay(pdMS_TO_TICKS(50));
            stop_drain_retries++;
        }
    }

    /* Final OFF */
    esp_err_t off_err = apply_valve_off();

    xSemaphoreTake(s_lc_mtx, portMAX_DELAY);
    s_rt.task_exit_result = off_err;
    s_rt.task_exited = true;
    xSemaphoreGive(s_lc_mtx);

    xSemaphoreGive(s_rt.task_done_sem);
    vTaskDelete(NULL);
}

/* ---- Lifecycle ---- */

esp_err_t drain_service_init(const drain_params_t *config,
                              const xiaojing_hal_t *hal,
                              machine_event_sink_t event_sink)
{
    if (!config || !hal) return ESP_ERR_INVALID_ARG;
    if (!s_lc_mtx) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lc_mtx, portMAX_DELAY);
    if (s_rt.lifecycle != LC_UNINITIALIZED) {
        xSemaphoreGive(s_lc_mtx);
        return ESP_ERR_INVALID_STATE;
    }

    s_rt.cfg = *config;
    s_rt.hal = hal;
    s_rt.event_sink = event_sink;
    drain_fsm_init(&s_rt.fsm, config);
    s_rt.snapshot_mtx = xSemaphoreCreateMutex();
    s_rt.cmd_queue = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(drain_cmd_t));
    s_rt.task_done_sem = xSemaphoreCreateBinaryStatic(&s_rt.task_done_storage);
    if (!s_rt.snapshot_mtx || !s_rt.cmd_queue) {
        if (s_rt.snapshot_mtx) vSemaphoreDelete(s_rt.snapshot_mtx);
        if (s_rt.cmd_queue) vQueueDelete(s_rt.cmd_queue);
        xSemaphoreGive(s_lc_mtx);
        return ESP_ERR_NO_MEM;
    }

    atomic_store(&s_rt.stop_requested, false);
    atomic_store(&s_rt.request_reserved, false);
    atomic_store(&s_rt.reserved_request_id, MACHINE_REQUEST_ID_INVALID);
    s_rt.hook_registered = false;
    s_rt.task_exited = false;
    s_rt.task_joined = false;
    s_rt.cleanup_pending = false;
    s_rt.task_exit_result = ESP_OK;
    s_rt.hook_unregister_error = ESP_OK;
    s_rt.terminal_publish_error = ESP_OK;
    s_rt.emerg_mailbox.pending = false;
    s_rt.emerg_mailbox.first_off_error = ESP_OK;
    s_rt.emerg_mailbox_consumed = 0;
    s_rt.pending_term.valid = false;
#ifdef DRAIN_SERVICE_TEST_HOOKS
    s_rt.task_pause_requested = false;
    if (!s_rt.task_pause_sem)
        s_rt.task_pause_sem = xSemaphoreCreateBinaryStatic(&s_rt.task_pause_storage);
#endif
    s_rt.lifecycle = LC_INITIALIZED;
    xSemaphoreGive(s_lc_mtx);
    return ESP_OK;
}

esp_err_t drain_service_start(void)
{
    if (!s_lc_mtx) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lc_mtx, portMAX_DELAY);
    if (s_rt.lifecycle != LC_INITIALIZED) {
        xSemaphoreGive(s_lc_mtx);
        return ESP_ERR_INVALID_STATE;
    }

    s_rt.lifecycle = LC_STARTING;
    atomic_store(&s_rt.stop_requested, false);
    s_rt.task_exited = false;
    s_rt.task_joined = false;
    s_rt.cleanup_pending = false;
    s_rt.task_exit_result = ESP_OK;
    s_rt.hook_unregister_error = ESP_OK;
    s_rt.terminal_publish_error = ESP_OK;

    esp_err_t hook_err = safety_manager_register_stop_hook(drain_stop_hook, NULL, "drain");
    if (hook_err != ESP_OK) {
        ESP_LOGE(TAG, "stop hook register failed: 0x%x", hook_err);
        s_rt.lifecycle = LC_INITIALIZED;
        xSemaphoreGive(s_lc_mtx);
        return hook_err;
    }
    s_rt.hook_registered = true;

    BaseType_t ret = xTaskCreatePinnedToCore(drain_task, "drain_svc",
                                               TASK_STACK_SIZE, NULL,
                                               TASK_PRIORITY, NULL, TASK_CORE);
    if (ret != pdPASS) {
        safety_manager_unregister_stop_hook(drain_stop_hook, NULL);
        s_rt.hook_registered = false;
        s_rt.lifecycle = LC_INITIALIZED;
        xSemaphoreGive(s_lc_mtx);
        return ESP_ERR_NO_MEM;
    }

    s_rt.lifecycle = LC_RUNNING;
    xSemaphoreGive(s_lc_mtx);
    return ESP_OK;
}

/* do_cleanup: called with s_lc_mtx held. Returns result. */
static esp_err_t drain_do_cleanup(bool off_already_retried)
{
    esp_err_t first_err = s_rt.task_exit_result;

    /* Retry final OFF if it failed and we haven't already retried */
    if (first_err != ESP_OK && !off_already_retried) {
        esp_err_t retry_off = apply_valve_off();
        if (retry_off == ESP_OK) {
            first_err = ESP_OK;
            s_rt.task_exit_result = ESP_OK;
            s_rt.cleanup_pending = false;
        } else {
            /* OFF still failing — preserve resources, set cleanup_pending */
            s_rt.cleanup_pending = true;
            return first_err;
        }
    }

    /* Terminal delivery contract: if pending terminal exists, try to publish it.
     * stop() caller context can retry the sink. If still failing, return TIMEOUT
     * and preserve resources for next stop() retry. */
    if (s_rt.pending_term.valid) {
        esp_err_t pub_err = try_publish_pending_term();
        if (pub_err != ESP_OK) {
            ESP_LOGW(TAG, "cleanup: terminal publish still failing (0x%x), preserving", pub_err);
            s_rt.cleanup_pending = true;
            if (first_err == ESP_OK) first_err = ESP_ERR_TIMEOUT;
            return first_err;
        }
        /* Terminal published successfully — clear pending */
        s_rt.pending_term.valid = false;
    }

    if (s_rt.hook_registered) {
        esp_err_t hook_err = safety_manager_unregister_stop_hook(drain_stop_hook, NULL);
        if (hook_err != ESP_OK) {
            ESP_LOGE(TAG, "hook unregister failed: 0x%x, preserving resources", hook_err);
            s_rt.hook_unregister_error = hook_err;
            s_rt.cleanup_pending = true;
            if (first_err == ESP_OK) first_err = hook_err;
            return first_err;
        }
        s_rt.hook_registered = false;
        s_rt.hook_unregister_error = ESP_OK;
    }

    if (s_rt.cmd_queue) { vQueueDelete(s_rt.cmd_queue); s_rt.cmd_queue = NULL; }
    if (s_rt.snapshot_mtx) { vSemaphoreDelete(s_rt.snapshot_mtx); s_rt.snapshot_mtx = NULL; }
    s_rt.lifecycle = LC_UNINITIALIZED;
    s_rt.cleanup_pending = false;
    s_rt.hook_unregister_error = ESP_OK;
    s_rt.terminal_publish_error = ESP_OK;
    return first_err;
}

esp_err_t drain_service_stop(void)
{
    if (!s_lc_mtx) return ESP_ERR_INVALID_STATE;
    if (!s_stop_mtx) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_stop_mtx, portMAX_DELAY);

    xSemaphoreTake(s_lc_mtx, portMAX_DELAY);
    if (s_rt.lifecycle == LC_UNINITIALIZED) {
        xSemaphoreGive(s_lc_mtx);
        xSemaphoreGive(s_stop_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_rt.lifecycle == LC_INITIALIZED) {
        xSemaphoreGive(s_lc_mtx);
        xSemaphoreGive(s_stop_mtx);
        return ESP_OK;
    }

    if (s_rt.cleanup_pending) {
        esp_err_t ret = drain_do_cleanup(false);
        xSemaphoreGive(s_lc_mtx);
        xSemaphoreGive(s_stop_mtx);
        return ret;
    }

    if (s_rt.lifecycle == LC_STOPPING && s_rt.task_joined) {
        esp_err_t ret = drain_do_cleanup(true);
        xSemaphoreGive(s_lc_mtx);
        xSemaphoreGive(s_stop_mtx);
        return ret;
    }

    s_rt.lifecycle = LC_STOPPING;
    atomic_store(&s_rt.stop_requested, true);
    xSemaphoreGive(s_lc_mtx);

    if (xSemaphoreTake(s_rt.task_done_sem, pdMS_TO_TICKS(STOP_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "stop timeout, task still alive");
        xSemaphoreGive(s_stop_mtx);
        return ESP_ERR_TIMEOUT;
    }

    xSemaphoreTake(s_lc_mtx, portMAX_DELAY);
    s_rt.task_joined = true;

    esp_err_t ret = drain_do_cleanup(false);
    xSemaphoreGive(s_lc_mtx);
    xSemaphoreGive(s_stop_mtx);
    return ret;
}

/* ---- Commands ---- */

esp_err_t drain_service_run_async(const drain_request_t *request)
{
    if (!request) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_lc_mtx, portMAX_DELAY);
    if (s_rt.lifecycle != LC_RUNNING || !s_rt.cmd_queue) {
        xSemaphoreGive(s_lc_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(s_lc_mtx);

    if (request->request_id == MACHINE_REQUEST_ID_INVALID) return ESP_ERR_INVALID_ARG;
    if (request->duration_ms == 0 || request->duration_ms > s_rt.cfg.max_duration_ms) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lc_mtx, portMAX_DELAY);
    if (s_rt.lifecycle != LC_RUNNING || !s_rt.cmd_queue) {
        xSemaphoreGive(s_lc_mtx);
        return ESP_ERR_INVALID_STATE;
    }

    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_rt.request_reserved, &expected, true)) {
        xSemaphoreGive(s_lc_mtx);
        return ESP_ERR_INVALID_STATE;
    }

    atomic_store(&s_rt.reserved_request_id, request->request_id);

    drain_cmd_t cmd = { .type = CMD_RUN, .request = *request };
    if (xQueueSend(s_rt.cmd_queue, &cmd, pdMS_TO_TICKS(10)) != pdTRUE) {
        atomic_store(&s_rt.request_reserved, false);
        atomic_store(&s_rt.reserved_request_id, MACHINE_REQUEST_ID_INVALID);
        xSemaphoreGive(s_lc_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(s_lc_mtx);
    return ESP_OK;
}

esp_err_t drain_service_cancel(machine_request_id_t request_id)
{
    if (request_id == MACHINE_REQUEST_ID_INVALID) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_lc_mtx, portMAX_DELAY);
    if (s_rt.lifecycle != LC_RUNNING || !s_rt.cmd_queue) {
        xSemaphoreGive(s_lc_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    drain_cmd_t cmd = { .type = CMD_CANCEL, .request.request_id = request_id };
    BaseType_t sent = xQueueSend(s_rt.cmd_queue, &cmd, pdMS_TO_TICKS(10));
    xSemaphoreGive(s_lc_mtx);
    return (sent == pdTRUE) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t drain_service_emergency_stop(void)
{
    /* Immediate OFF — always safe, even before init */
    esp_err_t off_err = apply_valve_off();

    if (!s_lc_mtx) return off_err;

    /* Try queue path first */
    xSemaphoreTake(s_lc_mtx, portMAX_DELAY);
    if (s_rt.cmd_queue && s_rt.lifecycle == LC_RUNNING) {
        drain_cmd_t cmd = { .type = CMD_EMERGENCY };
        if (xQueueSend(s_rt.cmd_queue, &cmd, pdMS_TO_TICKS(10)) == pdTRUE) {
            xSemaphoreGive(s_lc_mtx);
            return off_err;
        }
    }
    xSemaphoreGive(s_lc_mtx);

    /* Mailbox path — portMUX critical section (never blocks, never loses result) */
    if (!s_stop_mtx) return off_err;

    taskENTER_CRITICAL(&s_emerg_mailbox_mux);
    if (off_err != ESP_OK) {
        if (!s_rt.emerg_mailbox.pending || s_rt.emerg_mailbox.first_off_error == ESP_OK) {
            s_rt.emerg_mailbox.first_off_error = off_err;
        }
    } else if (!s_rt.emerg_mailbox.pending) {
        s_rt.emerg_mailbox.first_off_error = ESP_OK;
    }
    s_rt.emerg_mailbox.pending = true;
    taskEXIT_CRITICAL(&s_emerg_mailbox_mux);

    return off_err != ESP_OK ? off_err : ESP_ERR_TIMEOUT;
}

/* ---- Snapshot ---- */

esp_err_t drain_service_get_snapshot(drain_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_lc_mtx, portMAX_DELAY);
    if (s_rt.lifecycle == LC_UNINITIALIZED || !s_rt.snapshot_mtx) {
        xSemaphoreGive(s_lc_mtx);
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_rt.snapshot_mtx, SNAPSHOT_MUTEX_TIMEOUT) != pdTRUE) {
        xSemaphoreGive(s_lc_mtx);
        return ESP_ERR_TIMEOUT;
    }

    out->state = drain_fsm_state_to_public(s_rt.fsm.state);
    out->request_id = s_rt.fsm.request_id;
    out->requested_duration_ms = s_rt.fsm.duration_ms;
    out->output_state = drain_fsm_output_to_public(s_rt.fsm.output_state);
    out->terminal = s_rt.fsm.terminal;
    out->terminal_pending = drain_fsm_terminal_pending(&s_rt.fsm);
    out->elapsed_ms = 0U;
    if (s_rt.fsm.state == DRAIN_FSM_RUNNING) {
        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        out->elapsed_ms = (now_ms >= s_rt.fsm.state_enter_ms)
            ? (now_ms - s_rt.fsm.state_enter_ms) : 0U;
    }

    bool pv, ps, pf, mm;
    drum_position_t pos;
    get_position_info(&pv, &ps, &pf, &pos, &mm);
    out->position = pos;
    out->position_valid = pv;
    out->position_stable = ps;
    out->position_fresh = pf;

    xSemaphoreGive(s_rt.snapshot_mtx);
    xSemaphoreGive(s_lc_mtx);
    return ESP_OK;
}

#ifdef DRAIN_SERVICE_TEST_HOOKS
esp_err_t drain_service_test_pause_task(void)
{
    if (!s_lc_mtx) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lc_mtx, portMAX_DELAY);
    if (s_rt.lifecycle != LC_RUNNING || !s_rt.task_pause_sem) {
        xSemaphoreGive(s_lc_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    s_rt.task_pause_requested = true;
    xSemaphoreGive(s_lc_mtx);
    /* Wait for task to actually block on pause semaphore */
    vTaskDelay(pdMS_TO_TICKS(TICK_INTERVAL_MS * 3));
    return ESP_OK;
}

esp_err_t drain_service_test_resume_task(void)
{
    if (!s_lc_mtx) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lc_mtx, portMAX_DELAY);
    s_rt.task_pause_requested = false;
    if (s_rt.task_pause_sem) xSemaphoreGive(s_rt.task_pause_sem);
    xSemaphoreGive(s_lc_mtx);
    return ESP_OK;
}

uint32_t drain_service_test_get_mailbox_consumed(void)
{
    return s_rt.emerg_mailbox_consumed;
}

esp_err_t drain_service_abort_cleanup(void)
{
    if (!s_lc_mtx) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lc_mtx, portMAX_DELAY);
    if (s_rt.lifecycle == LC_UNINITIALIZED) {
        xSemaphoreGive(s_lc_mtx);
        return ESP_OK;
    }
    if (s_rt.lifecycle == LC_RUNNING || s_rt.lifecycle == LC_STARTING ||
        (s_rt.lifecycle == LC_STOPPING && !s_rt.task_exited)) {
        atomic_store(&s_rt.stop_requested, true);
        s_rt.lifecycle = LC_STOPPING;
#ifdef DRAIN_SERVICE_TEST_HOOKS
        s_rt.task_pause_requested = false;
        if (s_rt.task_pause_sem) xSemaphoreGive(s_rt.task_pause_sem);
#endif
        xSemaphoreGive(s_lc_mtx);
        bool task_exited = false;
        for (int i = 0; i < 3; i++) {
            if (s_rt.task_done_sem &&
                xSemaphoreTake(s_rt.task_done_sem, pdMS_TO_TICKS(STOP_TIMEOUT_MS)) == pdTRUE) {
                task_exited = true;
                break;
            }
        }
        if (!task_exited) {
            ESP_LOGE(TAG, "abort_cleanup: task did not exit, skipping resource destruction");
            return ESP_ERR_TIMEOUT;
        }
        xSemaphoreTake(s_lc_mtx, portMAX_DELAY);
        s_rt.task_joined = true;
    }
    if (s_rt.hook_registered) {
        safety_manager_unregister_stop_hook(drain_stop_hook, NULL);
        s_rt.hook_registered = false;
    }
    if (s_rt.cmd_queue) { vQueueDelete(s_rt.cmd_queue); s_rt.cmd_queue = NULL; }
    if (s_rt.snapshot_mtx) { vSemaphoreDelete(s_rt.snapshot_mtx); s_rt.snapshot_mtx = NULL; }
    memset(&s_rt, 0, sizeof(drain_ctx_t));
    s_rt.lifecycle = LC_UNINITIALIZED;
    atomic_store(&s_rt.reserved_request_id, MACHINE_REQUEST_ID_INVALID);
    xSemaphoreGive(s_lc_mtx);
    return ESP_OK;
}

esp_err_t drain_service_test_reset(void) { return drain_service_abort_cleanup(); }
bool drain_service_test_resources_alive(void) { return s_rt.cmd_queue != NULL && s_rt.snapshot_mtx != NULL; }
#endif
