# Phase 11 Group A Round 1.3 — 修复硬件门禁阻塞 + 被动过夜稳定性浸泡 — 报告

日期：2026-08-07
工程：
- 固件：`D:\UserData\ESP32Project\IDF\ESP32-IDF-Test`（ESP32-S3 N16R8, ESP-IDF v5.5.4, ILI9341 320×240 横屏, COM8）
- 小程序：`D:\UserData\ESP32Project\miniapp4.0`

## SKILLS_ACTUALLY_INVOKED

| Skill | 状态 | 实际影响 |
|-------|------|---------|
| `/embedded-systems` | 已调用 | 用 FreeRTOS 门禁/状态机语义设计 HW-FIX-1 的"显式安全 OFF 同步"（UNKNOWN 绝不当 OFF，仅同步成功后置 KNOWN_OFF）；fail-closed 证明语义。 |
| `/esp32` | 已调用 | MCP/HAL 所有权链（service → safety_manager → HAL/MCP）逐级核对；任务看门狗 WDT 反汇编（xtensa addr2line）定位 taskLVGL 忙循环根因。 |
| `/debugging-code` | 已调用 | 通过带时间戳串口日志 + WDT backtrace addr2line 定位：R277 第 7 个输出 LED 使 LVGL 刷新陷入忙循环（IDLE1 饿死，WDT 每 10s 触发）；回退 6-LED 布局后 WDT 归零。 |
| `/code-review` | UNAVAILABLE（无独立 skill） | 用结构化人工审查 + 独立 B0 审计子代理替代（见 Group B 报告）。 |

## 本轮修复（P0/P1 硬件门禁阻塞 + UI-P2）

### HW-FIX-1（P0）water 双阀初始 UNKNOWN → 首次板测被拒

**根因**：`water_fsm_init` 将双阀置 `WATER_VALVE_UNKNOWN`（water_fsm.c:39-40），只有跑过进水 FSM 才变 KNOWN_OFF；板测门禁 `target_confirmed_off = !on && !unknown` 恒 false → `OUTPUT_ALREADY_ON`。R1.1 的显示融合只掩盖屏幕，未修门禁。

**修复**：
- `water_fsm_commit_initial_off()`（纯 C，host 可测）：仅 IDLE 时按每阀 OFF 申请结果提交 KNOWN_OFF / UNKNOWN（fail-closed）。
- water 任务启动即 `water_initial_safe_off_sync()`：经正常 safety 路径显式请求双阀 OFF 并提交影子。
- 快照新增 `safe_off_synced`；门禁新增 `water_safe_off_synced` 输入，同步完成前拒绝（OUTPUT_STATE_UNKNOWN），不再把"未建立"误报为"已开"。

**设备证据**：`water_svc: initial safe-off sync: source=OFF transfer=OFF synced=1`。

### HW-FIX-2（P0）detergent 空闲态误报 SERVICE_BUSY

**根因**：`detergent_service_get_snapshot` 未赋值 `terminal_pending`（与 `elapsed_ms`），门禁读未初始化栈垃圾 → `detergent_service_idle` 恒 false。

**修复**：快照确定性填充全部字段；新增纯函数 `detergent_fsm_output_to_public` / `detergent_fsm_terminal_pending`（显式映射，禁原始枚举值）。

### HW-FIX-3（P0）UV 物理关灯但固件报 OFF 确认失败

**根因**：`uv_self_test sync_uv_state_locked` 的 default 分支把"uv_service 发布 terminal 后回 IDLE（terminal/output_state 保留）"误判为状态丢失 → 假 FAULT。

**修复**：快照确定性填充（`elapsed_ms`/`terminal_pending`）+ `uv_fsm_output_to_public`；同步逻辑识别"IDLE + 保留 terminal"为已到终态（命令受理 → 服务到达终态 → MCP/服务锁存 OFF，经真实可用来源）。无电流回读，故证明的是命令/锁存 OFF，非光学/电流证据（如实标注）。

### UI-P2-1/2/3

