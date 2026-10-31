# IDF 重构：小程序端改造指导

日期：2026-07-21  
范围：`miniapp4.0/miniprogram`  
目标：让小程序适配新版 ESP32-S3 IDF 固件、小净五姿态外设、新洗涤流程与 BLE 握手协议。

---

## 1. 当前小程序 BLE 结构

当前小程序 BLE 相关代码主要在：

| 文件 | 当前职责 | 改造重点 |
|---|---|---|
| `miniprogram/utils/ble.js` | 扫描、连接、服务发现、写包、通知监听、旧 start/stop 指令构造 | 保留连接层，升级指令构造和通知解析 |
| `miniprogram/pages/wash/wash.js` | 洗涤页业务状态、连接、发送启动/停止、解析 `P/S/A` 通知 | 增加新版 JSON status/ack/cap 支持 |
| `miniprogram/pages/wash/wash.wxml` | 洗涤页 UI | 增加姿态、阶段、故障、烘干/UV/水路状态显示 |
| `miniprogram/app.js` | 全局设备列表和 BLE 全局状态 | 设备名改为/增加 `JJTP-XIAOJING` |
| `miniprogram/docs/COMMAND_DICTIONARY.md` | 旧协议说明 | 更新为新版协议，保留旧协议兼容说明 |

现有 BLE UUID 已经与建议兼容：

```js
const SERVICE_UUID = '0000FFF0-0000-1000-8000-00805F9B34FB';
const WRITE_CHAR_UUID = '0000FFF1-0000-1000-8000-00805F9B34FB';
const NOTIFY_CHAR_UUID = '0000FFF2-0000-1000-8000-00805F9B34FB';
```

因此小程序端不需要重写 BLE 连接层，主要是协议层升级。

---

## 2. 设备扫描与连接

### 2.1 设备名

ESP32 端建议广播：

```text
JJTP-XIAOJING
```

当前 `utils/ble.js` 使用：

```js
const DEVICE_NAME_PREFIX = 'JJTP';
```

这个可以保留，因为 `JJTP-XIAOJING` 仍可被扫描到。

但 `app.js` 的预设设备建议增加/替换为：

```js
{
  id: 'xiaojing_001',
  name: '小净原型机',
  location: '实验室/展台',
  status: '空闲',
  ip: '',
  bleName: 'JJTP-XIAOJING'
}
```

### 2.2 连接流程

保留当前流程：

1. `wx.openBluetoothAdapter`
2. `scanDevices`
3. `createBLEConnection`
4. `getBLEDeviceServices`
5. `getBLEDeviceCharacteristics`
6. `notifyBLECharacteristicValueChange`

新增一步：Notify 启用成功后，小程序立即发送 `hello`。

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

设备返回：

```json
{
  "v": 1,
  "seq": 1,
  "type": "ack",
  "cmd": "hello",
  "ok": true
}
```

随后推送一次能力和状态快照：

```json
{
  "v": 1,
  "type": "cap",
  "payload": {
    "positions": [0, 45, 90, 180, 270],
    "max_total_sec": 3600,
    "max_dry_sec": 1800,
    "water_required": true,
    "drain_required": true,
    "uv_optional": true
  }
}
```

```json
{
  "v": 1,
  "type": "status",
  "payload": {
    "state": "idle",
    "phase": "idle",
    "position_deg": 45,
    "progress": 0
  }
}
```

---

## 3. `utils/ble.js` 建议改造

### 3.1 增加 seq

建议在 `utils/ble.js` 增加全局递增序号：

```js
let seq = 1;

function nextSeq() {
  return seq++;
}
```

### 3.2 新增统一命令构造

保留 `sendCommand()` 分包逻辑。新增：

```js
function buildFrame(cmd, payload = {}) {
  return JSON.stringify({
    v: 1,
    seq: nextSeq(),
    type: 'cmd',
    cmd,
    payload
  });
}

function buildHelloCommand() {
  return buildFrame('hello', {
    app: 'xiaojing-miniapp',
    proto: 1
  });
}

function buildFormalCommand() {
  return buildFrame('start_formal', {});
}

function buildDemoCommand() {
  return buildFrame('start_demo', {});
}

function buildAbortResetCommand() {
  return buildFrame('abort_reset', {});
}

function buildAckLoadCommand() {
  return buildFrame('ack_load', {});
}

function buildAckUnloadCommand() {
  return buildFrame('ack_unload', {});
}

function buildSkipUvCommand() {
  return buildFrame('skip_uv', {});
}
```

