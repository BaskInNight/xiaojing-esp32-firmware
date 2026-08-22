cmake_minimum_required(VERSION 3.16)
project(xiaojing_host_tests C)

set(CMAKE_C_STANDARD 17)

# Shims directory FIRST — provides esp_err.h for host compilation
set(SHIMS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shims)

# Component include directories
set(COMPONENT_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/machine_types/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/safety_contract/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/hal_contract/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/machine_config/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/machine_status_store/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/fake_hal/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/board_config/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/safety_manager/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/safety_manager/private_include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/button_service/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/button_service/private_include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gesture_service/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/wash_planner/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/wash_executor/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/wash_executor/private_include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/app_protocol/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/uv_service/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/uv_service/private_include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/uv_self_test/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ble_transport/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/voice_agent/include
)

# Production pure-C sources (no NVS/FreeRTOS dependencies)
set(PRODUCTION_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/machine_config/machine_config_validate.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/machine_status_store/machine_revision_next.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/safety_manager/safety_interlocks.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/button_service/button_debounce.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gesture_service/gesture_page_map.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/wash_planner/wash_planner.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/wash_executor/wash_executor_core.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/wash_executor/wash_executor_fake_adapter.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/uv_service/uv_fsm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/uv_self_test/uv_self_test_core.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/app_protocol/app_protocol_core.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ble_transport/ble_frame_assembler.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/voice_agent/voice_core.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/voice_agent/voice_tool_parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/voice_agent/voice_cloud.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/voice_agent/voice_profile.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/voice_agent/voice_route.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/voice_agent/voice_provider.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/voice_agent/voice_vad.c
)

add_executable(test_runner
    test_main.c
    ${PRODUCTION_SOURCES}
)

target_include_directories(test_runner PRIVATE
    ${SHIMS_DIR}
    ${COMPONENT_INCLUDES}
)

# Link math library (isfinite, NAN, INFINITY) on non-MSVC platforms
if(NOT MSVC)
    target_link_libraries(test_runner PRIVATE m)
endif()

enable_testing()
add_test(NAME xiaojing_host_tests COMMAND test_runner)

add_executable(test_voice_vad
    test_voice_vad.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/voice_agent/voice_vad.c
)
target_include_directories(test_voice_vad PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/voice_agent/include
)
add_test(NAME voice_vad_tests COMMAND test_voice_vad)

