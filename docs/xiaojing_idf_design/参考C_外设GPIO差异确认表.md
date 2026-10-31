# IDF 重构外设 GPIO 差异确认表

> 对比来源：
>
> - 旧工程：`include/HardwareConfig.h`
> - 新硬件文档：`D:/UserData/ESP32Project/物联网文档/doc/主控接线1.3.html`
> - 本表用于逐项向用户确认差异，确认一项更新一项。

## 1. 已确认项

| 编号 | 差异 | 确认结果 |
|---|---|---|
| C01 | 外筒位置从 0/90/180 扩展为 0/45/90/180/270 | 已确认 |
| C02 | 五个外筒角度为五路独立霍尔 GPIO | 已确认 |
| C03 | 霍尔触发为低电平有效 | 已确认 |
| C04 | 外筒机构允许无限同方向连续转 | 已确认 |
| C05 | 外筒 360°姿态旋转由 IBT-2 驱动的直流/减速电机执行，不再使用步进电机 | 已确认 |
| C06 | 主洗涤电机改用 BL50 控制器，软件控制 PWM、DIR、BRAKE、STOP 四个信号 | 已确认 |
| C07 | 0/45/90/180/270 与 BLDC 寻零六路霍尔统一接 MCP23017 GPB0-GPB5，GPIO13 接 MCP INTB 聚合中断 | 已确认 |
| C08 | MCP23017 控制水龙头进水阀、水箱到桶阀、UV、排水阀、PTC 加热器、洗衣液蠕动泵；旧 MCP 心跳 LED/EC11 按键分配不保留 | 已确认 |
| C09 | PAJ7620 手势模块保留，INT 从 GPIO8 迁移到 MCP GPB6，由 MCP INTB/GPIO13 聚合通知 | 已确认 |
| C10 | EC11 旋钮取消，改为 MCP GPA4/GPA5/GPA6 三个独立按钮；GPIO18 改为流量计 PCNT 输入 | 已确认 |
| C11 | 功放 I2S DIN 从 GPIO19 迁移到 GPIO14，新增 GPIO47 AMP_SD 功放使能/静音控制 | 已确认 |
| C12 | I2C 总线新增 SHT30/SHTC3 温湿度探头，与 MCP23017、PAJ7620 共用 GPIO6/7 | 已确认 |
| C13 | 新增双风扇，由 GPIO38 单路 PWM 同步调速，主要用于烘干/通风 | 已确认 |
| C14 | GPIO18 流量计用于进水计量和异常检测，包括有无水流判断、估算进水量 | 已确认 |
| C15 | 烘干加热使用 220VAC/400W PTC，由 MCP GPA7 控制继电器，必须与风扇、温湿度、超温/故障联锁绑定 | 已确认 |
| C16 | 洗衣液投放使用 5V 蠕动泵，由 MCP GPB7 控制开关，当前未标定，先按运行时间标定投放量 | 已确认 |
| C17 | 当前没有门锁/盖子确认输入，投衣和取衣确认先用按钮控制 | 已确认 |
| C18 | 当前没有软件可读取的独立过温/温控反馈，只有硬件温度保险丝 | 已确认 |
| C19 | 洗涤/烘干完成后默认停在 45° 取衣，取完衣服按钮确认后转到 0° 进行 UV 紫外杀菌 | 已确认 |
| C20 | BTN1 单击启动正式洗涤程序，BTN2 单击启动 Demo 洗涤程序，BTN3 单击中断洗涤桶复位并关闭水阀 | 已确认 |
| C21 | 取衣后 0° UV 紫外杀菌默认持续 10min，BTN3 可跳过 | 已确认 |
| C22 | UV 杀菌完成或跳过后，机器最终回到 45°待机 | 已确认 |
| C23 | BTN3 触发中断复位时，目标为外筒回 45° + BL50 内筒寻零 | 已确认 |
| C24 | 正式程序与 Demo 程序共用流程模板，Demo 总时长目标不超过 3min，正式程序按真实衣物洗涤时长 | 已确认 |
| C25 | Demo 烘干环节允许 PTC 短时加热，但必须满足风扇/温湿度/超时联锁 | 已确认 |
| C26 | 排水姿态固定为 180° | 已确认 |
| C27 | 正式洗涤程序默认总时长先设置为 30min，后续可修改，洗涤 agent 可调整参数 | 已确认 |
| C28 | 洗涤 agent 最大总时长 1h、最大烘干 30min；进水/排水不可跳过；波轮/滚筒/脱水轮数次序时间可自定义；UV 可自定义或关闭 | 已确认 |
| C29 | 第一版进水不设最大水量硬上限，依赖水位满、流量异常和超时保护 | 已确认 |
| C30 | BTN1/BTN2/BTN3 第一版只使用单击，后续预留长按、双击和组合键功能 | 已确认 |

## 2. 待逐项确认差异

