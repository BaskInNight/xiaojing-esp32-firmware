#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "machine_types.h"
#include "machine_config.h"
#include "xiaojing_hal.h"
#include "safety_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * water_service.h — 两级分批进水服务
 * 中间水箱进水 → 洗涤桶转移 → 水位检测循环
 *
 * 所有阀门操作通过 safety_manager_apply()。
 * water_service 不直接调用 hal->set_safe_output()。
 *
 * Lifecycle: init → start → [running] → stop
 * ================================================================ */

/* ---- Water Service State ---- */

typedef enum {
    WATER_STATE_UNINITIALIZED = 0,
    WATER_STATE_IDLE,
    WATER_STATE_VALIDATING,
    WATER_STATE_RESETTING_FLOW,
    WATER_STATE_OPENING_SOURCE,         /* 等待 OPEN_SOURCE 结果 */
    WATER_STATE_SOURCE_FILL,
    WATER_STATE_CLOSING_SOURCE,         /* 等待 CLOSE_SOURCE 结果 */
    WATER_STATE_SOURCE_SETTLE,
    WATER_STATE_OPENING_TRANSFER,       /* 等待 OPEN_TRANSFER 结果 */
    WATER_STATE_TRANSFER,
    WATER_STATE_CLOSING_TRANSFER,       /* 等待 CLOSE_TRANSFER 结果 */
    WATER_STATE_CHECK_DRUM_LEVEL,
    WATER_STATE_CLOSING_ALL,            /* 等待 CLOSE_ALL 结果 (cancel/emergency/stop) */
    WATER_STATE_COMPLETE,
    WATER_STATE_FAULT,
} water_state_t;

/* ---- Water Service Config ---- */

typedef struct {
    float    pulses_per_liter;             /* 0=未标定, >=0, isfinite */
    uint32_t no_flow_timeout_ms;           /* 开阀后无脉冲超时 */
    uint32_t low_flow_window_ms;           /* 低流检测窗口 */
    uint32_t low_flow_min_pulses;          /* 窗口内最小脉冲, 0=禁用低流检测 */
    uint32_t source_batch_target_pulses;   /* 单批目标脉冲 */
    uint32_t source_batch_max_ms;          /* 单批最大时间 */
    uint32_t source_settle_ms;             /* 源箱沉降时间 */
    uint32_t transfer_timeout_ms;          /* 转移硬超时 */
    uint32_t default_transfer_ms;          /* 默认转移时间 */
    uint16_t max_fill_cycles;              /* 最大批次 */
    uint32_t total_inlet_timeout_ms;       /* 进水总超时 */
} water_service_config_t;

/* ---- Water Fill Request ---- */

typedef struct {
    machine_request_id_t request_id;
    uint32_t target_volume_ml;             /* 仅用于日志, 不替代水位满判断 */
    uint32_t source_batch_max_ms;          /* 0=使用配置默认 */
    bool     complete_on_drum_full;        /* 生产模式必须为 true */
} water_in_request_t;

/* ---- Water Snapshot (线程安全快照) ---- */

typedef struct {
    water_state_t        state;
    machine_request_id_t request_id;
    bool                 source_valve_on;
    bool                 transfer_valve_on;
    bool                 source_valve_unknown;   /* valve state UNKNOWN (not safe OFF) */
    bool                 transfer_valve_unknown; /* valve state UNKNOWN (not safe OFF) */
    /* 初始安全 OFF 同步（HW-FIX-1）：water 任务启动时经 safety_manager 显式
     * 请求双阀 OFF 后置位。false = 同步未完成（门禁 OUTPUT_STATE_UNKNOWN）；
     * true = 同步已尝试到终态（每阀成败由 source/transfer_valve_unknown 表达）。 */
    bool                 safe_off_synced;
    bool                 drum_water_full;
    bool                 water_level_valid;
    uint16_t             completed_cycles;
    uint32_t             current_batch_pulses;
    uint32_t             total_flow_pulses;
    /* Live PCNT cumulative count.  Unlike total_flow_pulses this remains
     * meaningful while the water FSM is idle, so it is safe for telemetry. */
    uint32_t             raw_flow_pulses;
    uint32_t             estimated_ml;       /* 0 if uncalibrated */
    uint32_t             elapsed_ms;
    /* 受控单阀板测（water_service_actuator_valve_test）。由 water 任务
     * 单写者维护，设备端自动计时关阀。act_test_state 见
     * water_act_test_state_t。 */
    bool                 act_test_active;
    uint8_t              act_test_state;     /* water_act_test_state_t */
    uint8_t              act_test_valve;     /* water_actuator_valve_t */
    machine_request_id_t act_test_request_id;
    bool                 terminal_event_emitted;
    bool                 terminal_pending;   /* pending terminal not yet published */
    machine_fault_t      fault;
    uint32_t             revision;
} water_snapshot_t;

