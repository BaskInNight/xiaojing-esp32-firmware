# 小净 ESP-IDF 固件需求基线

版本：1.0  
目标：ESP32-S3-WROOM-1 N16R8  
工具链：ESP-IDF 5.5.4  
状态：作为 Claude Code 实现和测试的最高优先级需求

## 1. 产品边界

本工程是“小净”非交叉污染洗衣机的新 ESP-IDF 固件，不是旧 Arduino 工程的逐行迁移。

V1 范围：

- 五姿态外筒定位。
- BL50 内筒洗涤、脱水、寻零。
- 两级进水、洗衣液、波轮洗、滚筒洗、排水、脱水、烘干、取衣后 UV。
- BTN1/BTN2/BTN3 单击交互。
- 小程序 BLE 控制和状态同步。
- 语音唤醒、MiMo tool calling、TTS。
- 统一 planner/executor/safety 架构。

V1 不要求：

- 门锁/盖子传感器，因为当前硬件没有。
- 按钮长按、双击、组合键业务。
- 风扇转速 FG 闭环，因为当前只有 PWM。
- PTC 的独立软件过温输入，因为当前只有硬件温度保险丝。
- 精确水量和洗衣液 ml 闭环，标定前先提供参数和安全上限。
- 未定义的手势业务；PAJ7620 只完成驱动与事件上报，不允许默认触发高风险动作。

## 2. 硬件映射

### HW-01 ESP32 GPIO

所有 GPIO 必须只在 `board_config` 定义。

| GPIO | 信号 | 方向/外设 | 说明 |
|---:|---|---|---|
| 1 | BL50_DIR | 输出 | 极性由配置抽象 |
| 2 | BL50_BRAKE | 输出 | 有效电平待实测 |
| 4 | WATER_LEVEL | 输入 | XKC，测洗涤桶水位 |
| 5 | TURBIDITY_ADC | ADC 输入 | 浊度，V1 可作遥测/后续策略 |
| 6 | I2C_SDA | I2C | MCP23017 + PAJ7620 + SHT |
| 7 | I2C_SCL | I2C | 总线统一加锁 |
| 9 | LCD_DC | 输出 | ILI9341 |
| 10 | LCD_CS | 输出 | ILI9341 |
| 11 | LCD_MOSI | SPI | ILI9341 |
| 12 | LCD_SCK | SPI | ILI9341 |
| 13 | MCP_INTB | 输入/中断 | ISR 不允许 I2C |
| 14 | AMP_DIN | I2S TX | MAX98357 |
| 15 | I2S_WS | I2S | 麦克风/功放共享时序 |
| 16 | I2S_BCLK | I2S | 麦克风/功放共享时序 |
| 17 | MIC_SD | I2S RX | INMP441 |
| 18 | FLOW_PCNT | PCNT 输入 | 流量计 |
| 21 | BL50_STOP | 输出 | 有效电平待实测 |
| 38 | FAN_PWM | LEDC 输出 | 双风扇同步调速 |
| 39 | BL50_PWM | LEDC 输出 | 内筒调速 |
| 40 | IBT2_RPWM | LEDC 输出 | 对应方向待实测 |
| 41 | IBT2_LPWM | LEDC 输出 | 对应方向待实测 |
| 43/44 | UART0 | UART | 调试/下载 |
| 47 | AMP_SD | 输出 | 功放使能/静音 |
| 48 | IBT2_EN | 输出 | 姿态电机使能 |

### HW-02 MCP23017

