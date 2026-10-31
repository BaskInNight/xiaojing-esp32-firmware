# Phase 11 Group A Round 1: 执行器安全自检审计修复 + 三页 UI 实机精修 + 板测准备 — 报告

日期：2026-08-07
工程：
- 固件：`D:\UserData\ESP32Project\IDF\ESP32-IDF-Test`（ESP32-S3 N16R8, ESP-IDF v5.5.4, ILI9341 320×240 横屏, COM8）
- 小程序：`D:\UserData\ESP32Project\miniapp4.0`

## SKILLS_ACTUALLY_INVOKED

| Skill | 状态 | 实际如何影响设计/修复 |
|-------|------|----------------------|
| `/embedded-systems` | 已调用 | FreeRTOS 生命周期：stop() 先锁存 stop_requested 再释放互斥量 join（避免与 emergency 死锁）；emergency 原子闩锁 + 无锁后备关断（不依赖 actuator 互斥量）；fail-closed OFF 证明（无法证明 OFF ⇒ 保留生命周期可重试）；terminal exactly once。 |
| `/esp32` | 已调用 | ESP32-S3/LVGL：缩小顶栏心跳/Wi-Fi/BLE 图标并保证 ≥6px 净距；四态输出指示灯（形状+颜色双编码）；IO 页 INPUTS 四态；页指示器上移避免实机裁切。 |
| `/debugging-code` | 已调用 | 无实机会话（板未连接），本轮未触发反汇编；沿用上一轮建立的 addr2line/状态链路方法审阅本轮的 5 项审计修复。 |
| `/code-review` | UNAVAILABLE（无独立 skill） | 用 general-purpose agent 做独立 P0/P1/P2 审查（见 §CODE_REVIEW）；另做结构化人工审查。 |
| UI/design skill | 采用原则 | 只采用信息层级/像素对齐/状态表达原则：任务卡紧凑两列、OUTPUTS 紧凑图例一行小字、次要信息与告警颜色分级；未照搬 Web 布局，未增加资源。 |

## 实拍照片处理

三张实拍图（codex-clipboard-*.jpg，1713/1658/1699×1280）在本会话无法被模型渲染（vision 不可用，多尺寸尝试均返回 Unsupported Image），且无可用 OCR。**如实说明**：本轮 UI 精修以任务描述中逐条列出的实机缺陷为准（顶栏图标碰撞/过重、页指示裁切、任务卡大空白、INPUTS 命名、OUTPUTS 图例、SYSTEM 四行裁切、语音页后端名/切换文案、Wi-Fi 与云端跨帧不一致），并全部落实为像素坐标修正；照片本身未参与逐像素比对。

## 基线与保护

- 脏工作树全程保留，未做任何破坏性 git 操作。
- 初始固件：BIN `b61bcac6…` / ELF `acfa4248…`（Phase 9 终版）。
- 未覆盖 Config/NVS/API Key。

## FILES_CHANGED

固件：
- `components/actuator_self_test/actuator_self_test.c`（emergency 闩锁/无锁后备/OFF fail-closed/task_done_sem 生命周期）
- `components/actuator_self_test/actuator_self_test_core.c/.h`（新增 `actuator_off_proof_t` + `actuator_outputs_confirmed_off` 纯函数）
- `components/app_protocol/app_protocol.c`（target 有界快照，消除 cJSON UAF）
- `components/app_protocol/app_protocol_core.c/.h`（新增 `app_protocol_snapshot_target`）
- `main/app_bootstrap.c`（stop 失败阻断依赖销毁、BLE 断线 emergency 失败日志、telemetry 先采集 Wi-Fi 再算云端）
- `components/display_service/display_service.c`（三页精修）、`display_model.c`（新增 `display_cloud_from_wifi`）、`display_service.h`
- `tools/phase10_actuator_monitor.py`（h/L/iB 正确解析 + 复位分类 + NOT_COMPLETE）

测试：
- `host_tests/test_phase11_safety.c`（新增 11 用例）、`test_uv_act_core.c`（+9 OFF-proof）、`test_display_model.c`（+4 cloud）、`CMakeLists.txt`
- `test_apps/unit/main/phase11_actuator_lifecycle_tests.c`（新增 100 循环 + emergency）、`CMakeLists.txt`
- `tools/scan_utf8_glyphs.py`、`tools/check_display_layout.py`（新增静态检查）

## ROOT_CAUSES

