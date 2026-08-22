#include "coupled_hot_air_service.h"
#include "coupled_hot_air_fsm.h"
#include "safety_contract.h"
#include "safety_manager.h"
#include "machine_config.h"
#include "esp_log.h"
#include "esp_attr.h"
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include <stdatomic.h>
#include <string.h>

/* ================================================================
 * coupled_hot_air_service.c — 耦合热风模块服务层
 *
 * 唯一通电路径：service → safety_manager(SAFETY_OP_HOT_AIR_MODULE)
 * → HAL → MCP23017 GPA7 → 唯一继电器 → 风扇+加热丝并联模块。
 * 无 raw MCP / raw HAL 旁路；无独立 FAN_ON/HEATER_ON。
 * ================================================================ */

#define TASK_STACK_SIZE         3072
#define TASK_PRIORITY           15
#define TASK_CORE               1
#define CMD_QUEUE_DEPTH         4
#define TICK_INTERVAL_MS        50
#define INIT_TIMEOUT_MS         3000
#define STOP_TIMEOUT_MS         5000
#define SNAPSHOT_MUTEX_TIMEOUT  pdMS_TO_TICKS(10)
#define EVENT_PUBLISH_TIMEOUT   100
/* 动作链上限：一次 FSM 输出后可同步执行的补偿/重试动作数。
 * 最坏链 = 1 次 RELAY_ON + CHA_OFF_RETRY_MAX 次 OFF 重试 = 6，随后输出终态
 * 在第 7 轮跳出。绝不应因链满而丢弃 FSM 要求的补偿动作。 */
#define MAX_CHAINED_ACTIONS     (CHA_OFF_RETRY_MAX + 2)

#define NOTIFY_STOP_BIT         (1U << 0)
#define NOTIFY_EMERGENCY_BIT    (1U << 1)
#define NOTIFY_CANCEL_BIT       (1U << 2)

#define GI_UNINITIALIZED 0
#define GI_INITIALIZING  1
#define GI_READY         2
#define GI_FAILED        3

typedef enum {
    LC_UNINITIALIZED = 0,
    LC_INITIALIZED,
    LC_STARTING,
    LC_RUNNING,
    LC_STOPPING,
} lifecycle_t;

typedef struct {
    coupled_hot_air_request_t request;
    bool was_reserved;
} cha_cmd_t;

typedef struct {
    bool valid;
    machine_event_t event;
    esp_err_t last_error;
} cha_pending_terminal_t;

typedef struct {
    const xiaojing_hal_t *hal;
    machine_event_sink_t event_sink;
    cha_fsm_ctx_t fsm;
    lifecycle_t lifecycle;
    float cutoff_c;

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

    cha_pending_terminal_t pending_terminal;
    coupled_hot_air_snapshot_t snap;
    bool stop_hook_registered;
    _Atomic uint32_t notify_attempt_count;
    bool done_sem_consumed;
    bool outputs_synced;
} cha_ctx_t;

static cha_ctx_t s_cctx;
static _Atomic int s_global_init_state = GI_UNINITIALIZED;
static SemaphoreHandle_t s_lc_mtx;

/* cha_task 栈放 PSRAM：内部 RAM 预算紧张（BLE 控制器 init 需大块连续内部
 * 内存），3KB 任务栈若放内部堆会把最大连续块压过临界导致 BLE_Malloc 失败
 * 重启循环。ESP32-S3 DMA 可访问 PSRAM，任务栈放 PSRAM 安全（gesture_service
 * 同型先例）。static 栈在任务自删后仍保留，重启复用同一块。 */
static StaticTask_t s_task_tcb;
static EXT_RAM_BSS_ATTR StackType_t s_task_stack[TASK_STACK_SIZE];

static inline void lc_lock(void) { xSemaphoreTake(s_lc_mtx, portMAX_DELAY); }
static inline void lc_unlock(void) { xSemaphoreGive(s_lc_mtx); }

/* ---- Stop hook ---- */

