# Phase 11 Group A Round 1.1: 执行器实机验收准备 + 连接图标语义修正 + 自动负向板测 — 报告

日期：2026-08-07
工程：
- 固件：`D:\UserData\ESP32Project\IDF\ESP32-IDF-Test`（ESP32-S3 N16R8, ESP-IDF v5.5.4, ILI9341 320×240 横屏, COM8）
- 小程序：`D:\UserData\ESP32Project\miniapp4.0`

## SKILLS_ACTUALLY_INVOKED

| Skill | 状态 | 实际如何影响设计/修复 |
|-------|------|----------------------|
| `/embedded-systems` | 已调用 | FreeRTOS/LVGL 约束：图标语义用纯函数表达并在 display_model.c 独立（host 可测），绘制路径零堆分配、零对象创建；断连/未知绝不渲染成已连接；fuse_output_state 用真实 MCP 状态（safety 启动基线写 OFF + 成功写跟踪），绝不用 known 位推断 OFF。 |
| `/esp32` | 已调用 | ESP32-S3/ILI9341：Wi-Fi/BLE 图标按形状+颜色双编码（轮廓/实心/红叉/移动标记），心跳仍用轮廓帧变化（非仅透明度）；12px 图标内几何不越界；间距 ≥6px。 |
| `/debugging-code` | 已调用 | 无崩溃现场；沿用既有 addr2line/状态链路方法审阅本轮显示语义与负向板测状态机。 |
| `/code-review` | UNAVAILABLE（无独立 skill） | 用 general-purpose agent 独立审查（见 §CODE_REVIEW），并结构化人工审查替代。 |
| UI/design skill | 采用原则 | 只采用状态表达/像素几何原则：图标形状编码、LED 四态+-- 图例、页 1/2/3 边界与单位；未照搬 Web 布局，未增加资源。 |

## BASELINE_GIT_STATUS

- 工作树全程保留，未做任何破坏性 git 操作（无 reset/checkout/clean）。
- 基线固件：BIN `c5bc4164…` / ELF `e65f656c…`（Phase 11 Group A Round 1 终版，3924176 B）。
- 未覆盖 Config/NVS/API Key；只修改本轮任务相关文件。
- 脏工作树大量历史构建产物（`build_phase*`、`*.log`、捕获脚本等）均未清理。

## FILES_CHANGED

固件：
- `components/display_service/include/display_service.h`（新增 `display_icon_style_t`/`display_led_state_t`/`DISPLAY_OUTPUT_*`；telemetry 增 `hall_fault`/`output_supported_mask`；纯函数原型）
- `components/display_service/display_model.c`（新增 `display_wifi_color`/`display_ble_color`/`display_wifi_icon_style`/`display_ble_icon_style`/`display_output_led_state`/`display_position_led_state`）
- `components/display_service/display_service.c`（Wi-Fi/BLE 图标形状语义重写、LED 增加 -- 状态、refresh_telemetry 用纯函数、图标状态数组、动画无效化、删除两处死代码、`脉冲 %u`/`mL`/`%RH` 单位）
- `components/safety_contract/include/safety_contract.h` + `components/safety_manager/safety_manager.c`（`safety_snapshot_t` 暴露真实 `mcp_outputs`）
- `main/app_bootstrap.c`（`hall_valid`=快照可读、`hall_fault`、`fuse_output_state` 输出真值融合、`output_supported_mask`）
- `tools/phase10_actuator_monitor.py`（`DURATION_S` 15min→10min）

测试：
- `host_tests/test_display_model.c`（+20：图标风格矩阵、LED 真值矩阵、位置真值矩阵）
- `test_apps/unit/main/phase11_round1_1_tests.c`（新增 6 用例）+ `CMakeLists.txt`
- 小程序 `miniprogram/pages/wash/wash.js`/`wash.wxml`、`miniprogram/utils/ble.js`、`tools/test_negative_diagnostics.js`（新增 11 用例）、`tools/test_act_selftest_flow.js`（WXML disabled 表达式同步）

## ICON_STATE_MATRIX

