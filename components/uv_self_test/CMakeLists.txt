idf_component_register(SRCS "uv_self_test.c" "uv_self_test_core.c"
                       INCLUDE_DIRS "include"
                       REQUIRES machine_types machine_config
                                safety_contract safety_manager uv_service
                                wash_executor freertos)
