# Phase 9 交接文档

## 一、当前已完成的工作

### 1.1 P0 修复（已生效）

| # | 问题 | 修复 | 文件 |
|---|------|------|------|
| 1 | sdkconfig 模型名不匹配 | `HILEXIN` → `XIAOAITONGXUE` | `sdkconfig` |
| 2 | WiFi STATIC_RX=3 太低 | → 6 | `sdkconfig`, `sdkconfig.defaults` |
| 3 | ESP-SR feed worker 永不调度 | 移除 worker，直接调用 AFE feed/fetch | `voice_wake_esp_sr.c` |
| 4 | ESP-SR 模型任务栈4096→内部堆不足 | 降为3072（已验证够用） | `voice_wake_esp_sr.c:57` |
| 5 | BLE pause 导致系统卡死 | 移除 BLE pause 逻辑 | `voice_service.c`, `voice_agent/CMakeLists.txt` |
| 6 | 不必要的16000→16062重采样 | 采样率差<1%时跳过重采样 | `ei_classifier_wrapper.cpp` |
| 7 | 麦克风通道硬编码 | 改为启动时自动检测 + 每200帧重新检测 | `audio_driver.c` |
| 8 | WiFi 断连诊断不足 | 新增 RSSI/channel 日志 | `wifi_station.c` |

### 1.2 已验证功能

- **Edge Impulse 唤醒**: 成功（score=0.867~0.988）
- **ESP-SR AFE 初始化**: 成功（wn9_xiaoaitongxue, chunk=160）
- **BTN1 长按切换**: 成功（Edge ↔ ESP-SR）
- **NVS 持久化**: 成功（重启恢复）
- **WiFi 连接**: JINGJIE, RSSI=-10, WPA2
- **BLE 广播**: JJTP-XIAOJING
- **PAJ7620 手势**: LEFT/RIGHT/UP/DOWN 正常

### 1.3 编译证据

```
ELF SHA256: 4437c084f1c937a299f0b4bb92a6b9645941f97927a50f6b0f82a51bdb8c7572
BIN SHA256: 88dbed7453b7b43cb975ca73b3e0e8718dc2b7c0183365db0e56c97a4488f7b2
SRM SHA256: 380f07943ccfd3132fd0d43534d2712b403d16f9dadfe356c43abad0ce15f68f
Binary size: 0x3f0a70 (4066KB), 7% free
```

---

## 二、未解决的阻塞问题：I2S TX/RX 共享时钟干扰

### 2.1 问题现象

**硬件约束**: TX (MAX98357) 和 RX (INMP441) 共享同一个 I2S 端口的 WS (GPIO15) 和 BCLK (GPIO16)。

**触发路径**:
1. 开机 → Edge Impulse 唤醒正常 ✓
2. BTN1 长按 → `voice_service_toggle_backend()` → `stop_frontend()` → 播放提示音
3. 提示音播放期间：`audio_driver_start_tx()` 启用 I2S0 TX → TX DMA 开始在 GPIO15/16 上输出时钟
4. 提示音结束：`audio_driver_stop_tx()` → 无论怎么处理 TX，RX 信号都被破坏
5. 切换后：Lpeak=0（LEFT 静音）或 Lpeak=~1073M（饱和），分类器输出 `xj=0.000 u=0.996`

**已尝试但失败的方案**:

| 方案 | 结果 | 失败原因 |
|------|------|----------|
| `i2s_channel_disable(TX)` | RX 永久静音 | TX/RX 共享 WS/BCLK，禁用 TX 杀死 RX 时钟 |
| TX keep-alive + RX 重置 | LEFT/RIGHT 通道翻转 | `i2s_channel_disable/enable(RX)` 改变 DMA 对齐 |
| TX DMA 写静音刷新 | 信号仍被破坏 | TX 数据通过共享总线耦合到 RX |
| 禁用 TX 采样率切换 | 信号仍被破坏 | TX DMA 残留数据在共享总线上干扰 RX |
| TX keep-alive 不做任何处理 | 信号仍被破坏 | TX DMA 状态在提示音期间改变，RX DMA 失同步 |

### 2.2 根因分析

ESP32-S3 的 I2S 硬件中，TX 和 RX 如果在同一个 I2S 端口上，**共享同一个时钟发生器**。当 TX DMA 活动时：
- TX 的 DMA 描述符状态影响共享 WS/BCLK 的相位
- RX 的 DMA 采样点与 TX DMA 不同步
- 提示音播放期间 TX 采样率可能被临时切换（16kHz→24kHz→16kHz），导致时钟域混乱

### 2.3 推荐软件解决方案（按优先级排序）

#### 方案 A：TX 独立 I2S 端口（推荐，需验证 GPIO 可用性）

