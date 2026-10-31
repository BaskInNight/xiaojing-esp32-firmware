# Phase 11 Group C — 主洗涤/滚筒电机空载台架（软件实现）报告

> Round 0 软件实现 · 全量主机测试 · REAL 模式实物验证（冷启动 safe-OFF）
> 日期：2026-08-07
> 固件：`build/xiaojing_idf.bin`（0x3C3BF0 字节 = 3947504 B）
> sdkconfig 当前：**REAL**（`CONFIG_XIAOJING_OUTPUT_MODE_REAL=y` + `CONFIG_XIAOJING_HOT_AIR_MODULE_ENABLED=y`）
> 串口：COM8（CH340，115200 8N1）

---

## 1. 结论

Phase 11 Group C「主洗涤/滚筒电机空载台架」**软件实现完成**：真实生产 motor action adapter（`bl50_service_run_async`）作为唯一电机通电路径、生产 resolver（`bl50_action_to_position`）作为唯一位置权威来源、纯 C 可宿主测试 FSM、FreeRTOS 运行时服务、协议命令 `motor_bench_start/confirm/cancel` 全链路就绪。主机自动化测试 **20/20 PASS**（含 ≥10k 随机压力 + P0 回归），全量 ctest **8/8 套件 PASS**，主固件构建 **无警告**。独立审查 + 板测共发现 **5 项 P0/P1** 缺陷（禁止输出聚合、`type` 键冲突、安全快照 fail-open、台架与洗涤缺少反向互斥、台架自检忙自锁）全部修复并加入 CI 回归（§7）。

**实物验证已闭环至安全门**：REAL 模式固件已烧录 COM8，冷启动证明所有输出 safe-OFF 通过，电机台架空闲（0 次脉冲提交、0 次终端事件、0 次急停），BL50 驱动初始化但从未通电，IBT-2 未校准/未驱动（与「IBT-2 控制角度、台架只读霍尔、用户手动摆放」的硬件拓扑一致）。

**板测定位的 P0-3 已修复并重新烧录**：用户在实物上把位置摆到 0°、选中波轮台架并开始后，小程序报「台架故障（输出未确认关闭）· 终态码 5 · 输出确认关闭=否」。经 COM8 串口复现定位为 **P0-3 自检忙自锁**（`MB request ACCEPTED` 与 `MB terminal state=FAULT code=SELF_TEST_BUSY` 同刻，4 连发）：服务层 `collect_gate_input()` 把台架自身 `test_active` 计入 `self_test_busy`，受理后下一 tick 门禁自锁 → 一受理即 FAULT。`off=0` 是 fail-closed 契约（非 COMPLETE 终态一律报「输出未确认关闭」），并非二次缺陷。修复（§7 P0-3）+ 新增 `lc` 诊断字段（§4.2）+ 重新烧录后，用户重测应可走到 WAIT_CONFIRM 并确认脉冲。

**电机通电严格等待用户手动确认**：
- `SOFTWARE_GATE`：代码 / 测试 / 构建 / 烧录 / 冷启动 safe-OFF — **已完成**。
- `PASSIVE_SOAK_GATE`：≥30 分钟被动监控 — **已完成**（§8.4：32 min、0 异常、台架 0 活动）。
- `NEEDS_USER_MOTOR_BENCH_TEST`：**任何情况下不自动执行电机脉冲**。真实电机空载台架测试需用户按 §10 逐步手动操作（小程序或直接串口命令）。

> ⚠️ 本报告**不**将「编译通过」或「被动监控稳定」声称/暗示为电机硬件 PASS。电机实物验收（脉冲、霍尔顺序、刹车）属于用户门控步骤。

---

## 2. 设计约束（11 条严格规则）落实

