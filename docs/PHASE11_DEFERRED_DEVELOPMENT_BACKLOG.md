# Phase 11 待开发功能清单

更新时间：2026-08-11  
适用工程：`D:/UserData/ESP32Project/IDF/ESP32-IDF-Test`

本文记录已经确认但尚未进入代码实现的功能，防止对话上下文压缩后遗漏。
本文中的项目均不得因已有接口、占位代码或设计文档而宣称完成；只有通过各自退出门禁后才能改为 `DONE`。

## Backlog 状态

| 编号 | 功能 | 建议阶段 | 当前状态 |
|---|---|---|---|
| BL-AUDIO-00 | 音频单控制器架构重构（复用旧工程稳定思路） | Phase 11 Group D.0.0 | `PLANNED / NOT_IMPLEMENTED` |
| BL-AUDIO-01 | MAX98357A 实物播放与 I2S TX 声学闭环 | Phase 11 Group D.0.1 | `OPEN / DEPENDS_ON_BL-AUDIO-00` |
| BL-VOICE-01 | ESP-SR MultiNet 唤醒后离线语音指令 | Phase 11 Group D.1 | `DEFERRED / NOT_IMPLEMENTED` |
| BL-ADVISOR-01 | AI 智能洗护顾问：从生活场景推导定制洗涤方案 | Phase 11 Group D.2 | `DEFERRED / NOT_IMPLEMENTED` |

---

## BL-AUDIO-00：音频单控制器架构重构

### 背景与架构决策

旧 Arduino 工程的麦克风与功放可以稳定共存，核心原因不是“完全没有 RAM 压力”，而是音频拓扑更简单：同一个 `I2S_NUM_0` 同时配置 RX + TX，麦克风和功放共享一个 BCLK/WS 时钟域；播放时逻辑暂停唤醒采集，播放完成后恢复采集，属于“硬件具备双向 channel、业务半双工使用”。

当前 ESP-IDF 生产链路采用 I2S1 长驻采集、I2S0 播放，两者共享 GPIO15/16。这带来了 GPIO 所有权切换、DMA 复位、采样率恢复以及播放后 RX 失同步风险。内部 RAM 连续块是现实约束，但不是保留双控制器竞争同一时钟引脚的充分理由。

本轮架构决策：迁移旧工程的“单控制器、单时钟域、业务半双工”原则，但不照搬旧工程过大的 DMA 配置和阻塞式实现。

### 重构目标

1. 生产固件只保留一个音频控制器：`I2S_NUM_0` 成对创建 RX/TX channels，共享同一标准模式时钟域。
2. 音频 DMA 必须在 Wi-Fi、BLE、Edge Impulse/ESP-SR 启动前一次性申请；运行期不删除、重建 channels。
3. 建立唯一 `audio_controller` 所有权状态机：`STOPPED`、`WAKE_CAPTURE`、`DIALOG_CAPTURE`、`PLAYBACK`、`RECOVERY`。
4. 任一时刻只能有一个 capture consumer 或 playback producer 持有 owner；所有权带 generation/token，陈旧回调不得恢复旧会话。
5. 优先固定硬件采样域为 16 kHz。24 kHz 本地提示音和 MiMo TTS 在 PSRAM 工作缓冲中软件重采样为 16 kHz，再复制到左右 slot，禁止播放时修改共享时钟。
6. 保留 INMP441 32-bit stereo 输入、有效声道选择和中性 PCM 缩放；Edge 的 8x 增益只留在 Edge backend，不污染录音和云端 ASR。
7. MAX98357A `SD_MODE` 必须 fail-safe：进入播放前才使能；播放失败、超时、取消、后端切换或 shutdown 均先静音，再恢复 RX。
8. 不允许业务组件直接操作 I2S channel、GPIO15/16 或 AMP GPIO；全部通过音频控制器。

### 分阶段施工计划

#### D.0.0-A：冻结基线与资源审计

- 记录 BIN/ELF SHA256、internal heap、largest block、PSRAM、I2S DMA 和启动顺序；
- 枚举全部 `i2s_new_channel`、`i2s_del_channel`、GPIO15/16 重配和采样率切换点；
- 保存 Edge、ESP-SR、PTT、ASR/TTS 和提示音现状日志，形成可比较基线。

#### D.0.0-B：单控制器底层

- bootstrap 早期创建 I2S0 RX/TX paired channels；
- channel 生命周期与 bootstrap 一致，运行期只 enable/disable；
- 移除 I2S1 capture-only 生产路径和 GPIO ownership resync 补丁；
- 初始化失败回滚到功放静音、无悬空任务、无半初始化句柄。

#### D.0.0-C：固定采样域与重采样

- 硬件固定 16 kHz；
- 新增有界、可单测的 24 kHz → 16 kHz PCM 重采样器；
- WAV 解码后统一为 mono 16-bit，再复制到 TX 两个 slot；
- 音频/定时器回调内禁止动态分配、联网和大计算。

#### D.0.0-D：所有权状态机接线

- 接入 Edge Impulse、ESP-SR、PTT/VAD、提示音和云端 TTS；
- 播放前暂停 wake/dialog feed，播放 terminal 后 exactly-once 恢复正确后端；
- BTN1 后端切换、BTN2 PTT、BLE 断线、Wi-Fi 重连和 shutdown 共用同一控制面。

#### D.0.0-E：自动化验证

- Host：重采样长度、边界、饱和、owner 冲突、陈旧 generation、失败恢复、terminal exactly-once；
- Fake HAL：播放中切后端、播放中 PTT、超时、取消、连续 1000 次 capture/play 切换；
- 静态扫描：唯一 I2S owner、运行期 channel recreate=0、AMP/I2S 旁路=0。

#### D.0.0-F：板级验证与长跑

- 1 kHz 数字诊断音速度/频率正确；
- 本地提示音、MiMo TTS、Edge“小净小净”、ESP-SR“小爱同学”和 PTT 分别通过；
- 两个后端交替完成至少 20 轮“唤醒→录音→云端→显示/播放→恢复唤醒”；
- BLE、Wi-Fi、屏幕、手势同时开启完成 4 小时 soak。

### 退出门禁

