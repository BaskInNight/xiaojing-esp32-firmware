# program_control — 程序控制（多输入仲裁 + DEFAULT_DEMO_V1 演示流程）

## 职责

所有洗涤/急停输入（BTN1 / BTN3 / PAJ 手势 / mini-app BLE）统一经本组件仲裁后
才接触 `wash_executor`，保证优先级、互斥与 fail-closed。

- `program_control_core.c`：纯 C 模型，host-testable。
  - DEFAULT_DEMO_V1 26 阶段规范表 + 编译函数（位置可达性 + 系统 MOVE 插入）。
  - 输出审计纯谓词（UV-off / 全危险输出 safe-off，UNKNOWN 一律视为未确认 OFF）。
  - 仲裁：EMERGENCY > FAULT > STOP > START > backend switch。
  - BTN1 短按→START / 长按→后端切换（互斥）。
  - 手势 UP→START / DOWN→STOP / 左右→翻页（冷却 + 重武装 + 尾串抑制）。
- `program_control_service.c`：运行时接线（executor 提交/取消/中止、审计 sink、
  手势/按钮入口、每拍 refresh）。

## 安全模型

- 绝不自动给电机/热风通电：START 只提交程序，具体动作由各服务经
  `safety_manager` 门禁执行；急停由 app_bootstrap 注入的 emergency 回调走
  `safety_manager_emergency_stop`。
- 审计失败 fail-closed → executor 进入故障。
- 不直接访问 HAL/GPIO/NVS。
