#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "safety_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * safety_interlocks.h — 纯 C 确定性策略层
 * 不依赖 FreeRTOS、ESP Timer、I2C、MCP 或具体 service
 * 所有时间使用显式传入的 now_ms
 * 输入相同必须得到相同结果
 * ================================================================ */

/* ---- PTC Duration Limits (来自 machine_config 的安全配置) ---- */

typedef struct {
    float heater_cutoff_c;          /* 过温关断温度 */
    float heater_resume_c;          /* 重新允许温度 */
    uint32_t heat_on_max_ms;        /* 单次最大连续运行 */
    uint32_t pre_fan_ms;            /* 风扇预吹最少时间 */
    uint32_t sht_stale_timeout_ms;  /* SHT 样本过期时间 */
    uint32_t position_stale_timeout_ms; /* 姿态样本过期时间, 0=不检查 */
    xiaojing_output_mode_t output_mode;  /* 运行模式 */
    bool ptc_enabled;                    /* PTC 是否在配置中启用 */
    bool hot_air_module_enabled;         /* 耦合热风模块是否启用（同一 GPA7 继电器） */
    uint32_t hot_air_cooldown_ms;        /* 热风模块停止后的冷却锁定时间 */
    bool board_identity_confirmed;       /* 板身份是否已确认 */
} safety_interlock_config_t;

/* ---- 联锁判断 ---- */

/* 对单个安全请求执行全部联锁检查。
 * 纯函数：只读取 inputs 和 config，不修改任何状态。
 *
 * 返回:
 *   decision->allowed = true/false
 *   decision->reason  = 拒绝原因 (SAFETY_REJECT_NONE 当 allowed)
 *   decision->fault_code = 需要锁存的故障码 (FAULT_NONE 当无需锁存)
 *   decision->interlock_mask = 触发的联锁位掩码
 */
void safety_interlock_check(const safety_inputs_t *inputs,
                             const safety_interlock_config_t *config,
                             const safety_request_t *request,
                             safety_decision_t *decision);

/* ---- MCP Output Type Check ---- */

/* 返回 true 如果该操作对应 MCP 安全输出 */
bool safety_is_mcp_output_op(safety_operation_t op);

/* ---- MCP 输出互斥检查 ---- */

/* 检查请求的 MCP 输出与当前已开启的输出是否冲突。
 * 返回 true 表示存在冲突。 */
bool safety_mcp_output_conflict(const safety_mcp_output_state_t *current,
                                 safety_operation_t requested_op,
                                 bool requested_enable);

/* ---- PTC 关断条件检查 ---- */

/* 检查 PTC 是否应被强制关闭（过温/SHT失效等）。
 * 返回 true 表示需要强制关闭。 */
bool safety_ptc_force_shutdown(const safety_inputs_t *inputs,
                                const safety_interlock_config_t *config);

/* ---- MCP Output Known Helpers ---- */

/* 返回 true 如果该 MCP 输出在 known_mask 中已标记为已知 */
bool safety_mcp_output_is_known(uint32_t known_mask, safe_output_t output);

/* 返回 true 如果所有高风险 MCP 输出均为 KNOWN_OFF */
bool safety_all_mcp_known_off(const safety_mcp_output_state_t *mcp,
                               uint32_t known_mask);

/* ---- Allowed Operation Mask ---- */

/* 根据当前输入计算允许的操作掩码（使用 check 路径，不复制矩阵）。 */
uint64_t safety_compute_allowed_mask(const safety_inputs_t *inputs,
                                      const safety_interlock_config_t *config);

/* ---- Active Output Validation ---- */

/* 检查已开启的输出是否应保持开启。
 * 与 safety_interlock_check 不同：不要求所有 MCP KNOWN_OFF。
 * 只检查：
 *   - fault/emergency 状态
 *   - 姿态 fresh + stable + 正确位置
 *   - 目标输出为 KNOWN_ON
 *   - 互斥输出为 KNOWN_OFF（GPA0 vs GPA1 vs GPA3）
 *   - 电机未运动
 * 返回 true 表示应保持开启，false 表示应关闭。reason 输出拒绝原因。 */
bool safety_validate_active(const safety_inputs_t *inputs,
                             const safety_interlock_config_t *config,
                             safety_operation_t operation,
                             drum_position_t required_position,
                             safety_reject_reason_t *reason);

/* ---- Water Outputs Safe Check ---- */

/* 检查所有水路相关输出是否处于安全状态。
 * 要求全部已知且 OFF。 */
bool safety_water_outputs_safe(const safety_mcp_output_state_t *mcp,
                                uint32_t known_mask);

/* ---- Heater Safe Check ---- */

/* 检查加热器是否处于安全状态。
 * 要求 PTC 输出已知、OFF、无过温风险。 */
bool safety_heater_safe(const safety_mcp_output_state_t *mcp,
                         uint32_t known_mask,
                         const safety_inputs_t *inputs,
                         const safety_interlock_config_t *config);

#ifdef __cplusplus
}
#endif
