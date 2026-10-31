# Phase 6 Group B — BL50 架构差距审计

> 日期: 2026-07-25
> 状态: AUDIT_COMPLETE
> 下一步: 实现 bl50_service 组件

## 1. 现状总览

| 组件 | 状态 | 说明 |
|---|---|---|
| bl50_driver | DONE (Phase 2) | GPIO/LEDC 硬件驱动，无业务逻辑 |
| bl50_service | **NOT_EXIST** | 业务层服务，本次任务创建 |
| hal_contract BL50 | DONE | set_bl50/stop_bl50 已定义 |
| fake_hal BL50 | PARTIAL | 有历史记录+bl50_error注入，无 home 传感器模拟 |
| real_hal BL50 | DONE | GPIO/LEDC 实现 |
| safety_manager BL50 | DONE (Phase 4) | SAFETY_OP_BL50 联锁已实现 |
| machine_config BL50 | **NOT_EXIST** | 无 bl50_params_t |
| position bridge | DONE | 每 10ms 更新 safety_manager_update_position |
| app_bootstrap BL50 | **NOT_EXIST** | 无 bl50_service lifecycle |

## 2. bl50_driver 当前接口

**文件:** `components/bl50_driver/include/bl50_driver.h`

```c
bl50_handle_t *bl50_driver_init(void);
void bl50_driver_destroy(bl50_handle_t *handle);
esp_err_t bl50_driver_run_cw(bl50_handle_t *handle, uint8_t pwm_percent);
esp_err_t bl50_driver_run_ccw(bl50_handle_t *handle, uint8_t pwm_percent);
esp_err_t bl50_driver_brake(bl50_handle_t *handle);
esp_err_t bl50_driver_stop(bl50_handle_t *handle);
esp_err_t bl50_driver_emergency_stop(bl50_handle_t *handle);
bl50_state_t bl50_driver_get_state(bl50_handle_t *handle);
uint8_t bl50_driver_get_pwm(bl50_handle_t *handle);
```

**GPIO 映射:**
- DIR = GPIO 1
- BRAKE = GPIO 2
- PWM = GPIO 39 (LEDC timer 1, channel 1, 5kHz)
- STOP = GPIO 21

**评估:** bl50_driver 接口足够。它只做硬件命令，不感知产品含义（0/45/90/180/270）。bl50_service 通过 HAL 函数表间接调用，不直接访问 GPIO。

## 3. xiaojing_hal_t BL50 接口

**文件:** `components/hal_contract/include/xiaojing_hal.h`

```c
esp_err_t (*set_bl50)(bl50_command_t command);  // {BL50_CMD_STOP/RUN_CW/RUN_CCW/BRAKE, pwm_percent}
esp_err_t (*stop_bl50)(void);                    // 等价于 set_bl50({STOP, 0})
```

**评估:** HAL 接口完整。bl50_service 必须通过 safety_manager_apply(SAFETY_OP_BL50, ...) 间接触达，不得直接调用 hal->set_bl50/stop_bl50。

## 4. Fake HAL BL50 现状与缺口

**文件:** `components/fake_hal/include/fake_hal.h`

已有:
- `fake_fault_config_t.bl50_error` — 故障注入（set_bl50 返回指定错误）
- 输出历史: `FAKE_OUTPUT_SET_BL50` / `FAKE_OUTPUT_STOP_BL50`
- 时间控制: `fake_hal_advance_time/set_time`

**缺口:**
- 无 GPB5 home 传感器模拟（GPB5 通过 MCP gpio_b 读取，fake_hal_set_mcp 可设置）
- 无 bl50 stop 故障注入（stop_bl50_error 未实现，对比 stop_ibt2_error）
- 无 BL50 状态查询 mock（bl50_driver_get_state/pwm 不通过 HAL 函数表）

**修复方向:**
- fake_fault_config_t 新增 `stop_bl50_error` 字段
- GPB5 通过 mcp_input_snapshot_t.gpio_b 模拟（已可用，bit5 = GPB5）
- bl50_service 使用 HAL 函数表，不直接查 bl50_driver 状态

## 5. safety_manager SAFETY_OP_BL50

**文件:** `components/safety_manager/safety_manager.c`

当前实现 (apply_bl50):
- enable=true: 构造 bl50_command_t(RUN_CW/RUN_CCW, pwm), 调用 hal->set_bl50
- enable=false: 调用 hal->stop_bl50