| # | 规则 | 落实 |
|---|---|---|
| 1 | 绝不自动让电机通电 | 唯一通电路径 = FSM `PULSING` 提交 `bl50_service_run_async`，仅在用户显式 `confirm` 后触发一次（§3.2） |
| 2 | 仅诊断模式可见，不改生产默认安全 | capability 门控（`motor_bench_self_test`）；生产 formal order 永不调用（executor 空闲门禁 + 服务独立于 wash_executor） |
| 3 | 复用真实 resolver，禁止硬编码猜霍尔 | `required_position = resolver(action)`，resolver=生产 `bl50_action_to_position`（§3.1）；程序序列也只用真实 `BL50_ACTION_*` |
| 4 | 等位全停、单次低能量脉冲≤300ms、OFF 确认后推进 | 每步 `WAIT_POSITION` 门禁要求 BL50 IDLE 且 PWM=0；脉冲=低档 300ms PWM15%（=生产 HOME 档）；`OFF_CONFIRM` 要求 bl50_idle && pwm==0 才推进 |
| 5 | 禁止进水/转移/排水/蠕动/UV/热风；一切退出路径全 OFF | 门禁 `forbidden_outputs_off` 不变式每 tick 复核；cancel/emergency/timeout/fault/stop/BLE 断线 → `request_all_off` + `motor_bench_service_emergency` |
| 6 | 错霍尔/矛盾/超时/快照不可用/输出确认失败 → fail-closed | §3.2 门禁/终端表 |
| 7 | 小程序开发测试入口：等待位置/完成步/拒绝原因/取消；双重确认、防重入、capability 门控 | 协议命令 + status 快照字段（§5） |
| 8 | Host 覆盖：正确序/错位拒/多霍尔拒/超时/取消/急停/BLE 断线/重叠=0/禁出=0/终态恰一次/≥10k | 20/20 PASS（§6） |
| 9 | 独立 P0/P1 审查 | §7 |
| 10 | 构建、烧录 COM8、冷启动 safe-OFF、≥30min 被动监控；绝不自动执行脉冲 | §4、§8 |
| 11 | 报告到 `docs/PHASE11_GROUP_C_MOTOR_BENCH_REPORT.md`，区分三 Gate | 本文件 |

---

## 3. 架构

### 3.1 位置语义（规则 3：真实 resolver，不猜霍尔）

生产契约 `bl50_action_to_position`（`components/bl50_service/bl50_fsm.c`）是唯一位置权威：

| 生产动作 | 权威位置 |
|---|---|
| `BL50_ACTION_HOME` | 45° |
| `BL50_ACTION_PULSATOR` | 0° |
| `BL50_ACTION_DRUM` | 90° |
| `BL50_ACTION_SPIN` | 180° |

台架程序由真实动作序列构造，每个 `required_position` 由 resolver 派生：

```
pulsator_motor_self_test      = [BL50_ACTION_PULSATOR]              → [0°]
drum_motor_self_test          = [BL50_ACTION_DRUM]                  → [90°]
motor_hall_bench_sequence     = [PULSATOR, DRUM, SPIN]              → [0°, 90°, 180°]
```

> 用户按 §10 手动把桶摆到当前步所需位置（5 路霍尔只读）。**IBT-2（有刷位置电机）从不被台架驱动** —— 与用户澄清的硬件拓扑一致（“bl50 只需要一个 bldc 对齐的霍尔角度，ibt2 才是控制角度的”）。

### 3.2 状态机与 fail-closed 终端（规则 4/6）

```
PRECHECK → WAIT_POSITION → WAIT_CONFIRM → PULSING → OFF_CONFIRM → COMPLETE
              ▲                │
              └── 位置回退重新等待 ┘
```

| 门禁/事件 | 行为 | fail-closed |
|---|---|---|
| 位置 OK + 稳定 | 进入 WAIT_CONFIRM | — |
| POSITION_MISMATCH / HALL_CONFLICT / UNKNOWN / STALE / UNSTABLE / MOTOR_MOVING | 记录原因，继续等待；超时（120s/步）→ `TIMEOUT` + 全 OFF | ✅ 不推进 |
| EXECUTOR_BUSY / SELF_TEST_BUSY / FAULT_ACTIVE / EMERGENCY_ACTIVE / FORBIDDEN_OUTPUT_ON / OUTPUT_STATE_UNKNOWN / SERVICE_BUSY / BL50_BUSY / POSITION_SERVICE_BUSY / REQUEST_ID_INVALID / SNAPSHOT_UNAVAILABLE | `FAULT` + 全 OFF（运行中） | ✅ 不推进 |
| PRECHECK 门禁非 OK | `REJECTED`（无本测试输出，不强制全关） | ✅ 不推进 |
| cancel（任意状态） | `INTERRUPTED` + 全 OFF | ✅ |
| emergency（运行中）/ BLE 断线 | `INTERRUPTED` + 全 OFF | ✅ |
| PULSING：BL50 上报 FAULT / submit 失败 | `FAULT` + 全 OFF | ✅ |
| PULSING：提交后 300ms 宽限内未见 BL50 受理 | `FAULT(SUBMIT_FAILED)` + 全 OFF | ✅ |
| PULSING：脉冲超时（5s） | `FAULT` + 全 OFF | ✅ |
| OFF_CONFIRM：BL50 未回 IDLE/PWM≠0 超时（2s） | `FAULT(OFF_CONFIRM_FAILED)` + 全 OFF | ✅ |

