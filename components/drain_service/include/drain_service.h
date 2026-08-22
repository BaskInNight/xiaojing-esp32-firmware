#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "machine_types.h"
#include "machine_config.h"
#include "xiaojing_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * drain_service.h — 排水服务公共 API
 *
 * 纯 C 接口。状态机在 drain_fsm.h，不在此暴露。
 * 生命周期: global_init -> init -> start -> run_async/cancel -> stop
 * 输出: GPA3 排水阀 via SAFETY_OP_DRAIN_VALVE
 * 姿态要求: DRUM_POS_180 fresh+stable
 * ================================================================ */

/* ---- States ---- */

typedef enum {
    DRAIN_STATE_UNINITIALIZED = 0,
    DRAIN_STATE_IDLE,
    DRAIN_STATE_VALIDATING,
    DRAIN_STATE_RUNNING,
    DRAIN_STATE_STOPPING,
    DRAIN_STATE_COMPLETE,
    DRAIN_STATE_CANCELING,
    DRAIN_STATE_FAULT,
} drain_state_t;

/* ---- Terminal ---- */

typedef enum {
    DRAIN_TERMINAL_NONE = 0,
    DRAIN_TERMINAL_COMPLETE,
    DRAIN_TERMINAL_CANCELED,
    DRAIN_TERMINAL_TIMEOUT,
    DRAIN_TERMINAL_REJECTED,
    DRAIN_TERMINAL_FAULT,
} drain_terminal_t;

/* ---- Request ---- */

typedef struct {
    machine_request_id_t request_id;
    uint32_t duration_ms;
} drain_request_t;

/* ---- Snapshot ---- */

typedef struct {
    drain_state_t state;
    machine_request_id_t request_id;
    uint32_t elapsed_ms;
    uint32_t requested_duration_ms;
    drum_position_t position;
    bool position_valid;
    bool position_stable;
    bool position_fresh;
    int output_state;               /* drain_output_state: 0=UNKNOWN, 1=OFF, 2=ON */
    bool terminal_pending;
    drain_terminal_t terminal;
} drain_snapshot_t;

/* ---- Lifecycle ---- */

esp_err_t drain_service_global_init(void);
esp_err_t drain_service_init(const drain_params_t *config,
                              const xiaojing_hal_t *hal,
                              machine_event_sink_t event_sink);
esp_err_t drain_service_start(void);
esp_err_t drain_service_stop(void);

/* ---- Commands ---- */

esp_err_t drain_service_run_async(const drain_request_t *request);
esp_err_t drain_service_cancel(machine_request_id_t request_id);
esp_err_t drain_service_emergency_stop(void);

/* ---- Query ---- */

esp_err_t drain_service_get_snapshot(drain_snapshot_t *out);

/* ---- Test Hooks ---- */

#ifdef DRAIN_SERVICE_TEST_HOOKS

esp_err_t drain_service_abort_cleanup(void);
esp_err_t drain_service_test_reset(void);
bool drain_service_test_resources_alive(void);
esp_err_t drain_service_test_pause_task(void);
esp_err_t drain_service_test_resume_task(void);
uint32_t drain_service_test_get_mailbox_consumed(void);

#endif

#ifdef __cplusplus
}
#endif
