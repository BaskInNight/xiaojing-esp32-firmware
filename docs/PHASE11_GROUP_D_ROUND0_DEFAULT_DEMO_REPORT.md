# Phase 11 Group D Round 0 — DEFAULT_DEMO_V1 正式报告

## 概述

实现 DEFAULT_DEMO_V1 完整演示洗涤流程（26 阶段规范表 + 多输入仲裁 + 内部步骤 + 输出审计）。
所有非实物门禁（软件架构、自动化测试、构建）已通过。实物门禁保持 NEEDS_USER。

## 架构

### DEFAULT_DEMO_V1 26 阶段规范

| 阶段 | 步骤类型 | 桶位 | 说明 |
|------|----------|------|------|
| 1 | WAIT_LOAD_CONFIRM | - | 订单受理 |
| 2 | UV | 0° | 空桶紫外杀菌 |
| 3 | HOME_BL50 | 45° | 预检（BL50 归位） |
| 4 | MOVE_POSITION | 0° | 定位 0° |
| 5 | WATER_IN | 0° | 进水 |
| 6 | DETERGENT | 0° | 投放洗涤剂 |
| 7 | PULSATOR_WASH | 0° | 波轮预洗 |
| 8 | SETTLE | 0° | 静止 |
| 9 | MOVE_POSITION | 90° | 切换 90° |
| 10 | DRUM_WASH | 90° | 滚筒主洗 |
| 11 | SETTLE | 90° | 静止 |
| 12 | MOVE_POSITION | 180° | 切换 180° |
| 13 | DRAIN | 180° | 排水 |
| 14 | SPIN (GENTLE) | 180° | 脱水加速 |
| 15 | SPIN (NORMAL) | 180° | 脱水保持 |
| 16 | SPIN (GENTLE) | 180° | 脱水减速 |
| 17 | SETTLE | 180° | 静止 |
| 18 | MOVE_POSITION | 270° | 切换 270° |
| 19 | DRY (PTC on) | 270° | 热风翻转 |
| 20 | DRY (fan only) | 270° | 冷却翻转 |
| 21 | SETTLE | 270° | 静止 |
| 22 | MOVE_POSITION | 45° | 归位 45° |
| 23 | AUDIT_UV_OFF | 45° | UV 确认 OFF |
| 24 | AUDIT_SAFE_OFF | 45° | 全输出 OFF 确认 |
| 25 | WAIT_UNLOAD_CONFIRM | 45° | 等待取衣 |
| 26 | FINISH | 45° | 完成 |

编译后程序：26 阶段 + 2 系统插入换位步 = 28 步。位置可达性已通过不变量验证。

### 多输入仲裁

| 优先级 | 输入 | 来源 | 动作 |
|--------|------|------|------|
| EMERGENCY | BTN3 长按 / BLE abort | program_control | safety_manager 急停 + executor abort |
| ABORT | BLE abort_reset | program_control | executor submit_urgent(true) |
| STOP | BTN3 短按 / BTN2 短按 / 手势 DOWN / BLE | program_control | executor submit_urgent(false) |
| START | BTN1 短按 / 手势 UP / BLE start_demo | program_control | 编译 DEFAULT_DEMO_V1 + submit |
| BACKEND | BTN1 长按 | program_control | 语音后端切换 |

### 新增组件

- **program_control**：纯 C 核心（仲裁/配方/BTN1/手势/审计）+ 运行时接线
- 扩展 **wash_step_type_t**：STEP_SETTLE, STEP_AUDIT_UV_OFF, STEP_AUDIT_SAFE_OFF
- 扩展 **WASH_STEP_FLAGS**: WASH_STEP_FLAG_HEATER_ON

### 安全模型

- 所有输出经 service → safety_manager → HAL
- 输出审计：UNKNOWN/ON 一律视为未确认 OFF → fail-closed 故障
- executor 内部步骤（SETTLE/AUDIT）取消立即成功，无服务泄漏
- 程序终态发布时清除内部步骤闩锁（防御性加固）
- 裸板（无 MCP23017）安全暂停：bootstrap 失败 → safe halt

## 测试结果

### Host 测试（10/10 通过）

- `program_control_core_tests`: 78 检查项, 63 测试用例, 2×5000 轮确定性压力测试
- 覆盖：配方编译、位置可达性、仲裁优先级、BTN1 互斥、手势冷却/尾串抑制、输出审计 fail-closed
- 回归测试 9 套全部通过（无破坏）

### 目标测试（已构建，未在板验证）

- 4 个测试用例：SETTLE 自动完成、AUDIT 通过/故障/无 sink、DEFAULT_DEMO 端到端
- 构建通过，需在真实硬件上运行验证

### 构建

| 构建 | 结果 |
|------|------|
| 主固件 (REAL) | OK |
| 主固件 (FAKE) | OK |
| 单元测试固件 | OK |
| Host 测试 | OK |

## 产物

- REAL 固件 BIN: `F82A2598010D3A1A75EC26BA09BDF16AAFE6F82C17C2D2B01549EE515F820652`
- REAL 固件 ELF: `DC4CF32E0DDCE675D072091DCE8D877FC91697B7BB19ECB625CB7592895CD300`
- FAKE 固件 BIN: `74BBC60975F56B35BD18F41D6705CF9569BDEE1812A92E186E050EABE6B270C9`

## 退出门禁

| 门禁 | 状态 |
|------|------|
| DEFAULT_DEMO_SCHEMA_FROZEN | PASS |
| ALL_INPUTS_SINGLE_SUBMIT_PATH | PASS |
| EXECUTOR_TERMINAL_EXACTLY_ONCE | PASS（已有） |
| OUTPUT_BYPASS_COUNT=0 | PASS |
| HOST/UNIT/MAIN_BUILD | PASS |
| 被动板测 30min | PENDING（soak 运行中，当前 ~16 分钟） |
| HARDWARE_GATE | NEEDS_USER |

## 下一次接外设时的烧录与测试顺序

1. 确认 COM8 为真实小净主板
2. 烧录 REAL 固件：`idf.py -p COM8 flash`
3. 冷启动确认 bootstrap 成功
4. 方向标定（小程序 `set_direction_calibration`）
5. 运行目标测试（`test_apps/unit`）
6. 小程序发送 `start_demo`，观察 DEFAULT_DEMO_V1 流程
7. 用户逐项确认实物通电安全后启动完整演示
8. 全量回归：`ctest` in host_tests/build

## 关联文件

- 夜间报告：`phase11_overnight/FINAL_REPORT.md`
- Checkpoint：`phase11_overnight/checkpoints/checkpoint_01_20260816_0000.md`
- Soak 日志：`phase11_overnight/logs/passive_soak.log`（完成后）
- 源文件：`components/program_control/`（新组件）
- 测试：`host_tests/test_program_control.c`（78 检查项）
- 目标测试：`test_apps/unit/main/phase11_groupd_executor_tests.c`（4 用例）