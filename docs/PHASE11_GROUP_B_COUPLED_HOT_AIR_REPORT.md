# Phase 11 Group B — 单继电器耦合热风模块（安全适配）最终报告

> 代码评审 P0/P1 修复轮 · 全量测试与静态扫描 · REAL 模式实物验证（冷启动 + BLE 修复）
> 日期：2026-08-07
> 固件：`build/xiaojing_idf.bin`（0x3c15d0 字节，分区 12% 空闲）
> sdkconfig 当前：**REAL**（`CONFIG_XIAOJING_OUTPUT_MODE_REAL=y` + `CONFIG_XIAOJING_HOT_AIR_MODULE_ENABLED=y`）

---

## 1. 结论

Phase 11 Group B「单继电器耦合热风模块」软件安全适配 **代码层面完成**，独立代码评审发现的 **1 个 P0 + 3 个 P1 + 1 个相关缺陷全部修复**，并新增回归测试证明修复有效（评审员以"回退修复→测试变红"实证非空）。全量主机/单元/小程序/Python 测试与四项静态扫描 **全部 PASS**，主固件与单元固件构建 **无警告**。

**实物验证已闭环至安全门（220V 通电 / 热风自检除外）**：`sdkconfig` 已切换为 `REAL` 模式并烧录 COM8，冷启动证明所有输出 safe-OFF 通过，BLE 控制器启动崩溃循环已修复并验证稳定（不卡死、不重启）。**热风 1s 自检与 220V 通电仍严格等待用户手动点击确认，任何情况下不自动执行。**

---

## 2. 本轮修复（独立评审 P0/P1 + 相关）

### 2.1 P0 — `process_fsm_output` 丢弃补偿/重试动作 → FSM 卡死（可能继电器带电）

**缺陷**：`coupled_hot_air_service.c` 原 `if (!ok) break;` 在某个动作失败后立即跳出链，丢弃 FSM 在同一轮输出中要求的**补偿 `RELAY_OFF`**（RELAY_ON 失败场景）或**后续 OFF 重试**。FSM 的 `pending_action` 守卫令后续 tick 全部 no-op，服务永久卡在 STOPPING，继电器可能在未知状态下保持带电。

**修复**：
- 删除 `if (!ok) break;`，链在 `out->action == CHA_ACTION_NONE`（即 FSM 已发终态）时经 `save_terminal_event` 退出；失败动作的补偿/重试动作**必然被执行**。
- `MAX_CHAINED_ACTIONS` 从 2 提升到 `CHA_OFF_RETRY_MAX + 2`（= 7）。最坏链 = 1×RELAY_ON 失败 + 5×OFF 重试失败 = 6 个动作，终态在第 7 轮输出；上限留 1 裕量，且从常量推导，永不脱节。
- 终结性论证：FSM 一旦进入 STOPPING，`off_attempts` 每次 OFF 失败自增；`handle_stopping` 在 `UNKNOWN && off_attempts >= 5` 时发 FAULT（见 2.4），因此链必然收敛。终态 exactly-once 由 `save_terminal_event` 三重守卫保证。

### 2.2 P1-1 — 保留位在 `begin_request` 前释放 → 第二请求可覆盖 RUNNING

**缺陷**：`cha_task` 循环先 `release_request_reservation()`（FSM 空闲即释放），后 drain 队列执行 `cha_fsm_begin_request`。首个请求尚在 RUNNING 时保留位已放掉，第二个 `run_async` 可抢占保留位，其命令被 drain 后 `begin_request` 直接覆盖正在运行的请求。

**修复**：
- drain 仅在 `cha_fsm_is_idle(fsm)` 时进行，且一次只 begin 第一个保留命令（`break`），其余队列项等 FSM 回 IDLE 再取。
- `release_request_reservation()` 移到 drain **之后**，仍以 `cha_fsm_is_idle` 守卫：请求运行期间保留位绝不放。
- 已加服务级回归测试 P11C-02（见 §4）：请求活动期第二次 `run_async` **同步**返回 `ESP_ERR_INVALID_STATE`；终态后接受下一请求。

