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
 * uv_service.h — UV 杀菌服务公共 API
 *
 * 纯 C 接口。状态机在 uv_fsm.h，不在此暴露。
 * 生命周期: global_init -> init -> start -> run_async/cancel/skip -> stop
 * 输出: GPA2 UV 灯 via SAFETY_OP_UV
 * 姿态要求: DRUM_POS_0 fresh+stable
 * ================================================================ */

/* ---- States ---- */

typedef enum {
    UV_STATE_UNINITIALIZED = 0,
    UV_STATE_IDLE,
    UV_STATE_VALIDATING,
    UV_STATE_RUNNING,
    UV_STATE_STOPPING,
    UV_STATE_COMPLETE,
    UV_STATE_CANCELING,
    UV_STATE_SKIPPING,
    UV_STATE_FAULT,
} uv_state_t;

/* ---- Terminal ---- */

typedef enum {
    UV_TERMINAL_NONE = 0,
    UV_TERMINAL_COMPLETE,
    UV_TERMINAL_CANCELED,
    UV_TERMINAL_SKIPPED,
    UV_TERMINAL_TIMEOUT,
    UV_TERMINAL_REJECTED,
    UV_TERMINAL_FAULT,
} uv_terminal_t;

/* ---- Request ---- */

typedef struct {
    machine_request_id_t request_id;
    uint32_t duration_ms;
} uv_request_t;

/* ---- Snapshot ---- */

typedef struct {
    uv_state_t state;
    machine_request_id_t request_id;
    uint32_t elapsed_ms;
    uint32_t requested_duration_ms;
    drum_position_t position;
    bool position_valid;
    bool position_stable;
    bool position_fresh;
    int output_state;
    bool terminal_pending;
    uv_terminal_t terminal;
} uv_snapshot_t;

/* ---- Lifecycle ---- */

esp_err_t uv_service_global_init(void);
esp_err_t uv_service_init(const uv_params_t *config,
                            const xiaojing_hal_t *hal,
                            machine_event_sink_t event_sink);
esp_err_t uv_service_start(void);
esp_err_t uv_service_stop(void);

/* ---- Commands ---- */

esp_err_t uv_service_run_async(const uv_request_t *request);
esp_err_t uv_service_cancel(machine_request_id_t request_id);
esp_err_t uv_service_skip(machine_request_id_t request_id);
esp_err_t uv_service_emergency_stop(void);

/* ---- Query ---- */

esp_err_t uv_service_get_snapshot(uv_snapshot_t *out);

/* ---- Test Hooks ---- */

#ifdef UV_SERVICE_TEST_HOOKS

esp_err_t uv_service_abort_cleanup(void);
esp_err_t uv_service_test_reset(void);
bool uv_service_test_resources_alive(void);
esp_err_t uv_service_test_pause_task(void);
esp_err_t uv_service_test_resume_task(void);
uint32_t uv_service_test_get_mailbox_consumed(void);

#endif

#ifdef __cplusplus
}
#endif
