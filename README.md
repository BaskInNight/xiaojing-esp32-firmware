# 小净 ESP-IDF 固件

"小净"非交叉污染洗衣机 ESP-IDF 固件，基于 ESP-IDF v5.5.4。

目标芯片：ESP32-S3-WROOM-1 N16R8（16MB Flash, 8MB OPI PSRAM）。

## 环境要求

- ESP-IDF v5.5.4（已通过 `install.bat` 和 `export.bat` 配置）
- 目标芯片：ESP32-S3（可在 `menuconfig` 中切换）

## 常用命令

```bash
# 编译
idf.py build

# 烧录（需连接开发板）
idf.py -p COMx flash

# 烧录并监控串口输出
idf.py -p COMx flash monitor

# 仅监控
idf.py -p COMx monitor

# 打开菜单配置
idf.py menuconfig

# 清理编译产物
idf.py fullclean
```

> 将 `COMx` 替换为实际串口号（如 `COM3`）。

## 工程结构

```
.
├── .claude/                  # Claude Code 配置（见下方说明）
│   ├── esp-idf-activate.ps1  # ESP-IDF 环境激活脚本
│   └── settings.local.json   # hook 配置（自动激活 idf.py 环境）
├── components/               # 自定义组件（见"添加组件"）
│   └── README.md             # 组件开发规范
├── docs/                     # 设计文档（硬件、架构、引脚表）
│   └── README.md
├── main/
│   ├── CMakeLists.txt        # main 组件 CMake 配置
│   └── app_main.c            # 应用入口 app_main()
├── CLAUDE.md                 # Claude Code 开发规则
├── AGENTS.md                 # AI Agent 通用开发规则
├── CMakeLists.txt            # 顶层 CMake 配置
├── sdkconfig                 # 编译配置（idf.py menuconfig 生成）
└── README.md
```

## 如何基于此模板创建新项目

1. 复制整个工程目录到新位置：
   ```bash
   cp -r ESP32-IDF-Template my_new_project
   cd my_new_project
   ```

2. 修改 `CMakeLists.txt` 中的项目名称：
   ```cmake
   project(my_new_project)
   ```

3. 在 `main/` 中添加你的源文件，并更新 `main/CMakeLists.txt`：
   ```cmake
   idf_component_register(SRCS "app_main.c" "my_driver.c"
                          PRIV_REQUIRES spi_flash driver
                          INCLUDE_DIRS "")
   ```

4. 编写你的 `app_main.c` 应用逻辑。

5. 编译验证：
   ```bash
   idf.py build
   ```

## 添加组件

ESP-IDF 支持两种放置代码的位置：

| 位置 | 用途 |
|------|------|
| `main/` | 应用入口和与当前项目紧密耦合的代码 |
| `components/` | 可复用的独立组件（驱动、协议库、中间件） |

当一个模块可能被多个项目共享时，应放在 `components/` 中。

**创建新组件示例：**

```
components/
└── my_sensor/
    ├── CMakeLists.txt
    ├── include/
    │   └── my_sensor.h
    └── my_sensor.c
```

`components/my_sensor/CMakeLists.txt`：

```cmake
idf_component_register(SRCS "my_sensor.c"
                       INCLUDE_DIRS "include")
```

组件会被 ESP-IDF 构建系统自动发现，无需在顶层 `CMakeLists.txt` 中手动引用。

## Claude Code 集成

此工程已配置 Claude Code hook，在执行 `idf.py` 命令时自动激活 ESP-IDF Python 环境，无需手动 `export.bat`。

- `.claude/esp-idf-activate.ps1` — 环境变量和 PATH 设置
- `.claude/settings.local.json` — PreToolUse hook，拦截 `idf.py` 命令并自动注入环境

**迁移注意事项：** 如果 ESP-IDF 安装路径与本机不同，需同时更新以上两个文件中的路径。

## AI 协作开发

### AI Agent 规则文件

| 文件 | Agent | 用途 |
|------|-------|------|
| `CLAUDE.md` | Claude Code | 项目开发规则、代码规范、构建验证 |
| `AGENTS.md` | Codex / 通用 Agent | 架构说明、目录规范、Git 提交规范 |

启动 Claude Code 或 Codex 时会自动读取对应规则文件。

### 推荐开发流程

```
需求提出
  ↓
Claude/Codex 分析现有代码和文档
  ↓
修改 components/ 或 main/
  ↓
idf.py build 验证
  ↓
硬件测试
  ↓
Git 提交
```

### 使用 Claude Code

```bash
claude
# Claude 自动读取 CLAUDE.md
# 执行 idf.py 时自动激活 ESP-IDF 环境
```

### 使用 Codex

```bash
codex
# Codex 自动读取 AGENTS.md
```

### 设计文档

新硬件或新功能开发前，先在 `docs/` 中建立设计文档：

- `docs/hardware.md` — 硬件连接、GPIO 分配、电源说明
- `docs/software_arch.md` — 软件架构、组件关系
- `docs/pin_map.md` — GPIO 资源表
