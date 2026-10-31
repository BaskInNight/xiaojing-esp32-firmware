#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "machine_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * wash_contract.h — 洗涤意图、计划和状态类型
 * 固定容量结构，控制路径禁止无界动态分配
 * ================================================================ */

/* ---- Wash Intent (洗涤意图) ---- */

#define WASH_INTENT_MAX_ACTIONS  32

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

/* ---- Wash Step (执行计划步骤) ---- */

#define WASH_PROGRAM_MAX_STEPS  64

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
    /* Phase 11 Group D R0: DEFAULT_DEMO_V1 新增步骤。append 追加以保持既有
     * 枚举值稳定（禁止重排/重编号）。这三类是「内部步骤」：不经 real_adapter
     * 派发到服务，由 wash_executor 包装层内部处理（定时等待/审计门禁）。 */
    STEP_SETTLE,             /* 定时无输出等待（水/衣物沉降） */
    STEP_AUDIT_UV_OFF,       /* 审计：UV 输出已确认 OFF */
    STEP_AUDIT_SAFE_OFF,     /* 审计：全部危险输出已确认 OFF */
} wash_step_type_t;

typedef enum {
    WASH_STEP_FLAG_NONE            = 0,
    WASH_STEP_FLAG_SYSTEM_INSERTED = 1U << 0,
    WASH_STEP_FLAG_REQUIRED        = 1U << 1,
    WASH_STEP_FLAG_AGENT_MODIFIED  = 1U << 2,
    WASH_STEP_FLAG_HEATER_ON       = 1U << 3,  /* STEP_DRY: heater_requested=true（热风烘干） */
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

/* ---- Wash Program (编译后的不可变计划) ---- */

typedef struct {
    uint32_t program_id;
    wash_program_kind_t kind;
    uint32_t estimated_total_ms;
    size_t step_count;
    wash_step_t steps[WASH_PROGRAM_MAX_STEPS];
} wash_program_t;

/* ---- Machine Status (整机状态快照) ---- */

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

#ifdef __cplusplus
}
#endif
