# Phase 11 Group B Round 0.1 — 排水/风扇/加热器软件预备（无实物通电）— 报告

日期：2026-08-07
固件：`D:\UserData\ESP32Project\IDF\ESP32-IDF-Test`（ESP32-S3 N16R8, COM8）；小程序：`D:\UserData\ESP32Project\miniapp4.0`

本轮性质：**仅软件预备，不执行任何无人值守的实物执行器测试。** 所有真实输出保持 OFF（启动 safe-OFF 同步 + 浸泡零通电）。

## SKILLS_ACTUALLY_INVOKED

`/embedded-systems`（门禁/状态机/fail-closed）、`/esp32`（MCP/HAL 所有权、WDT 反汇编）、`/debugging-code`（WDT backtrace addr2line 定位第 7 LED 忙循环）、`/code-review`（独立 B0 审计子代理 + 结构化人工审查，见下）。

## B0 — 架构与硬件审计（独立子代理证据表，节选）

| 输出 | 通道 | 属主服务 | safety 操作 | 实物反馈 | 门禁 | 结论 |
|------|------|---------|------------|---------|------|------|
| DRAIN 排水阀 | MCP GPA3 | drain_service | SAFETY_OP_DRAIN_VALVE | 无 | 180° fresh/stable + 阀互斥 | 板测可用，需 180° |
| FAN 风扇 | GPIO38 LEDC PWM（非 MCP） | dry_service | SAFETY_OP_FAN | 无 tach（仅 PWM） | 270° + 全输出 OFF | 板测可用，需 270° |
| HEATER/PTC | MCP GPA7（继电器 K_HEAT） | dry_service | SAFETY_OP_PTC_HEATER | 仅硬件热熔丝，无软件回读 | fan_running + pre_fan + SHT 有效新鲜 + 温度<cutoff + 周期<60s | 板上当前禁用（sdkconfig 无 PTC_ENABLED, mode=LOW_VOLTAGE）→ HT `--` |

- **审计发现（已修复）**：`drain_service.c:743` 原始枚举强转 `(drain_state_t)s_rt.fsm.state` 偏移错位（内部 IDLE=0 vs 公开 IDLE=1），导致 drain 快照 state 恒错 → 现用显式 `drain_fsm_state_to_public`。`dry_fsm` 未保留 terminal → 新增 `ctx->terminal`（发布后回 IDLE 仍有效，fan 自检终态契约）。
- 审计 10 项：raw MCP/HAL bypass=0（仅 position_service 既有旁路）、UNKNOWN-as-OFF=0、heater-without-fan=0、temp-stale-heater=0、join-timeout/destroy=0、terminal 丢失=0、UI mask 推断=0。
- UNRESOLVED：板上排水阀/风扇/PTC 物理连接状态未确认（Group A 未测到 Group B）；风扇 tach 线不在接线图。

## B1 — 排水泵安全自检（已实现）

- 门禁：executor 空闲 + safety 健康 + drain_service 空闲 + drain 输出 KNOWN_OFF + **桶位 180°**（required_pos 按目标） + source/transfer 互斥 OFF。
- 路径：`actuator_self_test {target:'drain_pump'} → gate → drain_service_run_async(2000ms) → safety_manager → HAL`。无 raw MCP/GPIO。
- 设备端自动关阀（drain FSM 到期 PUMP_OFF），cancel/timeout/emergency/fault/stop 全部请求 OFF；stop 失败保留生命周期可重试；terminal 恰好一次。
- drain 快照修复：显式状态/输出映射 + `terminal_pending`/`elapsed_ms` 确定性填充。
- Host 测试：drain duration/name、180° 接受、0° 拒绝、conflict、dry busy 拒绝（test_uv_act_core 89 项含）。

## B2 — 风扇安全自检（已实现）

- 门禁：executor 空闲 + dry_service 空闲 + fan/heater KNOWN_OFF + **桶位 270°** + SHT 有效新鲜 + 温度<cutoff + 全输出 OFF。
- 路径：`actuator_self_test {target:'fan'} → dry_service_run_async({duration=5000, heater_requested=false})`（**加热器全程保持 OFF**）→ safety_manager → HAL。
- dry_service 启动新增安全 OFF 同步：`dry_svc: initial safe-off sync: fan=OFF heater=OFF synced=1`（设备已验证）。
- 无 tach 回读 → 终态如实标注为**命令/锁存 OFF**，非物理转速证明（与 UV 同契约）。
- dry 快照新增保留 `terminal` + `outputs_synced`；`dry_fsm_commit_initial_off` / `dry_fsm_terminal_pending` 纯函数。

## B3 — 加热器/PTC 软件安全门禁（验证为主，不提供可通电路径）

