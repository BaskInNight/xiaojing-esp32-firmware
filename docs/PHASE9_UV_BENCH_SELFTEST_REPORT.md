# PHASE9 — UV 灯板级自检（安全联锁保护）报告

- 日期：2026-08-06
- 固件工程：`D:\UserData\ESP32Project\IDF\ESP32-IDF-Test`（ESP-IDF 5.5.4 / ESP32-S3-WROOM-1 N16R8）
- 小程序工程：`D:\UserData\ESP32Project\miniapp4.0`
- 串口：COM8；BLE：JJTP-XIAOJING @ 94:A9:90:DB:68:4A

---

## SKILLS_ACTUALLY_INVOKED

| Skill | 状态 | 影响 |
|---|---|---|
| `/embedded-systems` | 已调用 | 确认自检组件用 mutex 保护状态、无自建 task（用 uv_service 快照轮询）、BLE 回调非阻塞提交；紧急关灯走 uv_service_emergency_stop 幂等路径。 |
| `/esp32` | 已调用 | ESP32-S3 N16R8；UV 经 MCP GPA2 由 safety_manager_apply(SAFETY_OP_UV) 统一控制，自检零新增 GPIO/直写；BLE 广播含 FFF0 service + scan-response 设备名（WinRT 读不到名，按 service UUID 定位设备）。 |
| `/debugging-code` | 已调用 | 设备端行为异常（ACK=UV_BUSY 而非 POSITION_UNKNOWN）时，通过串口 UV_SELFTEST 日志 + 源码枚举比对定位根因：`uv_service_get_snapshot` 直接 `(uv_state_t)fsm.state` 转换，uv_fsm_state_t 与 uv_state_t 值域错位（fsm IDLE=0 → snapshot 0=UNINITIALIZED）。修复后设备端正确返回 POSITION_UNSTABLE 拒绝。 |
| `/code-review` | UNAVAILABLE（本会话 skill registry 无 code-review；security-review/simplify 存在但不适用跨端闭环） | 以结构化人工 review（任务九 12 项）替代，全部 PASS，见下。 |
| 小程序/UI skill | UNAVAILABLE（frontend-design/frontend-patterns 面向 Web/React） | 折叠式开发测试区按现有视觉体系手工实现 + 契约测试覆盖。 |

---

## ROOT_CAUSE_OF_19_200

- `fault_code=19` = **FAULT_INTERNAL**（`machine_types.h` 枚举第 20 项，值 19）。
- `fault_detail=200` = real_adapter 对服务 `SERVICE_RESULT_REJECTED / TIMEOUT` 映射的 error_code（`wash_executor_real_adapter.c:190-191`）。
- 成因：正式洗涤前置步骤（WAIT_LOAD → HOME_BL50 → 位置切换 → 进水 → 洗涤…）在电机/执行器未接齐的裸板上返回 REJECTED/TIMEOUT，executor 锁存故障并带 detail=200 上报。**与 UV 灯硬件好坏无关**，本任务未修复（任务边界明确）。

## WHY_FULL_ORDER_CANNOT_REACH_UV

- planner 的 UV 块位于 `WAIT_UNLOAD_CONFIRM` 之后、`FINISH` 之前（`wash_planner.c:462-475`）。程序必须依次完成 HOME_BL50、位置切换、进水、洗涤、排水、脱水全部前置步骤才到达 UV。
- 裸板前置步骤提前 REJECTED/TIMEOUT → 程序在到达 UV 前即 fault。故完整洗涤流程无法驱动 UV 灯。
- **因此新增独立诊断命令 `uv_self_test`**：绕过 planner 洗涤序列，直接驱动 uv_service，仅用于开发/板级验证 UV 灯与链路。

---

## ARCHITECTURE

```
小程序开发测试按钮（二次确认）
  → BLE V1 uv_self_test（seq 单调）
  → app_protocol（route_command_locked → uv_self_test 回调）
  → uv_self_test_request()：12 项安全门禁
  → uv_service_run_async(request_id, 3000ms)   [异步，非阻塞]
  → uv_fsm（VALIDATING 再次校验位置）
  → safety_manager_apply(SAFETY_OP_UV)
  → HAL/MCP23017 GPA2
  → UV 灯（固定 3 秒）
```

