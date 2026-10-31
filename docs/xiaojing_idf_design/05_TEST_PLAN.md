# 小净 ESP-IDF 测试计划

工具链：ESP-IDF 5.5.4  
原则：先 fake、后低压、最后带水与 220VAC；不得跨级。

## 1. 测试等级

| 等级 | 环境 | 允许输出 | 进入条件 |
|---|---|---|---|
| T0 | 静态/编译 | 无 | 工具链可用 |
| T1 | Host 单元测试 | fake | T0 通过 |
| T2 | ESP32 target + fake HAL | fake | T1 通过 |
| T3 | 3.3V 逻辑台架 | 仅输入/I2C/示波器 | 人工检查接线 |
| T4 | 5V/低压执行器 | 受限电机/阀/泵，无水或安全负载 | 方向/极性逐项确认 |
| T5 | 带水低压整机 | 阀、电机、泵，PTC 禁用 | 漏水/排水方案就绪 |
| T6 | 完整整机 | 含 220VAC PTC | 专项安全确认和看护 |

任何等级失败必须退回安全输出，记录日志和复现步骤。T6 不允许自动无人执行。

## 2. 通用测试命令

### 主工程

```powershell
idf.py --version
idf.py reconfigure
idf.py build
idf.py size
idf.py size-components
```

### Target Unity 测试应用

```powershell
Push-Location test_apps/unit
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
Pop-Location
```

### Host planner 测试