联锁规则 (safety_interlocks.c):
1. safety_all_mcp_known_off() — 所有 MCP 输出必须 KNOWN_OFF
2. check_position_fresh_and_known() — 姿态 fresh/stable/known
3. required_position 有效且非 UNKNOWN
4. current position == required_position
5. position motor not moving
6. OFF 请求始终允许

**评估:** 联锁规则基本正确。bl50_service 将通过 safety_manager_apply 调用。

**缺口:**
- BL50 运行状态未回写 safety inputs（BL50_RUNNING 应阻止姿态电机启动）
- 需新增 `safety_inputs_t.bl50_running` 字段
- POSITION_MOTOR ON 应检查 BL50 未运行

## 6. ACTUATOR_MIGRATION_PENDING 状态

Phase 4 设计中已标记 BL50 为 ACTUATOR_MIGRATION_PENDING：
- "BL50/风扇迁移策略: 当前无业务组件直接调用 BL50/fan HAL (仅测试)。water_service/dry_service 实现时再迁移。"

本次 Group B 完成 BL50 迁移：
- bl50_service 通过 safety_manager_apply 间接调用 HAL
- BL50 不再有"仅测试"的直接 HAL 调用
- bypass scanner 将捕获 bl50_service 内的直接 HAL 调用

**完成后待更新:**
- safety_bypass_scan 白名单移除 BL50 相关直接调用标记
- 位置: bl50_driver/real_hal/fake_hal/safety_manager 为白名单

## 7. GPB5 BL50 Home 输入路径

**GPIO:** MCP GPB5 (XIAOJING_MCP_GPB5_HALL_BLDC)

**读取路径:**
```
mcp23017_driver → I2C read GPIOB → mcp_input_snapshot_t.gpio_b bit 5
    → hal->read_mcp_inputs() → bl50_service 读取
```

**中断路径:** INTB GPIO 13 → MCP GPB5 变化 → INTCAPB bit 5

**评估:**
- GPB5 输入已在 MCP 读取范围内，无需额外驱动
- bl50_service 在 HOMING 状态轮询 mcp_input_snapshot_t.gpio_b & (1<<5)
- 消抖: 虚拟时间消抖（fake_hal）或真实时间消抖（real）
- 极性: 默认低电平触发，由 board_config 冻结，不在业务层写死

## 8. Position Freshness/Stability Bridge

**当前位置→安全桥接:** position_service.c 每 10ms 调用:
```c
safety_manager_update_position(feed_position, feed_stable, feed_moving,
                               feed_valid, sample_ms);
```

**bl50_service 需要:**
- 读取 position_view: `safety_manager_get_position_view(&view)`
- view.position == required_position 检查
- view.fresh && view.stable 检查
- view.motor_moving == false 检查

**反馈需求:**
- BL50 运行时: `safety_manager_update_bl50_running(true)`
- BL50 停止时: `safety_manager_update_bl50_running(false)`

## 9. machine_config BL50 参数

**现有:** 无 bl50_params_t

**需新增 (bl50_params_t):**
```c
typedef struct {
    uint8_t home_pwm_percent;           /* 寻零低速, 5-30 */
    uint32_t home_timeout_ms;           /* 寻零超时, 1000-30000 */
    uint8_t ramp_step_percent;          /* 斜坡步进, 1-10 */
    uint32_t ramp_interval_ms;          /* 斜坡间隔, 10-500 */
    uint32_t reversal_stop_ms;          /* 反转前停止时间, 100-2000 */
    uint8_t pulsator_default_pwm;       /* 波轮默认 PWM, 10-100 */
    uint32_t pulsator_direction_interval_ms;  /* 波轮换向间隔, 1000-60000 */
    uint8_t drum_default_pwm;           /* 滚筒默认 PWM, 10-100 */
    uint32_t drum_direction_interval_ms;      /* 滚筒换向间隔, 1000-60000 */
    uint8_t spin_default_pwm;           /* 脱水默认 PWM, 10-100 */
    uint32_t max_action_duration_ms;    /* 单动作最大时长, 10000-3600000 */
    bool direction_calibrated;          /* 方向已标定 */
    bool home_direction_cw;             /* 寻零方向, true=CW */
} bl50_params_t;
```

**默认值:** FAKE 可运动（direction_calibrated=false 不阻止 FAKE 模式运动）