```c
// board_config.h 新增
#define XIAOJING_GPIO_I2S_TX_WS      8    // TX 独立 WS
#define XIAOJING_GPIO_I2S_TX_BCLK    45   // TX 独立 BCLK

// audio_driver.c: tx_gpio_config() 使用独立引脚
.bclk = XIAOJING_GPIO_I2S_TX_BCLK,
.ws   = XIAOJING_GPIO_I2S_TX_WS,
```

**前提**: GPIO8 和 GPIO45 必须物理连接到 MAX98357 的 LRC 和 BCLK。
**优点**: 根本解决，TX/RX 完全独立。
**风险**: GPIO45 是 VDD_SPI strapping pin，开机时必须为低。

#### 方案 B：TX/RX 使用 I2S TDM 全双工模式

将 TX 和 RX 配置为同一个 I2S 端口的 TDM 全双工模式，TX 使用 slot 0，RX 使用 slot 1。这样它们共享同一个时钟域，不会失同步。

```c
// 使用 i2s_tdm_config_t 替代 i2s_std_config_t
// TX: .slot_mask = I2S_TDM_SLOT0
// RX: .slot_mask = I2S_TDM_SLOT1
```

**优点**: 不需要额外 GPIO。
**风险**: INMP441 输出标准 I2S（不是 TDM），需要确认 slot 映射兼容性。

#### 方案 C：提示音期间暂停 RX，播放后重新初始化 RX

```c
esp_err_t audio_driver_play_prompt(audio_handle_t *handle, ...) {
    // 1. 暂停 RX
    i2s_channel_disable(handle->rx_handle);
    // 2. 启用 TX 并播放
    audio_driver_start_tx(handle);
    audio_driver_write(handle, pcm, size, &written, timeout);
    audio_driver_stop_tx(handle);
    // 3. 重新初始化 RX（不是 disable/enable，而是 delete + recreate）
    i2s_channel_disable(handle->rx_handle);
    i2s_del_channel(handle->rx_handle);
    handle->rx_handle = NULL;
    // 重新创建 RX 通道
    i2s_chan_config_t rx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    i2s_new_channel(&rx_cfg, NULL, &handle->rx_handle);
    i2s_channel_init_std_mode(handle->rx_handle, &rx_std_cfg);
    i2s_channel_enable(handle->rx_handle);
}
```

**优点**: 不需要额外 GPIO，RX 获得全新的 DMA 状态。
**风险**: 提示音播放期间麦克风不可用（~2秒）；`i2s_del_channel` 可能碎片化 DMA heap。

#### 方案 D：不播放提示音，仅用蜂鸣器/LED 反馈

```c
// voice_service_toggle_backend() 中：
// cfg.play_prompt 改为空操作或仅闪烁 LED
```

**优点**: 最简单，零风险。
**缺点**: 用户体验差，没有语音确认。

#### 方案 E：提示音用 RMT 或 LEDC PWM 输出

不用 I2S TX 播放提示音，改用 RMT peripheral 直接驱动 GPIO 输出简单音调。

```c
// 用 RMT 发送 1kHz 方波 200ms 作为确认音
rmt_transmit(tx_channel, encoder, &tone_data, sizeof(tone_data));
```

**优点**: 完全不碰 I2S TX，RX 不受影响。
**缺点**: 音质差（方波），不能播放 WAV。

### 2.4 方案 B 实现要点（TDM 全双工）

如果选择方案 B，关键代码修改在 `audio_driver.c` 的 `create_tx_only_channel()` 和 RX 通道创建函数：

```c
// TX/RX 合并为一个全双工通道
i2s_chan_config_t chan_cfg = {
    .id = I2S_NUM_0,
    .role = I2S_ROLE_MASTER,
    .dma_desc_num = 4,
    .dma_frame_num = 240,
    .auto_clear = true,
};
i2s_new_channel(&chan_cfg, &handle->tx_handle, &handle->rx_handle);

i2s_tdm_config_t tdm_cfg = {
    .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(16000),
    .slot_cfg = {
        .slot_mode = I2S_SLOT_MODE_STEREO,
        .slot_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
        .ws_width = 32,
        .ws_pol = false,
        .bit_shift = true,
        .left_align = false,
        .big_endian = false,
        .bit_order_lsb = false,
        .skip_mask = false,
        .total_slot_num = 2,
        .slot_mask = I2S_TDM_SLOT0 | I2S_TDM_SLOT1,
    },
    .gpio_cfg = {
        .mclk = I2S_GPIO_UNUSED,
        .bclk = XIAOJING_GPIO_I2S_BCLK,
        .ws = XIAOJING_GPIO_I2S_WS,
        .dout = XIAOJING_GPIO_AMP_DIN,
        .din = XIAOJING_GPIO_MIC_SD,
    },
};
i2s_channel_init_tdm_mode(handle->tx_handle, &tdm_cfg);
i2s_channel_init_tdm_mode(handle->rx_handle, &tdm_cfg);
```

