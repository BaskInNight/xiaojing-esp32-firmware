# Phase 11 Group A Round 1.2: 真实执行器用户板测监督与证据闭环 — 报告

日期：2026-08-07
工程：
- 固件：`D:\UserData\ESP32Project\IDF\ESP32-IDF-Test`（ESP32-S3 N16R8, ESP-IDF v5.5.4, ILI9341 320×240 横屏, COM8）
- 小程序：`D:\UserData\ESP32Project\miniapp4.0`

本轮性质：**只监控与采集实物证据，不改代码、不重烧**。所有实物动作均由用户操作，本会话零代码改动、零重新烧录。

## SKILLS_ACTUALLY_INVOKED

| Skill | 状态 | 实际影响 |
|-------|------|---------|
| `/embedded-systems` | 已调用 | 建立"门禁拒绝 → 输出永不通电"的证据链；用 FreeRTOS 门禁顺序（位置先于输出）解释 POSITION_NOT_ZERO 仍可测；用 water FSM 初始化语义解释 OUTPUT_ALREADY_ON。 |
| `/esp32` | 已调用 | 串口 D-token 解析（h=internal free / L=largest / iB=I2S bytes）、MCP known/影子状态与 service 快照的差异。 |
| `/debugging-code` | 已调用 | 从带时间戳串口日志逐条还原 14 次门禁拒绝与 4 次 UV submit，定位三个既有固件缺陷（无崩溃现场，故未触发反汇编）。 |
| `/code-review` | UNAVAILABLE（无独立 skill） | 本轮为实物证据采集，代码审查由 R1.1 独立 agent 完成；本轮以结构化证据复核替代。 |

## PORT / BIN_SHA256 / ELF_SHA256

- PORT：COM8（CH340，`USB\VID_1A86&PID_7523`）。
- BIN_SHA256：`342142b3c0a4907e3e0d98f8e2471fb20e832aceb4261fca53d60a3d09d437ff`（3925472 B，与已烧录 R1.1 固件一致，本轮未重烧）。
- ELF_SHA256：`22cefcb254a139de55c4905038c3b52602749634164c7f2dfb68066ac326dcf7`。
- 板启动 reset reason：POWERON（R1.1 烧录硬复位，01:45）；会话内（02:03–02:33，~29 分钟）0 次复位，心跳 tick 由 4500 单调推进至 11500（~45 分钟连续运行）。

## 会话证据日志

`build_phase11_r1_2_hw.log`（`tools/r11a_timestamped_log.py`，[HH:MM:SS.mmm] 逐行时间戳，02:03:43–02:33:03）。

## SOURCE_RUN1/2/3（有压进水阀）

| 运行 | 时间 | request | 结果 | terminal | off | 实物 |
|------|------|---------|------|----------|-----|------|
| 尝试 1 | 02:11:23 | source | NACK `OUTPUT_ALREADY_ON` | 未受理 | - | 无动作 |
| 尝试 2 | 02:11:36 | source | NACK `OUTPUT_ALREADY_ON` | 未受理 | - | 无动作 |
| 尝试 3 | 02:11:52 | source | NACK `OUTPUT_ALREADY_ON` | 未受理 | - | 无动作 |
| 附加 | 02:13:25 | source | NACK `OUTPUT_ALREADY_ON` | 未受理 | - | 无动作 |

- IN 从未实心（阀门从未通电，`ACT_TEST open`=0）；用户未接水压，无法观察出水。
- 每次拒绝参数一致：`tgt_off=0`（water 报阀 UNKNOWN）、`mcp_known=1`、`det_idle=0`。
- **结论：NOT_YET —— 阀门从未动作（固件门禁缺陷 HW-FIX-1）**。4/4 均被拒，TR 全程未 ON，0 复位。

## TRANSFER_RUN1/2/3（零压转移阀）