| 编号 | 旧 Arduino 工程 | 新 HTML 分配 | 初步判断 | 状态 |
|---|---|---|---|---|
| D01 | 外筒换位步进：GPIO41=PUL, GPIO40=DIR, GPIO48=ENA | GPIO41=IBT2_LPWM, GPIO40=IBT2_RPWM, GPIO48=IBT2_EN | 姿态驱动由步进电机改为 IBT-2 直流/减速电机，位置靠霍尔寻位停止 | 已确认 |
| D02 | BLDC 只用 GPIO39 PWM + GPIO42 DIR | BL50 使用 GPIO39 PWM + GPIO1 DIR + GPIO2 BRAKE + GPIO21 STOP | 主洗涤 BLDC 控制器从两线控制升级为 BL50 四线控制 | 已确认 |
| D03 | 外筒霍尔直连 ESP32：GPIO1/2/47，BLDC 霍尔 GPIO21 | 六路霍尔进 MCP：GPB0=0°, GPB1=45°, GPB2=90°, GPB3=180°, GPB4=270°, GPB5=BLDC | 位置输入从 ESP32 GPIO 迁移到 MCP23017 GPB 组，INTB 汇总到 GPIO13 | 已确认 |
| D04 | MCP 只控制进水阀 GPA0、UV GPA2、EC11 按键 GPB0、心跳 LED GPB7 | MCP GPA0=水龙头进水阀，GPA1=水箱到桶阀，GPA2=UV，GPA3=排水阀，GPA7=PTC，GPB7=洗衣液泵 | MCP 从少量 IO 变成执行器/输入聚合核心，旧心跳 LED/EC11 按键分配取消 | 已确认 |
| D05 | PAJ7620 手势 INT 直连 GPIO8 | PAJ7620 INT 进入 MCP GPB6，GPIO8 空闲 | 手势中断改由 MCP 聚合，GPIO8 释放 | 已确认 |
| D06 | EC11 A=GPIO18，B=GPIO13，按键=MCP GPB0 | GPIO18=流量计 PCNT，GPIO13=MCP INTB，按钮改为 MCP GPA4/GPA5/GPA6 | EC11 旋钮取消，改为三个独立按钮；GPIO18 用于流量计 PCNT | 已确认 |
| D07 | 音频：GPIO15 WS, GPIO16 SCK, GPIO17 MIC_SD, GPIO19 AMP_DIN | 音频：GPIO15 WS, GPIO16 BCLK, GPIO17 MIC_SD, GPIO14 AMP_DIN, GPIO47 AMP_SD | 功放数据脚从 GPIO19 迁到 GPIO14，并新增功放使能 GPIO47 | 已确认 |
| D08 | I2C 总线挂 PAJ7620 + MCP23017 | I2C 总线挂 MCP23017 + PAJ7620 + SHT30 | 新增 SHT30/SHTC3 温湿度探头，用于烘干/温湿度反馈 | 已确认 |
| D09 | 无风扇控制 | GPIO38=双风扇 PWM | 新增双风扇同步 PWM，服务于烘干/通风 | 已确认 |
| D10 | 无流量计 | GPIO18=FLOW_PCNT | 新增流量计脉冲输入，用于进水计量和异常检测 | 已确认 |
| D11 | 无 PTC 加热器 | MCP GPA7=HEATER_EN | 新增 220VAC/400W PTC 加热控制，必须纳入高风险安全联锁 | 已确认 |
| D12 | 无洗衣液泵 | MCP GPB7=PUMP_CTRL | 新增 5V 洗衣液蠕动泵，当前未标定，先按运行时间标定投放量 | 已确认 |

## 3. 新 HTML 摘要映射

### ESP32 GPIO

| GPIO | 新功能 |
|---|---|
| GPIO4 | XKC 水位检测 |
| GPIO5 | 浊度 ADC |
| GPIO6 | I2C SDA |
| GPIO7 | I2C SCL |
| GPIO9 | LCD DC |
| GPIO10 | LCD CS |
| GPIO11 | LCD MOSI |
| GPIO12 | LCD SCK |
| GPIO13 | MCP INTB |
| GPIO14 | I2S AMP DIN |
| GPIO15 | I2S WS |
| GPIO16 | I2S BCLK |
| GPIO17 | MIC I2S SD |
| GPIO18 | 流量计 PCNT |
| GPIO21 | BL50 STOP |
| GPIO38 | 双风扇 PWM |
| GPIO39 | BL50 PWM |
| GPIO40 | IBT-2 PWM_A / RPWM |
| GPIO41 | IBT-2 PWM_B / LPWM |
| GPIO47 | AMP_SD |
| GPIO48 | IBT-2 MOTOR_EN |
| GPIO1 | BL50 DIR |
| GPIO2 | BL50 BRAKE |
| GPIO43/44 | UART0 调试/下载 |

### MCP23017

| MCP 端口 | 新功能 |
|---|---|
| GPA0 | 水龙头进水阀 |
| GPA1 | 水箱到洗涤桶阀 |
| GPA2 | UV 杀菌灯 |
| GPA3 | 洗涤桶排水阀 |
| GPA4 | BTN1 |
| GPA5 | BTN2 |
| GPA6 | BTN3 |
| GPA7 | PTC 加热器 |
| GPB0 | 霍尔 0° |
| GPB1 | 霍尔 45° |
| GPB2 | 霍尔 90° |
| GPB3 | 霍尔 180° |
| GPB4 | 霍尔 270° |
| GPB5 | BLDC 寻零霍尔 |
| GPB6 | 手势中断 |
| GPB7 | 洗衣液蠕动泵 |
