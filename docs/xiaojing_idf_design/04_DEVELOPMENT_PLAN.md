# 小净 ESP-IDF 5.5.4 开发规划

本文是 Claude Code + MiMo V2.5 Pro 的执行顺序。一次只执行一个 Phase。每个 Phase 通过退出门槛后才能进入下一个。

## 0. 总体规则

### 0.1 阶段状态

`docs/xiaojing_idf_design/08_PROGRESS.md` 中每个 Phase 只能处于：

- `NOT_STARTED`
- `IN_PROGRESS`
- `BLOCKED_TOOLCHAIN`
- `BLOCKED_HARDWARE`
- `FAILED_REVIEW`
- `DONE`

只有构建/测试证据齐全才能标记 `DONE`。

### 0.2 每阶段固定动作

```text
读取 CLAUDE.md + 当前 Phase
-> 调用指定 Skill 先分析，不修改
-> 检查现有代码/git diff
-> 实现最小范围
-> build + tests
-> /code-review 审查真实 diff
-> 修复并复测
-> 更新 PROGRESS.md
```

### 0.3 完成报告格式

```text
Phase:
状态:
使用的 Skills:
修改文件:
关键设计决定:
执行命令:
测试结果:
未验证项:
风险/标定项:
下一 Phase:
```

## Phase 0：现有模板审计与安全基线

### 目标

在现有 `ESP32-IDF-Test` ESP-IDF 5.5.4 模板上建立可重复构建的安全基线。先验证和复用已有 CMake、环境 hook、main 与目录，不删除模板后重建；默认 fake 输出，尚不实现真实业务。

### Skills

- `/superpowers`：规划空工程和工作拆分。
- `/esp32`：检查目标芯片、Flash/PSRAM、GPIO表、分区和 IDF 5.5.4 配置。
- 完成后 `/code-review`。

### 工作项

1. 完整阅读根 `CLAUDE.md/AGENTS.md`，不得修改 `.claude/` 环境 hook。
2. 激活 ESP-IDF 5.5.4，记录 `idf.py --version`、Python、CMake、Ninja 和 Git 版本。
3. 在任何代码修改前运行一次 `idf.py build`，保存模板基线结果。
4. 审计并复用现有根 `CMakeLists.txt`、`main/`、`components/`、`.gitignore` 和 README；只补缺失项。
5. 不直接编辑生成的 `sdkconfig`；通过 `sdkconfig.defaults`、Kconfig 或 `idf.py menuconfig` 配置 16MB Flash、8MB OPI PSRAM、NimBLE、NVS、OTA、coredump。
6. 创建 Kconfig 输出模式，默认 `FAKE`，PTC 默认禁用。
7. 保持 `app_main` 简洁，只打印版本、输出模式和安全启动结果，再委托 bootstrap。
8. 补充 README 的环境激活、build、flash、monitor 命令。
9. 建立最初静态目录/GPIO检查；不提前创建全部业务组件空壳。

### 允许修改

现有工程根、`main/`、必要的基础 CMake/Kconfig 和本设计目录；不修改 `.claude/`，不直接编辑 `sdkconfig`，不创建业务组件空壳森林。

### 测试

```powershell
idf.py --version
idf.py set-target esp32s3
idf.py reconfigure
idf.py build
```

### 退出门槛

- 版本明确为 ESP-IDF 5.5.4。
- `idf.py build` 成功。
- 默认配置明确显示 FAKE 和 PTC disabled。
- 旧 Arduino 工程零改动。

## Phase 1：公共契约、配置、状态和 Fake HAL

### 目标

建立后续所有组件共享的稳定类型，先让状态机具备可测试的假硬件环境。

### Skills

- `/superpowers` + `/embedded-systems`：先审查类型、所有权、并发与 fake time 设计。
- 完成后 `/code-review`。

### 组件

- `machine_types`
- `safety_contract`
- `hal_contract`
- `fake_hal`
- `machine_config`
- `machine_status_store`
- `board_config`

### 工作项

