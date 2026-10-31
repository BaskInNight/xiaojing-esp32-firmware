# ESP-IDF 工程文件架构

工具链固定 ESP-IDF 5.5.4，工程名建议 `xiaojing_idf`。

## 1. 目标目录树

Claude Code 应逐阶段创建下列文件，不要求 Phase 0 一次生成全部空壳。

```text
ESP32-IDF-Test/
├── CLAUDE.md
├── README.md
├── CMakeLists.txt
├── sdkconfig.defaults
├── partitions.csv
├── dependencies.lock
├── main/
│   ├── CMakeLists.txt
│   ├── app_main.c
│   ├── app_bootstrap.c
│   └── include/app_bootstrap.h
├── components/
│   ├── machine_types/
│   │   ├── CMakeLists.txt
│   │   └── include/machine_types.h
│   ├── board_config/
│   │   ├── CMakeLists.txt
│   │   ├── Kconfig
│   │   ├── board_config.c
│   │   └── include/board_config.h
│   ├── machine_config/
│   │   ├── CMakeLists.txt
│   │   ├── machine_config.c
│   │   └── include/machine_config.h
│   ├── machine_status_store/
│   │   ├── CMakeLists.txt
│   │   ├── machine_status_store.c
│   │   └── include/machine_status_store.h
│   ├── safety_contract/
│   │   ├── CMakeLists.txt
│   │   └── include/safety_contract.h
│   ├── hal_contract/
│   │   ├── CMakeLists.txt
│   │   └── include/xiaojing_hal.h
│   ├── fake_hal/
│   │   ├── CMakeLists.txt
│   │   ├── fake_hal.c
│   │   └── include/fake_hal.h
│   ├── i2c_bus_manager/
│   │   ├── CMakeLists.txt
│   │   ├── i2c_bus_manager.c
│   │   └── include/i2c_bus_manager.h
│   ├── mcp23017_driver/
│   │   ├── CMakeLists.txt
│   │   ├── mcp23017_driver.c
│   │   └── include/mcp23017_driver.h
│   ├── ibt2_driver/
│   ├── bl50_driver/
│   ├── fan_driver/
│   ├── flow_meter/
│   ├── water_level_sensor/
│   ├── turbidity_sensor/
│   ├── sht_sensor/
│   ├── paj7620_adapter/
│   ├── audio_driver/
│   ├── position_service/
│   │   ├── CMakeLists.txt
│   │   ├── position_service.c
│   │   ├── position_debounce.c
│   │   ├── include/position_service.h
│   │   └── test/test_position_service.c
│   ├── water_service/
│   │   ├── CMakeLists.txt
│   │   ├── water_service.c
│   │   ├── include/water_service.h
│   │   └── test/test_water_service.c
│   ├── dry_service/
│   │   ├── CMakeLists.txt
│   │   ├── dry_service.c
│   │   ├── include/dry_service.h
│   │   └── test/test_dry_service.c
│   ├── bl50_service/
│   ├── detergent_service/
│   ├── drain_service/
│   ├── uv_service/
│   ├── safety_manager/
│   │   ├── CMakeLists.txt
│   │   ├── safety_manager.c
│   │   ├── safety_interlocks.c
│   │   ├── include/safety_manager.h
│   │   └── test/test_safety_manager.c
│   ├── wash_planner/
│   │   ├── CMakeLists.txt
│   │   ├── wash_planner.c
│   │   ├── wash_profiles.c
│   │   ├── include/wash_planner.h
│   │   └── test/test_wash_planner.c
│   ├── wash_executor/
│   │   ├── CMakeLists.txt
│   │   ├── wash_executor.c
│   │   ├── wash_step_dispatch.c
│   │   ├── include/wash_executor.h
│   │   └── test/test_wash_executor.c
│   ├── app_protocol/
│   │   ├── CMakeLists.txt
│   │   ├── app_protocol.c
│   │   ├── app_json_codec.c
│   │   ├── legacy_protocol.c
│   │   ├── include/app_protocol.h
│   │   └── test/test_app_protocol.c
│   ├── ble_transport/
│   │   ├── CMakeLists.txt
│   │   ├── ble_transport.c
│   │   ├── ble_reassembly.c
│   │   ├── include/ble_transport.h
│   │   └── test/test_ble_reassembly.c
│   ├── button_service/
│   ├── voice_agent/
│   │   ├── CMakeLists.txt
│   │   ├── voice_agent.c
│   │   ├── mimo_client.c
│   │   ├── wash_agent_parser.c
│   │   ├── include/voice_agent.h
│   │   └── test/test_wash_agent_parser.c
│   ├── display_adapter/
│   └── diagnostics/
├── test_apps/
│   ├── unit/
│   │   ├── CMakeLists.txt
│   │   ├── sdkconfig.defaults
│   │   └── main/test_runner.c
│   └── hardware_bringup/
│       ├── CMakeLists.txt
│       ├── sdkconfig.defaults
│       └── main/hardware_test_console.c
├── host_tests/
│   ├── CMakeLists.txt
│   └── planner/
├── tools/
│   ├── check_architecture.py
│   ├── check_gpio_map.py
│   ├── ble_protocol_test.py
│   ├── serial_smoke_test.py
│   └── decode_coredump.py
└── docs/
    ├── xiaojing_idf_design/       本目录：需求、接口、计划与测试
    ├── hardware.md                后续从确认需求生成
    ├── software_arch.md           后续从确认架构生成
    └── pin_map.md                 后续从确认GPIO表生成
```