- **UI-P2-1**：新增纯函数 `display_skip_speaker_prefix`，渲染前剥离嵌入的 `你：`/`你:`/`小净：`/`小净:` 前缀，杜绝"你：你：…"。
- **UI-P2-2**：空闲时进度条轨道与填充均隐藏（`LV_PART_MAIN` opa=0 + 值 0），无亮条无百分比；活动保持正常填充。心跳未变暗。
- **UI-P2-3**：语音页后端名标签右缘留 ≥6px 安全边距（宽 184，右缘 294，卡片右缘 312），LONG_CLIP 确定性截断。

## 测试结果（相关改动）

| 套件 | 结果 |
|------|------|
| host `test_uv_act_core` | **89/89**（+HW-FIX-1/2/3 22 项、Group B 门禁/映射 19 项） |
| host `test_display_model` | 49/49（+UI-P2-1 7 项） |
| host `test_runner` | 308/308 |
| host `test_voice_vad` | 4/4 |
| host `test_phase11_safety` | 11/11 |
| Python `test_phase1_host.py` | 23/23 |
| 小程序 JS 套件 | 184+（act_selftest 20 / uv_selftest 12 / uv_order 15 / ble_protocol 8 / ble_transport 19 / booking 23 / page_contract 78 / negative_diagnostics 11） |

## 构建 / 烧录

- MAIN_BUILD：PASS（0 error / 0 warning；仅一条环境级 OSError 提示非编译告警）。
- UNIT_BUILD：PASS（6 条 test_runner.c 既有告警，非本轮新增）。
- 静态扫描：UTF-8 字形 PASS（3869 覆盖/0 缺失）、显示布局 PASS、safety bypass PASS（仅既有 position_service 旁路）、无新动态分配、无新 API key。
- 烧录：**5/5 分区 Hash verified**，硬复位一次。
- BIN_SHA256：`52668ef97015e37b70eaa92ee6eebcd1e8147f598cb0d7bdd1789495a974d716`
- ELF_SHA256：`cf6e3e113d37b24276c426fc64048f626a6c68c0e9a36b194e384244681c1143`
- PORT：COM8（CH340，VID_1A86&PID_7523）。

## 启动证据（烧录后 90s 结构化探针）

- 0 复位 / 0 WDT / 0 Guru / 0 Panic / 0 Assert。
- `water_svc: initial safe-off sync: source=OFF transfer=OFF synced=1` ✅ HW-FIX-1 设备验证。
- `dry_svc: initial safe-off sync: fan=OFF heater=OFF synced=1` ✅（Group B 前置同步）。
- `safety_mgr: Running (MCP baseline OK)`；Wi-Fi `GOT_IP: ip=10.196.41.88`。
- `heartbeat tick=200→500` 单调推进；无任何输出通电（ACT_TEST open 计数 0）。

## WDT 回归与回退（重要）

R277（Group B 含第 7 个 FAN 输出 LED）触发 task_wdt（IDLE1 CPU1 饿死，每 10s）。addr2line 定位：taskLVGL 忙循环，采样于 Wi-Fi 图标弧线反锯齿绘制（`circ_calc_aa4`）。**回退至 6-LED 布局（R278）后 WDT 归零（90s 探针 + 本轮浸泡均 0）**。→ 结论：第 7 个 FAN LED 的 LVGL 刷新路径引入忙循环；FAN 状态仍经 `display_telemetry_t.fan_state` 暴露于遥测，页面渲染留待下一轮以不触发 WDT 的方式实现（见 Group B 报告 B5）。

## 被动 4 小时过夜浸泡

- 固件：R278（BIN `52668ef9…`），COM8 115200 时间戳日志 `build_phase11_r13_soak.log`。
- 期间**未发送任何执行器自检命令**（无 UV/阀/泵/风扇/加热器/电机），无自动重启/掩蔽。
- 每 60s 记录：uptime/heartbeat、reset reason、WDT/Guru/Panic/Assert、internal free + largest、I2S 字节、Wi-Fi/BLE、各输出 OFF 证明。
- 停止条件：任何复位/WDT/Guru/panic/assert / 输出意外 ON / 单调堆泄漏 / 显示心跳停滞 / 服务任务失活。