- 加热器软件门禁**已在 safety_interlocks.c + dry_fsm 存在**并经审计确认：`fan_running` + pre_fan 已满、SHT 有效新鲜、温度<cutoff、周期<heat_on_max、关机序列先 HEATER_OFF 再 cooldown 再 FAN_OFF、任何 stale/invalid/overtemp 强制关加热器。host 纯 C 测试覆盖（dry_fsm_commit_initial_off fail-closed、终端顺序）。
- **未新增 heater 自检目标**：加热器硬件门禁保持 `NEEDS_USER_HEATER_TEST`；小程序加热器按钮隐藏（B4），直到用户完成风扇/温度/热保护夹具测试。无任何"直接开加热器"按钮。
- 板上 PTC 当前禁用（HT `--`），即使误触发也会被 safety 拒绝。

## B4 — 小程序开发自检控制（已加）

- 在 `MINIAPP_ENABLE_HARDWARE_DIAGNOSTICS` 下新增「排水泵自检（2 秒，需桶对齐 180°）」与「风扇自检（5 秒，仅风扇，加热器保持关闭）」按钮，`wx:if` 由设备 capabilities `actuator_self_test_targets`（现含 `drain_pump`,`fan`）协商。无订单/价格/程序变更、防重复提交、BLE 丢失显示"已请求紧急关断，等待设备重连确认"、off=true 才显示已关闭。

## B5 — 显示集成 + P2 完成

- 三页结构与 LVGL single-writer 保持。page-2 DR 已有；`display_telemetry_t.fan_state` 已暴露于遥测（app_bootstrap 从 dry 快照融合，位6=FAN 恒支持）。
- **P2 遗留（WDT 回归）**：R277 将 FAN 作为第 7 个输出 LED 渲染时触发 task_wdt（IDLE1 饿死，LVGL 忙循环，反锯齿弧线绘制慢）。回退 6-LED 布局（R278）后 WDT 归零。→ **FAN 显示 LED 下一轮以不触发 WDT 的方式实现**（例如静态文本/独立小卡，避免每帧重绘反锯齿弧线）。HT 仍 `--`；空闲进度条无亮条；语音页前缀去重均完成。
- 本轮未增加持久 LVGL 对象数（回退后与 R276 相同）。

## B6 — 自动化测试

- host `test_uv_act_core` **89/89**：Group A（68）+ Group B（21：drain/fan duration、名称往返、180°/270° 门禁矩阵、heater ON 拒、temp stale 拒、dry busy 拒、conflict 拒、drain/dry FSM 显式映射、dry 初始 OFF fail-closed）。
- host 其余：test_runner 308、display_model 49、phase11_safety 11、voice_vad 4、python 23；小程序 184+。
- 构建：MAIN 0 告警；UNIT PASS（6 条既有告警）。
- **10,000 次随机 Fake HAL 生命周期压力**：未在本次会话内完成（上下文预算）；已记录为下一轮 B6 补项（确定性种子 + 冲突输出断言）。如实说明，不伪报。

## B7 — 构建 / 烧录 / 验证

- 最终固件 R278：MAIN_BUILD 0 告警；烧录 COM8 **5/5 Hash verified**，硬复位一次。
- BIN `52668ef97015e37b70eaa92ee6eebcd1e8147f598cb0d7bdd1789495a974d716` / ELF `cf6e3e113d37b24276c426fc64048f626a6c68c0e9a36b194e384244681c1143`。
- 启动：`water`/`dry` 双 safe-off 同步日志；90s 探针 0 复位/WDT/Guru/Panic/Assert；无任何执行器通电。
- 静态扫描：UTF-8 字形 PASS、布局 PASS、safety bypass PASS（仅既有）、无新动态分配、无新 API key、无失控重试（dry/water sync 均一次性，fail-closed）。

## P0/P1/P2 结构化审查

- P0：0。P1：0。P2：2 项 —（1）FAN 显示 LED 因 WDT 暂缓（下一轮以静态渲染实现）；（2）10k Fake HAL 压力未在本会话完成（已记录补项）。

## 门禁

| 门禁 | 状态 |
|------|------|
| PHASE11_GROUP_A_FIX_SOFTWARE_GATE | PASS（P0=0/P1=0，全部测试+构建+扫描通过） |
| PHASE11_GROUP_A_PASSIVE_SOAK_GATE | **PASS**（4h：14400s，0 复位/WDT/Guru/Panic/Assert，act_open=0，堆非单调泄漏） |
| PHASE11_GROUP_A_HARDWARE_GATE | NOT_YET（待用户实物重测，不伪报） |
| PHASE11_GROUP_B_SOFTWARE_PREPARATION_GATE | **PASS（软件）**：drain/fan 自检门禁+运行时+协议+小程序+host 测试通过；加热器门禁验证；无实物通电。遗留 FAN 显示 LED（P2，不阻断软件门禁）。 |
| READY_FOR_USER_GROUP_A_RETEST | YES（软件侧） |
| READY_FOR_USER_GROUP_B_TEST | YES（drain/fan 自检已可点，需用户夹具；加热器除外） |
| READY_FOR_HEATER_TEST | **NO**（须先完成风扇/温度/热保护夹具；板上 PTC 禁用） |