| 端口 | 功能 | 默认态/语义 |
|---|---|---|
| GPA0 | 自来水到中间水箱阀 | OFF |
| GPA1 | 中间水箱到洗涤桶转移阀 | OFF |
| GPA2 | UV 灯 | OFF |
| GPA3 | 洗涤桶排水阀 | OFF |
| GPA4 | BTN1 | 输入，低有效 |
| GPA5 | BTN2 | 输入，低有效 |
| GPA6 | BTN3 | 输入，低有效 |
| GPA7 | 220VAC/400W PTC 继电器 | OFF |
| GPB0 | 外筒 0° 霍尔 | 输入，低有效 |
| GPB1 | 外筒 45° 霍尔 | 输入，低有效 |
| GPB2 | 外筒 90° 霍尔 | 输入，低有效 |
| GPB3 | 外筒 180° 霍尔 | 输入，低有效 |
| GPB4 | 外筒 270° 霍尔 | 输入，低有效 |
| GPB5 | BL50 内筒寻零霍尔 | 输入，低有效 |
| GPB6 | PAJ7620 INT | 输入，低有效 |
| GPB7 | 5V 洗衣液蠕动泵 | OFF |

### HW-03 安全初始化

- 上电先准备所有输出 OFF 的 OLAT 值，再把 MCP 引脚配置为输出。
- IBT-2 默认 PWM=0、EN 禁用。
- BL50 默认 PWM=0，并进入按实测极性定义的 STOP/BRAKE 安全态。
- PTC、UV、所有阀、泵、风扇、功放默认关闭。
- 初始化失败不能继续进入可运行状态。

### HW-04 模组与载板身份门槛

- 软件目标型号为 `ESP32-S3-WROOM-1-N16R8`，不是带 `V` 后缀的 R8V/R16V 版本。
- 在启用 LOW_VOLTAGE/REAL 输出前，必须用实物屏蔽罩标识、采购料号和原理图确认实际模组型号。
- 如果实际芯片/模组为 `ESP32-S3R8V/R16V` 或 `N16R8V`，GPIO47/48 工作电压为 1.8V，必须增加电平转换或重新分配，不能直接驱动 3.3V AMP_SD/IBT2_EN。
- GPIO11 是 ESP32-S3 普通 I/O，本设计保留 `GPIO11=LCD_MOSI`，不得因误判 VDD_SPI 而迁移。
- DevKitC-1 v1.0/v1.1 的板载 RGB LED 分别可能占用 GPIO48/GPIO38；自研载板也可能没有。必须按实际载板版本/原理图确认，不能仅靠 Kconfig 假定硬件负载消失。
- 实物型号和载板未确认时，FAKE 模式允许开发，GPIO38/47/48 的真实执行器输出保持禁止。

## 3. 姿态系统

### POS-01 姿态定义

| 姿态 | 用途 |
|---:|---|
| 0° | 进水、洗衣液、波轮洗、UV |
| 45° | 投衣、取衣、最终待机 |
| 90° | 滚筒洗 |
| 180° | 排水、脱水 |
| 270° | 烘干 |

### POS-02 传感器

- 五个角度为五路独立霍尔，只被磁铁触发，低电平有效。
- 使用 MCP GPB0-GPB4；GPB5 单独用于 BL50 寻零。
- 霍尔必须非阻塞消抖，默认 30ms，配置范围建议 20-50ms。
- 正常稳定状态最多一个姿态霍尔有效。

### POS-03 360°运动

- 机构允许无限同方向连续旋转。
- 已知当前姿态时可以选择最短路径或配置的固定/首选方向。
- 当前姿态未知时按已配置首选方向寻找目标霍尔。
- 不按枚举数值大小判断方向。
- IBT-2 RPWM/LPWM 方向映射必须实机标定后写入配置。

### POS-04 到位与故障

- 目标霍尔稳定有效才算到位。
- 当前已在目标姿态时直接成功，不启动电机。
- 两个及以上姿态霍尔稳定有效：立即停机并锁存 `HALL_CONFLICT`。
- 到位超时：立即停机并锁存 `POSITION_TIMEOUT`。
- 运动期间每约 10ms 主动读取 MCP 输入；INTB 只用于唤醒加速。
- cancel/emergency stop 必须在有界时间内关闭 PWM/EN，且重复调用安全。

## 4. 水路

### WATER-01 外部动作

小程序、语音和 planner 只看到一个 `water_in` 动作。内部不得暴露 `valve_on`。

