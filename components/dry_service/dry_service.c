#include "dry_service.h"
#include "dry_fsm.h"
#include "safety_contract.h"
#include "safety_manager.h"
#include "board_config.h"
#include "esp_log.h"
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include <stdatomic.h>
#include <string.h>

/* ================================================================
 * dry_service.c — 烘干服务层
 *
 * process_fsm_output: 执行 action → 发送 ACTION_RESULT → FSM 内部 commit
 * service 不直接写 fsm 内部字段。
 * ================================================================ */

static inline uint32_t elapsed_ms_int(int64_t now, int64_t start) {
    if (now <= start) return 0;
    return (uint32_t)(now - start);
}

static inline bool request_id_valid(machine_request_id_t id) {
    return id != MACHINE_REQUEST_ID_INVALID;
}

/* ---- Constants ---- */

#define TASK_STACK_SIZE         4096
#define TASK_PRIORITY           15
#define TASK_CORE               1
#define CMD_QUEUE_DEPTH         4
#define TICK_INTERVAL_MS        50
#define INIT_TIMEOUT_MS         3000
#define STOP_TIMEOUT_MS         5000
#define SNAPSHOT_MUTEX_TIMEOUT  pdMS_TO_TICKS(10)
#define EVENT_PUBLISH_TIMEOUT   100
#define MAX_CHAINED_ACTIONS     8

#define NOTIFY_STOP_BIT         (1U << 0)
#define NOTIFY_EMERGENCY_BIT    (1U << 1)
#define NOTIFY_CANCEL_BIT       (1U << 2)

/* ---- Global init states ---- */

#define GI_UNINITIALIZED 0
#define GI_INITIALIZING  1
#define GI_READY         2
#define GI_FAILED        3

/* ---- Lifecycle ---- */

typedef enum {
    LC_UNINITIALIZED = 0,
    LC_INITIALIZED,
    LC_STARTING,
    LC_RUNNING,
    LC_STOPPING,
} lifecycle_t;

/* ---- Command ---- */

typedef struct {
    dry_request_t request;
    bool was_reserved;  /* snapshot of reservation at send time */
} dry_cmd_t;

/* ---- Pending terminal ---- */

typedef struct {
    bool valid;
    machine_event_t event;
    esp_err_t last_error;
} dry_pending_terminal_t;

/* ---- Context ---- */

typedef struct {
    dry_params_t cfg;
    const xiaojing_hal_t *hal;
    machine_event_sink_t event_sink;
    dry_fsm_ctx_t fsm;
    lifecycle_t lifecycle;

    SemaphoreHandle_t lc_mtx;
    SemaphoreHandle_t snapshot_mtx;
    SemaphoreHandle_t task_ready_sem;
    SemaphoreHandle_t task_done_sem;
    QueueHandle_t cmd_queue;

    _Atomic(TaskHandle_t) task_handle;
    _Atomic bool stop_requested;
    _Atomic bool request_reserved;
    _Atomic machine_request_id_t reserved_request_id;
    _Atomic machine_request_id_t cancel_request_id;
    uint32_t task_generation;

    dry_pending_terminal_t pending_terminal;
    dry_snapshot_t snap;
    bool stop_hook_registered;
    _Atomic uint32_t notify_attempt_count;
    bool done_sem_consumed;
    bool outputs_synced;            /* 启动时 fan/heater 显式 OFF 同步已完成 */
} dry_ctx_t;

static dry_ctx_t s_dctx;
static _Atomic int s_global_init_state = GI_UNINITIALIZED;
static SemaphoreHandle_t s_lc_mtx;

/* ---- Lifecycle lock ---- */

static inline void lc_lock(void) { xSemaphoreTake(s_lc_mtx, portMAX_DELAY); }
static inline void lc_unlock(void) { xSemaphoreGive(s_lc_mtx); }

/* ---- Stop hook ---- */

static esp_err_t dry_stop_hook(void *context) {
    (void)context;
    atomic_store(&s_dctx.stop_requested, true);
    TaskHandle_t h = atomic_load_explicit(&s_dctx.task_handle,
                                          memory_order_acquire);
    if (h) {
        atomic_fetch_add(&s_dctx.notify_attempt_count, 1);
        xTaskNotify(h, NOTIFY_EMERGENCY_BIT, eSetBits);
    }
    return ESP_OK;
}

/* ---- Hook invocation ---- */

#ifdef DRY_SERVICE_TEST_HOOKS

static struct {
    dry_hook_fn fn;
    void *user_data;
} s_hooks[DRY_HOOK_COUNT];