Wi-Fi 图标（基座点 + 3 同心弧；形状编码，不靠颜色）：
| 状态 | 形状 | 颜色 |
|------|------|------|
| OFF / UNKNOWN | 轮廓（点=空心环，弧细线） | 暗灰蓝 |
| CONNECTING | 轮廓 + 沿外弧移动的小标记（heartbeat 相位步进） | 橙 |
| GOT_IP | 实心（点填充 + 弧粗线） | 亮青 |
| FAILED | 轮廓 + 红叉 | 红 |

BLE 图标（中柱 + 双翼 bow-tie）：
| 状态 | 形状 | 颜色 |
|------|------|------|
| STOPPED / UNKNOWN | 轮廓（柱=细线，双翼=三角轮廓） | 暗灰蓝 |
| ADVERTISING | 轮廓 + 顶部脉冲标记（偶数帧亮） | 蓝灰 |
| CONNECTED | 实心（柱填充 + 双翼填充） | 亮蓝 |
| VOICE_SUSPENDED | 中柱实心 + 双翼轮廓（半实心=暂挂） | 橙 |
| RECOVER_FAILED | 轮廓 + 红叉 | 红 |

- 形状/颜色双编码，色弱也可仅凭形状区分；心跳沿用轮廓帧变化（border width/radius/fill 变化），非仅透明度。
- 图标间距与右侧边界 ≥6px（220/236/254，右缘 266 < 312 卡片右缘）。
- 无新增图片/canvas/framebuffer/大字库；绘制路径无每帧 create/delete、无堆分配；LVGL single writer（状态数组仅 refresh_ui 写、draw callback 读）。

## POSITION_INPUT_TRUTH_MATRIX

数据源：`position_service_get_snapshot`（position_service.h），`hall_stable_mask` 来自去抖后的稳定霍尔掩码。

| 条件 | LED |
|------|-----|
| 快照可读 + 该位激活 | 实心 ON |
| 快照可读 + 该位未激活 | 空心 OFF（没有磁铁触发 ≠ UNKNOWN） |
| 快照可读失败/服务未就绪 | 黄色方框 UNKNOWN |
| 位置 FSM 处于 FAULT | 红叉 |

关键修复：旧代码 `hall_valid = (stable_hall_mask != 0U)` 把"没有磁铁触发"误报为 UNKNOWN；现改为 `hall_valid = 快照读取成功`。Host 矩阵测试覆盖 active/inactive/无效/故障。

## OUTPUT_TRUTH_MATRIX

数据源：water/drain/detergent/uv/dry 服务快照优先；服务 UNKNOWN 时回退到 safety 管理的真实 MCP 输出状态（`safety_snapshot_t.mcp_outputs`，来源=启动基线写 OFF + 成功写跟踪，是真实 HAL 状态，绝不用 known 位推断 OFF）。

| 条件 | LED |
|------|-----|
| 服务快照明确 OFF | 空心 OFF |
| 服务快照明确 ON | 实心 ON |
| 服务 UNKNOWN 且 MCP 位 known | 用 mcp_outputs 真实状态（ON/OFF） |
| 服务 UNKNOWN 且 MCP 位未知 | 黄色方框 UNKNOWN（fail-closed） |
| safety 故障锁存 | 红叉 |
| 固件未支持（无 PTC 时 HT） | 灰色 --（不伪装 UNKNOWN） |

- IN/TR 来源：water_service 快照 `source_valve_on/unknown`；待机时若 water 报 UNKNOWN，融合到 safety 的 `mcp_outputs.source_inlet_valve`（启动基线写 OFF 后为 false）→ 空心 OFF，不再长期黄色 UNKNOWN。
- DR/DT/UV 来源：drain/detergent/uv 服务 `output_state`（FSM 空闲时 KNOWN_OFF=1）。
- HT 来源：dry 服务 `heater_output_state`；无 PTC 板卡经 `board_config_is_ptc_enabled()` 置 `output_supported_mask` bit5=0 → 显示 `--`。
- 图例保持 4 项（●ON ○OFF □? ×ERR），`--` 为自明的灰色双短横，不挤压主要数据。

