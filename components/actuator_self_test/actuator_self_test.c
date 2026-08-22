/*
 * actuator_self_test.c — 执行器板测运行时组件
 *
 * 调用链（受 15 项安全门禁保护）：
 *   BLE actuator_self_test → actuator_self_test_request(target)
 *     → 采集 executor/自身/uv/safety/position/water/detergent 快照
 *     → actuator_self_test_gate_check()
 *     → water_service_actuator_valve_test() / detergent_service_run_async()
 *     → safety_manager_apply → MCP23017
 *
 * 设备端计时与自动 OFF 由对应 service 负责（water 任务自身计时关阀、
 * detergent FSM 到期关泵），即使 BLE 断开/小程序崩溃也自动关闭。
 * 本组件通过任务轮询服务快照推进板测状态机，并在 fault/emergency 时
 * 请求紧急关断。
 */

#include "actuator_self_test.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "safety_manager.h"
#include "water_service.h"
#include "detergent_service.h"
#include "drain_service.h"
#include "dry_service.h"
#include "coupled_hot_air_service.h"
#include "uv_self_test.h"
#include "wash_executor.h"
#include "machine_config.h"

#define ACT_TICK_MS               50U
#define ACT_TASK_STACK            3072
#define ACT_TASK_PRIORITY         15
#define ACT_TASK_CORE             1
#define ACT_SUBMIT_GRACE_MS       2000U  /* 服务受理前允许的等待窗口 */
#define ACT_HARD_TIMEOUT_GRACE_MS 1500U  /* 超出固定时长后的硬超时冗余 */

/* 输出三态（与 uv/detergent fsm 输出枚举一致：UNKNOWN=0 / KNOWN_OFF=1 / KNOWN_ON=2） */
#define ACT_OUT_UNKNOWN 0
#define ACT_OUT_OFF     1
#define ACT_OUT_ON      2

#define ACT_NOTIFY_RUN        (1U << 0)
#define ACT_NOTIFY_EMERGENCY  (1U << 1)

static const char *TAG = "act_self_test";

typedef struct {
    actuator_self_test_config_t cfg;
    bool initialized;
    bool started;

    TaskHandle_t task;
    SemaphoreHandle_t task_done_sem;
    _Atomic bool stop_requested;
    /* 锁竞争安全 emergency 闩锁：任何 emergency 入口先置位；任务在互斥量
     * 内恢复后首先消费并执行关断，即使 emergency() 未能拿到互斥量。 */
    _Atomic bool emergency_latch;

    machine_request_id_t next_request_id;
    actuator_self_test_state_t state;
    actuator_self_test_terminal_t terminal;
    actuator_self_test_target_t target;
    machine_request_id_t request_id;
    uint32_t requested_duration_ms;
    uint32_t elapsed_ms;
    const char *last_code;
    bool output_confirmed_off;

    _Atomic bool test_active;  /* ACCEPTED/RUNNING/CHECKING 中（跨任务读） */
    uint32_t start_tick_ms;
} actuator_self_test_rt_t;

static actuator_self_test_rt_t s_rt;

/* Mutex lives OUTSIDE s_rt: init() memsets s_rt and must not wipe the handle. */
static SemaphoreHandle_t s_mtx = NULL;

static bool lock(void)
{
    return s_mtx && xSemaphoreTake(s_mtx, pdMS_TO_TICKS(500)) == pdTRUE;
}

static bool drain_confirmed_off(void);

/* ---- 纯辅助 ---- */

static safe_output_t act_safe_output(actuator_self_test_target_t t)
{
    switch (t) {
    case ACT_TARGET_SOURCE_VALVE:    return SAFE_OUTPUT_TAP_VALVE;
    case ACT_TARGET_TRANSFER_VALVE:  return SAFE_OUTPUT_TRANSFER_VALVE;
    case ACT_TARGET_DETERGENT_PUMP:  return SAFE_OUTPUT_DETERGENT_PUMP;
    case ACT_TARGET_DRAIN_PUMP:      return SAFE_OUTPUT_DRAIN_VALVE;
    case ACT_TARGET_HOT_AIR_COUPLED: return SAFE_OUTPUT_PTC_HEATER;
    default:                         return (safe_output_t)-1;
    }
}

static void set_terminal_locked(actuator_self_test_terminal_t t, const char *code)
{
    if (s_rt.terminal != ACT_TERMINAL_NONE) return;   /* terminal exactly once */
    s_rt.terminal = t;
    s_rt.last_code = code;
    /* INTERRUPTED 仅在所有 OFF 请求成功（emergency 关断/服务 CANCELED 终态且
     * 输出 KNOWN_OFF）时置位 → 输出已确认关闭，off=true。FAULT/TIMEOUT 为
     * off=false（OFF 未确认，fail-closed）。 */
    s_rt.output_confirmed_off =
        (t == ACT_TERMINAL_COMPLETE || t == ACT_TERMINAL_INTERRUPTED);
    atomic_store(&s_rt.test_active, false);
    switch (t) {
    case ACT_TERMINAL_COMPLETE:    s_rt.state = ACT_STATE_COMPLETE; break;
    case ACT_TERMINAL_REJECTED:    s_rt.state = ACT_STATE_REJECTED; break;
    case ACT_TERMINAL_INTERRUPTED: s_rt.state = ACT_STATE_INTERRUPTED; break;
    case ACT_TERMINAL_TIMEOUT:     s_rt.state = ACT_STATE_TIMEOUT; break;
    case ACT_TERMINAL_FAULT:       s_rt.state = ACT_STATE_FAULT; break;
    default:                       s_rt.state = ACT_STATE_UNKNOWN; break;
    }
    ESP_LOGI(TAG, "ACT_SELFTEST terminal=%d state=%d code=%s target=%s",
             (int)t, (int)s_rt.state, code ? code : "?",
             actuator_self_test_target_name(s_rt.target));
}

/* 请求所有板测输出 OFF。返回 ESP_OK 表示全部 OFF 请求成功；否则错误，
 * 调用方应将终态置为 FAULT（输出可能仍带电）。 */