空目录不提交；组件进入开发阶段时再创建。

## 2. 组件层次

### L0：稳定契约

- `machine_types`：姿态、状态、阶段、故障、request_id、事件。
- `safety_contract`：授权类型、停机 hook 签名，不依赖任何 service。
- `hal_contract`：驱动函数表和快照类型。

L0 不依赖其他业务组件。

### L1：平台与驱动

- `board_config/machine_config`
- `i2c_bus_manager/mcp23017_driver`
- `ibt2_driver/bl50_driver/fan_driver/flow_meter`
- `water_level_sensor/turbidity_sensor/sht_sensor/paj7620_adapter/audio_driver`
- `fake_hal`

L1 只依赖 L0 和 ESP-IDF 公共组件。

### L2：设备服务

- `position_service/water_service/dry_service`
- `bl50_service/detergent_service/drain_service/uv_service`
- `button_service`

L2 依赖 L0、L1 和 `safety_manager` 公共授权 API；不依赖协议或网络。

### L3：领域控制

- `wash_planner`
- `safety_manager`
- `wash_executor`
- `machine_status_store`

Planner 保持纯净；executor 编排 L2；safety 通过 L0 hook 调用紧急停止，不反向依赖 L2 头文件。

### L4：入口与表现

- `app_protocol/ble_transport/voice_agent`
- `display_adapter/diagnostics`

L4 不允许直接依赖具体硬件驱动。

## 3. 依赖规则

允许：

```text
main -> app_protocol, wash_executor, services, drivers
app_protocol -> wash_planner, wash_executor, status_store, ble_transport
voice_agent -> app_protocol contract, network/audio adapters
wash_executor -> services, safety_manager, status_store
services -> contracts + 自己需要的 HAL + safety public API
safety_manager -> contracts + MCP/低层紧急输出 + status_store
drivers -> board_config + ESP-IDF driver APIs
```

禁止：

```text
driver -> service
planner -> FreeRTOS/driver/JSON/network
ble_transport -> planner/executor/driver
voice_agent -> driver/service
service A -> service B 的内部头文件
safety_manager -> position_service/water_service/dry_service 组件依赖
```

`tools/check_architecture.py` 最终应扫描 `idf_component_register(REQUIRES/PRIV_REQUIRES)` 并阻止上述反向/环形依赖。

## 4. 文件职责