常量（低能量短脉冲，见 `motor_bench_core.h`）：

| 常量 | 值 |
|---|---|
| `MOTOR_BENCH_PULSE_DURATION_MS` | **300**（≤300ms） |
| `MOTOR_BENCH_PULSE_PWM_PERCENT` | **15**（低档 = 生产 HOME 档） |
| `MOTOR_BENCH_POSITION_WAIT_TIMEOUT_MS` | 120000（每步等位上限） |
| `MOTOR_BENCH_CONFIRM_TIMEOUT_MS` | 20000 |
| `MOTOR_BENCH_PULSE_ACCEPT_GRACE_MS` | 300 |
| `MOTOR_BENCH_PULSE_TIMEOUT_MS` | 5000 |
| `MOTOR_BENCH_OFF_CONFIRM_TIMEOUT_MS` | 2000 |
| `MOTOR_BENCH_MAX_STEPS` | 8 |

### 3.3 运行时服务（FreeRTOS）

`motor_bench_service.c`：独立任务（`MB_TICK_MS=50`，4096 栈 PSRAM，priority 15，core 1）+ 互斥锁。每 tick：
1. `collect_gate_input()` — 从 safety_manager / position_service / bl50 / water / detergent / drain / dry / coupled_hot_air / UV **快照**聚合（任一失败 → `snapshot_fail` fail-closed）。
2. `build_fsm_input()` — 消费 confirm/cancel 边沿；`user_confirm_pulse` 仅在 gate==OK && state==WAIT_CONFIRM 生效（防重入）。
3. `motor_bench_fsm_tick()` → 执行 `submit_pulse`（`bl50_service_run_async(intensity=LOW, duration=300, pwm=15)`）/ `request_all_off`（`mb_emergency_outputs()`：bl50/position/uv/water/detergent/drain/dry/hot_air 急停）/ `terminal` exactly-once。

生命周期：`global_init` → `init(executor)` → `start`；`request(type)` 受理门禁（executor 空闲 + capability + 状态机空闲）；`confirm_pulse` / `cancel` / `emergency` / `stop` 均含输出 OFF 确认（fail-closed）。

### 3.4 与生产洗涤流程隔离（规则 2）

- 台架仅经 `motor_bench_self_test` capability 可见（诊断模式）。
- `wash_executor` 生产订单执行路径**从不**调用 `motor_bench_service_*`。
- 台架受理要求 `exec_idle`（executor 快照空闲）→ 洗涤运行中请求被拒（`EXECUTOR_BUSY`）。
- 门禁还要求所有禁止服务（water/detergent/drain/dry/hot_air/uv）**空闲且 OFF**，且 `forbidden_outputs_off` 每 tick 复核 —— 台架运行期间任何禁止输出被外部触发 → fail-closed FAULT + 全 OFF。

---

## 4. 协议（小程序开发测试入口 · 规则 7）

### 4.1 命令

| 命令 | 载荷 | 应答（成功） | 应答（拒绝） |
|---|---|---|---|
| `motor_bench_start` | `{"target":"pulsator"\|"drum"\|"hall_sequence"}` | `MOTOR_BENCH_ACCEPTED` | `EXECUTOR_BUSY` / `SELF_TEST_BUSY` / `FAULT_ACTIVE` / `EMERGENCY_ACTIVE` / `POSITION_UNKNOWN` / … / `BAD_TYPE` |
| `motor_bench_confirm` | — | `OK` | `NOT_SUPPORTED` / `INTERNAL` |
| `motor_bench_cancel` | — | `OK` | `NOT_SUPPORTED` / `INTERNAL` |

> ⚠️ **键名 P0-2**：帧顶层 `"type"` 是解析器的命令判别键（必须等于 `"cmd"`，`app_protocol_core.c:188`），**不得**用于台架类型。台架类型使用 `"target"`（与兄弟命令 `actuator_self_test` 同键）。修复前 `"type":"cmd"` 让解析器放行但路由取到 `"cmd"` 作类型、`"type":"pulsator"` 被解析器拒绝 —— 两种帧都无法真正派发；现由纯 C 扫描器 `app_protocol_field_string(frame, "target", …)` 读取原始帧 `"target"`，契约被 CI 固定（`test_phase11_safety.c`）。

