# motor_bench — 电机空载台架（诊断模式专用）

**电机空载台架测试**：在诊断模式下验证主洗涤（波轮）与滚筒（BLDC）电机
能否以低能量短脉冲正常起转/制动。**绝不自动让电机通电** —— 每步仅在用户手动
摆放桶位到生产契约位置并显式确认后，才提交一次 ≤300ms 低档脉冲。

## 组成

| 文件 | 职责 |
|---|---|
| `motor_bench_core.c` / `.h` | 纯 C 门禁 + 多步 FSM（无 FreeRTOS/HAL 依赖，host-testable） |
| `motor_bench_service.c` / `.h` | FreeRTOS 运行时组件：快照采集、tick 驱动、脉冲提交、急停、生命周期 |

## 三种能力

| 类型 | 步骤 | 动作序列（生产契约派生，非硬编码） |
|---|---|---|
| `pulsator_motor_self_test` | 1 | 主洗涤/波轮：`PULSATOR`（0°） |
| `drum_motor_self_test` | 1 | 滚筒/BLDC：`DRUM`（90°） |
| `motor_hall_bench_sequence` | 3 | 人工霍尔流程：`PULSATOR`(0°) → `DRUM`(90°) → `SPIN`(180°) |

所需桶位全部由生产契约 `bl50_service_action_to_position()` 派生（HOME→45° /
PULSATOR→0° / DRUM→90° / SPIN→180°），**禁止硬编码猜测霍尔顺序**。

## 安全模型

- **通电路径唯一**：`bl50_service_run_async()`（真实生产动作适配器）→
  `safety_manager` → HAL。`duration_ms=300`，`target_pwm_percent=15`（低档）。
- IBT-2 位置电机（0/45/90/180/270）**从不由此组件驱动**；位置由用户手动摆放，
  五路霍尔读取校验。
- 门禁（fail-closed）：executor 空闲、无 fault/emergency、位置有效/新鲜/稳定、
  位置匹配当前步、BL50 IDLE+PWM=0、位置服务空闲、禁止输出全 OFF
  （水阀/转移/排水/蠕动/UV/热风）、服务快照全部可用。
- 每 tick 复核禁止输出全 OFF；违反 → `FAULT FORBIDDEN_OUTPUT_ON` + 全关。
- 错误霍尔/多霍尔矛盾/位置超时/快照不可用/输出确认失败 → fail-closed，不推进。
- start/cancel/timeout/exception/BLE 断线/emergency → 全部输出 OFF。
- 非 COMPLETE 终态一律不声明 `output_confirmed_off`（fail-closed 报告）。

## 脉冲时序（诚实披露）

`bl50_fsm` 生产时序：VALIDATING → RAMPING_UP(15%，300ms) → RUNNING_CW(300ms)
→ BRAKING(500ms) → STOPPING → COMPLETE → IDLE。BL50 从非 IDLE 到回 IDLE
全程约 1.1s，但**能量始终为低档 15% PWM**。FSM 的 `pulse_seen_running` +
`MOTOR_BENCH_PULSE_ACCEPT_GRACE_MS` 防止把受理宽限误判为提交失败。

## Host 测试

`motor_bench_core.c` 为纯逻辑，直接编入 host 测试（见 `host_tests/test_motor_bench.c`）。