- `AUDIO_SINGLE_I2S_CONTROLLER = PASS`
- `RUNTIME_CHANNEL_RECREATE_COUNT = 0`
- `GPIO15_16_OWNER_CONFLICT = 0`
- `PLAYBACK_TO_CAPTURE_RECOVERY = 20/20`
- `EDGE_AND_ESP_SR_POST_PLAYBACK_WAKE = PASS`
- `BLE_WIFI_START_WITH_AUDIO_DMA = PASS`
- `AUDIO_SOAK_4H = PASS`：0 reset/WDT/Guru、无单调 heap 泄漏、I2S 计数持续增长。

该门禁只证明软件架构和数字音频生命周期。MAX98357A、PCB 和扬声器真实出声仍必须由 BL-AUDIO-01 独立验收。

---

## BL-AUDIO-01：MAX98357A 实物播放与 I2S TX 声学闭环

### 当前诚实结论

播放无声问题尚不能定性为“纯硬件故障”。现有证据已经证明上层素材、播放调用、I2S 生命周期恢复和一部分数字信号链路工作，但没有证明 MAX98357A 实际收到并正确解释了 PCM 采样字，也没有证明板载功放产生了有效的差分扬声器输出。

已通过或有较强证据支持的项目：

- 本地 WAV 资源可解析，运行时 TTS 和本地提示音均进入真实 `audio_driver_play_pcm16()` 路径。
- 逻辑分析仪观察到 BCLK、WS、DIN 活动，频率量级合理。
- MAX98357A VDD 约 5.15～5.16V，播放期间 SD_MODE 约 3.17～3.24V。
- 扬声器直流电阻约 3.4Ω，使用约 1.3V 电池轻触可产生沙沙声，扬声器线圈和基本接线大概率正常。
- 共享 I2S 时钟的软件生命周期问题已经修复；20 轮播放/切换/RX 重建没有 I2S 失步、复位、WDT 或崩溃。

仍未闭环的关键现象：

- 播放时 SPK+、SPK− 分别对地可测到近似相同变化，但 SPK+ 对 SPK− 的差分交流读数接近 0，且无可闻声音。
- 该现象更像功放没有形成 BTL 差分输出，但普通万用表不适合验证 Class-D PWM 波形，因此不能仅凭该读数确定芯片损坏。
- 两块板均无声，使“单块板偶发虚焊”可能性下降；I2S 帧格式/slot/channel/位宽与板上 GAIN_SLOT、SD_MODE 配置不匹配，以及共同的原理图、PCB、BOM或装配问题仍需同时排查。

### 必须执行的隔离测试

1. 增加独立的确定性音频诊断模式：关闭云端、TTS、唤醒和 I2S RX，仅循环播放幅度受限的 1kHz PCM；左右 slot 同时写入已知采样值。
2. 在可接触的串联电阻焊盘处同时采集 BCLK、WS、DIN，并用逻辑分析仪 I2S 解码验证实际采样数值、slot 归属、WS 极性、位宽、符号位和字节序，不能只记录频率。
3. 逐项对照 MAX98357A 数据手册和当前 ESP-IDF I2S 配置：Philips/I2S 标准、16/24/32-bit slot、mono/stereo、right/left slot、数据延迟一位、采样率和 BCLK 比例。
4. 核对板载 SD_MODE、GAIN_SLOT 的静态电平、上下拉、BOM、芯片 Pin 1 朝向及 PCB 网络；不得把悬空或错误 strap 默认视为正确。
5. 使用双通道示波器做 CH1-CH2 数学差分，或使用隔离/差分探头测 SPK+ 对 SPK−。Class-D 为桥接输出，禁止把任一扬声器端直接接普通接地示波器地夹。
6. 用同一组 BCLK/WS/DIN、同一只扬声器并联替换为已知正常的外置 MAX98357A 模块做 A/B：
   - 外置模块有声：板载功放、strap、PCB 网络、焊接或芯片问题。
   - 外置模块也无声：优先修正 I2S 帧格式、slot 或数据内容。
7. 根因修复后再恢复共享 RX，完成“播放提示音 → 麦克风恢复 → 双唤醒后端继续识别”的完整回归。

### 退出门禁

- 1kHz 诊断音可闻，且示波器确认 SPK+ 与 SPK− 存在非零反相信号/有效差分输出。
- 9 个 MiMo 预合成提示音、迁移的旧提示音和 task_done 提示均可正确播放，无错速、爆音、截断或乱码。
- 云端 MiMo 回复 TTS 连续播放不少于 5 次；播放结束后 I2S RX、Edge Impulse 和 ESP-SR 均能恢复。
- 双后端交替播放/唤醒不少于 20 轮：0 卡死、0 I2S 失步、0 reset、0 WDT、0 Guru、无单调内存泄漏。
- 报告必须给出明确根因分类和 A/B 证据，不得以“逻辑分析仪有频率”代替声学播放验收。

---

## BL-VOICE-01：ESP-SR MultiNet 唤醒后离线语音指令

### 用户目标

用户通过“小爱同学”或“小净小净”唤醒设备后，可以直接说固定的本地控制命令；无网络时仍能完成低延迟控制。

第一版建议命令集：

- 开始洗涤
- 停止洗涤
- 取消洗涤
- 查询状态
- 确认开始
- 取消方案
- 不要烘干
- 不要紫外
- 多漂洗一次
- 轻柔一点

### 技术路线

```text
Edge Impulse / ESP-SR WakeNet 唤醒
→ audio_controller 将 I2S 所有权从 WAKE 切换到 COMMAND
→ ESP-SR AFE + MultiNet 离线命令识别
→ 本地 command_id 白名单
→ voice_command_router
→ app request / planner / executor
```

MultiNet 只能输出本地白名单 `command_id`，不得直接控制 GPIO、MCP、继电器、PWM、电机或桶体姿态。

### 与云端的路由关系

- 明确命中本地命令：本地执行，不上传云端。
- 未命中或用户表达开放式需求：转入现有 MiMo 云端 AUTO_ROUTE。
- “开始洗涤”等会改变设备状态的命令，必须经过统一 app/planner/executor 路径。
- “确认开始”只允许确认当前仍有效、未过期且 program_id 匹配的待确认方案。
- CHAT_PRIORITY 模式不得执行改变设备状态的命令。

### 资源与并发约束