**关键验证点**:
1. INMP441 输出的 I2S 数据是否正确映射到 TDM slot 1
2. MAX98357 是否正确接收 TDM slot 0 的数据
3. TX 启用/禁用是否影响 RX 的时钟连续性

---

## 三、当前代码状态

### 3.1 关键文件清单

| 文件 | 状态 | 说明 |
|------|------|------|
| `components/audio_driver/audio_driver.c` | **需修改** | I2S TX/RX 分离 |
| `components/voice_wake_esp_sr/src/voice_wake_esp_sr.c` | 已修复 | 直接 AFE 调用，栈3072 |
| `components/voice_wake_esp_sr/CMakeLists.txt` | 已修复 | 无 BLE 依赖 |
| `components/voice_wake_edge/src/ei_classifier_wrapper.cpp` | 已修复 | 跳过不必要的重采样 |
| `components/voice_wake_edge/src/voice_wake_edge.c` | 已有 | 滑动窗口+阈值+增益 |
| `components/voice_frontend/src/voice_frontend.c` | 已有 | 后端抽象层 |
| `components/voice_agent/voice_service.c` | 已修复 | 移除 BLE pause |
| `components/voice_agent/CMakeLists.txt` | 已修复 | 移除 ble_transport 依赖 |
| `components/wifi_station/wifi_station.c` | 已修复 | 增强断连诊断 |
| `components/button_service/button_service.c` | 已有 | BTN1 长按切换 |
| `components/display_service/display_service.c` | 已有 | 4页 LVGL |
| `components/ble_transport/ble_transport.c` | 已有 | NimBLE GATT |
| `sdkconfig` | 已修复 | XIAOAITONGXUE, STATIC_RX=6 |
| `main/app_bootstrap.c` | 已有 | 启动流程 |

### 3.2 构建命令

```powershell
# 激活 ESP-IDF 环境
. "D:\UserData\ESP32Project\IDF\ESP32-IDF-Test\.claude\esp-idf-activate.ps1"
Set-Location "D:\UserData\ESP32Project\IDF\ESP32-IDF-Test"

# 构建
idf.py build

# 烧录（COM8）
idf.py -p COM8 flash

# 监控
idf.py -p COM8 monitor
```

### 3.3 NVS 命名空间

| 命名空间 | 键 | 类型 | 说明 |
|----------|-----|------|------|
| `xj_wifi` | `ssid`, `password` | string | WiFi 凭据 |
| `xj_voice` | `schema`, `provider`, `api_key`, `model` | u8/str | 语音凭据 |
| `xj_runtime` | `wake_backend` | u8 | 0=Edge, 1=ESP-SR |

### 3.4 任务分配

| 任务 | 栈 | 优先级 | 核心 | 内存 |
|------|-----|--------|------|------|
| audio_controller capture | 4096 | 5 | 1 | internal |
| voice_frontend (已移除) | - | - | - | - |
| voice_cloud_adapter | 32768 | 5 | 0 | - |
| button_service | 4096 | 15 | 1 | PSRAM |
| wifi_async_start | 4096 | 5 | - | - |
| LVGL | - | 4 | 1 | PSRAM |
| NimBLE host | 4096 | - | 0 | PSRAM |
| esp_sr_model | 3072 | 6 | 1 | internal |

---

## 四、待完成工作

### 4.1 阻塞项（Stage 1-4 门禁）

- [ ] **I2S TX/RX 干扰修复**（方案 A/B/C/E 中选一个）
- [ ] **ESP-SR 唤醒验证**（修复后测试"小爱同学"）
- [ ] **后端切换后唤醒验证**（Edge→ESP-SR→Edge 全流程）
- [ ] **BTN1 长按不卡死验证**

### 4.2 Stage 5: LVGL 工业风界面

- [ ] 5 页面重构（仪表盘/洗涤/传感器/语音/诊断）
- [ ] 配色改为浅色工业风
- [ ] PAJ7620 手势翻页
- [ ] 故障全局告警条
- [ ] stale 数据指示

### 4.3 Stage 6: 联合测试

- [ ] 静态扫描（API key/密码泄漏）
- [ ] 1小时 soak 测试
- [ ] 资源门禁（heap/stack/I2S overrun）

---

## 五、已知硬件限制

1. **INMP441 L/R 引脚可能悬空** — 导致麦克风通道在 LEFT/RIGHT 间随机切换。建议将 L/R 引脚接 GND（固定 LEFT）或 VCC（固定 RIGHT）。

2. **SHT30 CRC 持续失败** — 每10秒一次，累计数千次错误。可能是 I2C 总线干扰或传感器故障。

3. **GPIO47 (AMP_SD) 需要 board identity 确认** — 未确认时不会驱动功放。
