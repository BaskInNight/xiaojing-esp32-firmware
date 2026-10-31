# 小净 IDF 重构文档索引

> Phase 11 已确认但尚未实施的功能（ESP-SR MultiNet 离线指令、AI 智能洗护顾问）统一记录在 [`../PHASE11_DEFERRED_DEVELOPMENT_BACKLOG.md`](../PHASE11_DEFERRED_DEVELOPMENT_BACKLOG.md)。

适用工程：`D:/UserData/ESP32Project/IDF/ESP32-IDF-Test`  
工具链：ESP-IDF 5.5.4  
目标：ESP32-S3-WROOM-1 N16R8

## 使用原则

本目录是小净新版固件的需求与实现基线。目标工程已经是可用的 ESP-IDF 模板，Claude Code 必须先审计现状，再按 Phase 增量实现，不能删除模板后重建。

根目录的 `CLAUDE.md` 和 `AGENTS.md` 继续有效，尤其是：

- 不修改 `.claude/` 环境 hook。
- 不直接编辑生成的 `sdkconfig`，通过 `sdkconfig.defaults`、Kconfig 或 `idf.py menuconfig` 管理配置。
- 每次修改后运行 `idf.py build`。
- 保持 `app_main()` 简洁，驱动和模块放在 `components/`。

## 必读顺序

1. `../../CLAUDE.md`
2. `../../AGENTS.md`
3. `00_README_文档索引.md`
4. `01_REQUIREMENTS.md`：产品、硬件和安全需求，最高优先级。
5. `02_PROJECT_STRUCTURE.md`：目标目录、组件层级和依赖规则。
6. `03_INTERFACES.md`：核心 C 接口契约。
7. `04_DEVELOPMENT_PLAN.md`：Phase 0-12 开发顺序。
8. `05_TEST_PLAN.md`：每阶段测试和整机测试。
9. `06_PHASE_PROMPTS.md`：分阶段 Claude Code Prompt。
10. `08_PROGRESS.md`：实际进度与证据。
11. `09_PHASE0_架构审计复核.md`：对首次 Claude 审计的硬件与构建纠错，Phase 0 实现前必读。
12. `12_PHASE6_GROUP_C_AUX_AUDIT.md`：Group C 辅助服务审计（drain/detergent/uv）。
13. `13_PHASE6_GROUP_D_BUTTON_AUDIT.md`：Group D 按钮服务审计（button_service + MCP23017 中断链）。

`参考A-G` 只用于追溯讨论背景和旧代码行为。它们不能覆盖 `01-07` 的已确认设计。

## 信息优先级

```text
用户当前明确指令
> 01_REQUIREMENTS
> 03_INTERFACES
> 02_PROJECT_STRUCTURE
> 04_DEVELOPMENT_PLAN / 05_TEST_PLAN
> 参考A-F设计讨论
> 参考G旧Arduino审计
> 旧Arduino源代码
```

## 工程隔离

```text
目标 IDF 工程：D:/UserData/ESP32Project/IDF/ESP32-IDF-Test
旧 Arduino 工程：D:/UserData/ESP32Project/ESP32-S3-Wroom1-DevKitC1_XIAOJING
旧小程序：D:/UserData/ESP32Project/ESP32-S3-Wroom1-DevKitC1_XIAOJING/miniapp4.0
```

旧 Arduino 工程只读。小程序需要修改时复制到独立目录 `D:/UserData/ESP32Project/IDF/miniapp4.0_IDF`。

## 推荐操作

首次先使用 `00_CLAUDE_CODE_使用Prompt.md` 的“首次架构审计 Prompt”。审计通过后，再使用“开始 Phase 0/下一 Phase Prompt”。

每次只处理一个 Phase，完成后必须：

1. `idf.py build`。
2. 运行当前 Phase 测试。
3. 使用 `/code-review` 审查真实 diff。
4. 修复并复测。
5. 更新 `08_PROGRESS.md`。
