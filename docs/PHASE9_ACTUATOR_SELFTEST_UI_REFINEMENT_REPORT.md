# Phase 9: 执行器安全板测框架 + 三页显示 UI 精修 + UV 审计修复 — 报告

日期：2026-08-06
工程：
- 固件：`D:\UserData\ESP32Project\IDF\ESP32-IDF-Test`（ESP32-S3 N16R8, ESP-IDF v5.5.4, ILI9341 320×240 横屏, COM8）
- 小程序：`D:\UserData\ESP32Project\miniapp4.0`

## 0. SKILLS_ACTUALLY_INVOKED

| Skill | 状态 | 实际如何影响设计/修复 |
|-------|------|----------------------|
| `/embedded-systems` | 已调用 | 驱动 FreeRTOS 任务生命周期与关断设计：actuator 协调任务 50ms 轮询 + 服务端设备计时自动 OFF；stop() 先锁存 stop_requested 再释放互斥量 join（避免与任务 emergency 处理死锁）；terminal exactly once；输出互锁经由 safety_manager 单一网关。 |
| `/esp32` | 已调用 | ESP32-S3/ILI9341/LVGL 约束：绘制 Wi-Fi/BLE 图标用 LVGL 图元（arc/triangle/line）而非缺失字形；输出指示灯形状+颜色双编码；避免内部 RAM 压力（图标色数组按页）。 |
| `/debugging-code` | 已调用 | 烧录后出现启动循环（`assert xQueueGenericSend (pxQueue)`），用 addr2line 反解析崩溃 Backtrace 定位到 `actuator_self_test_init`，根因是 `memset(&s_rt)` 清空了刚创建的互斥量句柄；修复为互斥量置于 s_rt 之外。 |
| `/code-review` | UNAVAILABLE（无独立 skill） | 用 general-purpose agent 做独立 P0/P1 审查（见 §9），另做结构化人工审查替代。 |
| `/frontend-design` | 存在（web UI skill） | 其为 Web 前端设计 skill，与 320×240 LVGL 嵌入式布局不直接等价。应用其「信息层级 / 像素布局 / 状态表达」原则并做了嵌入式适配：页面2 固定 3+2+2 卡片像素几何、SYSTEM 卡 4 行关键信息优先、OUTPUTS 形状+颜色双编码防色弱误判。UI 具体实现遵循任务给定像素坐标。 |

## 1. 基线与保护

- `git status --short`：工作树脏（大量历史构建产物），全程未做任何破坏性 git 操作；未删除未知/未跟踪文件。
- 初始主固件基线：
  - BIN SHA256 `53a94cf9f92f30c99aca84b069db4001af6fcc803e9a0e77bf1cbbe0ae488274`
  - ELF SHA256 `d95196975e5d950b2fc18038c233b13bc0e72ef270804fb6c844eda73f063703`
- 未覆盖 Config/NVS/API Key；只修改本任务相关文件。

### FILES_CHANGED