add_executable(test_display_model
    test_display_model.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/display_service/display_model.c
)
target_include_directories(test_display_model PRIVATE
    ${SHIMS_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/display_service/include
)
if(MSVC)
    target_compile_options(test_display_model PRIVATE /utf-8)
endif()
add_test(NAME display_model_tests COMMAND test_display_model)

add_executable(test_uv_act_core
    test_uv_act_core.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/uv_service/uv_fsm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/uv_self_test/uv_self_test_core.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/detergent_service/detergent_fsm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/actuator_self_test/actuator_self_test_core.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/water_service/water_fsm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/drain_service/drain_fsm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dry_service/dry_fsm.c
)
target_include_directories(test_uv_act_core PRIVATE
    ${SHIMS_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/machine_types/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/safety_contract/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/hal_contract/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/machine_config/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/board_config/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/uv_service/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/uv_service/private_include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/uv_self_test/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/detergent_service/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/detergent_service/private_include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/actuator_self_test/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/water_service/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/water_service/private_include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/drain_service/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/drain_service/private_include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dry_service/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dry_service/private_include
)
add_test(NAME uv_act_core_tests COMMAND test_uv_act_core)

# Phase 11 audit regression tests: app_protocol target UAF, seq cache
# duplicate replay, Wi-Fi/cloud same-frame combination.
add_executable(test_phase11_safety
    test_phase11_safety.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/app_protocol/app_protocol_core.c
)
target_include_directories(test_phase11_safety PRIVATE
    ${SHIMS_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/app_protocol/include
)
add_test(NAME phase11_safety_tests COMMAND test_phase11_safety)

# Group B R0.2: Fake HAL randomized pressure test (10k rounds, shadow HAL).
add_executable(test_fake_hal_stress
    test_fake_hal_stress.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/detergent_service/detergent_fsm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/drain_service/drain_fsm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/uv_service/uv_fsm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dry_service/dry_fsm.c
)
target_include_directories(test_fake_hal_stress PRIVATE
    ${SHIMS_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/machine_types/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/safety_contract/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/hal_contract/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/machine_config/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/board_config/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/detergent_service/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/detergent_service/private_include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/drain_service/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/drain_service/private_include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/uv_service/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/uv_service/private_include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dry_service/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dry_service/private_include
)
add_test(NAME fake_hal_stress COMMAND test_fake_hal_stress)

# Phase 11 Group B: 单继电器耦合热风模块安全测试（FSM + 联锁门禁 + act core 门禁 + 压力）
add_executable(test_coupled_hot_air
    test_coupled_hot_air.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/coupled_hot_air/coupled_hot_air_fsm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/safety_manager/safety_interlocks.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/actuator_self_test/actuator_self_test_core.c
)
target_include_directories(test_coupled_hot_air PRIVATE
    ${SHIMS_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/machine_types/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/safety_contract/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/hal_contract/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/machine_config/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/board_config/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/safety_manager/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/safety_manager/private_include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/coupled_hot_air/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/coupled_hot_air/private_include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/actuator_self_test/include
)
if(MSVC)
    target_compile_options(test_coupled_hot_air PRIVATE /utf-8)
endif()
add_test(NAME coupled_hot_air_tests COMMAND test_coupled_hot_air)

# Phase 11 Group C: motor_bench 电机空载台架 core FSM + 门禁。
# 编译真实生产源：motor_bench_core.c（本组件纯 C 核心）+ bl50_fsm.c
# （生产动作→位置契约，bl50_action_to_position 为权威来源，杜绝硬编码猜测）。
add_executable(test_motor_bench
    test_motor_bench.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/motor_bench/motor_bench_core.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/bl50_service/bl50_fsm.c
)
target_include_directories(test_motor_bench PRIVATE
    ${SHIMS_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/motor_bench/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/bl50_service/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/bl50_service/private_include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/machine_types/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/safety_contract/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/hal_contract/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/machine_config/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/board_config/include
)
if(MSVC)
    target_compile_options(test_motor_bench PRIVATE /utf-8)
endif()
add_test(NAME motor_bench_core_tests COMMAND test_motor_bench)

# 连续换向展示台：纯 C 逻辑（clamp + 状态机镜像）host 测试。
# 编译真实生产 clamp/常量 + 本文件的映射 stub，验证边界与拒绝码。
add_executable(test_position_swing
    test_position_swing.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/position_swing/position_swing_core.c
)
target_include_directories(test_position_swing PRIVATE
    ${SHIMS_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/position_swing/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/machine_types/include
)
if(MSVC)
    target_compile_options(test_position_swing PRIVATE /utf-8)
endif()
add_test(NAME position_swing_clamp_tests COMMAND test_position_swing)

# Phase 11 Group D R0: program_control 纯 C 模型（DEFAULT_DEMO_V1 26 阶段
# recipe / 优先级仲裁 / BTN1 互斥 / 手势冷却 / 输出审计 fail-closed）。
add_executable(test_program_control
    test_program_control.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/program_control/program_control_core.c
)
target_include_directories(test_program_control PRIVATE
    ${SHIMS_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/program_control/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/machine_types/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/safety_contract/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/hal_contract/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/machine_config/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/board_config/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/wash_planner/include
)
if(MSVC)
    target_compile_options(test_program_control PRIVATE /utf-8)
endif()
add_test(NAME program_control_core_tests COMMAND test_program_control)
