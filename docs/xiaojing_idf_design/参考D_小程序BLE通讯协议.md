# IDF 重构小程序 BLE 通讯协议设计

> 目标：小程序端沿用原 Arduino 固件的 BLE 连接形态，同时升级为 IDF 统一 `app_protocol -> wash_planner -> safety_manager -> wash_executor` 通讯协议。小程序可以下发姿态、进水、排水、烘干等动作请求，但不能直接控制 GPIO/MCP/PWM/继电器等裸硬件。

## 1. 兼容旧连接方式

保留旧工程的 BLE 发现与特征值定义，方便小程序端少改连接层。

| 项 | 定义 |
|---|---|
| Device Name | `JJTP-XIAOJING` |
| Service UUID | `0000FFF0-0000-1000-8000-00805F9B34FB` |
| Write Char | `0000FFF1-0000-1000-8000-00805F9B34FB` |
| Notify Char | `0000FFF2-0000-1000-8000-00805F9B34FB` |
| 写入方向 | 小程序 -> 设备 |
| 通知方向 | 设备 -> 小程序 |

旧协议行为参考：

- 小程序扫描 `JJTP-XIAOJING`。
- 连接后发现 `FFF0` 服务。
- 向 `FFF1` 写入 JSON，例如旧版 `{"cmd":"start"}` / `{"cmd":"stop"}`。
- 订阅 `FFF2` Notify。
- 设备原来会发送紧凑通知：`P,<progress>`、`S,<code>`、`A,<phase>,<seq>`。

IDF V1 建议保留这些紧凑通知作为兼容层，但主协议改用 JSON Notify。

当前 `miniapp4.0` 对照：

- `miniprogram/utils/ble.js` 已经使用 `FFF0/FFF1/FFF2`，连接层基本可保留。
- `miniprogram/utils/ble.js` 已支持 20 字节 BLE 分包发送，设备端继续支持 JSON 分包拼接即可。
- `miniprogram/pages/wash/wash.js` 已解析 `A/P/S` 紧凑通知，可继续保留作为兼容格式。
- 新版主要改造点是：连接后发送 `hello`、新增 `ack/cap/status/fault` JSON 解析、将旧 `start/stop` 升级为 `start_formal/start_demo/submit_plan/abort_reset`。

小程序端详细改造路径见：

- [IDF重构_小程序端改造指导_2026-07-21.md](./IDF重构_小程序端改造指导_2026-07-21.md)

## 2. 握手流程

推荐小程序连接流程：

```text
1. 扫描设备名 JJTP-XIAOJING 或 Service UUID FFF0
2. 连接 BLE
3. Discover service FFF0
4. Discover write FFF1 / notify FFF2
5. Enable notify on FFF2
6. 小程序写入 hello JSON 到 FFF1
7. 设备回复 ack + hello/status snapshot
8. 小程序进入在线控制状态
```

小程序 hello：

```json
{
  "v": 1,
  "seq": 1,
  "type": "cmd",
  "cmd": "hello",
  "payload": {
    "app": "xiaojing-miniapp",
    "proto": 1
  }
}
```

设备 ack：

```json
{
  "v": 1,
  "seq": 1,
  "type": "ack",
  "ok": true,
  "cmd": "hello"
}
```

设备连接快照：

```json
{
  "v": 1,
  "type": "status",
  "state": "idle",
  "phase": "idle",
  "position_deg": 45,
  "progress": 0,
  "fault": null,
  "cap": {
    "proto": 1,
    "positions": [0, 45, 90, 180, 270],
    "max_total_min": 60,
    "max_dry_min": 30
  }
}
```

兼容旧小程序：如果连接后没有发送 `hello`，设备仍可按旧方式接受 `{"cmd":"start"}` / `{"cmd":"stop"}`，并发送 `S,<code>`、`P,<progress>`。

## 3. 分包规则

沿用旧工程思路：小程序可把一个 JSON 对象拆成多次 BLE Write，设备侧拼接到完整 JSON 后解析。

建议 IDF V1 规则：

- 单条 JSON 最大长度建议 `2048` 字节，必要时可扩到 `4096`。
- 分片间隔超过 `3000ms`，设备丢弃当前缓冲。
- 设备按完整 JSON 对象解析，不要求单包写完。
- 小程序写入建议使用 `WRITE_NR`，但关键命令可用 `WRITE`。
- 小程序每条命令必须带 `seq`，用于 ack 对应。