1. 定义姿态、整机状态、阶段、故障、service result、request_id 和事件。
2. 定义固定容量 `wash_intent/program/status` 基础结构，避免运行路径动态分配。
3. 定义 `xiaojing_hal_t` 函数表。
4. fake HAL 支持：可控时间、输入脚本、输出历史、故障注入、原子快照。
5. 配置采用“编译默认值 + NVS 版本化覆盖 + 校验 + 恢复默认”。
6. 状态 store 用 mutex 保证一致快照，revision 单调递增。
7. `board_config` 放完整新 GPIO/MCP 映射；提供静态/运行校验。
8. 添加 `tools/check_gpio_map.py` 检查重复分配和禁止使用旧 GPIO。

### 测试

- enum/结构边界和固定数组容量。
- request_id wrap 策略。
- fake time 前进和输出历史。
- NVS 配置版本不匹配回默认。
- 多读者状态快照一致性。
- GPIO40/41/48、GPIO1/2/21、GPIO13/18 等映射唯一性。

### 退出门槛

- 后续 service 不需要自行定义重复状态类型。
- fake HAL 可以模拟五霍尔、水位、流量、SHT 和 MCP 写失败。
- `/code-review` 无未解决的 P0/P1 问题。

## Phase 2：底层驱动与硬件抽象

### 目标

实现 ESP-IDF 5.5.4 原生驱动，但仍默认不驱动真实执行器。

### Skills

- `/esp32`：逐项审查 GPIO、LEDC、PCNT、I2C、I2S、ADC、SPI 和芯片限制。
- `/embedded-systems`：审查驱动并发、ISR、超时和资源生命周期。
- 每完成一个驱动组使用 `/code-review`。

### 驱动组 A：I2C 与 MCP

- `i2c_bus_manager`
- `mcp23017_driver`
- SHT 探测适配器
- PAJ7620 中断适配器

要求：

- 使用 ESP-IDF 5.5.4 新 I2C master API。
- MCP 输出影子寄存器、原子刷新、输入快照、INTCAP/GPIO 读取。
- GPIO13 ISR 只 `vTaskNotifyGiveFromISR` 或投递轻量事件。
- 上电先安全 OLAT，再配置 IODIR。
- I2C 错误有界重试，不能在 safety task 无限重试。

### 驱动组 B：运动与 PWM

- `ibt2_driver`
- `bl50_driver`
- `fan_driver`

要求：

- LEDC timer/channel 资源集中分配并做冲突测试。
- IBT-2 不允许 RPWM/LPWM 同时非零。
- BL50 业务 API 不暴露 BRAKE/STOP 裸极性。
- PWM=0、disable、stop/emergency 操作幂等。

### 驱动组 C：传感器

- `flow_meter`：ESP-IDF 5.5.4 PCNT。
- `water_level_sensor`：洗涤桶水位。
- `turbidity_sensor`：ADC 校准/原始值。
- `sht_sensor`：温湿度、新鲜度和 CRC/通信异常。

### 驱动组 D：音频基础

- I2S RX/TX 基础驱动和 AMP_SD 安全静音。
- 本阶段不接云端和模型。

### 测试

- 先 fake/component test。
- `hardware_bringup` 控制台只读探测 I2C/输入。
- LOW_VOLTAGE 模式才允许短时 PWM/输出点动。
- 所有 init/deinit 重复调用与失败清理。

### 退出门槛

- `idf.py build` 通过。
- 静态 GPIO/资源检查通过。
- 所有真实输出仍可被总开关禁止。
- 未确认方向/极性的驱动不会自动运动。

## Phase 3：`position_service`

### 目标

用五霍尔和 IBT-2 实现可取消、可超时、可故障注入的异步 360°姿态服务。

### Skills

- `/embedded-systems`：先设计 task、queue、debounce、状态机和 request_id。
- `/esp32`：复核 MCP INTB、LEDC/IBT-2 和实时读取策略。
- 完成后 `/code-review` 审查整个组件。

### 工作项

1. 实现 `position_service.h` 已定义接口。
2. 状态：UNINITIALIZED/IDLE_UNKNOWN/IDLE_KNOWN/MOVING/BRAKING/STOPPING/FAULT。
3. 五路 raw/stable mask，默认 30ms 非阻塞消抖。
4. 已知位置最短/配置方向，未知位置首选方向寻位。
5. 目标稳定触发、超时、冲突、cancel、emergency stop。
6. 完成事件携带 request_id，迟到事件不污染下一请求。
7. real 自动运动受“方向已标定”配置门控。

