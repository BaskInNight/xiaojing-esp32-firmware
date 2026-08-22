/*
 * motor_bench_service.h — 电机空载台架 运行时组件（诊断模式专用）
 *
 * 三种能力（capability 门控，仅开发测试入口可见）：
 *   pulsator_motor_self_test  主洗涤/波轮独立自检（单步）
 *   drum_motor_self_test      滚筒/BLDC 独立自检（单步）
 *   motor_hall_bench_sequence 人工霍尔流程台架（多步交互）
 *
 * 安全模型（与生产洗涤流程同源，绝不自动让电机通电）：
 *   - 每步仅在用户手动摆放桶位稳定后，经用户显式确认（confirm_pulse），
 *     才提交一次低能量短脉冲（低档 ≤300ms）到真实 bl50_service_run_async；
 *   - 每步脉冲后确认 BL50 回 IDLE 且 PWM=0 才推进；否则 fail-closed；
 *   - 从不触发进水/转移/排水/蠕动/UV/热风/加热器；start/cancel/timeout/
 *     exception/BLE 断线/emergency 均使全部输出 OFF；
 *   - 生产默认洗涤订单绝不调用本测试（executor 空闲门禁 + capability 门控）。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "machine_types.h"
#include "motor_bench_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wash_executor wash_executor_t;

typedef struct {
    wash_executor_t *executor;   /* 门禁用 executor 快照 */
} motor_bench_service_config_t;

typedef struct {
    motor_bench_state_t state;
    motor_bench_type_t type;
    machine_request_id_t request_id;   /* 最近一次受理的台架 request_id */
    uint8_t current_step;              /* 0-based；仅在运行中有效 */
    uint8_t step_count;
    drum_position_t required_position; /* 当前步所需位置（生产契约派生） */
    bl50_action_t current_action;      /* 当前步动作 */
    bool position_ready;               /* 当前步位置已就位，等待确认脉冲 */
    motor_bench_terminal_t terminal;
    const char *last_code;             /* ACCEPTED / 拒绝码 / 等待原因 */
    bool output_confirmed_off;
    uint32_t elapsed_ms;               /* 本次台架运行时长 */
} motor_bench_snapshot_t;

esp_err_t motor_bench_service_global_init(void);
esp_err_t motor_bench_service_init(const motor_bench_service_config_t *config);
esp_err_t motor_bench_service_start(void);
/* 停止组件。停止前请求全部输出 OFF 并从服务快照确认；失败保留生命周期可重试。 */
esp_err_t motor_bench_service_stop(void);

/* 受理一个电机台架测试（由 app_protocol 回调调用，非阻塞，异步执行）。
 * 同步完成受理门禁；*out_code: 成功 "MOTOR_BENCH_ACCEPTED"，
 * 失败 machine-readable 拒绝码。返回 ESP_OK 表示调用成功（含被拒）。 */
esp_err_t motor_bench_service_request(motor_bench_type_t type,
                                      const char **out_code);

/* 当前步位置已就位后，用户显式确认提交一次脉冲。仅 WAIT_CONFIRM 状态消费；
 * 其他状态为 no-op。重复点击防重入。 */
esp_err_t motor_bench_service_confirm_pulse(void);

/* 取消当前台架（用户取消/abort）。全部输出 OFF。 */
esp_err_t motor_bench_service_cancel(void);

/* 急停（BLE 断线 / teardown / 急停冲突时调用）。全部输出 OFF。 */
esp_err_t motor_bench_service_emergency(void);

/* 快照。 */
esp_err_t motor_bench_service_get_snapshot(motor_bench_snapshot_t *out);

/* 供 status 消息：当前台架状态码（motor_bench_state_t） */
uint8_t motor_bench_service_state_code(void);

#ifdef __cplusplus
}
#endif
