# 核心组件 C 接口契约

状态：V1 实现基线。Claude Code 可以在 Phase 实现前提出小幅修改，但必须说明原因、保持需求不变，并同步更新本文件和测试。

## 1. 公共约定

- 公共 API 使用 C linkage。
- 时间字段统一 `_ms`。
- 异步操作以 `machine_request_id_t` 关联命令和终态事件。
- `*_start_async()` 返回只表示“请求已接收/拒绝”，不表示动作完成。
- `cancel/emergency` 幂等。
- 输入结构由组件在函数返回前复制。
- 公共头文件不暴露内部 Task/Queue/Semaphore 句柄。

## 2. `machine_types.h`

```c
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t machine_request_id_t;

typedef enum {
    APP_SOURCE_BUTTON = 0,
    APP_SOURCE_BLE,
    APP_SOURCE_VOICE,
    APP_SOURCE_WEB,
    APP_SOURCE_SYSTEM,
} app_source_t;

typedef enum {
    DRUM_POS_UNKNOWN = -1,
    DRUM_POS_0 = 0,
    DRUM_POS_45 = 45,
    DRUM_POS_90 = 90,
    DRUM_POS_180 = 180,
    DRUM_POS_270 = 270,
} drum_position_t;

typedef enum {
    MACHINE_STATE_BOOTING = 0,
    MACHINE_STATE_IDLE,
    MACHINE_STATE_WAITING_LOAD,
    MACHINE_STATE_RUNNING,
    MACHINE_STATE_WAITING_UNLOAD,
    MACHINE_STATE_UV,
    MACHINE_STATE_PAUSED,
    MACHINE_STATE_RESETTING,
    MACHINE_STATE_FAULT,
} machine_state_t;

typedef enum {
    WASH_PHASE_IDLE = 0,
    WASH_PHASE_MOVE_POSITION,
    WASH_PHASE_HOME_BL50,
    WASH_PHASE_WATER_SOURCE_FILL,
    WASH_PHASE_WATER_TRANSFER,
    WASH_PHASE_DETERGENT,
    WASH_PHASE_PULSATOR_WASH,
    WASH_PHASE_DRUM_WASH,
    WASH_PHASE_DRAIN,
    WASH_PHASE_SPIN,
    WASH_PHASE_DRY,
    WASH_PHASE_UV,
    WASH_PHASE_DONE,
} wash_phase_t;

typedef enum {
    FAULT_NONE = 0,
    FAULT_HALL_CONFLICT,
    FAULT_POSITION_TIMEOUT,
    FAULT_POSITION_DRIVER,
    FAULT_WATER_NO_FLOW,
    FAULT_WATER_LOW_FLOW,
    FAULT_WATER_TIMEOUT,
    FAULT_WATER_LEVEL_INVALID,
    FAULT_SHT_INVALID,
    FAULT_DRY_OVERTEMP,
    FAULT_HEATER_INTERLOCK,
    FAULT_MCP_IO,
    FAULT_I2C_BUS,
    FAULT_BL50,
    FAULT_PROGRAM_TIMEOUT,
    FAULT_INTERNAL,
} machine_fault_code_t;

typedef enum {
    FAULT_SEVERITY_WARNING = 0,
    FAULT_SEVERITY_RECOVERABLE,
    FAULT_SEVERITY_LATCHED,
} fault_severity_t;

typedef struct {
    machine_fault_code_t code;
    fault_severity_t severity;
    uint32_t detail;
    int64_t timestamp_ms;
} machine_fault_t;

typedef enum {
    SERVICE_RESULT_OK = 0,
    SERVICE_RESULT_CANCELED,
    SERVICE_RESULT_TIMEOUT,
    SERVICE_RESULT_REJECTED,
    SERVICE_RESULT_FAULT,
} service_result_t;

typedef enum {
    MACHINE_EVENT_POSITION_DONE = 0,
    MACHINE_EVENT_WATER_DONE,
    MACHINE_EVENT_DRY_DONE,
    MACHINE_EVENT_BL50_DONE,
    MACHINE_EVENT_DRAIN_DONE,
    MACHINE_EVENT_DETERGENT_DONE,
    MACHINE_EVENT_UV_DONE,
    MACHINE_EVENT_STEP_PROGRESS,
    MACHINE_EVENT_FAULT,
    MACHINE_EVENT_STATUS_CHANGED,
} machine_event_type_t;

typedef struct {
    machine_event_type_t type;
    machine_request_id_t request_id;
    service_result_t result;
    machine_fault_t fault;
    uint32_t value;
} machine_event_t;

typedef esp_err_t (*machine_event_publish_fn)(const machine_event_t *event,
                                              uint32_t timeout_ms,
                                              void *context);

typedef struct {
    machine_event_publish_fn publish;
    void *context;
} machine_event_sink_t;

#ifdef __cplusplus
}
#endif
```