/* ---- 受控单阀板测入口 (开发/板级诊断) ----
 *
 * 不经过正式进水 FSM 序列，但所有写仍经 safety_manager_apply()（与
 * water_service 正式路径相同的网关）。设备端（water 任务）自身计时，
 * 到达固定时长后自动 OFF——即使 BLE 断开/小程序崩溃/手机关机也自动关闭。
 * 受理状态经 water_snapshot_t 的 act_test_* / source_valve_on /
 * transfer_valve_on 查询。 */

typedef enum {
    WATER_ACTUATOR_VALVE_SOURCE = 0,   /* GPA0 有压进水阀 */
    WATER_ACTUATOR_VALVE_TRANSFER,     /* GPA1 零压转移阀 */
} water_actuator_valve_t;

typedef enum {
    WATER_ACT_TEST_IDLE = 0,     /* 无板测 */
    WATER_ACT_TEST_RUNNING,      /* 阀已开，等待时长到期自动关 */
    WATER_ACT_TEST_COMPLETE,     /* 已关且确认 OFF */
    WATER_ACT_TEST_FAULT,        /* 开/关失败或 OFF 未确认 */
} water_act_test_state_t;

/* 受理一条受控单阀板测：开启目标阀门，设备端计时 duration_ms 后自动关闭
 * 并确认 OFF。返回 ESP_OK 表示已入队；实际受理结果经快照查询。 */
esp_err_t water_service_actuator_valve_test(
    water_actuator_valve_t valve, machine_request_id_t request_id,
    uint32_t duration_ms);

/* 立即关闭指定板测阀（BLE 断线 / 急停）。非阻塞，best-effort。 */
esp_err_t water_service_actuator_valve_close(water_actuator_valve_t valve);

/* ---- Lifecycle ---- */

/* Global init: creates lifecycle mutexes. Call once from app_bootstrap. */
esp_err_t water_service_global_init(void);

/* Init: allocates FreeRTOS resources, validates config/hal. */
esp_err_t water_service_init(const water_service_config_t *config,
                              const xiaojing_hal_t *hal,
                              machine_event_sink_t event_sink);

/* Start: creates task, waits for ready. */
esp_err_t water_service_start(void);

/* Stop: signals task exit, waits, frees resources. Idempotent. */
esp_err_t water_service_stop(void);

/* ---- Commands ---- */

/* Submit fill request. Returns immediately. */
esp_err_t water_service_fill_async(const water_in_request_t *request);

/* Cancel active request by request_id. Non-blocking. */
esp_err_t water_service_cancel(machine_request_id_t request_id);

/* Emergency close: best-effort valve shutdown, non-blocking. */
esp_err_t water_service_emergency_close(void);

/* ---- Query ---- */

/* Thread-safe snapshot. */
esp_err_t water_service_get_snapshot(water_snapshot_t *out);

/* ---- Test hooks ---- */

/* Hook point enum is always defined (used internally) */
typedef enum {
    WATER_HOOK_PRE_QSEND = 0,
    WATER_HOOK_PRE_NOTIFY,
    WATER_HOOK_PRE_VALVE_APPLY,
    WATER_HOOK_COUNT,
} water_hook_point_t;

#ifdef WATER_SERVICE_TEST_HOOKS

typedef void (*water_hook_fn)(water_hook_point_t where, void *user_data);

void water_service_set_hook(water_hook_point_t where,
                             water_hook_fn fn, void *user_data);
void *water_service_get_barrier(water_hook_point_t where);

/* Test bridge: access private state for stop-retry verification */
esp_err_t water_service_test_fail_next_hook_unregister(esp_err_t error);
bool water_service_test_is_task_handle_null(void);
bool water_service_test_is_stop_hook_registered(void);
uint32_t water_service_test_get_notify_attempt_count(void);
bool water_service_test_resources_alive(void);

#endif /* WATER_SERVICE_TEST_HOOKS */

#ifdef __cplusplus
}
#endif