- **BOOTSTRAP_STOP_UAF（P0-1）**：deinit/rollback 对 `uv_self_test_stop()`/`actuator_self_test_stop()` 返回值 `(void)`，stop 失败仍继续销毁 water/detergent/uv → 活任务访问已销毁依赖。
- **ACK_TARGET_UAF（P1-1）**：`result.target_name = target`（cJSON valuestring 裸指针），`cJSON_Delete(root)` 后 `encode_reply` 读已释放内存。
- **OFF_CONFIRM_NOT_FAIL_CLOSED（P1-2）**：`act_outputs_confirmed_off()` 快照失败时跳过检查并返回 true（把"无法证明 OFF"当作"已关闭"）。
- **BLE_EMERGENCY_SILENT_SUCCESS（P1-3）**：`actuator_self_test_emergency()` 拿不到互斥量时静默返回 ESP_OK；bootstrap 断线路径 `(void)` 吞掉错误。
- **TASK_DONE_SEM_LEAK（P2-1）**：成功 join 后 sem 未删除，init() 的 memset 每次丢失句柄。
- **WIFI_CLOUD_CROSS_FRAME（Stage 2）**：`voice_cloud_state` 用上一帧的 `telemetry.wifi_state` 计算，Wi-Fi 已连接而云端仍显示 OFFLINE；且"云端 READY"仅由 Wi-Fi IP 推断，属伪报。
- **MONITOR_WRONG_TOKEN（Stage 3）**：报告曾把 `iB` 当作 internal heap（实际是 I2S bytes）。
- **REVIEW_STALE_TASK_HANDLE**：stop() 超时路径保留 `s_rt.task`，任务自删后重试 `xTaskNotify` 指向已释放 TCB。
- **REVIEW_TASK_COMPLETE_QUEUE**：deinit 在 worker 可能仍在播放时强删队列（当前 1s receive 超时 < 1.5s 轮询使其不可达，但脆弱）。
- **REVIEW_KNOWN_NOT_OFF**：display 以 safety mcp_known_mask 推断 OFF 不成立（safety 对 ON 写也置 known 位）。

## BOOTSTRAP_STOP_UAF_FIXED

- rollback 增加 `first_stop_err`（首个关断错误，不覆盖）；`uv/actuator_self_test_emergency()` 后再查 `stop()`，非 ESP_OK ⇒ 记录 + `rollback_halted=true`，跳过依赖服务销毁。
- deinit 同策略：stop 失败立即 `return`（保留全部资源），符合既有"首错即返"模式。
- 依赖顺序保持 UV→actuator→uv_service→detergent→drain→dry→water→bl50→position→executor→safety→queue→HAL。

## ACK_TARGET_UAF_FIXED

- `app_protocol_snapshot_target(target, buf, cap)`（纯函数，有界复制，超长拒绝不截断）。
- `route_result_t` 增 `target_buf[APP_PROTOCOL_TARGET_NAME_MAX(24)]`；ACT case 先快照再提交；`encode_reply` 只读 `target_buf`（cJSON_Delete 后仍有效）。
- 重复 seq：`app_seq_cache` 存的是已编码字符串，重放原 ACK，无重复动作（host 测试覆盖）。
- Host 测试：源释放+堆扰动后快照仍完整、空/NULL/超长拒绝、边界恰好容纳；重复 seq 重放一致、同 seq 异命令冲突。

## OFF_CONFIRM_FAIL_CLOSED

- 新增纯函数 `actuator_outputs_confirmed_off(const actuator_off_proof_t*)`：仅当 water+detergent 快照都成功且全部输出 KNOWN_OFF 才 true；任一快照失败/超时或输出 UNKNOWN ⇒ false。
- 运行时 `act_outputs_confirmed_off()` 组装 proof（失败时相关标志保持 false）。
- `actuator_self_test_stop()` 对 false 走 emergency + 有界等待；仍无法证明 ⇒ 返回 ESP_ERR_TIMEOUT，保留生命周期可重试，绝不把"无法证明 OFF"当"已关闭"。
- Host 矩阵测试：快照错/UNKNOWN/ON/NULL 全 false；全部 KNOWN_OFF 才 true。

## BLE_EMERGENCY_LATCH