`machine_event_sink_t` 由 bootstrap 适配到 executor 的 `machine_event_queue`。服务测试可以传入同步捕获函数。

## 3. 意图、计划与状态类型

建议放在 `machine_types/wash_contract.h`，避免 voice/app_protocol 依赖 planner 实现组件。

```c
#define WASH_INTENT_MAX_ACTIONS 32
#define WASH_PROGRAM_MAX_STEPS  64

typedef enum {
    WASH_ACTION_MOVE_POSITION = 0,
    WASH_ACTION_WATER_IN,
    WASH_ACTION_DETERGENT,
    WASH_ACTION_PULSATOR_WASH,
    WASH_ACTION_DRUM_WASH,
    WASH_ACTION_DRAIN,
    WASH_ACTION_SPIN,
    WASH_ACTION_DRY,
    WASH_ACTION_UV,
} wash_action_type_t;

typedef enum {
    WASH_INTENSITY_GENTLE = 0,
    WASH_INTENSITY_LOW,
    WASH_INTENSITY_NORMAL,
    WASH_INTENSITY_STRONG,
} wash_intensity_t;

typedef struct {
    wash_action_type_t type;
    drum_position_t position;
    uint32_t duration_ms;
    uint16_t rounds;
    wash_intensity_t intensity;
    bool enabled;
} wash_action_t;

typedef enum {
    WASH_PROGRAM_FORMAL = 0,
    WASH_PROGRAM_DEMO,
    WASH_PROGRAM_CUSTOM,
} wash_program_kind_t;

typedef struct {
    app_source_t source;
    wash_program_kind_t kind;
    bool allow_uv;
    bool allow_dry;
    size_t action_count;
    wash_action_t actions[WASH_INTENT_MAX_ACTIONS];
} wash_intent_t;

typedef enum {
    STEP_WAIT_LOAD_CONFIRM = 0,
    STEP_MOVE_POSITION,
    STEP_HOME_BL50,
    STEP_WATER_IN,
    STEP_DETERGENT,
    STEP_PULSATOR_WASH,
    STEP_DRUM_WASH,
    STEP_DRAIN,
    STEP_SPIN,
    STEP_DRY,
    STEP_WAIT_UNLOAD_CONFIRM,
    STEP_UV,
    STEP_FINISH,
} wash_step_type_t;

typedef enum {
    WASH_STEP_FLAG_NONE = 0,
    WASH_STEP_FLAG_SYSTEM_INSERTED = 1U << 0,
    WASH_STEP_FLAG_REQUIRED = 1U << 1,
    WASH_STEP_FLAG_AGENT_MODIFIED = 1U << 2,
} wash_step_flags_t;

typedef struct {
    uint16_t step_id;
    wash_step_type_t type;
    drum_position_t required_position;
    uint32_t duration_ms;
    uint32_t timeout_ms;
    uint16_t rounds;
    wash_intensity_t intensity;
    uint32_t flags;
} wash_step_t;

typedef struct {
    uint32_t program_id;
    wash_program_kind_t kind;
    uint32_t estimated_total_ms;
    size_t step_count;
    wash_step_t steps[WASH_PROGRAM_MAX_STEPS];
} wash_program_t;

typedef struct {
    uint32_t revision;
    machine_state_t state;
    wash_phase_t phase;
    uint32_t program_id;
    uint16_t current_step;
    uint16_t total_steps;
    uint8_t progress_percent;
    uint32_t elapsed_ms;
    uint32_t remaining_ms;
    drum_position_t position;
    drum_position_t target_position;
    bool water_full;
    uint32_t flow_pulses;
    uint32_t estimated_water_ml;
    float temperature_c;
    float humidity_rh;
    bool sht_valid;
    uint8_t fan_percent;
    bool heater_on;
    bool uv_on;
    bool detergent_pump_on;
    bool ble_connected;
    bool wifi_connected;
    uint8_t voice_phase_code;
    machine_fault_t fault;
} machine_status_t;
```