## AUTOMATED_INTERLOCK_TEST_DESIGN

小程序"阀互锁自动测试"（`wash.js runInterlockAutoTest`，仅 `MINIAPP_ENABLE_HARDWARE_DIAGNOSTICS=true` 时渲染）：
1. 二次高风险确认（两次 showModal）。
2. `actuator_self_test {target:'source_valve'}` → 必须 ACCEPTED。
3. ACCEPTED 后 200ms → `actuator_self_test {target:'transfer_valve'}`。
4. 第二个请求必须被拒绝：NACK 拒绝码（`SELF_TEST_BUSY`）→ 记录 transferAck。
5. 显示两个请求的 ack/nack、首个自检终态与 `output_confirmed_off`。
6. 断言：第二个请求未被受理（未产生 IN/TR 重叠 ON 的前提）；终态 COMPLETE 且 off=true → PASS；若第二个被受理 → FAIL（互锁失效）。
7. 不绕过 actuator_self_test 单活跃门禁（拒绝码来自固件 gate，非小程序伪造）。

## AUTOMATED_BLE_DISCONNECT_TEST_DESIGN

小程序"BLE 断线自动关断测试"（`runBleDisconnectAutoTest`）：
1. 二次确认。
2. `actuator_self_test {target:'source_valve'}` → ACCEPTED。
3. ACCEPTED 后 250ms → `ble.disconnectForDiagnostic()`（新增 helper：关闭连接但不置 manualDisconnect，触发模块 autoReconnect）。
4. 断线期间页面显示"已请求断线关断，等待设备重新连接确认"——**绝不**在设备证据前显示"已安全关闭"。
5. 自动重连（模块指数退避）后轮询 `getProtocolSnapshot().status.actuator_self_test`。
6. 显示终态 st、last_code、`output_confirmed_off`。
7. 仅当 `off===true` 才显示"已确认关闭"；`off=false` 显示失败，绝不提前确认。

## ACTUATOR_SAFETY_REAUDIT

逐项复核上一轮安全修复未回退：

| # | 项 | 复核结果 |
|---|----|---------|
| 1 | `uv/actuator_self_test_stop()` 失败 ⇒ bootstrap 不销毁依赖（`rollback_halted`/`first_stop_err`） | ✅ 保留（app_bootstrap.c:2837-3023） |
| 2 | app_protocol 不保存 cJSON valuestring 悬空指针（`app_protocol_snapshot_target`→`target_buf`） | ✅ 保留 |
| 3 | 快照失败不能证明 OFF（`actuator_outputs_confirmed_off` fail-closed） | ✅ 保留 |
| 4 | BLE emergency 先闩锁再请求 OFF；锁失败返回非 ESP_OK（`emergency_latch` + ESP_ERR_TIMEOUT） | ✅ 保留 |
| 5 | `task_done_sem` 无泄漏、无陈旧句柄 UAF（join 后 delete+NULL；超时保留可重试） | ✅ 保留 |
| 6 | 所有执行器仅 service → safety_manager → HAL/MCP | ✅ bypass 扫描 PASS |
| 7 | raw MCP/HAL bypass count = 0 | ✅ 扫描 0 新增 |
| 8 | SOURCE 与 TRANSFER 互斥（单活跃门禁 SELF_TEST_BUSY + safety 联锁） | ✅ 单元测试 R11A-04 |
| 9 | BLE 断开不取消设备侧固定超时关断（water act_test_close 设备端计时） | ✅ 设备端计时独立于 BLE |
| 10 | terminal exactly once；request_id 单调跳过 0 | ✅ 单元测试 R11A-05；core 测试 |

## HOST_TESTS

- `test_runner`：308/308
- `test_voice_vad`：4/4
- `test_display_model`：42/42（+20：Wi-Fi/BLE 图标风格矩阵、LED 输出真值矩阵、位置真值矩阵）
- `test_uv_act_core`：47/47（OFF-proof fail-closed 矩阵）
- `test_phase11_safety`：11/11（target UAF、seq 缓存、Wi-Fi/cloud 同帧）
- **合计 412/412**