| 运行 | 时间 | request | 结果 | 实物 |
|------|------|---------|------|------|
| 1 | 02:21:05 | transfer | NACK `OUTPUT_ALREADY_ON` | 无动作 |
| 2 | 02:21:10 | transfer | NACK `OUTPUT_ALREADY_ON` | 无动作 |
| 3 | 02:21:40/58 | transfer | NACK `OUTPUT_ALREADY_ON` | 无动作 |

- 转移阀从未通电，SOURCE 全程 OFF，无水流。
- **结论：NOT_YET —— 阀门从未动作（HW-FIX-1）**。TR 未 ON，SOURCE 未 ON，无重叠，0 复位。

## DETERGENT_RUN1/2/3（洗涤剂蠕动泵）

| 运行 | 时间 | request | 结果 | 实物 |
|------|------|---------|------|------|
| 1 | 02:22:35 | detergent | NACK `SERVICE_BUSY` | 泵无动作 |
| 2 | 02:22:53 | detergent | NACK `SERVICE_BUSY` | 泵无动作 |
| 3 | 02:23:09 | detergent | NACK `SERVICE_BUSY` | 泵无动作 |

- 拒绝参数：`det_idle=0`（洗涤剂服务空闲态误报）、`tgt_off=1`（泵输出本身 KNOWN_OFF）。
- **结论：NOT_YET —— 泵从未动作（HW-FIX-2）**。DT 未 ON，0 复位。

## POSITION_NOT_ZERO_RESULT

**PASS**。用户转至 45°，INPUTS「45」实心稳定（hall stable=0x02，02:25:00），点 SOURCE：
```
02:24:52 ACT_SELFTEST reject code=POSITION_NOT_ZERO target=source_valve pos=45 valid=1 fresh=1 stable=1 moving=0
```
- 请求未受理，IN/TR/DT 均未通电；快照确实有效 45° 稳定——**真实 POSITION_NOT_ZERO，非 UNKNOWN_POSITION 替代**。
- 小程序显示「被拒绝：桶位置不是 0°」，正确。

## INTERLOCK_RESULT

**NOT_YET**。用户点「阀互锁自动测试」：第一步 SOURCE 请求即被 `OUTPUT_ALREADY_ON` 拒绝 → 小程序**正确中止**（未发送 transfer），显示 `source: OUTPUT_ALREADY_ON · 结果 FAIL`。
- 串口：02:26:50 一条 source 拒绝；**无 transfer 请求**（4 条 transfer 记录全部来自 Stage 2 手动测试）。
- 互锁测试机制本身正确（源未受理即中止、不伪造、不提前确认），但互锁 PASS 无法建立——需先修复 HW-FIX-1。

## BLE_DISCONNECT_RESULT

**NOT_YET**。用户点「BLE 断线自动关断测试」：SOURCE 请求被 `OUTPUT_ALREADY_ON` 拒绝 → 测试在源受理前中止，**未触发断线**，小程序显示 FAIL、`output_confirmed_off=false`、**未在任何设备证据前显示"已关闭"**（机制正确）。
- 串口：02:28:05 一条 source 拒绝；无 BLE disconnect 事件、无 emergency 活动（`HEAP[ble_disconnected]` 是标签名，非事件）。

## UV 补充证据（用户要求跑完采集）

- 4 次 UV submit（request_id 3/4/5/6）全部 `result=0x0`（受理），但串口**无任何 UV terminal / ON confirmed / lamp OFF 确认日志**；小程序每次报 `FAULT(6)`「关灯识别失败，请立即断电检查」。
- 用户观察：UV 灯 3 秒测试后**实际关掉了**，但固件无法确认 KNOWN_OFF → 报 FAULT。屏幕 UV 灯从未实心（output_state 未确认 KNOWN_ON）。
- **结论：硬件动作已发生（UV 亮→灭），但固件 OFF 确认不成立 → 报 FAULT。属 HW-FIX-3。**

## OUTPUT_OVERLAP_COUNT / OUTPUT_CONFIRMED_OFF_COUNT