static esp_err_t cha_stop_hook(void *context)
{
    (void)context;
    atomic_store(&s_cctx.stop_requested, true);
    TaskHandle_t h = atomic_load_explicit(&s_cctx.task_handle,
                                          memory_order_acquire);
    if (h) {
        atomic_fetch_add(&s_cctx.notify_attempt_count, 1);
        xTaskNotify(h, NOTIFY_EMERGENCY_BIT, eSetBits);
    }
    return ESP_OK;
}

static void cleanup_resources(void)
{
    if (s_cctx.cmd_queue) { vQueueDelete(s_cctx.cmd_queue); s_cctx.cmd_queue = NULL; }
    if (s_cctx.task_ready_sem) { vSemaphoreDelete(s_cctx.task_ready_sem); s_cctx.task_ready_sem = NULL; }
    if (s_cctx.task_done_sem) { vSemaphoreDelete(s_cctx.task_done_sem); s_cctx.task_done_sem = NULL; }
    if (s_cctx.snapshot_mtx) { vSemaphoreDelete(s_cctx.snapshot_mtx); s_cctx.snapshot_mtx = NULL; }
    atomic_store_explicit(&s_cctx.task_handle, NULL, memory_order_release);
    atomic_store(&s_cctx.stop_requested, false);
    atomic_store(&s_cctx.request_reserved, false);
    atomic_store(&s_cctx.reserved_request_id, MACHINE_REQUEST_ID_INVALID);
    atomic_store(&s_cctx.cancel_request_id, MACHINE_REQUEST_ID_INVALID);
    s_cctx.task_generation = 0;
    s_cctx.stop_hook_registered = false;
    s_cctx.done_sem_consumed = false;
    atomic_store(&s_cctx.notify_attempt_count, 0);
    memset(&s_cctx.pending_terminal, 0, sizeof(s_cctx.pending_terminal));
    memset(&s_cctx.snap, 0, sizeof(s_cctx.snap));
    s_cctx.lifecycle = LC_UNINITIALIZED;
}

static bool try_reserve_request(const coupled_hot_air_request_t *req)
{
    if (atomic_load(&s_cctx.request_reserved)) return false;
    atomic_store(&s_cctx.reserved_request_id, req->request_id);
    atomic_store(&s_cctx.request_reserved, true);
    return true;
}

static void release_request_reservation(void)
{
    atomic_store(&s_cctx.request_reserved, false);
    atomic_store(&s_cctx.reserved_request_id, MACHINE_REQUEST_ID_INVALID);
}

/* ---- Terminal event ---- */

static machine_event_t build_terminal_event(const cha_fsm_ctx_t *fsm)
{
    machine_event_t evt = {0};
    evt.type = MACHINE_EVENT_HOT_AIR_DONE;
    evt.request_id = fsm->request_id;
    switch (fsm->terminal) {
    case CHA_TERMINAL_INTERRUPTED: evt.result = SERVICE_RESULT_CANCELED; break;
    case CHA_TERMINAL_TIMEOUT:     evt.result = SERVICE_RESULT_TIMEOUT; break;
    case CHA_TERMINAL_FAULT:       evt.result = SERVICE_RESULT_FAULT; break;
    default:                       evt.result = SERVICE_RESULT_OK; break;
    }
    return evt;
}

static esp_err_t publish_event_once(const machine_event_t *evt)
{
    if (!s_cctx.event_sink.publish) return ESP_OK;
    return s_cctx.event_sink.publish(evt, EVENT_PUBLISH_TIMEOUT,
                                     s_cctx.event_sink.context);
}

static void save_terminal_event(cha_fsm_output_t *out)
{
    if (out->terminal == CHA_TERMINAL_NONE) return;
    if (s_cctx.pending_terminal.valid) return;
    if (s_cctx.fsm.terminal_emitted) return;

    s_cctx.pending_terminal.event = build_terminal_event(&s_cctx.fsm);
    s_cctx.pending_terminal.valid = true;
    s_cctx.pending_terminal.last_error =
        publish_event_once(&s_cctx.pending_terminal.event);
    if (s_cctx.pending_terminal.last_error == ESP_OK) {
        cha_fsm_mark_terminal_emitted(&s_cctx.fsm);
        s_cctx.pending_terminal.valid = false;
    }
}

