/*
 * water_service.c — 两级分批进水服务 (Round 5)
 *
 * 所有动作通过 ACTION_RESULT 反馈 FSM
 * reservation 在 FSM→IDLE 后释放
 * stop 使用 draining 模型
 * start 不持锁等待
 * task_handle 和 shutdown_draining 使用 _Atomic 同步
 * start/stop 线性化: start 最终提交检查 stop_requested
 */

#include "water_service.h"
#include "water_fsm.h"
#include "safety_manager.h"
#include <stdatomic.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *TAG = "water_svc";

#define TASK_STACK_SIZE      4096
#define TASK_PRIORITY        15
#define TASK_CORE            1
#define CMD_QUEUE_DEPTH      4
#define TICK_INTERVAL_MS     50
#define INIT_TIMEOUT_MS      3000
#define EVENT_PUBLISH_MAX_PER_TICK  2

#define NOTIFY_STOP_BIT      (1U << 0)
#define NOTIFY_EMERGENCY_BIT (1U << 1)
#define NOTIFY_CANCEL_BIT    (1U << 2)

typedef enum { CMD_FILL = 0, CMD_ACT_TEST, CMD_ACT_CLOSE } water_cmd_type_t;
typedef struct {
    water_cmd_type_t type;
    union {
        water_in_request_t fill;
        struct {
            water_actuator_valve_t valve;
            machine_request_id_t request_id;
            uint32_t duration_ms;
        } act_test;
    };
} water_cmd_t;

typedef enum {
    LC_UNINITIALIZED = 0, LC_INITIALIZED, LC_STARTING, LC_RUNNING, LC_STOPPING,
} lifecycle_t;

typedef struct {
    water_service_config_t cfg;
    const xiaojing_hal_t *hal;
    machine_event_sink_t event_sink;

    water_fsm_ctx_t fsm;

    atomic_bool request_reserved;
    atomic_uint_fast32_t reserved_request_id;
    atomic_uint_fast32_t cancel_request_id;

    water_pending_terminal_t pending_terminal;

    water_snapshot_t snapshot;
    SemaphoreHandle_t snapshot_mutex;

    QueueHandle_t cmd_queue;
    SemaphoreHandle_t task_ready_sem;
    SemaphoreHandle_t task_done_sem;
    _Atomic(TaskHandle_t) task_handle;
    uint32_t task_generation;
    esp_err_t task_exit_result;
    lifecycle_t lifecycle;
    atomic_bool stop_requested;
    bool stop_hook_registered;
    atomic_bool shutdown_draining;  /* stop 后正在排空活动请求, _Atomic: task writes, fill_async reads */
    atomic_bool emergency_requested;/* 板测受理前拒绝：紧急关断已请求过 */

    /* 初始安全 OFF 同步（HW-FIX-1）：仅 water 任务写。 */
    bool valve_off_synced;

    /* 受控单阀板测状态 — 仅 water 任务写（单写者）。 */
    bool act_test_active;
    water_actuator_valve_t act_test_valve;
    machine_request_id_t act_test_request_id;
    uint32_t act_test_duration_ms;
    int64_t act_test_start_ms;
    water_act_test_state_t act_test_state;
} water_ctx_t;

static water_ctx_t s_wctx;
static StaticSemaphore_t s_lc_mtx_storage;
static SemaphoreHandle_t s_lc_mtx = NULL;
static atomic_bool s_global_init_done = false;
static uint32_t s_notify_attempt_count = 0;

static inline void lc_lock(void)   { xSemaphoreTake(s_lc_mtx, portMAX_DELAY); }
static inline void lc_unlock(void) { xSemaphoreGive(s_lc_mtx); }

/* ---- Test hooks ---- */
#ifdef WATER_SERVICE_TEST_HOOKS
typedef struct { water_hook_fn fn; void *user_data; } water_hook_entry_t;
static volatile water_hook_entry_t s_hooks[WATER_HOOK_COUNT];
static SemaphoreHandle_t s_hook_barriers[WATER_HOOK_COUNT];
void water_service_set_hook(water_hook_point_t where, water_hook_fn fn, void *ud) {
    s_hooks[where].fn = fn; s_hooks[where].user_data = ud;
}
void *water_service_get_barrier(water_hook_point_t where) { return (void *)s_hook_barriers[where]; }
static void water_hooks_init(void) {
    for (int i = 0; i < WATER_HOOK_COUNT; i++)
        if (!s_hook_barriers[i]) s_hook_barriers[i] = xSemaphoreCreateBinary();
}
static inline void water_invoke_hook(water_hook_point_t where) {
    water_hook_fn fn = s_hooks[where].fn; if (fn) fn(where, s_hooks[where].user_data);
}
#else
static inline void water_invoke_hook(water_hook_point_t where) { (void)where; }
#endif

/* ---- Helpers ---- */

