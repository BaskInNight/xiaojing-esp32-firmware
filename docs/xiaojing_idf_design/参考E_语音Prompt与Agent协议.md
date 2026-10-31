# IDF 重构语音 Prompt 与洗涤 Agent 协议设计

> 目标：保留现有“小净”语音交互体验，但重写 LLM 到执行器之间的协议。LLM 可以编排姿态、进水、排水、烘干等洗涤动作请求，但不能直接控制 GPIO/PWM/继电器等裸硬件；所有动作必须由本地 `wash_planner` 和 `safety_manager` 编译、校验并执行。

## 1. 总原则

旧 Arduino 版本的语音链路大致是：

```text
唤醒/VAD/云端大模型 -> tool_calls pipeline[{cmd,val,time}] -> WashStep_t -> 执行器
```

新 IDF 版本建议改成：

```text
唤醒/VAD/云端大模型
  -> wash_agent_intent
  -> app_protocol
  -> wash_planner 本地编译
  -> safety_manager 校验/裁剪
  -> wash_executor 执行安全步骤
```

核心变化：

- LLM 可以输出 `move_position`、`water_in`、`drain`、`dry` 等洗涤动作请求。
- LLM 可以调整姿态切换、进水、排水、波轮洗、滚筒洗、脱水、烘干、UV 的次序、轮数和时长。
- LLM 不能跳过进水和排水；如果输出计划缺少它们，本地 planner 必须补齐。
- LLM 不能突破最大总时长 1h、最大烘干时长 30min。
- LLM 可以根据衣物材质关闭 UV，例如羊毛、生物织料等。
- LLM 可以请求姿态切换，但本地 planner 必须校验姿态合法性和动作前置姿态。
- 本地 safety 层负责 PTC、风扇、温湿度、流量、水位、霍尔、BL50、IBT-2 等硬件联锁。

## 2. 保留与重写

可以保留：

- 唤醒词“小净”
- VAD 录音
- 云端 LLM 请求
- TTS 回复
- TTS 播放完再启动执行
- 洗涤中禁用/限制唤醒，避免电机噪声误触发

必须重写：

- system prompt
- tool schema
- tool call 解析
- AI pipeline 到执行队列的注入方式
- 默认 UV 追加逻辑
- 姿态/动作请求到安全步骤的编译逻辑

旧 prompt 中这类规则要废弃：

- 只支持旧的 0/90/180 姿态
- 让 AI 直接输出 `WashStep_t`
- pipeline 末尾补 `TILT 0`
- 默认插入 60s UV

新规则是：AI 可以输出“想怎么洗、希望切到哪个姿态、是否进水/排水/烘干”，本地系统负责把请求编译成安全执行步骤。

## 3. Agent 可见能力

Prompt 中应该告诉 agent 的能力，使用产品语义，不暴露 GPIO。

可编排能力：

| 能力 | 姿态 | Agent 权限 |
|---|---:|---|
| 姿态切换 | 0/45/90/180/270 | 可请求；本地校验合法姿态与运动安全 |
| 进水 | 0° | 可请求，必须存在，不能跳过；本地确保进水前在 0° |
| 洗衣液投放 | 0° | 可启用/禁用，时间/剂量可建议 |
| 波轮洗 | 0° | 可自定义轮数、次序、时间、强度 |
| 滚筒洗 | 90° | 可自定义轮数、次序、时间、强度 |
| 排水 | 180° | 可请求，必须存在，不能跳过；本地确保排水前在 180° |
| 脱水 | 180° | 可自定义轮数、次序、时间、强度；本地确保脱水前在 180° |
| 烘干 | 270° | 可请求/启用/禁用，最大 30min；本地确保烘干前在 270° |
| UV 紫外 | 0° | 可启用/禁用，材质敏感时可关闭 |
| 取衣/待机 | 45° | 系统自动处理 |
| BL50 内筒寻零 | 自动 | 系统自动处理 |

Agent 不可控制：

- 任意 GPIO/MCP 端口
- IBT-2 双 PWM
- BL50 STOP/BRAKE/DIR 裸电平
- PTC 继电器直接开关
- 风扇先后顺序
- 霍尔/姿态寻位
- 水阀直接开关
- 排水强制跳过
- 任何安全上限

## 4. 推荐 Tool Schema

建议 tool 名称：`propose_wash_program`

LLM 输出一个结构化动作计划，而不是裸硬件控制。