static void try_publish_pending(void)
{
    if (!s_cctx.pending_terminal.valid) return;
    s_cctx.pending_terminal.last_error =
        publish_event_once(&s_cctx.pending_terminal.event);
    if (s_cctx.pending_terminal.last_error == ESP_OK) {
        cha_fsm_mark_terminal_emitted(&s_cctx.fsm);
        s_cctx.pending_terminal.valid = false;
    }
}

/* ---- Safety apply ---- */

static esp_err_t apply_relay_on(machine_request_id_t rid)
{
    safety_request_t req = {
        .operation = SAFETY_OP_HOT_AIR_MODULE, .enable = true,
        .request_id = rid, .required_position = DRUM_POS_270,
        .source = APP_SOURCE_SYSTEM,
    };
    return safety_manager_apply(&req);
}

static esp_err_t apply_relay_off(void)
{
    safety_request_t req = {
        .operation = SAFETY_OP_HOT_AIR_MODULE, .enable = false,
        .request_id = MACHINE_REQUEST_ID_INVALID,
        .required_position = DRUM_POS_UNKNOWN,
        .source = APP_SOURCE_SYSTEM,
    };
    return safety_manager_apply(&req);
}

/* ---- Action execution ---- */

static esp_err_t execute_action(cha_action_t action, machine_request_id_t rid)
{
    if (action == CHA_ACTION_RELAY_ON) return apply_relay_on(rid);
    if (action == CHA_ACTION_RELAY_OFF) return apply_relay_off();
    return ESP_ERR_INVALID_ARG;
}

static void process_fsm_output(cha_fsm_output_t *out)
{
    int chain_count = 0;

    while (chain_count < MAX_CHAINED_ACTIONS) {
        if (out->action != CHA_ACTION_NONE) {
            esp_err_t err = execute_action(out->action, s_cctx.fsm.request_id);
            bool ok = (err == ESP_OK);
            cha_fsm_event_t res = {
                .type = CHA_EVT_ACTION_RESULT,
                .now_ms = s_cctx.hal ? s_cctx.hal->now_ms() : 0,
                .action = out->action,
                .action_ok = ok,
            };
            cha_fsm_tick(&s_cctx.fsm, &res, out);
            chain_count++;
            /* P0: 失败动作绝不能丢弃 FSM 随后要求的补偿/重试动作（如 RELAY_ON
             * 失败后必须继续执行的 RELAY_OFF）。FSM 的 pending_action 守卫保证
             * 在发出终态前 out->action 一直有值，因此链只会经由终态分支退出；
             * 上限仅作极端保护（最坏链 6 个动作，上限 7）。 */
            continue;
        }
        save_terminal_event(out);
        break;
    }
}

/* ---- SHT ---- */

static bool read_sht(sht_sample_t *out)
{
    if (!s_cctx.hal || !s_cctx.hal->read_sht) return false;
    return s_cctx.hal->read_sht(out) == ESP_OK && out->valid;
}

static void update_safety_sht(const sht_sample_t *sht, bool valid)
{
    const dry_params_t *dp = &machine_config_get()->dry;
    bool fresh = valid && (sht->age_ms < dp->sht_stale_timeout_ms);
    safety_manager_update_sht(sht, valid, fresh);
}

/* ---- Cooldown remaining from safety snapshot ---- */

static uint32_t cooldown_remaining_ms(void)
{
    safety_snapshot_t safety;
    if (safety_manager_get_snapshot(&safety) != ESP_OK) return 0;
    if (safety.hot_air_cooldown_until_ms <= 0) return 0;
    int64_t now = s_cctx.hal ? s_cctx.hal->now_ms() : 0;
    int64_t rem = safety.hot_air_cooldown_until_ms - now;
    return rem > 0 ? (uint32_t)rem : 0U;
}