- OUTPUT_OVERLAP_COUNT：**0**（任何时刻无任何输出通电，无 IN/TR 重叠；`ACT_TEST open` 总计数 = 0）。
- OUTPUT_CONFIRMED_OFF_COUNT：**0**（无执行器开启故无 OFF 确认事件；UV 4 次 OFF 确认均失败 → FAULT）。
- 全程无持续通电、无双阀重叠、无 off=false 残留（SOURCE/TRANSFER/DETERGENT 从未 ON）。

## USER_PHYSICAL_OBSERVATIONS

- 无法正式接水压测试 SOURCE（用户条件限制）——无出水观察。
- TRANSFER/DETERGENT：门禁拒绝，无水流/泵转动观察。
- UV：灯按测试亮 3 秒后实际熄灭，但固件报关灯确认失败。
- 45° 位置拒绝测试正常（POSITION_NOT_ZERO）。

## UI_VISUAL_RESULT

| 项 | 结果 |
|----|------|
| 顶栏 Wi‑Fi 实心（GOT_IP）、BLE 实心（CONNECTED）；形状可区分（弧 vs 中柱双翼） | ✅ PASS |
| INPUTS 0° 实心、45/90/180/270/BL 空心 | ✅ PASS |
| OUTPUTS IN/TR/DR/DT/UV 空心 ○、HT 灰色 `--`、图例正常 | ✅ PASS |
| 第二页无裁切/重叠 | ✅ PASS |
| 第一页空闲进度条仍为亮色粗条（非暗色细轨） | ⚠ UI-P2-2（记录） |
| 第三页「你：你：—」重复前缀 | ⚠ UI-P2-1（记录） |
| 第三页无裁切/重叠；「ESP-SR - 小爱同学」贴右缘过紧（建议留 4–6px 安全边距） | ⚠ 记录（cosmetic） |

## RESET_BY_REASON / WDT / GURU / PANIC_ASSERT

- RESET_BY_REASON：`{}`（会话内 0 次复位；板上电复位为 R1.1 烧录硬复位）
- WDT_COUNT：0
- GURU_COUNT：0
- PANIC_ASSERT_COUNT：0（panic/abort/assert 均 0）

## INTERNAL_FREE_START/MIN/END / INTERNAL_LARGEST_MIN

- INTERNAL_FREE_START/MIN/END：**11867 / 11867 / 11879 B**（D-token `h=`，真实 internal free；全程稳定，微升，无持续下降）
- INTERNAL_LARGEST_MIN：**4096 B**（`L=`）
- iB（I2S bytes）：36259840 → 91591680（语音采集全程活跃；iB=I2S bytes 非 heap）
- 会话时长：~29 分钟；心跳 tick 4500→11500 单调（display 任务存活）

## CODE_REVIEW_P0/P1/P2

- 本轮无代码改动，无新增代码审查面。引用 R1.1 独立 agent 结论：P0=0，P1=0，P2=1（图标 1px 裁剪 cosmetic，接受）。
- 本轮新增**实物暴露的固件缺陷**（非代码审查项，是硬件验收阻塞项）：

| ID | 缺陷 | 证据 | 严重度 |
|----|------|------|--------|
| HW-FIX-1 | water 服务将双阀初始化为 `WATER_VALVE_UNKNOWN`（water_fsm.c:39-40），只有跑过进水 FSM 才变 KNOWN_OFF → 首次进水前 SOURCE/TRANSFER 门禁 `target_confirmed_off = !on && !unknown` 恒 false → `OUTPUT_ALREADY_ON` 拒绝 | 6× source + 4× transfer 全部拒绝，`tgt_off=0` | **P0（板测全阻塞）** |
| HW-FIX-2 | detergent 服务空闲态误报（`det_idle=0`），DETERGENT 被 `SERVICE_BUSY` 拒绝；根因待查（get_snapshot 失败或 FSM 状态非 IDLE / terminal_pending） | 3× detergent 拒绝，`det_idle=0 tgt_off=1` | **P0（板测全阻塞）** |
| HW-FIX-3 | UV 输出 KNOWN_ON/KNOWN_OFF 确认在实物上不成立（submit 后无任何 UV terminal/ON/OFF 确认日志），OFF 确认失败 → FAULT | 4× UV submit 无 terminal；小程序 4× FAULT；屏幕 UV 不实心 | **P1（UV 自检不可用）** |