### 必测场景

- 初始 45° -> 0/90/180/270。
- 270° -> 0°跨 360°。
- 当前未知 -> 找到目标。
- 原始毛刺不算到位。
- 两霍尔冲突立即停机。
- 永远不触发目标导致超时。
- cancel 与到位同一 tick 的竞争。
- emergency stop 重复调用。

### 退出门槛

- fake 测试覆盖全部终态。
- IBT-2 双向 PWM 互斥有断言。
- 未标定配置下 real 模式拒绝自动运动。

## Phase 4：`safety_manager`

### 目标

建立所有高风险输出的唯一授权网关和锁存故障机制。

### Skills

- `/embedded-systems`：联锁、任务优先级、停机钩子、无环依赖。
- `/esp32`：MCP 故障路径和真实输出默认态。
- 完成后 `/code-review`。

### 工作项

1. 实现授权检查、apply、故障上报、故障清除请求和安全快照。
2. 注册式非阻塞停机 hook；safety 不依赖 service 组件。
3. 建立 REQUIREMENTS 的完整联锁矩阵。
4. emergency stop 的有序动作和幂等性。
5. 区分 `abort_reset` 与锁存故障。
6. 监控 20ms tick 和事件触发；不得做长网络/I2C 扫描。
7. 清故障前重新采样危险条件。

### 必测场景

- 非 270°请求 PTC 被拒绝。
- 风扇未预吹/SHT 无效请求 PTC 被拒绝。
- GPA0/GPA1/GPA3 冲突请求被拒绝。
- 姿态运动时拒绝 BL50/阀/PTC。
- 故障中拒绝所有新高风险动作。
- emergency stop 次序和重复调用。
- hook 失败时仍能关 safety 直接掌握的 MCP 输出。

### 退出门槛

- 任何测试路径不能绕过 safety 开启高风险输出。
- 锁存故障不会因为传感器恢复自动清除。

## Phase 5：`water_service`

### 目标

实现两级分批进水，最终由洗涤桶水位满结束。

### Skills

- `/embedded-systems`：状态机、定时、批次、取消和故障路径。
- `/esp32`：PCNT、水位 GPIO、MCP 阀门输出检查。
- 完成后 `/code-review`。

### 工作项

1. VALIDATING -> SOURCE_FILL -> SETTLE -> TRANSFER -> CHECK_LEVEL 循环。
2. GPA0/GPA1 通过 safety apply，永不同时开启。
3. SOURCE_FILL 检查无流、低流、单批脉冲/时间。
4. TRANSFER 后检查洗涤桶水位；未满下一批。
5. 总超时、最大批次、MCP/PCNT/水位异常。
6. cancel/emergency 关闭两阀并只发一次终态事件。
7. 未标定 pulses/L 时只上报脉冲和基于时间的保守批次。

### 必测场景

- 一批后满、三批后满。
- 水位启动前已满：不得开 GPA0。
- 开 GPA0 无脉冲。
- 低流量持续。
- 最大批次仍未满。
- SOURCE_FILL/TRANSFER/CHECK 各阶段 BTN3。
- MCP 写失败。
- 姿态从 0°丢失。

### 退出门槛

- fake 输出历史证明阀门互斥。
- 每个失败路径最终 GPA0/GPA1 都为 OFF。

## Phase 6：烘干及辅助服务

### 目标

实现 `dry_service`、BL50、排水、UV、洗衣液等可组合服务。

### Skills

- `/embedded-systems`：各局部状态机与 cancel。
- `/esp32`：LEDC、BL50 控制、SHT/I2C、MCP 输出。
- 每个服务组完成后 `/code-review`。

### `dry_service`

- 270°验证、PRE_FAN、HEATING_ON/OFF、COOLDOWN。
- 55/45°C 初始回差、SHT stale、最大连续加热、总 30min。
- PTC 只经 safety；停止先 PTC OFF 后冷却风扇。

### `bl50_service`

- HOME、PULSATOR、DRUM、SPIN 动作。
- 斜坡、换向停顿、超时、GPB5 home。
- 每个动作前复核姿态。

### 其他服务