- 复用现有 ESP-SR、AFE、音频控制器和 PSRAM，不创建第二个 I2S owner。
- WakeNet、MultiNet、录音、播放之间必须使用现有音频所有权状态机串行切换。
- BLE、Wi-Fi、显示、Edge Impulse、ESP-SR 并存时重新记录 internal heap、largest block 和任务栈水位。
- 不允许在 LVGL、定时器或音频回调中执行阻塞识别或网络请求。

### 退出门禁

- 固定命令集在安静、背景噪声和不同说话距离下完成实机测试。
- 误触发测试不少于 30 分钟，危险控制误执行次数为 0。
- 唤醒 → 命令 → 唤醒连续不少于 30 轮，无卡死、无 I2S 失步。
- 本地命令与云端 AUTO_ROUTE 不重复提交同一 program。
- cancel/abort/fault/timeout 后所有输出遵循现有安全关断契约。
- 4 小时 BLE + Wi-Fi + display + voice soak：0 reset、0 WDT、0 Guru、无单调内存泄漏。

---

## BL-ADVISOR-01：AI 智能洗护顾问

### 用户目标

用户不需要像遥控器一样逐项指定时间、强度和次数。用户只描述衣物或生活场景，例如：

- “我要洗羊毛围巾。”
- “我刚打完球，帮我洗衣服。”
- “这几件衣服沾了很多泥。”

系统应推断衣物、面料、污渍、护理风险和洗护目标，再由本地规则生成最优且安全的洗涤方案。

正常回复只需要简要播报方案，不输出长篇推理，例如：

> 羊毛轻柔方案：短时低强度波轮洗、滚筒轻柔洗、漂洗两次、低速脱水，关闭 UV 和热风。是否开始？

只有缺少会显著改变安全方案的信息时才追问。

### 责任边界

```text
MiMo：理解生活场景，生成 WashProfile，标注假设和置信度
care_policy_engine：应用本地面料/污渍/护理规则，生成 WashRecipe
wash_planner：将 Recipe 编译成带安全步骤的 wash_program
wash_executor：唯一整机执行所有者
```

大模型不得直接生成或控制：

- GPIO / MCP / relay
- PWM、方向和电机命令
- 桶体角度或归位动作
- 未受限的阶段时长
- 绕过 safety_manager 的任何输出

### WashProfile（云端语义画像）

至少包含：

- garment：衣物类别
- fabric：面料类型及置信度
- soil_types：汗液、皮脂、泥沙、日常灰尘等
- soil_level：轻/中/重及置信度
- care_priority：护衣、洁净、除味、节水等
- machine_wash_allowed：已知/未知/允许/禁止
- color_mix_risk
- assumptions
- needs_clarification

云端输出必须使用严格 JSON Schema、枚举白名单、长度上限和置信度范围。

### WashRecipe（本地受约束配方）

至少包含：

- pulsator_level / pulsator_seconds
- drum_level / drum_seconds
- rinse_count（限定 1～3）
- spin_level
- enable_uv
- enable_hot_air / dry_level
- requires_confirmation
- recipe_id / rule_revision

所有数值必须由本地配置表和规则上限裁剪，不能直接信任模型数值。

### 第一版场景范围

1. 羊毛及羊毛围巾
2. 化纤或混纺运动衣物
3. 普通日常棉织物
4. 重污棉织物
5. 贴身衣物
6. 未知面料的保守降级方案

示例规则：

- 羊毛：极低波轮强度、滚筒轻柔、低速脱水、关闭热风，UV 默认关闭。
- 运动场景：推断汗液/皮脂/异味；默认增加漂洗，但不能仅凭“打球”断言存在重泥污。
- 未知面料：不得自动采用高机械强度、高速脱水或高温方案。
- 危险或互相矛盾的要求：拒绝、保守降级或要求确认。

### 方案确认状态机

```text
LISTENING
→ RECORDING
→ CLOUD_INFERENCE
→ PROFILE_PARSED
→ PLAN_PREVIEW
→ WAIT_CONFIRMATION
   ├─ 确认/开始 → SUBMITTING → EXECUTING
   ├─ 取消 → CANCELLED → LISTENING
   ├─ 修改偏好 → RECOMPILE → PLAN_PREVIEW
   └─ 超时 → EXPIRED → LISTENING
```

- 确认前执行器通电次数必须为 0。
- 待确认方案必须绑定 generation、program_id、创建时间和规则版本。
- 旧确认、重复确认、BLE/语音双入口并发确认不得重复提交。
- 用户可以说“不要烘干”“不要 UV”“多漂洗一次”“轻柔一点”，但每次修改都必须重新经过本地规则和 planner。

### 与默认 Demo 流程的结合

Recipe 编译后的安全骨架：

```text
UV 开启（若允许）
→ 启动前检查
→ 桶体归位 0°
→ 进水
→ 投放洗涤剂
→ 波轮预洗
→ 停机与静止确认
→ 桶体切换 90°
→ 滚筒主洗
→ 停机与静止确认
→ 桶体切换 180°
→ 排水与脱水
→ 按 rinse_count 执行受约束漂洗循环
→ 桶体切换 270°
→ 低速间歇翻转热风（若允许）
→ 桶体归位 45°
→ UV 关闭
→ 等待取衣
```

所有换位、停机、静止确认、排水、脱水、热风和最终关断仍由本地 planner/executor 插入与校验。

### 建议开发分轮

1. **D.2.1 协议与解析**：`recommend_wash_program`、WashProfile Schema、严格解析、错误降级。
2. **D.2.2 本地策略引擎**：纯 C `care_policy` 组件、规则矩阵、方案摘要。
3. **D.2.3 Planner 接入**：`wash_planner_compile_recipe()`、默认 Demo 骨架、漂洗循环和热风间歇翻转。
4. **D.2.4 确认与 UI**：语音/屏幕/小程序方案预览、修改、确认、取消、超时。
5. **D.2.5 验收**：场景集、冲突集、故障注入、并发入口、30 轮循环和 4 小时 soak。

### 退出门禁

