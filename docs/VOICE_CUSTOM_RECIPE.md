# 语音需求到本地自定义演示流程

本版接通：MiMo v2.6-flash → generate_wash_pipeline 工具参数 → 严格解析 →
wash_recipe_validate/compile → program_control 仲裁 → wash_executor → 原有服务与安全管理器。
TTS 保留 v2.5-tts。模型不接触 GPIO、PWM 输出或 HAL。

## 当前硬件范围

沿用未上水、空桶比赛演示配置，不代表已支持真实衣物洗涤或按升计量。
固定本地前后处理不由模型决定：到45°装衣位 → 等待按钮1确认 → 到0° →
GPA0开5秒并关闭 → GPA1与GPB7洗衣液泵各运行5秒并关闭 → 自定义动作 →
回45° → 确认输出关闭。水量校验仍仅在这一演示通道豁免，正式水服务不变。
冗余 GPB5 主轴 HOME 仍按此前设置跳过，五个桶姿态霍尔、安全闩锁、温度与超时检查保留。

## 工具契约

强制 tool_choice.function.name=generate_wash_pipeline；max_completion_tokens=1024；thinking disabled。
根对象沿用 domain/transcript/confidence_milli/requires_confirmation/tool/speech_reply/
updated_preferences/pipeline；start_program 使用 program=custom，显式提供 allow_uv/allow_dry。
updated_preferences 仅作为结构化字符串接收，本版不把模型文本写入持久偏好。

pipeline 最多8项，每项严格只有 cmd、val、time，禁止重复字段、负数、浮点数、布尔/字符串数字。

| cmd | val | time（秒） | 本地行为 |
| --- | --- | --- | --- |
| CMD_WATER_IN | 0 | 5 | 可省略；仅首项一次，本地固定执行两段阀泵动作，不重复加水 |
| CMD_TILT_BARREL | 0/45/90/180/270 | 0 | 受控姿态换位；通常无需模型显式插入 |
| CMD_WASH_PULSATOR | 强度1/2/3 | 5–30 | 本地映射20/30/40% PWM，0°交替 |
| CMD_WASH_DRUM | 强度1/2/3 | 5–30 | 本地映射15/20/30% PWM，90°交替 |
| CMD_SPIN_DRY | 强度1/2/3 | 5–12 | 本地映射40/60/80% PWM，180°单向 |
| CMD_DRY | 1 | 5–15 | 270°受控热风+既有40% PWM间歇翻动，必须 allow_dry |
| CMD_UV_STERILIZE | 1 | 1–15 | 示意灯等待阶段，必须 allow_uv；allow_uv=true时整个组合阶段亮灯 |

至少一个洗涤动作；脱水、烘干只能在洗涤后，各最多一次，不允许其后再次洗涤。
估算动作预算≤176秒，组合执行器硬时限180秒；人为延迟霍尔触发仍可能触发时限。
CMD_TURBIDITY_CHECK 没有对应可执行流程服务，本版不向模型宣告此命令；浊度遥测继续工作。

## 确认与反馈

方案建议、低置信度、按钮2聊天优先输入均只暂存一个候选方案，90秒过期。
按按钮1（或上挥）确认候选后到45°；装好再按按钮1，才开始组合动作。
明确开始且本地授权通过的唤醒请求可直接前往装衣位，仍等待物理装衣确认。
confirm_plan 只能接受本地已有且未过期的候选；无候选、过期均不回退成默认演示。
停止、下挥、急停清除候选并保留原有取消与输出关闭保护。

屏幕与TTS使用本地已校验方案摘要；拒绝后不复述模型“已开始”的承诺。
普通 content 和 JSON对象/单元素数组只提取正文，不执行工具；结构化 content 若包含
start_program/confirm_plan，则明确提示未收到完整工具调用。JSON字段名和括号不进入TTS。
可选TTS失败不回滚已接受的方案，也不误报主模型失败。

## 调试面板与速度

小程序 simple-controls-20261031-r2：仅展示UV、源水/转移阀、蠕动泵、搅动交替/单向、
五角度换位、开机固定语音与速度保存。移除热风、排水、自动负向测试和连续换向入口。
诊断 ON 继续使用原有3秒UV限时；阀泵时长读取已有benchtst配置，当前默认1.5秒，
界面与确认弹窗显示配置值，受理后以ACK实际duration_ms回显，不再硬编码泵0.8秒。
统一停止只提交关闭/取消，不宣称已物理关闭。
play_prompt 只允许 boot，排队播放已固化的开机音频，不调用云端合成。

自动换位原默认30% PWM，独立测试原默认80%。新字段 position_move_pwm 暴露现有位置参数，
小程序换位滑杆同时下发自动/测试速度，新值在下次动作生效。
保持已有 NVS 与方向标定；不批量恢复默认。速度是占空比，非转速反馈。
拖动预览不下发，松开临时应用 persist=false，点击保存 persist=true 才异步写 NVS。
确认ACK只表示保存受理，实际保存日志与重启读回用于验证持久化。
修复了秒/毫秒混用、uint8转换前未校验导致的PWM回绕、无效JSON值被忽略、未检查ACK就显示成功等问题。

## 验证

- host_tests 新增 voice_recipe_tests：严格解析、边界/顺序、负向输入、候选授权、
  值拷贝、编译后45°装衣等待和过期代际确认拒绝；现有回归继续运行。
- 小程序 tools/test_simple_controls.js：实际速度回显、临时应用与持久保存分离、单位换算、
  保存拒绝不显示成功、精简入口与统一停止。
- 真实云端文本请求仅验证工具协议与本地编译，不连接硬件执行器；不等价于麦克风真机闭环。
- 真机验收：说“给我轻柔波轮洗20秒方案，不脱水不烘干不开UV”，检查候选无输出；
  按钮1到45°后再按按钮1，按目标提示人工触发霍尔；按钮2/下挥可停止。
- 真机速度保存后重启读回；固定语音测试只播开机音频。

真机捕获到启动时main任务3584字节栈在同步开机播报路径溢出。开机播报已改为
投递已有4096字节notice worker队列，main不再承载音频深调用；worker记录栈余量。
不修改sdkconfig，不增加主任务栈，不新增播报任务。
