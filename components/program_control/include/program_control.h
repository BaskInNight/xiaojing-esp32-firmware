/*
 * program_control.h — 程序控制 运行时服务公共 API
 *
 * 所有洗涤/急停输入（BTN1/BTN3/PAJ 手势/mini-app BLE）都经本服务仲裁后
 * 才接触 wash_executor。核心裁决逻辑在 program_control_core.c（纯 C，
 * host-testable）；本文件仅做运行时接线：
 *   - 持有 pc_arbiter_t + 手势状态（单线程控制平面）；
 *   - request/gesture/btn1 入口 → 纯函数裁决 → 执行动作（提交/取消/中止程序）；
 *   - 每拍 refresh() 从 executor 快照回填外部真实；
 *   - 审计 sink：executor 内部审计步骤通过注入的 audit provider 读取服务
 *     快照，再经纯谓词判定（fail-closed）。
 *
 * 安全边界：本服务不直接操作 HAL/GPIO。急停动作由 app_bootstrap 注入的
 * emergency 回调（内部走 safety_manager）执行；语音后端切换由注入的
 * backend 回调执行。未注入时相应动作返回 ESP_ERR_NOT_SUPPORTED（fail-closed）。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "machine_types.h"
#include "wash_executor.h"
#include "machine_config.h"
#include "xiaojing_hal.h"
#include "program_control_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 注入回调 ---- */

/* 急停：app_bootstrap 注入，内部执行 safety_manager 急停闩锁 + 全输出关。 */
typedef esp_err_t (*program_control_emergency_fn)(void *ctx);

/* 语音后端切换：app_bootstrap 注入。 */
typedef esp_err_t (*program_control_backend_fn)(void *ctx);

/* 审计输入 provider：填 pc_output_audit_t（从各服务快照 + safety_manager
 * 读取）。返回 ESP_OK 表示已填充；否则审计按失败处理（fail-closed）。 */
typedef esp_err_t (*program_control_audit_provider_fn)(
    pc_output_audit_t *out, void *ctx);

/* ---- 服务配置 ---- */

typedef struct {
    wash_executor_t *executor;              /* 提交/取消/中止程序 */
    const machine_config_t *(*get_config)(void);  /* NULL 用编译默认 */
    bool (*is_voice_active)(void);          /* BTN1 长按门禁；NULL 视为 false */
    uint32_t gesture_cooldown_ms;           /* 0 → 1000ms 默认 */
    program_control_emergency_fn emergency;       /* 可 NULL */
    void *emergency_ctx;
    program_control_backend_fn backend;           /* 可 NULL */
    void *backend_ctx;
    program_control_audit_provider_fn audit_provider; /* 可 NULL（审计将失败） */
    void *audit_ctx;
} program_control_service_config_t;

/* ---- 生命周期（仅 app_bootstrap 调用） ---- */

esp_err_t program_control_global_init(void);
esp_err_t program_control_service_init(const program_control_service_config_t *config);
esp_err_t program_control_service_start(void);
esp_err_t program_control_service_stop(void);

/* ---- 输入入口（单线程控制平面） ---- */

/* 通用请求（含优先级仲裁）。返回执行结果；若被仲裁拒绝则返回
 * ESP_OK + *out_action=PC_ACTION_NONE（非错误）。 */
esp_err_t program_control_request(pc_request_t req, pc_action_t *out_action);

/* 手势入口（UP→START / DOWN→STOP / 左右→翻页）。翻页动作返回
 * PC_GESTURE_ACTION_PAGE_*，由调用方执行显示翻页。 */
esp_err_t program_control_gesture(hal_gesture_t gesture,
                                  pc_gesture_action_t *out_action);

/* BTN1 入口（短按→START / 长按→后端切换）。 */
esp_err_t program_control_btn1(pc_btn1_event_t ev, pc_btn1_action_t *out_action);

/* 每拍（定时器/任务）调用：从 executor 快照回填外部真实并刷新闩锁。 */
esp_err_t program_control_refresh(void);

/* 当前仲裁状态（诊断/状态页用）。 */
pc_state_t program_control_get_state(void);

/* 当前是否有程序在运行/保留（供 motor_bench 等反向互斥查询）。 */
bool program_control_program_active(void);

/* 最近一次成功受理 START 分配的 program_id（0=从未受理）。 */
uint32_t program_control_last_program_id(void);

/* 设置/清除外部 busy（诊断组件持有 executor 时，禁止 START）。 */
void program_control_set_external_busy(bool busy);

/* ---- executor 审计 sink（程序内部审计步骤回调） ---- */

bool program_control_executor_audit(wash_step_type_t step_type, void *ctx);

#ifdef __cplusplus
}
#endif
