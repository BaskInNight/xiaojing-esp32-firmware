#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "machine_types.h"
#include "wash_contract.h"
#include "bl50_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * bl50_fsm.h — BL50 纯 C 状态机
 * 无 FreeRTOS、HAL、GPIO、LEDC 依赖
 * 所有时间使用传入 now_ms，tick wrap-safe
 * ================================================================ */

/* ---- BL50 Action Output ---- */

typedef enum {
    BL50_FSM_ACTION_NONE = 0,
    BL50_FSM_ACTION_STOP,
    BL50_FSM_ACTION_BRAKE,
    BL50_FSM_ACTION_RUN_CW,
    BL50_FSM_ACTION_RUN_CCW,
} bl50_fsm_action_t;

typedef enum {
    BL50_FSM_ACTION_RESULT_NONE = 0,
    BL50_FSM_ACTION_RESULT_OK,
    BL50_FSM_ACTION_RESULT_FAILED,
} bl50_fsm_action_result_t;

/* ---- FSM Output ---- */

typedef struct {
    bl50_fsm_action_t action;           /* 要执行的硬件命令 */
    uint8_t pwm_percent;                /* action=RUN_CW/RUN_CCW 时的 PWM */
    bool terminal;                      /* 是否为终态 */
    bool emit_event;                    /* 是否需要发布事件 */
    service_result_t event_result;      /* 终态事件结果 */
    machine_fault_code_t fault_code;    /* 终态故障码 */
    uint32_t fault_detail;              /* 终态故障详情 */
} bl50_fsm_output_t;

/* ---- FSM Context ---- */

typedef struct {
    /* 状态 */
    uint8_t state;                      /* bl50_state_t */
    uint8_t prev_state;                 /* 用于检测状态变化 */

    /* 请求 */
    machine_request_id_t request_id;
    uint8_t action;                     /* bl50_action_t */
    uint32_t duration_ms;
    uint8_t target_pwm_percent;
    uint32_t direction_interval_ms;

    /* 运行时 */
    uint8_t current_pwm_percent;
    bool running_cw;
    uint32_t state_enter_ms;            /* 进入当前状态的时间 */
    uint32_t total_run_ms;              /* 累计运行时间 */
    uint32_t run_start_ms;              /* 本次请求运行起点（跨换向总时长基准） */
    uint8_t ramp_step;                  /* 当前斜坡步数 */

    /* HOME */
    bool home_sensor_triggered;
    uint32_t home_start_ms;

    /* 终态 */
    bool terminal_event_emitted;
    service_result_t terminal_result;
    machine_fault_code_t terminal_fault_code;
    uint32_t terminal_fault_detail;

    /* pending action */
    uint8_t pending_action;             /* bl50_fsm_action_t */
    uint8_t pending_action_result;      /* bl50_fsm_action_result_t */
    uint8_t pending_action_id;          /* 匹配用 */

    /* 故障 */
    machine_fault_t fault;
} bl50_fsm_ctx_t;

/* ---- FSM API ---- */

/* 初始化 FSM 上下文 */
void bl50_fsm_init(bl50_fsm_ctx_t *ctx);

/* 开始一个新请求。仅在 IDLE 状态有效。 */
esp_err_t bl50_fsm_begin_request(bl50_fsm_ctx_t *ctx,
                                  const bl50_request_t *request,
                                  uint8_t home_pwm,
                                  uint32_t home_timeout_ms,
                                  uint32_t max_action_duration_ms,
                                  bool direction_calibrated,
                                  bool is_fake_mode);

/* FSM tick。输入当前时间和 home 传感器状态。输出硬件命令。 */
void bl50_fsm_tick(bl50_fsm_ctx_t *ctx,
                    uint32_t now_ms,
                    bool home_sensor_triggered,
                    bool position_valid,
                    bool position_stable,
                    drum_position_t current_position,
                    bl50_fsm_output_t *out);

/* 提交动作结果（两阶段） */
void bl50_fsm_commit_result(bl50_fsm_ctx_t *ctx,
                             bl50_fsm_action_result_t result);

/* 取消请求。request_id 不匹配时返回 false。 */
bool bl50_fsm_cancel(bl50_fsm_ctx_t *ctx, machine_request_id_t request_id);

/* 急停。从任何状态进入 STOP。 */
void bl50_fsm_emergency_stop(bl50_fsm_ctx_t *ctx);

/* 故障：带明确身份进入 STOP。code/detail 由调用者指定。 */
void bl50_fsm_fail(bl50_fsm_ctx_t *ctx,
                   machine_fault_code_t code,
                   uint32_t detail);

/* 标记终态事件已发布 */
void bl50_fsm_mark_terminal_emitted(bl50_fsm_ctx_t *ctx);

/* 获取当前状态 */
bl50_service_state_t bl50_fsm_get_state(const bl50_fsm_ctx_t *ctx);

/* 获取活跃请求 ID */
machine_request_id_t bl50_fsm_get_request_id(const bl50_fsm_ctx_t *ctx);

/* ---- 姿态-动作映射 ---- */

drum_position_t bl50_action_to_position(bl50_action_t action);

/* ---- Config defaults ---- */

#define BL50_DEFAULT_HOME_PWM          15
#define BL50_DEFAULT_HOME_TIMEOUT_MS   10000
#define BL50_DEFAULT_RAMP_STEP_PERCENT  5
#define BL50_DEFAULT_RAMP_INTERVAL_MS  100
#define BL50_DEFAULT_REVERSAL_STOP_MS  500
#define BL50_DEFAULT_PULSATOR_PWM      40
#define BL50_DEFAULT_PULSATOR_DIR_MS   5000
#define BL50_DEFAULT_DRUM_PWM          30
#define BL50_DEFAULT_DRUM_DIR_MS       8000
#define BL50_DEFAULT_SPIN_PWM          80
#define BL50_DEFAULT_MAX_ACTION_MS     1800000

#ifdef __cplusplus
}
#endif