- `drain_service`：180°，仅 GPA3 排水阀。
- `detergent_service`：0°，按时投放，单次上限。
- `uv_service`：0°，可跳过，超时关闭。
- `button_service`：GPA4-GPA6 消抖、单击和未来事件预留。

### 必测场景

- 烘干正常周期、55°C 截止、45°C 恢复。
- SHT stale/错误立即 PTC OFF，风扇冷却。
- 非 270°拒绝烘干。
- Demo 10-20s PTC 仍经过联锁。
- 排水非 180°拒绝。
- 泵/UV 非 0°拒绝。
- BTN3 在所有辅助服务中关闭输出。

### 退出门槛

- 所有服务异步、可取消、request_id 正确。
- 高风险输出无旁路。

## Phase 7：`wash_planner` 与 `wash_executor`

### 目标

完成 Formal、Demo、自定义计划编译和唯一整机流程状态机。

### Skills

- `/superpowers` + `/embedded-systems`：计划编译、状态机、队列所有权。
- 完成后 `/code-review`。

### Planner 工作项

1. 固定容量 intent/program/report。
2. Formal/Demo profile 参数表，不复制状态机。
3. 补齐姿态、合并相邻重复 move。
4. 强制进水和排水。
5. 总时长 1h、烘干累计 30min。
6. UV 可关闭，结束仍回 45°。
7. 对裁剪、补齐、拒绝输出可解释报告。

### Executor 工作项

1. BOOT/IDLE/WAIT_LOAD/RUNNING/WAIT_UNLOAD/UV/PAUSED/RESETTING/FAULT。
2. 每步生成新 request_id，等待匹配终态事件。
3. BTN3 urgent queue 抢占普通事件。
4. `abort_reset` 安全关闭后 45° + BL50 home。
5. 故障不自动机械复位。
6. 进度、预计剩余和完整 status snapshot。
7. 重启恢复策略：上电不自动恢复危险动作，进入安全 idle/fault 并记录中断程序。

### 必测场景

- Formal 完整 happy path。
- Demo 自动动作不超过 3min。
- Agent 多轮/乱序动作被正确补姿态。
- 缺进水/排水被补齐并报告。
- UV disabled 仍回 45°。
- 1h/30min 边界与超限。
- 每个步骤 BTN3 抢占。
- 迟到 request_id 被忽略。
- fault 后禁止继续步骤。

### 退出门槛

- 全流程在 fake HAL 可确定性运行。
- planner 无 FreeRTOS/硬件/JSON 依赖。
- executor 是唯一整机状态所有者。

## Phase 8：`app_protocol` 与 BLE

### 目标

完成小程序 V1 握手、命令、状态、分包和旧协议兼容。

### Skills

- `/embedded-systems`：RX/TX task、队列、拥塞、内存所有权。
- `/esp32`：NimBLE/PSRAM/任务资源检查。
- 完成后 `/code-review`。

### 工作项

1. FFF0/FFF1/FFF2、设备名 `JJTP-XIAOJING`。
2. 回调只复制 chunk；RX task 2048B 重组、3s 超时。
3. 安全 JSON 完整性检测，正确处理字符串内 `{}`。
4. V1 `hello -> ACK -> capability -> full status`。
5. seq 对应和幂等缓存。
6. 命令路由到统一 app request。
7. TX 串行化；critical ACK/fault 不被 status 挤掉。
8. 旧 start/stop 和 P/S/A 兼容开关。
9. 断连继续本地流程，重连返回完整快照。

### 小程序策略

不要在旧 Arduino 根目录修改 `miniapp4.0`。需要联调时先复制为独立同级工程：

```text
D:/UserData/ESP32Project/IDF/miniapp4.0_IDF
```

再按旧 `miniapp4.0/miniprogram/utils/ble.js` 和 `pages/wash/wash.js` 定点迁移。

### 必测场景

- hello 成功/版本错误。
- 1/20/21/2048 字节帧。
- 分片超时、越界、坏 JSON、字符串花括号。
- 重复 seq 不重复启动程序。
- BUSY/PLAN_REJECTED/PLAN_CLIPPED。
- fault 抢占普通 status。
- 断线重连。
- 旧 start/stop。

### 退出门槛

- BLE 回调无业务执行和硬件调用。
- 自动协议测试脚本通过。
- 小程序收到完整姿态、阶段、故障和进度。

