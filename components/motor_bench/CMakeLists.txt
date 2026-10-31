idf_component_register(SRCS "motor_bench_core.c" "motor_bench_service.c"
                       INCLUDE_DIRS "include"
                       REQUIRES machine_types machine_config
                                safety_contract safety_manager
                                water_service detergent_service drain_service
                                dry_service coupled_hot_air
                                uv_self_test actuator_self_test
                                wash_executor position_service bl50_service
                                freertos)
