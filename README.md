# 小净洗护设备固件

小净非交叉污染洗护设备的 ESP32-S3 固件，基于 ESP-IDF v5.5.4。仓库只保留当前可维护源码、构建配置和测试代码，不包含构建缓存、串口日志、阶段性验证产物或历史固件快照。

## 硬件与软件基线

- MCU：ESP32-S3-WROOM-1 N16R8
- Flash：16 MB
- PSRAM：8 MB OPI，80 MHz
- SDK：ESP-IDF v5.5.4
- 工程名：`xiaojing_idf`
- 分区表：`partitions.csv`

## 功能概览

固件采用 ESP-IDF 组件化结构，主要能力包括：

- 洗护流程编排：进水、排水、洗涤、烘干、紫外、热风和位置控制
- 安全控制：安全互锁、执行器自检、故障状态管理
- 硬件驱动：BL50、IBT-2、MCP23017、PAJ7620、SHT、浊度及水位/流量检测
- 交互与联网：BLE、Wi-Fi、显示、手势、音频和语音代理
- 测试支持：Fake HAL、主机单元测试、ESP-IDF 板端测试应用

## 目录结构

```text
.
├── main/             应用入口、初始化与顶层编排
├── components/       驱动、服务、协议和可复用业务组件
├── host_tests/       可在主机运行的核心逻辑测试
├── test_apps/        ESP-IDF 板端测试与启动冒烟测试
├── CMakeLists.txt    ESP-IDF 顶层工程配置
├── partitions.csv    16 MB Flash 分区表
├── sdkconfig         当前硬件构建基线
└── sdkconfig.defaults* 默认及 Fake HAL 配置
```

`main/app_main.c` 保持精简，具体初始化由 `app_bootstrap.c` 负责；硬件驱动、状态机和协议实现均位于 `components/`。

## 环境准备

安装并激活 ESP-IDF v5.5.4，然后确认：

```bash
idf.py --version
```

目标芯片已经配置为 ESP32-S3。首次在新环境构建时，可执行：

```bash
idf.py set-target esp32s3
idf.py build
```

如需严格复现仓库中的当前配置，保留仓库自带的 `sdkconfig`，直接执行 `idf.py build`。

## 编译、烧录与监控

```bash
# 编译
idf.py build

# 烧录
idf.py -p COMx flash

# 烧录并打开串口监控
idf.py -p COMx flash monitor

# 只打开串口监控
idf.py -p COMx monitor

# 图形化修改配置
idf.py menuconfig
```

将 `COMx` 替换为实际串口号。不要直接手工编辑 `sdkconfig`；配置变更应通过 `idf.py menuconfig` 完成，并将需要跨环境保持的选项同步到 `sdkconfig.defaults`。

## 语音凭据

真实凭据不纳入版本管理。复制示例文件：

```powershell
Copy-Item main/include/voice_secrets.example.h main/include/voice_secrets.h
```

然后仅在本地填写 `XIAOJING_VOICE_API_KEY`。`main/include/voice_secrets.h` 已被 `.gitignore` 排除。

## 测试

主机测试入口位于 `host_tests/`，板端测试项目位于 `test_apps/`：

- `test_apps/unit/`：组件单元与集成测试
- `test_apps/boot_smoke/`：标准启动验证
- `test_apps/boot_smoke_b/`、`boot_smoke_c/`：不同功能组合启动验证
- `test_apps/boot_smoke_d_no_psram/`：无 PSRAM 场景验证

提交代码前至少运行主工程构建：

```bash
idf.py build
```

涉及核心状态机或驱动逻辑时，应同时运行相应主机测试或板端测试。

## 开发约定

- 应用编排、任务创建和顶层状态机放在 `main/`
- 驱动、协议库及可复用模块放在 `components/`
- 优先使用 ESP-IDF 官方 API，检查所有关键 `esp_err_t` 返回值
- 使用 `ESP_LOGI/W/E` 记录日志，不使用 `printf`
- 修改硬件接口时同步维护引脚定义和相关说明
- 不提交 `build/`、`managed_components/`、日志、固件二进制或真实凭据

## 交接说明

本仓库是从当前有效工作区整理出的干净交接基线。过程性日志、阶段报告、旧固件和硬件厂商原始资料未包含在仓库中；如需审计历史，应从原工作区或独立归档获取。