- 危险组合和底层硬件字段拦截率 100%。
- 相同 WashProfile + 相同规则版本必须生成相同 WashRecipe。
- 羊毛等保护规则不能被云端输出覆盖。
- 云端超时、JSON 损坏、低置信度和规则编译失败均不得启动设备。
- 确认前无输出通电；取消、超时、fault 后遵循统一安全关断。
- 语音、小程序、按钮和手势入口不得重复提交同一 program。
- 至少完成羊毛、运动衣、日常棉、重污棉、贴身衣物和未知面料实机/回放验收。
- 连续多轮对话不得复用过期方案；30 轮自动循环与 4 小时稳定性测试通过。

---

## 实施顺序与依赖

以下为强依赖顺序。执行器、音频和协议生命周期不应在缺少冻结接口的情况下并行大改。

| 优先级 | 阶段/目标 | 排序理由 | 完成后解锁 |
|---|---|---|---|
| P0 | Phase 11 Group C：默认 Demo 洗涤流程 + 主洗涤/换位电机与真实执行器验收 | 产品核心闭环；先冻结 executor、安全门禁和按钮/手势/小程序入口语义 | 稳定的 program schema、planner 和真实执行能力 |
| P1 | BL-AUDIO-00：单 I2S 控制器音频架构重构 | 播放破坏采集或后端恢复会阻塞所有语音进阶能力 | 稳定提示音、TTS、双唤醒和 PTT 循环 |
| P2 | BL-AUDIO-01：MAX98357A 实物声学闭环 | 数字链路稳定后再隔离功放、PCB、strap 和扬声器问题 | 可听反馈和端到端语音体验 |
| P3 | BL-VOICE-01：ESP-SR MultiNet 离线命令 | 依赖稳定采集所有权；优先建立确定性的开始/停止/确认/取消 | 无网基础控制、智能方案确认入口 |
| P4 | BL-ADVISOR-01：AI 洗护顾问核心 | 依赖冻结的洗涤 schema、planner 和安全规则 | 自然语言需求到受约束的个性化洗涤方案 |
| P5 | 语音融合收尾 | MultiNet 负责确定性控制，AI 顾问负责开放需求，统一屏幕/TTS/小程序/审计 | Phase 11 完整智能交互闭环 |

### 推荐里程碑

1. **M11-C：Demo 洗涤可执行**——按钮、手势、小程序启动/终止一致，电机和执行器台架门禁通过。
2. **M11-D0：音频基础设施稳定**——BL-AUDIO-00 软件架构门禁与 BL-AUDIO-01 声学门禁分别通过。
3. **M11-D1：离线可控**——唤醒后 MultiNet 完成高频确定性命令，不依赖网络。
4. **M11-D2：智能规划**——用户描述衣物/场景，顾问生成受 schema 和本地规则约束的方案，确认后才能执行。
5. **M11-D3：融合验收**——双唤醒、离线命令、AI 顾问、屏幕、TTS、BLE/Wi-Fi 连续运行与长跑通过。

---

## Claude Code 分阶段执行提示词

### 使用方法

- 严格按 P0 → P5 顺序执行；每次只复制一个阶段的提示词。
- 若上一阶段对应 Gate 未 `PASS`，不得以“接口已存在”“编译通过”或“Fake HAL 通过”代替并跳到下一阶段。
- Claude Code 若能调用 skills，必须优先调用提示词列出的 skills；skill 不可用时必须在报告中写 `UNAVAILABLE`，并说明替代审查方法，禁止伪报调用。
- 所有阶段先读取仓库内 `AGENTS.md`、本文件、上一阶段报告以及 `git status --short`。工作树可能有用户改动，不得 reset、checkout、clean 或覆盖无关文件。
- 主工程：`D:/UserData/ESP32Project/IDF/ESP32-IDF-Test`。
- 旧音频参考工程：`D:/UserData/ESP32Project/ESP32-S3-Wroom1-DevKitC1_XIAOJING`。
- 硬件资料：`D:/UserData/ESP32Project/IDF/ESP32-IDF-Test/docs`。
- 默认板口：COM8。只有提示词明确允许且设备存在时才烧录；任何市电、高压、加热器、电机或真实执行器通电必须等待用户明确确认并由用户手动触发。

### 通用报告契约

每阶段最终报告至少包含：

```text
SKILLS_ACTUALLY_INVOKED
BASELINE_GIT_STATUS
FILES_CHANGED
ARCHITECTURE_DECISIONS
SAFETY_INVARIANTS
HOST_TESTS / UNIT_TESTS / PYTHON_TESTS / MINIAPP_TESTS
MAIN_BUILD / UNIT_BUILD
STATIC_SCAN
FLASH_RESULT（未烧录必须写 NOT_RUN）
PORT
BIN_SHA256 / ELF_SHA256
RESET / WDT / GURU / PANIC / ASSERT
INTERNAL_FREE_MIN / LARGEST_INTERNAL_BLOCK_MIN / PSRAM_MIN
CODE_REVIEW_P0 / P1 / P2
NEEDS_USER_* 项
本阶段 EXIT_GATE
下一阶段是否解锁及理由
```

报告必须把 `SOFTWARE_PASS`、`BOARD_DIGITAL_PASS`、`HARDWARE_ACOUSTIC_PASS`、`USER_ACCEPTANCE_PASS` 分开，不得互相替代。

---

### Prompt P0：M11-C 默认 Demo 洗涤与执行器闭环

