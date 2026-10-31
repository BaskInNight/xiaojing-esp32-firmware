# Claude Code 分阶段 Prompt

使用方式：先用 `00_CLAUDE_CODE_使用Prompt.md` 的首次审计 Prompt。之后每次只复制当前 Phase 的 Prompt，不要一次把全部 Phase 发给 MiMo。

## Phase 0

```text
使用 /superpowers 和 /esp32。先分析，不修改：检查根 CLAUDE.md、AGENTS.md、现有 CMake/main/components 和 xiaojing_idf_design 的 REQUIREMENTS、PROJECT_STRUCTURE、DEVELOPMENT_PLAN Phase 0，确认 ESP-IDF 5.5.4、ESP32-S3 N16R8、Flash/PSRAM/分区和默认 fake 输出方案。列出可复用文件、最小修改和风险，不删除模板重建。

分析通过后只实现 Phase 0。先运行修改前基线 build，再执行 idf.py --version、reconfigure、build，更新 08_PROGRESS.md。不得修改 .claude/ 或直接编辑 sdkconfig。最后使用 /code-review 审查真实 diff 和 ESP-IDF 5.5.4 配置，修复后复测。不要创建后续业务组件。
```

## Phase 1

```text
使用 /superpowers 和 /embedded-systems。先分析 Phase 1 的公共类型、所有权、fake time、HAL函数表、状态快照和配置版本，不修改。重点检查固定容量、并发和无环依赖。

再实现 Phase 1 和测试。完成后使用 /code-review 审查真实 diff，修复并运行 idf.py build/本阶段测试，更新 PROGRESS.md。
```

## Phase 2

```text
使用 /esp32 和 /embedded-systems。先分析 Phase 2，只读取新 REQUIREMENTS 的 GPIO 表；旧 Arduino 引脚只用于识别废弃项。检查 ESP32-S3 GPIO、LEDC、PCNT、I2C、I2S、SPI/ADC资源、ISR和ESP-IDF 5.5.4 API，不修改。

按驱动组 A/B/C/D 逐组实现，每组保持默认 fake/真实输出禁用；每组完成使用 /code-review。出现编译或运行异常时才调用 /systematic-debugging。记录构建和台架未验证项。
```

## Phase 3：Position

```text
使用 /embedded-systems 设计 position_service 的Task、Queue、request_id、非阻塞30ms消抖、五霍尔、未知位置、360°方向、超时/取消/冲突状态机；使用 /esp32 复核 MCP INTB 和 GPIO40/41/48。先分析，不修改。

再只实现 Phase 3 和 TEST_PLAN 的 POS-T01~T14。未实测方向时禁止REAL自动运动。完成后 /code-review，修复、复测、更新PROGRESS。
```

## Phase 4：Safety

```text
使用 /embedded-systems，先审查 safety_manager 联锁矩阵、20ms任务、锁存故障、非阻塞停机hook和组件无环依赖；使用 /esp32 检查 MCP 安全输出路径。先分析，不修改。

再只实现 Phase 4 和 SAFE-T01~T14。完成后 /code-review，证明没有业务旁路可以开启高风险输出。
```

## Phase 5：Water

```text
使用 /embedded-systems 和 /esp32。先分析分批水路 SOURCE_FILL->TRANSFER->CHECK_DRUM_LEVEL、GPIO18 PCNT、洗涤桶水位、阀门互锁、最大批次、总超时和所有取消点，不修改。

再只实现 Phase 5 和 WATER-T01~T14。用fake输出历史证明GPA0/GPA1/GPA3不重叠。完成后 /code-review。
```

## Phase 6：Dry/Aux

```text
使用 /embedded-systems 和 /esp32。先分析 dry/BL50/drain/detergent/UV/button 各局部状态机和安全边界，不修改。PTC真实输出必须保持禁用。

分组件实现并测试，每个组件完成使用 /code-review。出现watchdog、栈、I2C或内存异常时使用 /systematic-debugging，不得用增加delay掩盖问题。
```

## Phase 7：Planner/Executor

```text
使用 /superpowers 和 /embedded-systems。先分析纯planner、唯一executor、Formal/Demo共模板、进排水强制、姿态补齐、request_id迟到事件、BTN3抢占和故障终态，不修改。

再只实现 Phase 7 及 PLAN/EXE 全部用例。完成后 /code-review，确认planner没有FreeRTOS/JSON/硬件依赖，executor是唯一整机流程所有者。
```

## Phase 8：BLE

```text
使用 /embedded-systems 和 /esp32。先分析 NimBLE callback边界、RX重组、TX优先级、2048B/3s、seq幂等、断线重连和ESP-IDF 5.5.4 API，不修改。

再实现 Phase 8 和 BLE-T01~T14。不要修改旧Arduino目录的小程序；需要小程序时创建独立 miniapp4.0_IDF。完成后 /code-review。
```

## Phase 9：Voice

```text
使用 /superpowers、/embedded-systems 和 /esp32。先分析ESP-IDF 5.5.4 I2S、Wi-Fi/BLE共存、PSRAM、HTTPS证书、MiMo tool parser、任务栈和BTN3抢占，不修改。

再实现 Phase 9 和 VOICE-T01~T12。若有TLS崩溃/watchdog/内存下降，使用 /systematic-debugging 基于日志和backtrace定位。完成后 /code-review。
```

## Bug 模板

```text
使用 /systematic-debugging。不要先改代码。收集并分析：完整日志、复现步骤、idf.py版本、固件hash、reset reason、backtrace、ELF/map、heap、各Task栈余量和最近状态/request_id。提出可证伪的根因假设与最小实验，定位后再修复。修复完成使用 /code-review 并运行相关回归测试。
```

## 硬件测试模板

```text
使用 /esp32 和 /embedded-systems。只执行TEST_PLAN中我指定的测试等级。先列出接线复核、GPIO/外设限制、输出模式、测量点、预期波形/状态、超时、立即断电条件，不修改也不驱动硬件。等待人工确认后再给出逐步操作。完成后记录CALIBRATION.md，并使用 /code-review 审查标定值如何进入配置。
```