static void update_snapshot(const water_snapshot_t *snap) {
    if (xSemaphoreTake(s_wctx.snapshot_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        s_wctx.snapshot = *snap;
        xSemaphoreGive(s_wctx.snapshot_mutex);
    }
}

static esp_err_t publish_event_once(const machine_event_t *ev) {
    if (!s_wctx.event_sink.publish) return ESP_ERR_INVALID_STATE;
    return s_wctx.event_sink.publish(ev, 100, s_wctx.event_sink.context);
}

static esp_err_t apply_valve(safety_operation_t op, bool enable, machine_request_id_t rid) {
    safety_request_t req = { .operation = op, .enable = enable, .request_id = rid,
                              .required_position = DRUM_POS_0, .source = APP_SOURCE_SYSTEM };
    water_invoke_hook(WATER_HOOK_PRE_VALVE_APPLY);
    return safety_manager_apply(&req);
}

static bool check_safety_active_ok(safety_operation_t op) {
    bool allowed = false; safety_reject_reason_t reason;
    safety_manager_validate_active(op, DRUM_POS_0, &allowed, &reason);
    return allowed;
}

/* ---- Request reservation ---- */

static bool try_reserve_request(const water_in_request_t *req) {
    if (atomic_load(&s_wctx.request_reserved)) return false;
    atomic_store(&s_wctx.reserved_request_id, req->request_id);
    atomic_store(&s_wctx.request_reserved, true);
    return true;
}

static void release_request_reservation(void) {
    atomic_store(&s_wctx.request_reserved, false);
    atomic_store(&s_wctx.reserved_request_id, MACHINE_REQUEST_ID_INVALID);
}

/* ---- Terminal event ---- */

static void build_terminal_event(water_terminal_t terminal, machine_request_id_t rid,
                                  machine_fault_code_t fc, fault_severity_t sev,
                                  int64_t now_ms, machine_event_t *out) {
    memset(out, 0, sizeof(*out));
    out->type = MACHINE_EVENT_WATER_DONE;
    out->request_id = rid;
    switch (terminal) {
    case WATER_TERMINAL_COMPLETE:  out->result = SERVICE_RESULT_OK; break;
    case WATER_TERMINAL_CANCELLED: out->result = SERVICE_RESULT_CANCELED; break;
    case WATER_TERMINAL_FAULT:     out->result = SERVICE_RESULT_FAULT;
        out->fault.code = fc; out->fault.severity = sev; out->fault.timestamp_ms = now_ms; break;
    default: break;
    }
}

static void try_publish_pending(void) {
    if (!s_wctx.pending_terminal.valid) return;
    esp_err_t err = publish_event_once(&s_wctx.pending_terminal.event);
    s_wctx.pending_terminal.attempts++;
    s_wctx.pending_terminal.last_error = err;
    if (err == ESP_OK) {
        water_fsm_mark_terminal_emitted(&s_wctx.fsm);
        s_wctx.pending_terminal.valid = false;
    }
}

static void save_terminal_event(water_fsm_output_t *out, int64_t now_ms) {
    if (!out->emit_event || s_wctx.pending_terminal.valid || s_wctx.fsm.terminal_event_emitted) return;
    build_terminal_event(out->terminal, s_wctx.fsm.request_id,
                          out->fault_code, out->fault_severity, now_ms,
                          &s_wctx.pending_terminal.event);
    s_wctx.pending_terminal.valid = true;
    s_wctx.pending_terminal.attempts = 0;
    s_wctx.pending_terminal.last_error = ESP_OK;
    try_publish_pending();
}

/* ---- Action execution with per-valve commit ---- */

/* Execute a CLOSE_ALL: close both valves, commit per-valve results.
 * Returns true if both valves closed successfully AND commit succeeded. */
static bool execute_close_all_with_commit(machine_request_id_t rid) {
    esp_err_t err1 = apply_valve(SAFETY_OP_SOURCE_INLET_VALVE, false, rid);
    esp_err_t err2 = apply_valve(SAFETY_OP_TRANSFER_VALVE, false, rid);

    water_fsm_output_t dummy;
    esp_err_t commit_err = water_fsm_commit_close_all(&s_wctx.fsm,
                                (err1 == ESP_OK) ? WATER_COMMIT_OK : WATER_COMMIT_FAILED,
                                (err2 == ESP_OK) ? WATER_COMMIT_OK : WATER_COMMIT_FAILED,
                                &dummy);
    if (commit_err != ESP_OK) {
        ESP_LOGE(TAG, "commit_close_all rejected: 0x%x", commit_err);
        return false;
    }
    return (err1 == ESP_OK && err2 == ESP_OK);
}

/* Execute a single action and commit. Returns ESP_OK if action succeeded. */
static esp_err_t execute_single_with_commit(water_fsm_action_t action, machine_request_id_t rid) {
    esp_err_t err;
    switch (action) {
    case WATER_FSM_ACTION_OPEN_SOURCE:     err = apply_valve(SAFETY_OP_SOURCE_INLET_VALVE, true, rid); break;
    case WATER_FSM_ACTION_CLOSE_SOURCE:    err = apply_valve(SAFETY_OP_SOURCE_INLET_VALVE, false, rid); break;
    case WATER_FSM_ACTION_OPEN_TRANSFER:   err = apply_valve(SAFETY_OP_TRANSFER_VALVE, true, rid); break;
    case WATER_FSM_ACTION_CLOSE_TRANSFER:  err = apply_valve(SAFETY_OP_TRANSFER_VALVE, false, rid); break;
    default: return ESP_OK;
    }
    water_fsm_output_t dummy;
    esp_err_t commit_err = water_fsm_commit_result(&s_wctx.fsm, action,
                             (err == ESP_OK) ? WATER_COMMIT_OK : WATER_COMMIT_FAILED, &dummy);
    if (commit_err != ESP_OK) {
        ESP_LOGE(TAG, "commit_result rejected action=%d: 0x%x", action, commit_err);
        return commit_err;
    }
    return err;
}

/* Execute action, commit results, and feed ACTION_RESULT back to FSM.
 * Returns the fsm output from the result event processing. */
static water_fsm_output_t execute_action_and_report(water_fsm_action_t action,
                                                      machine_request_id_t rid, int64_t now_ms)
{
    water_fsm_output_t result_out = {0};
    if (action == WATER_FSM_ACTION_NONE || action == WATER_FSM_ACTION_RESET_FLOW) return result_out;

    bool success;
    if (action == WATER_FSM_ACTION_CLOSE_ALL) {
        success = execute_close_all_with_commit(rid);
    } else {
        success = (execute_single_with_commit(action, rid) == ESP_OK);
    }

    /* Feed ACTION_RESULT back to FSM */
    water_event_t result_evt = {0};
    result_evt.type = WATER_EVT_ACTION_RESULT;
    result_evt.now_ms = now_ms;
    result_evt.action_result_ok = success;
    result_evt.action_result_action = action;
    water_fsm_tick(&s_wctx.fsm, &result_evt, &result_out);
    return result_out;
}

/* ---- 初始安全 OFF 同步（HW-FIX-1） ---- */

/* 启动时经正常 safety/service 路径显式请求双阀 OFF 并提交影子状态：
 * 成功 → KNOWN_OFF，失败 → UNKNOWN（fail-closed）。UNKNOWN 绝不静默当作
 * OFF。valve_off_synced 表示同步已尝试到终态（门禁在同步完成前拒绝板测）。 */
static void water_initial_safe_off_sync(void)
{
    esp_err_t e1 = apply_valve(SAFETY_OP_SOURCE_INLET_VALVE, false,
                               MACHINE_REQUEST_ID_INVALID);
    esp_err_t e2 = apply_valve(SAFETY_OP_TRANSFER_VALVE, false,
                               MACHINE_REQUEST_ID_INVALID);
    water_fsm_output_t dummy;
    esp_err_t ce = water_fsm_commit_initial_off(
        &s_wctx.fsm,
        (e1 == ESP_OK) ? WATER_COMMIT_OK : WATER_COMMIT_FAILED,
        (e2 == ESP_OK) ? WATER_COMMIT_OK : WATER_COMMIT_FAILED,
        &dummy);
    s_wctx.valve_off_synced = (ce == ESP_OK);
    ESP_LOGI(TAG, "initial safe-off sync: source=%s transfer=%s synced=%d",
             e1 == ESP_OK ? "OFF" : "FAIL",
             e2 == ESP_OK ? "OFF" : "FAIL",
             s_wctx.valve_off_synced ? 1 : 0);
}

/* ---- 受控单阀板测（仅 water 任务调用，单写者） ---- */

static safety_operation_t act_test_op(water_actuator_valve_t valve)
{
    return (valve == WATER_ACTUATOR_VALVE_SOURCE)
        ? SAFETY_OP_SOURCE_INLET_VALVE : SAFETY_OP_TRANSFER_VALVE;
}

static void act_test_open(machine_request_id_t rid, water_actuator_valve_t valve,
                          uint32_t duration_ms)
{
    esp_err_t err = apply_valve(act_test_op(valve), true, rid);
    if (err != ESP_OK) {
        s_wctx.act_test_state = WATER_ACT_TEST_FAULT;
        ESP_LOGE(TAG, "ACT_TEST open valve=%d rid=%" PRIu32 " err=0x%x",
                 (int)valve, rid, (unsigned)err);
        return;
    }
    s_wctx.act_test_active = true;
    s_wctx.act_test_valve = valve;
    s_wctx.act_test_request_id = rid;
    s_wctx.act_test_duration_ms = duration_ms;
    s_wctx.act_test_start_ms = s_wctx.hal->now_ms();
    s_wctx.act_test_state = WATER_ACT_TEST_RUNNING;
    ESP_LOGI(TAG, "ACT_TEST open valve=%d rid=%" PRIu32 " dur=%" PRIu32,
             (int)valve, rid, duration_ms);
}

/* 关闭板测阀并确认 OFF。apply_valve(off) 成功即 MCP 已写 OFF（known off）；
 * 失败置 FAULT（OFF 未确认），绝不标记 COMPLETE。 */
static void act_test_close(void)
{
    if (!s_wctx.act_test_active) return;
    water_actuator_valve_t valve = s_wctx.act_test_valve;
    machine_request_id_t rid = s_wctx.act_test_request_id;
    esp_err_t err = apply_valve(act_test_op(valve), false, rid);
    s_wctx.act_test_active = false;
    if (err == ESP_OK) {
        s_wctx.act_test_state = WATER_ACT_TEST_COMPLETE;
        ESP_LOGI(TAG, "ACT_TEST close valve=%d rid=%" PRIu32 " OFF confirmed",
                 (int)valve, rid);
    } else {
        s_wctx.act_test_state = WATER_ACT_TEST_FAULT;
        ESP_LOGE(TAG, "ACT_TEST close valve=%d rid=%" PRIu32
                 " OFF NOT confirmed err=0x%x", (int)valve, rid, (unsigned)err);
    }
}

/* ---- Main task ---- */

static void water_task(void *arg)
{
    (void)arg;
    uint32_t my_gen = s_wctx.task_generation;
    xSemaphoreGive(s_wctx.task_ready_sem);

    /* HW-FIX-1：启动即经安全路径显式请求双阀 OFF（在受理任何板测/进水前）。 */
    water_initial_safe_off_sync();

    TickType_t last_wake = xTaskGetTickCount();
    bool stop_requested_seen = false;
    uint32_t stop_drain_count = 0;
    int64_t last_flow_diag_ms = 0;
    #define WATER_STOP_DRAIN_MAX  90  /* 90 * 50ms = 4.5s, leaves 500ms margin for stop timeout */

    while (1) {
        /* 1. Notifications */
        uint32_t notify = 0;
        if (xTaskNotifyWait(0, NOTIFY_STOP_BIT | NOTIFY_EMERGENCY_BIT |
                            NOTIFY_CANCEL_BIT, &notify, 0) == pdTRUE) {
            if (notify & NOTIFY_STOP_BIT) {
                /* Quick exit only if truly idle: no pending terminal, no
                 * reservation, no active board-test valve. */
                if (s_wctx.fsm.state == WATER_STATE_IDLE &&
                    !s_wctx.pending_terminal.valid &&
                    !atomic_load(&s_wctx.request_reserved) &&
                    !s_wctx.act_test_active) {
                    break;
                }
                /* Active request or pending terminal: enter draining mode */
                stop_requested_seen = true;
                atomic_store(&s_wctx.shutdown_draining, true);
                /* Close any board-test valve first (device-side OFF). */
                if (s_wctx.act_test_active) act_test_close();
                /* Cancel the active request */
                water_fsm_output_t out;
                water_fsm_cancel(&s_wctx.fsm, s_wctx.fsm.request_id,
                                  s_wctx.hal->now_ms(), &out);
                if (out.action != WATER_FSM_ACTION_NONE) {
                    water_fsm_output_t r = execute_action_and_report(
                        out.action, s_wctx.fsm.request_id, s_wctx.hal->now_ms());
                    save_terminal_event(&r, s_wctx.hal->now_ms());
                }
                save_terminal_event(&out, s_wctx.hal->now_ms());
            }
            if (notify & NOTIFY_EMERGENCY_BIT) {
                if (s_wctx.act_test_active) act_test_close();
                water_fsm_output_t out;
                water_fsm_emergency(&s_wctx.fsm, s_wctx.hal->now_ms(), &out);
                if (out.action != WATER_FSM_ACTION_NONE) {
                    water_fsm_output_t r = execute_action_and_report(
                        out.action, s_wctx.fsm.request_id, s_wctx.hal->now_ms());
                    save_terminal_event(&r, s_wctx.hal->now_ms());
                }
                save_terminal_event(&out, s_wctx.hal->now_ms());
            }
            if (notify & NOTIFY_CANCEL_BIT) {
                machine_request_id_t cid = (machine_request_id_t)atomic_load(&s_wctx.cancel_request_id);
                water_fsm_output_t out;
                water_fsm_cancel(&s_wctx.fsm, cid, s_wctx.hal->now_ms(), &out);
                if (out.action != WATER_FSM_ACTION_NONE) {
                    water_fsm_output_t r = execute_action_and_report(
                        out.action, s_wctx.fsm.request_id, s_wctx.hal->now_ms());
                    save_terminal_event(&r, s_wctx.hal->now_ms());
                }
                save_terminal_event(&out, s_wctx.hal->now_ms());
            }
        }

        /* Check stop conditions (from atomic flag, not notification) */
        if (atomic_load(&s_wctx.stop_requested) && !stop_requested_seen) {
            stop_requested_seen = true;
            atomic_store(&s_wctx.shutdown_draining, true);
            if (s_wctx.fsm.state != WATER_STATE_IDLE ||
                s_wctx.pending_terminal.valid ||
                atomic_load(&s_wctx.request_reserved) ||
                s_wctx.act_test_active) {
                if (s_wctx.act_test_active) act_test_close();
                water_fsm_output_t out;
                water_fsm_cancel(&s_wctx.fsm, s_wctx.fsm.request_id,
                                  s_wctx.hal->now_ms(), &out);
                if (out.action != WATER_FSM_ACTION_NONE) {
                    water_fsm_output_t r = execute_action_and_report(
                        out.action, s_wctx.fsm.request_id, s_wctx.hal->now_ms());
                    save_terminal_event(&r, s_wctx.hal->now_ms());
                }
                save_terminal_event(&out, s_wctx.hal->now_ms());
            }
        }

        /* 2. Retry pending terminal */
        try_publish_pending();

        /* 3. Release reservation if FSM is now IDLE */
        if (s_wctx.fsm.state == WATER_STATE_IDLE && atomic_load(&s_wctx.request_reserved)) {
            lc_lock();
            release_request_reservation();
            lc_unlock();
            atomic_store(&s_wctx.shutdown_draining, false);
        }

        /* 3b. 紧急关断已处理完毕（FSM 回 IDLE、无保留、无板测、无 pending）→
         * 清除 emergency_requested，使后续板测可再次受理。BLE 断线等 emergency
         * 是瞬态事件，不应永久阻塞进水/转移阀自检直到重启（P1-1 同类）。 */
        if (s_wctx.fsm.state == WATER_STATE_IDLE &&
            !atomic_load(&s_wctx.request_reserved) &&
            !s_wctx.pending_terminal.valid &&
            !s_wctx.act_test_active &&
            atomic_load(&s_wctx.emergency_requested)) {
            atomic_store(&s_wctx.emergency_requested, false);
            ESP_LOGI(TAG, "emergency_requested cleared (FSM idle, valves OFF)");
        }

        /* 4. Check if draining stop can exit: FSM→IDLE, no pending, no reservation,
         *    no board-test valve.  Also force exit after WATER_STOP_DRAIN_MAX
         *    iterations to prevent stuck task. */
        if (stop_requested_seen) {
            stop_drain_count++;
            if ((s_wctx.fsm.state == WATER_STATE_IDLE &&
                 !s_wctx.pending_terminal.valid &&
                 !atomic_load(&s_wctx.request_reserved) &&
                 !s_wctx.act_test_active) ||
                stop_drain_count >= WATER_STOP_DRAIN_MAX) {
                if (stop_drain_count >= WATER_STOP_DRAIN_MAX) {
                    ESP_LOGW(TAG, "stop drain forced exit after %u iterations", stop_drain_count);
                }
                break;
            }
        }

        /* 5. Drain command queue */
        water_cmd_t cmd;
        while (xQueueReceive(s_wctx.cmd_queue, &cmd, 0) == pdTRUE) {
            if (cmd.type == CMD_FILL && s_wctx.fsm.state == WATER_STATE_IDLE) {
                water_fsm_begin_request(&s_wctx.fsm, &cmd.fill, s_wctx.hal->now_ms());
            } else if (cmd.type == CMD_ACT_TEST) {
                /* 板测只在正式进水空闲且无其他板测时受理；紧急关断或停机后
                 * 一律拒绝，避免已排队的 OPEN 在 emergency OFF 之后重新上电。 */
                if (s_wctx.fsm.state == WATER_STATE_IDLE &&
                    !s_wctx.act_test_active &&
                    !s_wctx.pending_terminal.valid &&
                    !atomic_load(&s_wctx.request_reserved) &&
                    !atomic_load(&s_wctx.emergency_requested) &&
                    !atomic_load(&s_wctx.stop_requested) &&
                    !atomic_load(&s_wctx.shutdown_draining)) {
                    act_test_open(cmd.act_test.request_id, cmd.act_test.valve,
                                  cmd.act_test.duration_ms);
                } else {
                    ESP_LOGW(TAG, "ACT_TEST rejected: not idle (state=%d act=%d)",
                             (int)s_wctx.fsm.state, s_wctx.act_test_active ? 1 : 0);
                }
            } else if (cmd.type == CMD_ACT_CLOSE) {
                act_test_close();
            }
        }

        /* 5b. Actuator board-test device-side auto-close.  The service owns
         * the valve on the device; it closes itself even if BLE dropped. */
        if (s_wctx.act_test_active &&
            s_wctx.act_test_state == WATER_ACT_TEST_RUNNING) {
            int64_t now_ms = s_wctx.hal->now_ms();
            if (now_ms - s_wctx.act_test_start_ms >=
                (int64_t)s_wctx.act_test_duration_ms) {
                ESP_LOGI(TAG, "ACT_TEST duration elapsed (%" PRIu32 " ms), closing",
                         s_wctx.act_test_duration_ms);
                act_test_close();
            }
        }

        /* 6. Build event */
        water_event_t evt = {0};
        evt.type = WATER_EVT_TICK;
        evt.now_ms = s_wctx.hal->now_ms();

        bool drum_full = false;
        esp_err_t wl_err = s_wctx.hal->read_water_level(&drum_full);
        evt.water_level_valid = (wl_err == ESP_OK);
        evt.drum_full = drum_full;
        safety_manager_update_water(drum_full, evt.water_level_valid);

        flow_snapshot_t flow = {0};
        esp_err_t fl_err = s_wctx.hal->read_flow(&flow);
        evt.flow_read_ok = (fl_err == ESP_OK);
        evt.flow_delta = flow.delta_pulses;
        evt.flow_total = flow.pulses;
        /* This is deliberately the only flow_meter_read() client.  A second
         * reader would consume the delta accounting used by the water FSM. */
        if (evt.flow_read_ok && evt.flow_delta > 0U &&
            (evt.now_ms - last_flow_diag_ms) >= 250) {
            ESP_LOGI(TAG, "FLOW_DIAG pcnt=%" PRIu32 " delta=%" PRIu32 " state=%d",
                     evt.flow_total, evt.flow_delta, (int)s_wctx.fsm.state);
            last_flow_diag_ms = evt.now_ms;
        }

        evt.safety_gpa0_active_ok = check_safety_active_ok(SAFETY_OP_SOURCE_INLET_VALVE);
        evt.safety_gpa1_active_ok = check_safety_active_ok(SAFETY_OP_TRANSFER_VALVE);
        evt.safety_apply_ok = true;

        /* 7. FSM tick */
        water_fsm_output_t out;
        water_fsm_tick(&s_wctx.fsm, &evt, &out);

        /* 8. Handle reset flow */
        if (out.action == WATER_FSM_ACTION_RESET_FLOW) {
            esp_err_t reset_err = s_wctx.hal->reset_flow_counter();
            water_event_t reset_evt = {0};
            reset_evt.type = WATER_EVT_FLOW_RESET_DONE;
            reset_evt.now_ms = s_wctx.hal->now_ms();
            reset_evt.action_result_ok = (reset_err == ESP_OK);
            if (reset_err != ESP_OK) ESP_LOGE(TAG, "flow reset failed: 0x%x", reset_err);
            water_fsm_tick(&s_wctx.fsm, &reset_evt, &out);
        }

        /* 9. Execute action and feed result.
         * execute_action_and_report internally calls water_fsm_tick with the
         * ACTION_RESULT event, which may produce a new action (e.g. CLOSE_ALL
         * after OPEN_SOURCE fails). We must use the returned output to drive
         * the next action in the same tick, not wait for the next loop iteration
         * (where the TICK event would be ignored by CLOSING_ALL/FAULT). */
        if (out.action != WATER_FSM_ACTION_NONE && out.action != WATER_FSM_ACTION_RESET_FLOW) {
            water_fsm_output_t r = execute_action_and_report(
                out.action, s_wctx.fsm.request_id, s_wctx.hal->now_ms());
            save_terminal_event(&r, s_wctx.hal->now_ms());
            /* Chain: if the result produced another action, execute it too.
             * This handles OPEN_SOURCE fail → CLOSE_ALL → FAULT in one tick. */
            if (r.action != WATER_FSM_ACTION_NONE && r.action != WATER_FSM_ACTION_RESET_FLOW) {
                water_fsm_output_t r2 = execute_action_and_report(
                    r.action, s_wctx.fsm.request_id, s_wctx.hal->now_ms());
                save_terminal_event(&r2, s_wctx.hal->now_ms());
                /* One more chain for safety (e.g. close-all → fault) */
                if (r2.action != WATER_FSM_ACTION_NONE && r2.action != WATER_FSM_ACTION_RESET_FLOW) {
                    water_fsm_output_t r3 = execute_action_and_report(
                        r2.action, s_wctx.fsm.request_id, s_wctx.hal->now_ms());
                    save_terminal_event(&r3, s_wctx.hal->now_ms());
                }
            }
        }

        /* 10. Save terminal event from tick */
        save_terminal_event(&out, s_wctx.hal->now_ms());

        /* 11. Update snapshot */
        water_snapshot_t snap;
        water_fsm_snapshot(&s_wctx.fsm, &snap);
        snap.raw_flow_pulses = evt.flow_read_ok ? evt.flow_total : 0U;
        snap.elapsed_ms = (uint32_t)(s_wctx.hal->now_ms() - s_wctx.fsm.request_start_ms);
        /* Overlay 受控板测阀状态：阀已开时按板测目标写 ON；其余保持 FSM 影子。
         * 板测结束后快照回到 FSM 的 OFF/unknown 语义。 */
        snap.act_test_active = s_wctx.act_test_active;
        snap.act_test_state = (uint8_t)s_wctx.act_test_state;
        snap.act_test_valve = (uint8_t)s_wctx.act_test_valve;
        snap.act_test_request_id = s_wctx.act_test_request_id;
        snap.safe_off_synced = s_wctx.valve_off_synced;
        if (s_wctx.act_test_active &&
            s_wctx.act_test_state == WATER_ACT_TEST_RUNNING) {
            if (s_wctx.act_test_valve == WATER_ACTUATOR_VALVE_SOURCE) {
                snap.source_valve_on = true;
                snap.source_valve_unknown = false;
            } else {
                snap.transfer_valve_on = true;
                snap.transfer_valve_unknown = false;
            }
        }
        update_snapshot(&snap);

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TICK_INTERVAL_MS));
    }

    /* Task exiting */
    lc_lock();
    if (my_gen == s_wctx.task_generation) {
        s_wctx.task_exit_result = ESP_OK;
    }
    lc_unlock();

    xSemaphoreGive(s_wctx.task_done_sem);
    vTaskDelete(NULL);
}