### 3.3 升级 `buildWashCommand`

当前旧版：

```js
function buildWashCommand(params) {
  return JSON.stringify({
    cmd: 'start',
    steps: params.steps,
    totalTime: params.totalTime
  });
}
```

新版建议不要再把 `soak/wash/rinse/dry` 直接发给设备，而是转换成 `submit_plan`：

```js
function buildWashCommand(params) {
  return buildFrame('submit_plan', {
    mode: params.mode || 'formal',
    total_time_sec: Math.min(params.totalTimeSec || 1800, 3600),
    steps: params.steps || []
  });
}
```

`steps` 使用新版动作语义：

```json
[
  {"action":"water_in","timeout_sec":180},
  {"action":"detergent","duration_sec":6},
  {"action":"pulsator_wash","duration_sec":480,"intensity":"normal"},
  {"action":"drum_wash","duration_sec":360,"intensity":"normal"},
  {"action":"drain","timeout_sec":180},
  {"action":"spin","duration_sec":240,"intensity":"normal"},
  {"action":"dry","duration_sec":300,"intensity":"low"},
  {"action":"uv","duration_sec":600}
]
```

说明：

- 小程序可以发 `move_position`，但通常不需要；设备端 planner 会自动补齐姿态。
- 小程序不能发 `heater_on`、`fan_on`、`valve_on`、`pump_on`、GPIO/MCP/PWM 等裸硬件命令。
- 进水和排水不能跳过。即使小程序 UI 允许用户自定义流程，提交前也必须确保包含 `water_in` 和 `drain`，设备端还会再校验一次。

### 3.4 保留旧 start/stop 兼容

如果短期不想一次性修改所有页面，可以先保留：

```js
function buildLegacyStartCommand(params) {
  return JSON.stringify({
    cmd: 'start',
    steps: params.steps,
    totalTime: params.totalTime
  });
}

function buildLegacyStopCommand() {
  return JSON.stringify({ cmd: 'stop' });
}
```

但新页面按钮应逐步改用：

| 旧命令 | 新命令 |
|---|---|
| `{"cmd":"start"}` | `start_formal` 或 `submit_plan` |
| `{"cmd":"stop"}` | `abort_reset` |

---

## 4. Notify 解析改造

当前 `pages/wash/wash.js` 已支持：

- `A,<code>,<seq>`：语音状态
- `P,<value>`：进度
- `S,<code>`：设备状态
- 旧 JSON：`progress` / `ai_status`

新版要增加：

```js
const msg = JSON.parse(data);

if (msg.type === 'ack') {
  this.handleDeviceAck(msg);
} else if (msg.type === 'nack') {
  this.handleDeviceNack(msg);
} else if (msg.type === 'cap') {
  this.handleDeviceCapabilities(msg.payload);
} else if (msg.type === 'status') {
  this.handleDeviceStatus(msg.payload);
} else if (msg.type === 'fault') {
  this.handleDeviceFault(msg.payload);
} else if (msg.type === 'ai_status') {
  this.handleAiStatus(msg);
}
```

建议 `handleDeviceStatus(payload)` 映射字段：

| ESP32 字段 | 小程序状态 |
|---|---|
| `state` | `deviceState` |
| `phase` | `currentPhase` |
| `position_deg` | `positionDeg` |
| `target_position_deg` | `targetPositionDeg` |
| `progress` | `progress` |
| `elapsed_sec` | `elapsedSec` |
| `remaining_sec` | `remainingSec` |
| `fault.code` | `faultCode` |
| `fault.message` | `faultMessage` |

推荐 UI 显示文案：

| `state` | 文案 |
|---|---|
| `idle` | 待机 |
| `waiting_load` | 请放入衣物 |
| `running` | 洗涤中 |
| `waiting_unload` | 请取出衣物 |
| `uv` | 紫外杀菌中 |
| `paused` | 已暂停 |
| `resetting` | 复位中 |
| `fault` | 故障 |

| `phase` | 文案 |
|---|---|
| `move_position` | 姿态切换 |
| `home_bldc` | 内筒寻零 |
| `water_in` | 进水 |
| `water_source_fill` | 水箱进水 |
| `water_transfer_to_drum` | 转移进桶 |
| `detergent` | 投放洗衣液 |
| `pulsator_wash` | 波轮洗 |
| `drum_wash` | 滚筒洗 |
| `drain` | 排水 |
| `spin` | 脱水 |
| `dry` | 烘干 |
| `uv` | 紫外杀菌 |
| `done` | 完成 |