固件（ESP32-IDF）：
- `components/actuator_self_test/`（新增：actuator_self_test.c / actuator_self_test_core.c / include/*.h / CMakeLists.txt）
- `components/uv_service/`（uv_service.c、uv_fsm.c、uv_fsm.h 枚举显式映射）
- `components/detergent_service/`（detergent_service.c、detergent_fsm.c、detergent_fsm.h 同类修复）
- `components/uv_self_test/`（uv_self_test.c stop 关灯确认/stale 门禁、uv_self_test_core.c/.h terminal 映射）
- `components/water_service/`（water_service.c/.h 受控单阀入口 + 设备端自动关阀 + emergency 门禁）
- `components/app_protocol/`（app_protocol.c/.h、app_protocol_core.c/.h：actuator_self_test 命令/ACK/capabilities/status）
- `components/display_service/`（display_service.c 三页精修+图标、display_model.c 后端切换文案、display_service.h）
- `main/`（app_bootstrap.c 接线/断线紧急/teardown、CMakeLists.txt）

测试：
- `host_tests/test_uv_act_core.c`（新增）、`test_display_model.c`、`CMakeLists.txt`
- `test_apps/unit/main/phase9_actuator_tests.c`（新增 Fake HAL 隔离）、`CMakeLists.txt`、`test_runner.c`（枚举断言更新）
- `tools/phase10_actuator_monitor.py`（新增监控）

小程序（miniapp4.0）：
- `miniprogram/pages/wash/wash.js` / `wash.wxml` / `wash.wxss`（三个板测按钮+状态）、`miniprogram/utils/version.js`
- `tools/test_act_selftest_flow.js`（新增 20 用例）

## 2. UV 审计修复（P1 已修复）

### UV_ENUM_MAPPING
`uv_service.c:get_snapshot` 不再直接强转 `(uv_state_t)fsm.state`。新增显式映射 `uv_fsm_state_to_public()`（`uv_fsm.c`，声明于 `uv_fsm.h`），穷举全部 8 个内部状态（IDLE→IDLE, VALIDATING→VALIDATING, … FAULT→FAULT），未知值映射为安全 FAULT，绝不误报 IDLE。detergent 同缺陷一并修复（`detergent_fsm_state_to_public`）。`uv_self_test` 不再用硬编码内部数值，改用公共枚举。单元测试断言同步更新。Host 测试覆盖全部状态映射（test_uv_act_core 38/38）。

### UV_STOP_OFF_CONFIRMED
`uv_self_test_stop()` 重写：先请求 `uv_service_emergency_stop()`，从快照确认 `output_state == KNOWN_OFF`（2.5s 有界等待）；确认失败/超时返回明确错误并**保留生命周期资源**（不销毁 started/initialized），允许调用者重试；只有确认 OFF 后才清除。

### UV_STALE_GATE
`uv_self_test.c:collect_gate_input` 修复：`pos_valid = view.sample_valid`（样本是否存在），`pos_fresh = view.fresh`（新鲜度单独表达）。此前 `sample_valid && fresh` 把 stale 折叠成 POSITION_UNKNOWN，使 POSITION_STALE 门禁不可达。现在 `valid=true fresh=false → POSITION_STALE` 拒绝且 UV 不亮（Host 测试覆盖）。

### UV_TERMINAL_SEMANTICS
`map_uv_terminal` 移到纯 core（`uv_self_test_map_terminal`）：COMPLETE→COMPLETE、REJECTED→REJECTED、FAULT→FAULT、CANCELED/SKIPPED→INTERRUPTED、TIMEOUT→TIMEOUT、NONE/未知→UNKNOWN。绝不把 canceled/skip/timeout 折叠成 COMPLETE。`uv_self_test_state_t` 扩展 INTERRUPTED/TIMEOUT/UNKNOWN。小程序 status 常量与轮询同步更新（UV_SELF_TEST_STATUS 7/8/9）。

### BLE_DISCONNECT_WORDING
小程序 BLE 断线文案从「设备已执行关灯」改为「**已请求紧急关灯，状态待设备确认**」；只有设备状态确认 OFF 才显示「UV 已关闭」。执行器板测同理「已请求紧急关闭，状态待设备确认」。

## 3. 执行器安全板测框架

### ACTUATOR_SELFTEST_ARCHITECTURE
```
components/actuator_self_test/
├── include/actuator_self_test_core.h   纯 C 类型：目标/状态/终态/门禁/固定时长
├── include/actuator_self_test.h        运行时 API
├── actuator_self_test_core.c           纯 C 门禁判定、拒绝码、request_id、target 解析
├── actuator_self_test.c                FreeRTOS 协调任务（50ms 轮询服务快照推进状态机）
└── CMakeLists.txt
```
- water_service 提供受控单阀入口：`water_service_actuator_valve_test/close`（经 water 任务单写者执行，设备端计时自动关阀）。
- detergent_service 复用 `detergent_service_run_async(800ms)`（设备端 FSM 自动关泵）。
- 所有真实输出一律经 `safety_manager_apply`（water 的 `apply_valve`、detergent 的 `apply_pump_*`）。

### 路径
| 目标 | UI 名称 | 通道 | MCP | 安全操作 | 固定时长 |
|------|--------|------|-----|---------|---------|
| SOURCE_VALVE | 有压进水阀 | IN | GPA0 | SAFETY_OP_SOURCE_INLET_VALVE | 1500 ms |
| TRANSFER_VALVE | 零压转移阀 | TR | GPA1 | SAFETY_OP_TRANSFER_VALVE | 1500 ms |
| DETERGENT_PUMP | 洗涤剂蠕动泵 | DT | GPB7 | SAFETY_OP_DETERGENT_PUMP | 800 ms |

时长由固件固定（`actuator_self_test_duration_ms`），客户端不可覆盖。

### 门禁（15 项，pure core 可测）
executor IDLE、无其他自检（自身 + UV）、无 fault、无 emergency、位置有效/新鲜/稳定/未运动/0°、目标 MCP 已知、目标确认 OFF、互斥输出确认 OFF、water/detergent 服务空闲、request_id 非 0 单调。拒绝码：`EXECUTOR_BUSY / SELF_TEST_BUSY / FAULT_ACTIVE / EMERGENCY_ACTIVE / POSITION_UNKNOWN / POSITION_STALE / POSITION_UNSTABLE / MOTOR_MOVING / POSITION_NOT_ZERO / OUTPUT_STATE_UNKNOWN / OUTPUT_ALREADY_ON / CONFLICT_OUTPUT_ON / SERVICE_BUSY / REQUEST_ID_INVALID / OFF_CONFIRM_FAILED`。

互锁：SOURCE↔TRANSFER 不同时 ON、二者↔DRAIN 不同时 ON（门禁预检 + safety_manager VALVE_CONFLICT 原子联锁）；蠕动泵不绕过 safety_manager 现有门禁。

### 设备端自动关断证明
- water：`water_task` 内 `act_test_close()` 在 `now - start >= duration` 时自动关阀；即使 BLE 断开也执行。
- detergent：FSM 到期 `PUMP_OFF`。
- BLE 断线：app_bootstrap → `actuator_self_test_emergency()` → water 紧急关阀 + detergent emergency。
- 关断失败：`act_test_close` 失败 → WATER_ACT_TEST_FAULT；detergent terminal COMPLETE 但输出非 OFF → FAULT；紧急关断 OFF 失败 → FAULT（不吞掉）。
- stop/deinit 先确认 OFF，失败保留生命周期可重试。

### RAW_MCP_BYPASS_COUNT / RAW_HAL_BYPASS_COUNT
静态扫描：actuator 组件无 mcp23017/HAL `set_safe_output` 直接调用，全部经服务 + safety_manager。均为 0。

## 4. 协议

- 命令：`{"cmd":"actuator_self_test","seq":N,"target":"source_valve|transfer_valve|detergent_pump"}`。
- 成功 ACK：`{"v":1,"type":"ack","seq":N,"ok":true,"code":"ACTUATOR_SELF_TEST_ACCEPTED","target":"source_valve","duration_ms":1500}`。
- 拒绝：machine-readable 拒绝码（nack）。
- capabilities：`"diagnostics":["uv_self_test","actuator_self_test"]` + `"actuator_self_test_targets":["source_valve","transfer_valve","detergent_pump"]`。
- status：`"actuator_self_test":{"st":..,"tg":..,"rid":..,"dur":..,"off":..}`（state/target/request_id/duration/output_confirmed_off）。
- 状态机：INACTIVE/CHECKING/ACCEPTED/RUNNING/COMPLETE/REJECTED/INTERRUPTED/TIMEOUT/FAULT/UNKNOWN。
- seq cache 去重 → 重复帧重放原 ACK，不延长动作；request_id 单调跳过 0。

## 5. 三页显示 UI 精修

### DISPLAY_PAGE_COUNT = 3（运行总览 / 设备遥测 / 语音助手），未恢复 4 页。
- 顶栏：标题+进度条+百分比+心跳+Wi-Fi/BLE 图标。空闲只显示暗色轨道、百分比 `--`；运行时青色填充+真实百分比。Wi-Fi/BLE 用 LVGL 图元绘制（Wi-Fi=基座点+3 段圆弧；BLE=中柱+双翼），颜色语义：Wi-Fi 灰/橙/青/红（断/连/有 IP/失败），BLE 灰/蓝/绿/红（停/广播/连接/恢复失败）。心跳轮廓形态变化（border width + radius + fill opa）。心跳/Wi-Fi/BLE 间距 ≥6px。普通卡片暗蓝 1px，顶栏青色 2px；仅活动/心跳/故障高亮。
- 第一页：任务卡（阶段/剩余/当前/下一步，删除与顶栏重复的百分比）；流程区 198px（6 节点：已完成暗绿/当前亮青/未开始灰/故障红）；姿态区 98px（桶位+对齐）；核心指标统一单位：水量 `0 mL`、流量 `%up`（固件无 L/min 速率接口，不虚构）、温度 `26.9C`、湿度 `32%RH`、水位 `OK/FULL`、UV `ON/OFF/UNKNOWN`。浊度 EST/UNCAL 不当作标定值（page2 QUALITY 显示 `~xxx NTU`）。
- 第二页：WATER 98 / QUALITY 98 / ENV 98；MOTION 98 / INPUTS 201（**六路霍尔 0/45/90/180/270/BL**，来自 stable_hall_mask，水位不再混入 INPUTS）；OUTPUTS 198 / SYSTEM 98。删除独立 LINK 卡（Wi-Fi/BLE 已在顶栏常驻）。OUTPUTS 形状+颜色双编码：绿实心圆=ON、灰空心圆=OFF、黄方框=UNKNOWN、红叉=FAULT。SYSTEM：MEM / PAJ / MCP / FAULT（MCP 来自 mcp_unknown_mask）。
- 第三页：状态卡 40px / 用户文本 44px（2 行）/ 回复 84px（4 行），底部指示器（上移 2px，y=226）不裁切。状态卡第一行左状态右后端，第二行麦克风「采集中」扫描动画（非伪装音量表）+ 云端 READY/OFFLINE（诚实推断自联网）。后端切换全文案：`正在切换 → X` / `已切换：X` / `切换失败，仍为X` / `切换超时，保持X`；切换中不提前提交 target。

### UTF8_GLYPH_RESULT
解析 telemetry_font cmap 校验：display_service.c + display_model.c 中全部 232 个非 ASCII 字符（含 → U+2192、：U+FF1A）均被字形覆盖，缺失 0。动态云端回复经 `display_utf8_sanitize` 兜底，绝不出现 tofu。

### LVGL_SINGLE_WRITER
`refresh_header`/`refresh_telemetry` 写入的图标色数组与 `s_output_led_state[6]` 仅在 LVGL 任务（`refresh_ui`）写、LVGL 绘制回调读；无外部任务调用 `lv_*`。刷新路径不创建/销毁对象，无对象泄漏。

## 6. 测试

| 套件 | 结果 |
|------|------|
| Host C（MSVC） | **368/368**（test_runner 308 + voice_vad 4 + display_model 18 + uv_act_core 38） |
| Python host | **39/39**（phase1_host 23 + runner_offline 16；ble_phase8 需 bleak 模块+真机，环境缺 bleak 未跑） |
| 小程序 | **171/171**（act_selftest 20 + uv_selftest 12 + uv_order 15 + ble_protocol 8 + page_contract 74 + booking_flow 23 + ble_transport 19） |
| 单元固件 | **构建通过**（含新增 phase9_actuator_tests.c 7 个 Fake HAL 隔离用例，编译 OK；本会话未烧单元固件以保留主固件烧录/监控流程，Fake HAL 用例经编译验证） |
| 主固件 build | PASS（BIN 0x3b6e70 → 大小 3922672 B，12% free） |

Host 覆盖：UV 枚举全映射、UV stop 关灯确认（纯门禁侧）、POSITION_STALE 运行时可达、canceled/skip/timeout 不映射 COMPLETE；actuator 三目标、非法目标、executor/fault/emergency/pos*/service busy/output 冲突/request_id 门禁矩阵、固定时长不可覆盖、request_id 跳过 0。
Fake HAL 单元覆盖（编译验证）：SOURCE 只改 GPA0、TRANSFER 只改 GPA1、DETERGENT 只改 GPB7、不触碰 UV/排水/PTC、非法参数零硬件写、water emergency 双阀 OFF、detergent emergency OFF、全部写经 set_safe_output。