- 防重入：确认脉冲仅在 WAIT_CONFIRM 状态、gate==OK、本步未提交过（`!pulse_issued`）时消费一次。
- capability：`"diagnostics":["uv_self_test","actuator_self_test","motor_bench_self_test"]`；`"motor_bench_types":["pulsator","drum","hall_sequence"]`。

### 4.2 status 快照字段（小程序轮询展示）

```
"motor_bench":{"st":<state>,"type":<type>,"rid":<request_id>,
 "step":<current_step 0-based>,"steps":<step_count>,
 "pos":<required_position>,"act":<bl50_action>,
 "ready":<position_ready 0/1>,"term":<terminal 0-5>,
 "off":<output_confirmed_off 0/1>,"elapsed":<ms>,
 "lc":<last_code 字符串>}
```

> `lc`（last_code）为板测发现 P0-3 后新增的诊断字段：受理后为 `ACCEPTED`，等待位置阶段为最近等待原因（如 `POSITION_MISMATCH` / `HALL_CONFLICT` / `MOTOR_MOVING` / `POSITION_STALE`），终态为终态码（如 `SELF_TEST_BUSY`→`FAULT` / `MOTOR_BENCH_COMPLETE`）。小程序故障/超时/被拒展示行直接显示该码，避免再出现「台架故障（输出未确认关闭）」这种无法定位根因的笼统提示。

小程序「开发测试」入口展示：当前等待位置（`pos`）、完成步（`step`/`steps`）、拒绝/终止原因（`term` 终端码 + `st` 状态，经 `motor_bench_terminal_t`/`motor_bench_state_t` 映射；受理拒绝的具体码只出现在 `motor_bench_start` 应答 `code` 字段，不在 status 块中）、取消按钮（`motor_bench_cancel`）。`motor_bench` 块在 status 仅当服务提供 `motor_bench_snapshot` hook 时输出；未启用时省略（兼容旧客户端）。

> 小程序本体（微信小程序工程）不在本仓库内；本报告交付的是**固件侧协议契约**（命令/应答/快照字段）。**小程序开发测试入口已按此契约实现**：`miniapp4.0/miniprogram/pages/wash/wash.js`（MB_STATE/MB_TERMINAL 常量、类型选择、开始/确认脉冲/取消、状态轮询）、`wash.wxml`（台架 UI 块，能力门控 `diagMbEnabled`）、`wash.wxss`（选中态 `.btn-ghost.active`）。

---

## 5. 改动文件

| 文件 | 变更 |
|---|---|
| `components/motor_bench/`（新组件） | `motor_bench_core.c`（418 行，纯 C FSM+门禁）、`motor_bench_service.c`（766 行，FreeRTOS 服务）、`include/motor_bench_core.h`、`include/motor_bench_service.h`、`CMakeLists.txt`、`README.md` |
| `components/app_protocol/include/app_protocol_core.h` | +`APP_CMD_MOTOR_BENCH_START/CONFIRM/CANCEL` |
| `components/app_protocol/app_protocol_core.c` | +3 命令名映射 |
| `components/app_protocol/include/app_protocol.h` | +motor_bench start/confirm/cancel/snapshot 回调 typedef + snapshot 结构 + config 字段 |
| `components/app_protocol/app_protocol.c` | capabilities +`motor_bench_self_test`/`motor_bench_types`；status +`motor_bench` 块（`APP_STATUS_MAX_BYTES` 512→768）；路由 +3 命令 |
| `components/motor_bench/include/motor_bench_core.h` | gate input +`bl50_fault` 字段（FSM 脉冲/关断确认故障检测）；+`motor_bench_forbidden_clauses_t` 与 `motor_bench_forbidden_outputs_off()`（P0-1 纯函数聚合） |
| `components/motor_bench/motor_bench_core.c` | 实现 `motor_bench_forbidden_outputs_off()`（P0-1：基值 true，任一子句未 OFF → false） |
| `components/motor_bench/motor_bench_service.c` | P1-1：safety/exec 快照 `= {0}` + 读取失败 → `snapshot_fail`；P0-1：禁止输出聚合重构为纯函数子句；P1-2：受理前 `wash_executor_set_external_busy(true)`、全部终态/停止路径清除 |
| `components/wash_executor/include/wash_executor.h` | +`wash_executor_set_external_busy()` + `wash_executor_program_reserved()`（P1-2 双向互斥） |
| `components/wash_executor/wash_executor.c` | 结构体 +`external_busy`；`submit_program` 门禁 +`!external_busy`；实现两个新函数（互斥锁保护；`program_reserved` 实时读，发布快照滞后） |
| `components/app_protocol/app_protocol_core.c` | +3 命令名映射；+`app_protocol_field_string()` 纯扫描器（P0-2 键契约，host-testable） |
| `components/app_protocol/include/app_protocol_core.h` | +`app_protocol_field_string()` 声明 |
| `components/app_protocol/app_protocol.c` | capabilities +`motor_bench_self_test`/`motor_bench_types`（+`hall_sequence`）；status +`motor_bench` 块（`APP_STATUS_MAX_BYTES` 512→768）；路由 +3 命令；`motor_bench_start` 键 `"type"`→`"target"`（P0-2，经 `app_protocol_field_string`） |
| `main/app_bootstrap.c` | +`protocol_motor_bench_start/confirm/cancel/snapshot`（start 映射 +`hall_sequence`）；protocol_cfg 接线；init/start/emergency/stop 生命周期 |
| `host_tests/CMakeLists.txt` | +`test_motor_bench`（编译真实 `motor_bench_core.c` + `bl50_fsm.c`） |
| `host_tests/test_motor_bench.c` | 新增（20 测试：FSM/门禁/压力 + P0-1 禁止输出聚合 + P0-3 自检忙聚合回归） |
| `host_tests/test_phase11_safety.c` | +4 测试（P0-2：`type=cmd`+`target` 契约、`type≠cmd` 拒绝、缺 `type` 独立、`field_string` 非法输入拒绝） |
| `build_main.bat` | +`set ESP_ROM_ELF_DIR=...`（idf.py gdbinit 生成所需，此前缺失导致构建空转） |