static esp_err_t act_emergency_outputs(void)
{
    esp_err_t r = ESP_OK;
    esp_err_t e;
    e = water_service_actuator_valve_close(WATER_ACTUATOR_VALVE_SOURCE);
    if (e != ESP_OK) r = e;
    e = water_service_actuator_valve_close(WATER_ACTUATOR_VALVE_TRANSFER);
    if (e != ESP_OK) r = e;
    e = water_service_emergency_close();
    if (e != ESP_OK) r = e;
    e = detergent_service_emergency_stop();
    if (e != ESP_OK) r = e;
    e = drain_service_emergency_stop();
    if (e != ESP_OK) r = e;
    e = dry_service_emergency_stop();
    if (e != ESP_OK) r = e;
    e = coupled_hot_air_service_emergency_stop();
    if (e != ESP_OK) r = e;
    return r;
}

/* ---- 门禁输入采集 ---- */

static void collect_gate_input(actuator_self_test_gate_input_t *in,
                               actuator_self_test_target_t target)
{
    memset(in, 0, sizeof(*in));
    in->target = target;
    in->pos = DRUM_POS_UNKNOWN;
    in->required_pos = DRUM_POS_0;
    /* 非 water 目标不要求该同步；water 目标在下方按快照覆盖。 */
    in->water_safe_off_synced = true;
    /* 非 fan/heater 目标：风扇/加热器/温度前置为空条件（默认满足）。 */
    in->fan_confirmed_off = true;
    in->heater_confirmed_off = true;
    in->sht_valid_fresh = true;
    in->sht_below_cutoff = true;
    /* 非热风目标：耦合模块前置为空条件（默认满足）。 */
    in->hot_air_service_idle = true;
    in->hot_air_confirmed_off = true;
    in->hot_air_cooldown_active = false;

    switch (target) {
    case ACT_TARGET_DRAIN_PUMP:  in->required_pos = DRUM_POS_180; break;
    case ACT_TARGET_FAN:         in->required_pos = DRUM_POS_270; break;
    case ACT_TARGET_HOT_AIR_COUPLED: in->required_pos = DRUM_POS_270; break;
    default:                     in->required_pos = DRUM_POS_0;   break;
    }

    if (s_rt.cfg.executor) {
        wash_exec_snapshot_t snap;
        if (wash_executor_get_snapshot(s_rt.cfg.executor, &snap) == ESP_OK) {
            in->exec_idle = snap.state == WASH_EXEC_STATE_IDLE &&
                            !snap.program_active && !snap.terminal_pending;
        }
    }

    /* 无其他自检运行：自身活动中的板测，或 UV 自检在运行。 */
    bool act_busy = atomic_load(&s_rt.test_active);
    uint8_t uv_code = uv_self_test_state_code();
    bool uv_busy = uv_code == (uint8_t)UV_STATE_SELFTEST_VALIDATING ||
                   uv_code == (uint8_t)UV_STATE_SELFTEST_RUNNING ||
                   uv_code == (uint8_t)UV_STATE_SELFTEST_STOPPING;
    in->self_test_busy = act_busy || uv_busy;

    safety_snapshot_t safety;
    if (safety_manager_get_snapshot(&safety) == ESP_OK) {
        in->fault_active = safety.fault_active;
        in->emergency_active = safety.emergency_stop_active;
        safe_output_t so = act_safe_output(target);
        if (so != (safe_output_t)-1) {
            in->mcp_target_known =
                (safety.mcp_known_mask & (1U << (unsigned)so)) != 0U;
        }
    }

    safety_position_view_t view;
    if (safety_manager_get_position_view(&view) == ESP_OK) {
        in->pos_valid = view.sample_valid;
        in->pos_fresh = view.fresh;
        in->pos_stable = view.stable;
        in->pos_moving = view.motor_moving;
        in->pos = view.position;
    }

    /* P1-2：所有服务快照初始化为确定值，分别记录 snapshot_ok；任何目标依赖
     * 的快照读取失败 → 置 snapshot_fail（fail-closed），绝不读取失败快照字段。 */
    water_snapshot_t ws = {0};
    bool water_ok = (water_service_get_snapshot(&ws) == ESP_OK);
    if (water_ok) {
        in->water_service_idle =
            ws.state == WATER_STATE_IDLE && !ws.act_test_active &&
            !ws.terminal_pending;
        switch (target) {
        case ACT_TARGET_SOURCE_VALVE:
            in->water_safe_off_synced = ws.safe_off_synced;
            in->target_confirmed_off = !ws.source_valve_on && !ws.source_valve_unknown;
            in->conflict_confirmed_off =
                (!ws.transfer_valve_on && !ws.transfer_valve_unknown) &&
                drain_confirmed_off();
            break;
        case ACT_TARGET_TRANSFER_VALVE:
            in->water_safe_off_synced = ws.safe_off_synced;
            in->target_confirmed_off = !ws.transfer_valve_on && !ws.transfer_valve_unknown;
            in->conflict_confirmed_off =
                (!ws.source_valve_on && !ws.source_valve_unknown) &&
                drain_confirmed_off();
            break;
        default:
            in->target_confirmed_off = true;
            in->conflict_confirmed_off = true;
            break;
        }
    } else {
        in->snapshot_fail = ACT_GATE_WATER_SNAPSHOT_UNAVAILABLE;
    }

    detergent_snapshot_t ds = {0};
    bool det_ok = (detergent_service_get_snapshot(&ds) == ESP_OK);
    if (det_ok) {
        in->detergent_service_idle =
            ds.state == DETERGENT_STATE_IDLE && !ds.terminal_pending;
        if (target == ACT_TARGET_DETERGENT_PUMP) {
            in->target_confirmed_off = (ds.output_state == ACT_OUT_OFF);
        }
    } else if (in->snapshot_fail == ACT_GATE_OK) {
        in->snapshot_fail = ACT_GATE_DETERGENT_SNAPSHOT_UNAVAILABLE;
    }

    drain_snapshot_t drain = {0};
    bool drain_ok = (drain_service_get_snapshot(&drain) == ESP_OK);
    if (drain_ok) {
        in->drain_service_idle =
            drain.state == DRAIN_STATE_IDLE && !drain.terminal_pending;
        if (target == ACT_TARGET_DRAIN_PUMP) {
            in->target_confirmed_off = (drain.output_state == ACT_OUT_OFF);
            /* 排水与进水阀互斥：source/transfer 均须 OFF（仅在 water 快照可用
             * 时读取；不可用则由 snapshot_fail 提前拒绝）。 */
            if (water_ok) {
                in->conflict_confirmed_off =
                    (!ws.source_valve_on && !ws.source_valve_unknown) &&
                    (!ws.transfer_valve_on && !ws.transfer_valve_unknown);
            } else {
                in->conflict_confirmed_off = false;
            }
        }
    } else if (in->snapshot_fail == ACT_GATE_OK) {
        in->snapshot_fail = ACT_GATE_DRAIN_SNAPSHOT_UNAVAILABLE;
    }

    dry_snapshot_t dry = {0};
    bool dry_ok = (dry_service_get_snapshot(&dry) == ESP_OK);
    if (dry_ok) {
        in->dry_service_idle =
            dry.state == DRY_STATE_IDLE && !dry.terminal_pending;
        if (target == ACT_TARGET_FAN) {
            /* 风扇非 MCP 输出：known 以 dry 快照非 UNKNOWN 表达。 */
            in->mcp_target_known = (dry.fan_output_state != ACT_OUT_UNKNOWN);
            in->target_confirmed_off = (dry.fan_output_state == ACT_OUT_OFF);
            in->fan_confirmed_off = (dry.fan_output_state == ACT_OUT_OFF);
            in->heater_confirmed_off = (dry.heater_output_state == ACT_OUT_OFF);
            in->sht_valid_fresh = dry.sht_valid && dry.sht_fresh;
            const float cutoff = (machine_config_get())
                ? machine_config_get()->dry.heater_cutoff_c : 55.0f;
            in->sht_below_cutoff =
                dry.sht_valid && dry.sht_fresh &&
                dry.temperature_c < cutoff;
            /* 风扇测试须全输出 OFF（水阀/排水/洗涤剂互斥）。 */
            if (water_ok && det_ok && drain_ok) {
                in->conflict_confirmed_off =
                    (!ws.source_valve_on && !ws.source_valve_unknown) &&
                    (!ws.transfer_valve_on && !ws.transfer_valve_unknown) &&
                    drain_confirmed_off() &&
                    (ds.output_state == ACT_OUT_OFF);
            } else {
                in->conflict_confirmed_off = false;
            }
        }
    } else if (in->snapshot_fail == ACT_GATE_OK) {
        in->snapshot_fail = ACT_GATE_DRY_SNAPSHOT_UNAVAILABLE;
    }

    coupled_hot_air_snapshot_t cha = {0};
    bool cha_ok = (coupled_hot_air_service_get_snapshot(&cha) == ESP_OK);
    if (cha_ok) {
        in->hot_air_service_idle = (cha.state == CHA_STATE_IDLE_OFF);
        if (target == ACT_TARGET_HOT_AIR_COUPLED) {
            in->hot_air_confirmed_off = cha.output_known && cha.output_confirmed_off;
            in->hot_air_cooldown_active = cha.cooldown_remaining_ms > 0;
            in->target_confirmed_off = in->hot_air_confirmed_off;
            in->sht_valid_fresh = cha.temperature_valid && cha.temperature_fresh;
            const float cutoff = (machine_config_get())
                ? machine_config_get()->dry.heater_cutoff_c : 55.0f;
            in->sht_below_cutoff = cha.temperature_valid && cha.temperature_fresh &&
                cha.temperature_c < cutoff;
            /* 热风模块须全输出 OFF（水阀/排水/洗涤剂/UV 互斥）。 */
            if (water_ok && det_ok && drain_ok) {
                in->conflict_confirmed_off =
                    (!ws.source_valve_on && !ws.source_valve_unknown) &&
                    (!ws.transfer_valve_on && !ws.transfer_valve_unknown) &&
                    drain_confirmed_off() &&
                    (ds.output_state == ACT_OUT_OFF);
            } else {
                in->conflict_confirmed_off = false;
            }
        }
    } else if (in->snapshot_fail == ACT_GATE_OK) {
        in->snapshot_fail = ACT_GATE_HOT_AIR_SNAPSHOT_UNAVAILABLE;
    }

    in->request_id_ok = (s_rt.next_request_id != MACHINE_REQUEST_ID_INVALID);
}