### 2.3 P1-2 — `begin_request` 把 UNKNOWN 重写为 OFF（不诚实语义）

**缺陷**：`cha_fsm_begin_request` 无条件 `ctx->relay_state = CHA_OUT_OFF;`。若上一次终态是 OFF 确认失败（relay_state==UNKNOWN），新请求会把未知状态**谎报为 OFF**，快照 `output_confirmed_off` 失真。

**修复**：删除该赋值，`begin_request` 保留 relay_state 原值。安全性由 safety 层兜底：`SAFETY_OP_HOT_AIR_MODULE` 的 ON 门禁要求**所有 MCP 输出 KNOWN_OFF**（`safety_interlocks.c` L626-631）；继电器未确认 OFF（KNOWN 位未置）时 RELAY_ON 被拒（`SAFETY_REJECT_OUTPUT_UNKNOWN`）→ 走 fail-closed OFF 重试 → FAULT，物理继电器绝不在未知态被加电。快照各字段（`output_known/on/off`）在确认前保持 false。

### 2.4 相关 — `handle_stopping` FAULT 依赖 `relay_ever_on` → 无保证终止

**缺陷**：FAULT 条件为 `relay_ever_on && UNKNOWN && off_attempts>=5`。若本次运行从未通电（例如 RELAY_ON 失败进入 STOPPING，或上次遗留 UNKNOWN），`relay_ever_on==false`，OFF 重试**永不收敛**，配合服务链上限会再次卡死。

**修复**：去掉 `relay_ever_on &&`，条件变为 `UNKNOWN && off_attempts >= CHA_OFF_RETRY_MAX`。OFF 重试链在任何路径下至多 5 次后必然发 FAULT（OFF_CONFIRM_FAILED, off=false）。回归测试 `test_fsm_never_on_off_retry_terminates` 覆盖。

### 2.5 P1-3 — stop hook 对已删除任务的悬垂句柄窗口

**缺陷**：`cha_stop_hook` 读 `task_handle` 后对句柄发通知；任务已 `vTaskDelete` 后 `service_stop` 才清句柄，窗口内 hook 可能对已删除任务发通知（UAF）。

**修复**：`cha_task` 退出路径在 `xSemaphoreGive(task_done_sem)` **之前**先 `atomic_store_explicit(&task_handle, NULL, memory_order_release)`。此后 hook 读到 NULL 不再通知；此前任务仍存活，通知目标有效。与既有兄弟服务（dry_service 同型）一致并加固。

### 2.6 独立评审复核结论

独立评审员对五个修复逐一验证 **全部正确、无新增 P0/P1**，并实证回归测试非空（回退 Fix 3/Fix 4 → 对应测试变红）。残留 2 项良性项（非缺陷，记录在案）：
- **P2（接受）**：保留位释放存在 ~纳秒级 TOCTOU，窗口内第二个 `run_async` 可能被接受。评审员判定：**不会双继电器加电、不会丢失/覆盖命令**（队列 FIFO + safety `hot_air_on`/`all_mcp_known_off` 互锁兜底）；仅在 FSM 空闲的瞬间放宽严格单飞。保持现状，报告留档。
- `relay_ever_on` 字段已无读取点（Fix 4 移除唯一读），写多读零，无害，留待后续清理。

---

## 3. 改动文件

| 文件 | 变更 |
|---|---|
| `components/coupled_hot_air/coupled_hot_air_service.c` | Fix 1/2/5：链不丢弃补偿动作、保留位时序、task_handle 退出清理 |
| `components/coupled_hot_air/coupled_hot_air_fsm.c` | Fix 3/4：begin_request 诚实语义、OFF 重试保证终止 |
| `host_tests/test_coupled_hot_air.c` | +2 测试（+14 断言）：`test_fsm_begin_keeps_unknown`、`test_fsm_never_on_off_retry_terminates` |
| `test_apps/unit/main/phase11_coupled_hot_air_tests.c` | 新增：服务级 P0/P1-1 回归（P11C-01/P11C-02） |
| `test_apps/unit/main/CMakeLists.txt` | 注册新测试文件 + `coupled_hot_air` 组件 |
| `host_tests/test_main.c` | `SAFETY_OP_COUNT == 10` 断言修正（本会话早前） |