> 注：HW-FIX-1 的显示层被 R1.1 的 `fuse_output_state` 掩盖（屏幕显示空心 OFF），但门禁读 water 原始快照，未被修复——本轮实物测试首次暴露。

## UI_P2_FINDINGS

- **UI-P2-1（已确认）**：语音页用户文本显示「你：你：—」——文本数据可能已带 speaker 前缀，显示层又加了一次。仅记录，本轮未改。
- **UI-P2-2（已确认）**：第一页空闲态顶栏进度条仍为亮色粗条，未降为暗色细轨/隐藏填充。仅记录，本轮未改。
- **UI-P2-3（新增记录）**：第三页后端名「ESP-SR - 小爱同学」贴右缘过紧，建议留 4–6px 右侧安全边距。仅记录。

## PHASE11_GROUP_A_HARDWARE_GATE

**NOT_YET（受阻）** — 按门禁规则逐项：

| 门禁项 | 判定 | 说明 |
|--------|------|------|
| 任一执行器未实际动作 | NOT_YET | SOURCE/TRANSFER/DETERGENT 全部被门禁拒绝，实物动作 0 次 |
| 任一输出未确认 OFF | NOT_YET | 无输出开启故无 OFF 确认；UV 4 次 OFF 确认失败（FAULT） |
| 任一 IN/TR 重叠 | 通过（0 次） | 从未同时通电 |
| NOT_ZERO 未拒绝 | 通过 | 45° 时正确 POSITION_NOT_ZERO |
| BLE 断线后不能证明关断 | NOT_YET | 源请求被拒，断线路径未启动，无证据 |
| reset/WDT/Guru 非零 | 通过（均为 0） | 会话内稳定 |
| 实物证据全部通过 | NOT_YET | 关键实物动作（阀/泵动作、出水、断线关断）缺 |

- 不把自动测试/编译成功/用户未回答伪报为硬件 PASS：本门禁 NOT_YET 是基于"实物动作证据缺失 + 三个固件缺陷阻塞"的诚实判定。

## READY_FOR_PHASE11_GROUP_B

**NO**。PHASE11_GROUP_A_HARDWARE_GATE=NOT_YET，需先修复 HW-FIX-1/2/3 并重跑实物板测后方可进入 Group B（排水泵/加热器/电机等）。

## 下一步（后续修复轮，本轮未执行）

1. HW-FIX-1：water 服务启动/首次待机时经 safety MCP 已知态建立双阀 KNOWN_OFF（或在 gate 用 safety 的 mcp_known+shadow 替代 water `unknown`）。
2. HW-FIX-2：查 detergent `get_snapshot`/FSM 空闲态，消除 `det_idle=0`。
3. HW-FIX-3：查 UV 输出 KNOWN_ON/OFF 确认路径（safety_manager_apply → uv_service output_state），修复实物 OFF 确认。
4. UI-P2-1/2/3 在下一 UI 轮一并处理（前缀去重、空闲进度条降亮度/细轨、后端名安全边距）。

## 结束语

本轮严格执行"只监控、逐动作、上一步未确认 OFF 不进下一步、不自主触发执行器"。全程零代码改动、零重烧；设备稳定（0 复位/0 WDT/Guru/panic，内存稳定），未发生任何危险通电或重叠。三个 P0/P1 固件缺陷（HW-FIX-1/2/3）仅在真实硬件板测下暴露，已完整取证，等待 Codex code review 后进入修复轮。