static bool drain_confirmed_off(void)
{
    drain_snapshot_t ds;
    if (drain_service_get_snapshot(&ds) != ESP_OK) return false;
    return ds.output_state == ACT_OUT_OFF;
}

/* ---- 状态推进（仅任务调用） ---- */

static void advance_water_locked(void)
{
    water_snapshot_t ws;
    if (water_service_get_snapshot(&ws) != ESP_OK) return;

    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    s_rt.elapsed_ms = (now_ms > s_rt.start_tick_ms)
        ? (now_ms - s_rt.start_tick_ms) : 0U;

    if (ws.act_test_request_id != s_rt.request_id) {
        /* 仅当另一板测确实在 RUNNING 且 rid 不同 → 状态丢失。连续自检时上一个
         * 测试的 rid 残留且 act_test_active=false，属"尚未受理"，等宽限而非 FAULT。 */
        if (ws.act_test_active &&
            ws.act_test_state == WATER_ACT_TEST_RUNNING &&
            ws.act_test_request_id != MACHINE_REQUEST_ID_INVALID) {
            set_terminal_locked(ACT_TERMINAL_FAULT, "ACT_STATE_LOST");
            return;
        }
        if (s_rt.elapsed_ms > ACT_SUBMIT_GRACE_MS) {
            set_terminal_locked(ACT_TERMINAL_FAULT, "ACT_SUBMIT_FAILED");
        }
        return;
    }

    switch (ws.act_test_state) {
    case WATER_ACT_TEST_RUNNING:
        s_rt.output_confirmed_off = false;
        if (s_rt.state != ACT_STATE_RUNNING) {
            ESP_LOGI(TAG, "ACT_SELFTEST ON confirmed (target=%s elapsed=%" PRIu32 ")",
                     actuator_self_test_target_name(s_rt.target), s_rt.elapsed_ms);
        }
        s_rt.state = ACT_STATE_RUNNING;
        break;
    case WATER_ACT_TEST_COMPLETE:
        set_terminal_locked(ACT_TERMINAL_COMPLETE, "ACTUATOR_SELF_TEST_ACCEPTED");
        break;
    case WATER_ACT_TEST_FAULT:
        set_terminal_locked(ACT_TERMINAL_FAULT, "OFF_CONFIRM_FAILED");
        break;
    default:
        if (s_rt.elapsed_ms > ACT_SUBMIT_GRACE_MS) {
            set_terminal_locked(ACT_TERMINAL_UNKNOWN, "ACT_STATE_LOST");
        }
        break;
    }
}