```text
你现在执行 Phase 11 Group C 默认 Demo 洗涤闭环收尾。只做本阶段，不提前施工音频、MultiNet 或 AI 顾问。

必须先调用并实际使用：
1. /embedded-systems：FreeRTOS 生命周期、executor/planner、非阻塞执行器、安全关断；
2. /esp32：ESP32-S3 N16R8、PSRAM/internal DMA、GPIO/MCP/PWM/PCNT 资源审计；
3. /debugging-code：仅在真实错误、崩溃、状态机卡死或输出不一致时做根因定位；
4. /code-review（若存在）：完成后独立 P0/P1 审查。不可用则明确写 UNAVAILABLE，并用独立 agent 或结构化人工审查替代。

先读取：
- AGENTS.md
- docs/PHASE11_DEFERRED_DEVELOPMENT_BACKLOG.md
- Phase 11 Group A/B/C 最新报告、handover、resume manifest
- docs 下电机、MCP、执行器和主控接线资料
- git status --short

目标：冻结并闭环默认 Demo 程序语义：
订单受理 → UV（配置允许时）→ 启动前检查 → 0°归位 → 进水 → 洗涤剂 → 0°波轮预洗 → 停机/静止确认 → 90°滚筒主洗 → 停机/静止确认 → 180°排水/脱水 → 270°低速间歇翻转热风（配置允许时）→ 45°等待取衣 → UV OFF。

入口必须统一进入同一个 app request/planner/executor：BTN1 单击开始、BTN3 单击终止、PAJ 上挥开始、下挥终止、小程序下单/停止。禁止入口直接操作 GPIO/MCP/PWM。

要求：
- program_id + generation 去重，同一动作不得被多入口重复提交；
- 每次姿态切换前确认主洗涤电机停止且静止；
- cancel/abort/fault/timeout/emergency 全部 fail-safe，terminal exactly-once；
- UV、双阀、蠕动泵、排水、热风、电机全部只能经 service → safety_manager → HAL；
- 无 delay()、无定时器回调阻塞、ISR 只记事件；
- Fake HAL 覆盖正常流程、每阶段超时、传感器 stale、入口竞争、取消和急停；
- 建立默认程序的 machine-readable trace，证明步骤顺序及 OFF 确认；
- 先自动测试和 safe-OFF 被动板测。真实电机、高压和执行器通电必须停下等待用户逐项确认，绝不自动执行。

退出门禁：
- DEFAULT_DEMO_SCHEMA_FROZEN=PASS
- ALL_INPUTS_SINGLE_SUBMIT_PATH=PASS
- EXECUTOR_TERMINAL_EXACTLY_ONCE=PASS
- OUTPUT_BYPASS_COUNT=0
- HOST/UNIT/MAIN_BUILD=PASS
- 被动板测 30min：0 reset/WDT/Guru，所有输出保持 OFF
- 真实执行器未由用户验证时，HARDWARE_GATE 必须保持 NEEDS_USER，不得宣称完成。

输出报告到 docs/PHASE11_GROUP_C_DEFAULT_DEMO_CLOSURE_REPORT.md，并更新 resume manifest。只有 M11-C Gate 真正 PASS 才写 READY_FOR_BL_AUDIO_00=YES。
```

---

### Prompt P1：BL-AUDIO-00 单 I2S 控制器架构重构

```text
你现在执行 BL-AUDIO-00，只重构音频基础设施，不做 MAX98357A 损坏定性、不开发 MultiNet/AI 顾问、不改变生产洗涤语义。

必须实际调用：/embedded-systems、/esp32；遇到错速、DMA 失步、WDT、崩溃或死锁时调用 /debugging-code；完成后调用 /code-review（若可用）。

先读取：
- AGENTS.md 与 docs/PHASE11_DEFERRED_DEVELOPMENT_BACKLOG.md 的 BL-AUDIO-00 全文
- 当前 audio_driver/audio_controller/voice_frontend/voice_service/bootstrap/提示音/TTS 代码
- 旧工程 src/main.cpp、HardwareDriver、LocalAudio 中可工作的 I2S RX+TX 链路
- 当前 sdkconfig、map、启动 heap 日志和历史音频报告
- ESP-IDF 当前版本的官方 I2S channel API 头文件/本地文档

按 D.0.0-A~F 施工，不得大爆炸式重写：
A. 先输出审计表：两个工程的 controller、port、pins、sample rate、slot、DMA、任务、AMP enable、播放前后恢复差异；记录基线 SHA/heap/largest/PSRAM。
B. 创建单一 audio_controller：I2S0 paired RX/TX，在 Wi-Fi/BLE/模型前一次分配 DMA；运行期 channel delete/recreate=0；移除生产 I2S1 与 GPIO15/16 争用。
C. 固定 16kHz 硬件域；实现有界 24k→16k PCM 重采样，PSRAM 缓冲，mono 复制双 slot；不得继续把 24k PCM 直接以 16k 播放。
D. 实现 STOPPED/WAKE_CAPTURE/DIALOG_CAPTURE/PLAYBACK/RECOVERY 所有权状态机，generation 防陈旧恢复；AMP fail-safe；所有 terminal exactly-once。
E. 接入 Edge、ESP-SR、PTT/VAD、本地提示音和 MiMo TTS；业务组件不得直接碰 channel/GPIO。
F. 自动化与板级数字验证。

内存/实时约束：
- DMA 只用 MALLOC_CAP_DMA/internal；大 PCM、重采样和模型工作区优先 PSRAM；
- 记录每阶段 internal free/largest、PSRAM、任务栈 high-water；
- 不照搬旧工程 8×1024 大 DMA；用测量确定最小可靠配置；
- 回调/ISR/esp_timer 内不 malloc、不联网、不执行 classifier、不长时间持锁；
- LVGL single-writer 不得回归。

必须测试：
- 重采样长度、端点、饱和、DC、频率；
- owner 竞争、播放中切后端、PTT 抢占、取消、超时、陈旧 generation；
- Fake HAL 1000 次 capture↔playback；
- Edge/ESP-SR 播放后恢复；
- BLE+Wi-Fi+display+gesture 同时启动；
- 20 轮双后端数字闭环，随后 4h soak（若当轮时间不够，使用可恢复 checkpoint 继续，不能缩短后宣称 PASS）。

禁止：
- 为省事永久关闭 BLE/Wi-Fi/屏幕/任一唤醒后端；
- 用反复销毁 I2S 作为恢复机制；
- 把“有 BCLK/WS/DIN”写成扬声器声学 PASS；
- 未经用户确认自动播放高音量或操作任何高压执行器。

输出 docs/PHASE11_BL_AUDIO_00_SINGLE_I2S_REFACTOR_REPORT.md。仅当本文件定义的 7 个门禁全部通过才写 BL_AUDIO_00_GATE=PASS；声学门禁必须仍为 NEEDS_BL_AUDIO_01。
```

---

### Prompt P2：BL-AUDIO-01 MAX98357A 声学闭环