### WATER-02 前置姿态

- `water_in` 只能在外筒稳定 0°时开始。
- 姿态丢失、故障或排水阀开启时，禁止进水。

### WATER-03 两级批次循环

现有水位传感器测洗涤桶，不测中间水箱。内部流程为：

```text
SOURCE_FILL：GPA0 ON，向中间水箱注水
SOURCE_SETTLE：GPA0 OFF
TRANSFER：GPA1 ON，将本批水转入洗涤桶
CHECK_DRUM_LEVEL：GPA1 OFF，读取洗涤桶水位
未满 -> 下一批 SOURCE_FILL
满 -> water_in 完成
```

### WATER-04 阀门互锁

- GPA0 和 GPA1 永远不能同时开启。
- 进水阀/转移阀与排水阀不能同时开启。
- 互锁同时在 `water_service` 与 `safety_manager` 实现。

### WATER-05 流量计

- GPIO18 使用 ESP-IDF 5.5.4 PCNT 驱动。
- 用于检查 GPA0 打开后有无水流、低流量异常、累计脉冲和估算体积。
- `pulses_per_liter=0` 表示未标定，不能用估算 ml 作为唯一停止条件。
- 建议无流默认 3-5s，可配置。

### WATER-06 完成条件

- 正常完成条件是洗涤桶水位满。
- V1 不设置整个程序的最大水量硬上限，但必须有单批上限、最大批次数和进水总超时。
- 中间水箱单批进水脉冲/时间和转移时间属于标定项。

### WATER-07 异常

- 开 GPA0 后无流量：`WATER_NO_FLOW`。
- 达到最大批次或进水总超时仍未满：`WATER_TIMEOUT/WATER_LEVEL_INVALID`。
- 中断、故障、I2C/MCP 写失败：先请求关闭 GPA0/GPA1，再上报终态。
- 排水独立为 180°步骤，只控制 GPA3，没有排水泵。

## 5. 烘干

### DRY-01 前置条件

- 烘干只能在稳定 270°进行。
- PTC 开启前必须：风扇预吹至少 5s、SHT 数据有效且新鲜、无锁存故障。

### DRY-02 保守策略

- 双风扇由 GPIO38 同一 PWM 同步控制，无 FG 反馈。
- PTC 单次连续开启初始上限 60s。
- 初始策略可用 60s 开/60s 关或更保守。
- 初始 55°C 关 PTC，降到 45°C 以下才允许再次加热。
- SHT 无效或 I2C 失败立即关 PTC，可保留风扇通风。
- 停止、取消和故障时先关 PTC，风扇继续冷却默认 60s。

### DRY-03 时长

- 所有程序累计烘干不超过 30min。
- Demo PTC 实际加热建议 10-20s，仍必须满足全部联锁。
- 只有硬件温度保险丝，不得将其作为正常温控手段。

## 6. 洗衣液、UV 和内筒

### AUX-01 洗衣液

- 只允许在 0°运行 MCP GPB7。
- 未标定前按时间：Demo 默认 2-3s，Formal 默认 6s。
- 预留单次最大时间和 `ml_per_second`；中断/故障立即关闭。

### AUX-02 UV

- 只允许在 0°且与其他危险动作无冲突时开启 GPA2。
- 正式程序在用户取衣确认后执行，默认 10min。
- BTN3 在 UV 阶段表示跳过 UV，然后回 45°待机。
- Agent 可以关闭 UV；羊毛、生物织料等场景允许关闭。

### AUX-03 BL50

- GPIO39 PWM、GPIO1 DIR、GPIO2 BRAKE、GPIO21 STOP。
- BRAKE/STOP 有效电平待实机确认，业务层不得感知裸电平。
- PWM 斜坡启动，禁止命令洪泛。
- 每个洗涤/脱水动作前复核姿态；姿态未知或故障时禁止启动。
- BTN3 复位目标包含 BL50 内筒寻零，使用 MCP GPB5 霍尔。