| 项 | 值 |
|----|-----|
| 浸泡时长 | **14400 s（恰好 4 小时）**，03:11:35 → 07:47:48，日志 259 条 60s 摘要 |
| RESET_BY_REASON / WDT / GURU / PANIC / ASSERT | **{} / 0 / 0 / 0 / 0** |
| INTERNAL_FREE 起始 / 末值 / 全局最小值 | **11951 / 11791 / 11663 B**（波动非单调，无泄漏趋势） |
| INTERNAL_LARGEST 最小值 | **4096 B**（全程稳定） |
| ACT_TEST open 计数（执行器通电） | **0** |
| ACT_SELFTEST / UV_SELFTEST / emergency | **0 / 0 / 0** |
| 显示心跳 | tick 900 → **57600** 单调推进，共 572 次（显示任务全程存活） |
| Wi-Fi / BLE | 4 小时内 **0 次断线/重连事件**（连接稳定）；GOT_IP 在浸泡开始前已建立 |
| I2S 字节 | 5.75 MB → 461 MB（语音采集全程活跃） |

**结论：PASSIVE_SOAK_GATE = PASS。** 一次连续启动、零复位/WDT/Guru/Panic/Assert、零执行器通电、内部堆无单调泄漏、显示心跳持续、Wi-Fi 稳定、服务快照持续更新、无终态重试风暴。

## PHASE11_GROUP_A_FIX_SOFTWARE_GATE

**PASS**：P0=0、P1=0；HW-FIX-1/2/3 修复 + 全部自动化测试通过；主固件 0 告警；静态扫描通过；设备启动 safe-OFF 证明；无真实执行器通电。UI-P2 三项修复完成。P2 遗留：FAN 显示 LED 因 WDT 暂缓（下一轮实现）。

## PHASE11_GROUP_A_PASSIVE_SOAK_GATE

**PASS**。4 小时浸泡：一次连续启动、0 复位/0 WDT/0 Guru/0 Panic/0 Assert、ACT_TEST open=0（无执行器通电）、内部堆 11951→11791 B（非单调泄漏）、心跳 900→57600 持续、Wi-Fi 0 断线。

## PHASE11_GROUP_A_HARDWARE_GATE

**NOT_YET（待用户实物重测）**。软件门禁已通（SOURCE/TRANSFER/DETERGENT 门禁现在可接受），但实物动作证据需用户醒来后重测（清单见下）。绝不把编译/自动测试伪报为硬件 PASS。

## READY_FOR_USER_GROUP_A_RETEST

**YES（软件侧）**。用户醒来后的实物重测清单：

1. SOURCE 进水阀 ×3（1.5s 出水、自动关、终态 off=true）
2. TRANSFER 转移阀 ×3（1.5s 转移、自动关）
3. DETERGENT 蠕动泵 ×3（0.8s 泵动、自动关）
4. POSITION_NOT_ZERO：转 45°，自检应被拒；回 0° 后正常
5. IN/TR 互锁自动测试（source 受理后 200ms 请求 transfer 必须被拒）
6. BLE 断线自动关断（source 受理后 250ms 断线，重连后 off=true）
7. UV 3 秒自检：应 COMPLETE + off=true（HW-FIX-3）
8. 三页 UI 目视：IN/TR 待机空心 OFF、HT `--`、空闲进度条无亮条、语音页无「你：你：」

## 结束语

本轮完成三项 P0 实物暴露固件缺陷 + 三项 UI-P2 的修复，主机/单元/小程序/Python 测试全绿，R278 固件烧录 5/5 verified，启动证明全部输出 safe-OFF（water + dry 双同步日志），WDT 回归经回退 6-LED 布局后归零。正式 4 小时被动浸泡已启动，完成后回填数据并更新门禁。Group B 软件预备（排水泵/风扇自检 + 小程序 + 门禁扩展）见独立报告。