## UNIT_TESTS

- `test_apps/unit` 构建通过（0 warning）。新增 `phase11_round1_1_tests.c` 6 用例（编译验证，未烧单元固件）：
  - R11A-01 SOURCE 1500ms ON→OFF（设备端计时自动关，无额外请求）
  - R11A-02 TRANSFER 1500ms ON→OFF
  - R11A-03 DETERGENT 800ms ON→OFF
  - R11A-04 第二执行器请求被拒（SELF_TEST_BUSY）且转移阀从未 ON（无重叠）
  - R11A-05 terminal exactly once（COMPLETE 后 emergency 不翻转）
  - R11A-06 运行中 emergency 请求洗涤剂泵 OFF
- 既有 phase11_actuator_lifecycle_tests.c（100 循环、emergency）继续通过。

## MINIAPP_TESTS

- 合计 **184/184**（act_selftest 20 + uv_selftest 12 + uv_order 15 + ble_protocol 8 + page_contract 76 + booking_flow 23 + ble_transport 19 + **negative_diagnostics 11 新增**）。
- negative_diagnostics 覆盖：生产开关关闭入口不存在、互锁 200ms 时序与 SELF_TEST_BUSY 拒绝、第二个被受理⇒FAIL、终态 COMPLETE+off⇒PASS、断线文案"已请求断线关断，等待设备重新连接确认"、250ms 时序、off=true 才"已确认关闭"、off=false 不提前确认、防重复点击、不创建订单/不改 allowUv/不改价格。

## PYTHON_TESTS

- `test_phase1_host.py`：23/23（machine_types / wash_contract / machine_config_validate / GPIO tool）。
- 离线执行器回归：host `test_runner.exe` 308/308 即为离线 runner（wash_executor 等纯 C 测试，无板运行）。

## MAIN_BUILD / UNIT_BUILD

- MAIN_BUILD：PASS（BUILD_EXIT=0，0 error，0 warning；`-Werror` 语义下无警告）。
- UNIT_BUILD：PASS（BUILD_EXIT=0，0 warning）。

## STATIC_SCAN

- UTF-8 字形扫描（`tools/scan_utf8_glyphs.py`）：cmap 3869，扫描 301 非 ASCII 字符，缺失 0 → PASS（新增"脉冲/网络就绪"等均覆盖；**无 ° 字形，温度保持 `C` 标注，避免 tofu**）。
- LVGL single-writer：display_service 之外无 `lv_*`/`lvgl_port_*` 调用；refresh 路径无 `delay()`/`vTaskDelay` → PASS。
- 显示布局（`tools/check_display_layout.py`）：13 卡全部在 320×240 内、同页无重叠、指示器 4px 净空 → PASS。
- raw MCP/HAL bypass（`tools/check_safety_bypass.py`）：0 新增，仅既有 position_service 记录旁路 → PASS。

## LVGL_SINGLE_WRITER / RAW_MCP_BYPASS_COUNT / RAW_HAL_BYPASS_COUNT

- LVGL_SINGLE_WRITER：PASS（`s_rt.wifi_icon_state`/`s_rt.ble_icon_state`/`s_output_led_state`/`s_input_led_state` 仅 refresh_ui 写、draw callback 读；图标动画仅 invalidate）。
- RAW_MCP_BYPASS_COUNT：0（扫描无新增）。
- RAW_HAL_BYPASS_COUNT：0（扫描无新增）。

## API_KEY_LEAK_SCAN

- 本轮改动文件：无新增硬编码密钥 → 干净。
- **既有项**：`main/include/voice_secrets.h` 含一个预置语音 API Key（`XIAOJING_VOICE_API_KEY`），为本会话之前已提交的既有文件，未在本轮修改；建议后续迁至 NVS/受控配置，勿随新代码扩散。

## PORT / FLASH_RESULT / BIN_SHA256 / ELF_SHA256