## 7. 程序和交互

### FLOW-01 通用主流程

```text
IDLE
 -> 45° 投衣姿态
 -> WAITING_LOAD（按钮/小程序确认）
 -> BL50 HOME
 -> 按计划执行水路/洗涤/排水/脱水/烘干
 -> 45° 取衣姿态
 -> WAITING_UNLOAD（按钮/小程序确认）
 -> 可选：0° UV
 -> 45°
 -> IDLE
```

### FLOW-02 Formal

- 默认目标总时长约 30min，可由配置和 Agent 调整。
- 等待投衣/取衣的人为时间不计入自动动作预算。
- 进水和排水必须存在。

### FLOW-03 Demo

- 与 Formal 共用同一状态机和流程模板，只使用更短参数。
- 自动动作目标不超过 3min；人工等待不计入。
- 必须能展示 45/0/90/180/270 姿态和关键动作概念。
- Demo 烘干允许短时 PTC，但不得使用旁路逻辑。

### FLOW-04 Agent 计划边界

- 最大自动动作总时长 1h。
- 累计烘干最大 30min。
- 进水和排水不可跳过；缺失时 planner 补齐并报告计划被修改。
- 波轮、滚筒、脱水轮数、次序、时间可自定义。
- Agent 可以请求合法姿态、进水、排水、烘干等产品动作。
- Planner 必须补齐/调整每个动作的安全姿态。
- Agent 不得输出 GPIO/MCP/PWM/继电器、裸 PTC/风扇/阀/泵或 `home_bldc` 控制。

### FLOW-05 按钮 V1

- BTN1 单击：IDLE 启动 Formal；WAITING_LOAD 时确认 Formal 投衣完成。
- BTN2 单击：IDLE 启动 Demo；WAITING_LOAD 时确认 Demo 投衣完成。
- WAITING_UNLOAD 时 BTN1 或 BTN2 单击均可确认已取衣。
- BTN3 单击：UV 阶段跳过 UV；其他运行阶段执行 `abort_reset`。
- `abort_reset`：关闭水阀和高风险输出，取消当前步骤，条件安全后外筒回 45°并执行 BL50 寻零。
- 第一版只实现单击；事件结构预留长按、双击、组合键。

上述 WAITING_LOAD/WAITING_UNLOAD 的具体按钮映射是 V1 软件约定，可集中在 `button_service` 配置中调整，不能散落在 executor。

## 8. 软件架构

### ARCH-01 唯一流程所有者

`wash_executor` 是唯一整机流程状态机。服务只维护自己的局部状态。

### ARCH-02 Planner

`wash_planner` 为纯函数组件，输入 `wash_intent_t`，输出不可变 `wash_program_t + wash_plan_report_t`。

### ARCH-03 异步服务

- position/water/dry 等服务使用 command queue。
- 请求带单调递增 `request_id`。
- 完成、取消、超时、故障通过 `machine_event_queue` 返回 executor。
- executor 忽略与当前步骤 request_id 不匹配的迟到事件。

### ARCH-04 Safety

- Safety 在动作前授权并在运行中持续撤销不安全输出。
- 锁存故障不能自行恢复。
- 紧急停机钩子非阻塞、幂等，不能形成组件环依赖。

### ARCH-05 状态快照

- `machine_status_store` 保存一致的整机快照和递增 revision。
- BLE、显示和 Web 只读快照，不访问服务内部变量。

## 9. BLE 小程序协议

### BLE-01 GATT

| 项 | 值 |
|---|---|
| Device Name | `JJTP-XIAOJING` |
| Service | `0000FFF0-0000-1000-8000-00805F9B34FB` |
| Write | `0000FFF1-0000-1000-8000-00805F9B34FB` |
| Notify | `0000FFF2-0000-1000-8000-00805F9B34FB` |

### BLE-02 握手

```text
连接 -> 发现服务/特征 -> enable notify -> app 写 hello
-> device ACK -> capability + full status -> online
```

### BLE-03 帧与分片