---

## 5. 洗涤页 UI 改造建议

现有 `pages/wash/wash.wxml` 偏订单/自定义步骤页面。新版建议增加四个信息区。

### 5.1 设备状态卡

显示：

- 蓝牙连接状态
- 设备名
- 当前状态 `state`
- 当前阶段 `phase`
- 当前姿态 `position_deg`
- 目标姿态 `target_position_deg`
- 进度 `progress`

### 5.2 流程确认按钮

根据设备状态显示不同主按钮：

| 状态 | 小程序按钮 |
|---|---|
| `idle` | 正式洗涤 / Demo |
| `waiting_load` | 已放入衣物，开始 |
| `running` | 中断复位 |
| `waiting_unload` | 已取出衣物 |
| `uv` | 跳过 UV / 中断复位 |
| `fault` | 查看故障 / 复位 |

对应 BLE 命令：

| 按钮 | cmd |
|---|---|
| 正式洗涤 | `start_formal` |
| Demo | `start_demo` |
| 已放入衣物 | `ack_load` |
| 已取出衣物 | `ack_unload` |
| 跳过 UV | `skip_uv` |
| 中断复位 | `abort_reset` |

### 5.3 自定义流程编辑

如果保留洗涤 agent/自定义流程入口，小程序 UI 不应继续使用旧的 `soak/wash/rinse/dry` 四段表达作为最终协议，而应在发送前转换为新版 `steps[]`：

- 进水：固定存在
- 洗衣液：默认存在，时间可调
- 波轮洗：可多轮
- 滚筒洗：可多轮
- 排水：固定存在
- 脱水：可多轮
- 烘干：可选，最大 30min
- UV：可选，可关闭

### 5.4 故障显示

应至少显示：

- `POSITION_TIMEOUT`：姿态切换超时
- `HALL_CONFLICT`：霍尔状态冲突
- `WATER_NO_FLOW`：进水无流量
- `WATER_TIMEOUT`：进水超时
- `SHT_ERROR`：温湿度传感器异常
- `HEATER_LOCKED`：加热被安全锁定
- `FAULT_ACTIVE`：设备处于故障锁定

---

## 6. 小程序和 ESP32 的职责边界

小程序可以做：

- 发起正式洗涤 / Demo。
- 提交洗涤计划。
- 确认已投衣 / 已取衣。
- 跳过 UV。
- 中断复位。
- 显示姿态、阶段、进度、故障。

小程序不能做：

- 直接开关水阀。
- 直接开关 PTC。
- 直接开关风扇。
- 直接控制洗衣液泵。
- 直接写 GPIO/MCP/PWM。
- 绕过进水/排水必需步骤。

这些动作必须交给 ESP32 端 `wash_planner` 和 `safety_manager` 判断。

---

## 7. 推荐迁移顺序

### 第一步：不改 UI，只改连接后 hello

- 在 `ble.connectDevice()` 成功后，页面发送 `hello`。
- 页面能收到 `ack/cap/status` 并打印。

### 第二步：兼容新版 status

- 保留 `P/S/A` 解析。
- 增加 `status/cap/fault` JSON 解析。
- 洗涤页显示 `state/phase/position/progress`。

### 第三步：替换启动/停止命令

- `buildStopCommand()` 改为 `abort_reset`。
- 正式洗涤按钮用 `start_formal`。
- Demo 按钮用 `start_demo`。

### 第四步：改自定义流程

- 旧 `steps.soak/wash/rinse/dry` 转换为新版 `steps[]`。
- 小程序提交前做一次基本校验。
- ESP32 端仍做最终校验。

### 第五步：增加投衣/取衣/UV 阶段交互

- `waiting_load` 状态显示“已放入衣物”。
- `waiting_unload` 状态显示“已取出衣物”。
- `uv` 状态显示“跳过 UV”。

---

## 8. 与语音逻辑的关系

语音和小程序是同级入口：

```text
语音 / 小程序 / 按钮
        ↓
app_protocol
        ↓
wash_planner
        ↓
safety_manager
        ↓
hardware services
```

因此小程序端不需要理解所有底层外设，只需要理解：

- 当前状态是什么。
- 用户现在能点什么。
- 自定义流程有哪些安全边界。
- 出错时应该显示什么。

设备端才负责决定：

- 是否需要先转到 0°/180°/270°。
- 是否允许 PTC 开启。
- 是否允许进水继续。
- 是否允许跳过 UV。
- 是否需要立即进入故障锁定。