## 4. `position_service.h`

> Phase 1: `position_direction_policy_t` 已移至 `machine_types.h`（L0），因为 `machine_config` 需要它但不能反向依赖 L2 `position_service`。Phase 3 实现 `position_service.h` 时不再重复定义此枚举。

```c
typedef enum {
    POSITION_DIR_AUTO_SHORTEST = 0,
    POSITION_DIR_PREFER_CW,
    POSITION_DIR_PREFER_CCW,
    POSITION_DIR_FORCE_CW,
    POSITION_DIR_FORCE_CCW,
    POSITION_DIR_USE_DEFAULT,   /* 请求中使用配置默认策略 */
} position_direction_policy_t;

/* 方向策略语义:
 * - AUTO_SHORTEST: CW/CCW 距离短者胜；180° 等距时 CW 胜 (tie-break)
 * - PREFER_CW/CCW: 未知位置按首选方向搜索
 * - FORCE_CW/CCW: 强制方向
 * - USE_DEFAULT: 解析为 config.default_policy
 * 角度映射使用显式表，不依赖枚举值 cast。
 */

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
    bool moving_cw;
    uint8_t pwm_percent;
    machine_request_id_t active_request_id;
    machine_fault_t fault;
} position_snapshot_t;

/* Lifecycle: init → start → [running] → stop */
esp_err_t position_service_init(const position_service_config_t *config,
                                const xiaojing_hal_t *hal,
                                machine_event_sink_t event_sink);
esp_err_t position_service_start(void);
esp_err_t position_service_stop(void);  /* 有界、幂等 */

esp_err_t position_service_move_async(const position_move_request_t *request);
esp_err_t position_service_cancel(machine_request_id_t request_id);
esp_err_t position_service_get_snapshot(position_snapshot_t *out);
esp_err_t position_service_emergency_stop(void);  /* task notification, 非阻塞 */
```

## 5. `water_service.h`

```c
typedef enum {
    WATER_STATE_IDLE = 0,
    WATER_STATE_VALIDATING,
    WATER_STATE_SOURCE_FILL,
    WATER_STATE_SOURCE_SETTLE,
    WATER_STATE_TRANSFER,
    WATER_STATE_CHECK_DRUM_LEVEL,
    WATER_STATE_CANCELING,
    WATER_STATE_COMPLETE,
    WATER_STATE_FAULT,
} water_state_t;

typedef struct {
    float pulses_per_liter;
    uint32_t no_flow_timeout_ms;
    uint32_t low_flow_window_ms;
    uint32_t source_batch_target_pulses;
    uint32_t source_batch_max_ms;
    uint32_t source_settle_ms;
    uint32_t transfer_timeout_ms;
    uint32_t default_transfer_ms;
    uint16_t max_fill_cycles;
    uint32_t total_inlet_timeout_ms;
} water_service_config_t;

typedef struct {
    machine_request_id_t request_id;
    uint32_t target_volume_ml;
    uint32_t source_batch_max_ms;
    uint32_t transfer_timeout_ms;
    uint32_t total_inlet_timeout_ms;
    bool complete_on_drum_full;
} water_in_request_t;

typedef struct {
    water_state_t state;
    machine_request_id_t active_request_id;
    bool source_valve_on;
    bool transfer_valve_on;
    bool drum_water_full;
    uint16_t completed_cycles;
    uint32_t current_batch_pulses;
    uint32_t total_flow_pulses;
    uint32_t estimated_ml;
    uint32_t elapsed_ms;
    machine_fault_t fault;
} water_snapshot_t;

esp_err_t water_service_init(const water_service_config_t *config,
                             const xiaojing_hal_t *hal,
                             machine_event_sink_t event_sink);
esp_err_t water_service_start(void);
esp_err_t water_service_fill_async(const water_in_request_t *request);
esp_err_t water_service_cancel(machine_request_id_t request_id);
esp_err_t water_service_get_snapshot(water_snapshot_t *out);
esp_err_t water_service_emergency_close(void);
```

`complete_on_drum_full` 在正常洗涤必须为 true；只有硬件 bring-up 专项测试允许 false。

## 6. `dry_service.h`