- V1 JSON 帧含 `v/seq/type/cmd/payload`。
- 最大完整 JSON 2048 字节。
- 支持多次 Write 拼接；分片超时 3000ms。
- NimBLE 回调只复制数据，JSON 在 `ble_rx_task/app_protocol_task` 解析。
- ACK 表示命令被接受，不表示洗涤完成。
- seq 用于 ACK 对应和短期幂等去重。

### BLE-04 命令

`hello/get_status/start_formal/start_demo/submit_plan/ack_load/ack_unload/skip_uv/abort_reset/pause/resume`。

### BLE-05 状态与兼容

- 状态变化立即 Notify，运行中建议 1s 周期精简状态。
- fault/abort ACK 优先级高于普通周期状态。
- 保留旧 `{"cmd":"start"}` -> Formal、`{"cmd":"stop"}` -> abort_reset。
- 可保留 `P,`、`S,`、`A,` 紧凑通知供旧小程序使用。
- BLE 断开不自动停止本地程序；重连 hello 后返回完整快照。

## 10. 语音与 MiMo

### VOICE-01 链路

保留“小净”唤醒词、VAD 录音、HTTPS MiMo tool calling、TTS，重写工具协议和执行适配。

### VOICE-02 权限

- 语音 parser 只生成 `wash_intent_t/app_request_t`。
- 所有计划经 `app_protocol -> wash_planner -> wash_executor -> safety_manager`。
- 云端失败、超时、非法 JSON 或未知动作不能改变硬件输出。
- TTS 只有在 planner 接受后才能确认开始；计划被裁剪时要说明。

### VOICE-03 并发

- 网络/TTS 不阻塞控制任务。
- BTN3 和故障可抢占录音、上传和播放。
- 默认保留 BLE 连接；如需降低 BLE 活动，只暂停普通周期通知，结束后发送完整状态。

## 11. 安全与非功能要求

### SAFE-01 输出模式

默认 fake HAL；LOW_VOLTAGE 和 REAL 需显式配置。PTC 还需独立使能。

### SAFE-02 BTN3 停机顺序

```text
拒绝新动作 -> PTC OFF -> BL50 STOP/PWM0
-> GPA0/GPA1/泵/UV OFF -> 必要时风扇冷却
-> cancel 当前服务 -> 确认安全 -> 45° + BL50 home
```

### SAFE-03 故障停机

霍尔冲突、MCP/I2C 失败、进水异常、SHT/超温等故障先关闭危险输出并锁存。故障态不盲目机械复位。

### SAFE-04 时间与阻塞

- 禁止 Arduino `delay()` 和忙等。
- 周期任务使用 `vTaskDelayUntil()`；网络只在低优先级任务。
- 控制任务循环必须有界，并配置 task watchdog 策略。

### SAFE-05 密钥

Wi-Fi 和 MiMo API key 不进入版本库；使用 NVS/menuconfig/本地未跟踪配置。HTTPS 校验证书。

### NFR-01 可测试性

- 所有驱动通过接口/函数表支持 fake。
- planner 可在 host 环境测试。
- 服务状态机可用假时间、假传感器、假输出做故障注入。
- 每个需求 ID 在 `TEST_PLAN.md` 至少有一个测试或明确硬件验证项。

### NFR-02 可诊断性

- 记录复位原因、主要故障、运行阶段、request_id、最小 heap、关键任务栈余量。
- 日志不得泄露密钥和完整音频。
- fault/status 必须能通过 BLE 和串口读取。

## 12. 待标定而非待设计

以下参数不阻塞接口和 fake 实现：

- IBT-2 方向映射、PWM、到位惯性和超时。
- BL50 BRAKE/STOP 极性。
- 中间水箱每批进水量/时间、GPA1 转移时间。
- 流量计 pulses/L。
- 洗衣液泵 ml/s。
- PTC/SHT 安装后的温升、回差和周期参数。

未标定参数必须集中配置、使用保守值或保持真实输出禁用。
