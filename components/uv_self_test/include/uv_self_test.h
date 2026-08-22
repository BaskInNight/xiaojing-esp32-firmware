#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "machine_types.h"
#include "uv_service.h"
#include "uv_self_test_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wash_executor wash_executor_t;

/* ================================================================
 * uv_self_test.h — UV 灯安全自检运行时组件
 *
 * 开发/板级诊断：从 BLE 收 uv_self_test → 12 项安全门禁 →
 * uv_service_run_async(3000ms) → uv_fsm → safety_manager_apply →
 * HAL/MCP23017 → UV 灯。
 *
 * 与正式洗涤的关系：
 *   - 不创建订单，不经 wash_planner 完整序列；
 *   - 仅在 executor/uv_service 都 IDLE 时受理（门禁）；
 *   - uv_service 的事件 sink 仍归正式洗涤（bootstrap），自检通过
 *     uv_service_get_snapshot() 轮询观察，terminal 不会被 wash_executor
 *     错误消费（executor 空闲时 real_adapter 以 stale drop 丢弃）。
 * ================================================================ */

typedef struct {
    wash_executor_t *executor;   /* 用于门禁的 executor 快照 */
} uv_self_test_config_t;

typedef struct {
    uv_self_test_state_t state;
    machine_request_id_t request_id;    /* 最近一次自检 request_id */
    uint32_t requested_duration_ms;     /* 固定 UV_SELF_TEST_DURATION_MS */
    uint32_t elapsed_ms;                /* 最近同步到的已运行时长 */
    uv_terminal_t terminal;             /* 最近 terminal（仅终态有意义） */
    const char *last_code;              /* ACCEPTED 或最近拒绝码 */
    bool lamp_on;                       /* 最近同步到的 UV 灯状态 */
} uv_self_test_snapshot_t;

esp_err_t uv_self_test_init(const uv_self_test_config_t *config);
esp_err_t uv_self_test_start(void);
/* 停止自检组件。停止前必须请求 UV OFF 并从 uv_service 快照确认
 * output_state == KNOWN_OFF；确认失败/超时返回明确错误并保留生命周期
 * 资源（不销毁 started/initialized），调用者可重试。成功后才清除状态。 */
esp_err_t uv_self_test_stop(void);

/* 受理一条 uv_self_test（由 app_protocol 回调调用，非阻塞，异步提交）。
 * *out_code: 成功 "UV_SELF_TEST_ACCEPTED" + *out_duration_ms=3000；
 *            失败 machine-readable 拒绝码 + duration=0。
 * 返回 ESP_OK 表示调用成功（含被拒），ESP_ERR_INVALID_STATE 表示未初始化。 */
esp_err_t uv_self_test_request(const char **out_code, uint32_t *out_duration_ms);

/* 紧急关灯（BLE 断线 / teardown / 急停冲突时调用）。立即 uv_service_emergency_stop。 */
esp_err_t uv_self_test_emergency(void);

/* 快照（同步 uv_service 状态推进自检状态机；无阻塞）。 */
esp_err_t uv_self_test_get_snapshot(uv_self_test_snapshot_t *out);

/* 供 status 消息：当前自检状态码（uv_self_test_state_t） */
uint8_t uv_self_test_state_code(void);

#ifdef __cplusplus
}
#endif
