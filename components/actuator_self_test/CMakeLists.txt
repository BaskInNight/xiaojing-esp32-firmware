idf_component_register(SRCS "actuator_self_test.c" "actuator_self_test_core.c"
                       INCLUDE_DIRS "include"
                       REQUIRES machine_types machine_config
                                safety_contract safety_manager
                                water_service detergent_service drain_service
                                dry_service coupled_hot_air
                                uv_self_test wash_executor freertos)