```text
你现在执行 BL-AUDIO-01。前置条件：BL-AUDIO-00_GATE=PASS；若未通过，停止并报告，不得绕开。

必须调用：/embedded-systems、/esp32、/debugging-code；若存在 /code-review 则完成后调用。先读取 MAX98357A 数据手册、docs/MAX98357_TEST、原理图/PCB/BOM资料、历史逻辑分析仪记录及 BL-AUDIO-00 报告。

目标是严格隔离：数字 I2S 配置问题、MAX98357A strap/焊接/PCB 问题、扬声器/连接问题。不要凭经验猜测。

施工与验证：
1. 增加编译期受控诊断模式：关闭云端/唤醒/RX consumer，只播放幅度受限 1kHz，左右 slot 写相同已知采样；生产构建默认关闭。
2. 输出 BCLK/WS/DIN 期望值和逻辑分析仪 I2S 解码步骤，验证采样值、slot、WS 极性、位宽、符号和一位延迟。
3. 对照原理图检查 DIN、BCLK、WS、SD_MODE、GAIN_SLOT、VDD、Pin1/BOM/网络连续性。
4. 提供安全示波器方法：Class-D BTL 禁止把普通地夹接 SPK±；必须差分探头或双通道 CH1-CH2。
5. 若用户可接外置已知正常 MAX98357A，做同源 A/B；没有硬件时保持 NEEDS_USER_AB_TEST。
6. 根因修复后验证 9 个提示音、旧素材、task_done、连续 5 次 MiMo TTS，以及播放后双唤醒恢复 20 轮。

不得自动要求用户操作市电；播放音量由低到高，先确认扬声器额定参数。普通万用表读不到 Class-D 差分不能直接定性芯片损坏。

输出 docs/PHASE11_BL_AUDIO_01_ACOUSTIC_CLOSURE_REPORT.md，给出 ROOT_CAUSE_CLASS、I2S_DECODE_EVIDENCE、BOARD_AB_RESULT、ACOUSTIC_RESULT。只有真实可闻且差分输出证据成立才写 HARDWARE_ACOUSTIC_GATE=PASS。
```

---

### Prompt P3：BL-VOICE-01 MultiNet 离线命令

```text
你现在执行 BL-VOICE-01。前置条件：BL-AUDIO-00=PASS；若播放提示反馈属于验收范围，则 BL-AUDIO-01 也必须 PASS，否则用屏幕反馈并诚实标注无声学验收。

必须调用：/embedded-systems、/esp32；识别状态机或音频切换异常时调用 /debugging-code；完成后做 /code-review（若可用）。只使用当前项目已有 ESP-SR 版本和本地 API，不凭记忆发明接口。

实现唤醒后的 MultiNet COMMAND 状态，第一版白名单：开始洗涤、停止洗涤、取消洗涤、查询状态、确认开始、取消方案、不要烘干、不要紫外、多漂洗一次、轻柔一点。

架构约束：
- WakeNet/Edge 唤醒 → audio_controller COMMAND owner → AFE/MultiNet → command_id → voice_command_router → app/planner/executor；
- command_id 不得直写 GPIO/MCP/PWM/电机/姿态；
- 明确命中本地命令不上传云端；未命中才转 MiMo AUTO_ROUTE；
- state-changing 命令进行 generation/program_id/权限/当前状态校验；
- CHAT_PRIORITY 不得执行控制；
- 超时、低置信度、冲突、陈旧确认一律 fail-closed 并恢复 WAKE；
- 支持 NVS 版本化命令配置，但不能让小程序写入任意底层动作。

测试：
- Host router/去重/过期/冲突/降级；
- 录音回放正样本、近音词、噪声、开放问句；
- 30min 误触发：危险误执行=0；
- 两唤醒后端各至少 15 轮 wake→command→wake；
- BLE/Wi-Fi/display 并存 4h soak；
- 所有取消/终止验证输出安全关断。

不要自动启动真实洗涤或执行器。板测先用 dry-run planner/屏幕/协议 trace；真实启动必须用户确认。

输出 docs/PHASE11_BL_VOICE_01_MULTINET_REPORT.md。只有离线命令、路由互斥、误触发和恢复门禁全部通过才写 BL_VOICE_01_GATE=PASS。
```

---

### Prompt P4：BL-ADVISOR-01 AI 智能洗护顾问

```text
你现在执行 BL-ADVISOR-01 核心。前置条件：默认 Demo schema/planner 已冻结；不得让模型直接生成执行器步骤。

必须调用：/embedded-systems（安全边界与状态机）、/esp32（资源/网络/PSRAM）、/debugging-code（JSON/并发/生命周期异常时）、/code-review（若可用）。如有适合 schema、安全评审或 UI 的 skill，可额外调用并在报告列出实际影响。

按 D.2.1~D.2.5 分轮实现，每轮独立 commit/checkpoint 和 Gate：
1. 严格 JSON Schema 的 WashProfile：衣物、面料、污渍、程度、护理优先级、机洗许可、混色风险、假设、置信度、是否追问。
2. 纯 C/可 host test 的 care_policy_engine：Profile → WashRecipe；规则版本化、确定性、数值裁剪、危险组合 fail-closed。
3. planner 编译 Recipe 到已冻结 Demo 骨架；姿态、停机、静止、排水、脱水、热风、UV 和关断只能由本地 planner 插入。
4. PLAN_PREVIEW/WAIT_CONFIRMATION 状态机；屏幕、小程序和语音展示简短方案。确认前输出通电次数=0。
5. 确认/取消/修改偏好/超时/陈旧 generation/双入口竞争验收。

第一版场景必须覆盖：羊毛围巾、运动衣、日常棉、重污棉、贴身衣物、未知面料。特别规则：羊毛不得被云端覆盖为高强度/高速/热风；UV 默认关闭；“打球”可推断汗液/异味但不能无证据断言泥污。

云端只负责需求理解；不得输出 GPIO、MCP、relay、PWM、角度、未受限时长。API 超时、401/402/429/5xx、JSON 损坏、低置信度、schema 失败均不得启动设备。API key 只从 NVS/小程序受控配置读取，日志/报告/源码不得泄漏。

测试：golden scenarios、adversarial prompts、schema fuzz、规则确定性、重复确认、并发入口、30 轮自动方案循环、4h soak。真实执行方案只在用户目视确认后测试。

输出 docs/PHASE11_BL_ADVISOR_01_REPORT.md。分别报告 PROFILE_GATE、POLICY_GATE、PLANNER_GATE、CONFIRMATION_GATE、USER_SCENARIO_GATE；任何子门禁未过则总 Gate=NOT_YET。
```

