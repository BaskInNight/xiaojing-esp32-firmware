#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "safety_contract.h"
#include "xiaojing_hal.h"
#include "machine_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * safety_manager.h — 安全授权网关
 * 所有 MCP 安全输出的唯一业务写入口
 * 负责联锁判断、故障锁存、急停、stop hook 调度
 *
 * 依赖方向:
 *   safety_manager -> L0 contract, HAL, status store, config
 *   position/water/dry -> safety_manager (公共接口)
 *   safety_manager -X-> position/water/dry (反向通过 stop hook)
 *
 * Lifecycle: UNINITIALIZED -> INITIALIZED -> RUNNING -> STOPPED/FAILED
 * ================================================================ */

/* ---- Lifecycle State ---- */

typedef enum {
    SAFETY_MGR_UNINITIALIZED = 0,
    SAFETY_MGR_INITIALIZED,
    SAFETY_MGR_RUNNING,
    SAFETY_MGR_STOPPING,
    SAFETY_MGR_STOPPED,
    SAFETY_MGR_FAILED,
} safety_mgr_state_t;

/* ---- Stop Hook Registration ---- */

#define SAFETY_MAX_STOP_HOOKS  8

typedef struct {
    safety_stop_hook_t fn;
    void *context;
    const char *name;           /* 调试标签，不拥有 */
} safety_stop_hook_entry_t;

/* ---- Configuration ---- */

typedef struct {
    const xiaojing_hal_t *hal;
    const machine_config_t *config;
    xiaojing_output_mode_t output_mode;     /* 从 board_config 传入 */
    bool ptc_enabled;                       /* 从 board_config 传入 */
    bool hot_air_module_enabled;            /* 耦合热风模块（同一 GPA7 继电器） */
    uint32_t hot_air_cooldown_ms;           /* 停止后冷却锁定 ms，0=不锁定 */
    bool board_identity_confirmed;          /* 从 board_config 传入 */
} safety_manager_config_t;

/* ---- Lifecycle ---- */

/* 初始化安全管理器。必须在 HAL 创建后调用。
 * 串行化：不得并发调用 init/start/stop。 */
esp_err_t safety_manager_init(const safety_manager_config_t *config);

/* 启动安全管理器，进入 RUNNING 状态。
 * 初始化失败自动清理。 */
esp_err_t safety_manager_start(void);

/* 停止安全管理器。幂等。
 * 1. 进入 STOPPING
 * 2. 调用已注册 stop hooks
 * 3. emergency_shutdown 兜底
 * 4. 释放资源，进入 STOPPED */
esp_err_t safety_manager_stop(void);

/* 获取当前生命周期状态 */
safety_mgr_state_t safety_manager_get_state(void);

/* ---- Stop Hook ---- */

/* 注册非阻塞停机回调。RUNNING 之前或期间均可。
 * 返回 ESP_ERR_NO_MEM 超过上限。 */
esp_err_t safety_manager_register_stop_hook(safety_stop_hook_t fn,
                                             void *context,
                                             const char *name);

/* 注销停机回调。按 fn+context 匹配。
 * 并发安全：hook 正在执行时不释放资源。 */
esp_err_t safety_manager_unregister_stop_hook(safety_stop_hook_t fn,
                                               void *context);

/* ---- Inputs Update (分类型，各组独立，不覆盖其他组) ---- */

/* 更新姿态快照。原子更新 position/stable/moving/sample_valid/sample_ms。
 * position_service 调用。 */
esp_err_t safety_manager_update_position(drum_position_t position,
                                          bool stable,
                                          bool motor_moving,
                                          bool sample_valid,
                                          uint32_t sample_ms);

/* 更新 SHT 快照。app_main 调用。 */
esp_err_t safety_manager_update_sht(const sht_sample_t *sample,
                                     bool valid, bool fresh);

/* 更新水位快照。app_main 调用。 */
esp_err_t safety_manager_update_water(bool full, bool valid);

/* 更新风扇快照。app_main 调用。 */
esp_err_t safety_manager_update_fan(uint8_t percent, bool running,
                                     int64_t running_since_ms);

/* 更新当前时间。可选，manager 内部也会从 HAL 获取。 */
esp_err_t safety_manager_update_time(int64_t now_ms);

/* ---- Check (查询，无副作用) ---- */