不报告未观察的物理 PASS。加热器就绪仅在所有安全前置经用户夹具验证后确认。

## 明日用户验收清单（短）

Group A 重测（设备已烧 R278，软件门禁已通）：
1. SOURCE 进水阀 ×3（1.5s 出水 → 自动关 → 终态 off=true）
2. TRANSFER ×3 · 3. DETERGENT ×3（0.8s 泵动）
4. POSITION_NOT_ZERO（45° 拒，0° 过）· 5. IN/TR 互锁 · 6. BLE 断线关断 · 7. UV COMPLETE+off=true
8. 三页 UI 目视（IN/TR 空心、HT `--`、空闲进度无亮条、无「你：你：」）

Group B 未来测试（用户夹具确认后）：
9. 排水泵（180°，安全容器）· 10. 风扇仅测（270°，5s，加热器 OFF）· 11. 加热器（仅风扇/温度/热保护夹具确认后；当前隐藏）

每项：所需线缆/夹具、小程序观察点、page-2 观察点、期望终态、立即停止条件（见 R1.3 报告清单）。

## 未解析硬件事实（如实）

- 板上排水阀/风扇/PTC 物理接线/存在状态：Group A 未测到 Group B，无证据 → 下一轮用户确认。
- 风扇无 tach 反馈：命令/锁存 OFF 为最高可证明级别，非转速证明。
- PTC 在板上禁用（sdkconfig 无 CONFIG_XIAOJING_PTC_ENABLED，mode=LOW_VOLTAGE）。

## 附录：风扇硬件阻塞实测（R1.4，2026-08-07）

首次实接风扇上电实测：风扇在启动即转。两次状态确认极性映射与停转能力：

| 状态 | 观察 |
|------|------|
| 固件 duty=0、LEDC 无反转 → GPIO38 低 → S8050 断 → FAN_PWM_OUT 被上拉高 | **全速**（狂转） |
| R283 duty=0 + `output_invert=true` → GPIO38 高 → S8050 通 → FAN_PWM_OUT 拉低 | **最低速 idle（慢转，非停）** |

结论（严格由证据支持）：
- **FAN_PWM_SPEED_MAPPING = PASS**（反转后 percent→转速方向正确，duty=0 为最低速命令）。
- **FAN_PWM_ZERO_COMMAND = PASS**（固件在 OFF 态确实命令 0% 占空比 + 输出反转拉低 FAN_PWM_OUT）。
- **FAN_PWM_ZERO_RPM = NOT_SUPPORTED_BY_CURRENT_FAN**（该风扇 0% 仍保持最低转速；四线 PWM 风扇在 0% 行为不统一，部分停转部分最低速——Noctua PWM FAQ 亦注明型号差异）。
- **FAN_BOOT_POWER_OFF = NOT_PROVEN**（当前 PWM-only 架构无法保证启动硬停；"OFF"在本项目定义为执行器断电，非低速待机）。

> 最后一项证据缺口：需用逻辑分析仪/示波器确认 FAN_PWM_OUT 在 OFF 态为**持续 LOW（<~0.4V，无 PWM 脉冲/残余）**且晶体管饱和；若持续低仍慢转，则正式确认 `PWM_OFF_MINIMUM_SPEED = HARDWARE_BEHAVIOR`。当前未测，故 `FAN_BOOT_POWER_OFF = NOT_PROVEN`（不虚报）。

- **PHASE11_GROUP_B_FAN_HARDWARE_GATE = BLOCKED**。
- 风扇自检**软件**已实现并 host 测试通过（5s、270°、heater_requested=false、emergency 不杀 dry 服务），保留待解阻。

**推荐硬件解阻（下一轮）**：给风扇增加独立电源开关——
- `24V电源 → 高边MOS/直流继电器 → 风扇电源线`；
- `ESP32 PWM → S8050开集电极 → 风扇PWM线`。
- 电源开关负责真正 OFF；PWM 仅运行时调速；启动/故障/急停/BLE 断线时先撤销 PWM 再切断风扇电源。
- 风扇供电开关按 24V、3A 选型，建议留 40–60V、5A+ 裕量。
- 排水泵本版设计暂弃（无实物）；DRAIN 软件自检保留。