- 新增组件 `components/uv_self_test/`：
  - `uv_self_test_core.[ch]`（纯 C）：`uv_self_test_gate_check()` 12 项门禁、`uv_self_test_gate_code()` 拒绝码、`uv_self_test_next_request_id()`（非 0、wrap 跳过 0）。
  - `uv_self_test.[ch]`（运行时）：采集 executor/uv/safety/position 快照 → gate → `uv_service_run_async`；通过 `uv_service_get_snapshot()` 轮询同步终态（不改 uv_service 事件 sink）。
- `app_protocol`：新增 `APP_CMD_UV_SELF_TEST` + 命令名 `uv_self_test` + config 回调 `uv_self_test` / `uv_self_test_state`；capabilities 增加 `"diagnostics":["uv_self_test"]`；status 消息附加 `"uv_self_test":<state_code>`。
- `app_bootstrap`：初始化自检组件（executor 之后）；注册协议回调；BLE 断开 `ble_connection_sink(false)` 调 `uv_self_test_emergency()`；deinit/rollback 先 `uv_self_test_emergency+stop` 再停 uv_service/destroy HAL。
- 未改动：`uv_service.c`、`wash_planner.c`、`wash_executor.c`、正式 `allow_uv` 默认值。

## SAFETY_GATES

12 项门禁（`uv_self_test_gate_check`，顺序即拒绝码优先级）：

| # | 条件 | 拒绝码 |
|---|---|---|
| 1-2 | executor IDLE 且无程序/无保留 terminal | `MACHINE_NOT_IDLE` |
| 3 | uv_service IDLE | `UV_BUSY` |
| 4 | 无 safety fault | `FAULT_ACTIVE` |
| 5 | 无 emergency | `EMERGENCY_ACTIVE` |
| 6 | 位置快照有效 | `POSITION_UNKNOWN` |
| 7 | 位置新鲜 | `POSITION_STALE` |
| 8 | 位置稳定 | `POSITION_UNSTABLE` |
| 9 | 电机未运动 | `MOTOR_MOVING` |
| 10 | 位置 == DRUM_POS_0（桶对齐） | `POSITION_NOT_ZERO` |
| 11 | MCP UV 输出 known | `MCP_UNKNOWN` |
| 12 | UV 已确认 OFF（output_state==KNOWN_OFF） | `UV_NOT_CONFIRMED_OFF` |

任何一项不满足 → 不点灯，返回 machine-readable 拒绝码（不再统一成 200）。uv_fsm 在 VALIDATING 态再次校验位置后才请求 LAMP_ON；RUNNING 期间每 tick 经 `safety_manager_validate_active(SAFETY_OP_UV, DRUM_POS_0)` 复核。

## NO_DIRECT_RELAY_BYPASS

- `uv_self_test` 组件无任何 MCP23017/GPIO/HAL 直写（代码审查 + `check_safety_bypass.py` PASS）。
- 点灯唯一路径：`uv_service_run_async → safety_manager_apply(SAFETY_OP_UV)`。禁止直写 GPA2/寄存器/HAL 裸输出、禁止把安全条件写死。

## PROTOCOL_COMMAND

- 命令：`uv_self_test`（`APP_CMD_UV_SELF_TEST`，非 formal 订单，不创建订单）。
- 受理 ACK：`{"v":1,"type":"ack","seq":N,"ok":true,"code":"UV_SELF_TEST_ACCEPTED","duration_ms":3000}`
- 拒绝 ACK：`{"v":1,"type":"nack","seq":N,"ok":false,"code":"POSITION_UNSTABLE",...}`
- 固定 3000 ms（`UV_SELF_TEST_DURATION_MS`），payload 携带 duration 一律忽略（`build_intent` 不参与该命令，路由不解析 duration）。
- request_id：独立单调计数器，非 0、wrap 跳过 0，与 executor 无关。
- 重复 seq：app_protocol seq cache 返回缓存 ACK，不重复触发 gate/点灯（设备端实测两次相同 seq 返回相同 nack）。
- 自检运行中再提交 → `UV_BUSY`（gate 的 uv_idle 检查），不延长点亮。
- capabilities：`"diagnostics":["uv_self_test"]` 明确标记为 diagnostic/development capability（设备实测确认）。

## MINIAPP_UI