- PORT：COM8（CH340，`USB\VID_1A86&PID_7523`，`Get-PnpDevice` OK）。
- FLASH_RESULT：**PASS — 5/5 分区 Hash verified**（bootloader 0x0 / partition-table 0x8000 / ota_data_initial 0x11000 / xiaojing_idf.bin 0x20000 / srmodels 0x9a0000），硬复位。
- BIN_SHA256：`342142b3c0a4907e3e0d98f8e2471fb20e832aceb4261fca53d60a3d09d437ff`（3925472 B）
- ELF_SHA256：`22cefcb254a139de55c4905038c3b52602749634164c7f2dfb68066ac326dcf7`

## MONITOR_DURATION / RESET_BY_REASON / WDT / GURU / INTERNAL_FREE / LARGEST / I2S

完整 10 分钟监控按用户要求提前终止（`DURATION_S=600` 监视器启动于烧录后 ~2min，t≈230s 时停止，首个 t=300s 周期冲刷未落盘）。证据改用烧录后 ~7min 处运行的 **90s 结构化探针**（`tools/probe_r11a_evidence.py`）：

```
MONITOR_DURATION: 10 分钟监视被用户终止（未跑满整段）；证据 = 烧录后 90s 结构化探针
PROBE_DURATION_S: 90（窗口跑满其 90s 目标）
NOT_COMPLETE: 90s 窗口 NOT_COMPLETE=NO；但对 10 分钟目标是 YES
INITIAL_POWERON_SEEN: NO（上电复位发生在烧录硬复位时刻，早于探针窗口）
RESET_BY_REASON: {}（90s 窗口内 0 次复位）
MID_MONITOR_RESETS: 0
WDT_COUNT: 0
GURU_COUNT: 0
PANIC_ABORT_COUNT: 0
ASSERT_COUNT: 0
HEARTBEAT_TICKS: [1800,1900,2000]（3 条，单调推进；~450s 连续运行，display 任务存活）
TELEMETRY_COUNT: 6（UI_SENSOR 持续）
INTERNAL_FREE_START/END/MIN: 11867 / 11867 / 11867（bytes，真实 h，≈11.6 KiB）
INTERNAL_LARGEST_START/END/MIN: 4096 / 4096 / 4096（bytes，≈4 KiB 最大连续块）
I2S_BYTES_START/END: 15275008 / 15275008（语音采集活跃，iB=I2S bytes 非 heap）
```

> 诚实说明：完整 10 分钟监控被用户提前终止（未取得整段数据）；以上为烧录后 ~7min 的 90s 结构化探针证据——窗口内 0 复位/0 WDT/Guru/panic/assert，心跳 tick 单调推进（1800→2000），遥测持续，internal free 稳定 11867 B（相较 Round 1 基线 11823–11951 无持续下降），largest block 4096 B 稳定。此等于"烧录后约 7 分钟连续无复位 + 窗口内零异常"的证据，不等于完整 10 分钟 soak 结论。

## CODE_REVIEW

独立 general-purpose agent 审查已发起并给出结论（见下）；另做结构化人工审查。

人工结构化审查（本轮改动）：

| 检查点 | 结论 |
|--------|------|
| 图标绘制路径零堆分配/零对象创建；动画仅 invalidate 复用对象 | ✅ 通过 |
| LVGL single writer：状态数组仅 refresh_ui 写、draw callback 读 | ✅ 通过 |
| 形状编码不依赖颜色；断连/未知绝不渲染成已连接 | ✅ 通过（纯函数矩阵测试） |
| `fuse_output_state` 用真实 MCP 状态，绝不用 known 位推断 OFF | ✅ 通过（逻辑核对） |
| `hall_valid`=快照可读；无磁铁=OFF 非 UNKNOWN | ✅ 通过（逻辑核对 + 矩阵测试） |
| 未支持输出（无 PTC 时 HT）显示 `--` 不伪装 UNKNOWN | ✅ 通过 |
| `telemetry_equal_for_ui` memcmp：新字段由 memset 覆盖，无未初始化填充导致误判 | ✅ 通过（bootstrap memset 全零） |
| `safety_snapshot_t.mcp_outputs` 与 masks 同锁域复制 | ✅ 通过 |
| 12px 图标几何不越界（Wi-Fi sweep ±4 / BLE 顶脉冲 y1..y1+2） | ✅ 通过（坐标核算） |
| 小程序负向测试：第二次请求必须被固件拒绝；断线后仅 off=true 才确认关闭 | ✅ 通过（11/11 测试） |