---

## 4. 测试与扫描全量结果（全部 PASS）

### 4.1 主机 C（MSVC + Ninja，`host_tests/build`）

| 套件 | 结果 |
|---|---|
| `test_runner` | 308/308 |
| `test_display_model` | 57/57 |
| `test_fake_hal_stress` | 10000 轮，0 失败，overlap=0，HEATER_ON=0 |
| `test_phase11_safety` | 11/11 |
| `test_voice_vad` | 4/4 |
| `test_uv_act_core` | 100/100 |
| `test_coupled_hot_air` | **95/95**（原 81 + 14 回归断言）；10000 轮压力 overlap=0 / fan_actions=0 / terminal_dups=0 / not_off_at_end=0 |

### 4.2 单元（ESP32 目标固件，`test_apps/unit`）

- 构建成功，`coupled_hot_air` 组件 + 新测试文件编译链接通过，**无警告**。
- 新增 2 条服务级回归（Fake HAL，零实物通电）：
  - **P11C-01（P0）**：`run_async` 后 RELAY_ON 被 safety 拒绝（FAKE 模式门禁）→ 服务必须执行补偿 RELAY_OFF（Fake HAL 历史出现 PTC OFF 写入）→ 收敛 FAULT 终态 + `output_confirmed_off=true`，**绝不卡死**。
  - **P11C-02（P1-1）**：请求活动期第二次 `run_async` 同步返回 `ESP_ERR_INVALID_STATE`；终态后保留位释放，第三请求被接受。
- 运行时验证需烧录板卡执行（见 §6，当前门禁未放行）。

### 4.3 Python（主机）

| 套件 | 结果 |
|---|---|
| `host_tests/test_phase1_host.py` | 23/23 |

### 4.4 小程序（Node vm 无真机）

| 套件 | 结果 |
|---|---|
| `test_act_selftest_flow`（含 4 条热风） | 33/33 |
| `test_negative_diagnostics` | 11/11 |
| `test_page_contract` | 78/78 |
| `test_ble_protocol` | 8/8 |
| `test_ble_transport` | 19/19 |
| `test_booking_flow` | 23/23 |
| `test_uv_order_flow` | 15/15 |
| `test_uv_selftest_flow` | 12/12 |

### 4.5 静态扫描

| 扫描 | 结果 |
|---|---|
| `tools/check_safety_bypass.py` | PASS（无新增旁路；既有 position_service 已知旁路留档） |
| `tools/scan_utf8_glyphs.py` | PASS（cmap 3869，扫描 291 非 ASCII，缺失 0） |
| `tools/check_display_layout.py` | PASS（320×240，最低卡片底部 218） |
| API-key 泄漏扫描 | PASS（唯一命中为主机测试显式占位 `test-key-not-secret`；`voice_secrets.h` 为 gitignore 本地演示凭据，属设计） |

### 4.6 构建

| 目标 | 结果 |
|---|---|
| 主固件 `build/xiaojing_idf.bin` | 构建通过，无警告（0x3c15d0 B，分区 12% 空闲） |
| 单元固件 `test_apps/unit/build` | 构建通过，无警告 |

---

## 5. 诚实语义与安全属性（保持不变）

