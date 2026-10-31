# Phase 0 架构差距审计复核

日期：2026-07-21  
结论：**有条件通过；纠正下列问题后可以开始 Phase 0。**

## 1. 必须纠正：GPIO11 不是 VDD_SPI 电源脚

原审计把 GPIO11 判定成 ESP32-S3 的 VDD_SPI 内部 LDO 电源输出，因此要求迁移 LCD MOSI。该结论错误，疑似混入了旧 ESP32 或其他封装资料。

官方 ESP32-S3-DevKitC-1 管脚表把 GPIO11 明确列为普通 `I/O/T`，功能包括 `RTC_GPIO11/GPIO11/ADC2_CH0/FSPID` 等。项目接线文档 `主控接线1.3.html` 也把 GPIO11 审核为可用的 LCD MOSI。

处理决定：

- 保留 `GPIO11 = LCD_MOSI`。
- 保留 `GPIO12 = LCD_SCK`、GPIO9 DC、GPIO10 CS。
- Phase 0 不修改 LCD 接线，不修改 `01_REQUIREMENTS.md` 的 GPIO11 分配。
- `board_config` 可以使用 SPI GPIO Matrix 将 MOSI/SCK 映射到 GPIO11/12。

官方依据：

- [ESP32-S3-DevKitC-1 用户指南](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/index.html)
- [ESP32-S3 系列芯片技术规格书](https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_cn.pdf)

## 2. 真正需要确认：GPIO47/48 的电压域

ESP32-S3 数据手册说明：如果芯片是 `ESP32-S3R8V/R16V`，VDD_SPI 为 1.8V，GPIO47/48 也工作在 1.8V。这里的关键是型号尾缀 `V`。

本项目设计目标写的是 `ESP32-S3-WROOM-1-N16R8`，不是 `WROOM-2-N16R8V`。在真实输出前必须查看模组屏蔽罩/采购标签和载板版本，不能只按工程名推断。

硬件门槛：

- 若实物确认是 `ESP32-S3-WROOM-1-N16R8`：按对应模组资料确认 GPIO47/48 3.3V 能力后，继续使用 GPIO47 AMP_SD、GPIO48 IBT2_EN。
- 若实物是 `N16R8V` 或采用 R8V/R16V 芯片：GPIO47/48 不能直接当 3.3V 输出，必须加电平转换或重新分配。
- 未确认型号前，FAKE 模式不受影响；LOW_VOLTAGE/REAL 模式禁止驱动 GPIO47/48。

官方依据：

