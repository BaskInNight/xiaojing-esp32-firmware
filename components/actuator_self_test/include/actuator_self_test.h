#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "machine_types.h"
#include "actuator_self_test_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wash_executor wash_executor_t;

/* ================================================================
 * actuator_self_test.h — 执行器板测运行时组件
 *
 * 板级诊断命令 actuator_self_test → 15 项安全门禁 → 提交到目标服务
 * （water_service 受控单阀入口 / detergent_service_run_async）→
 * safety_manager_apply → MCP23017。所有真实输出必须经过 safety_manager。
 *
 * 与正式洗涤的关系：
 *   - 不创建订单，不经过 wash_planner 完整序列；
 *   - 仅在 executor/service 都空闲时受理（门禁）；
 *   - 时长由固件固定，客户端不可覆盖；
 *   - 设备端（对应 service）自身计时自动 OFF，即使 BLE 断开也关闭。
 * ================================================================ */

typedef struct {
    wash_executor_t *executor;   /* 用于门禁的 executor 快照 */
} actuator_self_test_config_t;

typedef struct {
    actuator_self_test_state_t state;
    actuator_self_test_target_t target;
    machine_request_id_t request_id;    /* 最近一次受理的板测 request_id */
    uint32_t requested_duration_ms;     /* 固件固定时长 */
    uint32_t elapsed_ms;                /* 最近同步到的已运行时长 */
    actuator_self_test_terminal_t terminal;
    const char *last_code;              /* ACCEPTED 或最近拒绝码/错误码 */
    bool output_confirmed_off;          /* 最近同步到的输出 OFF 确认 */
} actuator_self_test_snapshot_t;

esp_err_t actuator_self_test_init(const actuator_self_test_config_t *config);
esp_err_t actuator_self_test_start(void);
/* 停止组件。停止前请求所有板测输出 OFF 并从服务快照确认；失败保留生命周期，
 * 调用者可重试。 */
esp_err_t actuator_self_test_stop(void);

/* 受理一条 actuator_self_test（由 app_protocol 回调调用，非阻塞，异步提交）。
 * request_id 由组件内部单调分配（非 0、跳过 0）。
 * *out_code: 成功 "ACTUATOR_SELF_TEST_ACCEPTED" + *out_duration_ms=固定时长；
 *            失败 machine-readable 拒绝码 + duration=0。
 * 返回 ESP_OK 表示调用成功（含被拒），ESP_ERR_INVALID_STATE 表示未初始化。 */
esp_err_t actuator_self_test_request(actuator_self_test_target_t target,
                                     const char **out_code,
                                     uint32_t *out_duration_ms);

/* 紧急关停所有板测输出（BLE 断线 / teardown / 急停冲突时调用）。 */
esp_err_t actuator_self_test_emergency(void);

/* 快照（同步各服务快照推进板测状态机；无阻塞）。 */
esp_err_t actuator_self_test_get_snapshot(actuator_self_test_snapshot_t *out);

/* 供 status 消息：当前板测状态码（actuator_self_test_state_t） */
uint8_t actuator_self_test_state_code(void);

#ifdef __cplusplus
}
#endif