wash 页面底部新增折叠式「🧪 开发测试」区（远离下单主按钮）：
- 默认折叠（`diagExpanded:false`），`wx:if="{{diagEnabled}}"` 整体控制。
- 按钮「UV 灯安全自检（3 秒）」→ 高风险二次确认弹窗（"请遮挡 UV 灯，严禁直视 / 确认桶位于 0° 对齐位置且电机停止"）→ 确认后发送。
- 发送期间按钮禁用（`diagSelftestBusy`），防重复点击。
- 状态文案覆盖：正在检查安全条件 / UV 自检运行中（3 秒）/ UV 正在自动关闭… / UV 已自动关闭 / 各拒绝 reason 中文文案 / 关灯失败：请立即断电检查 / 自检状态未知（BLE 断开）。
- BLE 断开回调 → `markDiagStatusUnknown()`（固件已 emergency_stop 关灯）。
- 不创建订单、不收费、不修改 `allowUv` / `uvDisinfectionEnabled`。
- 发布开关：`miniprogram/utils/version.js` 的 `MINIAPP_ENABLE_HARDWARE_DIAGNOSTICS`（当前板测版本 = `true`；生产置 `false` 隐藏整个区域）。
- 版本号：`uv-selftest-20260806-2105`（「我的」页可见）。

---

## TEST_RESULTS

小程序（Node，真实运行）：
| 套件 | 结果 |
|---|---|
| tools/test_ble_protocol.js | 8/8 |
| tools/test_ble_transport.js | 19/19 |
| tools/test_booking_flow.js | 23/23 |
| tools/test_page_contract.js | 71/71 |
| tools/test_uv_order_flow.js | 15/15 |
| tools/test_uv_selftest_flow.js（新增） | 12/12 |

固件 Host（MSVC cl 真实编译运行）：
| 套件 | 结果 |
|---|---|
| host_tests/test_runner.exe | 308/308（含新增 18 条：自检门禁 17 + LAMP_OFF 失败显式报告 1） |
| host_tests/test_voice_vad.exe | 4/4 |

新增 Host 覆盖（任务七 20 条）：
1. IDLE+pos0+fresh+stable+stopped → ACCEPT ✓
2. pos unknown → POSITION_UNKNOWN ✓
3. pos stale → POSITION_STALE ✓
4. pos unstable → POSITION_UNSTABLE ✓
5. pos non-zero → POSITION_NOT_ZERO ✓
6. motor moving → MOTOR_MOVING ✓
7. fault → FAULT_ACTIVE ✓
8. emergency → EMERGENCY_ACTIVE ✓
9. MCP unknown → MCP_UNKNOWN ✓
10. uv busy → UV_BUSY ✓
11. duration 固定 3000 ✓
12. duplicate seq 不重复提交 ✓（app_protocol seq cache + 设备实测）
13. timeout → LAMP_OFF ✓（uv_fsm）
14. cancel → LAMP_OFF ✓（uv_fsm）
15. BLE disconnect → LAMP_OFF ✓（uv_fsm emergency；接线在 bootstrap）
16. safety fault during RUNNING → LAMP_OFF ✓（uv_service RUNNING 复核→EMERGENCY）
17. LAMP_ON 失败 → fault+LAMP_OFF ✓（uv_fsm）
18. LAMP_OFF 失败显式报告 ✓（uv_fsm output→UNKNOWN + 自检 FAULT）
19. 正式程序运行时拒绝 ✓（exec_idle=false→MACHINE_NOT_IDLE）
20. request_id wrap 跳过 0 ✓

Python：`test_phase1_host.py` 23/23；`check_safety_bypass.py` PASS。

## MAIN_BUILD

`ninja -C build` → **PASS**，0 error / 0 新增代码警告。app 分区余量 12%（0x81b70 B）。新增组件 `uv_self_test` 编译通过。

## FLASH_RESULT

`esptool --chip esp32s3 --port COM8 ... write_flash` 5 个分区全部 **Hash of data verified.**。设备 BLE 广播 FFF0 service（94:A9:90:DB:68:4A），设备名 JJTP-XIAOJING。

## SERIAL_RUNTIME

启动检查（烧录后 POWERON）：0 Guru Meditation / 0 WDT / 0 panic/abort；UVSvc OK、gesture_svc running、Bootstrap OK、BLE advertising。