/* ---- Resource cleanup ---- */

static void cleanup_resources(void) {
    if (s_wctx.cmd_queue) {
        water_cmd_t drain;
        while (xQueueReceive(s_wctx.cmd_queue, &drain, 0) == pdTRUE) {}
        vQueueDelete(s_wctx.cmd_queue); s_wctx.cmd_queue = NULL;
    }
    if (s_wctx.task_ready_sem) { vSemaphoreDelete(s_wctx.task_ready_sem); s_wctx.task_ready_sem = NULL; }
    if (s_wctx.task_done_sem) { vSemaphoreDelete(s_wctx.task_done_sem); s_wctx.task_done_sem = NULL; }
    if (s_wctx.snapshot_mutex) { vSemaphoreDelete(s_wctx.snapshot_mutex); s_wctx.snapshot_mutex = NULL; }
    atomic_store_explicit(&s_wctx.task_handle, NULL, memory_order_release);
    s_wctx.task_exit_result = ESP_OK;
    s_wctx.hal = NULL;
    s_wctx.event_sink = (machine_event_sink_t){0};
    atomic_store(&s_wctx.stop_requested, false);
    atomic_store(&s_wctx.emergency_requested, false);
    s_wctx.stop_hook_registered = false;
    atomic_store(&s_wctx.shutdown_draining, false);
    s_wctx.pending_terminal.valid = false;
    release_request_reservation();
    s_wctx.act_test_active = false;
    s_wctx.act_test_state = WATER_ACT_TEST_IDLE;
    s_wctx.act_test_request_id = MACHINE_REQUEST_ID_INVALID;
    s_wctx.act_test_duration_ms = 0U;
    s_wctx.act_test_start_ms = 0;
    s_wctx.valve_off_synced = false;
    s_wctx.lifecycle = LC_UNINITIALIZED;
}