static SemaphoreHandle_t s_barriers[DRY_HOOK_COUNT];
static esp_err_t s_fail_next_unregister = ESP_OK;

static void invoke_hook(dry_hook_point_t where) {
    if (where < DRY_HOOK_COUNT && s_hooks[where].fn)
        s_hooks[where].fn(where, s_hooks[where].user_data);
    if (where < DRY_HOOK_COUNT && s_barriers[where])
        xSemaphoreTake(s_barriers[where], pdMS_TO_TICKS(1000));
}

#else
static inline void invoke_hook(int where) { (void)where; }
#endif

/* ---- Resource cleanup ---- */

static void cleanup_resources(void) {
    if (s_dctx.cmd_queue) { vQueueDelete(s_dctx.cmd_queue); s_dctx.cmd_queue = NULL; }
    if (s_dctx.task_ready_sem) { vSemaphoreDelete(s_dctx.task_ready_sem); s_dctx.task_ready_sem = NULL; }
    if (s_dctx.task_done_sem) { vSemaphoreDelete(s_dctx.task_done_sem); s_dctx.task_done_sem = NULL; }
    if (s_dctx.snapshot_mtx) { vSemaphoreDelete(s_dctx.snapshot_mtx); s_dctx.snapshot_mtx = NULL; }
    atomic_store_explicit(&s_dctx.task_handle, NULL, memory_order_release);
    atomic_store(&s_dctx.stop_requested, false);
    atomic_store(&s_dctx.request_reserved, false);
    atomic_store(&s_dctx.reserved_request_id, MACHINE_REQUEST_ID_INVALID);
    atomic_store(&s_dctx.cancel_request_id, MACHINE_REQUEST_ID_INVALID);
    s_dctx.task_generation = 0;
    s_dctx.stop_hook_registered = false;
    s_dctx.done_sem_consumed = false;
    atomic_store(&s_dctx.notify_attempt_count, 0);
    memset(&s_dctx.pending_terminal, 0, sizeof(s_dctx.pending_terminal));
    memset(&s_dctx.snap, 0, sizeof(s_dctx.snap));
    s_dctx.lifecycle = LC_UNINITIALIZED;
}

/* ---- Reservation ---- */

static bool try_reserve_request(const dry_request_t *req) {
    if (atomic_load(&s_dctx.request_reserved)) return false;
    atomic_store(&s_dctx.reserved_request_id, req->request_id);
    atomic_store(&s_dctx.request_reserved, true);
    return true;
}

static void release_request_reservation(void) {
    atomic_store(&s_dctx.request_reserved, false);
    atomic_store(&s_dctx.reserved_request_id, MACHINE_REQUEST_ID_INVALID);
}

/* ---- Terminal event ---- */

static machine_event_t build_terminal_event(const dry_fsm_ctx_t *fsm,
                                            dry_terminal_t terminal) {
    machine_event_t evt = {0};
    evt.type = MACHINE_EVENT_DRY_DONE;
    evt.request_id = fsm->request_id;
    switch (terminal) {
    case DRY_TERMINAL_CANCELED: evt.result = SERVICE_RESULT_CANCELED; break;
    case DRY_TERMINAL_FAULT:
        evt.result = SERVICE_RESULT_FAULT;
        evt.fault.code = fsm->m_fault_code;
        evt.fault.severity = fsm->fault_severity;
        evt.fault.timestamp_ms = fsm->tick_ms;
        break;
    default: evt.result = SERVICE_RESULT_OK; break;
    }
    return evt;
}

static esp_err_t publish_event_once(const machine_event_t *evt) {
    if (!s_dctx.event_sink.publish) return ESP_OK;
    return s_dctx.event_sink.publish(evt, EVENT_PUBLISH_TIMEOUT,
                                     s_dctx.event_sink.context);
}

static void save_terminal_event(dry_fsm_output_t *out) {
    if (!out->emit_event) return;
    if (s_dctx.pending_terminal.valid) return;
    if (s_dctx.fsm.terminal_event_emitted) return;

    s_dctx.pending_terminal.event =
        build_terminal_event(&s_dctx.fsm, out->terminal);
    s_dctx.pending_terminal.valid = true;
    s_dctx.pending_terminal.last_error =
        publish_event_once(&s_dctx.pending_terminal.event);
    if (s_dctx.pending_terminal.last_error == ESP_OK) {
        dry_fsm_mark_terminal_emitted(&s_dctx.fsm);
        s_dctx.pending_terminal.valid = false;
    }
}