```c
typedef enum {
    DRY_STATE_IDLE = 0,
    DRY_STATE_VALIDATING,
    DRY_STATE_PRE_FAN,
    DRY_STATE_HEATING_ON,
    DRY_STATE_HEATING_OFF,
    DRY_STATE_COOLDOWN,
    DRY_STATE_CANCELING,
    DRY_STATE_COMPLETE,
    DRY_STATE_FAULT,
} dry_state_t;

typedef struct {
    uint32_t max_total_ms;
    uint32_t pre_fan_ms;
    uint32_t heat_on_max_ms;
    uint32_t heat_off_min_ms;
    uint32_t cooldown_ms;
    float heater_cutoff_c;
    float heater_resume_c;
    uint8_t fan_percent;
    uint32_t sht_stale_timeout_ms;
} dry_service_config_t;

typedef struct {
    machine_request_id_t request_id;
    uint32_t duration_ms;
    wash_intensity_t intensity;
    bool allow_heater;
    uint32_t requested_heat_on_ms;
} dry_request_t;

typedef struct {
    dry_state_t state;
    machine_request_id_t active_request_id;
    float temperature_c;
    float humidity_rh;
    bool sht_valid;
    bool fan_on;
    uint8_t fan_percent;
    bool heater_on;
    uint32_t elapsed_ms;
    uint32_t remaining_ms;
    machine_fault_t fault;
} dry_snapshot_t;

esp_err_t dry_service_init(const dry_service_config_t *config,
                           const xiaojing_hal_t *hal,
                           machine_event_sink_t event_sink);
esp_err_t dry_service_start(void);
esp_err_t dry_service_run_async(const dry_request_t *request);
esp_err_t dry_service_cancel(machine_request_id_t request_id);
esp_err_t dry_service_get_snapshot(dry_snapshot_t *out);
esp_err_t dry_service_emergency_shutdown(void);
```

## 7. `wash_planner.h`

```c
typedef enum {
    PLAN_RESULT_ACCEPTED = 0,
    PLAN_RESULT_ACCEPTED_WITH_CHANGES,
    PLAN_RESULT_REJECTED,
} wash_plan_result_t;

typedef enum {
    PLAN_WARNING_NONE = 0,
    PLAN_WARNING_WATER_INSERTED = 1U << 0,
    PLAN_WARNING_DRAIN_INSERTED = 1U << 1,
    PLAN_WARNING_DURATION_CLIPPED = 1U << 2,
    PLAN_WARNING_DRY_CLIPPED = 1U << 3,
    PLAN_WARNING_POSITION_ADJUSTED = 1U << 4,
    PLAN_WARNING_UNKNOWN_ACTION_DROPPED = 1U << 5,
} wash_plan_warning_t;

typedef struct {
    wash_plan_result_t result;
    uint32_t warning_mask;
    uint32_t rejected_action_mask;
    uint32_t original_total_ms;
    uint32_t compiled_total_ms;
    machine_fault_code_t reason;
} wash_plan_report_t;

typedef struct {
    uint32_t max_total_ms;
    uint32_t max_dry_ms;
    uint32_t default_uv_ms;
    uint32_t formal_target_ms;
    uint32_t demo_target_max_ms;
    bool insert_required_water;
    bool insert_required_drain;
} wash_policy_t;

esp_err_t wash_planner_init(const wash_policy_t *policy);
esp_err_t wash_planner_make_default(wash_program_kind_t kind,
                                    wash_intent_t *out_intent);
esp_err_t wash_planner_compile(const wash_intent_t *intent,
                               wash_program_t *out_program,
                               wash_plan_report_t *out_report);
esp_err_t wash_planner_validate_program(const wash_program_t *program,
                                        wash_plan_report_t *out_report);
```

`wash_planner` 不得包含 FreeRTOS、HAL、driver、cJSON、NimBLE、HTTP 或语音依赖。

## 8. `safety_manager.h`

