#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "machine_types.h"
#include "wash_contract.h"
#include "xiaojing_hal.h"
#include "machine_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * bl50_service.h — BL50 内筒业务服务
 * 管理 BL50 电机的洗涤动作：HOME/PULSATOR/DRUM/SPIN
 * 通过 safety_manager_apply 间接控制 HAL，不直接访问 GPIO/LEDC
 *
 * Lifecycle: init → start → [running] → stop → init → ...
 * stop() 幂等。start/stop 串行化。
 * 同一时刻只接受一个 request。
 * ================================================================ */

/* ---- BL50 Action ---- */

typedef enum {
    BL50_ACTION_HOME = 0,
    BL50_ACTION_PULSATOR,
    BL50_ACTION_DRUM,
    BL50_ACTION_SPIN,
} bl50_action_t;

/* ---- 生产契约：动作→所需桶位 ----
 * HOME→45° / PULSATOR→0° / DRUM→90° / SPIN→180°（与 bl50_fsm 同源）。
 * 对外公开：motor_bench 等诊断组件据此派生位置语义，禁止硬编码猜测。 */
drum_position_t bl50_service_action_to_position(bl50_action_t action);

/* ---- BL50 Request ---- */

typedef struct {
    machine_request_id_t request_id;
    bl50_action_t action;
    wash_intensity_t intensity;
    uint32_t duration_ms;
    uint8_t target_pwm_percent;
    uint32_t direction_interval_ms;
} bl50_request_t;

/* ---- BL50 Service State (只读快照用) ---- */

typedef enum {
    BL50_SVC_STATE_IDLE = 0,
    BL50_SVC_STATE_VALIDATING,
    BL50_SVC_STATE_HOMING,
    BL50_SVC_STATE_RAMPING_UP,
    BL50_SVC_STATE_RUNNING_CW,
    BL50_SVC_STATE_RAMPING_DOWN,
    BL50_SVC_STATE_REVERSAL_WAIT,
    BL50_SVC_STATE_RUNNING_CCW,
    BL50_SVC_STATE_BRAKING,
    BL50_SVC_STATE_STOPPING,
    BL50_SVC_STATE_CANCELING,
    BL50_SVC_STATE_COMPLETE,
    BL50_SVC_STATE_FAULT,
} bl50_service_state_t;

/* ---- BL50 Snapshot ---- */

typedef struct {
    bl50_service_state_t state;
    bl50_action_t active_action;
    machine_request_id_t active_request_id;
    uint8_t current_pwm_percent;
    bool running_cw;
    machine_fault_t fault;
} bl50_snapshot_t;

/* ---- Configuration ---- */

typedef struct {
    const xiaojing_hal_t *hal;
    const machine_config_t *config;
    machine_event_sink_t event_sink;
} bl50_service_config_t;

/* ---- Lifecycle ---- */

esp_err_t bl50_service_init(const bl50_service_config_t *config);
esp_err_t bl50_service_start(void);
esp_err_t bl50_service_stop(void);

/* ---- Request ---- */

/* 异步运行一个 BL50 动作。busy 时返回 ESP_ERR_INVALID_STATE。 */
esp_err_t bl50_service_run_async(const bl50_request_t *request);

/* 取消当前请求。request_id 不匹配时忽略。 */
esp_err_t bl50_service_cancel(machine_request_id_t request_id);

/* 急停。从任何状态进入 STOP 路径。 */
esp_err_t bl50_service_emergency_stop(void);

/* ---- Snapshot ---- */

esp_err_t bl50_service_get_snapshot(bl50_snapshot_t *out);

/* ---- Test Helpers ---- */

#ifdef CONFIG_BL50_SERVICE_TEST_HOOKS

esp_err_t bl50_service_test_reset(void);
void bl50_service_test_set_home_sensor(bool triggered);
void bl50_service_test_inject_mcp_error(esp_err_t error);
bool bl50_service_test_get_running(void);

/* 测试用: 暴露内部 GPB5 解码和读取 (生产路径) */
bool bl50_home_decode_gpb5(uint8_t gpio_b);
esp_err_t bl50_service_test_read_home_raw(bool *triggered);
esp_err_t bl50_service_test_read_home_debounced(bool *triggered);

/* 测试用: 确定性 timeout 测试的 exit barrier */
void bl50_service_test_set_exit_barrier(bool enabled);
void bl50_service_test_set_stop_halt(bool enabled);

/* 测试用: 仅设置内部 HAL 指针用于防抖测试，不启动 service task */
void bl50_service_test_set_hal(const xiaojing_hal_t *hal);

#endif /* BL50_SERVICE_TEST_HOOKS */

#ifdef __cplusplus
}
#endif