static void try_publish_pending(void) {
    if (!s_dctx.pending_terminal.valid) return;
    s_dctx.pending_terminal.last_error =
        publish_event_once(&s_dctx.pending_terminal.event);
    if (s_dctx.pending_terminal.last_error == ESP_OK) {
        dry_fsm_mark_terminal_emitted(&s_dctx.fsm);
        s_dctx.pending_terminal.valid = false;
    }
}

/* ---- Safety apply helpers ---- */

static esp_err_t apply_fan_on(uint8_t percent, machine_request_id_t rid) {
    safety_request_t req = {
        .operation = SAFETY_OP_FAN, .enable = true,
        .request_id = rid, .required_position = DRUM_POS_270,
        .source = APP_SOURCE_SYSTEM, .params.fan.percent = percent,
    };
    return safety_manager_apply(&req);
}

static esp_err_t apply_fan_off(void) {
    safety_request_t req = {
        .operation = SAFETY_OP_FAN, .enable = false,
        .request_id = MACHINE_REQUEST_ID_INVALID,
        .required_position = DRUM_POS_UNKNOWN,
        .source = APP_SOURCE_SYSTEM, .params.fan.percent = 0,
    };
    return safety_manager_apply(&req);
}

static esp_err_t apply_heater_on(machine_request_id_t rid) {
    safety_request_t req = {
        .operation = SAFETY_OP_PTC_HEATER, .enable = true,
        .request_id = rid, .required_position = DRUM_POS_270,
        .source = APP_SOURCE_SYSTEM,
    };
    return safety_manager_apply(&req);
}

static esp_err_t apply_heater_off(void) {
    safety_request_t req = {
        .operation = SAFETY_OP_PTC_HEATER, .enable = false,
        .request_id = MACHINE_REQUEST_ID_INVALID,
        .required_position = DRUM_POS_UNKNOWN,
        .source = APP_SOURCE_SYSTEM,
    };
    return safety_manager_apply(&req);
}

/* ---- Action execution ---- */

static esp_err_t execute_single_action(dry_fsm_action_t action) {
    esp_err_t err = ESP_OK;
    switch (action) {
    case DRY_ACTION_FAN_ON:
#ifdef DRY_SERVICE_TEST_HOOKS
        invoke_hook(DRY_HOOK_PRE_FAN_APPLY);
#endif
        err = apply_fan_on(s_dctx.cfg.fan_percent, s_dctx.fsm.request_id);
        break;
    case DRY_ACTION_FAN_OFF:
        err = apply_fan_off();
        break;
    case DRY_ACTION_HEATER_ON:
#ifdef DRY_SERVICE_TEST_HOOKS
        invoke_hook(DRY_HOOK_PRE_PTC_APPLY);
#endif
        err = apply_heater_on(s_dctx.fsm.request_id);
        break;
    case DRY_ACTION_HEATER_OFF:
        err = apply_heater_off();
        break;
    default: break;
    }
    return err;
}

/* ---- Unified FSM output processor ---- */

static void process_fsm_output(dry_fsm_output_t *out) {
    int chain_count = 0;

    while (chain_count < MAX_CHAINED_ACTIONS) {
        if (out->action != DRY_ACTION_NONE) {
            esp_err_t err = execute_single_action(out->action);
            bool ok = (err == ESP_OK);

            /* Send ACTION_RESULT to FSM */
            dry_fsm_event_t res_evt = {
                .type = DRY_EVT_ACTION_RESULT,
                .now_ms = s_dctx.fsm.tick_ms,
                .action_result_action = out->action,
                .action_result_ok = ok,
            };
            dry_fsm_tick(&s_dctx.fsm, &res_evt, out);
            chain_count++;

            /* On failure, FSM decides next state (may output NONE to wait
             * for next tick). Do NOT retry in same chain. */
            if (!ok) break;

            continue;
        }

        save_terminal_event(out);
        break;
    }
    /* Chain limit: FSM is in a consistent state (no dangling pending_action
     * because each ACTION_RESULT was committed). The FSM will retry on
     * subsequent ticks via its normal backoff. */
}

/* ---- SHT reading ---- */

static bool read_sht(sht_sample_t *out) {
    if (!s_dctx.hal || !s_dctx.hal->read_sht) return false;
    /* The driver deliberately returns ESP_OK for a stale/invalid sample so
     * callers can inspect age and preserve the last-good value.  Do not turn
     * that transport success into a sensor-valid result. */
    return s_dctx.hal->read_sht(out) == ESP_OK && out->valid;
}

