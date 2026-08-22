#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "wash_contract.h"
#include "machine_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * wash_planner.h — Pure C wash program compiler
 *
 * Pure function: no static mutable state, no FreeRTOS/HAL/NVS/GPIO.
 * Reentrant. Deterministic. Thread-safe.
 * Fixed capacity output. No dynamic memory.
 * ================================================================ */

/* ---- Planner result ---- */

typedef enum {
    PLAN_RESULT_OK       = 0,
    PLAN_RESULT_REJECTED = 1,
} planner_result_t;

/* ---- Planner error ---- */

typedef enum {
    PLAN_ERROR_NONE                = 0,
    PLAN_ERROR_INVALID_ARG         = 1,
    PLAN_ERROR_INVALID_PROGRAM_ID  = 2,
    PLAN_ERROR_INVALID_ACTION      = 3,
    PLAN_ERROR_INVALID_POSITION    = 4,
    PLAN_ERROR_INVALID_DURATION    = 5,
    PLAN_ERROR_DURATION_OVERFLOW   = 6,
    PLAN_ERROR_STEP_OVERFLOW       = 7,
    PLAN_ERROR_TOTAL_LIMIT         = 8,
    PLAN_ERROR_PROFILE_UNAVAILABLE = 9,
    PLAN_ERROR_INVALID_CONFIG      = 10,
} planner_error_t;

/* ---- Planner report ---- */

typedef struct {
    planner_result_t result;
    planner_error_t reject_reason;
    uint16_t reject_action_index;
    uint32_t original_total_ms;
    uint32_t final_total_ms;
    uint32_t original_dry_ms;
    uint32_t final_dry_ms;
    uint16_t steps_inserted;   /* system-inserted steps only */
    uint16_t steps_merged;     /* explicit adjacent MOVE merges only */
    uint16_t steps_dropped;    /* disabled/skipped actions */
    uint32_t fixup_flags;
} planner_report_t;

/* ---- Fixup flag bits ---- */

#define PLAN_FIXUP_SKIP_UV      (1U << 0)
#define PLAN_FIXUP_SKIP_DRY     (1U << 1)

/* ---- Compile a wash intent into a wash program ---- */

planner_report_t wash_planner_compile(
    const wash_intent_t *intent,
    const machine_config_t *config,
    uint32_t program_id,
    wash_program_t *out_program);

#ifdef __cplusplus
}
#endif