```json
{
  "speech_reply": "好嘞，我按羊毛温和模式来，紫外先关掉。",
  "program_type": "formal",
  "fabric_type": "wool",
  "soil_level": "normal",
  "allow_uv": false,
  "allow_dry": true,
  "target_total_min": 30,
  "steps": [
    {
      "action": "move_position",
      "position_deg": 0
    },
    {
      "action": "water_in",
      "duration_sec": 180,
      "intensity": "normal"
    },
    {
      "action": "pulsator_wash",
      "rounds": 1,
      "duration_sec": 420,
      "intensity": "gentle"
    },
    {
      "action": "drum_wash",
      "rounds": 1,
      "duration_sec": 360,
      "intensity": "gentle"
    },
    {
      "action": "spin",
      "rounds": 1,
      "duration_sec": 180,
      "intensity": "low"
    },
    {
      "action": "dry",
      "rounds": 1,
      "duration_sec": 300,
      "intensity": "low"
    }
  ],
  "detergent": {
    "enabled": true,
    "dose_level": "normal"
  },
  "preferences_update": ""
}
```

字段建议：

| 字段 | 类型 | 说明 |
|---|---|---|
| `speech_reply` | string | TTS 回复，建议 30 字内 |
| `program_type` | enum | `formal` / `demo` / `custom` / `chat_only` |
| `fabric_type` | enum/string | cotton、wool、silk、synthetic、unknown 等 |
| `soil_level` | enum | light、normal、heavy |
| `allow_uv` | bool | 是否允许 UV |
| `allow_dry` | bool | 是否允许烘干 |
| `target_total_min` | number | 目标总时长，必须 <= 60 |
| `steps` | array | 洗涤动作请求，可包含姿态、进水、排水、波轮、滚筒、脱水、烘干、UV |
| `detergent` | object | 洗衣液投放建议 |
| `preferences_update` | string | 用户偏好更新摘要 |

允许的 `steps[].action`：

- `pulsator_wash`
- `drum_wash`
- `spin`
- `dry`
- `uv`
- `water_in`
- `drain`
- `move_position`

不允许的 `steps[].action`：

- `home_bldc`
- `heater_on`
- `fan_on`
- `valve_on`
- `pump_on`

原因：`water_in`、`drain`、`move_position` 是洗涤动作请求；`heater_on`、`fan_on`、`valve_on`、`pump_on`、`home_bldc` 属于底层硬件/校准动作，必须由本地系统处理。

## 5. 本地 Planner 编译规则

无论 agent 输出什么，本地 planner 都必须校验并生成安全执行步骤。agent 可以显式请求姿态、进水、排水、烘干；planner 可以接受、补齐、重排或拒绝不安全动作。

正式程序编译模板：

```text
LOAD_POSITION 45°
等待 BTN1/确认投衣
HOME_BL50
agent/planner: MOVE 0°
agent/planner: WATER_IN   // 不可跳过
DETERGENT_IN 可选
agent steps:
  pulsator_wash -> MOVE 0° + PULSATOR
  drum_wash     -> MOVE 90° + DRUM
  spin          -> MOVE 180° + SPIN
  dry           -> MOVE 270° + FAN + PTC联锁 + DRY
agent/planner: MOVE 180°
agent/planner: DRAIN      // 不可跳过
MOVE 45°
等待用户取衣确认
if UV enabled:
  MOVE 0°
  UV 10min 或 agent 指定时间
MOVE 45°
IDLE
```

注意：排水固定 180°。如果 agent steps 中多轮脱水/洗涤穿插，planner 仍要确保最终排水存在。

Demo 程序编译模板：

```text
LOAD_POSITION 45°
等待 BTN2/确认投衣
HOME_BL50
MOVE 0°
WATER_IN 演示
DETERGENT_IN 演示
PULSATOR 演示
MOVE 90°
DRUM 演示
MOVE 180°
DRAIN 演示
SPIN 演示
MOVE 270°
FAN + PTC 短时加热演示
MOVE 45°
等待取衣确认
MOVE 0°
UV 演示
MOVE 45°
IDLE
```

Demo 总时长目标不超过 3min。

## 6. 本地校验与裁剪

Planner 接收 agent 意图后必须做校验：

| 校验项 | 规则 |
|---|---|
| 总时长 | <= 1h |
| 烘干时长 | <= 30min |
| 进水 | 必须存在，不能跳过；执行前必须在 0° |
| 排水 | 必须存在，不能跳过；执行前必须在 180° |
| UV | 可关闭；启用时必须 0° |
| PTC | 必须风扇先行、SHT 有效、姿态 270° |
| 排水姿态 | 固定 180° |
| 波轮姿态 | 固定 0° |
| 滚筒姿态 | 固定 90° |
| 脱水姿态 | 固定 180° |
| 洗衣液姿态 | 固定 0° |
| BL50 | 姿态未到位/故障时禁止启动 |
| 水路 | 水位满/流量异常/超时保护 |