---

### Prompt P5：M11-D3 语音融合与最终验收

```text
你现在执行 Phase 11 M11-D3 融合收尾，不新增大型功能。前置条件：M11-C、BL-AUDIO-00、BL-VOICE-01、BL-ADVISOR-01 软件 Gate 均 PASS；BL-AUDIO-01 若未 PASS，必须把最终声学 Gate 标为 NOT_YET。

必须调用：/embedded-systems、/esp32、/debugging-code、/code-review（可用时），并对跨组件并发、资源和安全做最终独立审计。

统一路由：
- MultiNet：开始/停止/查询/确认/取消/偏好等确定性命令；
- AI 顾问：衣物、场景、污渍和开放式洗护需求；
- 普通闲聊：不生成 program；
- 低置信度/歧义：追问或保守降级，不执行；
- 所有实际 program 只经 app→planner→executor。

统一 UI/TTS/小程序状态：LISTENING、RECORDING、LOCAL_COMMAND、CLOUD_INFERENCE、PLAN_PREVIEW、WAIT_CONFIRMATION、EXECUTING、ERROR、RECOVERY。切换后端、PTT 打断、BLE/Wi-Fi 变化和云端失败不得卡死页面或音频 owner。

最终验收矩阵：
1. Edge 与 ESP-SR 各自唤醒、切换和重启恢复；
2. 本地命令与云端顾问不会双提交；
3. PTT 可以打断上一轮回答并启动新会话；
4. 无网时本地命令可用，顾问明确提示离线；
5. BLE 配网/改 key 不破坏唤醒和屏幕；
6. 方案确认前零输出，确认后 program_id 唯一；
7. cancel/abort/fault/emergency 全链路安全关断；
8. 100 轮混合自动/回放循环；
9. 用户完成至少 10 轮真实双唤醒与 6 类顾问场景；
10. 最终 8h soak：BLE+Wi-Fi+display+gesture+voice 全开，0 reset/WDT/Guru/Panic/Assert、无单调 heap 泄漏、心跳/I2S/遥测持续。

禁止在 soak 中自动给电机、高压、UV、阀、泵、热风通电；使用 dry-run/FAKE program 或保持执行器 safe-OFF。真实动作只由用户单独门控。

输出 docs/PHASE11_M11_D3_FINAL_INTEGRATION_REPORT.md，并生成可恢复 manifest、固件 SHA、测试日志索引和剩余硬件验收清单。只有全部必要 Gate 有真实证据才写 READY_FOR_PHASE12=YES。
```

---

### Overnight Prompt：无人值守软件推进与 DevKitC-1 安全长跑