```c
typedef enum {
    SAFETY_OP_POSITION_MOTOR = 0,
    SAFETY_OP_BL50,
    SAFETY_OP_SOURCE_INLET_VALVE,
    SAFETY_OP_TRANSFER_VALVE,
    SAFETY_OP_DRAIN_VALVE,
    SAFETY_OP_DETERGENT_PUMP,
    SAFETY_OP_UV,
    SAFETY_OP_FAN,
    SAFETY_OP_PTC_HEATER,
} safety_operation_t;

typedef struct {
    safety_operation_t operation;
    bool enable;
    machine_request_id_t request_id;
    drum_position_t required_position;
    app_source_t source;
} safety_request_t;

typedef struct {
    bool allowed;
    machine_fault_code_t reason;
    uint32_t interlock_mask;
} safety_decision_t;

typedef struct {
    bool fault_active;
    machine_fault_t primary_fault;
    uint64_t active_fault_mask;
    uint64_t allowed_operation_mask;
    bool emergency_stop_active;
    bool water_outputs_safe;
    bool heater_safe;
} safety_snapshot_t;

typedef esp_err_t (*safety_stop_hook_t)(void *context);

esp_err_t safety_manager_init(const xiaojing_hal_t *hal,
                              machine_event_sink_t event_sink);
esp_err_t safety_manager_start(void);
esp_err_t safety_manager_register_stop_hook(safety_operation_t operation,
                                             safety_stop_hook_t hook,
                                             void *context);
esp_err_t safety_manager_check(const safety_request_t *request,
                               safety_decision_t *out_decision);
esp_err_t safety_manager_apply(const safety_request_t *request);
esp_err_t safety_manager_report_fault(const machine_fault_t *fault);
esp_err_t safety_manager_emergency_stop(machine_fault_code_t reason);
esp_err_t safety_manager_request_fault_clear(app_source_t source);
esp_err_t safety_manager_get_snapshot(safety_snapshot_t *out);
```

`safety_manager_apply()` 是 GPA0/GPA1/GPA2/GPA3/GPA7/GPB7 的唯一业务写入口。

## 9. `ble_transport.h`

```c
#define BLE_TRANSPORT_MAX_FRAME 2048

typedef enum {
    BLE_TRANSPORT_DISCONNECTED = 0,
    BLE_TRANSPORT_CONNECTED,
    BLE_TRANSPORT_NOTIFY_READY,
} ble_transport_state_t;

typedef enum {
    BLE_TX_NORMAL = 0,
    BLE_TX_IMPORTANT,
    BLE_TX_CRITICAL,
} ble_tx_priority_t;

typedef struct {
    const char *device_name;
    size_t max_frame_len;
    uint32_t fragment_timeout_ms;
    size_t rx_queue_depth;
    size_t tx_queue_depth;
} ble_transport_config_t;

typedef void (*ble_frame_handler_t)(const uint8_t *data,
                                    size_t len,
                                    void *context);
typedef void (*ble_state_handler_t)(ble_transport_state_t state,
                                    void *context);

esp_err_t ble_transport_init(const ble_transport_config_t *config);
esp_err_t ble_transport_set_handlers(ble_frame_handler_t frame_handler,
                                     ble_state_handler_t state_handler,
                                     void *context);
esp_err_t ble_transport_start(void);
esp_err_t ble_transport_notify(const uint8_t *data,
                               size_t len,
                               ble_tx_priority_t priority);
esp_err_t ble_transport_get_state(ble_transport_state_t *out_state);
```

Transport 不解析业务 JSON、不生成 ACK、不调用 planner/executor。

## 10. `voice_agent.h`

```c
typedef enum {
    VOICE_PHASE_DISABLED = 0,
    VOICE_PHASE_IDLE,
    VOICE_PHASE_WAKE_DETECTED,
    VOICE_PHASE_RECORDING,
    VOICE_PHASE_UPLOADING,
    VOICE_PHASE_WAITING_LLM,
    VOICE_PHASE_PARSING,
    VOICE_PHASE_SPEAKING,
    VOICE_PHASE_SUBMITTING,
    VOICE_PHASE_COOLDOWN,
    VOICE_PHASE_ERROR,
} voice_phase_t;

typedef struct {
    bool enabled;
    voice_phase_t phase;
    uint32_t interaction_seq;
    int last_http_status;
    esp_err_t last_error;
} voice_agent_snapshot_t;

typedef struct {
    const char *api_endpoint;
    const char *model;
    uint32_t max_record_ms;
    uint32_t vad_silence_ms;
    bool suppress_during_motor_run;
} voice_agent_config_t;

typedef esp_err_t (*voice_submit_handler_t)(const wash_intent_t *intent,
                                            uint32_t interaction_seq,
                                            void *context);
typedef void (*voice_phase_handler_t)(voice_phase_t phase,
                                      uint32_t interaction_seq,
                                      void *context);

esp_err_t voice_agent_init(const voice_agent_config_t *config,
                           voice_submit_handler_t submit_handler,
                           voice_phase_handler_t phase_handler,
                           void *context);
esp_err_t voice_agent_start(void);
esp_err_t voice_agent_set_enabled(bool enabled);
esp_err_t voice_agent_on_machine_state(machine_state_t state,
                                       wash_phase_t phase);
esp_err_t voice_agent_cancel_interaction(uint32_t interaction_seq);
esp_err_t voice_agent_get_snapshot(voice_agent_snapshot_t *out);
```

