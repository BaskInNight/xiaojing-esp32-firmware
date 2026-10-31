# SHT30 Arduino 库分析报告

> 来源：https://github.com/CNflysky/SHT30
> 分析日期：2026-07-15

---

## 一、仓库结构

```
SHT30/
├── src/
│   ├── SHT30.h              # 头文件，类声明
│   └── SHT30.cpp            # 实现，I2C 读写逻辑
├── examples/
│   └── SHT30_temphumi/
│       └── SHT30_temphumi.ino  # 例程
├── library.properties       # Arduino 库描述
└── keywords.txt             # IDE 语法高亮关键词
```

---

## 二、SHT30 传感器简介

SHT30 是 Sensirion 出品的数字温湿度传感器，主要特性：

| 参数 | 值 |
|------|-----|
| 接口 | I2C |
| 默认 I2C 地址 | 0x44（可通过 ADDR 引脚切换到 0x45） |
| 温度测量范围 | -40°C ~ +125°C |
| 温度精度 | ±0.3°C（典型） |
| 湿度测量范围 | 0% ~ 100% RH |
| 湿度精度 | ±2% RH（典型） |
| 分辨率 | 16 bit |

---

## 三、数据读取流程

### 3.1 总体流程

```
[初始化] → [发送测量命令] → [等待测量完成] → [读取 6 字节原始数据] → [CRC 校验] → [换算实际值]
```

### 3.2 详细步骤

#### 第 1 步：初始化（`begin`）

```cpp
void SHT30::begin() {
    Wire.begin();   // 初始化 I2C 总线（使用默认 SDA/SCL 引脚）
}
```

#### 第 2 步：发送测量命令（`requestMeasurement`）

```cpp
uint32_t SHT30::requestMeasurement() {
    Wire.beginTransmission(_addr);   // 开始向传感器发送
    Wire.write(0x2c);                // 命令高字节
    Wire.write(0x06);                // 命令低字节
    Wire.endTransmission();
    return 15;                       // 返回所需等待时间（ms）
}
```

**命令说明**：发送 `0x2C06`，对应 SHT30 数据手册中的 **"中等精度单次测量"（Medium Repeatability, Clock Stretching Disabled）** 模式。

SHT30 支持的测量命令参考：

| 命令 | 模式 | 精度 | 典型测量时间 |
|------|------|------|-------------|
| 0x2400 | 单次高精度 | 最高 | ~15ms |
| 0x240B | 单次中精度 | 中等 | ~6ms |
| 0x2416 | 单次低精度 | 最低 | ~4ms |
| **0x2C06** | **单次中精度（本库使用）** | **中等** | **~15ms** |

#### 第 3 步：读取原始数据（`getResult`）

```cpp
bool SHT30::getResult() {
    Wire.requestFrom(_addr, (uint8_t)6);  // 从传感器读取 6 字节
    if (Wire.available() == 6) {
        for (uint8_t i = 0; i < 6; i++)
            _data[i] = Wire.read();
        return true;
    }
    return false;
}
```

传感器返回的 6 字节数据布局：

```
字节位置：  [0]     [1]     [2]     [3]     [4]     [5]
含义：     温度MSB  温度LSB  温度CRC  湿度MSB  湿度LSB  湿度CRC
```

- **MSB**：最高有效字节（高 8 位）
- **LSB**：最低有效字节（低 8 位）
- **CRC**：该数据对的 CRC-8 校验值

#### 第 4 步：CRC-8 校验

```cpp
uint8_t SHT30::_getCRC(uint8_t* data) {
    uint8_t crc = 0xFF;                              // 初始值
    for (uint8_t dataCounter = 0; dataCounter < 2; dataCounter++) {
        crc ^= (data[dataCounter]);                  // XOR 当前字节
        for (uint8_t bit = 8; bit > 0; --bit) {      // 逐位处理
            if (crc & 0x80)
                crc = (crc << 1) ^ 0x31;             // 多项式 0x31
            else
                crc = (crc << 1);
        }
    }
    return crc;
}
```

- 算法：**CRC-8**，多项式 `0x31`（即 `x⁸ + x⁵ + x⁴ + 1`），初始值 `0xFF`
- 校验对象：温度用 `_data[0..1]` 计算，与 `_data[2]` 比对；湿度用 `_data[3..4]` 计算，与 `_data[5]` 比对
- 校验失败时：温度返回 `-273.15`，湿度返回 `-1`（可据此判断数据无效）