## 7. 构建 / 烧录 / 监控

- MAIN_BUILD：PASS，无警告（`-Werror`）。
- UNIT_BUILD：PASS。
- 静态扫描：显示路径无 `delay()`、无旧 4 页常量、无 emoji、无 unsupported glyph；actuator 无 raw MCP/HAL 旁路。
- API_KEY_LEAK_SCAN：干净（仅既有配置引用，无新增硬编码密钥）。
- PORT：COM8。FLASH_RESULT：5/5 分区 Hash verified，硬复位。
- 首次烧录出现启动循环 → addr2line 定位并修复（互斥量清空）→ 重新烧录后干净启动：`display running (pages=3)`、`Bootstrap OK`、`GOT_IP 10.196.41.88`、heartbeat 持续。

## 8. 最终 Code Review（独立 agent + 结构化人工）

| 发现 | 严重度 | 处置 |
|------|--------|------|
| CMD_ACT_TEST 未受 emergency/stop 门禁（队列中的 OPEN 可能于 emergency OFF 后重新上电） | P1 | 修复：water 任务受理板测前检查 `emergency_requested/stop_requested/shutdown_draining` |
| actuator stop() join 脆弱、僵尸任务、task_done_sem 复用竞态 | P1 | 修复：先锁存 stop 再释放互斥量 join、join 超时保留生命周期、start 每次重建 task_done_sem |
| `s_rt.test_active`/`s_rt.task` 跨任务非原子读 | P1 | 修复：test_active 改 `_Atomic`，emergency 在锁内捕获 task 句柄 |
| 紧急关断 OFF 失败被 terminal-exactly-once 吞掉 | P1 | 修复：紧急关断 OFF 失败 → FAULT（而非 INTERRUPTED） |
| water_service_stop 持 lc_mtx 注销 hook 的锁序（既有代码，触发器已被上一条消除） | P1/PLAUSIBLE | 记录为既有 P2；僵尸任务修复后不可达 |

