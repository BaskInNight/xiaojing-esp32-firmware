# Phase 6 Group A: dry_service 架构差距审计

状态：审计完成，实现前记录。

## 1. safety_manager FAN/PTC 完整性

### SAFETY_OP_FAN

**当前状态**：仅检查 `percent <= 100`，不检查 position/fault/emergency。

**差距**：
- 无 required_position 检查（dry_service 启动 FAN 时需 270°）
- 无 fault/emergency 全局拦截

**修复决策**：在 SAFETY_OP_FAN case 中增加 fault/emergency 拦截，以及 required_position 检查（当 request->required_position != DRUM_POS_UNKNOWN 时）。原因：安全管理层应统一拦截全局危险状态，不依赖每个 service 自行检查。

**回归范围**：仅影响 SAFETY_OP_FAN ON 路径。OFF 路径不受影响。已有测试中 FAN ON 不设置 required_position，不会触发新检查。

### SAFETY_OP_PTC_HEATER

**当前状态**：已完整覆盖以下联锁：
- ptc_enabled config gate ✓
- output_mode == REAL gate ✓
- board_identity_confirmed gate ✓
- position fresh + known + 270° + stable ✓
- motor not moving ✓
- all MCP known off ✓
- SHT valid + fresh ✓
- fan running ✓
- fan pre-blow >= pre_fan_ms ✓
- temperature < heater_cutoff_c ✓
- continuous heating < heat_on_max_ms ✓

**差距**：无。PTC 联锁完全满足 dry_service 要求。

### safety_manager_apply() 成功/失败后的 shadow 可信度

**当前状态**：
- 成功：fan shadow 更新（fan_percent, fan_running, fan_running_since_ms）
- 成功：PTC shadow 更新（ptc_on, ptc_on_since_ms, mcp_outputs.ptc_heater, mcp_known_mask）
- 失败：mark_mcp_unknown() 清除 known bit，latch FAULT_MCP_IO
- fan 失败：无明确 shadow 未知标记

**差距**：fan apply 失败时无 shadow 未知标记。但 dry_service 不依赖 fan shadow（直接通过 safety_manager 的 fan_running 查询），风险低。

## 2. 270° / 姿态 freshness / fan pre-blow / SHT validity / 55°C cutoff / PTC config gate

| 检查项 | 安全管理层 | dry_service 层 |
|--------|-----------|---------------|
| 270° position | PTC ON check 中完整 ✓ | VALIDATING 中检查 |
| position freshness | check_position_fresh_and_known() ✓ | 每 tick 检查 |
| position stable | PTC ON check 中检查 ✓ | 每 tick 检查 |
| motor not moving | PTC ON check 中检查 ✓ | 每 tick 检查 |
| fan pre-blow | PTC ON check 中检查 ✓ | PRE_FAN 状态计时 |
| SHT valid/fresh | PTC ON check 中检查 ✓ | HEATING 每 tick 检查 |
| 55°C cutoff | PTC ON check + ptc_force_shutdown ✓ | HEATING 每 tick 检查 |
| PTC config gate | ptc_enabled + REAL + board_confirmed ✓ | VALIDATING 检查 |
| heat_on_max_ms | PTC ON check + ptc_force_shutdown ✓ | HEATING 计时 |
| heat_off_min_ms | NOT in safety_manager | HEAT_REST 状态管理 |
| heater_resume_c | NOT in safety_manager | HEAT_REST 状态管理 |
| cooldown_ms | NOT in safety_manager | COOLING 状态管理 |
| max_total_ms | NOT in safety_manager | VALIDATING + 总计时 |

**结论**：heat_off_min_ms、heater_resume_c、cooldown_ms、max_total_ms 是业务逻辑约束，由 dry_service FSM 管理，不属于硬件安全联锁。

## 3. fake_hal 能力审计

| 需求 | 支持情况 |
|------|---------|
| SHT 样本注入 | ✓ fake_hal_set_sht() |
| SHT I/O 失败注入 | ✓ faults.sht_error |
| Fan 输出历史 | ✓ FAKE_OUTPUT_SET_FAN |
| PTC 输出历史 | ✓ FAKE_OUTPUT_SET_SAFE (SAFE_OUTPUT_PTC_HEATER) |
| now_ms 虚拟时间 | ✓ fake_hal_set_time() / fake_hal_advance_time() |
| MCP write 失败注入 | ✓ faults.mcp_write_error |