注意：旧代码用“大括号深度”判断完整 JSON。IDF 中建议继续兼容，但解析时要避免字符串里的 `{}` 干扰；更稳妥的实现是支持长度前缀或换行结束符，V1 可先保持 JSON 对象拼接。

## 4. 通用帧格式

小程序下发：

```json
{
  "v": 1,
  "seq": 1001,
  "type": "cmd",
  "cmd": "submit_plan",
  "payload": {}
}
```

设备 ACK：

```json
{
  "v": 1,
  "seq": 1001,
  "type": "ack",
  "ok": true,
  "cmd": "submit_plan"
}
```

设备 NACK：

```json
{
  "v": 1,
  "seq": 1001,
  "type": "ack",
  "ok": false,
  "cmd": "submit_plan",
  "error": {
    "code": "PLAN_REJECTED",
    "message": "dry duration exceeds 30min"
  }
}
```

## 5. 命令列表

| cmd | 作用 |
|---|---|
| `hello` | 握手，获取能力 |
| `get_status` | 获取当前完整状态 |
| `start_formal` | 启动正式洗涤程序 |
| `start_demo` | 启动 Demo 程序 |
| `submit_plan` | 下发自定义洗涤动作计划 |
| `ack_load` | 投衣完成确认 |
| `ack_unload` | 取衣完成确认 |
| `skip_uv` | UV 阶段跳过 UV |
| `abort_reset` | 中断当前流程，关闭水阀/高风险输出，外筒回 45° + BL50 寻零 |
| `pause` | 暂停；第一版可等价为安全停止或流程暂停 |
| `resume` | 恢复；第一版可暂不实现 |

## 6. submit_plan 协议

小程序的自定义计划和语音 agent 使用同一套动作 schema。

允许小程序请求：

- `move_position`
- `water_in`
- `drain`
- `pulsator_wash`
- `drum_wash`
- `spin`
- `dry`
- `uv`

不允许小程序直接请求：

- `heater_on`
- `fan_on`
- `valve_on`
- `pump_on`
- `home_bldc`
- GPIO/MCP/PWM/继电器裸控制

示例：

```json
{
  "v": 1,
  "seq": 2001,
  "type": "cmd",
  "cmd": "submit_plan",
  "payload": {
    "program_type": "custom",
    "target_total_min": 30,
    "allow_uv": true,
    "allow_dry": true,
    "steps": [
      {"action": "move_position", "position_deg": 0},
      {"action": "water_in", "duration_sec": 180, "intensity": "normal"},
      {"action": "pulsator_wash", "duration_sec": 600, "intensity": "normal"},
      {"action": "move_position", "position_deg": 90},
      {"action": "drum_wash", "duration_sec": 420, "intensity": "normal"},
      {"action": "move_position", "position_deg": 180},
      {"action": "drain", "duration_sec": 150, "intensity": "normal"},
      {"action": "spin", "duration_sec": 240, "intensity": "normal"},
      {"action": "move_position", "position_deg": 270},
      {"action": "dry", "duration_sec": 300, "intensity": "normal"}
    ],
    "detergent": {
      "enabled": true,
      "dose_level": "normal"
    }
  }
}
```

本地 planner 必须校验：

- 姿态只允许 `0/45/90/180/270`。
- 进水执行前必须在 `0°`，缺失则补齐。
- 排水执行前必须在 `180°`，缺失则补齐。
- 脱水执行前必须在 `180°`。
- 烘干执行前必须在 `270°`。
- 总时长不超过 `1h`。
- 烘干不超过 `30min`。
- 进水/排水不能跳过。

## 7. 快捷命令

正式程序：

```json
{
  "v": 1,
  "seq": 3001,
  "type": "cmd",
  "cmd": "start_formal",
  "payload": {}
}
```

Demo 程序：

```json
{
  "v": 1,
  "seq": 3002,
  "type": "cmd",
  "cmd": "start_demo",
  "payload": {}
}
```

中断复位：

```json
{
  "v": 1,
  "seq": 3003,
  "type": "cmd",
  "cmd": "abort_reset",
  "payload": {}
}
```

取衣确认：

```json
{
  "v": 1,
  "seq": 3004,
  "type": "cmd",
  "cmd": "ack_unload",
  "payload": {}
}
```

## 8. 状态上报

设备应在状态变化时立即 Notify，并在运行中周期性 Notify。建议运行中 `1s` 一次，空闲可低频或只在变化时发送。

```json
{
  "v": 1,
  "type": "status",
  "state": "running",
  "phase": "spin",
  "position_deg": 180,
  "progress": 62,
  "elapsed_sec": 840,
  "remaining_sec": 780,
  "fault": null
}
```