static void update_safety_sht(const sht_sample_t *sht, bool valid) {
    bool fresh = valid && (sht->age_ms < s_dctx.cfg.sht_stale_timeout_ms);
    safety_manager_update_sht(sht, valid, fresh);
}

/* ---- Task ---- */

static bool is_safe_to_exit(void) {
    return dry_fsm_is_idle(&s_dctx.fsm) ||
           (dry_fsm_is_terminal(&s_dctx.fsm) &&
            s_dctx.fsm.terminal_event_emitted &&
            !s_dctx.pending_terminal.valid);
}

static void dry_task(void *arg) {
    (void)arg;
    uint32_t my_gen = s_dctx.task_generation;
    xSemaphoreGive(s_dctx.task_ready_sem);

    /* 初始安全 OFF 同步：启动即经 safety 显式请求 fan/heater OFF 并提交影子。
     * 成功 → KNOWN_OFF，失败 → UNKNOWN（fail-closed）。fan/heater 板测门禁
     * 在 outputs_synced 置位前拒绝。 */
    esp_err_t e_fan = apply_fan_off();
    esp_err_t e_heat = apply_heater_off();
    esp_err_t ce = dry_fsm_commit_initial_off(&s_dctx.fsm,
                                              e_fan == ESP_OK,
                                              e_heat == ESP_OK);
    s_dctx.outputs_synced = (ce == ESP_OK);
    ESP_LOGI("dry_svc", "initial safe-off sync: fan=%s heater=%s synced=%d",
             e_fan == ESP_OK ? "OFF" : "FAIL",
             e_heat == ESP_OK ? "OFF" : "FAIL",
             s_dctx.outputs_synced ? 1 : 0);

    TickType_t last_wake = xTaskGetTickCount();
    bool running = true;
    uint32_t stop_drain_count = 0;
    #define DRY_STOP_DRAIN_MAX  60  /* 60 * 50ms = 3s max drain time */

    while (running) {
        uint32_t notify = 0;
        xTaskNotifyWait(0, UINT32_MAX, &notify, 0);

        /* Stop requested */
        if (atomic_load(&s_dctx.stop_requested) || (notify & NOTIFY_STOP_BIT)) {
            stop_drain_count++;
            if (is_safe_to_exit() || stop_drain_count >= DRY_STOP_DRAIN_MAX) {
                if (stop_drain_count >= DRY_STOP_DRAIN_MAX) {
                    ESP_LOGW("dry_svc", "stop drain forced exit after %u iterations", stop_drain_count);
                }
                running = false;
                continue;
            }
            if (s_dctx.fsm.state != DRY_FSM_CANCELING &&
                s_dctx.fsm.state != DRY_FSM_FAULT &&
                s_dctx.fsm.state != DRY_FSM_FINISHING) {
                dry_fsm_event_t emerg = {
                    .type = DRY_EVT_EMERGENCY,
                    .now_ms = s_dctx.hal ? s_dctx.hal->now_ms() : 0,
                };
                dry_fsm_output_t out;
                dry_fsm_tick(&s_dctx.fsm, &emerg, &out);
                process_fsm_output(&out);
            }
        }

        /* Emergency notification */
        if (notify & NOTIFY_EMERGENCY_BIT) {
            if (!is_safe_to_exit() &&
                s_dctx.fsm.state != DRY_FSM_CANCELING &&
                s_dctx.fsm.state != DRY_FSM_FAULT &&
                s_dctx.fsm.state != DRY_FSM_FINISHING) {
                dry_fsm_event_t emerg = {
                    .type = DRY_EVT_EMERGENCY,
                    .now_ms = s_dctx.hal ? s_dctx.hal->now_ms() : 0,
                };
                dry_fsm_output_t out;
                dry_fsm_tick(&s_dctx.fsm, &emerg, &out);
                process_fsm_output(&out);
            }
        }

        /* Cancel notification */
        if (notify & NOTIFY_CANCEL_BIT) {
            machine_request_id_t cid = atomic_load(&s_dctx.cancel_request_id);
            dry_fsm_event_t cancel = {
                .type = DRY_EVT_CANCEL,
                .now_ms = s_dctx.hal ? s_dctx.hal->now_ms() : 0,
                .cancel_request_id = cid,
            };
            dry_fsm_output_t out;
            dry_fsm_tick(&s_dctx.fsm, &cancel, &out);
            process_fsm_output(&out);
        }

        /* Retry pending terminal */
        try_publish_pending();

        /* Release reservation if idle */
        if (dry_fsm_is_idle(&s_dctx.fsm) &&
            atomic_load(&s_dctx.request_reserved)) {
            release_request_reservation();
        }

        /* Drain command queue */
        dry_cmd_t cmd;
        while (xQueueReceive(s_dctx.cmd_queue, &cmd, 0) == pdTRUE) {
            if (cmd.was_reserved) {
                int64_t now = s_dctx.hal ? s_dctx.hal->now_ms() : 0;
                dry_fsm_begin_request(&s_dctx.fsm, &cmd.request, now);
            }
        }

        /* Read sensors */
        int64_t now = s_dctx.hal ? s_dctx.hal->now_ms() : 0;

        sht_sample_t sht = {0};
        bool sht_ok = read_sht(&sht);
        update_safety_sht(&sht, sht_ok);

        safety_position_view_t pview;
        esp_err_t perr = safety_manager_get_position_view(&pview);

        /* Build tick event */
        dry_fsm_event_t tick = {
            .type = DRY_EVT_TICK,
            .now_ms = now,
            .position = (perr == ESP_OK) ? pview.position : DRUM_POS_UNKNOWN,
            .position_valid = (perr == ESP_OK) && pview.sample_valid,
            .position_stable = (perr == ESP_OK) && pview.stable,
            .position_fresh = (perr == ESP_OK) && pview.fresh,
            .position_motor_moving = (perr == ESP_OK) && pview.motor_moving,
            .sht_valid = sht_ok,
            .sht_fresh = sht_ok && (sht.age_ms < s_dctx.cfg.sht_stale_timeout_ms),
            .temperature_c = sht.temperature_c,
            .humidity_percent = sht.humidity_rh,
        };

        safety_snapshot_t safety_snap;
        if (safety_manager_get_snapshot(&safety_snap) == ESP_OK) {
            tick.fault_active = safety_snap.fault_active;
            tick.emergency_active = safety_snap.emergency_stop_active;
        }

        /* Run FSM */
        dry_fsm_output_t out;
        dry_fsm_tick(&s_dctx.fsm, &tick, &out);
        process_fsm_output(&out);

        /* Update snapshot */
        if (s_dctx.snapshot_mtx &&
            xSemaphoreTake(s_dctx.snapshot_mtx, SNAPSHOT_MUTEX_TIMEOUT) == pdTRUE) {
            s_dctx.snap.state = (dry_state_t)s_dctx.fsm.state;
            s_dctx.snap.request_id = s_dctx.fsm.request_id;
            s_dctx.snap.elapsed_ms = elapsed_ms_int(now, s_dctx.fsm.heating_start_ms);
            s_dctx.snap.requested_duration_ms = s_dctx.fsm.duration_ms;
            s_dctx.snap.position = s_dctx.fsm.position;
            s_dctx.snap.position_valid = s_dctx.fsm.position_valid;
            s_dctx.snap.position_stable = s_dctx.fsm.position_stable;
            s_dctx.snap.position_fresh = s_dctx.fsm.position_fresh;
            s_dctx.snap.fan_percent = dry_fsm_fan_is_on(&s_dctx.fsm) ? s_dctx.cfg.fan_percent : 0;
            s_dctx.snap.fan_on = dry_fsm_fan_is_on(&s_dctx.fsm);
            s_dctx.snap.heater_requested = s_dctx.fsm.heater_requested;
            s_dctx.snap.heater_on = dry_fsm_heater_is_on(&s_dctx.fsm);
            s_dctx.snap.fan_output_state = (int)s_dctx.fsm.fan_state;
            s_dctx.snap.heater_output_state = (int)s_dctx.fsm.heater_state;
            s_dctx.snap.sht_valid = s_dctx.fsm.sht_valid;
            s_dctx.snap.sht_fresh = s_dctx.fsm.sht_fresh;
            s_dctx.snap.temperature_c = s_dctx.fsm.temperature_c;
            s_dctx.snap.humidity_percent = s_dctx.fsm.humidity_percent;
            s_dctx.snap.heat_cycle_elapsed_ms = s_dctx.fsm.current_heat_cycle_ms;
            s_dctx.snap.cooldown_remaining_ms = s_dctx.fsm.cooldown_remaining_ms;
            s_dctx.snap.terminal_pending = s_dctx.pending_terminal.valid;
            s_dctx.snap.terminal = s_dctx.fsm.terminal;
            s_dctx.snap.fault = s_dctx.fsm.fault_code;
            s_dctx.snap.outputs_synced = s_dctx.outputs_synced;
            s_dctx.snap.revision = s_dctx.fsm.revision;
            xSemaphoreGive(s_dctx.snapshot_mtx);
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TICK_INTERVAL_MS));
    }

    if (my_gen == s_dctx.task_generation) {
        xSemaphoreGive(s_dctx.task_done_sem);
    }
    vTaskDelete(NULL);
}