static void advance_detergent_locked(void)
{
    detergent_snapshot_t ds;
    if (detergent_service_get_snapshot(&ds) != ESP_OK) return;

    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    s_rt.elapsed_ms = (now_ms > s_rt.start_tick_ms)
        ? (now_ms - s_rt.start_tick_ms) : 0U;

    if (ds.request_id != s_rt.request_id) {
        /* 仅当另一请求仍在 RUNNING（state 非 IDLE）→ 状态丢失。连续自检时
         * 上一终态 rid 残留且 state=IDLE，属"尚未受理"，等宽限。 */
        if (ds.state != DETERGENT_STATE_IDLE &&
            ds.request_id != MACHINE_REQUEST_ID_INVALID) {
            set_terminal_locked(ACT_TERMINAL_FAULT, "ACT_STATE_LOST");
            return;
        }
        if (s_rt.elapsed_ms > ACT_SUBMIT_GRACE_MS) {
            set_terminal_locked(ACT_TERMINAL_FAULT, "ACT_SUBMIT_FAILED");
        }
        return;
    }

    if (ds.output_state == ACT_OUT_ON) {
        s_rt.output_confirmed_off = false;
        if (s_rt.state != ACT_STATE_RUNNING) {
            ESP_LOGI(TAG, "ACT_SELFTEST ON confirmed (target=%s elapsed=%" PRIu32 ")",
                     actuator_self_test_target_name(s_rt.target), s_rt.elapsed_ms);
        }
        s_rt.state = ACT_STATE_RUNNING;
    }

    if (ds.terminal != DETERGENT_TERMINAL_NONE) {
        switch (ds.terminal) {
        case DETERGENT_TERMINAL_COMPLETE:
            if (ds.output_state == ACT_OUT_OFF) {
                set_terminal_locked(ACT_TERMINAL_COMPLETE,
                                    "ACTUATOR_SELF_TEST_ACCEPTED");
            } else {
                set_terminal_locked(ACT_TERMINAL_FAULT, "OFF_CONFIRM_FAILED");
            }
            break;
        case DETERGENT_TERMINAL_CANCELED:
            set_terminal_locked(ACT_TERMINAL_INTERRUPTED, "DETERGENT_CANCELED");
            break;
        case DETERGENT_TERMINAL_TIMEOUT:
            set_terminal_locked(ACT_TERMINAL_TIMEOUT, "DETERGENT_TIMEOUT");
            break;
        case DETERGENT_TERMINAL_REJECTED:
            set_terminal_locked(ACT_TERMINAL_REJECTED, "DETERGENT_REJECTED");
            break;
        case DETERGENT_TERMINAL_FAULT:
            set_terminal_locked(ACT_TERMINAL_FAULT, "DETERGENT_FAULT");
            break;
        default:
            set_terminal_locked(ACT_TERMINAL_UNKNOWN, "DETERGENT_TERMINAL_UNKNOWN");
            break;
        }
    }

    /* 硬超时兜底：超过固定时长+冗余仍未终态 → 请求关断并置 TIMEOUT。 */
    if (atomic_load(&s_rt.test_active) &&
        s_rt.elapsed_ms > s_rt.requested_duration_ms + ACT_HARD_TIMEOUT_GRACE_MS) {
        (void)detergent_service_emergency_stop();
        set_terminal_locked(ACT_TERMINAL_TIMEOUT, "ACT_TIMEOUT");
    }
}

static void advance_drain_locked(void)
{
    drain_snapshot_t ds;
    if (drain_service_get_snapshot(&ds) != ESP_OK) return;

    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    s_rt.elapsed_ms = (now_ms > s_rt.start_tick_ms)
        ? (now_ms - s_rt.start_tick_ms) : 0U;

    if (ds.request_id != s_rt.request_id) {
        /* 仅当另一请求仍在 RUNNING（state 非 IDLE）→ 状态丢失（连续自检同 water）。 */
        if (ds.state != DRAIN_STATE_IDLE &&
            ds.request_id != MACHINE_REQUEST_ID_INVALID) {
            set_terminal_locked(ACT_TERMINAL_FAULT, "ACT_STATE_LOST");
            return;
        }
        if (s_rt.elapsed_ms > ACT_SUBMIT_GRACE_MS) {
            set_terminal_locked(ACT_TERMINAL_FAULT, "ACT_SUBMIT_FAILED");
        }
        return;
    }

    if (ds.output_state == ACT_OUT_ON) {
        s_rt.output_confirmed_off = false;
        if (s_rt.state != ACT_STATE_RUNNING) {
            ESP_LOGI(TAG, "ACT_SELFTEST ON confirmed (target=%s elapsed=%" PRIu32 ")",
                     actuator_self_test_target_name(s_rt.target), s_rt.elapsed_ms);
        }
        s_rt.state = ACT_STATE_RUNNING;
    }

    if (ds.terminal != DRAIN_TERMINAL_NONE) {
        switch (ds.terminal) {
        case DRAIN_TERMINAL_COMPLETE:
            if (ds.output_state == ACT_OUT_OFF) {
                set_terminal_locked(ACT_TERMINAL_COMPLETE,
                                    "ACTUATOR_SELF_TEST_ACCEPTED");
            } else {
                set_terminal_locked(ACT_TERMINAL_FAULT, "OFF_CONFIRM_FAILED");
            }
            break;
        case DRAIN_TERMINAL_CANCELED:
            set_terminal_locked(ACT_TERMINAL_INTERRUPTED, "DRAIN_CANCELED");
            break;
        case DRAIN_TERMINAL_TIMEOUT:
            set_terminal_locked(ACT_TERMINAL_TIMEOUT, "DRAIN_TIMEOUT");
            break;
        case DRAIN_TERMINAL_REJECTED:
            set_terminal_locked(ACT_TERMINAL_REJECTED, "DRAIN_REJECTED");
            break;
        case DRAIN_TERMINAL_FAULT:
            set_terminal_locked(ACT_TERMINAL_FAULT, "DRAIN_FAULT");
            break;
        default:
            set_terminal_locked(ACT_TERMINAL_UNKNOWN, "DRAIN_TERMINAL_UNKNOWN");
            break;
        }
    }

    if (atomic_load(&s_rt.test_active) &&
        s_rt.elapsed_ms > s_rt.requested_duration_ms + ACT_HARD_TIMEOUT_GRACE_MS) {
        (void)drain_service_emergency_stop();
        set_terminal_locked(ACT_TERMINAL_TIMEOUT, "ACT_TIMEOUT");
    }
}