裁剪策略：

- 超过 1h：按比例缩短可变步骤，或拒绝并让 TTS 说明。
- 烘干超过 30min：裁剪到 30min。
- agent 试图跳过进水/排水：忽略该意图，本地强制插入。
- agent 输出未知动作：丢弃该动作。
- agent 输出合法姿态动作：校验后执行；姿态只允许 0/45/90/180/270。
- agent 输出裸硬件动作：丢弃并记录日志。
- agent 输出进水但前序姿态不是 0°：planner 自动插入/调整到 0°，再执行进水。
- agent 输出排水/脱水但前序姿态不是 180°：planner 自动插入/调整到 180°，再执行。
- agent 输出烘干但前序姿态不是 270°：planner 自动插入/调整到 270°，并检查风扇/PTC/SHT 联锁。
- SHT 失效：禁止 PTC 加热，可保留风扇通风。
- 流量计无脉冲：进入进水异常。

## 7. System Prompt 草案

```text
你是“小净”洗涤 agent，负责根据用户语音生成洗涤意图。

重要边界：
1. 你输出洗涤动作计划和程序参数，可以请求姿态切换、进水、排水、烘干等动作。
2. 你不能输出 GPIO、MCP、PWM、霍尔、姿态电机、BL50、阀门、继电器等底层控制。
3. 你不能跳过进水和排水。
4. 总时长不能超过 1 小时，烘干不能超过 30 分钟。
5. 波轮洗、滚筒洗、脱水的轮数、次序、时间可以根据衣物和用户要求调整。
6. UV 紫外可以开启、缩短或关闭；羊毛、真丝、生物织料、用户说不要杀菌时，应关闭 UV。
7. 烘干可以开启或关闭；需要温和处理的材质应减少或关闭烘干。
8. 姿态只能使用 0/45/90/180/270，并且动作要放在正确姿态：
   - 0°：进水、洗衣液、波轮洗、UV
   - 45°：投衣/取衣/待机
   - 90°：滚筒洗
   - 180°：排水、脱水
   - 270°：烘干
9. 如果你忘记切姿态，本地系统会补齐；但你应尽量输出清晰的动作顺序。
10. 回复要简短自然，speech_reply 建议 30 字以内。
11. 如果用户只是闲聊，program_type 输出 chat_only，steps 为空。

你必须调用 propose_wash_program 工具输出结构化 JSON。
```

## 8. Function Tool Schema 草案

```json
{
  "type": "function",
  "function": {
    "name": "propose_wash_program",
    "description": "Generate a washing program intent. The local planner will compile and enforce safety.",
    "parameters": {
      "type": "object",
      "properties": {
        "speech_reply": {
          "type": "string",
          "description": "Short TTS reply in Chinese, ideally under 30 Chinese characters."
        },
        "program_type": {
          "type": "string",
          "enum": ["formal", "demo", "custom", "chat_only"]
        },
        "fabric_type": {
          "type": "string",
          "enum": ["unknown", "cotton", "wool", "silk", "synthetic", "delicate", "bio_fabric", "mixed"]
        },
        "soil_level": {
          "type": "string",
          "enum": ["light", "normal", "heavy"]
        },
        "allow_uv": {
          "type": "boolean"
        },
        "allow_dry": {
          "type": "boolean"
        },
        "target_total_min": {
          "type": "integer",
          "minimum": 0,
          "maximum": 60
        },
        "steps": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "action": {
                "type": "string",
                "enum": ["move_position", "water_in", "drain", "pulsator_wash", "drum_wash", "spin", "dry", "uv"]
              },
              "position_deg": {
                "type": "integer",
                "enum": [0, 45, 90, 180, 270],
                "description": "Required only when action is move_position."
              },
              "rounds": {
                "type": "integer",
                "minimum": 0,
                "maximum": 10
              },
              "duration_sec": {
                "type": "integer",
                "minimum": 0,
                "maximum": 3600
              },
              "intensity": {
                "type": "string",
                "enum": ["low", "gentle", "normal", "strong"]
              }
            },
            "required": ["action"]
          }
        },
        "detergent": {
          "type": "object",
          "properties": {
            "enabled": {
              "type": "boolean"
            },
            "dose_level": {
              "type": "string",
              "enum": ["none", "low", "normal", "high"]
            }
          },
          "required": ["enabled", "dose_level"]
        },
        "preferences_update": {
          "type": "string"
        }
      },
      "required": [
        "speech_reply",
        "program_type",
        "fabric_type",
        "soil_level",
        "allow_uv",
        "allow_dry",
        "target_total_min",
        "steps",
        "detergent",
        "preferences_update"
      ]
    }
  }
}
```