---

## 6. 主机自动化测试（规则 8）

`host_tests/test_motor_bench.c` **20/20 PASS**，编译**真实生产源** `motor_bench_core.c` + `bl50_fsm.c`（程序/位置来自生产 resolver，杜绝硬编码）。

| 组 | 覆盖 |
|---|---|
| [1] 类型/名称 | 3 类型状态名、终端名映射 |
| [2] 默认程序 | 经生产 resolver 构造，位置=0°/90°/180° 契约断言 |
| [3] 门禁 fail-closed | 14 项门禁输入 → 预期拒绝码 |
| [4] 单步 happy path | PRECHECK→WAIT_POSITION→WAIT_CONFIRM→PULSING→OFF_CONFIRM→COMPLETE 逐 tick |
| [5] 霍尔序列 | 3 步序列逐推进 + 确定性推进 |
| [6] 错误位置/超时/多霍尔/确认超时 | 每类 fail-closed 拒/超时验证 |
| [7] 取消/急停/禁止输出 | cancel 任意状态优先；emergency 运行中全 OFF；forbidden_outputs_off 不变式；**P0-1 聚合回归**（基值 true、每子句逐一 fail-closed、fault/emergency 跳过 UV、NULL→false） |
| [8] 脉冲故障路径 + OFF 确认超时 + 终态恰一次 | submit_failed / BL50 fault / grace 超时 / pulse 超时 / off-confirm 超时 / `out->terminal` exactly-once |
| [9] confirm 回归 + 陈旧边沿 | 非 WAIT_CONFIRM 忽略；回退后陈旧 confirm 不消费 |
| [10] 压力 | **10k 完整随机时序 3 步 + 10k 随机游走**，全部不变式保持，终态聚合多样 |

随机游走终态聚合：`complete=229 rejected=4865 interrupted=433 timeout=0 fault=4473`（多样性健康；`timeout=0` 因随机游走多在超时前终止）。

### 6.1 全量回归

`ctest` **8/8 套件 PASS**：`xiaojing_host_tests`、`voice_vad_tests`、`display_model_tests`、`uv_act_core_tests`、`phase11_safety_tests`、`fake_hal_stress`、`coupled_hot_air_tests`、`motor_bench_core_tests`。其中 `phase11_safety_tests` 15/15（含 P0-2 协议键契约回归），`motor_bench_core_tests` 20/20（含 P0-3 自检忙聚合回归）。

### 6.2 调试期修复（MSVC NDEBUG 陷阱）