#### 第 5 步：原始值换算

**温度换算（`getTempInC`）**：

```
温度(°C) = ((data[0] × 256 + data[1]) × 175 / 65535) - 45
```

计算过程拆解：
1. `data[0] × 256 + data[1]` — 拼接为 16 位无符号整数（范围 0 ~ 65535）
2. `× 175 / 65535` — 线性映射到 0 ~ 175°C
3. `- 45` — 偏移，最终范围 -45°C ~ +130°C（有效范围 -40°C ~ +125°C）

**湿度换算（`getHumi`）**：

```
湿度(%RH) = (data[3] × 256 + data[4]) × 100 / 65535
```

计算过程拆解：
1. `data[3] × 256 + data[4]` — 拼接为 16 位无符号整数
2. `× 100 / 65535` — 线性映射到 0 ~ 100% RH

---

## 四、例程代码解析

```cpp
#include <SHT30.h>

SHT30 sht30(0x45);          // 创建传感器对象，I2C 地址 0x45

void setup() {
    Serial.begin(9600);
    sht30.begin();           // 初始化 I2C
    sht30.enableCRC(true);   // 开启 CRC 校验（默认已开启）
}

void loop() {
    long interval = sht30.requestMeasurement();  // 发送测量命令，返回等待时间
    delay(interval);                              // 等待测量完成（15ms）

    if (sht30.getResult()) {                      // 读取并校验数据
        Serial.print("Temp:");
        Serial.print(sht30.getTempInC());          // 输出温度
        Serial.println(" C");

        Serial.print("Humi:");
        Serial.print(sht30.getHumi());             // 输出湿度
        Serial.println("%");
    } else {
        Serial.println("Measurement failed!");
    }
    delay(1000);                                  // 每秒测量一次
}
```

**输出示例**：
```
CRC state:1
Temp:25.32 C
Humi:58.47%
Temp:25.33 C
Humi:58.42%
```

---

## 五、I2C 地址说明

SHT30 的 I2C 地址由 ADDR 引脚决定：

| ADDR 引脚接法 | 7位地址 |
|--------------|---------|
| 接 GND（默认）| 0x44 |
| 接 VDD | 0x45 |
| 接 SDA | 0x45 |
| 接 SCL | 0x46 |

> 例程中使用 `0x45`，说明 ADDR 引脚接了 VDD。请根据实际硬件选择正确地址。

---

## 六、类接口一览

| 方法 | 返回值 | 说明 |
|------|--------|------|
| `SHT30()` | — | 构造，默认地址 0x44 |
| `SHT30(uint8_t addr)` | — | 构造，指定地址 |
| `begin()` | void | 初始化 I2C |
| `requestMeasurement()` | uint32_t | 发送测量命令，返回等待时间（ms） |
| `getResult()` | bool | 读取 6 字节数据，成功返回 true |
| `getTempInC()` | double | 返回温度（°C），CRC 失败返回 -273.15 |
| `getHumi()` | double | 返回湿度（%RH），CRC 失败返回 -1 |
| `enableCRC(bool)` | bool | 开关 CRC 校验 |
| `enableCRC()` | bool | 查询 CRC 开关状态 |

---

## 七、注意事项

1. **I2C 上拉电阻**：SHT30 的 SDA/SCL 线需要外接 4.7kΩ 上拉电阻到 VDD（部分模块已内置）
2. **电源**：工作电压 2.4V ~ 5.5V，建议 3.3V 供电
3. **地址冲突**：同一条 I2C 总线上只能有一个 0x44 或 0x45 地址的设备；若需挂多个，使用不同地址的传感器或加 I2C 多路复用器
4. **测量频率**：例程每秒测量一次（`delay(1000)`），SHT30 最高支持 10Hz 测量频率
5. **CRC 校验**：建议保持开启，可有效检测 I2C 传输错误；关闭时任何数据都视为有效

---

## 八、本库与 SHT30 数据手册命令对照

本库使用 `0x2C06` 命令，属于 SHT30 单次测量模式中的中等精度（Medium Repeatability）模式。

该命令的特点：
- 不启用时钟拉伸（Clock Stretching Disabled）：发送命令后传感器不拉住 SCL 线，需要主机自行等待测量完成
- 测量精度中等：在精度和速度之间取得平衡
- 典型测量时间约 15ms（库返回值正好是 15）

若需要更高精度，可将命令改为 `0x2400`（高精度），但测量时间会增加到约 15ms 以上。
