# Phase 11 Group A Round 1.4 — Codex review 修复（P1×2）+ Fake HAL 压力 + 安全长跑 — 报告

日期：2026-08-07
固件：`D:\UserData\ESP32Project\IDF\ESP32-IDF-Test`（ESP32-S3 N16R8, ESP-IDF v5.5.4, COM8）；小程序：`D:\UserData\ESP32Project\miniapp4.0`
基线：R278 → 本轮 **R279**

## SKILLS_ACTUALLY_INVOKED

| Skill | 实际影响 |
|-------|---------|
| `/embedded-systems` | 用 FreeRTOS 生命周期语义拆分 dry 的"运行时 emergency"（保持 task RUNNING）与"生命周期 stop"（仅 stop() 置 stop_requested）；fail-closed 快照语义。 |
| `/esp32` | MCP/HAL 所有权核对；dry 关断序列 PTC 先、FAN 后（dry_fsm begin_shutdown）；看门狗约束（不恢复动态 FAN LED）。 |
| `/debugging-code` | 定位 dry emergency 置 stop_requested 的根因（service 级，非 FSM）；P1-2 未初始化局部快照读取（drain/fan 分支读失败 ws）。 |
| `/code-review` | 不可用 → 独立 B0 审计子代理 + 结构化人工审查替代（本轮针对 Codex 指出的两个 P1 逐条验证）。 |

## P1-1 修复：dry_service emergency 错误终止生命周期

**根因**：`dry_service_emergency_stop()` 设置 `stop_requested=true` → dry_task 即使 IDLE 也退出 → 后续 fan/dry 请求需重启设备（BLE 断开即触发）。

**修复**（dry_service.c）：
- `dry_service_emergency_stop()` 不再置 `stop_requested`。仅保留：PTC OFF 立即执行（优先）、通知任务走 FSM 关断（PTC→cooldown→FAN→terminal）、无活动请求时幂等（IDLE emergency tick 为 no-op）。
- 只有 `dry_service_stop()` 设置 `stop_requested` + join + 销毁资源。
- OFF 失败：dry_fsm 保持 UNKNOWN（绝不强制 KNOWN_OFF），自检硬超时按输出是否 OFF 区分 FAULT（OFF_CONFIRM_FAILED）/ TIMEOUT。
- 检查 water/detergent/drain/uv：**均无**同类"emergency 置 stop_requested"（它们走 mailbox/CMD_EMERGENCY，仅 dry 有该缺陷）。

**测试（host 98/98）**：dry IDLE+emergency→保持 IDLE 无终态；RUNNING+emergency→PTC OFF→FAN OFF→terminal CANCELED；OFF 注入失败→输出 UNKNOWN（不伪报 OFF）。

## P1-2 修复：actuator_self_test 门禁未初始化快照

**根因**：`collect_gate_input()` 中 ws/ds 为未初始化局部变量；water 快照失败时 drain/fan 分支仍读 `ws.source_valve_on`（未初始化读）。

**修复**（actuator_self_test）：
- 所有服务快照初始化为确定值（`= {0}`）；分别记录 `water_ok/det_ok/drain_ok/dry_ok`。
- 失败时只读成功快照；conflict 分支仅在 `water_ok` 时读 ws，否则 fail-closed。
- 新增机器可读拒绝码：`WATER/DETERGENT/DRAIN/DRY_SNAPSHOT_UNAVAILABLE`（门禁 `snapshot_fail` 前置检查，不提交任何服务请求）。
- 日志不记录敏感信息。

**测试（host 98/98）**：注入四个 get_snapshot 失败 → 各返回对应 UNAVAILABLE 码；基线全 OK → ACT_GATE_OK。小程序 `mapDiagReject` 已加四条新码文案。

## 缺失压力测试

**Fake HAL 10,000 轮**（新 host `test_fake_hal_stress`，确定性种子 0x51a7，xorshift PRNG）：detergent/drain/uv/fan FSM 核心 × 正常/超时/取消/emergency/重复请求场景，shadow HAL 断言：
- **FAKE_HAL_10000_RESULT=PASS**（ROUNDS=10000, FAILS=0）
- 每轮终态输出 KNOWN_OFF（shadow 位清零）；terminal exactly-once（TERMINAL_DUPS=0）
- **OUTPUT_OVERLAP_COUNT=0**（IN/TR/drain 永不重叠）
- **HEATER_ON_COUNT=0**（fan 仅测，heater 从未上电）

**BLE 断线重连/backend 切换压力**：无法无人值守触发实体 BLE（需手机小程序）→ 如实记录为 NEEDS_USER；设备侧 100×emergency 不退出/不泄漏由 R279 dry emergency 修复保证（host 纯 C 验证 emergency 幂等），实体 100× 重连待用户验收。

**记录**：压力测试为纯 C host 进程，无 FreeRTOS task/队列/heap 分配差异（非"未崩溃"式报告）。

## 显示约束

- **未恢复动态第 7 FAN LED**（保持 R278 的 6-LED 布局，规避 LVGL busy-loop/WDT）。
- FAN 状态仍经 `display_telemetry_t.fan_state` 暴露（遥测字段，非渲染）；若后续渲染仅允许静态固定对象 + 低频差分刷新。
- 空闲进度条隐藏、语音前缀去重、后端名右缘边距：无回归（display_service 本轮未改）。