/* Fan-only dry 自检：dry_fsm 的 terminal 在发布后回 IDLE 仍保留（同 UV 契约）。
 * 轮询在 IDLE 看到保留 terminal 即判终态；命令/锁存 OFF（无 tach 反馈）。 */
static void advance_dry_locked(void)
{
    dry_snapshot_t ds;
    if (dry_service_get_snapshot(&ds) != ESP_OK) return;

    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    s_rt.elapsed_ms = (now_ms > s_rt.start_tick_ms)
        ? (now_ms - s_rt.start_tick_ms) : 0U;

    if (ds.request_id != s_rt.request_id) {
        /* 仅当另一请求仍在 RUNNING（state 非 IDLE）→ 状态丢失（连续自检同 water）。 */
        if (ds.state != DRY_STATE_IDLE &&
            ds.request_id != MACHINE_REQUEST_ID_INVALID) {
            set_terminal_locked(ACT_TERMINAL_FAULT, "ACT_STATE_LOST");
            return;
        }
        if (s_rt.elapsed_ms > ACT_SUBMIT_GRACE_MS) {
            set_terminal_locked(ACT_TERMINAL_FAULT, "ACT_SUBMIT_FAILED");
        }
        return;
    }

    if (ds.fan_output_state == ACT_OUT_ON) {
        s_rt.output_confirmed_off = false;
        if (s_rt.state != ACT_STATE_RUNNING) {
            ESP_LOGI(TAG, "ACT_SELFTEST ON confirmed (target=%s elapsed=%" PRIu32 ")",
                     actuator_self_test_target_name(s_rt.target), s_rt.elapsed_ms);
        }
        s_rt.state = ACT_STATE_RUNNING;
    }

    if (ds.terminal != DRY_TERMINAL_NONE) {
        if (ds.fan_output_state == ACT_OUT_OFF) {
            switch (ds.terminal) {
            case DRY_TERMINAL_COMPLETE:
                set_terminal_locked(ACT_TERMINAL_COMPLETE,
                                    "ACTUATOR_SELF_TEST_ACCEPTED");
                break;
            case DRY_TERMINAL_CANCELED:
                set_terminal_locked(ACT_TERMINAL_INTERRUPTED, "DRY_CANCELED");
                break;
            default:
                set_terminal_locked(ACT_TERMINAL_FAULT, "DRY_FAULT");
                break;
            }
        } else {
            set_terminal_locked(ACT_TERMINAL_FAULT, "OFF_CONFIRM_FAILED");
        }
    }

    if (atomic_load(&s_rt.test_active) &&
        s_rt.elapsed_ms > s_rt.requested_duration_ms + ACT_HARD_TIMEOUT_GRACE_MS) {
        (void)dry_service_emergency_stop();
        set_terminal_locked(ACT_TERMINAL_TIMEOUT, "ACT_TIMEOUT");
    }
}

/* 耦合热风模块自检：coupled_hot_air_service 的终态契约同 UV/dry —— 服务 FSM
 * 在 OFF 确认后发出终态；发布后回 IDLE。轮询在终态看到输出确认即判终态。
 * 命令/锁存 OFF（无触点/气流回读），绝不明示物理转速。 */
static void advance_hot_air_locked(void)
{
    coupled_hot_air_snapshot_t cha;
    if (coupled_hot_air_service_get_snapshot(&cha) != ESP_OK) return;

    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    s_rt.elapsed_ms = (now_ms > s_rt.start_tick_ms)
        ? (now_ms - s_rt.start_tick_ms) : 0U;

    if (cha.request_id != s_rt.request_id) {
        /* 仅当另一请求仍在 RUNNING（state 非 IDLE_OFF）→ 状态丢失
         * （连续自检同 water，见 R281）。 */
        if (cha.state != CHA_STATE_IDLE_OFF &&
            cha.request_id != MACHINE_REQUEST_ID_INVALID) {
            set_terminal_locked(ACT_TERMINAL_FAULT, "ACT_STATE_LOST");
            return;
        }
        if (s_rt.elapsed_ms > ACT_SUBMIT_GRACE_MS) {
            set_terminal_locked(ACT_TERMINAL_FAULT, "ACT_SUBMIT_FAILED");
        }
        return;
    }

    if (cha.state == CHA_STATE_RUNNING) {
        s_rt.output_confirmed_off = false;
        if (s_rt.state != ACT_STATE_RUNNING) {
            ESP_LOGI(TAG, "ACT_SELFTEST ON confirmed (target=%s elapsed=%" PRIu32 ")",
                     actuator_self_test_target_name(s_rt.target), s_rt.elapsed_ms);
        }
        s_rt.state = ACT_STATE_RUNNING;
    }

    if (cha.terminal != CHA_TERMINAL_NONE && cha.state != CHA_STATE_IDLE_OFF) {
        bool off_ok = cha.output_confirmed_off;
        switch (cha.terminal) {
        case CHA_TERMINAL_COMPLETE:
            set_terminal_locked(off_ok ? ACT_TERMINAL_COMPLETE
                                       : ACT_TERMINAL_FAULT,
                                off_ok ? "ACTUATOR_SELF_TEST_ACCEPTED"
                                       : "OFF_CONFIRM_FAILED");
            break;
        case CHA_TERMINAL_INTERRUPTED:
            set_terminal_locked(off_ok ? ACT_TERMINAL_INTERRUPTED
                                       : ACT_TERMINAL_FAULT,
                                off_ok ? "EMERGENCY_REQUESTED"
                                       : "OFF_CONFIRM_FAILED");
            break;
        case CHA_TERMINAL_TIMEOUT:
            set_terminal_locked(ACT_TERMINAL_TIMEOUT, "HOT_AIR_TIMEOUT");
            break;
        case CHA_TERMINAL_FAULT:
            set_terminal_locked(ACT_TERMINAL_FAULT,
                                off_ok ? "HOT_AIR_FAULT" : "OFF_CONFIRM_FAILED");
            break;
        default:
            set_terminal_locked(ACT_TERMINAL_UNKNOWN, "HOT_AIR_TERMINAL_UNKNOWN");
            break;
        }
    }

    if (atomic_load(&s_rt.test_active) &&
        s_rt.elapsed_ms > s_rt.requested_duration_ms + ACT_HARD_TIMEOUT_GRACE_MS) {
        (void)coupled_hot_air_service_emergency_stop();
        set_terminal_locked(ACT_TERMINAL_TIMEOUT, "ACT_TIMEOUT");
    }
}