/* 联锁判断。不修改任何状态，不调用 HAL。
 * 仅当 manager 处于 RUNNING 状态时有效。 */
esp_err_t safety_manager_check(const safety_request_t *request,
                                safety_decision_t *out_decision);

/* ---- Active Output Validation (已开启输出的持续安全检查) ---- */

/* 检查已开启的输出是否应保持开启。
 * 不要求所有 MCP KNOWN_OFF（区别于 check）。
 * 检查：fault/emergency、姿态 fresh+stable+正确、目标输出 KNOWN_ON、
 * 互斥输出 KNOWN_OFF、电机未运动。
 * 返回 ESP_OK 表示调用成功，allowed=true 表示应保持开启。 */
esp_err_t safety_manager_validate_active(safety_operation_t operation,
                                          drum_position_t required_position,
                                          bool *allowed,
                                          safety_reject_reason_t *reason);

/* ---- Apply (检查 + 执行，原子) ---- */

/* 原子联锁检查 + HAL 执行。
 * OFF 请求在 fault/emergency 下仍允许。
 * ON 请求在 fault/emergency 下拒绝。
 * MCP 写失败锁存 FAULT_MCP_IO。
 * 返回 ESP_OK 表示操作已执行，ESP_ERR_INVALID_STATE 表示拒绝。 */
esp_err_t safety_manager_apply(const safety_request_t *request);

/* ---- Fault Management ---- */

/* 报告故障。锁存，不自动清除。
 * 线程安全。 */
esp_err_t safety_manager_report_fault(machine_fault_code_t code,
                                       fault_severity_t severity,
                                       uint32_t detail);

/* 请求清除故障。clear 前重新检查危险条件。
 * 过温/SHT无效/MCP状态未知/输出未关闭 → 拒绝。
 * 成功后 revision 变化。 */
esp_err_t safety_manager_request_fault_clear(void);

/* ---- Emergency Stop ---- */

/* 有序、幂等、best-effort 急停。
 * 1. 原子锁存 emergency gate
 * 2. PTC OFF
 * 3. BL50 STOP
 * 4. MCP 全关
 * 5. IBT-2 STOP
 * 6. 调用 stop hooks
 * 7. HAL emergency_shutdown 兜底
 * 可安全并发调用，不死锁。 */
esp_err_t safety_manager_emergency_stop(void);

/* ---- Snapshot ---- */

/* 获取安全状态快照。线程安全。 */
esp_err_t safety_manager_get_snapshot(safety_snapshot_t *out);

/* ---- Position View (只读姿态视图) ---- */

typedef struct {
    drum_position_t position;
    bool sample_valid;
    bool stable;
    bool motor_moving;
    bool fresh;
    uint32_t sample_ms;
    int64_t now_ms;
} safety_position_view_t;

/* 获取当前姿态视图（同一锁域复制，fresh 使用 position_stale_timeout_ms）。
 * 线程安全。读取失败时 out 清零（position=UNKNOWN, valid=false）。 */
esp_err_t safety_manager_get_position_view(safety_position_view_t *out);

/* ---- Test Helpers ---- */

#ifdef SAFETY_MANAGER_TEST_HOOKS

/* 直接设置内部输入（仅测试用） */
void safety_manager_test_set_inputs(const safety_inputs_t *inputs);

/* 注入 BL50 状态（仅测试用，绕过 apply_bl50） */
void safety_manager_test_set_bl50_state(bool state_known, bool running);

/* 获取内部输入快照（仅测试用） */
void safety_manager_test_get_inputs(safety_inputs_t *out);

/* 重置到 UNINITIALIZED（仅测试 tearDown 用） */
void safety_manager_test_reset(void);

/* 获取当前注册的 stop hook 数量（仅测试用） */
uint8_t safety_manager_test_get_hook_count(void);

/* 注入下一次 unregister 失败：匹配 fn+context 的 unregister 返回 error。
 * 一次性：消耗后自动清除。safety_manager_test_reset() 清理。 */
void safety_manager_test_fail_next_unregister(esp_err_t error,
                                               safety_stop_hook_t match_fn,
                                               void *match_ctx);

/* 清除 pending unregister 注入（不清理运行时） */
void safety_manager_test_clear_unregister_injection(void);

#endif /* SAFETY_MANAGER_TEST_HOOKS */

#ifdef __cplusplus
}
#endif
