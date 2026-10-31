# 实机标定记录

每次标定必须记录硬件版本、接线版本、固件 hash、人员、日期和环境。不能只写最终数字。

## 1. 设备信息

```text
主控板版本:
接线文档版本: 主控接线1.3
机械版本:
固件 commit:
ESP-IDF: 5.5.4
操作者:
日期:
```

## 2. IBT-2 与姿态

| 项目 | 结果 | 方法/证据 | 状态 |
|---|---|---|---|
| RPWM 实际方向 | 待测 | 低压5-10%短点动 | PENDING |
| LPWM 实际方向 | 待测 | 低压5-10%短点动 | PENDING |
| 默认运动 PWM | 待测 | 五姿态重复定位 | PENDING |
| 到位制动方式 | 待测 | 惯性/过冲测量 | PENDING |
| 霍尔防抖 | 初值30ms | 毛刺/速度实测 | PENDING |
| 0°超时 | 待测 | 最远路径+裕量 | PENDING |
| 45°超时 | 待测 | 最远路径+裕量 | PENDING |
| 90°超时 | 待测 | 最远路径+裕量 | PENDING |
| 180°超时 | 待测 | 最远路径+裕量 | PENDING |
| 270°超时 | 待测 | 最远路径+裕量 | PENDING |

## 3. BL50

| 项目 | 结果 | 方法/证据 | 状态 |
|---|---|---|---|
| DIR 映射 | 待测 | 低速空载 | PENDING |
| BRAKE 有效电平 | 待测 | 示波器+低速空载 | PENDING |
| STOP 有效电平 | 待测 | 示波器+低速空载 | PENDING |
| Home 方向/PWM | 待测 | GPB5 霍尔 | PENDING |
| 各强度 PWM | 待测 | 洗涤负载 | PENDING |
| 换向停顿 | 待测 | 电流/冲击 | PENDING |

## 4. 水路

| 项目 | 结果 | 方法/证据 | 状态 |
|---|---|---|---|
| 流量计 pulses/L | 待测 | 多次定容取平均 | PENDING |
| 无流超时 | 初值4s | 阀开无水测试 | PENDING |
| 低流阈值 | 待测 | 正常/节流对比 | PENDING |
| 中间水箱单批安全脉冲 | 待测 | 留防溢裕量 | PENDING |
| 中间水箱单批最大时间 | 待测 | 最低正常流量 | PENDING |
| GPA1 单批转移时间 | 待测 | 完全转移+裕量 | PENDING |
| 最大批次数 | 待测 | 正常满桶批数+故障裕量 | PENDING |
| 总进水超时 | 初值180-240s | 最低正常流量 | PENDING |

## 5. 洗衣液泵

| 项目 | 结果 | 方法/证据 | 状态 |
|---|---|---|---|
| ml/s | 待测 | 10/20/30s称重或量筒 | PENDING |
| Demo 默认 | 初值2-3s | 展示效果 | PENDING |
| Formal 默认 | 初值6s | 实际剂量 | PENDING |
| 单次最大时间 | 待测 | 泵温升/最大剂量 | PENDING |

## 6. 风扇、SHT 与 PTC

| 项目 | 结果 | 方法/证据 | 状态 |
|---|---|---|---|
| 风扇方向 | 待测 | 风道检查 | PENDING |
| 风扇安全 PWM | 待测 | 启动可靠性 | PENDING |
| SHT 温度偏差 | 待测 | 参考温度计 | PENDING |
| SHT 湿度偏差 | 待测 | 参考仪表 | PENDING |
| SHT 响应滞后 | 待测 | 阶跃升温曲线 | PENDING |
| PTC 5/10/20/60s温升 | 待测 | 看护测试 | PENDING |
| cutoff | 初值55°C | 温升/衣物安全 | PENDING |
| resume | 初值45°C | 回差测试 | PENDING |
| cooldown | 初值60s | 出风温度曲线 | PENDING |

## 7. 任务资源

| Task | 初始栈 | 最低 High Water Mark | 调整后 | 状态 |
|---|---:|---:|---:|---|
| safety_task | 4096 | - | - | PENDING |
| position_task | 4096 | - | - | PENDING |
| wash_executor_task | 6144 | - | - | PENDING |
| water_task | 4096 | - | - | PENDING |
| dry_task | 5120 | - | - | PENDING |
| app_protocol_task | 6144 | - | - | PENDING |
| ble_rx/tx | 4096 | - | - | PENDING |
| voice_agent_task | 待定 | - | - | PENDING |