static void advance_act_locked(void)
{
    if (s_rt.terminal != ACT_TERMINAL_NONE) return;

    /* 锁竞争 emergency：闩锁先置位，任务在拿到互斥量后首先执行关断。 */
    if (atomic_exchange_explicit(&s_rt.emergency_latch, false,
                                 memory_order_acq_rel)) {
        esp_err_t off_err = act_emergency_outputs();
        if (atomic_load(&s_rt.test_active)) {
            set_terminal_locked(
                off_err == ESP_OK ? ACT_TERMINAL_INTERRUPTED
                                  : ACT_TERMINAL_FAULT,
                off_err == ESP_OK ? "EMERGENCY_REQUESTED"
                                  : "EMERGENCY_OFF_FAILED");
        }
        return;
    }

    /* fault/emergency 优先：请求关断并置终态。 */
    safety_snapshot_t safety;
    if (safety_manager_get_snapshot(&safety) == ESP_OK) {
        if (safety.fault_active || safety.emergency_stop_active) {
            esp_err_t off_err = act_emergency_outputs();
            /* 紧急关断成功 → INTERRUPTED；OFF 失败 → FAULT（输出可能仍带电）。 */
            bool clean = (off_err == ESP_OK);
            set_terminal_locked(
                (safety.emergency_stop_active && clean)
                    ? ACT_TERMINAL_INTERRUPTED : ACT_TERMINAL_FAULT,
                clean ? (safety.emergency_stop_active
                             ? "EMERGENCY_ACTIVE" : "FAULT_ACTIVE")
                      : "EMERGENCY_OFF_FAILED");
            return;
        }
    }

    switch (s_rt.target) {
    case ACT_TARGET_DETERGENT_PUMP: advance_detergent_locked(); break;
    case ACT_TARGET_DRAIN_PUMP:     advance_drain_locked();     break;
    case ACT_TARGET_FAN:            advance_dry_locked();       break;
    case ACT_TARGET_HOT_AIR_COUPLED: advance_hot_air_locked();  break;
    default:                        advance_water_locked();     break;
    }
}

/* ---- 任务 ---- */

static void act_task(void *arg)
{
    (void)arg;
    while (!atomic_load(&s_rt.stop_requested)) {
        uint32_t notify = 0;
        TickType_t wait = atomic_load(&s_rt.test_active)
            ? pdMS_TO_TICKS(ACT_TICK_MS)
            : pdMS_TO_TICKS(250);
        xTaskNotifyWait(0, ACT_NOTIFY_RUN | ACT_NOTIFY_EMERGENCY, &notify, wait);

        if (notify & ACT_NOTIFY_EMERGENCY) {
            if (lock()) {
                if (atomic_load(&s_rt.test_active)) {
                    esp_err_t off_err = act_emergency_outputs();
                    set_terminal_locked(
                        off_err == ESP_OK ? ACT_TERMINAL_INTERRUPTED
                                          : ACT_TERMINAL_FAULT,
                        off_err == ESP_OK ? "EMERGENCY_REQUESTED"
                                          : "EMERGENCY_OFF_FAILED");
                }
                xSemaphoreGive(s_mtx);
            }
        }

        if (lock()) {
            if (atomic_load(&s_rt.test_active)) advance_act_locked();
            xSemaphoreGive(s_mtx);
        }
    }
    xSemaphoreGive(s_rt.task_done_sem);
    vTaskDelete(NULL);
}

/* ---- 生命周期 ---- */

esp_err_t actuator_self_test_init(const actuator_self_test_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    if (!s_mtx) {
        s_mtx = xSemaphoreCreateMutex();
        if (!s_mtx) return ESP_ERR_NO_MEM;
    }
    if (!lock()) return ESP_ERR_TIMEOUT;
    if (s_rt.initialized) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_rt, 0, sizeof(s_rt));
    s_rt.cfg = *config;
    s_rt.next_request_id = 1;
    s_rt.state = ACT_STATE_INACTIVE;
    s_rt.terminal = ACT_TERMINAL_NONE;
    s_rt.last_code = "ACTUATOR_SELF_TEST_ACCEPTED";
    s_rt.initialized = true;
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