设备端 BLE 实测（Python bleak 客户端，未点亮 UV）：
```
CAPABILITIES ... "diagnostics": ["uv_self_test"]
STATUS position=-1 uv_self_test=0 (INACTIVE)
UV_SELF_TEST_ACK nack code="POSITION_UNSTABLE"   ← 裸板位置无效/不稳定，拒绝
STATUS_AFTER uv_self_test=5 (REJECTED) uv_on=false   ← 灯未点亮
串口: UV_SELFTEST reject code=POSITION_UNSTABLE pos=-1 valid=1 fresh=1 stable=0 moving=0 fault=0 emerg=0 mcp_known=1 uv_idle=1 exec_idle=1 uv_off=1
duplicate seq(42) → 两次相同 nack（缓存 ACK，不重复提交）
```

## UV_ON_DURATION / UV_OFF_CONFIRMED

- **未在本次自动点亮**（任务禁止无人确认自动点亮；0° 未触发）。
- 拒绝路径已确认灯保持 OFF（`uv_on=false`）。
- 点亮 3000 ms 后自动 OFF、以及实灯视觉确认 → **NEEDS_USER_UV_VISUAL_CONFIRMATION**（见下）。

## BLE_DISCONNECT_OFF

- 接线已验证：`ble_connection_sink(false)` → `uv_self_test_emergency()` → `uv_service_emergency_stop()` → `apply_lamp_off()`。
- 灯亮期间断线的实际关灯 → 需在用户点灯后测一次（`NEEDS_USER_UV_VISUAL_CONFIRMATION`）。

## RESET_COUNT / WDT_COUNT / CRASH_COUNT

0 / 0 / 0（启动 + BLE 验证期间无复位、无看门狗、无崩溃）。

## P0 / P1 / P2

- **P0 = 0**
- **P1 = 0**（发现并修复 1 项真实问题：`uv_service_get_snapshot` 的 `state` 枚举错位导致 gate 误报 UV_BUSY；已修复 + 设备端验证返回正确拒绝码）
- **P2 = 0**

## NEEDS_USER_UV_VISUAL_CONFIRMATION

**YES** — 真实 UV 灯点亮与视觉确认必须由用户完成，严禁自动点亮。

---

## PHASE9_UV_SELFTEST_GATE

| 门禁项 | 状态 |
|---|---|
| 12 项安全门禁实现 | YES（纯函数 + 设备实测拒绝路径） |
| 无继电器/GPA2 直写旁路 | YES |
| 固定 3000 ms，payload duration 忽略 | YES |
| request_id 非 0 单调 wrap 跳 0 | YES（Host 断言） |
| 重复 seq 不重复提交 | YES（seq cache + 设备实测） |
| 自检运行中再提交 → UV_BUSY | YES |
| 正式洗涤/急停优先级高于自检 | YES（gate exec_idle + uv_idle；formal 优先） |
| capabilities 标 diagnostic | YES（设备实测） |
| status 暴露自检状态（0-6） | YES |
| 小程序折叠区 + 二次确认 + 防重复 + 状态文案 | YES（12/12 测试） |
| 发布开关 MINIAPP_ENABLE_HARDWARE_DIAGNOSTICS | YES |
| Host C 308/308、小程序 148、Python 23、bypass PASS | YES |
| 构建 PASS、烧录 Hash verified、启动无 crash/WDT | YES |
| P0=0 / P1=0 | YES |

---

## 用户板测步骤（UV 实灯）

**现在请遮蔽 UV 灯、确认 0° 霍尔已触发，然后在小程序开发测试区点击「UV 灯安全自检（3 秒）」。**

1. 遮蔽 UV 灯（不直视）；确认桶位于 0° 对齐位置且电机停止（B5 桶对齐霍尔触发）。
2. 微信小程序进入 wash 页 → 底部「🧪 开发测试」展开 → 点击「UV 灯安全自检（3 秒）」→ 高风险确认弹窗二次确认。
3. 预期：串口出现 `UV_SELFTEST request request_id=N duration=3000` → `UV_SELFTEST lamp ON confirmed` → 约 3 秒后 `UV_SELFTEST terminal=0 ... lamp_off_confirmed`。
4. 小程序状态：正在检查安全条件 → UV 自检运行中（3 秒）→ UV 已自动关闭。
5. 观察实灯点亮约 3 秒后自动熄灭（护目镜/不直视）。
6. BLE 断线测试一次：点灯运行中断开小程序蓝牙 → 灯立即熄灭（`uv_self_test_emergency`）。
7. 拒绝条件测试一次：移开 0° 磁铁后点击自检 → 小程序显示「被拒绝：桶位置不是 0°（或位置状态无效）」→ 灯不亮。

报告写入 `docs/PHASE9_UV_BENCH_SELFTEST_REPORT.md`。