CODE_REVIEW_P0=0，CODE_REVIEW_P1=0（5 项 P1 中 4 项已修复、1 项既有触发器已消除），CODE_REVIEW_P2=2（既有锁序注记 + act_emergency_outputs 错误聚合）。

## 9. 监控（稳定性）结果

用户要求提前终止完整 15 分钟监控，改为即时采集。已采集证据：
- 启动日志（重烧后 30s）：`rst:0x1` 一次（上电），无 `RTC_SW_CPU_RST` 循环，0 assert/Guru/panic；`display running (pages=3)`、`Bootstrap OK`、`GOT_IP 10.196.41.88`、`heartbeat tick=100`。
- 存活确认（重烧后约 7 分钟，日志时间戳 `t=421101ms`）：15s 窗口内 RST=0、ASSERT/Guru/panic=0；telemetry `UI_SENSOR` 持续；`iB=13267968`（internal free ≈12.6 MiB）。
- 完整 15 分钟监控被用户终止，未取得整段数据；本固件监控日志未产生 300s 周期输出。

```
MONITOR_DURATION: 终止于用户请求（约 1 分钟）；设备侧已验证约 7 分钟连续无复位
RESET_COUNT: 0（启动后 7 分钟无复位）
WDT_COUNT: 0
GURU_COUNT: 0
PANIC_ABORT_COUNT: 0
HEARTBEAT: 启动段确认（tick=100 正常推进）
TELEMETRY: UI_SENSOR 持续输出
HEAP_START/END/MIN: 单点 iB=13267968（internal free ≈12.6 MiB），未做长期趋势
```