/* ---- Public API ---- */

#define GI_TIMEOUT_MS 5000

esp_err_t dry_service_global_init(void) {
    int expected = GI_UNINITIALIZED;
    if (!atomic_compare_exchange_strong(&s_global_init_state, &expected,
                                        GI_INITIALIZING)) {
        /* Wait for in-progress init with timeout */
        int waited = 0;
        while (atomic_load(&s_global_init_state) == GI_INITIALIZING) {
            vTaskDelay(pdMS_TO_TICKS(1));
            if (++waited > GI_TIMEOUT_MS) return ESP_ERR_TIMEOUT;
        }
        int state = atomic_load(&s_global_init_state);
        return state == GI_READY ? ESP_OK : ESP_ERR_NO_MEM;
    }
    s_lc_mtx = xSemaphoreCreateMutex();
    if (!s_lc_mtx) {
        atomic_store(&s_global_init_state, GI_FAILED);
        return ESP_ERR_NO_MEM;
    }
    atomic_store(&s_global_init_state, GI_READY);
    return ESP_OK;
}

esp_err_t dry_service_init(const dry_params_t *config,
                           const xiaojing_hal_t *hal,
                           machine_event_sink_t event_sink) {
    if (!config || !hal) return ESP_ERR_INVALID_ARG;
    if (!s_lc_mtx) return ESP_ERR_INVALID_STATE;

    lc_lock();
    if (s_dctx.lifecycle != LC_UNINITIALIZED) {
        lc_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    s_dctx.cfg = *config;
    s_dctx.hal = hal;
    s_dctx.event_sink = event_sink;
    dry_fsm_init(&s_dctx.fsm, config);

    atomic_store(&s_dctx.stop_requested, false);
    atomic_store(&s_dctx.request_reserved, false);
    atomic_store(&s_dctx.reserved_request_id, MACHINE_REQUEST_ID_INVALID);
    atomic_store(&s_dctx.cancel_request_id, MACHINE_REQUEST_ID_INVALID);
    atomic_store_explicit(&s_dctx.task_handle, NULL, memory_order_release);
    atomic_store(&s_dctx.notify_attempt_count, 0);

    s_dctx.snapshot_mtx = xSemaphoreCreateMutex();
    s_dctx.task_ready_sem = xSemaphoreCreateBinary();
    s_dctx.task_done_sem = xSemaphoreCreateBinary();
    s_dctx.cmd_queue = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(dry_cmd_t));

    if (!s_dctx.snapshot_mtx || !s_dctx.task_ready_sem ||
        !s_dctx.task_done_sem || !s_dctx.cmd_queue) {
        cleanup_resources();
        lc_unlock();
        return ESP_ERR_NO_MEM;
    }

    s_dctx.lifecycle = LC_INITIALIZED;
    lc_unlock();
    return ESP_OK;
}