/* ---- Task ---- */

static bool is_safe_to_exit(void)
{
    return cha_fsm_is_idle(&s_cctx.fsm) ||
           (cha_fsm_is_terminal(&s_cctx.fsm) &&
            s_cctx.fsm.terminal_emitted && !s_cctx.pending_terminal.valid);
}

static void cha_task(void *arg)
{
    (void)arg;
    uint32_t my_gen = s_cctx.task_generation;
    xSemaphoreGive(s_cctx.task_ready_sem);

    /* 初始安全 OFF 同步：启动即经 safety 显式请求继电器 OFF。 */
    esp_err_t e_relay = apply_relay_off();
    cha_fsm_ctx_t *fsm = &s_cctx.fsm;
    if (e_relay == ESP_OK) {
        fsm->relay_state = CHA_OUT_OFF;
        s_cctx.outputs_synced = true;
    } else {
        fsm->relay_state = CHA_OUT_UNKNOWN;
        s_cctx.outputs_synced = false;
    }
    ESP_LOGI("cha_svc", "initial safe-off sync: relay=%s synced=%d",
             e_relay == ESP_OK ? "OFF" : "FAIL",
             s_cctx.outputs_synced ? 1 : 0);

    TickType_t last_wake = xTaskGetTickCount();
    bool running = true;
    uint32_t stop_drain_count = 0;
    #define CHA_STOP_DRAIN_MAX  60  /* 60 * 50ms = 3s max drain */

    while (running) {
        uint32_t notify = 0;
        xTaskNotifyWait(0, UINT32_MAX, &notify, 0);

        if (atomic_load(&s_cctx.stop_requested) || (notify & NOTIFY_STOP_BIT)) {
            stop_drain_count++;
            if (is_safe_to_exit() || stop_drain_count >= CHA_STOP_DRAIN_MAX) {
                if (stop_drain_count >= CHA_STOP_DRAIN_MAX) {
                    ESP_LOGW("cha_svc", "stop drain forced exit");
                }
                running = false;
                continue;
            }
            if (!is_safe_to_exit()) {
                cha_fsm_event_t em = {
                    .type = CHA_EVT_EMERGENCY,
                    .now_ms = s_cctx.hal ? s_cctx.hal->now_ms() : 0,
                };
                cha_fsm_output_t out;
                cha_fsm_tick(fsm, &em, &out);
                process_fsm_output(&out);
            }
        }

        if (notify & NOTIFY_EMERGENCY_BIT) {
            if (!is_safe_to_exit()) {
                cha_fsm_event_t em = {
                    .type = CHA_EVT_EMERGENCY,
                    .now_ms = s_cctx.hal ? s_cctx.hal->now_ms() : 0,
                };
                cha_fsm_output_t out;
                cha_fsm_tick(fsm, &em, &out);
                process_fsm_output(&out);
            }
        }

        if (notify & NOTIFY_CANCEL_BIT) {
            cha_fsm_event_t cancel = {
                .type = CHA_EVT_CANCEL,
                .now_ms = s_cctx.hal ? s_cctx.hal->now_ms() : 0,
                .cancel_request_id = atomic_load(&s_cctx.cancel_request_id),
            };
            cha_fsm_output_t out;
            cha_fsm_tick(fsm, &cancel, &out);
            process_fsm_output(&out);
        }

        try_publish_pending();

        /* 只在 FSM 空闲时启动队列中的保留请求，且一次只启动一个；其余队列项
         * 等 FSM 回到 IDLE 再取。防止第二个 run_async 在首个请求运行期间
         * 抢占保留位而覆盖 RUNNING 请求（P1-1）。 */
        cha_cmd_t cmd;
        if (cha_fsm_is_idle(fsm)) {
            while (xQueueReceive(s_cctx.cmd_queue, &cmd, 0) == pdTRUE) {
                if (cmd.was_reserved) {
                    int64_t now = s_cctx.hal ? s_cctx.hal->now_ms() : 0;
                    cha_fsm_begin_request(fsm, &cmd.request, now);
                    break;
                }
            }
        }

        /* 仅当 FSM 空闲且队列已无保留请求时才释放保留位。 */
        if (cha_fsm_is_idle(fsm) && atomic_load(&s_cctx.request_reserved)) {
            release_request_reservation();
        }

        int64_t now = s_cctx.hal ? s_cctx.hal->now_ms() : 0;

        sht_sample_t sht = {0};
        bool sht_ok = read_sht(&sht);
        update_safety_sht(&sht, sht_ok);

        safety_position_view_t pview;
        esp_err_t perr = safety_manager_get_position_view(&pview);

        safety_snapshot_t safety;
        bool safety_ok = safety_manager_get_snapshot(&safety) == ESP_OK;

        cha_fsm_event_t tick = {
            .type = CHA_EVT_TICK,
            .now_ms = now,
            .position = (perr == ESP_OK) ? pview.position : DRUM_POS_UNKNOWN,
            .position_valid = (perr == ESP_OK) && pview.sample_valid,
            .position_stable = (perr == ESP_OK) && pview.stable,
            .position_fresh = (perr == ESP_OK) && pview.fresh,
            .position_motor_moving = (perr == ESP_OK) && pview.motor_moving,
            .sht_valid = sht_ok,
            .sht_fresh = sht_ok && (sht.age_ms < machine_config_get()->dry.sht_stale_timeout_ms),
            .temperature_c = sht.temperature_c,
            .fault_active = safety_ok && safety.fault_active,
            .emergency_active = safety_ok && safety.emergency_stop_active,
            .cooldown_remaining_ms = cooldown_remaining_ms(),
        };

        cha_fsm_output_t out;
        cha_fsm_tick(fsm, &tick, &out);
        process_fsm_output(&out);

        if (s_cctx.snapshot_mtx &&
            xSemaphoreTake(s_cctx.snapshot_mtx, SNAPSHOT_MUTEX_TIMEOUT) == pdTRUE) {
            coupled_hot_air_snapshot_t *snap = &s_cctx.snap;
            switch (fsm->state) {
            case CHA_FSM_IDLE:
                snap->state = fsm->terminal != CHA_TERMINAL_NONE
                    ? CHA_STATE_IDLE_OFF : CHA_STATE_IDLE_OFF;
                break;
            case CHA_FSM_VALIDATING: snap->state = CHA_STATE_STARTING; break;
            case CHA_FSM_STARTING:   snap->state = CHA_STATE_STARTING; break;
            case CHA_FSM_RUNNING:    snap->state = CHA_STATE_RUNNING; break;
            case CHA_FSM_STOPPING:   snap->state = CHA_STATE_STOPPING; break;
            case CHA_FSM_COMPLETE:
                switch (fsm->terminal) {
                case CHA_TERMINAL_COMPLETE:    snap->state = CHA_STATE_COMPLETE; break;
                case CHA_TERMINAL_INTERRUPTED: snap->state = CHA_STATE_INTERRUPTED; break;
                case CHA_TERMINAL_TIMEOUT:     snap->state = CHA_STATE_TIMEOUT; break;
                case CHA_TERMINAL_FAULT:       snap->state = CHA_STATE_FAULT; break;
                default:                       snap->state = CHA_STATE_IDLE_OFF; break;
                }
                break;
            default: snap->state = CHA_STATE_IDLE_OFF; break;
            }
            snap->request_id = fsm->request_id;
            snap->elapsed_ms = (fsm->started_at_ms > 0 && now > fsm->started_at_ms)
                ? (uint32_t)(now - fsm->started_at_ms) : 0U;
            snap->requested_duration_ms = fsm->duration_ms;
            snap->command_on = (fsm->relay_state == CHA_OUT_ON);
            snap->output_known = (fsm->relay_state != CHA_OUT_UNKNOWN);
            snap->output_confirmed_on = (fsm->relay_state == CHA_OUT_ON);
            snap->output_confirmed_off =
                (fsm->terminal != CHA_TERMINAL_NONE)
                    ? fsm->terminal_off_confirmed
                    : (fsm->relay_state == CHA_OUT_OFF);
            snap->started_at_ms = fsm->started_at_ms;
            snap->deadline_ms = fsm->deadline_ms;
            snap->terminal = fsm->terminal;
            snap->terminal_reason = fsm->terminal_reason ? fsm->terminal_reason : "";
            snap->generation = fsm->generation;
            snap->revision = fsm->revision;
            snap->temperature_valid = sht_ok;
            snap->temperature_fresh = sht_ok && (sht.age_ms < machine_config_get()->dry.sht_stale_timeout_ms);
            snap->temperature_c = sht.temperature_c;
            snap->cooldown_remaining_ms = tick.cooldown_remaining_ms;
            xSemaphoreGive(s_cctx.snapshot_mtx);
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TICK_INTERVAL_MS));
    }

    if (my_gen == s_cctx.task_generation) {
        /* 退出前先把 task_handle 清空（release 序），关闭 stop_hook / 通知
         * 路径对已删除任务的悬垂句柄窗口（P1-3）。之后任何 hook 触发都读到
         * NULL，不再对不存在的任务发通知。 */
        atomic_store_explicit(&s_cctx.task_handle, NULL, memory_order_release);
        xSemaphoreGive(s_cctx.task_done_sem);
    }
    vTaskDelete(NULL);
}