## Phase 9：`voice_agent`

### 目标

迁移唤醒/VAD/云端/TTS，接入新的产品意图和统一执行链路。

### Skills

- `/superpowers` + `/embedded-systems`：网络、音频、状态机、资源生命周期。
- `/esp32`：I2S、PSRAM、任务栈、Wi-Fi/BLE 共存。
- 若出现 TLS/崩溃使用 `/systematic-debugging`。
- 完成后 `/code-review`。

### 工作项

1. 只定点参考旧音频和 Edge Impulse 模型适配，禁止复制 Arduino HTTP/I2S 封装。
2. ESP-IDF 原生 I2S、esp_http_client/esp-tls、证书校验。
3. voice phase：idle/wake/record/upload/wait/parse/speak/submit/error。
4. 新 MiMo tool schema，只产生产品动作。
5. parser 做 schema、枚举、数量、时长和字符串边界检查。
6. planner 接受后再确认执行；裁剪时给出提示。
7. BTN3/fault 抢占交互；网络失败不改变硬件。
8. 记录 heap/PSRAM/stack，不记录密钥和完整音频。

### 必测场景

- 合法 Formal/Demo/羊毛无 UV tool call。
- 裸 `heater_on/valve_on/GPIO` 被拒绝。
- 非法姿态、未知动作、超大数组、超时长。
- HTTP 4xx/5xx、TLS、超时、断网、JSON 截断。
- 录音/上传/TTS 时 BTN3。
- BLE 同时连接下的状态同步。
- 连续多轮交互无 heap 持续下降。

### 退出门槛

- voice 不直接依赖 service/driver。
- 24h 或规定压力测试无稳定性回退后再默认启用。

## Phase 10：显示、诊断与产品集成

### 目标

补齐显示适配、串口诊断、复位/故障记录和统一状态展示。

### Skills

- `/embedded-systems`。
- 异常使用 `/systematic-debugging`。
- 完成后 `/code-review`。

### 工作项

- 显示只读 `machine_status_store`。
- 展示五姿态、状态、阶段、进度、温湿度、主要故障。
- 串口诊断命令默认只读；危险校准命令受 output mode 和二次确认保护。
- 记录 reset reason、coredump、最小 heap、栈余量、I2C/网络错误计数。
- 完善 README、分区、OTA 和恢复策略。

### 退出门槛

- 显示/日志压力不会饿死 safety/position task。
- 断电/重启后所有输出先进入安全态。

## Phase 11：逐级硬件联调与标定

### 目标

严格按 `TEST_PLAN.md` 从逻辑电平到带水和 PTC 逐级验证。

### Skills

- `/esp32`：每个硬件等级开始前检查接线、GPIO 和外设限制。
- `/embedded-systems`：实时日志、状态机和超时。
- 任意异常 `/systematic-debugging`。
- 每个模块通过后 `/code-review`。

### 顺序

1. 3.3V 逻辑/I2C/传感器。
2. IBT-2 低占空比点动和方向标定。
3. 五姿态单步定位。
4. BL50 极性、home、低速动作。
5. 阀门/泵低压空载。
6. 无 PTC 的风扇与 SHT。
7. 带水分批进水、排水、流量标定。
8. 洗衣液泵标定。
9. 低风险完整 Demo。
10. 最后才是 220VAC PTC 看护测试。

### 退出门槛

- `CALIBRATION.md` 每项有硬件版本、日期、方法、结果和签字/确认。
- 全部关键故障注入真实验证。
- Formal/Demo 连续运行和长稳测试通过。

## Phase 12：小程序独立工程适配

### 目标

在独立 `miniapp4.0_IDF` 目录升级小程序，不修改 Arduino 工程中的原件。

### 工作项

- 复制必要小程序源文件，不复制 node_modules/归档。
- hello、seq、ACK/NACK、cap/status/fault。
- start_formal/start_demo/submit_plan/ack_load/ack_unload/skip_uv/abort_reset。
- 五姿态和新阶段 UI。
- 保留旧 P/S/A 解析作为兼容。
- 断线重连、语音阶段和完整状态恢复。

### 退出门槛

- 微信开发者工具测试通过。
- 真机 BLE 握手、控制、断线重连、故障显示通过。