esp_err_t actuator_self_test_start(void)
{
    if (!lock()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.initialized || s_rt.started) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    s_rt.started = true;
    /* 每次启动作废旧的 task_done_sem：避免旧任务残留信号被新实例消费。 */
    if (s_rt.task_done_sem) vSemaphoreDelete(s_rt.task_done_sem);
    s_rt.task_done_sem = xSemaphoreCreateBinary();
    if (!s_rt.task_done_sem) {
        s_rt.started = false;
        xSemaphoreGive(s_mtx);
        return ESP_ERR_NO_MEM;
    }
    atomic_store(&s_rt.stop_requested, false);
    if (xTaskCreatePinnedToCore(act_task, "act_selftest", ACT_TASK_STACK, NULL,
                                ACT_TASK_PRIORITY, &s_rt.task, ACT_TASK_CORE) != pdPASS) {
        s_rt.started = false;
        xSemaphoreGive(s_mtx);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

/* 停止前确保所有板测输出 OFF 并从服务快照确认。失败保留生命周期可重试。 */
#define ACT_STOP_OFF_TIMEOUT_MS 2500U

/* Fail-closed OFF confirmation: any snapshot failure or UNKNOWN output means
 * "cannot prove OFF", which stop() treats as not-yet-closed.  A missing or
 * timed-out snapshot must NEVER be read as "everything is off".  The pure
 * proof is delegated to actuator_outputs_confirmed_off() (host-tested). */
static bool act_outputs_confirmed_off(void)
{
    water_snapshot_t ws;
    bool water_ok = water_service_get_snapshot(&ws) == ESP_OK;
    detergent_snapshot_t ds;
    bool det_ok = detergent_service_get_snapshot(&ds) == ESP_OK;
    drain_snapshot_t drain;
    bool drain_ok = drain_service_get_snapshot(&drain) == ESP_OK;
    dry_snapshot_t dry;
    bool dry_ok = dry_service_get_snapshot(&dry) == ESP_OK;
    coupled_hot_air_snapshot_t cha;
    bool cha_ok = coupled_hot_air_service_get_snapshot(&cha) == ESP_OK;
    actuator_off_proof_t p;
    memset(&p, 0, sizeof(p));
    p.water_snapshot_ok = water_ok;
    p.source_off = water_ok && !ws.source_valve_on;
    p.source_unknown = water_ok && ws.source_valve_unknown;
    p.transfer_off = water_ok && !ws.transfer_valve_on;
    p.transfer_unknown = water_ok && ws.transfer_valve_unknown;
    p.detergent_snapshot_ok = det_ok;
    p.detergent_off = det_ok && ds.output_state == ACT_OUT_OFF;
    p.drain_snapshot_ok = drain_ok;
    p.drain_off = drain_ok && drain.output_state == ACT_OUT_OFF;
    p.dry_snapshot_ok = dry_ok;
    p.fan_off = dry_ok && dry.fan_output_state == ACT_OUT_OFF;
    p.heater_off = dry_ok && dry.heater_output_state == ACT_OUT_OFF;
    p.hot_air_snapshot_ok = cha_ok;
    p.hot_air_off = cha_ok && cha.output_known && cha.output_confirmed_off;
    return actuator_outputs_confirmed_off(&p);
}

esp_err_t actuator_self_test_stop(void)
{
    if (!s_mtx) return ESP_ERR_INVALID_STATE;
    if (!lock()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.initialized) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    /* 先锁存 stop_requested 与 started=false：停止窗口内并发 request()
     * 必须被拒绝，避免 OFF 确认期间重新上电。 */
    atomic_store(&s_rt.stop_requested, true);
    s_rt.started = false;
    TaskHandle_t task = s_rt.task;
    /* 上次 stop() 超时后任务可能已自行退出（stop_requested 已锁存）。
     * 非阻塞 take task_done_sem：成功即证明任务已给出并即将自删，清空陈旧
     * 句柄，避免对已释放 TCB 再发通知（UAF）。任务给 sem 先于自删，故
     * take 失败 ⇒ 任务仍存活，通知安全。 */
    if (task && s_rt.task_done_sem &&
        xSemaphoreTake(s_rt.task_done_sem, 0) == pdTRUE) {
        s_rt.task = NULL;
        task = NULL;
    }
    if (task) xTaskNotify(task, ACT_NOTIFY_EMERGENCY, eSetBits);
    xSemaphoreGive(s_mtx);

    if (!act_outputs_confirmed_off()) {
        act_emergency_outputs();
        TickType_t deadline =
            xTaskGetTickCount() + pdMS_TO_TICKS(ACT_STOP_OFF_TIMEOUT_MS);
        bool confirmed = false;
        while (xTaskGetTickCount() < deadline) {
            if (act_outputs_confirmed_off()) { confirmed = true; break; }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (!confirmed) {
            ESP_LOGE(TAG, "stop: outputs not confirmed OFF; preserving lifecycle");
            return ESP_ERR_TIMEOUT;
        }
    }

    /* 在互斥量之外 join：任务自身需要该互斥量才能退出。 */
    if (task) {
        if (s_rt.task_done_sem &&
            xSemaphoreTake(s_rt.task_done_sem, pdMS_TO_TICKS(2000)) != pdTRUE) {
            ESP_LOGE(TAG, "stop: task did not exit; preserve lifecycle for retry");
            return ESP_ERR_TIMEOUT;
        }
    }
    if (!lock()) return ESP_ERR_INVALID_STATE;
    /* 成功 join 后释放 sem 并置 NULL：init() 的 memset 不得丢失仍拥有的
     * 句柄，泄漏的 sem 会累积 FreeRTOS 内核对象。 */
    if (s_rt.task_done_sem) {
        vSemaphoreDelete(s_rt.task_done_sem);
        s_rt.task_done_sem = NULL;
    }
    s_rt.task = NULL;
    s_rt.started = false;
    s_rt.initialized = false;
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

/* ---- 受理 ---- */

esp_err_t actuator_self_test_request(actuator_self_test_target_t target,
                                     const char **out_code,
                                     uint32_t *out_duration_ms)
{
    if (out_code) *out_code = NULL;
    if (out_duration_ms) *out_duration_ms = 0;
    if (!lock()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.initialized || !s_rt.started) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t duration = actuator_self_test_duration_ms(target);
    if (duration == 0U) {
        s_rt.state = ACT_STATE_REJECTED;
        s_rt.last_code = "UNKNOWN_TARGET";
        xSemaphoreGive(s_mtx);
        if (out_code) *out_code = "UNKNOWN_TARGET";
        return ESP_OK;
    }
    /* 执行器自检时长可由 machine_config.benchtst.act_duration_ms 覆盖
     * （滑杆/NVS 可调，默认 1500ms）。硬上限：热风模块 ≤3000ms
     * （耦合单继电器安全上限）、水阀 ≤10000ms、其余 ≤服务自身 max。 */
    uint32_t override = 0U;
    const machine_config_t *mcfg = machine_config_get();
    if (mcfg) override = mcfg->benchtst.act_duration_ms;
    if (override > 0U) {
        if (target == ACT_TARGET_HOT_AIR_COUPLED &&
            override > ACT_SELF_TEST_HOT_AIR_MAX_DURATION_MS) {
            override = ACT_SELF_TEST_HOT_AIR_MAX_DURATION_MS;
        }
        if (override >= 200U && override <= 10000U) {
            duration = override;
        }
    }

    actuator_self_test_gate_input_t in;
    collect_gate_input(&in, target);
    actuator_self_test_gate_result_t gate = actuator_self_test_gate_check(&in);
    if (gate != ACT_GATE_OK) {
        const char *code = actuator_self_test_gate_code(gate);
        ESP_LOGI(TAG,
                 "ACT_SELFTEST reject code=%s target=%s pos=%d valid=%d fresh=%d "
                 "stable=%d moving=%d fault=%d emerg=%d mcp_known=%d "
                 "exec_idle=%d water_idle=%d det_idle=%d safe_off=%d "
                 "tgt_off=%d conflict_off=%d",
                 code, actuator_self_test_target_name(target), (int)in.pos,
                 in.pos_valid ? 1 : 0, in.pos_fresh ? 1 : 0,
                 in.pos_stable ? 1 : 0, in.pos_moving ? 1 : 0,
                 in.fault_active ? 1 : 0, in.emergency_active ? 1 : 0,
                 in.mcp_target_known ? 1 : 0, in.exec_idle ? 1 : 0,
                 in.water_service_idle ? 1 : 0, in.detergent_service_idle ? 1 : 0,
                 in.water_safe_off_synced ? 1 : 0,
                 in.target_confirmed_off ? 1 : 0, in.conflict_confirmed_off ? 1 : 0);
        s_rt.state = ACT_STATE_REJECTED;
        s_rt.last_code = code;
        s_rt.requested_duration_ms = 0;
        s_rt.elapsed_ms = 0;
        xSemaphoreGive(s_mtx);
        if (out_code) *out_code = code;
        return ESP_OK;
    }

    /* 受理：单调 request_id（跳过 0），异步提交固定时长。 */
    machine_request_id_t rid =
        actuator_self_test_next_request_id(s_rt.next_request_id);
    s_rt.next_request_id = rid;

    esp_err_t err = ESP_OK;
    switch (target) {
    case ACT_TARGET_DETERGENT_PUMP: {
        detergent_request_t req = { .request_id = rid, .duration_ms = duration };
        err = detergent_service_run_async(&req);
        break;
    }
    case ACT_TARGET_DRAIN_PUMP: {
        drain_request_t req = { .request_id = rid, .duration_ms = duration };
        err = drain_service_run_async(&req);
        break;
    }
    case ACT_TARGET_FAN: {
        /* 仅风扇测试：heater_requested=false，加热器全程保持 OFF。 */
        dry_request_t req = { .request_id = rid, .duration_ms = duration,
                              .heater_requested = false };
        err = dry_service_run_async(&req);
        break;
    }
    case ACT_TARGET_HOT_AIR_COUPLED: {
        /* 耦合热风模块：单继电器同步启停（风扇+加热丝并联）。 */
        coupled_hot_air_request_t req = { .request_id = rid,
                                          .duration_ms = duration };
        err = coupled_hot_air_service_run_async(&req);
        break;
    }
    default: {
        water_actuator_valve_t valve =
            (target == ACT_TARGET_SOURCE_VALVE)
                ? WATER_ACTUATOR_VALVE_SOURCE : WATER_ACTUATOR_VALVE_TRANSFER;
        err = water_service_actuator_valve_test(valve, rid, duration);
        break;
    }
    }
    ESP_LOGI(TAG, "ACT_SELFTEST submit target=%s rid=%" PRIu32
             " duration=%" PRIu32 " result=0x%x",
             actuator_self_test_target_name(target), rid, duration,
             (unsigned)err);
    if (err != ESP_OK) {
        s_rt.state = ACT_STATE_FAULT;
        s_rt.last_code = "ACT_SUBMIT_FAILED";
        s_rt.requested_duration_ms = 0;
        s_rt.elapsed_ms = 0;
        xSemaphoreGive(s_mtx);
        if (out_code) *out_code = "ACT_SUBMIT_FAILED";
        return ESP_OK;
    }

    s_rt.state = ACT_STATE_ACCEPTED;
    s_rt.last_code = "ACTUATOR_SELF_TEST_ACCEPTED";
    s_rt.target = target;
    s_rt.request_id = rid;
    s_rt.requested_duration_ms = duration;
    s_rt.elapsed_ms = 0;
    s_rt.terminal = ACT_TERMINAL_NONE;
    s_rt.output_confirmed_off = false;
    atomic_store(&s_rt.test_active, true);
    atomic_store(&s_rt.emergency_latch, false);   /* 新一次板测不带旧闩锁 */
    s_rt.start_tick_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (s_rt.task) xTaskNotify(s_rt.task, ACT_NOTIFY_RUN, eSetBits);
    xSemaphoreGive(s_mtx);

    if (out_code) *out_code = "ACTUATOR_SELF_TEST_ACCEPTED";
    if (out_duration_ms) *out_duration_ms = duration;
    return ESP_OK;
}

esp_err_t actuator_self_test_emergency(void)
{
    if (!s_mtx) return ESP_ERR_INVALID_STATE;

    /* 先置位闩锁（与互斥量无关）：任务在锁内恢复后首先消费并关断，
     * 保证锁竞争时 emergency 不会静默丢失。 */
    atomic_store_explicit(&s_rt.emergency_latch, true, memory_order_release);

    /* 不依赖 actuator 互斥量的后备关断：直接请求所有输出 OFF。这些调用
     * 各自持有 service 内部锁并经 safety_manager 下电，无需 s_mtx。 */
    esp_err_t off_err = act_emergency_outputs();
    esp_err_t r = off_err;

    if (lock()) {
        if (s_rt.initialized && atomic_load(&s_rt.test_active)) {
            set_terminal_locked(
                off_err == ESP_OK ? ACT_TERMINAL_INTERRUPTED
                                  : ACT_TERMINAL_FAULT,
                off_err == ESP_OK ? "EMERGENCY_REQUESTED"
                                  : "EMERGENCY_OFF_FAILED");
        }
        TaskHandle_t t = s_rt.task;
        xSemaphoreGive(s_mtx);
        if (t) xTaskNotify(t, ACT_NOTIFY_EMERGENCY, eSetBits);
    } else {
        /* 互斥量被占用：闩锁 + 无锁 OFF 已覆盖关断，但状态机未在锁内更新。
         * 不得返回 ESP_OK（避免调用方误以为状态已落定）；任务稍后会消费闩锁。 */
        if (r == ESP_OK) r = ESP_ERR_TIMEOUT;
    }
    return r;
}

esp_err_t actuator_self_test_get_snapshot(actuator_self_test_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!lock()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.initialized) {
        memset(out, 0, sizeof(*out));
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    out->state = s_rt.state;
    out->target = s_rt.target;
    out->request_id = s_rt.request_id;
    out->requested_duration_ms = s_rt.requested_duration_ms;
    out->elapsed_ms = s_rt.elapsed_ms;
    out->terminal = s_rt.terminal;
    out->last_code = s_rt.last_code ? s_rt.last_code : "";
    out->output_confirmed_off = s_rt.output_confirmed_off;
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

uint8_t actuator_self_test_state_code(void)
{
    actuator_self_test_snapshot_t snap;
    if (actuator_self_test_get_snapshot(&snap) != ESP_OK)
        return (uint8_t)ACT_STATE_INACTIVE;
    return (uint8_t)snap.state;
}