esp_err_t dry_service_start(void) {
    if (!s_lc_mtx) return ESP_ERR_INVALID_STATE;

    lc_lock();
    if (s_dctx.lifecycle != LC_INITIALIZED) {
        lc_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    s_dctx.lifecycle = LC_STARTING;
    atomic_store(&s_dctx.stop_requested, false);
    s_dctx.done_sem_consumed = false;
    s_dctx.task_generation++;

    TaskHandle_t handle = NULL;
    BaseType_t ret = xTaskCreatePinnedToCore(
        dry_task, "dry_task", TASK_STACK_SIZE, NULL,
        TASK_PRIORITY, &handle, TASK_CORE);
    if (ret != pdPASS || !handle) {
        s_dctx.lifecycle = LC_INITIALIZED;
        lc_unlock();
        return ESP_ERR_NO_MEM;
    }
    atomic_store_explicit(&s_dctx.task_handle, handle, memory_order_release);
    lc_unlock();

    /* Wait for task ready (without holding lc_lock) */
    if (xSemaphoreTake(s_dctx.task_ready_sem, pdMS_TO_TICKS(INIT_TIMEOUT_MS)) != pdTRUE) {
        atomic_store(&s_dctx.stop_requested, true);
        TaskHandle_t h = atomic_load_explicit(&s_dctx.task_handle, memory_order_acquire);
        if (h) xTaskNotify(h, NOTIFY_STOP_BIT, eSetBits);
        return ESP_ERR_TIMEOUT;
    }

    /* Register stop hook */
    esp_err_t hook_err = safety_manager_register_stop_hook(dry_stop_hook, NULL, "dry_svc");
    if (hook_err != ESP_OK) {
        /* Hook failed: stop task, join, clean up */
        atomic_store(&s_dctx.stop_requested, true);
        TaskHandle_t h = atomic_load_explicit(&s_dctx.task_handle, memory_order_acquire);
        if (h) xTaskNotify(h, NOTIFY_STOP_BIT, eSetBits);
        SemaphoreHandle_t ds = s_dctx.task_done_sem;
        if (ds && !s_dctx.done_sem_consumed) {
            if (xSemaphoreTake(ds, pdMS_TO_TICKS(STOP_TIMEOUT_MS)) == pdTRUE)
                s_dctx.done_sem_consumed = true;
        }
        /* Only cleanup if task confirmed exited */
        if (s_dctx.done_sem_consumed) {
            atomic_store_explicit(&s_dctx.task_handle, NULL, memory_order_release);
            cleanup_resources();
        } else {
            lc_lock();
            s_dctx.lifecycle = LC_STOPPING;
            lc_unlock();
        }
        return hook_err;
    }
    s_dctx.stop_hook_registered = true;

    /* Linearization: check for concurrent stop */
    lc_lock();
    if (s_dctx.lifecycle != LC_STARTING || atomic_load(&s_dctx.stop_requested)) {
        if (s_dctx.stop_hook_registered) {
            esp_err_t uerr = safety_manager_unregister_stop_hook(dry_stop_hook, NULL);
            if (uerr != ESP_OK) {
                lc_unlock();
                dry_service_stop();
                return ESP_ERR_INVALID_STATE;
            }
            s_dctx.stop_hook_registered = false;
        }
        s_dctx.lifecycle = LC_STOPPING;
        lc_unlock();
        dry_service_stop();
        return ESP_ERR_INVALID_STATE;
    }

    s_dctx.lifecycle = LC_RUNNING;
    lc_unlock();
    return ESP_OK;
}

esp_err_t dry_service_stop(void) {
    if (!s_lc_mtx) return ESP_OK;

    lc_lock();
    switch (s_dctx.lifecycle) {
    case LC_UNINITIALIZED:
        lc_unlock();
        return ESP_OK;
    case LC_INITIALIZED:
        cleanup_resources();
        lc_unlock();
        return ESP_OK;
    case LC_STARTING:
    case LC_RUNNING:
        s_dctx.lifecycle = LC_STOPPING;
        atomic_store(&s_dctx.stop_requested, true);
        break;
    case LC_STOPPING:
        atomic_store(&s_dctx.stop_requested, true);
        break;
    }
    lc_unlock();

    /* Notify task */
    TaskHandle_t h = atomic_load_explicit(&s_dctx.task_handle, memory_order_acquire);
    if (h) xTaskNotify(h, NOTIFY_STOP_BIT, eSetBits);

    /* Wait for task done */
    SemaphoreHandle_t ds = s_dctx.task_done_sem;
    esp_err_t join_err = ESP_OK;
    if (ds && !s_dctx.done_sem_consumed) {
        if (xSemaphoreTake(ds, pdMS_TO_TICKS(STOP_TIMEOUT_MS)) != pdTRUE) {
            join_err = ESP_ERR_TIMEOUT;
        } else {
            s_dctx.done_sem_consumed = true;
        }
    }

    lc_lock();

    if (join_err != ESP_OK) {
        /* Task still running: preserve everything for retry */
        lc_unlock();
        return join_err;
    }

    /* Task confirmed exited: safe to clear handle */
    atomic_store_explicit(&s_dctx.task_handle, NULL, memory_order_release);

    /* Unregister hook */
    if (s_dctx.stop_hook_registered) {
#ifdef DRY_SERVICE_TEST_HOOKS
        if (s_fail_next_unregister != ESP_OK) {
            esp_err_t inject_err = s_fail_next_unregister;
            s_fail_next_unregister = ESP_OK;
            lc_unlock();
            return inject_err;
        }
#endif
        esp_err_t uerr = safety_manager_unregister_stop_hook(dry_stop_hook, NULL);
        if (uerr != ESP_OK) {
            lc_unlock();
            return uerr;  /* Retry-safe: task_handle NULL, done_sem consumed */
        }
        s_dctx.stop_hook_registered = false;
    }
    cleanup_resources();
    lc_unlock();
    return ESP_OK;
}

esp_err_t dry_service_run_async(const dry_request_t *request) {
    if (!request) return ESP_ERR_INVALID_ARG;
    if (!request_id_valid(request->request_id)) return ESP_ERR_INVALID_ARG;
    if (request->duration_ms == 0) return ESP_ERR_INVALID_ARG;

    lc_lock();
    if (s_dctx.lifecycle != LC_RUNNING) { lc_unlock(); return ESP_ERR_INVALID_STATE; }
    if (request->duration_ms > s_dctx.cfg.max_total_ms) { lc_unlock(); return ESP_ERR_INVALID_ARG; }
    if (request->heater_requested && !board_config_is_ptc_enabled()) { lc_unlock(); return ESP_ERR_NOT_SUPPORTED; }
    if (!try_reserve_request(request)) { lc_unlock(); return ESP_ERR_INVALID_STATE; }

    dry_cmd_t cmd = { .request = *request, .was_reserved = true };
    if (xQueueSend(s_dctx.cmd_queue, &cmd, pdMS_TO_TICKS(10)) != pdTRUE) {
        release_request_reservation();
        lc_unlock();
        return ESP_ERR_TIMEOUT;
    }
    lc_unlock();
    return ESP_OK;
}

esp_err_t dry_service_cancel(machine_request_id_t request_id) {
    if (request_id == MACHINE_REQUEST_ID_INVALID) return ESP_ERR_INVALID_ARG;
    lc_lock();
    if (s_dctx.lifecycle != LC_RUNNING) { lc_unlock(); return ESP_ERR_INVALID_STATE; }
    lc_unlock();
    atomic_store(&s_dctx.cancel_request_id, request_id);
    TaskHandle_t h = atomic_load_explicit(&s_dctx.task_handle, memory_order_acquire);
    if (h) xTaskNotify(h, NOTIFY_CANCEL_BIT, eSetBits);
    return ESP_OK;
}

esp_err_t dry_service_emergency_stop(void) {
    /* P1-1 修复：运行时 emergency ≠ 生命周期 stop。
     * - 绝不设置 stop_requested（只有 dry_service_stop() 才能让 task 退出）。
     * - PTC OFF 优先（立即经 safety 执行，独立于任务）。
     * - 通知任务走 FSM 关断序列（PTC→cooldown→FAN→terminal），保证 FAN 在
     *   PTC 已 OFF 后关闭；当前请求 exactly-once 进 INTERRUPTED/FAULT。
     * - 无活动请求时调用为幂等（FSM IDLE 时 emergency tick 为 no-op）。
     */
    esp_err_t ptc_err = apply_heater_off();
    TaskHandle_t h = atomic_load_explicit(&s_dctx.task_handle, memory_order_acquire);
    if (h) {
        atomic_fetch_add(&s_dctx.notify_attempt_count, 1);
        xTaskNotify(h, NOTIFY_EMERGENCY_BIT, eSetBits);
    }
    return ptc_err;
}

esp_err_t dry_service_get_snapshot(dry_snapshot_t *out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_dctx.snapshot_mtx) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_dctx.snapshot_mtx, SNAPSHOT_MUTEX_TIMEOUT) == pdTRUE) {
        *out = s_dctx.snap;
        xSemaphoreGive(s_dctx.snapshot_mtx);
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

/* ---- Test hooks ---- */

#ifdef DRY_SERVICE_TEST_HOOKS

void dry_service_set_hook(dry_hook_point_t where, dry_hook_fn fn, void *user_data) {
    if (where >= DRY_HOOK_COUNT) return;
    s_hooks[where].fn = fn;
    s_hooks[where].user_data = user_data;
}

void *dry_service_get_barrier(dry_hook_point_t where) {
    if (where >= DRY_HOOK_COUNT) return NULL;
    if (!s_barriers[where]) s_barriers[where] = xSemaphoreCreateBinary();
    return s_barriers[where];
}

esp_err_t dry_service_test_fail_next_hook_unregister(esp_err_t error) {
    s_fail_next_unregister = error;
    return ESP_OK;
}

bool dry_service_test_is_task_handle_null(void) {
    return atomic_load_explicit(&s_dctx.task_handle, memory_order_acquire) == NULL;
}

bool dry_service_test_is_stop_hook_registered(void) {
    return s_dctx.stop_hook_registered;
}

uint32_t dry_service_test_get_notify_attempt_count(void) {
    return atomic_load(&s_dctx.notify_attempt_count);
}

bool dry_service_test_resources_alive(void) {
    return s_dctx.cmd_queue != NULL && s_dctx.snapshot_mtx != NULL;
}

#endif