- `app_main.c`：只调用 bootstrap，不写业务状态机。
- `app_bootstrap.c`：按安全顺序创建驱动、队列、服务、入口；失败进入安全故障态。
- `board_config.h`：唯一 GPIO/MCP 映射源。
- `machine_config`：Kconfig 默认值 + NVS 版本化覆盖 + 校验。
- `machine_status_store`：锁保护一致快照，revision 递增。
- `*_driver.c`：硬件寄存器和 IDF driver API，不含洗涤流程。
- `*_service.c`：局部异步状态机，不解析 JSON。
- `wash_profiles.c`：Formal/Demo 默认参数表。
- `wash_planner.c`：意图编译、补姿态、补进排水、裁剪时长。
- `wash_executor.c`：唯一整机状态和步骤推进。
- `app_json_codec.c`：V1 JSON 编解码和限制检查。
- `legacy_protocol.c`：旧 start/stop、P/S/A 兼容，随时可移除。
- `wash_agent_parser.c`：MiMo tool call -> `wash_intent_t`，拒绝裸硬件动作。

## 5. HAL 设计

业务组件不得用条件编译直接散落 fake/real 分支。使用统一函数表：

```c
typedef struct {
    esp_err_t (*read_mcp_inputs)(mcp_input_snapshot_t *out);
    esp_err_t (*set_safe_output)(safe_output_t output, bool enable);
    esp_err_t (*set_ibt2)(ibt2_command_t command);
    esp_err_t (*set_bl50)(bl50_command_t command);
    esp_err_t (*set_fan_percent)(uint8_t percent);
    esp_err_t (*read_water_level)(bool *full);
    esp_err_t (*read_sht)(sht_sample_t *out);
    esp_err_t (*read_flow)(flow_snapshot_t *out);
    int64_t (*now_ms)(void);
} xiaojing_hal_t;
```

- `fake_hal` 保存调用历史、传感器脚本和可控单调时间。
- real HAL 在 bootstrap 中装配各驱动。
- safety_manager 拥有高风险输出的业务入口；HAL 本身不能替代安全授权。

## 6. 任务与通信对象

建议默认：

| 任务 | Core | Priority | Stack 初值 | 输入 |
|---|---:|---:|---:|---|
| safety_task | 1 | 20 | 4096 | safety_event_queue + 20ms tick |
| position_task | 1 | 16 | 4096 | position_cmd_queue + notify |
| wash_executor_task | 1 | 12 | 6144 | control + machine_event QueueSet |
| water_task | 1 | 11 | 4096 | water_cmd_queue + 50ms tick |
| dry_task | 1 | 11 | 5120 | dry_cmd_queue + 100ms tick |
| app_protocol_task | unpinned/0 | 8 | 6144 | app_request_queue |
| ble_rx_task | unpinned/0 | 7 | 4096 | BLE chunks |
| ble_tx_task | unpinned/0 | 7 | 4096 | priority TX queue |
| voice_agent_task | 0 | 5 | 实测 | semaphore/command |

栈大小只是初值，必须通过 `uxTaskGetStackHighWaterMark()` 实测，记录到 `CALIBRATION.md/PROGRESS.md`。

关键对象：

- `app_request_queue`
- `executor_control_queue`
- `executor_urgent_queue`
- `machine_event_queue`
- `safety_event_queue`
- 各 service command queue
- `ble_tx_critical_queue` 与可合并普通 status 队列
- `machine_status_store` mutex
- `i2c_bus_manager` mutex

## 7. Kconfig 结构

建议顶层菜单 `Xiaojing configuration`：

```text
Output mode
  FAKE (default)
  LOW_VOLTAGE
  REAL
Enable PTC (default n)
Enable voice cloud (default n for bring-up)
Enable display
Enable legacy BLE protocol
Position calibration
Water calibration
Dry safety policy
Detergent calibration
Diagnostics/log level
```

不能把 API key 放入 sdkconfig.defaults；只提供占位和安全的 provisioning 入口。

## 8. 构建产物与分区

- 16MB Flash、8MB OPI PSRAM。
- `sdkconfig.defaults` 固定 `esp32s3`、NimBLE、PSRAM、安全日志和默认 fake 输出。
- 分区至少考虑 NVS、OTA 双槽、coredump/诊断；具体大小在 Phase 0 根据固件预算确定。
- `dependencies.lock` 记录受控第三方组件版本；优先 ESP-IDF 内置能力。
- 不直接复制 PlatformIO 的 Arduino `sdkconfig.defaults`，逐项迁移必要设置。
