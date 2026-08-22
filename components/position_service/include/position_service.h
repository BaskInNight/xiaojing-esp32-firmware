#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "machine_types.h"
#include "xiaojing_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * position_service.h — 姿态定位服务
 * 五路霍尔 + IBT-2 电机，360° 连续旋转定位
 *
 * Lifecycle: init → start → [running] → stop → init → ...
 * - init:   校验配置和 HAL 回调，创建 mutex/queue/semaphore
 * - start:  创建 task，等待初始化完成
 * - stop:   通知 task 退出，等待完成，释放全部资源
 *
 * stop() 幂等：UNINITIALIZED 返回 ESP_OK。
 * stop() INITIALIZED(未 start)也释放资源并回到 UNINITIALIZED。
 * stop() 超时返回 ESP_ERR_TIMEOUT，资源保持不变（可重试）。
 *
 * Thread safety:
 * - init/start/stop 串行化（lifecycle_mutex）。
 * - move/cancel/emergency/snapshot 可从不同任务调用。
 * - STOPPING 生命周期后 move/cancel/emergency 返回 ESP_ERR_INVALID_STATE。
 * - get_snapshot 在 STOPPING 期间返回最后一致快照。
 * - event sink 回调在所有锁外执行，可安全调用 get_snapshot。
 * ================================================================ */

typedef enum {
    POSITION_STATE_UNINITIALIZED = 0,
    POSITION_STATE_IDLE_UNKNOWN,
    POSITION_STATE_IDLE_KNOWN,
    POSITION_STATE_MOVING,
    POSITION_STATE_BRAKING,
    POSITION_STATE_STOPPING,
    POSITION_STATE_FAULT,
} position_state_t;

typedef struct {
    bool direction_calibrated;
    bool rpwm_is_cw;
    position_direction_policy_t default_policy;
    uint8_t move_pwm_percent;
    uint8_t approach_pwm_percent;  /* V1 reserved: 五霍尔离散系统无法定义接近条件 */
    uint32_t debounce_ms;
    uint32_t move_timeout_ms;
    uint32_t brake_ms;
} position_service_config_t;

typedef struct {
    machine_request_id_t request_id;
    drum_position_t target;
    position_direction_policy_t direction_policy;
    uint8_t pwm_percent;   /* 0 = 使用配置默认 move_pwm_percent */
    uint32_t timeout_ms;   /* 0 = 使用配置默认 move_timeout_ms */
} position_move_request_t;

typedef struct {
    position_state_t state;
    drum_position_t current;
    drum_position_t target;
    uint8_t raw_hall_mask;
    uint8_t stable_hall_mask;
    /* MCP23017 GPB5, active-low.  This is the BLDC drum home/alignment
     * sensor, deliberately separate from the five discrete tilt positions. */
    bool bucket_aligned;
    bool moving_cw;
    uint8_t pwm_percent;
    machine_request_id_t active_request_id;
    machine_fault_t fault;
} position_snapshot_t;

/* ---- Global init (call once from app_bootstrap before any other API) ---- */

/* Creates lifecycle/stop mutexes. Exactly-once, concurrent-safe.
 * Must be called from app_bootstrap before init/start/stop.
 * If not called, stop() returns ESP_OK; other APIs return INVALID_STATE. */
esp_err_t position_service_global_init(void);

/* ---- Lifecycle (串行化，不可与另一个 lifecycle 操作并发) ---- */

esp_err_t position_service_init(const position_service_config_t *config,
                                const xiaojing_hal_t *hal,
                                machine_event_sink_t event_sink);

esp_err_t position_service_start(void);

/* 有界等待 task 退出后释放全部资源。超时返回 ESP_ERR_TIMEOUT。 */
esp_err_t position_service_stop(void);

/* ---- Commands (线程安全，STOPPING 后返回 INVALID_STATE) ---- */

/* POSITION_DIR_USE_DEFAULT: 使用 config.default_policy */
esp_err_t position_service_move_async(const position_move_request_t *request);

esp_err_t position_service_cancel(machine_request_id_t request_id);

/* 通过 task notification 绕过队列，非阻塞、幂等 */
esp_err_t position_service_emergency_stop(void);

/* ---- Query (线程安全，event sink 可重入调用) ---- */

esp_err_t position_service_get_snapshot(position_snapshot_t *out);

/* ---- Test hook points (always defined for pos_invoke_hook calls) ---- */

typedef enum {
    POS_HOOK_PRE_LC_LOCK = 0,
    POS_HOOK_POST_LC_LOCK,
    POS_HOOK_PRE_SM_LOCK,
    POS_HOOK_POST_SM_LOCK,
    POS_HOOK_PRE_QSEND,
    POS_HOOK_PRE_NOTIFY,
    POS_HOOK_PRE_CLEANUP,
    POS_HOOK_PRE_EXIT_WRITE,
    POS_HOOK_COUNT,
} pos_hook_point_t;

#ifdef POSITION_SERVICE_TEST_HOOKS

typedef void (*pos_hook_fn)(pos_hook_point_t where, void *user_data);

void pos_service_set_hook(pos_hook_point_t where,
                          pos_hook_fn fn, void *user_data);
/* Returns SemaphoreHandle_t — caller must include freertos/semphr.h */
void *pos_service_get_barrier(pos_hook_point_t where);
#endif /* POSITION_SERVICE_TEST_HOOKS */

#ifdef __cplusplus
}
#endif