- `command_on` = 仅请求继电器闭合（命令态）；`output_confirmed_on` = OLAT 闩锁命令成功；**绝无 `airflow_proven`**，不声称风扇转速。
- 单继电器耦合：风扇+加热丝按厂家并联同步启停，无预吹/无延迟关风，24V PWM 风扇保持禁用。
- FSM：IDLE_OFF→STARTING→RUNNING→STOPPING→COMPLETE→IDLE_OFF；10 启动门禁；本地硬超时（默认 1s/上限 3s）；过温/SHT 失效/cancel/abort/emergency/BLE 断线 → 立即 OFF；终态 exactly once；OFF 确认失败 → 显式 FAULT；冷却锁 30s，机器码 `HOT_AIR_COOLDOWN_ACTIVE`，状态 `"cd"` 字段。
- OFF 请求经 safety 无条件放行；ON 请求经"全 MCP KNOWN_OFF + 位置 270° + SHT + 温度 + 冷却锁 + 重复开启拒绝"门禁。
- 自检 `HOT_AIR_COUPLED_SELF_TEST`：能力仅 `hot_air_coupled`；小程序按钮「热风模块安全自检（1秒）」capability 门控 + 双重确认 + 防连点 + 冷却倒计时 + BLE 断线诚实文案 + 生产隐藏。

---

## 6. 实物验证状态（REAL 模式闭环）

用户确认后已执行：sdkconfig 切换 `LOW_VOLTAGE → REAL`，`CONFIG_XIAOJING_HOT_AIR_MODULE_ENABLED=y` 使 MCP23017 将 GPA7（热风继电器）配置为 OUTPUT（IODIRA=0x70）、MCP 上电 OLAT=0 先于 IODIR → 继电器启动即 OFF。

### 6.1 发现并修复：BLE 控制器启动崩溃循环（新缺陷，P0）

REAL 固件首次烧录后，板卡每 ~3.3s 崩溃重启一次：

```
BLE_INIT: Malloc failed   →  BLE assert emi.c 164  →  Guru Meditation (Interrupt wdt CPU0)  →  rst:0xc (RTC_SW_CPU_RST)
```

**根因**：Group B 增加 cha_svc/耦合热风代码后，BLE 控制器 init 所需的连续内部内存被压垮 —— `HEAP[before_ble] int_largest` 从 R1.4 稳定固件的 **12288 → 8192**（临界 ~11-12KB）。而 `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` 未开启导致 `EXT_RAM_BSS_ATTR` 宏展开为空：4 处任务栈（cha_task/gesture/audio_capture/voice_fetch，合计 ~13KB）**静默停在内部 `.bss`**，正是这 13KB 压垮了 BLE 堆。

**修复**（`components/coupled_hot_air/coupled_hot_air_service.c` + sdkconfig/sdkconfig.defaults）：
1. `cha_task` 改用 `xTaskCreateStaticPinnedToCore` + `StaticTask_t` + `EXT_RAM_BSS_ATTR StackType_t[]`（栈声明放 PSRAM）；
2. **开启 `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`** —— 使全部 4 处 `EXT_RAM_BSS_ATTR` 任务栈真正落入 `.ext_ram.bss`（map 证实：`0x3c3b0000` 起，cha 0xc00 + voice 0xc00 + gesture 0xc00 + audio 0x1000），内部静态 DRAM 释放 **~13KB**。

### 6.2 修复后验证（冷启动串口证据）

```
Output mode: REAL / PTC: disabled / MCP23017 initialized (addr=0x20, IOCON=0x44)
safety_mgr: Initialized (mode=2 ptc=0 board=1)
safety_mgr: Running (MCP baseline OK)
water_svc: initial safe-off sync: source=OFF transfer=OFF synced=1
dry_svc:   initial safe-off sync: fan=OFF heater=OFF synced=1
cha_svc:   initial safe-off sync: relay=OFF synced=1        ← 热风继电器启动即 OFF
HEAP[before_ble] int_free=60983 int_largest=27648          ← 8192 → 27648（3.4× 余量）
nimble_port_init err=0x0 / gap/gatt init done / adv_start rc=0 / running as JJTP-XIAOJING
HEAP[after_ble]  int_largest=19456
```