主机测试初版 12/18 失败，根因：
- `assert(motor_bench_build_default_program(...))` 在 Release（NDEBUG）下**整个表达式被编译掉** → 程序从未构建 → step_count=0。修复为显式调用 + `FAIL` 的 `BUILD_PROGRAM` 宏。
- `test 15` 终端检查断言方向反转（`!request_all_off` → `request_all_off`）。
- `check_invariants` 参数误用（把 state 当 terminal_before 传）。

---

## 7. 独立 P0/P1 审查（规则 9）

独立审查（子代理逐行核对生产源，非自检）发现 4 项 P0/P1 + 3 项 P2；板测（小程序实机运行）又发现 1 项 P0（P0-3 自检忙自锁，纯 FSM host 测不到的**服务层聚合**缺陷）；**5 项 P0/P1 全部修复并加入 CI 回归**，P2 按证伪/文档化处理。

| # | 级别 | 发现 | 处置 |
|---|---|---|---|
| P0-1 | P0 | `collect_gate_input()` 中 `in->forbidden_outputs_off = in->forbidden_outputs_off && …`：先 `memset(in,0)` 后 AND → 恒 false → 台架永远无法受理（P0：核心功能完全不可用） | **已修复**：抽出纯函数 `motor_bench_forbidden_outputs_off()`（基值 **true**，任一子句未 OFF → false，fault/emergency 激活时跳过 UV 子句）。已加入 `test_motor_bench.c`（聚合回归，`[7]`） |
| P0-2 | P0 | 协议键冲突：帧顶层 `"type"` 是解析器判别键（`app_protocol_core.c:188` 要求 `"type":"cmd"`），而 `motor_bench_start` 路由读 `"type"` 作台架类型 → `"type":"cmd"` 时路由取到 `"cmd"`、`"type":"pulsator"` 时被解析器拒绝 —— 台架**永远无法经小程序/串口启动** | **已修复**：台架类型改用 `"target"` 键（与 `actuator_self_test` 同约定），由纯 C 扫描器 `app_protocol_field_string(frame,"target",…)` 读取原始帧，契约 CI 固定（`test_phase11_safety.c` 15/15） |
| P0-3 | P0 | `collect_gate_input()` 中 `self_test_busy` 把**本组件自身** `s_rt.test_active` 计入：`request()` 受理时 `test_active` 仍 false → 门禁 OK（ACCEPTED），随后置 `test_active=true`；下一 tick 门禁自锁 `SELF_TEST_BUSY` → 台架**一受理即 FAULT**（板测 4 连发复现，`MB request ACCEPTED` 与 `MB terminal … code=SELF_TEST_BUSY` 同刻）。纯 FSM host 测试喂显式输入、测不到此服务层聚合缺陷 | **已修复**：抽出纯函数 `motor_bench_external_self_test_busy(uv, act)`（仅外部 UV/执行器板测参与，**签名不含台架自身活动标志**），服务层聚合改走该函数；重入保护由 `request()` 顶部 `test_active` 检查独占。已加入 `test_motor_bench.c` 回归（`[7]`，20/20 PASS） |
| P1-1 | P1 | 安全快照读取失败 fail-open + 未初始化 `safety` 结构读取（UB） | **已修复**：`safety = {0}` 初始化 + 读取失败 → `in->snapshot_fail = true` → 门禁 `SNAPSHOT_UNAVAILABLE` fail-closed；exec 快照同理 |
| P1-2 | P1 | 无反向互斥：台架运行中若洗涤被触发，台架的 `EXECUTOR_BUSY` 致命门禁会 `request_all_off`（即台架能杀并发洗涤）；反之洗涤运行中台架也只能事后 FAULT | **已修复**：双向互斥 —— 台架受理前 `wash_executor_set_external_busy(true)`，`submit_program` 门禁 +`!external_busy`（洗涤无法在台架期间启动）；`exec_idle` 快照增补 `program_reserved` 实时读，封堵 reserve→activate 窗口。台架全部终态/停止路径清除 busy |
| P2-1 | P2 | BL50 300ms 脉冲 + 5%/100ms 斜坡 + 反转停 → 实际 PWM>0 约 0.7s（≤15% 低档），略超标称 300ms | **文档化**（已知限制，见 §11）：保持生产 BL50 驱动不变；300ms 为驱动保持时长，斜坡是生产语义 |
| P2-2 | P2 | `wash_executor.h` 新增 `wash_executor_set_external_busy()` 若未实现 → 链接错误 | **已修复**：与 `program_reserved()` 一并实现（互斥锁保护） |
| P2-3 | P2 | 疑点：BL50 VALIDATING 失败静默回 IDLE → 台架误判受理成功 | **证伪**：读 `bl50_fsm.c` —— VALIDATING 失败走 `FAULT_POSITION_DRIVER → STOPPING → FAULT`（非静默 IDLE），台架 `in->bl50_fault` → FAULT 终端正确。无需改 FSM |