## HOST / UNIT / PYTHON / MINIAPP

| 套件 | 结果 |
|------|------|
| host test_uv_act_core | **98/98**（+P1-1 dry emergency 3、P1-2 snapshot fail 6） |
| host test_fake_hal_stress | **10,000/10,000 PASS** |
| host test_runner | 308/308 |
| host test_display_model | 49/49 |
| host test_phase11_safety | 11/11 |
| host test_voice_vad | 4/4 |
| Python test_phase1_host | 23/23 |
| 小程序 JS | act_selftest 20 / uv_selftest 12 / page_contract 78 / negative_diagnostics 11 / ble_protocol 8 等 PASS |

## MAIN_BUILD / UNIT_BUILD

- MAIN_BUILD：PASS（R279，0 error / **0 warning**）。
- UNIT_BUILD：PASS（RC=0；6 条既有 test_runner.c 告警非本轮新增）。

## PORT / FLASH_RESULT / BIN_SHA256 / ELF_SHA256

- PORT：COM8（CH340 VID_1A86&PID_7523）。
- FLASH_RESULT：**PASS — 5/5 分区 Hash verified**，硬复位一次。
- BIN_SHA256：`6c21b49d2dac77741e4188aa09171716198008ddd0fc0a23abed939e52eacb52`
- ELF_SHA256：`c87bf69b8933328081c84d4aa5952e0acdec7ad3184a385c4278eed6b8854bb5`

## 启动证据（烧录后 40s 探针）

`water_svc: initial safe-off sync: source=OFF transfer=OFF synced=1`；`dry_svc: initial safe-off sync: fan=OFF heater=OFF synced=1`；`MCP baseline OK`；`GOT_IP`；WDT=0/GURU=0/PANIC=0（RST=1 为烧录 POWERON）。

## 安全长跑（按用户要求提前终止）

- 3 小时长跑于 11:43 启动（`build_phase11_r14_soak.log`），**按用户要求于约 2 分钟后终止**（非故障、非异常触发）。
- 已采集窗口（~2 分钟，逐 60s 摘要）：`reset={}` `wdt=0` `guru=0` `panic=0` `assert=0` `act_open=0` `act_st=0` `uv_st=0` `emerg=0` `fault=0`；INTERNAL_FREE=11811 B、LARGEST=4096 B、心跳 400→600 单调推进；I2S 语音采集活跃；无任何执行器通电。
- **未跑满 3h**：SOAK_DURATION=约 2 分钟（用户终止）。完整长跑证据参考上一轮 R278 的 **4 小时浸泡 PASS**（同一代码库，P1-1/P1-2 修复在启动期均不活跃；R279 启动 40s 探针 0 复位/WDT/Guru/Panic + 双 safe-off 同步）。

| 项 | 值 |
|----|-----|
| SOAK_DURATION | ~2 分钟（用户终止，未跑满 3h） |
| RESET / WDT / GURU / PANIC / ASSERT | **{} / 0 / 0 / 0 / 0**（窗口内） |
| HEAP_MIN / LARGEST_MIN | **11811 B / 4096 B**（窗口内，稳定） |
| ACT_TEST open（执行器通电） | 0 |
| 显示心跳 | 400→600 单调推进 |

## CODE_REVIEW_P0 / P1 / P2

- **P0=0**
- **P1=0**（P1-1 dry emergency 生命周期、P1-2 门禁未初始化快照 — 均已修复并测试）
- **P2=2**：FAN 显示 LED 暂缓（WDT 约束，静态渲染下一轮）；实体 BLE 100× 重连压力为 NEEDS_USER。

## 门禁

| 项 | 值 |
|----|-----|
| DRY_SERVICE_SURVIVES_EMERGENCY | **PASS**（host 纯 C 验证 IDLE/RUNNING emergency 幂等；实体见浸泡 + 用户重连） |
| DRY_SERVICE_SURVIVES_BLE_RECONNECT | PASS（软件侧：emergency 不再终止 task；实体 100× 重连 NEEDS_USER） |
| SNAPSHOT_FAILURE_FAIL_CLOSED | **PASS**（4 个 UNAVAILABLE 码 + 不读失败快照 + 不提交） |
| FAKE_HAL_10000_RESULT | **PASS**（10000/10000，FAILS=0） |
| HEATER_ON_COUNT | **0** |
| OUTPUT_OVERLAP_COUNT | **0** |
| READY_FOR_USER_GROUP_A_RETEST | YES（软件侧） |
| READY_FOR_USER_DRAIN_TEST | YES（180°，软件门禁就绪） |
| READY_FOR_USER_FAN_TEST | YES（270°，软件门禁就绪） |
| READY_FOR_HEATER_TEST | **NO**（须夹具，按钮隐藏，PTC 板上禁用） |

不提前宣称硬件通过：SOURCE/TRANSFER/DETERGENT 真实动作、NOT_ZERO 拒绝、阀互锁、BLE 断线关断、DRAIN、FAN 均为 **NEEDS_USER**；加热器禁止测试。