- `s_rt.emergency_latch`（`_Atomic bool`）：emergency() 先置位再抢锁。
- 无锁后备关断：`act_emergency_outputs()`（仅 water/detergent 服务 API，各自持锁并经 safety_manager 下电，不依赖 actuator 互斥量）在抢锁前直接执行。
- 拿不到互斥量 ⇒ 返回 ESP_ERR_TIMEOUT（不得静默 ESP_OK）；闩锁由任务在 `advance_act_locked()` 顶部消费，锁竞争时任务恢复后首先关断。
- `ble_connection_sink` 记录 `uv/actuator_self_test_emergency()` 返回码，失败必日志。
- 输出仍由 water/detergent 服务 + safety_manager 关闭，无 raw HAL（bypass 扫描 PASS）。
- 单元测试（编译验证）：运行中 emergency ⇒ 双阀 OFF + 终态；未 init 时 emergency ⇒ 非 ESP_OK。

## TASK_DONE_SEM_LIFECYCLE

- stop() 成功 join 后 `vSemaphoreDelete(task_done_sem)` + 置 NULL（memset 不再丢失句柄）。
- timeout 保留 sem 与生命周期，retry 可再次 join。
- 单元测试（编译验证）：100 次 init/start/stop 循环，`uxTaskGetNumberOfTasks` + internal free 无漂移；循环后再 init/start/stop 仍可用。

## WIFI_CLOUD_FRAME_CONSISTENCY

- `publish_display_telemetry()` 先采集 Wi-Fi 快照到局部 `wifi_state_now`，再在 voice 块用 `display_cloud_from_wifi(wifi_state_now)` 计算 `voice_cloud_state`：同一帧内 Wi-Fi IP 绝不会配"云端 OFFLINE"。
- 语义修正：无真实 cloud readiness API ⇒ `display_cloud_from_wifi(IP)=2（网络就绪）`，其余=1（离线）；绝不伪报"云端 READY"。显示为"网络就绪"（青色）。
- Host 测试：IP→2、connecting/off/failed→1、同帧无 (IP, offline) 矛盾。

## UI_VISUAL_CHANGES

统一顶栏：
- 心跳 16→10px、Wi-Fi/BLE 14→12px；净距 ≥6px（220/236/254）；Wi-Fi 弧半径 5/9/13→3/6/9 防与 BLE 碰撞。
- 空闲百分比占位 `--` 移除（空串）；进度条仅细暗轨（h=8），运行才亮色填充+百分比。
- 页指示器 y=226→222；底部卡片高度收敛（核心62 / OUTPUTS·SYSTEM 66）使 LOWEST_CARD_BOTTOM=218，指示器 4px 净空。

第一页：任务卡紧凑两列（当前/剩余 + 下一步/姿态），姿态并入任务卡右列；流程卡全宽 304、六节点一行居中；空闲保持完整信息层级。

第二页：`INPUTS 霍尔`→`位置输入`，六路 0/45/90/180/270/BL 四态（激活亮色实心/未激活暗色空心/未知黄方框/故障红叉）；OUTPUTS 增紧凑图例一行小字（●ON ○OFF □? ×ERR）；SYSTEM 四行 y=13/26/39/52 不裁切；OUTPUTS 空闲时若服务快照确认 KNOWN_OFF 显示 OFF，快照 UNKNOWN 则保留 UNKNOWN（黄框，诚实表达——safety 的 mcp_known_mask 只表"已知"，ON 写也置位，不能据此推断 OFF）。

第三页：状态窄条 36px / 用户 44px(2行) / 回复 80px(4行)；后端名宽 190px 容纳"ESP-SR - 小爱同学"与切换文案；切换文案完整（正在切换到…/已切换…/切换失败，仍为…/切换超时，保持…）；UTF-8 安全截断+省略号（96/200 字节预算）+ LONG_WRAP；采集中/思考中/回复中/切换中状态色+轻量动画；云端真实状态（网络就绪/离线/未知）不用装饰掩盖。

## HOST_TESTS

- `test_runner`：308/308
- `test_voice_vad`：4/4
- `test_display_model`：22/22（+4 cloud same-frame）
- `test_uv_act_core`：47/47（+9 OFF-proof fail-closed 矩阵）
- `test_phase11_safety`：11/11（新增：target UAF 生存、边界、seq 缓存重放/冲突、Wi-Fi/cloud 组合）
- **合计 392/392**

## UNIT_TESTS

- `test_apps/unit` 构建通过（含新增 `phase11_actuator_lifecycle_tests.c`：100 循环 init/start/stop 资源稳定、运行中 emergency 关断、未 init emergency 报错）。
- 本会话未烧录单元固件（板未连接）；单元用例编译验证。