## 10. machine_event_t BL50_DONE

已存在: `MACHINE_EVENT_BL50_DONE` (machine_types.h index 3)

事件结构:
```c
machine_event_t {
    type = MACHINE_EVENT_BL50_DONE,
    request_id = 原请求 ID,
    result = SERVICE_RESULT_OK/CANCELED/TIMEOUT/REJECTED/FAULT,
    fault = {FAULT_BL50, severity, detail} // 如有故障,
    value = 0
}
```

## 11. app_bootstrap 生命周期

**需新增:**
```c
bool bl50_svc_ok;  // app_bootstrap_result_t 新增字段
```

**初始化顺序:**
1. NVS → Config → Status Store → HAL
2. position_service_global_init → init → start
3. safety_manager_init → start
4. **bl50_service_init → start** (Group B 新增)
5. water_service_init → start
6. dry_service_init → start
7. event consumer

**清理顺序 (逆序):**
dry → water → **bl50** → safety → position → HAL

## 12. stop Hook 容量与注销

safety_manager:
- 最多 8 个 stop hooks (SAFETY_MAX_STOP_HOOKS)
- 当前已用: position_service + water_service + dry_service = 3
- bl50_service 注册 1 个 → 总计 4 个，未超限
- 注销 API: safety_manager_unregister_stop_hook(fn, context) 已实现

## 13. 现有测试缺口

**已有 [hw][bl50] 测试 (3 个):**
- bl50 lifecycle [hw]
- bl50 run(0) equals STOP [hw]
- bl50 pwm>100 rejected [hw]

**缺失 (需新增):**
- bl50_service FSM 全部状态转换
- bl50_service safety gateway 集成
- bl50_service lifecycle (init/start/stop/restart)
- bl50_service cancel/emergency
- bl50 HOME 传感器逻辑
- bl50 方向反转序列
- bl50 斜坡升降速
- bl50 direction_calibrated 门控
- fake_hal BL50 stop 故障注入
- safety_manager BL50 运行状态回写

## 14. 真实硬件方向和极性待标定项

| 项目 | 待标定 | 说明 |
|---|---|---|
| BL50 DIR 高/低 = CW/CCW | 是 | 需实物验证 |
| BL50 GPB5 传感器极性 | 低电平有效(默认) | 需实物验证，board_config 冻结 |
| BL50 HOME 寻零方向 | CW 或 CCW | 需实物标定，配置项冻结 |
| BL50 PWM 频率 5kHz | 已知 | BL50 标称 1-10kHz |
| BL50 PWM 占空比范围 | 5-100% | 0%=停止 |
| BRAKE 引脚极性 | 高有效(默认) | 需实物验证 |
| STOP 引脚极性 | 高有效(默认) | 需实物验证 |

**REAL 模式门控:** direction_calibrated=false 时，所有 RUN/HOME 拒绝。STOP 始终允许。

## 15. 组件所有权声明

### bl50_service (新建)
- BL50 正常运行的唯一业务所有者
- 不直接访问 GPIO/LEDC
- 不绕过 safety_manager
- 不控制姿态电机
- 通过 safety_manager_apply 间接调用 HAL

### safety_manager (已有)
- 所有 BL50 RUN/BRAKE/STOP 的运行时授权入口
- emergency STOP 通道
- STOP/OFF 不受姿态 stale 阻止
- RUN 必须检查姿态和其他危险输出

### bl50_driver (已有)
- 只实现硬件命令
- 不包含洗涤业务流程
- 不感知 0/45/90/180/270 的产品含义

## 16. 实现计划摘要

| 任务 | 文件 | 说明 |
|---|---|---|
| 公共接口 | bl50_service.h | action/request/snapshot 类型 + 7 个 API |
| 纯 C FSM | bl50_fsm.c + bl50_fsm.h | 13 个状态，无 FreeRTOS 依赖 |
| 服务层 | bl50_service.c | FreeRTOS task + queue + lifecycle |
| 配置 | machine_config 扩展 | bl50_params_t + Kconfig |
| Safety 扩展 | safety_contract/safety_manager | bl50_running 回写 |
| Fake HAL 扩展 | fake_hal | stop_bl50_error + home 传感器 |
| Bootstrap | app_bootstrap.c | init/start/stop |
| 测试 | test_runner.c | 32 个新测试 |