/* ---- Stop hook ---- */

static esp_err_t water_stop_hook(void *context) {
    (void)context;
    atomic_store(&s_wctx.stop_requested, true);
    TaskHandle_t h = atomic_load_explicit(&s_wctx.task_handle, memory_order_acquire);
    if (h) { s_notify_attempt_count++; xTaskNotify(h, NOTIFY_EMERGENCY_BIT, eSetBits); }
    return ESP_OK;
}

/* ================================================================
 * Public API — Lifecycle
 * ================================================================ */

esp_err_t water_service_global_init(void) {
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_global_init_done, &expected, true)) return ESP_OK;
    s_lc_mtx = xSemaphoreCreateMutexStatic(&s_lc_mtx_storage);
    return s_lc_mtx ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t water_service_init(const water_service_config_t *config,
                              const xiaojing_hal_t *hal, machine_event_sink_t event_sink) {
    if (!config || !hal) return ESP_ERR_INVALID_ARG;
    if (!s_lc_mtx) return ESP_ERR_INVALID_STATE;
    if (config->no_flow_timeout_ms == 0 || config->source_batch_max_ms == 0 ||
        config->default_transfer_ms == 0 || config->transfer_timeout_ms == 0 ||
        config->max_fill_cycles == 0 || config->total_inlet_timeout_ms == 0) return ESP_ERR_INVALID_ARG;
    if (!isfinite(config->pulses_per_liter) || config->pulses_per_liter < 0.0f) return ESP_ERR_INVALID_ARG;

    lc_lock();
    if (s_wctx.lifecycle != LC_UNINITIALIZED) { lc_unlock(); return ESP_ERR_INVALID_STATE; }

    s_wctx.cfg = *config; s_wctx.hal = hal; s_wctx.event_sink = event_sink;
    water_fsm_init(&s_wctx.fsm, config);
    atomic_store(&s_wctx.cancel_request_id, MACHINE_REQUEST_ID_INVALID);
    atomic_store(&s_wctx.stop_requested, false);
    atomic_store(&s_wctx.request_reserved, false);
    atomic_store(&s_wctx.reserved_request_id, MACHINE_REQUEST_ID_INVALID);
    s_wctx.stop_hook_registered = false;
    s_wctx.shutdown_draining = false;
    s_notify_attempt_count = 0;
    memset(&s_wctx.pending_terminal, 0, sizeof(s_wctx.pending_terminal));

    s_wctx.snapshot_mutex = xSemaphoreCreateMutex();
    s_wctx.task_ready_sem = xSemaphoreCreateBinary();
    s_wctx.task_done_sem = xSemaphoreCreateBinary();
    s_wctx.cmd_queue = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(water_cmd_t));
    if (!s_wctx.snapshot_mutex || !s_wctx.task_ready_sem || !s_wctx.task_done_sem || !s_wctx.cmd_queue) {
        if (s_wctx.cmd_queue) vQueueDelete(s_wctx.cmd_queue);
        if (s_wctx.task_done_sem) vSemaphoreDelete(s_wctx.task_done_sem);
        if (s_wctx.task_ready_sem) vSemaphoreDelete(s_wctx.task_ready_sem);
        if (s_wctx.snapshot_mutex) vSemaphoreDelete(s_wctx.snapshot_mutex);
        s_wctx.cmd_queue = NULL; s_wctx.task_done_sem = NULL;
        s_wctx.task_ready_sem = NULL; s_wctx.snapshot_mutex = NULL;
        lc_unlock(); return ESP_ERR_NO_MEM;
    }