## MINIAPP_TESTS

- 171/171（act_selftest 20 + uv_selftest 12 + uv_order 15 + ble_protocol 8 + page_contract 74 + booking_flow 23 + ble_transport 19）。本轮协议改动保持 ACK 格式不变，小程序无需变更。

## PYTHON_TESTS

- 39/39（phase1_host 23 + runner_offline 16）。

## MAIN_BUILD / UNIT_BUILD

- MAIN_BUILD：PASS（BUILD_EXIT=0，`Project build complete`，无警告 `-Werror`）。
- UNIT_BUILD：PASS（BUILD_EXIT=0）。

## 静态扫描

- UTF-8 字形扫描（`tools/scan_utf8_glyphs.py`）：cmap 3869 覆盖，扫描 301 非 ASCII 字符，缺失 0 → PASS（新增"位置输入/网络就绪/姿态"等全部覆盖）。
- LVGL single-writer：display_service 之外无 `lv_*`/`lvgl_port_*` 调用；refresh 路径无 `delay()`/`vTaskDelay` → PASS。
- raw MCP/HAL bypass（`tools/check_safety_bypass.py`）：无新增旁路，仅既有已记录 position_service 直接调用 → PASS。
- API key 泄漏扫描：改动文件无新增硬编码密钥 → 干净。
- 显示布局（`tools/check_display_layout.py`）：13 张卡全部在 320×240 内、同页无重叠、指示器 4px 净空 → PASS。
- 无 emoji / 无 tofu 风险字形 → 干净。

## PORT / FLASH_RESULT / MONITOR

- PORT：COM8（CH340 已连接，`Get-PnpDevice PresentOnly` = OK）。
- FLASH_RESULT：**PASS — 5/5 分区 Hash verified**（bootloader 0x0 / partition-table 0x8000 / ota_data_initial 0x11000 / xiaojing_idf.bin 0x20000 / srmodels 0x9a0000），硬复位。
- MONITOR：完整 10 分钟监控按用户要求提前终止（缓冲输出未落盘）；改用 **90s 结构化窗口 + 20s 存活探针**采集证据，见下节。

## BIN_SHA256 / ELF_SHA256

- BIN：`c5bc41647c7a4a6b05fff962751d45629259e6f0ecc51ec9e9a303b27c954862`（3924176 B）
- ELF：`e65f656c72ab32f18f50f69e98e6d0e9077e74cbb97724e103733557577ea093`

## MONITOR_DURATION / RESET_BY_REASON / WDT / GURU / ASSERT / HEAP / GAP

```
MONITOR_DURATION: 完整 10 分钟监控被用户终止（未跑满，无整段数据）；
                  证据 = 90s 后烧录稳定性窗口 + 20s 存活探针
NOT_COMPLETE: 90s 窗口跑满其 90s 目标（NOT_COMPLETE=NO），但非完整 10 分钟
INITIAL_POWERON_SEEN: NO（上电复位发生在烧录硬复位时刻，早于监控窗口；
                      由烧录日志 "Hard resetting via RTS pin" 佐证）
RESET_BY_REASON: {}（90s 窗口内 0 次复位）
MID_MONITOR_RESETS: 0
WDT_COUNT: 0
GURU_COUNT: 0
PANIC_ABORT_COUNT: 0
ASSERT_COUNT: 0
DISPLAY_HEARTBEAT_LOG_COUNT: 4（最大间隔 25s；心跳 tick 由 100 单调推进到 1400，
                              证明 display 任务持续存活）
TELEMETRY_UI_SENSOR_COUNT: 6（最大间隔 15s，遥测持续）
INTERNAL_FREE_START/END/MIN: 11823 / 11951 / 11823（bytes，≈11.5 KiB；真实 internal
                             free，非 iB）
INTERNAL_LARGEST_START/END/MIN: 4096 / 4096 / 4096（bytes，≈4 KiB 最大连续块）
PSRAM_FREE: --（窗口内无 HEAP[..] psram= 周期行，事件型 log_heap 未触发）
I2S_BYTES_START/END: 7632896 → 9538560（90s 窗口）；探针继续累积到 11444224，
                    语音采集活跃，iB 不再被误作 internal heap
```

> 诚实说明：完整 10 分钟监控被用户提前终止；以上为烧录后 90s 结构化窗口 + 20s 存活探针证据，板在烧录硬复位后持续运行且各窗口均 0 复位/0 WDT/Guru/panic/assert，心跳与遥测持续，internal free/largest 为真实 `h`/`L` 数据。此不等于完整 soak 结论。

