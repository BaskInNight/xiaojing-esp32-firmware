# Claude Code 使用 Prompt

## 1. 首次架构审计 Prompt

首次在 Claude Code 打开 `D:/UserData/ESP32Project/IDF/ESP32-IDF-Test` 后，复制以下内容：

```text
你正在现有 ESP32-IDF-Test 模板上开发“小净”洗衣机新版 ESP-IDF 固件，固定使用 ESP-IDF 5.5.4、目标 ESP32-S3-WROOM-1 N16R8。

这一轮只做分析审计，不修改任何文件。

使用 /superpowers 和 /embedded-systems 分析工程架构；使用 /esp32 检查 GPIO、外设资源和 ESP32-S3 限制。不要一次调用 /systematic-debugging 或 /code-review，因为现在还没有实现 diff 或具体故障。

按顺序完整阅读：
1. 根目录 CLAUDE.md
2. 根目录 AGENTS.md
3. docs/xiaojing_idf_design/00_README_文档索引.md
4. docs/xiaojing_idf_design/01_REQUIREMENTS.md
5. docs/xiaojing_idf_design/02_PROJECT_STRUCTURE.md
6. docs/xiaojing_idf_design/03_INTERFACES.md
7. docs/xiaojing_idf_design/04_DEVELOPMENT_PLAN.md
8. docs/xiaojing_idf_design/05_TEST_PLAN.md
9. docs/xiaojing_idf_design/08_PROGRESS.md

然后检查当前工程的 CMakeLists.txt、main/、components/、sdkconfig配置来源和已有构建状态。不要扫描 build/，不要逐行分析旧 Arduino 工程，不要修改 .claude/，不要直接编辑 sdkconfig。

输出一份“Phase 0 架构差距审计”，必须包括：
- 当前工程已有内容和可复用部分；
- 与 01_REQUIREMENTS/02_PROJECT_STRUCTURE/03_INTERFACES 的差距；
- ESP-IDF 5.5.4 是否正确激活；
- GPIO1/2/4/5/6/7/9/10/11/12/13/14/15/16/17/18/21/38/39/40/41/47/48 的ESP32-S3限制与冲突；
- LEDC、PCNT、I2C、I2S、SPI、ADC、NimBLE、Wi-Fi、PSRAM资源规划；
- 当前代码是否存在阻塞、裸GPIO、跨层调用或不安全输出；
- Phase 0 最小修改文件列表；
- 风险、待标定项和不能凭空假定的参数；
- 建议的测试/构建命令。

最后停止，不要实现代码，等待我确认。
```

## 2. 开始 Phase 0 Prompt

确认审计结果后复制：

```text
开始执行 docs/xiaojing_idf_design/04_DEVELOPMENT_PLAN.md 的 Phase 0。

使用 /superpowers 和 /esp32，严格遵守根 CLAUDE.md、AGENTS.md 以及 xiaojing_idf_design 的 01-05 文档。

要求：
- 在现有 ESP32-IDF-Test 模板上增量开发，不删除后重建；
- 固定 ESP-IDF 5.5.4，先记录 idf.py --version 和修改前基线 build；
- 不修改 .claude/；
- 不直接编辑 sdkconfig；
- 不修改旧 Arduino 工程；
- 默认 FAKE 输出，PTC 真实输出关闭；
- 只完成 Phase 0，不提前生成所有业务组件；
- 每个结论必须有实际文件或命令输出支持，不伪造构建和测试结果。

实现完成后：
1. 运行 idf.py reconfigure 和 idf.py build；
2. 使用 /code-review 审查当前真实 diff 是否符合 ESP-IDF 5.5.4、并发和安全规则；
3. 修复审查问题并重新构建；
4. 更新 docs/xiaojing_idf_design/08_PROGRESS.md；
5. 汇报修改文件、命令、原始结果摘要、剩余风险和下一 Phase，但不要自动进入下一 Phase。
```

## 3. 后续 Phase Prompt

```text
继续小净 ESP-IDF 重构。先读取根 CLAUDE.md、AGENTS.md、docs/xiaojing_idf_design/08_PROGRESS.md 和 git diff，只执行 04_DEVELOPMENT_PLAN.md 中下一个未完成 Phase。

按 06_PHASE_PROMPTS.md 和当前 Phase 选择 Skills：
- 普通新模块：/superpowers + /embedded-systems，先分析不修改；
- GPIO/ESP32外设：/esp32；
- Task/Queue/EventGroup/状态机：/embedded-systems；
- 崩溃/watchdog/栈/内存/编译异常：/systematic-debugging；
- 模块完成：必须 /code-review。

不要每轮调用全部 Skills。只修改当前 Phase 范围；完成 build、测试、code-review、修复和 08_PROGRESS.md 后停止。
```

## 4. Bug Prompt

```text
使用 /systematic-debugging 分析这个 ESP32-S3 问题。先不要改代码。

收集并引用：完整日志、复现步骤、ESP-IDF 5.5.4确认、固件hash、reset reason、backtrace、ELF/map、heap/PSRAM、各Task stack high-water mark、当前 machine state/phase/request_id/fault 和最近输出历史。

给出可证伪的根因假设和最小验证实验；定位后再做最小修复。修复后使用 /code-review，并运行相关用例及 idf.py build。不要用delay、加大栈或屏蔽watchdog掩盖根因。
```

## 5. 硬件联调 Prompt

```text
使用 /esp32 和 /embedded-systems。只执行 docs/xiaojing_idf_design/05_TEST_PLAN.md 中我明确指定的测试等级。

先不要驱动硬件。先列出：接线复核、GPIO/外设限制、当前输出模式、测量点、预期波形/状态、超时、立即断电条件。等待我确认后再给出逐步操作。

严禁自行启用真实 PTC；IBT-2方向、BL50 BRAKE/STOP极性未标定前只允许受控低压点动。完成后更新 07_CALIBRATION.md 和 08_PROGRESS.md。
```