## 9. 示例

### 普通正式洗涤

用户：“小净，正常洗一桶衣服。”

```json
{
  "speech_reply": "好嘞，按标准流程来洗。",
  "program_type": "formal",
  "fabric_type": "unknown",
  "soil_level": "normal",
  "allow_uv": true,
  "allow_dry": true,
  "target_total_min": 30,
  "steps": [
    {"action": "move_position", "position_deg": 0},
    {"action": "water_in", "duration_sec": 180, "intensity": "normal"},
    {"action": "pulsator_wash", "rounds": 1, "duration_sec": 540, "intensity": "normal"},
    {"action": "move_position", "position_deg": 90},
    {"action": "drum_wash", "rounds": 1, "duration_sec": 420, "intensity": "normal"},
    {"action": "move_position", "position_deg": 180},
    {"action": "drain", "duration_sec": 150, "intensity": "normal"},
    {"action": "spin", "rounds": 1, "duration_sec": 240, "intensity": "normal"},
    {"action": "move_position", "position_deg": 270},
    {"action": "dry", "rounds": 1, "duration_sec": 360, "intensity": "normal"},
    {"action": "move_position", "position_deg": 0},
    {"action": "uv", "rounds": 1, "duration_sec": 600, "intensity": "gentle"}
  ],
  "detergent": {"enabled": true, "dose_level": "normal"},
  "preferences_update": ""
}
```

### 羊毛/生物织料

用户：“这批是羊毛，温柔点，不要紫外。”

```json
{
  "speech_reply": "明白，羊毛我轻柔洗，紫外关闭。",
  "program_type": "custom",
  "fabric_type": "wool",
  "soil_level": "normal",
  "allow_uv": false,
  "allow_dry": false,
  "target_total_min": 25,
  "steps": [
    {"action": "move_position", "position_deg": 0},
    {"action": "water_in", "duration_sec": 180, "intensity": "gentle"},
    {"action": "pulsator_wash", "rounds": 1, "duration_sec": 360, "intensity": "gentle"},
    {"action": "move_position", "position_deg": 90},
    {"action": "drum_wash", "rounds": 1, "duration_sec": 300, "intensity": "gentle"},
    {"action": "move_position", "position_deg": 180},
    {"action": "drain", "duration_sec": 150, "intensity": "gentle"},
    {"action": "spin", "rounds": 1, "duration_sec": 120, "intensity": "gentle"}
  ],
  "detergent": {"enabled": true, "dose_level": "low"},
  "preferences_update": ""
}
```

### Demo 程序

用户或 BTN2：“启动 Demo。”

```json
{
  "speech_reply": "开始演示流程。",
  "program_type": "demo",
  "fabric_type": "unknown",
  "soil_level": "light",
  "allow_uv": true,
  "allow_dry": true,
  "target_total_min": 3,
  "steps": [
    {"action": "move_position", "position_deg": 0},
    {"action": "water_in", "duration_sec": 12, "intensity": "low"},
    {"action": "pulsator_wash", "rounds": 1, "duration_sec": 25, "intensity": "normal"},
    {"action": "move_position", "position_deg": 90},
    {"action": "drum_wash", "rounds": 1, "duration_sec": 25, "intensity": "normal"},
    {"action": "move_position", "position_deg": 180},
    {"action": "drain", "duration_sec": 10, "intensity": "low"},
    {"action": "spin", "rounds": 1, "duration_sec": 25, "intensity": "normal"},
    {"action": "move_position", "position_deg": 270},
    {"action": "dry", "rounds": 1, "duration_sec": 25, "intensity": "gentle"},
    {"action": "move_position", "position_deg": 0},
    {"action": "uv", "rounds": 1, "duration_sec": 15, "intensity": "gentle"}
  ],
  "detergent": {"enabled": true, "dose_level": "low"},
  "preferences_update": ""
}
```

## 10. IDF 实现落点

建议新增模块：

- `app_protocol`: 接收 BTN/BLE/语音的统一请求。
- `wash_agent_parser`: 解析 LLM tool call，产出 `wash_intent_t`。
- `wash_planner`: 将 `wash_intent_t` 编译为内部 `wash_step_t`。
- `wash_policy`: 保存最大时长、材质规则、UV/烘干策略。
- `safety_manager`: 运行时授权高风险动作。

建议数据流：

```text
voice_task
  -> llm_client
  -> wash_agent_parser
  -> app_protocol_submit_intent()
  -> wash_planner_compile()
  -> safety_manager_validate()
  -> wash_executor_enqueue()
```

这样语音、BLE、按钮三套入口可以共享同一个 planner，不会出现“语音一套安全规则、按钮一套安全规则”的问题。