## CODE_REVIEW

独立 agent（general-purpose）审查本轮 5 项修复 + display 变更，发现并处置：

| 发现 | 严重度 | 处置 |
|------|--------|------|
| `actuator_self_test_stop()` 超时路径保留 `s_rt.task`，任务自删后重试 `xTaskNotify` 指向已释放 TCB（UAF） | P1 | 修复：stop() 先做 task_done_sem 非阻塞 take——成功即证明任务已退出并清空陈旧句柄；take 失败 ⇒ 任务仍存活，通知安全（sem give 先于 vTaskDelete）。 |
| deinit 中 `task_complete_worker` 队列删除：播放（real_hal_audio_play_pcm16 可数秒）期间强制置空句柄并删队列（fragile） | P1 | 硬化：worker 的 xQueueReceive 超时 1s < 轮询 1.5s，故删队列时其必已离开 receive（当前不可达 UAF）；仍将"等 worker 真正退出"改为返回 ESP_ERR_TIMEOUT 保留资源，禁止在活任务上强删队列。 |
| `request()` 无条件清 `emergency_latch`（理论竞态） | P2 | 接受为良性：emergency() 在抢到锁的路径自己执行 OFF+终态，闩锁仅为后备；单调用者（BLE 帧路径）下不可达。 |
| stop() 的互斥量外窗口内并发 request() 可能重上电 | P2 | 修复：stop() 首个锁内即 `started=false`，并发 request() 被 ESP_ERR_INVALID_STATE 拒绝。 |
| display OUTPUT "known==OFF" 推断不成立（safety ON 写也置 known 位） | P2 | 修复：移除该推断，服务快照 UNKNOWN 时保留 UNKNOWN（黄框），诚实表达。 |

CODE_REVIEW_P0=0，CODE_REVIEW_P1=0（2 项 P1 均修复），CODE_REVIEW_P2=0（3 项 P2 中 2 项修复、1 项接受为良性单调用者不可达）。

## PHASE11_GROUP_A_ROUND1_SOFTWARE_GATE

- 软件门禁：**PASS**（P0=0、P1=0、全部自动测试与构建通过、字形/旁路/布局扫描通过）。
- 烧录 + 启动稳定性：PASS（5/5 分区 Hash verified；烧录后监控窗口 0 复位/0 WDT/Guru/panic/assert，心跳与遥测持续）。完整 10 分钟监控按用户要求提前终止，稳定性证据为 90s 窗口 + 20s 探针。
- 硬件门禁保持 NEEDS_USER_TEST（真实水阀/泵/断线关断未由用户观察）。

## READY_FOR_USER_ACTUATOR_TEST / NEEDS_USER_*

- READY_FOR_USER_ACTUATOR_TEST：**YES**（软件 + 构建 + 烧录 + 启动稳定性通过；固件处于安全 OFF 状态，等待用户点击板测）。
- NEEDS_USER_SOURCE_VALVE_TEST：YES — 接好水路+容器，点自检，IN 亮约 1.5s，阀动作，自动关，output_confirmed_off=true。
- NEEDS_USER_TRANSFER_VALVE_TEST：YES — 中间箱加水，点自检，TR 亮约 1.5s，水转移，自动关。
- NEEDS_USER_DETERGENT_PUMP_TEST：YES — 泵管安装，点自检，DT 亮约 0.8s，泵动作，自动关。
- NEEDS_USER_POSITION_NOT_ZERO_TEST：YES — 姿态非 0° 时点自检应被 POSITION_NOT_ZERO 拒绝。
- NEEDS_USER_IN_TR_INTERLOCK_TEST：YES — IN 运行中发起 TR（或反之）应被拒绝（safety 联锁）。
- NEEDS_USER_BLE_DISCONNECT_TEST：YES — 任一自检运行中断开 BLE，输出应请求关闭、状态显示 INTERRUPTED/UNKNOWN。
- NEEDS_USER_UI_VISUAL_CONFIRMATION：YES — 目视三页顶栏/流程节点/位置输入四态/OUTPUTS 图例/语音页几何与切换文案。

## 用户实测顺序

有压进水阀 → 零压转移阀 → 蠕动泵 → POSITION_NOT_ZERO 拒绝 → IN/TR 互锁拒绝 → 任一运行中断开 BLE 验证断线关断 → 每项动作后确认 output_confirmed_off=true。