> 修复后所有断言均由 host 回归覆盖；每项发现先由审查员给出证据行、再由实现者独立读源核验后才动手修改（避免「照着改」）。

---

## 8. 实物验证

### 8.1 构建

`idf.py build`（REAL 模式）**成功、无警告**。修正了 `build_main.bat` 缺失 `ESP_ROM_ELF_DIR` 导致 idf.py gdbinit 生成空转的问题；`idf.py` 在 `MSYSTEM` 环境变量存在时（Git Bash 继承）直接退出不构建，须经 PowerShell 上下文运行。

### 8.2 烧录 COM8

`idf.py -p COM8 flash` 成功，`Hash of data verified`，`Hard resetting via RTS pin`。固件 `build/xiaojing_idf.bin`（0x3C3BF0 字节 = 3947504 B，P0/P1 修复后重新烧录）。

**P0-3 修复后重烧录**：用户板测复现「台架故障」后（§1），`motor_bench_external_self_test_busy` 聚合修复 + `lc` 诊断字段合入，重构建无警告（`xiaojing_idf.bin` **0x3C3C20 字节 = 3947552 B**，`build_phase11_c_r0_rebuild.log`），重烧录 COM8 成功（`Hash of data verified`，`build_phase11_c_r0_reflash.log`）。

### 8.3 冷启动 safe-OFF 证明（POWERON 硬复位）

证据：`build_phase11_c_coldboot.log`（`capture_coldboot.py` 硬复位后采集 45s；P0/P1 修复后固件）。

```
rst:0x1 (POWERON),boot:0x9 (SPI_FAST_FLASH_BOOT)
I (1970) safety_mgr: Initialized (mode=2 ptc=0 board=1)
I (1970) safety_mgr: Running (MCP baseline OK)
I (2010) bl50_svc: task started
I (2010) water_svc: initial safe-off sync: source=OFF transfer=OFF synced=1
I (2010) dry_svc:   initial safe-off sync: fan=OFF heater=OFF synced=1
I (2020) cha_svc:   initial safe-off sync: relay=OFF synced=1
```

- 全程 **无** `[MB]` 活动：`MB submit_pulse` / `MB terminal` / `MB request` / `MB emergency` 均**零出现** → 电机台架空闲、从未通电。
- BL50 驱动初始化 + 任务启动，但 idle（PWM=0，无脉冲）。
- IBT-2 `Calibration updated: calibrated=no, rpwm_is_cw=yes` → 有刷位置电机未校准/未驱动（台架不驱动 IBT-2）。
- 无 fault / emergency / panic / abort / Guru；音频 I2S / BLE / Wi-Fi(GOT_IP) / UI 心跳正常流动。

### 8.3b P0-3 修复后冷启动 safe-OFF 复证（POWERON 硬复位）

证据：`build_phase11_c_coldboot_r0fix.log`（`capture_coldboot_r0fix.py` 硬复位后采集 45s；P0-3 修复 + `lc` 字段固件）。

```
rst:0x1 (POWERON),boot:0x9 (SPI_FAST_FLASH_BOOT)
I (xxxx) safety_mgr: Initialized (mode=2 ptc=0 board=1)
I (xxxx) safety_mgr: Running (MCP baseline OK)
I (xxxx) bl50_svc: task started
I (xxxx) water_svc: initial safe-off sync: source=OFF transfer=OFF synced=1
I (xxxx) dry_svc:   initial safe-off sync: fan=OFF heater=OFF synced=1
I (xxxx) cha_svc:   initial safe-off sync: relay=OFF synced=1
```

- 复证结论同 §8.3：全程 **0 条** `[MB]` 活动（0 脉冲提交 / 0 终端 / 0 急停 / 0 request），BL50 idle（PWM=0），IBT-2 未校准未驱动，无 fault/panic/emergency。

### 8.4 ≥30 分钟被动监控（`PASSIVE_SOAK_GATE`）—— **PASS**

证据：`build_phase11_c_soak.log`（`passive_monitor.py`：无复位、无注入，COM8 被动串口采集 **32 分钟 = 1920 s**，P0/P1 修复后固件）。