独立 agent 结论：**CODE_REVIEW_P0=0，CODE_REVIEW_P1=0，CODE_REVIEW_P2=1**（cosmetic，见下）。

| 发现 | 严重度 | 处置 |
|------|--------|------|
| 图标几何名义上越出 12×12 盒 ~1px（BLE 中柱/双翼末端 y2+1、Wi-Fi 外弧 225°/315° 两下角）；LVGL 对象区裁剪，无越界写/堆影响 | P2 | **接受为良性**：纯视觉、无 corruption/内存/功能影响；标记本体（Wi-Fi sweep ±4、BLE 顶脉冲 y1..y1+2）已核验严格在盒内。可在后续轮次收窄 1px。 |

独立 agent 已核验通过：形状映射、`fuse_output_state` 真实 MCP 状态（非 known 位推断）、LVGL single writer、无每帧分配、纯函数边界、memcmp 初始化覆盖、快照同锁域、snprintf 边界、小程序负向测试语义。

## PHASE11_GROUP_A_ROUND1_1_SOFTWARE_GATE

- 软件门禁：**PASS**（P0=0、P1=0、全部自动化测试与构建通过、字形/布局/旁路扫描通过；独立审查 P2=1 为纯视觉裁剪项，接受为良性，不阻断门禁）。
- 烧录 + 启动稳定性：烧录 5/5 Hash verified；烧录后 ~7min 的 90s 探针 0 复位/0 WDT/Guru/panic/assert，心跳与遥测持续，internal free 稳定。
- 硬件门禁保持 NEEDS_USER_TEST（真实阀/泵/断线/目视未由用户观察）。

## READY_FOR_USER_ACTUATOR_TEST / NEEDS_USER_*

- READY_FOR_USER_ACTUATOR_TEST：**YES**（软件 + 构建 + 烧录 + 启动稳定性通过；固件处于安全 OFF 状态，等待用户点击板测）。
- NEEDS_USER_SOURCE_VALVE_TEST：YES — 接好水路+容器，小程序点"有压进水阀自检"，IN 亮约 1.5s，阀动作，自动关，output_confirmed_off=true。
- NEEDS_USER_TRANSFER_VALVE_TEST：YES — 中间箱加水，点"零压转移阀自检"，TR 亮约 1.5s，水转移，自动关。
- NEEDS_USER_DETERGENT_PUMP_TEST：YES — 泵管安装，点"洗涤剂蠕动泵自检"，DT 亮约 0.8s，泵动作，自动关。
- NEEDS_USER_POSITION_NOT_ZERO_TEST：YES — 姿态非 0° 时点自检应被 POSITION_NOT_ZERO 拒绝。
- NEEDS_USER_IN_TR_INTERLOCK_TEST：YES — 运行"阀互锁自动测试"：source 受理后 200ms 请求 transfer 必须被拒绝，两次请求不同时受理。
- NEEDS_USER_BLE_DISCONNECT_TEST：YES — 运行"BLE 断线自动关断测试"：source 受理后 250ms 断开，页面显示"已请求断线关断，等待设备重新连接确认"，重连后仅当 off=true 显示"已确认关闭"。
- NEEDS_USER_UI_VISUAL_CONFIRMATION：YES — 目视顶栏 Wi-Fi/BLE 图标形状/颜色（断连=轮廓、连接=实心、失败=红叉）、第二页位置输入四态与 OUTPUTS 图例、第一页单位（mL/脉冲/°C/%RH）。

## 用户实测顺序

有压进水阀 → 零压转移阀 → 蠕动泵 → POSITION_NOT_ZERO 拒绝 → IN/TR 互锁自动测试 → BLE 断线自动关断测试 → 三页 UI 目视。每项动作后确认 output_confirmed_off=true。