`submit_handler` 的实现只能进入 `app_protocol`，不能触达 service/driver。

## 11. `app_protocol.h`

```c
typedef enum {
    APP_CMD_HELLO = 0,
    APP_CMD_GET_STATUS,
    APP_CMD_START_FORMAL,
    APP_CMD_START_DEMO,
    APP_CMD_SUBMIT_PLAN,
    APP_CMD_ACK_LOAD,
    APP_CMD_ACK_UNLOAD,
    APP_CMD_SKIP_UV,
    APP_CMD_ABORT_RESET,
    APP_CMD_PAUSE,
    APP_CMD_RESUME,
} app_command_t;

esp_err_t app_protocol_init(void);
esp_err_t app_protocol_start(void);
esp_err_t app_protocol_submit_frame(app_source_t source,
                                    const uint8_t *data,
                                    size_t len);
esp_err_t app_protocol_submit_command(app_source_t source,
                                      app_command_t command,
                                      uint32_t source_seq);
esp_err_t app_protocol_submit_intent(app_source_t source,
                                     const wash_intent_t *intent,
                                     uint32_t source_seq);
esp_err_t app_protocol_publish_status(bool full_snapshot);
esp_err_t app_protocol_publish_fault(const machine_fault_t *fault);
```

协议层拥有 JSON/seq/ACK/NACK/旧协议兼容，不拥有整机状态。

## 12. `wash_executor.h`

```c
typedef struct {
    size_t control_queue_depth;
    size_t urgent_queue_depth;
    size_t event_queue_depth;
} wash_executor_config_t;

esp_err_t wash_executor_init(const wash_executor_config_t *config);
esp_err_t wash_executor_start(void);
esp_err_t wash_executor_submit(const wash_program_t *program,
                               app_source_t source,
                               uint32_t source_seq);
esp_err_t wash_executor_confirm_load(void);
esp_err_t wash_executor_confirm_unload(void);
esp_err_t wash_executor_skip_uv(void);
esp_err_t wash_executor_abort_reset(app_source_t source);
esp_err_t wash_executor_pause(app_source_t source);
esp_err_t wash_executor_resume(app_source_t source);
esp_err_t wash_executor_get_status(machine_status_t *out);
```

Executor 必须：

- 每个异步步骤生成新 request_id。
- 只接收当前 service/type/request_id 匹配的终态事件。
- 不轮询等待传感器；服务自己完成局部状态机。
- urgent abort 抢占普通命令。
- 故障时停止推进，不自动机械复位。

## 13. `machine_status_store.h`

```c
esp_err_t machine_status_store_init(void);
esp_err_t machine_status_store_update(const machine_status_t *status);
esp_err_t machine_status_store_get(machine_status_t *out_status);
```

`get` 必须返回一致快照；`update` 自动递增 revision，调用者不能手工回退 revision。

## 14. Driver/HAL 边界

`xiaojing_hal_t` 的精确函数可以在 Phase 1 冻结，但必须覆盖：

- MCP 输入快照和安全输出。
- IBT-2 命令/停止。
- BL50 PWM/DIR/STOP/BRAKE 抽象命令。
- 风扇百分比。
- 洗涤桶水位。
- 流量快照/清零。
- SHT 温湿度和新鲜度。
- 单调时间。

Fake HAL 必须可脚本化输入、注入每个调用点失败、查询输出历史。Real HAL 不能改变上述业务语义。

## 15. 接口一致性检查

Claude Code 应实现 `tools/check_architecture.py` 或等价测试验证：

- 所有 service 公共 start API 含 request_id。
- planner 公共依赖不含 driver/FreeRTOS/JSON。
- ble_transport 不依赖 app_protocol 的实现头。
- voice_agent 不依赖 service/driver。
- safety_manager 不依赖具体 service 组件。
- 裸 GPIO 数字只出现在 board_config/test fixture。
- GPA 高风险输出只有 safety_manager/底层 MCP driver 可写。