```text
你现在进入 Phase 11 夜间无人值守软件推进模式。用户即将离开电脑并睡觉，预计给你 4～6 小时。目标是在完全不需要用户交互、不自动驱动真实执行器的前提下，尽可能推进软件开发、自动化验证和安全长跑，让用户下次接外设时可以直接进入烧录/实物验收。

当前电脑连接了一块 ESP32-S3-DevKitC-1 开发板。它仅用于安全固件启动、Wi-Fi/BLE/内存/任务生命周期和被动 soak；不得假定板上连接了本项目的 MCP23017、屏幕、麦克风、功放、传感器、电机、阀、泵、UV 或热风模块。

## 0. 必须先做的事

必须实际调用并使用：
- /embedded-systems：FreeRTOS 生命周期、DMA、同步、bounded stop、栈/heap 和长跑设计；
- /esp32：ESP32-S3 N16R8、PSRAM、internal DMA、Wi-Fi/BLE 共存、I2S 和安全 GPIO；
- /debugging-code：仅当出现崩溃、WDT、死锁、错误状态或测试不一致时进行根因定位；
- /code-review：若存在则在每个重大阶段结束后调用；若不可用，报告 `UNAVAILABLE`，使用独立 agent 或结构化 P0/P1 审查替代。

完整读取：
1. 仓库根目录 AGENTS.md；
2. docs/PHASE11_DEFERRED_DEVELOPMENT_BACKLOG.md 全文，尤其“实施顺序与依赖”和 Prompt P0～P5；
3. docs 下最新 Phase 11 Group A/B/C/D 报告、HANDOVER、RESUME_MANIFEST、checkpoint；
4. git status --short、最近提交和当前未提交差异；
5. 当前 sdkconfig、sdkconfig.defaults、分区表、CMake 依赖和 build map；
6. 旧参考工程 D:/UserData/ESP32Project/ESP32-S3-Wroom1-DevKitC1_XIAOJING 中的可用音频 RX+TX 实现；
7. D:/UserData/ESP32Project/IDF/ESP32-IDF-Test/docs 中与本轮组件相关的数据手册。

不得 reset/checkout/clean 用户改动，不得删除未跟踪文件，不得修改或提交真实 API key。先记录 baseline SHA256 和脏工作树清单，建立：

phase11_overnight/
  checkpoints/
  logs/
  manifests/
  FINAL_REPORT.md

每完成一个子阶段立即写 checkpoint，确保中断后可恢复，不要等最后才写报告。

## 1. 决定从哪里开始

根据现有报告、代码和测试证据，找到 P0→P5 中“最早尚未通过的软件 Gate”。

- 已有真实 PASS 证据的工作不得重做或降级；
- 只有接口/占位代码/编译通过不算完成；
- HARDWARE、ACOUSTIC、USER_ACCEPTANCE Gate 不得在无人值守期间改为 PASS；
- 如果 P0 软件 Gate 已通过，从 P1 BL-AUDIO-00 开始；
- 如果 P1 在夜间真正通过，可继续 P2 的纯软件/数字诊断准备，但不能宣称声学通过；
- P3 只有在 P1 软件 Gate 通过后才能开始；
- P4 只有默认 wash schema/planner 已冻结后才能开始；
- 不为了“多完成阶段”而跳过测试、降低门禁或堆叠未经验证的大改。

开始时在日志中写：
`OVERNIGHT_START_STAGE=<stage>` 与选择依据。

## 2. 夜间优先施工顺序

### 第一优先：修复/完成最早未通过的软件 Gate

直接采用本文对应 Prompt P0～P5 的范围、架构和退出门禁。每次只推进一个可独立验证的小增量：审计 → 测试先行 → 实现 → 回归 → code review → checkpoint。

### 第二优先：扩大自动化验证

对已经实现的组件补齐：
- Host 纯 C 状态机测试；
- Fake HAL 故障注入、超时、取消、emergency、重复请求、陈旧 generation；
- 1000～10000 轮确定性压力测试；
- Python 协议/JSON/schema/日志解析测试；
- 小程序协议测试（不得启动微信 GUI 等待人工）；
- static scan：GPIO/MCP/I2S/AMP/执行器旁路、LVGL single-writer、API key 泄漏、阻塞 timer callback、运行期 I2S recreate；
- 主固件和 unit firmware 构建，0 error，新增 warning=0。

### 第三优先：DevKitC-1 安全板测

先枚举串口并确认目标确实为 ESP32-S3；不要只凭 COM 编号。不得 erase-flash。

为裸 DevKit 创建或使用独立的 SAFE/FAKE/DEVKIT 构建配置和独立 build 目录：
- 所有外部执行器输出必须编译期禁用或 Fake HAL；
- MCP23017/屏幕/传感器/音频外设缺失必须是可诊断降级，不能形成重启循环；
- 禁止使用 REAL actuator mode；
- 不得覆盖生产 REAL 配置或改变用户下次实物板烧录的默认配置；
- 如无法证明当前构建为 safe mode，则只构建、不烧录。

只有同时满足以下条件才允许自动烧录 DevKit：
1. 芯片识别为 ESP32-S3；
2. safe/devkit 配置中所有 actuator/high-voltage outputs 明确 disabled/fake；
3. build PASS；
4. flash 命令不会擦除 NVS/整片 Flash；
5. 串口未被其它监视器占用，且可安全关闭本项目遗留监视进程。

烧录后只做：冷启动、Wi-Fi/BLE（若凭据可用）、任务生命周期、heap/PSRAM、协议 dry-run、音频数字计数和显示服务缺失降级。不要播放高音量，不要自动运行洗涤程序，不要触发任何 GPIO 执行器。

## 3. 长跑策略

完成本轮可验证的软件增量后，利用剩余时间运行 4 小时目标 soak；如果开发耗时导致不足 4 小时，则运行到本次会话可用时间结束，并诚实记录实际秒数，绝不把短跑写成 4h PASS。

soak 模式：
- 裸 DevKit safe/FAKE 固件；
- 执行器通电次数必须为 0；
- 可进行 dry-run/FAKE 状态机循环；
- Wi-Fi + BLE 同时开启（若网络凭据有效）；
- 音频外设不存在时只验证任务降级和数字 owner 状态，不制造假 PCM 来声称声学通过；
- 每 60 秒输出一条紧凑诊断快照，不得在 timer callback 中执行阻塞采集，使用专用低优先级 task；
- 监控 heartbeat、reset reason、WDT、Guru/Panic/Assert、internal free、largest block、PSRAM、任务栈 high-water、I2S/协议计数和错误累计。

失败立即：保存完整串口日志和 backtrace → addr2line → 调用 /debugging-code → 修复 → 全量回归 → 重新开始一轮新的连续 soak。不得把失败前后的片段拼接成一个连续 PASS。

不要使用单个阻塞 wait 超过 60 秒；后台运行并周期性检查。不要因串口暂时无输出就判断死机，必须结合 heartbeat、端口状态和 reset token。

## 4. 自动停止边界

遇到以下情况停止对应硬件动作，但继续做其它安全的软件工作：
- 需要用户说唤醒词、观察屏幕、听扬声器或操作小程序；
- 需要连接/移动传感器、磁铁、水路或外设；
- 需要真实电机、阀、泵、UV、继电器、220V、热风或加热器通电；
- 需要用户选择会改变产品语义的方案；
- 无法确认所连板卡身份或 safe build；
- 密钥失效、余额不足或外部云服务不可用。

这些项目写入 `NEEDS_USER_*`，不得反复等待用户，也不得阻塞整夜；转去完成 host 测试、文档、审查或 safe soak。

## 5. 代码质量与安全硬规则

- 无 `delay()`；ISR 短小，只记录事件；timer callback 不阻塞；
- bounded queue、bounded retry、bounded stop；stop timeout 保留资源可重试，禁止 UAF；
- terminal exactly-once；取消/故障/emergency fail-safe；
- DMA/internal 与 PSRAM 分配能力明确；记录内存预算和 stack high-water；
- 不通过减配/永久关闭 BLE、Wi-Fi、屏幕、Edge 或 ESP-SR 来伪造资源通过；
- 不把 unknown 当 off，不把命令态当物理反馈，不把 dry-run 当硬件 PASS；
- 不泄漏 Wi-Fi 密码、API key、Authorization、音频 base64；
- 所有新增诊断生产默认关闭，或有明确发布开关；
- 不自行 git push；只有用户明确要求时才提交。可以生成建议 commit 列表，但保持用户工作树可审查。

## 6. 最终交付

结束前更新 phase11_overnight/FINAL_REPORT.md，并在对应 Phase 11 正式报告中追加本轮证据，不覆盖历史失败记录。最终摘要必须包含通用报告契约全部字段，另外必须给出：

OVERNIGHT_START_STAGE
OVERNIGHT_END_STAGE
SOFTWARE_STAGES_COMPLETED
STAGES_PARTIAL
SOAK_DURATION_SECONDS
SOAK_CONTINUOUS_BOOT_ID
DEVKIT_IDENTITY_EVIDENCE
DEVKIT_SAFE_MODE_PROOF
ACTUATOR_ON_COUNT=0
RESET_BY_REASON
FIRST_FAILURE_ROOT_CAUSE（如有）
CHECKPOINT_PATH
NEXT_USER_HARDWARE_STEPS（按接线/测试顺序列出）
READY_TO_FLASH_REAL_BOARD=YES/NO（必须说明依据）

最后只根据证据设置各 Gate。目标是在用户回来时得到“可继续烧录/实物验收的软件基线 + 可恢复 checkpoint + 长跑证据”，不是追求虚假的全部完成。
```

实施前必须重新审计当前代码，不能把本文中的接口名称视为已经存在。