/* ---- Public API ---- */

#define GI_TIMEOUT_MS 5000

esp_err_t coupled_hot_air_service_global_init(void)
{
    int expected = GI_UNINITIALIZED;
    if (!atomic_compare_exchange_strong(&s_global_init_state, &expected,
                                        GI_INITIALIZING)) {
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

esp_err_t coupled_hot_air_service_init(const xiaojing_hal_t *hal,
                                       machine_event_sink_t event_sink)
{
    if (!hal) return ESP_ERR_INVALID_ARG;
    if (!s_lc_mtx) return ESP_ERR_INVALID_STATE;

    lc_lock();
    if (s_cctx.lifecycle != LC_UNINITIALIZED) {
        lc_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    s_cctx.hal = hal;
    s_cctx.event_sink = event_sink;
    s_cctx.cutoff_c = machine_config_get()->dry.heater_cutoff_c;
    cha_fsm_init(&s_cctx.fsm, s_cctx.cutoff_c);

    atomic_store(&s_cctx.stop_requested, false);
    atomic_store(&s_cctx.request_reserved, false);
    atomic_store(&s_cctx.reserved_request_id, MACHINE_REQUEST_ID_INVALID);
    atomic_store(&s_cctx.cancel_request_id, MACHINE_REQUEST_ID_INVALID);
    atomic_store_explicit(&s_cctx.task_handle, NULL, memory_order_release);
    atomic_store(&s_cctx.notify_attempt_count, 0);

    s_cctx.snapshot_mtx = xSemaphoreCreateMutex();
    s_cctx.task_ready_sem = xSemaphoreCreateBinary();
    s_cctx.task_done_sem = xSemaphoreCreateBinary();
    s_cctx.cmd_queue = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(cha_cmd_t));

    if (!s_cctx.snapshot_mtx || !s_cctx.task_ready_sem ||
        !s_cctx.task_done_sem || !s_cctx.cmd_queue) {
        cleanup_resources();
        lc_unlock();
        return ESP_ERR_NO_MEM;
    }

    s_cctx.lifecycle = LC_INITIALIZED;
    lc_unlock();
    return ESP_OK;
}

esp_err_t coupled_hot_air_service_start(void)
{
    if (!s_lc_mtx) return ESP_ERR_INVALID_STATE;

    lc_lock();
    if (s_cctx.lifecycle != LC_INITIALIZED) {
        lc_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    s_cctx.lifecycle = LC_STARTING;
    atomic_store(&s_cctx.stop_requested, false);
    s_cctx.done_sem_consumed = false;
    s_cctx.task_generation++;

    TaskHandle_t handle = xTaskCreateStaticPinnedToCore(
        cha_task, "cha_task", TASK_STACK_SIZE, NULL,
        TASK_PRIORITY, s_task_stack, &s_task_tcb, TASK_CORE);
    if (!handle) {
        s_cctx.lifecycle = LC_INITIALIZED;
        lc_unlock();
        return ESP_ERR_NO_MEM;
    }
    atomic_store_explicit(&s_cctx.task_handle, handle, memory_order_release);
    lc_unlock();

    if (xSemaphoreTake(s_cctx.task_ready_sem, pdMS_TO_TICKS(INIT_TIMEOUT_MS)) != pdTRUE) {
        atomic_store(&s_cctx.stop_requested, true);
        TaskHandle_t h = atomic_load_explicit(&s_cctx.task_handle, memory_order_acquire);
        if (h) xTaskNotify(h, NOTIFY_STOP_BIT, eSetBits);
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t hook_err = safety_manager_register_stop_hook(cha_stop_hook, NULL, "cha_svc");
    if (hook_err != ESP_OK) {
        atomic_store(&s_cctx.stop_requested, true);
        TaskHandle_t h = atomic_load_explicit(&s_cctx.task_handle, memory_order_acquire);
        if (h) xTaskNotify(h, NOTIFY_STOP_BIT, eSetBits);
        SemaphoreHandle_t ds = s_cctx.task_done_sem;
        if (ds && !s_cctx.done_sem_consumed) {
            if (xSemaphoreTake(ds, pdMS_TO_TICKS(STOP_TIMEOUT_MS)) == pdTRUE)
                s_cctx.done_sem_consumed = true;
        }
        if (s_cctx.done_sem_consumed) {
            atomic_store_explicit(&s_cctx.task_handle, NULL, memory_order_release);
            cleanup_resources();
        } else {
            lc_lock();
            s_cctx.lifecycle = LC_STOPPING;
            lc_unlock();
        }
        return hook_err;
    }
    s_cctx.stop_hook_registered = true;

    lc_lock();
    if (s_cctx.lifecycle != LC_STARTING || atomic_load(&s_cctx.stop_requested)) {
        if (s_cctx.stop_hook_registered) {
            esp_err_t uerr = safety_manager_unregister_stop_hook(cha_stop_hook, NULL);
            if (uerr != ESP_OK) {
                lc_unlock();
                coupled_hot_air_service_stop();
                return ESP_ERR_INVALID_STATE;
            }
            s_cctx.stop_hook_registered = false;
        }
        s_cctx.lifecycle = LC_STOPPING;
        lc_unlock();
        coupled_hot_air_service_stop();
        return ESP_ERR_INVALID_STATE;
    }

    s_cctx.lifecycle = LC_RUNNING;
    lc_unlock();
    return ESP_OK;
}

esp_err_t coupled_hot_air_service_stop(void)
{
    if (!s_lc_mtx) return ESP_OK;

    lc_lock();
    switch (s_cctx.lifecycle) {
    case LC_UNINITIALIZED:
        lc_unlock();
        return ESP_OK;
    case LC_INITIALIZED:
        cleanup_resources();
        lc_unlock();
        return ESP_OK;
    case LC_STARTING:
    case LC_RUNNING:
        s_cctx.lifecycle = LC_STOPPING;
        atomic_store(&s_cctx.stop_requested, true);
        break;
    case LC_STOPPING:
        atomic_store(&s_cctx.stop_requested, true);
        break;
    }
    lc_unlock();

    TaskHandle_t h = atomic_load_explicit(&s_cctx.task_handle, memory_order_acquire);
    if (h) xTaskNotify(h, NOTIFY_STOP_BIT, eSetBits);

    SemaphoreHandle_t ds = s_cctx.task_done_sem;
    esp_err_t join_err = ESP_OK;
    if (ds && !s_cctx.done_sem_consumed) {
        if (xSemaphoreTake(ds, pdMS_TO_TICKS(STOP_TIMEOUT_MS)) != pdTRUE) {
            join_err = ESP_ERR_TIMEOUT;
        } else {
            s_cctx.done_sem_consumed = true;
        }
    }

    lc_lock();
    if (join_err != ESP_OK) {
        lc_unlock();
        return join_err;
    }
    atomic_store_explicit(&s_cctx.task_handle, NULL, memory_order_release);

    if (s_cctx.stop_hook_registered) {
        esp_err_t uerr = safety_manager_unregister_stop_hook(cha_stop_hook, NULL);
        if (uerr != ESP_OK) {
            lc_unlock();
            return uerr;
        }
        s_cctx.stop_hook_registered = false;
    }
    cleanup_resources();
    lc_unlock();
    return ESP_OK;
}

esp_err_t coupled_hot_air_service_run_async(const coupled_hot_air_request_t *request)
{
    if (!request) return ESP_ERR_INVALID_ARG;
    if (request->request_id == MACHINE_REQUEST_ID_INVALID) return ESP_ERR_INVALID_ARG;
    if (request->duration_ms == 0) return ESP_ERR_INVALID_ARG;
    if (request->duration_ms > COUPLED_HOT_AIR_HARD_CAP_MS) return ESP_ERR_INVALID_ARG;

    lc_lock();
    if (s_cctx.lifecycle != LC_RUNNING) { lc_unlock(); return ESP_ERR_INVALID_STATE; }
    if (!try_reserve_request(request)) { lc_unlock(); return ESP_ERR_INVALID_STATE; }

    cha_cmd_t cmd = { .request = *request, .was_reserved = true };
    if (xQueueSend(s_cctx.cmd_queue, &cmd, pdMS_TO_TICKS(10)) != pdTRUE) {
        release_request_reservation();
        lc_unlock();
        return ESP_ERR_TIMEOUT;
    }
    lc_unlock();
    return ESP_OK;
}

esp_err_t coupled_hot_air_service_cancel(machine_request_id_t request_id)
{
    if (request_id == MACHINE_REQUEST_ID_INVALID) return ESP_ERR_INVALID_ARG;
    lc_lock();
    if (s_cctx.lifecycle != LC_RUNNING) { lc_unlock(); return ESP_ERR_INVALID_STATE; }
    lc_unlock();
    atomic_store(&s_cctx.cancel_request_id, request_id);
    TaskHandle_t h = atomic_load_explicit(&s_cctx.task_handle, memory_order_acquire);
    if (h) xTaskNotify(h, NOTIFY_CANCEL_BIT, eSetBits);
    return ESP_OK;
}

esp_err_t coupled_hot_air_service_emergency_stop(void)
{
    /* 与生命周期 stop 分离：绝不设置 stop_requested。
     * 立即经 safety 请求继电器 OFF（safety 对 OFF 无条件放行），再通知任务
     * 走 FSM 关断序列收敛终态。 */
    esp_err_t relay_err = apply_relay_off();
    TaskHandle_t h = atomic_load_explicit(&s_cctx.task_handle, memory_order_acquire);
    if (h) {
        atomic_fetch_add(&s_cctx.notify_attempt_count, 1);
        xTaskNotify(h, NOTIFY_EMERGENCY_BIT, eSetBits);
    }
    return relay_err;
}

esp_err_t coupled_hot_air_service_get_snapshot(coupled_hot_air_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_cctx.snapshot_mtx) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_cctx.snapshot_mtx, SNAPSHOT_MUTEX_TIMEOUT) == pdTRUE) {
        *out = s_cctx.snap;
        xSemaphoreGive(s_cctx.snapshot_mtx);
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}
