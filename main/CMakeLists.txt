idf_component_register(SRCS "app_main.c" "app_bootstrap.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES spi_flash nvs_flash board_config
                                     machine_types hal_contract machine_config
                                     machine_status_store fake_hal real_hal
                                     position_service safety_manager
                                     bl50_service water_service dry_service
                                     coupled_hot_air
                                     drain_service detergent_service uv_service
                                     uv_self_test actuator_self_test
                                     motor_bench position_swing
                                     button_service
                                     turbidity_sensor
                                     wash_planner wash_executor
                                     program_control
                                     app_protocol ble_transport
                                     voice_agent voice_credentials
                                     voice_frontend voice_wake_edge voice_wake_esp_sr
                                     wifi_station audio_driver display_service
                                     gesture_service audio_controller)

# A/B/C test mode: 1=BLE+Edge+WiFi, 2=BLE+WiFi, 3=WiFi only
if(NOT DEFINED XIAOJING_TEST_MODE)
    set(XIAOJING_TEST_MODE 1)
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE XIAOJING_TEST_MODE=${XIAOJING_TEST_MODE})

if(DEFINED XIAOJING_PHASE9_AUTOTEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
                               XIAOJING_PHASE9_AUTOTEST=${XIAOJING_PHASE9_AUTOTEST})
endif()