#ifdef WATER_SERVICE_TEST_HOOKS
    water_hooks_init();
#endif
    s_wctx.lifecycle = LC_INITIALIZED;
    lc_unlock();
    return ESP_OK;
}

esp_err_t water_service_start(void) {
    if (!s_lc_mtx) return ESP_ERR_INVALID_STATE;
    lc_lock();
    if (s_wctx.lifecycle != LC_INITIALIZED) { lc_unlock(); return ESP_ERR_INVALID_STATE; }
    s_wctx.lifecycle = LC_STARTING;
    atomic_store(&s_wctx.stop_requested, false);
    s_wctx.task_generation++;
    lc_unlock();

    TaskHandle_t handle = NULL;
    if (xTaskCreatePinnedToCore(water_task, "water_svc", TASK_STACK_SIZE, NULL,
                                 TASK_PRIORITY, &handle, TASK_CORE) != pdPASS) {
        lc_lock(); s_wctx.lifecycle = LC_INITIALIZED; lc_unlock();
        return ESP_ERR_NO_MEM;
    }

    /* Commit task handle under lock */
    lc_lock(); atomic_store_explicit(&s_wctx.task_handle, handle, memory_order_release); lc_unlock();

    /* Wait for ready WITHOUT holding lc_lock */
    if (xSemaphoreTake(s_wctx.task_ready_sem, pdMS_TO_TICKS(INIT_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "start: task init timeout");
        /* Signal task to stop; caller must call stop() for cleanup */
        atomic_store(&s_wctx.stop_requested, true);
        if (handle) xTaskNotify(handle, NOTIFY_STOP_BIT, eSetBits);
        return ESP_ERR_TIMEOUT;
    }

    /* Register stop hook */
    esp_err_t hook_err = safety_manager_register_stop_hook(water_stop_hook, NULL, "water_svc");
    if (hook_err != ESP_OK) {
        ESP_LOGE(TAG, "stop hook failed: 0x%x", hook_err);
        /* Signal task to stop; caller must call stop() for cleanup */
        atomic_store(&s_wctx.stop_requested, true);
        if (handle) xTaskNotify(handle, NOTIFY_STOP_BIT, eSetBits);
        return hook_err;
    }
    s_wctx.stop_hook_registered = true;

    /* Linearization point: check stop was not called concurrently */
    lc_lock();
    if (s_wctx.lifecycle != LC_STARTING || atomic_load(&s_wctx.stop_requested)) {
        /* Stop was called during start. Unregister hook and let stop() finish cleanup. */
        if (s_wctx.stop_hook_registered) {
            esp_err_t unreg = safety_manager_unregister_stop_hook(water_stop_hook, NULL);
            if (unreg == ESP_OK) {
                s_wctx.stop_hook_registered = false;
            } else {
                ESP_LOGW(TAG, "linearization hook unregister failed: 0x%x, keeping registered", unreg);
                /* Keep registered: stop() will retry unregister */
            }
        }
        lc_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_wctx.lifecycle = LC_RUNNING;
    lc_unlock();
    return ESP_OK;
}

esp_err_t water_service_stop(void) {
    if (!s_lc_mtx) return ESP_OK;
    lc_lock();
    lifecycle_t lc = s_wctx.lifecycle;
    if (lc == LC_UNINITIALIZED) { lc_unlock(); return ESP_OK; }
    if (lc == LC_INITIALIZED) { cleanup_resources(); lc_unlock(); return ESP_OK; }

    /* STARTING, RUNNING, or STOPPING: task exists (or existed), need to join */
    if (lc == LC_STARTING || lc == LC_RUNNING) {
        s_wctx.lifecycle = LC_STOPPING;
        atomic_store(&s_wctx.stop_requested, true);
        TaskHandle_t h = atomic_load_explicit(&s_wctx.task_handle, memory_order_acquire);
        if (h) { s_notify_attempt_count++; xTaskNotify(h, NOTIFY_STOP_BIT, eSetBits); }
    } else if (lc == LC_STOPPING) {
        atomic_store(&s_wctx.stop_requested, true);
        TaskHandle_t h = atomic_load_explicit(&s_wctx.task_handle, memory_order_acquire);
        if (h) { s_notify_attempt_count++; xTaskNotify(h, NOTIFY_STOP_BIT, eSetBits); }
    }
    SemaphoreHandle_t done = s_wctx.task_done_sem;
    TaskHandle_t task = atomic_load_explicit(&s_wctx.task_handle, memory_order_acquire);
    uint32_t gen = s_wctx.task_generation;
    lc_unlock();

    /* Wait WITHOUT holding lc_lock */
    bool got_done = false;
    if (done && task) got_done = (xSemaphoreTake(done, pdMS_TO_TICKS(5000)) == pdTRUE);
    else if (!task) got_done = true;

    lc_lock();
    if (got_done || atomic_load_explicit(&s_wctx.task_handle, memory_order_acquire) == NULL || s_wctx.task_generation != gen) {
        esp_err_t exit_result = s_wctx.task_exit_result;

        /* Null task_handle immediately: task has exited, no more notifications */
        atomic_store_explicit(&s_wctx.task_handle, NULL, memory_order_release);

        if (s_wctx.stop_hook_registered) {
            esp_err_t unreg = safety_manager_unregister_stop_hook(water_stop_hook, NULL);
            if (unreg == ESP_OK) {
                s_wctx.stop_hook_registered = false;
            } else {
                /* Unregister failed: keep registered, don't cleanup.
                 * task_handle is already NULL so retry won't notify deleted task.
                 * done_sem is already consumed so retry won't double-wait. */
                ESP_LOGE(TAG, "hook unregister failed: 0x%x, preserving for retry", unreg);
                lc_unlock();
                return unreg;
            }
        }

        /* Terminal delivery contract: if pending terminal exists, try to publish.
         * stop() caller context can retry the sink. If still failing, return TIMEOUT
         * and preserve resources for next stop() retry. */
        if (s_wctx.pending_terminal.valid) {
            try_publish_pending();
            if (s_wctx.pending_terminal.valid) {
                ESP_LOGW(TAG, "cleanup: terminal still pending, preserving for retry");
                lc_unlock();
                return ESP_ERR_TIMEOUT;
            }
        }

        cleanup_resources();
        lc_unlock();
        return exit_result;
    }
    /* Timeout: still stopping. Don't unregister hook or cleanup.
     * Caller should retry stop(). */
    s_wctx.lifecycle = LC_STOPPING;
    lc_unlock();
    return ESP_ERR_TIMEOUT;
}

/* ================================================================
 * Public API — Commands
 * ================================================================ */

esp_err_t water_service_fill_async(const water_in_request_t *request) {
    if (!request) return ESP_ERR_INVALID_ARG;
    if (request->request_id == MACHINE_REQUEST_ID_INVALID) return ESP_ERR_INVALID_ARG;
    if (!request->complete_on_drum_full) return ESP_ERR_INVALID_ARG;

    lc_lock();
    if (s_wctx.lifecycle != LC_RUNNING || atomic_load(&s_wctx.shutdown_draining)) { lc_unlock(); return ESP_ERR_INVALID_STATE; }

    if (!try_reserve_request(request)) { lc_unlock(); return ESP_ERR_INVALID_STATE; }

    water_cmd_t cmd = { .type = CMD_FILL, .fill = *request };
    water_invoke_hook(WATER_HOOK_PRE_QSEND);
    if (xQueueSend(s_wctx.cmd_queue, &cmd, pdMS_TO_TICKS(10)) != pdTRUE) {
        release_request_reservation();
        lc_unlock();
        return ESP_ERR_TIMEOUT;
    }
    lc_unlock();
    return ESP_OK;
}

esp_err_t water_service_cancel(machine_request_id_t request_id) {
    if (request_id == MACHINE_REQUEST_ID_INVALID) return ESP_ERR_INVALID_ARG;
    lc_lock();
    if (s_wctx.lifecycle != LC_RUNNING) { lc_unlock(); return ESP_ERR_INVALID_STATE; }
    atomic_store(&s_wctx.cancel_request_id, request_id);
    water_invoke_hook(WATER_HOOK_PRE_NOTIFY);
    TaskHandle_t h = atomic_load_explicit(&s_wctx.task_handle, memory_order_acquire);
    BaseType_t ret = h ? xTaskNotify(h, NOTIFY_CANCEL_BIT, eSetBits) : pdFAIL;
    lc_unlock();
    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

esp_err_t water_service_emergency_close(void) {
    esp_err_t err1 = apply_valve(SAFETY_OP_SOURCE_INLET_VALVE, false, MACHINE_REQUEST_ID_INVALID);
    esp_err_t err2 = apply_valve(SAFETY_OP_TRANSFER_VALVE, false, MACHINE_REQUEST_ID_INVALID);
    lc_lock();
    atomic_store(&s_wctx.emergency_requested, true);
    TaskHandle_t h = atomic_load_explicit(&s_wctx.task_handle, memory_order_acquire);
    if (s_wctx.lifecycle == LC_RUNNING && h) {
        xTaskNotify(h, NOTIFY_EMERGENCY_BIT, eSetBits);
    }
    lc_unlock();
    return (err1 != ESP_OK) ? err1 : err2;
}

/* ---- 受控单阀板测（开发/板级诊断） ---- */

#define WATER_ACT_TEST_MAX_DURATION_MS 10000U

esp_err_t water_service_actuator_valve_test(
    water_actuator_valve_t valve, machine_request_id_t request_id,
    uint32_t duration_ms)
{
    if (valve != WATER_ACTUATOR_VALVE_SOURCE &&
        valve != WATER_ACTUATOR_VALVE_TRANSFER) return ESP_ERR_INVALID_ARG;
    if (request_id == MACHINE_REQUEST_ID_INVALID) return ESP_ERR_INVALID_ARG;
    if (duration_ms == 0U || duration_ms > WATER_ACT_TEST_MAX_DURATION_MS)
        return ESP_ERR_INVALID_ARG;

    lc_lock();
    if (s_wctx.lifecycle != LC_RUNNING || !s_wctx.cmd_queue) {
        lc_unlock(); return ESP_ERR_INVALID_STATE;
    }
    water_cmd_t cmd = { .type = CMD_ACT_TEST };
    cmd.act_test.valve = valve;
    cmd.act_test.request_id = request_id;
    cmd.act_test.duration_ms = duration_ms;
    BaseType_t ok = xQueueSend(s_wctx.cmd_queue, &cmd, pdMS_TO_TICKS(10));
    lc_unlock();
    return (ok == pdTRUE) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t water_service_actuator_valve_close(water_actuator_valve_t valve)
{
    if (valve != WATER_ACTUATOR_VALVE_SOURCE &&
        valve != WATER_ACTUATOR_VALVE_TRANSFER) return ESP_ERR_INVALID_ARG;

    lc_lock();
    if (s_wctx.lifecycle != LC_RUNNING || !s_wctx.cmd_queue) {
        lc_unlock(); return ESP_ERR_INVALID_STATE;
    }
    water_cmd_t cmd = { .type = CMD_ACT_CLOSE, .act_test.valve = valve };
    BaseType_t ok = xQueueSend(s_wctx.cmd_queue, &cmd, pdMS_TO_TICKS(10));
    lc_unlock();
    return (ok == pdTRUE) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t water_service_get_snapshot(water_snapshot_t *out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_wctx.snapshot_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_wctx.snapshot_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        *out = s_wctx.snapshot;
        xSemaphoreGive(s_wctx.snapshot_mutex);
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

/* ================================================================
 * Test bridge API (WATER_SERVICE_TEST_HOOKS only)
 * ================================================================ */

#ifdef WATER_SERVICE_TEST_HOOKS

esp_err_t water_service_test_fail_next_hook_unregister(esp_err_t error) {
    safety_manager_test_fail_next_unregister(error, water_stop_hook, NULL);
    return ESP_OK;
}

bool water_service_test_is_task_handle_null(void) {
    return atomic_load_explicit(&s_wctx.task_handle, memory_order_acquire) == NULL;
}

bool water_service_test_is_stop_hook_registered(void) {
    return s_wctx.stop_hook_registered;
}

uint32_t water_service_test_get_notify_attempt_count(void) {
    return s_notify_attempt_count;
}

bool water_service_test_resources_alive(void) {
    return s_wctx.snapshot_mutex != NULL &&
           s_wctx.task_ready_sem != NULL &&
           s_wctx.task_done_sem != NULL &&
           s_wctx.cmd_queue != NULL;
}

#endif /* WATER_SERVICE_TEST_HOOKS */
