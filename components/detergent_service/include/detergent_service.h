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
 * detergent_service.h — 洗衣液投放服务公共 API
 *
 * 纯 C 接口。状态机在 detergent_fsm.h，不在此暴露。
 * 生命周期: global_init -> init -> start -> run_async/cancel -> stop
 * 输出: GPB7 洗衣液泵 via SAFETY_OP_DETERGENT_PUMP
 * 姿态要求: DRUM_POS_0 fresh+stable
 * ================================================================ */

/* ---- States ---- */

typedef enum {
    DETERGENT_STATE_UNINITIALIZED = 0,
    DETERGENT_STATE_IDLE,
    DETERGENT_STATE_VALIDATING,
    DETERGENT_STATE_RUNNING,
    DETERGENT_STATE_STOPPING,
    DETERGENT_STATE_COMPLETE,
    DETERGENT_STATE_CANCELING,
    DETERGENT_STATE_FAULT,
} detergent_state_t;

/* ---- Terminal ---- */

typedef enum {
    DETERGENT_TERMINAL_NONE = 0,
    DETERGENT_TERMINAL_COMPLETE,
    DETERGENT_TERMINAL_CANCELED,
    DETERGENT_TERMINAL_TIMEOUT,
    DETERGENT_TERMINAL_REJECTED,
    DETERGENT_TERMINAL_FAULT,
} detergent_terminal_t;

/* ---- Request ---- */

typedef struct {
    machine_request_id_t request_id;
    uint32_t duration_ms;
} detergent_request_t;

/* ---- Snapshot ---- */

typedef struct {
    detergent_state_t state;
    machine_request_id_t request_id;
    uint32_t elapsed_ms;
    uint32_t requested_duration_ms;
    drum_position_t position;
    bool position_valid;
    bool position_stable;
    bool position_fresh;
    int output_state;
    bool terminal_pending;
    detergent_terminal_t terminal;
} detergent_snapshot_t;

/* ---- Lifecycle ---- */

esp_err_t detergent_service_global_init(void);
esp_err_t detergent_service_init(const detergent_params_t *config,
                                   const xiaojing_hal_t *hal,
                                   machine_event_sink_t event_sink);
esp_err_t detergent_service_start(void);
esp_err_t detergent_service_stop(void);

/* ---- Commands ---- */

esp_err_t detergent_service_run_async(const detergent_request_t *request);
esp_err_t detergent_service_cancel(machine_request_id_t request_id);
esp_err_t detergent_service_emergency_stop(void);

/* ---- Query ---- */

esp_err_t detergent_service_get_snapshot(detergent_snapshot_t *out);

/* ---- Test Hooks ---- */

#ifdef DETERGENT_SERVICE_TEST_HOOKS

esp_err_t detergent_service_abort_cleanup(void);
esp_err_t detergent_service_test_reset(void);
bool detergent_service_test_resources_alive(void);
esp_err_t detergent_service_test_pause_task(void);
esp_err_t detergent_service_test_resume_task(void);
uint32_t detergent_service_test_get_mailbox_consumed(void);

#endif

#ifdef __cplusplus
}
#endif