> 诚实说明：完整 15 分钟稳定性监控因用户终止未跑满；以上为启动段 + 7 分钟连续无复位 + 单点堆取样的证据，不等于完整 soak 结论。

## 10. 门禁与用户实测准备

固件已烧录并处于安全 OFF 状态（未自动触发任何真实阀/泵）。烧录后设备停在「等待用户点击测试」状态。

| 字段 | 值 |
|------|-----|
| PHASE9_ACTUATOR_SELFTEST_SOFTWARE_GATE | PASS（P0=0, P1=0, 软件退出门真实通过） |
| PHASE9_DISPLAY_REFINEMENT_SOFTWARE_GATE | PASS（代码+构建+字形+静态通过） |
| READY_FOR_CODEX_REVIEW | YES（软件侧） |
| NEEDS_USER_SOURCE_VALVE_TEST | YES — 接好水路+容器，点自检，IN 亮约 1.5s，阀动作，自动关 |
| NEEDS_USER_TRANSFER_VALVE_TEST | YES — 中间箱加水，点自检，TR 亮约 1.5s，水转移，自动关 |
| NEEDS_USER_DETERGENT_PUMP_TEST | YES — 泵管安装，点自检，DT 亮约 0.8s，泵动作，自动关 |
| NEEDS_USER_UI_VISUAL_CONFIRMATION | YES — 需目视三页顶栏/图标/流程节点/OUTPUTS 形状与颜色/语音页几何 |
| NEEDS_USER_BLE_DISCONNECT_TEST | YES — 任一自检运行中断开 BLE，输出应请求关闭、状态显示 INTERRUPTED/UNKNOWN |

用户实测顺序（见 §8 任务要求）：有压进水阀 → 零压转移阀 → 蠕动泵 → 任一运行中断开 BLE 验证断线关断。

## 最终固件

- BIN SHA256：`b61bcac6f62b3cb56343cc667600268b3cda702372b6eb330b52ad3ebe7a1ebf`（3922672 B）
- ELF SHA256：`acfa424812d761729d04b78ff52f50c188aa1e95d1d1827f0cc6cbf92e94dfcc`