`state` 建议枚举：

| state | 含义 |
|---|---|
| `idle` | 待机 |
| `waiting_load` | 等待投衣确认 |
| `running` | 洗涤运行中 |
| `waiting_unload` | 等待取衣确认 |
| `uv` | 取衣后 UV 杀菌 |
| `paused` | 暂停 |
| `resetting` | 中断复位中 |
| `fault` | 故障 |

`phase` 建议枚举：

| phase | 含义 |
|---|---|
| `idle` | 空闲 |
| `move_position` | 姿态切换 |
| `home_bldc` | BL50 内筒寻零 |
| `water_in` | 进水 |
| `detergent` | 洗衣液投放 |
| `pulsator_wash` | 波轮洗 |
| `drum_wash` | 滚筒洗 |
| `drain` | 排水 |
| `spin` | 脱水 |
| `dry` | 烘干 |
| `uv` | 紫外 |
| `done` | 完成 |

## 9. 传感器/执行状态字段

完整状态可包含：

```json
{
  "position_deg": 45,
  "target_position_deg": null,
  "water_full": false,
  "flow_pulses": 1234,
  "turbidity": 520,
  "temp_c": 42.5,
  "humidity_rh": 55.2,
  "fan_percent": 60,
  "heater_on": false,
  "uv_on": false,
  "pump_on": false,
  "ble_connected": true,
  "wifi_connected": true,
  "voice_phase": "idle"
}
```

小程序 UI 可按需使用，不要求每次 Notify 都带全量字段。`get_status` 应返回完整字段，周期上报可返回精简字段。

## 10. 错误码

| code | 含义 |
|---|---|
| `BAD_JSON` | JSON 解析失败 |
| `BAD_VERSION` | 协议版本不支持 |
| `UNKNOWN_CMD` | 未知命令 |
| `BUSY` | 当前状态不允许该命令 |
| `PLAN_REJECTED` | 计划被拒绝 |
| `PLAN_CLIPPED` | 计划被裁剪后接受 |
| `INVALID_POSITION` | 非法姿态 |
| `DURATION_LIMIT` | 总时长或烘干时长超限 |
| `WATER_REQUIRED` | 计划缺失进水，已补齐或拒绝 |
| `DRAIN_REQUIRED` | 计划缺失排水，已补齐或拒绝 |
| `WATER_NO_FLOW` | 开阀后流量计无脉冲 |
| `WATER_TIMEOUT` | 进水超时 |
| `POSITION_TIMEOUT` | 姿态切换超时 |
| `HALL_CONFLICT` | 多个姿态霍尔同时触发 |
| `SHT_ERROR` | 温湿度传感器异常 |
| `HEATER_LOCKED` | PTC 联锁不满足 |
| `FAULT_ACTIVE` | 设备处于故障态 |

## 11. 旧协议兼容映射

为了参考并兼容原小程序握手/控制习惯，IDF V1 可以保留旧命令映射：

| 旧命令 | 新语义 |
|---|---|
| `{"cmd":"start"}` | 映射为 `start_formal` 或按小程序设置映射为 `start_demo` |
| `{"cmd":"stop"}` | 映射为 `abort_reset` |

旧 Notify 兼容：

| 旧通知 | 保留建议 |
|---|---|
| `P,<progress>` | 可继续发送，给旧小程序显示粗略进度 |
| `S,<code>` | 可继续发送，给旧小程序显示状态 |
| `A,<phase>,<seq>` | 可继续发送，给旧小程序显示语音/云端阶段 |

新小程序应优先解析 JSON Notify；如果收到 `P,` / `S,` / `A,`，按兼容模式解析。

## 12. IDF 实现建议

模块建议：

- `ble_transport`: 只负责 BLE 连接、分包、收包、notify。
- `app_protocol`: 解析 JSON 帧、seq、ack/nack、命令路由。
- `program_adapter`: 把小程序/语音计划转换为统一 `wash_intent_t`。
- `wash_planner`: 编译姿态、水路、洗涤、烘干、UV 步骤。
- `safety_manager`: 校验和运行时授权。

关键点：

- BLE 回调里不要直接执行业务，只把完整 JSON 投递到队列。
- Notify 不要在多个任务里直接抢发，建议统一 `app_protocol_notify()`。
- 云端语音请求期间如果仍要暂停 BLE，需要给小程序发 `voice_phase` / `ble_suspended` 状态，恢复后发完整 status。
- 所有入口共享同一个 planner，不允许小程序绕过语音/按钮的安全规则。
