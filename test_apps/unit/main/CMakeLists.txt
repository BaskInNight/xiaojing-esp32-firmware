idf_component_register(SRCS "test_runner.c" "group_d_tests.c" "group_d_test_support.c" "wash_executor_tests.c" "wash_executor_round2_integration_tests.c" "phase8_protocol_tests.c" "phase9_voice_tests.c" "phase9_cloud_tests.c" "phase9_cloud_adapter_tests.c" "phase9_wifi_tests.c" "phase9_credentials_tests.c" "phase9_display_tests.c" "phase9_actuator_tests.c" "phase11_actuator_lifecycle_tests.c" "phase11_round1_1_tests.c" "phase11_coupled_hot_air_tests.c" "phase11_groupd_executor_tests.c"
                       INCLUDE_DIRS "."
                       PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/water_service/private_include"
                                          "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/dry_service/private_include"
                                          "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/bl50_service/private_include"
                                          "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/drain_service/private_include"
                                          "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/detergent_service/private_include"
                                          "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/uv_service/private_include"
                                          "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/button_service/private_include"
                                          "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/wash_executor/private_include"
                       PRIV_REQUIRES unity machine_types hal_contract
                                     fake_hal real_hal machine_status_store
                                     machine_config freertos nvs_flash esp_rom
                                     i2c_bus_manager mcp23017_driver
                                     ibt2_driver bl50_driver fan_driver
                                     flow_meter water_level_sensor
                                     turbidity_sensor sht_sensor
                                     audio_driver board_config position_service
                                     safety_manager bl50_service water_service dry_service
                                     drain_service detergent_service uv_service
                                     uv_self_test actuator_self_test
                                     button_service wash_planner wash_executor
                                     program_control
                                     coupled_hot_air
                                     voice_agent voice_frontend
                                     voice_credentials
                                     wifi_station
                                     display_service
                                     app_protocol ble_transport
                                     esp_driver_i2c esp_driver_pcnt
                                     esp_driver_ledc esp_driver_i2s esp_adc
                                     esp_driver_gpio esp_timer)