- **冷启动 safe-OFF 证明 PASS**（GPA7 继电器 + 水阀 + 烘干全 OFF）。
- **BLE init 成功，无崩溃循环**：监控日志 0 崩溃 token、1 次干净 POWERON 冷启动，板卡持续正常运行（I2S 麦克风检测等工作正常）。
- 构建无新增警告（仅既有 button_service/audio_controller 2 项）。**验收标准（不卡死/不崩溃循环）已满足**；长时 ≥30min 浸泡用户指示不执行。

### 6.3 仍未执行（严格用户门控）

- **热风 1s 自检与任何 220V 通电：等待用户手动点击确认**，任何情况下不自动执行。
- **Group C（主洗涤/滚筒电机空载台架验收）严格串行**，待 Group B 实物闭环后才启动，绝不并行、绝不自动让电机通电。

### 6.4 实测发现：热风自检位置门禁（0° 拒 / 270° 过，符合设计）

用户实测：屏幕「位置输入」行（六路霍尔实时读数）显示滚筒停在 **0°**，小程序热风自检被拒，文案为「桶位置不是 0°」。

**根因（符合设计的 fail-closed）**：热风模块自检的硬性位置要求是 **270°**，非 0°。拒绝发生在滚筒实际停在 0° 时，安全门禁正确不放行继电器。门禁在四个层级同源强强制：

| 层级 | 位置要求 | 文件 |
|---|---|---|
| FSM | `position == DRUM_POS_270` 且 valid/stable/fresh/电机停 | `coupled_hot_air_fsm.c:137-140` |
| Safety 互锁 | `position != DRUM_POS_270` → `SAFETY_REJECT_POSITION_MISMATCH` | `safety_interlocks.c:615-620` |
| 执行器门禁 | `required_pos = DRUM_POS_270`（热风），`DRUM_POS_180`（排水） | `actuator_self_test.c:174-176` |
| 请求侧 | `required_position = DRUM_POS_270` | `coupled_hot_air_service.c:215` |

这解释了用户先前的行为反差：**270° 时自检成功**（门禁放行），**0° 时被拒**（门禁正确拦截）。

**文案缺陷（已修）**：执行器自检共享线码 `POSITION_NOT_ZERO`（与 UV 自检同名；UV 真要求 0°），但热风/风扇需 270°、排水需 180°，文案「桶位置不是 0°」双重误导——实际位置确实是 0°，且真实要求是 270°。修复（保持线码稳定以兼容已部署小程序）：
- 小程序 `mapActReject` 对 `POSITION_NOT_ZERO` 改为按目标动态显示：「被拒绝：桶位置未对齐（需 270° 且电机停稳）」（排水为 180°）。
- 小程序确认弹窗补充对齐要求：热风「请先将桶转到 270° 对齐位置且电机停稳」、排水「180°」。
- 固件 `actuator_self_test_core.c` 门禁处加注释澄清共享码语义，不改线协议。
- 验证：小程序 33/33（act flow）+ 12（UV）+ 78（page）+ 11（negative）+ 23（booking）+ 8（protocol）PASS；固件 host `test_uv_act_core` 100/100、`test_coupled_hot_air` 95/95 PASS。

**操作指引**：热风自检前必须先把滚筒转到 **270°**（「位置输入」行 270° 点亮）并等电机停稳；排水自检需 **180°**。

---

## 7. 后续动作清单

- [x] 用户确认 REAL 模式 → 烧录 COM8 → 冷启动 safe-OFF 证明（含 §6.1 BLE 崩溃循环修复）
- [x] 稳定性验证：不卡死、不崩溃循环（验收通过）
- [x] 位置门禁实测（§6.4）：0° 拒 / 270° 过符合设计；小程序拒绝文案已改为按目标动态显示角度
- [x] **REAL 模式热风 1s 自检 PASS**（用户手动点击确认后执行：风扇+加热丝同步启动后自动关闭，终态正常）→ **Group B 实物门禁全部 PASS，Group B 全闭环**
- [x] Group C Round 0（滚筒电机空载台架）已授权启动：软件实现+被动验证，不自动通电
- [ ] （可选项）清理 `relay_ever_on` 写多读零字段