```powershell
cmake -S host_tests -B build/host -G Ninja
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

### 硬件 bring-up

```powershell
Push-Location test_apps/hardware_bringup
idf.py build
idf.py -p COMx flash monitor
Pop-Location
```

实际 COM 口、测试输出和固件 hash 记录到 `PROGRESS.md/CALIBRATION.md`。

## 3. T0 静态与构建测试

| ID | 检查 | 预期 | 需求 |
|---|---|---|---|
| T0-01 | `idf.py --version` | 5.5.4 | 工具链 |
| T0-02 | set-target/build | esp32s3 编译成功 | 全局 |
| T0-03 | GPIO 扫描 | 无重复、无旧步进/EC11 引脚残留 | HW-01 |
| T0-04 | 依赖扫描 | 无组件环和禁止反向依赖 | ARCH-01~04 |
| T0-05 | secret 扫描 | 无 Wi-Fi/API key/证书私钥 | SAFE-05 |
| T0-06 | 阻塞 API 扫描 | 业务层无 delay/忙等 | SAFE-04 |
| T0-07 | 输出模式 | 默认 FAKE，PTC disabled | SAFE-01 |
| T0-08 | stack/heap 配置 | 有诊断和合理初值 | NFR-02 |
| T0-09 | MCP 静态映射 | GPA0-GPA7/GPB0-GPB7 与新接线表逐项一致 | HW-02 |
| T0-10 | 模组/载板身份记录 | 记录实物完整料号、是否带V后缀、DevKit/自研载板及RGB LED实际连接 | HW-04 |
| T0-11 | GPIO11资料复核 | board_config保持LCD_MOSI，官方S3资料确认普通I/O | HW-04 |

## 4. Planner 单元测试

| ID | 输入 | 断言 | 需求 |
|---|---|---|---|
| PLAN-01 | 默认 Formal | 45投衣、home、进水、排水、45取衣、UV、45待机 | FLOW-01/02 |
| PLAN-02 | 默认 Demo | 同模板、参数短、自动预算 <=180s | FLOW-03 |
| PLAN-03 | 缺进水 | 自动补齐并 report changed | FLOW-04 |
| PLAN-04 | 缺排水 | 自动补齐并 report changed | FLOW-04 |
| PLAN-05 | 水动作前无 0° | 插入 MOVE 0 | POS-01/WATER-02 |
| PLAN-06 | 滚筒前无 90° | 插入 MOVE 90 | POS-01 |
| PLAN-07 | 排水/脱水前无 180° | 插入 MOVE 180 | POS-01 |
| PLAN-08 | 烘干前无 270° | 插入 MOVE 270 | DRY-01 |
| PLAN-09 | 相邻相同姿态 | 合并冗余 move | ARCH-02 |
| PLAN-10 | UV disabled | 不生成 UV，但最终 45° | AUX-02 |
| PLAN-11 | 总时长=3600000ms | 接受边界 | FLOW-04 |
| PLAN-12 | 总时长>3600000ms | 裁剪或拒绝并有报告 | FLOW-04 |
| PLAN-13 | dry=1800000ms | 接受边界 | DRY-03 |
| PLAN-14 | dry>1800000ms | 裁剪或拒绝 | DRY-03 |
| PLAN-15 | 非法角度/未知动作 | 拒绝/丢弃并报告 | FLOW-04 |
| PLAN-16 | 最大 action 数 | 不越界，结果确定 | NFR-01 |
| PLAN-17 | 羊毛计划 | 可关闭 UV，温和参数 | FLOW-04 |
| PLAN-18 | 默认 Formal 参数 | 自动动作目标约30min，人工等待不计入 | FLOW-02 |

Planner 测试要求相同输入得到字节级一致的逻辑结果，不依赖系统时间和任务调度。

## 5. Position Service 测试

### Fake/自动测试

| ID | 场景 | 预期 | 需求 |
|---|---|---|---|
| POS-T01 | 启动时只有 45°有效 | IDLE_KNOWN/45 | POS-02 |
| POS-T02 | 启动时无霍尔 | IDLE_UNKNOWN | POS-03 |
| POS-T03 | 已在目标 45° | 不输出 PWM，立即 DONE | POS-04 |
| POS-T04 | 270->0 | 正确跨零方向和到位 | POS-03 |
| POS-T05 | 未知->90 PREFER_CW | 按 CW 方向寻找 | POS-03 |
| POS-T05b | 未知->90 PREFER_CCW | 按 CCW 方向寻找 | POS-03 |
| POS-T06 | 10ms 毛刺 | stable mask 不改变 | POS-02 |
| POS-T07 | 持续30ms目标低 | 到位成功 | POS-02/04 |
| POS-T08 | MOVING 两霍尔同时稳定 | PWM/EN OFF，HALL_CONFLICT LATCHED | POS-04 |
| POS-T08b | 启动双霍尔冲突 | 直接 FAULT/HALL_CONFLICT | POS-04 |
| POS-T08c | IDLE 双霍尔冲突 | FAULT/HALL_CONFLICT | POS-04 |
| POS-T09 | 目标永不触发 | FAULT, TIMEOUT, 终态恰好一次 | POS-04 |
| POS-T10 | move 中 cancel | 有界停止、CANCELED 一次 | ARCH-03 |
| POS-T11 | cancel 与到位竞态 | 只有一个终态事件 | ARCH-03 |
| POS-T12 | 旧 request cancel 不影响新请求 | 新请求继续 MOVING | ARCH-03 |
| POS-T13 | emergency 重复10次 | 不崩溃，HAL 记录 emergency | SAFE-03 |
| POS-T14 | RPWM/LPWM | 历史非空且不同时非零 | HW-03 |
| — | 45->0/270 最短路径 | 正确方向选择 | POS-03 |
| — | 未标定拒动 | REJECTED, 无 IBT-2 输出 | POS-04 |
| — | 未标定已在目标 | 立即 OK (不需运动) | POS-04 |
| — | MCP 读取失败 | FAULT_MCP_IO | POS-04 |
| — | 非法 request/target/policy | 返回错误码 | ARCH-03 |
| — | 重复 init/start/stop | 正确拒绝或幂等 | ARCH-03 |
| — | emergency/arrival 竞争 | 安全停止 | SAFE-03 |

> POS-T12: Phase 3 只验证 position_service 不产生重复/迟到终态。
> executor 的迟到事件过滤留到 Phase 7。

### 实机步骤

1. 断开机械负载或架空，LOW_VOLTAGE 模式。
2. `/esp32` 检查 GPIO40/41/48 和 LEDC 配置。
3. 5-10% PWM 短点动 RPWM，记录实际方向，立即停止。
4. 5-10% PWM 短点动 LPWM，记录实际方向。
5. 写入 `rpwm_is_cw`，重复验证。
6. 手动经过五个磁铁，观察 raw/stable mask 和角度映射。
7. 单次低速寻位，逐步测试五个目标。
8. 实测惯性、制动、PWM 和超时；写入 `CALIBRATION.md`。

立即断电条件：双 PWM 同时非零、方向与预期不符且无法停止、机械干涉、异常噪声/发热、霍尔冲突未停机。

在任何 GPIO38/47/48 低压输出测试前，必须先通过 T0-10。若模组属于 R8V/R16V 电压域，先验证电平转换，不允许直接连接 3.3V 外设输入。

## 6. Safety Manager 测试

| ID | 故障/请求 | 预期 | 需求 |
|---|---|---|---|
| SAFE-T01 | 非270° PTC ON | 拒绝 HEATER_LOCKED | DRY-01 |
| SAFE-T02 | 风扇未预吹 PTC | 拒绝 | DRY-01 |
| SAFE-T03 | SHT invalid PTC | 拒绝 | DRY-02 |
| SAFE-T04 | temp>=55°C | PTC OFF | DRY-02 |
| SAFE-T05 | GPA0 ON后请求GPA1 | 拒绝，GPA0不变 | WATER-04 |
| SAFE-T06 | 进水时请求排水 | 拒绝 | WATER-04 |
| SAFE-T07 | 姿态电机运行请求BL50 | 拒绝 | POS/SAFE |
| SAFE-T08 | 锁存故障后请求输出 | 拒绝 FAULT_ACTIVE | SAFE-03 |
| SAFE-T09 | 传感器恢复未clear | 故障仍锁存 | ARCH-04 |
| SAFE-T10 | 条件未恢复clear | 拒绝清除 | ARCH-04 |
| SAFE-T11 | emergency stop | 输出关闭顺序正确 | SAFE-02 |
| SAFE-T12 | hook失败 | safety直控输出仍OFF | SAFE-03 |
| SAFE-T13 | emergency重复 | 幂等、无死锁 | SAFE-02 |
| SAFE-T14 | MCP写失败 | FAULT_MCP_IO、不可继续 | SAFE-03 |

## 7. Water Service 测试

| ID | 场景 | 预期 | 需求 |
|---|---|---|---|
| WATER-T01 | 启动时桶已满 | 不开阀，成功/无需进水 | WATER-06 |
| WATER-T02 | 一批后桶满 | GPA0->OFF->GPA1->OFF，DONE | WATER-03 |
| WATER-T03 | 三批后桶满 | 正好循环三次 | WATER-03 |
| WATER-T04 | 无 flow pulse | 3-5s内关阀并 NO_FLOW | WATER-05/07 |
| WATER-T05 | 低流持续 | 按策略告警/故障，阀关 | WATER-05 |
| WATER-T06 | max cycles 未满 | WATER_TIMEOUT，双阀OFF | WATER-06/07 |
| WATER-T07 | total timeout | WATER_TIMEOUT，双阀OFF | WATER-06/07 |
| WATER-T08 | SOURCE_FILL cancel | GPA0/GPA1 OFF | WATER-07 |
| WATER-T09 | TRANSFER cancel | GPA0/GPA1 OFF | WATER-07 |
| WATER-T10 | CHECK_LEVEL cancel | 不启动下一批 | WATER-07 |
| WATER-T11 | 姿态丢失 | 关闭阀、故障 | WATER-02 |
| WATER-T12 | MCP write fail | FAULT_MCP_IO | WATER-07 |
| WATER-T13 | pulses/L=0 | 不使用 estimated ml完成 | WATER-05 |
| WATER-T14 | 全部输出历史 | GPA0/GPA1/GPA3无重叠 | WATER-04 |

### 带水测试步骤

1. PTC 物理断电，BL50 禁用，准备人工总阀和漏水接水盘。
2. 单独验证 GPA0 对应正确阀，1s 点动后关闭。
3. 单独验证 GPA1，1s 点动后关闭。
4. 验证 GPIO18 只有 GPA0 有水流时产生合理脉冲。
5. 标定中间水箱安全单批时间/脉冲，至少留溢流裕量。
6. 标定 GPA1 转移时间。
7. 空桶运行多批循环，确认洗涤桶水位满即停止。
8. 断开流量计、水位信号、I2C，逐项验证故障停阀。

## 8. Dry Service 测试

| ID | 场景 | 预期 | 需求 |
|---|---|---|---|
| DRY-T01 | 270°正常启动 | 先风扇5s，再申请PTC | DRY-01 |
| DRY-T02 | 非270° | 拒绝，无风扇/PTC | DRY-01 |
| DRY-T03 | SHT invalid | PTC不启动，可通风 | DRY-02 |
| DRY-T04 | 升至55°C | 立即PTC OFF | DRY-02 |
| DRY-T05 | 50°C | 不重新加热 | DRY-02 |
| DRY-T06 | 降至<=45°C | 满足周期后可重新加热 | DRY-02 |
| DRY-T07 | 连续加热60s | PTC OFF | DRY-02 |
| DRY-T08 | cancel during heat | PTC先OFF，风扇冷却60s | DRY-02 |
| DRY-T09 | SHT stale during heat | PTC OFF，fault/degraded | DRY-02 |
| DRY-T10 | duration>30min | planner拒绝/裁剪 | DRY-03 |
| DRY-T11 | Demo heat 15s | 仍有5s预吹与冷却 | DRY-03 |
| DRY-T12 | MCP fail关PTC | 锁存严重故障和人工断电提示 | SAFE-03 |

### 真实 PTC 测试前置清单

- 保险丝/热熔断器安装正确。
- 保护接地、漏电保护器、急停/断电手段可用。
- 风道畅通，风扇方向正确。
- SHT 安装位置已确认且读数与参考温度计比对。
- PTC 继电器触点、电流、隔离和线径符合要求。
- 现场有人看护，不运行语音/自动计划，只用专项测试固件。

真实 PTC 首次测试：5s、10s、20s逐级，记录 SHT 与参考温度计曲线；任何风扇停止、异味、异常升温、继电器粘连立即物理断电。

## 9. Executor 与交互测试

| ID | 场景 | 预期 | 需求 |
|---|---|---|---|
| EXE-T01 | Formal happy path | 完整流程回45°idle | FLOW-01/02 |
| EXE-T02 | Demo happy path | 自动预算<=180s | FLOW-03 |
| EXE-T03 | load confirm | 未确认不进入进水 | FLOW-01/05 |
| EXE-T04 | unload confirm | 未确认不进入UV | FLOW-01/05 |
| EXE-T05 | UV BTN3 | 跳过并回45° | AUX-02 |
| EXE-T06 | BTN3 during water | 安全停阀、45°+home | SAFE-02 |
| EXE-T07 | BTN3 during heat | PTC OFF、冷却、再复位 | SAFE-02 |
| EXE-T08 | hall conflict | fault，不自动转45° | SAFE-03 |
| EXE-T09 | late event | 不推进当前步骤 | ARCH-03 |
| EXE-T10 | reset during voice | 控制优先，voice取消 | VOICE-03 |
| EXE-T11 | reboot during run | 不自动恢复危险动作 | SAFE-03 |
| EXE-T12 | 100轮fake Demo | 无死锁、无heap持续下降 | NFR-01/02 |

### 辅助动作与按钮

| ID | 场景 | 预期 | 需求 |
|---|---|---|---|
| AUX-T01 | 0° Demo 洗衣液2-3s | GPB7按时关闭，事件成功 | AUX-01 |
| AUX-T02 | 非0°或超单次上限投液 | 拒绝，GPB7保持OFF | AUX-01 |
| AUX-T03 | 洗涤液泵运行时BTN3 | GPB7立即OFF，终态唯一 | AUX-01/FLOW-05 |
| AUX-T04 | BL50 home命中GPB5 | PWM0/安全停车，HOME_DONE | AUX-03 |
| AUX-T05 | BL50 home超时 | PWM0/STOP，故障上报 | AUX-03 |
| AUX-T06 | 非匹配姿态启动BL50动作 | safety拒绝 | AUX-03 |
| BTN-T01 | IDLE BTN1/BTN2 | 分别启动Formal/Demo | FLOW-05 |
| BTN-T02 | WAITING_LOAD BTN1/BTN2 | 按当前V1映射确认投衣 | FLOW-05 |
| BTN-T03 | WAITING_UNLOAD BTN1或BTN2 | 确认取衣并进入可选UV | FLOW-05 |
| BTN-T04 | 任意运行态BTN3 | abort_reset；UV态则skip_uv | FLOW-05 |

### 状态快照

| ID | 场景 | 预期 | 需求 |
|---|---|---|---|
| STATUS-T01 | 多任务更新/读取10000次 | 每次快照字段一致，无撕裂 | ARCH-05 |
| STATUS-T02 | 状态变化 | revision严格递增 | ARCH-05 |
| STATUS-T03 | BLE/显示并发读取 | 只读store，不触达service内部状态 | ARCH-05 |

## 10. BLE 协议测试

由 `tools/ble_protocol_test.py` 自动执行尽可能多的用例。

| ID | 场景 | 预期 | 需求 |
|---|---|---|---|
| BLE-T01 | hello v1 | ACK+cap+full status | BLE-02 |
| BLE-T02 | version unsupported | BAD_VERSION | BLE-03 |
| BLE-T03 | 20B/21B fragmented | 正确重组 | BLE-03 |
| BLE-T04 | 2048B frame | 边界行为正确 | BLE-03 |
| BLE-T05 | >2048B | 拒绝并清缓冲 | BLE-03 |
| BLE-T06 | chunk gap>3s | 丢弃半帧 | BLE-03 |
| BLE-T07 | string contains braces | 不误判结束 | BLE-03 |
| BLE-T08 | malformed JSON | BAD_JSON，无泄漏 | BLE-03 |
| BLE-T09 | duplicate seq start | 只启动一次，返回缓存ACK | BLE-03 |
| BLE-T10 | fault vs status拥塞 | fault先发 | BLE-05 |
| BLE-T11 | disconnect during run | 继续本地运行 | BLE-05 |
| BLE-T12 | reconnect hello | 当前完整状态 | BLE-05 |
| BLE-T13 | legacy start | 映射 Formal | BLE-05 |
| BLE-T14 | legacy stop | 映射 abort_reset | BLE-05 |
| BLE-T15 | GATT发现 | 名称、FFF0/FFF1/FFF2及属性正确 | BLE-01 |
| BLE-T16 | 全部V1命令 | 逐个路由到正确app command并ACK/NACK | BLE-04 |
| BLE-T17 | JSON裸 valve_on/PTC/GPIO | UNKNOWN_CMD/PLAN_REJECTED，无硬件调用 | WATER-01/FLOW-04 |

## 11. Voice/Agent 测试

| ID | 输入/故障 | 预期 | 需求 |
|---|---|---|---|
| VOICE-T01 | 正式洗衣请求 | 合法 intent，经 planner | VOICE-01/02 |
| VOICE-T02 | Demo请求 | Demo profile | FLOW-03 |
| VOICE-T03 | 羊毛 | UV可关闭、温和 | FLOW-04 |
| VOICE-T04 | heater_on/GPIO | parser拒绝 | VOICE-02 |
| VOICE-T05 | 角度46° | INVALID_POSITION | VOICE-02 |
| VOICE-T06 | 缺进水排水 | planner补齐 | FLOW-04 |
| VOICE-T07 | 90min计划 | 裁剪/拒绝 | FLOW-04 |
| VOICE-T08 | HTTP timeout | error，不改变硬件 | VOICE-02 |
| VOICE-T09 | TLS失败 | error，不泄漏key | SAFE-05 |
| VOICE-T10 | JSON截断/超大 | 有界拒绝，无崩溃 | NFR-01 |
| VOICE-T11 | upload时BTN3 | 立即取消/后台退出 | VOICE-03 |
| VOICE-T12 | 100次交互 | heap和任务数稳定 | NFR-02 |

## 12. 压力与故障注入

### 12.1 自动压力

- 1000次 planner 随机合法/非法 intent，开启 ASan 的 host 测试（若工具链支持）。
- 100次 fake Demo 全流程。
- 24h fake 状态机、BLE status、语音 mock 并发。
- BLE 连续连接/断开 500 次。
- I2C 每个操作点注入失败。
- queue full、事件迟到、request_id wrap 邻域。
- heap/PSRAM 分配失败注入。

### 12.2 看门狗与栈

- 记录每任务 high-water mark。
- 人为延迟网络 60s，控制任务仍正常。
- 人为日志洪泛，position/safety 周期不失约。
- 检查 task watchdog、interrupt watchdog、brownout reset 记录。

## 13. 最终验收矩阵

最终发布前必须全部满足：

- T0/T1/T2 自动测试全绿。
- T3/T4 每个 GPIO、霍尔、PWM、MCP 输出映射有实测记录。
- T5 进水/转移/满水/排水和所有异常停阀通过。
- T6 PTC 温升与故障关断通过，硬件安全条件齐全。
- Formal 连续至少 5 个完整周期。
- Demo 连续至少 20 个完整周期，单次自动动作 <=3min。
- BTN3 在每个阶段均实测。
- BLE 小程序握手、断线重连和故障显示通过。
- 语音合法/非法/断网/超时场景通过。
- 无未解释 watchdog、coredump、heap下降或栈余量不足。

任何未执行的硬件测试必须明确写“未测试”，不能以 fake 测试替代真实测试结论。