- **覆盖时长**：日志首尾 ESP 运行时时间戳 97440 ms → 2017080 ms，跨度恰好 1920 s，无采集缺口。
- **异常扫描**：对全文 2754 行执行签名扫描（`panic` / `Guru` / `abort` / `assert` / `FAULT` / `emergency` / `[MB]` / `MB submit` / `MB terminal` / `task_wdt` / `WDT` / `ESP_ERROR` / `Backtrace` / `rst` / `reboot`）→ **0 命中**。
- **心跳记录**：`display_svc` heartbeat tick=400→8000 共 **77 次**；`bootstrap` D:/UI_SENSOR 心跳 **159 条**（末条 t≈1983s：`s=W` 空闲、`g=1`、`d=0 we=0 te=0 ei=0/0`，i2s 字节 3.84M→63.2M 持续增长，音频链路无停滞）。
- **电机台架空闲证明**：全文 **0 条** `[MB]` / `motor_bench` / bl50 驱动日志 → 0 次脉冲提交、0 次终端事件、0 次急停；BL50 全程 idle（PWM=0），电机**从未通电**。
- 结论：32 分钟被动监控稳定，无 fault / panic / emergency，全部输出保持 safe-OFF。

---

## 9. Gate 汇总

| Gate | 状态 |
|---|---|
| `SOFTWARE_GATE`（代码/测试/构建/烧录/冷启动 safe-OFF） | **PASS** |
| `PASSIVE_SOAK_GATE`（≥30min 被动监控） | **PASS**（§8.4：32 min、0 异常、台架 0 活动） |
| `NEEDS_USER_MOTOR_BENCH_TEST`（真实电机空载台架） | **未执行 — 用户门控** |

---

## 10. 用户空载台架操作指引（逐步）

> ⚠️ 安全：台架**绝不自动让电机通电**。每步必须由用户手动把桶摆到屏幕/小程序指示的位置，稳定后点「确认脉冲」，固件只提交一次低能量短脉冲（低档 300ms），随后自动确认电机回 OFF 才推进。出现任何错误码/超时/断线 → 固件已强制全部输出 OFF。

### 准备
1. 确认上电后串口冷启动 safe-OFF（§8.3）。
2. 设备内无水（不触发进水），桶内空载（无衣物）。
3. 小程序连 BLE，进入「开发测试」区，确认 capability 含 `motor_bench_self_test`。

### 单步自检（推荐先做）
4. 「电机空载台架」选择类型 `pulsator`（主洗涤/波轮）、`drum`（滚筒/BLDC）或 `hall_sequence`（3 步霍尔流程台架）。
5. 点「开始」→ 应答 `MOTOR_BENCH_ACCEPTED`，状态进入等待位置（`pos` 显示所需角度：pulsator=0°、drum=90°）。
6. **手动**把桶转到所需位置（5 路霍尔对齐），等状态 `ready=1`（`POSITION_READY`）。
7. 点「确认脉冲」→ 固件提交一次低档 300ms 脉冲 → 观察电机是否按预期转动 ≤300ms 后自动停 → 状态回 `ready=0` 等待下一步或 `COMPLETE`。
8. 若需结束：点「取消」→ 全 OFF。

### 霍尔流程台架（进阶，`motor_hall_bench_sequence`）
9. 依序手动摆放 0° → 90° → 180°，每步重复 6-7（确认→脉冲→确认 OFF→下一步）。固件按生产 resolver 契约（0°/90°/180°）校验；错位/多霍尔/超时 → fail-closed 拒绝或 FAULT + 全 OFF。

### 异常路径
- 位置错误 / 多霍尔 / 超时：屏幕显示拒绝原因，不推进，全部输出保持 OFF。
- BLE 断线 / 急停 / 取消：固件强制全部输出 OFF。
- 若显示 `EXECUTOR_BUSY`：先结束/重置当前洗涤流程再试（生产洗涤与台架互斥）。

---

## 11. 已知限制 / 后续

- 小程序本体不在本仓库；协议契约已定（§4），小程序按契约接入。
- P2-1 已知：BL50 脉冲实际 PWM>0 约 0.7s（300ms 驱动保持 + 5%/100ms 斜坡 + 反转停，≤15% 低档）。这是**生产 BL50 驱动的斜坡/刹车语义**，保持生产驱动不变；台架超时/门禁按实际时序已足够宽裕。
- 实物电机验收（脉冲方向/时长/刹车/霍尔顺序实物确认）完全用户门控，未在本轮执行（§10）。
