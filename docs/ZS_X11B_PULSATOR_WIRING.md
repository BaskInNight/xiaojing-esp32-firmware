# ZS-X11B 搅动电机驱动接线

## 当前实物接线

ZS-X11B 的 `PWM IN` 未检测到内部 5V 上拉（悬空实测约 0.06V），因此在线束上使用 4.7k 电阻上拉至驱动板自带 5V。SC 不接。

### 主控 5Pin 到 ZS-X11B

| 主控 J_BL50_CTRL | 当前用途 | ZS-X11B |
|---|---|---|
| Pin 1 `BL50_PWM`（GPIO39） | 停用 | 悬空并绝缘 |
| Pin 2 `BL50_DIR_N`（GPIO1，经 2N7002） | 方向开漏 | 正反转 3Pin 的中间脚 |
| Pin 3 `BL50_BRAKE_N`（GPIO2，经 2N7002） | 停用 | 悬空并绝缘 |
| Pin 4 `BL50_STOP_N`（GPIO21，经 2N7002） | 5V 开漏 PWM | `PWM IN` |
| Pin 5 `GND` | 控制参考地 | PWM接口 `GND` |

PWM 节点：

```text
ZS-X11B +5V -- 4.7k --+-- ZS-X11B PWM IN
                       +-- 主控 BL50_STOP_N

ZS-X11B GND ----------- 主控 GND
```

ZS-X11B 的 5V 只通过 4.7k 上拉电阻连接 PWM 节点，不与主控 5V 电源网直接并联。

### 方向接口

- 拔掉原方向跳线帽。
- 中间脚接主控 `BL50_DIR_N`。
- `CCW`、`CW` 两个外侧脚均悬空。
- 实际方向通过 `machine_config.position.bl50_reverse_dir` 标定。

### 电机、霍尔和电源

| ZS-X11B | 接线 |
|---|---|
| `MA/MB/MC` | 对应电机 U/V/W，相线与霍尔相序必须匹配 |
| `Ha/Hb/Hc` | 对应电机三路霍尔 |
| 霍尔 `+5V/GND` | 电机霍尔电源，合计电流不得超过驱动板标注的 30mA |
| `VCC/GND` | 5-36V 电机主电源，首次板测使用限流与保险 |
| `SC` | 悬空 |
| `VR` | 不接；外接 PWM 时拔掉板载 VR 电位器 |

### 2026-08-16 实机相序验证

轮毂电机三根原始相线依次记为 `A/B/C`。在霍尔 `Ha/Hb/Hc` 保持当前接法
不变时，实机验证可连续起动的相线排列为 `BAC`：

| ZS-X11B 相端子 | 电机相线 |
|---|---|
| `MA` | `B` |
| `MB` | `A` |
| `MC` | `C` |

验证条件为 5kHz PWM、60% 占空比、5s 单方向台架测试；固件返回
`MOTOR_BENCH_COMPLETE`。此前 `ABC` 不转、`ACB` 仅短暂扭动。

## 固件电气语义

- GPIO21 高：Q_STOP 导通，`PWM IN=0V`，电机停止。
- GPIO21 低：Q_STOP 截止，4.7k 将 `PWM IN` 拉到约 5V。
- LEDC 使用硬件输出反相，使业务层的 N% 占空比在 `PWM IN` 上仍为 N%。
- GPIO1 高：Q_DIR 导通，方向中间脚被拉低；GPIO1 低：方向输入释放。
- ZS-X11B 无 STOP/BRAKE，`stop`、`brake`、`emergency_stop` 均退化为 `PWM=0` 滑行停车。

## 安全边界

- 固件烧录及示波器确认前禁止给电机功率端通电。
- 先确认静止时 `PWM IN` 接近 0V，再验证 5kHz、10%/20%低占空比波形。
- 主控掉电而 ZS-X11B 仍有电时，主控 3.3V 消失，现有 STOP Gate 上拉不能证明 PWM 仍保持低电平；正式系统必须保证共同断电/可靠时序，或增加独立硬件功率切断。
- 没有 SC 反馈时只能确认已发出 PWM=0 命令，不能证明转子已经停稳。