- [ESP32-S3 数据手册：R8V/R16V 的 GPIO47/48 为 1.8V](https://documentation.espressif.com/esp32-s3_datasheet_en.pdf)
- [Espressif DevKit FAQ：v1.0/v1.1 RGB LED 引脚差异](https://docs.espressif.com/projects/esp-faq/en/latest/hardware-related/development-board.html)

## 3. GPIO38/48 RGB LED 结论不能笼统套用

原审计写“GPIO48 与内置 RGB LED 冲突”。官方资料实际为：

- DevKitC-1 v1.0：RGB LED 在 GPIO48。
- DevKitC-1 v1.1：RGB LED 改到 GPIO38。
- 自研主控板只使用 WROOM 模组时，不一定存在开发板 RGB LED。

本项目 GPIO38 用双风扇 PWM、GPIO48 用 IBT2_EN。因此 Phase 0 应记录实际载板/开发板版本并检查原理图，不应假设某个 Kconfig 能“禁用硬件 LED”。即使软件不初始化 RGB LED，板载 LED 电路仍可能加载该引脚。

## 4. 工具链已实际验证

在目标工程通过 `.claude/esp-idf-activate.ps1` 实测：

```text
ESP-IDF v5.5.4
IDF_PATH=D:\Program\esp\.espressif\v5.5.4\esp-idf
Python 3.10.19
CMake 3.30.2
Ninja 1.12.1
```

Phase 0 不再把 IDF 版本标记为未知，但仍应把这些信息写入进度记录。

## 5. 修改前 build 的真实结果

执行 `idf.py build` 未进入源码编译，报错：

```text
Build directory ...\ESP32-IDF-Test\build configured for project
...\ESP32-IDF-Template not ...\ESP32-IDF-Test.
Run 'idf.py fullclean' to start again.
```

这是复制/改名工程后遗留的生成缓存，不是源码失败。

Phase 0 正确步骤：

1. 确认当前目录确为 `ESP32-IDF-Test`。
2. `idf.py fullclean`，只清理当前工程生成的 `build/`。
3. 在任何源码修改前重新执行 `idf.py build`，记录真正模板基线。
4. 不删除 main、components、docs 或 `.claude`。

## 6. `sdkconfig.defaults` 的应用方式

原审计写“创建 sdkconfig.defaults 后运行 reconfigure 覆盖现有 sdkconfig”，表述不严谨。现有 `sdkconfig` 的显式值通常优先于 defaults。

正确流程：

1. 先保存当前配置差异/基线信息。
2. 创建 `sdkconfig.defaults` 和 Kconfig 默认值。
3. 不手工编辑生成的 `sdkconfig`。
4. 使用 `idf.py set-target esp32s3` 受控重新生成配置；该操作会清理 build 并处理旧 sdkconfig。
5. `idf.py reconfigure` 后检查实际生效值。

必须验证：

```text
Flash size = 16MB
Flash mode = QIO（与实际 N16R8 硬件和原工程已用 qio 一致）
PSRAM = Octal/OPI 8MB，80MHz（以IDF 5.5.4实际Kconfig项为准）
NimBLE enabled
FAKE output mode
PTC disabled
partition table = 新16MB分区
```

不要使用审计中的模糊值 `QIO/DOUT`；DOUT 不是本项目目标配置。

## 7. 对审计其他内容的判断

可以接受：

- 现有模板可复用，业务组件为空。
- Phase 0 不创建后续业务组件空壳。
- GPIO26-32 保留给 SPI0/1 Flash/PSRAM，GPIO33-37 在 Octal PSRAM配置下不使用。
- LEDC、PCNT、I2C、ADC、NimBLE/Wi-Fi 资源总体足够。
- MCP/I2C ISR 不做 I2C。
- 默认 FAKE、PTC OFF、安全输出优先。

需要调整措辞：

- GPIO4 水位是数字输入，不因具备 ADC 能力而使用 ADC。
- 大语音缓冲可考虑 PSRAM；BLE 2KB 帧和实时控制数据不强制放 PSRAM。
- I2S RX/TX 共享 BCLK/WS 需要在音频 Phase 按一致采样率/slot 配置验证，不在 Phase 0 假定必然兼容。
- 分区名是 OTA，不是 `ott`；具体分区大小需要在 Phase 0 给出 offset/size 校验和总容量计算。

## 8. Phase 0 批准范围

可以开始：

- 清理错误路径的 build 缓存并记录真实模板基线。
- 创建 `sdkconfig.defaults`、项目 Kconfig 和 16MB partitions。
- 建立默认 FAKE/PTC OFF 配置。
- 创建最小 `board_config` 和 GPIO 静态校验。
- 更新项目名、bootstrap 最小安全启动、README 和进度记录。

禁止开始：

- 修改 GPIO11/LCD 接线。
- 在未确认模组/载板版本前真实驱动 GPIO47/48/38。
- 创建所有 Phase 1-9 业务组件空壳。
- 开启真实 IBT-2、BL50、阀、泵、UV 或 PTC。

## 9. 给 Claude Code 的纠正与执行 Prompt

```text
你的 Phase 0 审计有条件通过，但开始实现前必须接受以下纠正：

1. GPIO11 不是 ESP32-S3 的 VDD_SPI 电源脚。官方 DevKitC-1 管脚表明确 GPIO11 是普通 I/O/T，本项目继续使用 GPIO11=LCD_MOSI，禁止重新分配 LCD 引脚。
2. 真正需要确认的是模组是否带 V 后缀。只有 ESP32-S3R8V/R16V 的 GPIO47/48 明确工作在1.8V。当前需求写的是 WROOM-1-N16R8；在实物型号确认前，FAKE可继续，但禁止 LOW_VOLTAGE/REAL 驱动 GPIO47/48。
3. RGB LED 引脚随 DevKitC-1 版本不同：v1.0为GPIO48，v1.1为GPIO38；自研载板可能没有。不要用“禁用RGB Kconfig”代替载板/原理图确认。
4. 工具链已经实测为 ESP-IDF v5.5.4、Python3.10.19、CMake3.30.2、Ninja1.12.1。
5. 当前build目录绑定旧路径ESP32-IDF-Template。Phase 0先在确认当前目录后执行idf.py fullclean，仅清理生成目录，再运行修改前基线build。
6. sdkconfig.defaults不能靠普通reconfigure覆盖已有sdkconfig。创建defaults后，用idf.py set-target esp32s3受控重新生成，禁止手工编辑sdkconfig。
7. 目标存储配置为16MB Flash、QIO 80MHz、8MB Octal/OPI PSRAM 80MHz；具体IDF 5.5.4 Kconfig键需用 /esp32 和实际menuconfig核对，不能写QIO/DOUT。

现在开始 Phase 0。使用 /superpowers 和 /esp32；只做 docs/xiaojing_idf_design/04_DEVELOPMENT_PLAN.md 的 Phase 0。

约束：
- 保留GPIO11 LCD_MOSI；
- 不修改.claude；
- 不直接编辑sdkconfig；
- 不修改旧Arduino工程；
- 默认FAKE，PTC disabled；
- 不提前创建业务组件空壳；
- 不驱动真实GPIO47/48/38执行器。

实施顺序：
1. 读取 docs/xiaojing_idf_design/09_PHASE0_架构审计复核.md。
2. idf.py fullclean并完成修改前模板基线build。
3. 实现Phase 0最小范围。
4. idf.py set-target esp32s3、reconfigure、build、size、size-components。
5. 检查实际sdkconfig结果和16MB分区总容量。
6. 使用 /code-review 审查真实diff、ESP-IDF5.5.4 API、安全默认态和配置。
7. 修复并复测，更新08_PROGRESS.md。
8. 汇报真实命令与结果后停止，不进入Phase 1。
```