**差距**：无。

## 4. machine_config.dry 参数

当前参数与校验范围：

| 参数 | 默认值 | 校验范围 | 满足需求 |
|------|--------|---------|---------|
| max_total_ms | 1,800,000 (30min) | 60,000..3,600,000 | ✓ |
| pre_fan_ms | 5,000 | 1,000..60,000 | ✓ |
| heat_on_max_ms | 60,000 | 5,000..300,000 | ✓ |
| heat_off_min_ms | 60,000 | 5,000..300,000 | ✓ |
| cooldown_ms | 60,000 | 5,000..300,000 | ✓ |
| heater_cutoff_c | 55.0 | 40.0..80.0 | ✓ |
| heater_resume_c | 45.0 | 30.0..<cutoff | ✓ |
| fan_percent | 80 | 0..100 | ✓ |
| sht_stale_timeout_ms | 10,000 | 1,000..60,000 | ✓ |

**差距**：无。所有 dry 参数已定义且范围正确。

## 5. app_bootstrap 启动/停止顺序

**当前顺序**：
```
HAL → safety_manager → position_service → water_service → event_consumer
```

**建议添加**：
```
HAL → safety_manager → position_service → water_service → dry_service → event_consumer
```

dry_service 依赖：HAL、safety_manager、position_service（位置查询）、machine_config。

停止顺序（逆序）：
```
dry_service → water_service → position_service → safety_manager → consumer → HAL
```

dry_service 必须在 safety_manager/HAL 之前停止。

## 6. machine_event_t 表达能力

| 事件 | 当前支持 |
|------|---------|
| DRY_DONE (正常完成) | MACHINE_EVENT_DRY_DONE + SERVICE_RESULT_OK ✓ |
| DRY_DONE (取消) | MACHINE_EVENT_DRY_DONE + SERVICE_RESULT_CANCELED ✓ |
| DRY_DONE (故障) | MACHINE_EVENT_DRY_DONE + SERVICE_RESULT_FAULT ✓ |
| 紧急停止 | SERVICE_RESULT_CANCELED 或 SERVICE_RESULT_FAULT ✓ |

**差距**：无。

## 7. Phase 5 已验证模式复用评估

| 模式 | 可复用 | 注意事项 |
|------|--------|---------|
| lifecycle mutex + states | ✓ | 直接复制 |
| _Atomic task_handle | ✓ | 直接复制 |
| task generation counter | ✓ | 直接复制 |
| request reservation (atomic) | ✓ | 直接复制 |
| terminal_pending + retry | ✓ | 直接复制 |
| stop hook register/unregister | ✓ | 直接复制 |
| start() linearization point | ✓ | 直接复制 |
| action-pending FSM 协议 | ✓ | 适配为 fan/heater |
| cmd_queue + notify bits | ✓ | 直接复制 |
| snapshot via mutex | ✓ | 直接复制 |
| commit_result | 部分 | dry 只有 fan+heater，无 valve |

**不能机械复制的**：
- valve shadow 不适用（dry 无 valve，用 fan/heater shadow）
- drain model 不适用（dry 无 drain 阶段）
- drum_full check 不适用

## 8. 安全缺口修复清单

### 缺口 1: SAFETY_OP_FAN 无 fault/emergency 拦截

**修复**：在 SAFETY_OP_FAN ON 路径增加 fault_active / emergency_stop_active 检查。
**原因**：故障/急停状态不应允许任何新输出开启。
**回归**：现有 FAN 测试不设置 fault/emergency，不受影响。

### 缺口 2: SAFETY_OP_FAN 无 required_position 检查

**修复**：当 request->required_position != DRUM_POS_UNKNOWN 时，检查 position freshness + match。
**原因**：dry_service 需要 FAN 仅在 270° 时开启。
**回归**：现有 FAN 测试不设置 required_position（默认 UNKNOWN），不会触发。

## 9. 结论

架构差距主要在 SAFETY_OP_FAN 的 fault/emergency/position 拦截。PTC 联锁完整。fake_hal 能力充分。config 参数就绪。bootstrap 需要添加 dry_service 步骤。
