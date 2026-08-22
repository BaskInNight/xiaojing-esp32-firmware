#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * machine_types.h — 公共类型定义
 * 所有后续组件共享的稳定类型契约
 * ================================================================ */

/* ---- Request ID ---- */

typedef uint32_t machine_request_id_t;

#define MACHINE_REQUEST_ID_INVALID  0U

/* 递增 request_id，UINT32_MAX 后回绕到 1（跳过 0=INVALID） */
static inline machine_request_id_t machine_request_id_next(machine_request_id_t current)
{
    machine_request_id_t next = current + 1;
    return (next == MACHINE_REQUEST_ID_INVALID) ? 1 : next;
}

/* ---- Application Source ---- */

typedef enum {
    APP_SOURCE_BUTTON = 0,
    APP_SOURCE_BLE,
    APP_SOURCE_VOICE,
    APP_SOURCE_WEB,
    APP_SOURCE_SYSTEM,
} app_source_t;

/* ---- Drum Position (五姿态外筒定位) ---- */

typedef enum {
    DRUM_POS_UNKNOWN = -1,
    DRUM_POS_0   =   0,
    DRUM_POS_45  =  45,
    DRUM_POS_90  =  90,
    DRUM_POS_180 = 180,
    DRUM_POS_270 = 270,
} drum_position_t;

#define DRUM_POSITION_COUNT  5

/* 返回有效姿态数（不含 UNKNOWN）的位掩码，用于输入校验 */
static inline bool drum_position_is_valid(drum_position_t pos)
{
    switch (pos) {
    case DRUM_POS_0:
    case DRUM_POS_45:
    case DRUM_POS_90:
    case DRUM_POS_180:
    case DRUM_POS_270:
        return true;
    default:
        return false;
    }
}

/* ---- Position Direction Policy (置于 L0 供 machine_config 使用) ---- */

typedef enum {
    POSITION_DIR_AUTO_SHORTEST = 0,
    POSITION_DIR_PREFER_CW,
    POSITION_DIR_PREFER_CCW,
    POSITION_DIR_FORCE_CW,
    POSITION_DIR_FORCE_CCW,
    POSITION_DIR_USE_DEFAULT,   /* 请求中使用配置默认策略 */
} position_direction_policy_t;

#define POSITION_DIR_MAX_VALID_FOR_CONFIG  POSITION_DIR_FORCE_CCW

/* ---- Machine State (整机状态) ---- */

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

/* ---- Wash Phase (当前执行阶段) ---- */

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

/* ---- Fault Codes ---- */

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
    FAULT_FLOW_METER_IO,        /* PCNT/flow sensor I/O error */
    FAULT_FLOW_METER_RESET,     /* flow counter reset failure */
    FAULT_I2C_BUS,
    FAULT_BL50,
    FAULT_BL50_HOME_IO,         /* HOME 传感器 MCP/I2C I/O 错误 */
    FAULT_BL50_HOME_TIMEOUT,    /* HOME 传感器超时 */
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

static inline bool machine_fault_is_active(const machine_fault_t *fault)
{
    return fault != NULL && fault->code != FAULT_NONE;
}

static inline void machine_fault_clear(machine_fault_t *fault)
{
    if (fault) {
        fault->code = FAULT_NONE;
        fault->severity = FAULT_SEVERITY_WARNING;
        fault->detail = 0;
        fault->timestamp_ms = 0;
    }
}

/* ---- Service Result ---- */

typedef enum {
    SERVICE_RESULT_OK = 0,
    SERVICE_RESULT_CANCELED,
    SERVICE_RESULT_TIMEOUT,
    SERVICE_RESULT_REJECTED,
    SERVICE_RESULT_SKIPPED,
    SERVICE_RESULT_FAULT,
} service_result_t;

/* ---- Machine Event ---- */

typedef enum {
    MACHINE_EVENT_POSITION_DONE = 0,
    MACHINE_EVENT_WATER_DONE,
    MACHINE_EVENT_DRY_DONE,
    MACHINE_EVENT_HOT_AIR_DONE,   /* 耦合热风模块终态 */
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

/* Event sink: bootstrap 将其适配到 executor 的 machine_event_queue */
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
