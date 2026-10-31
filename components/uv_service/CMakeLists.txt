idf_component_register(SRCS "uv_service.c" "uv_fsm.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "private_include"
                       REQUIRES machine_types hal_contract safety_contract
                                machine_config safety_manager board_config
                       PRIV_REQUIRES freertos)

if(CONFIG_UV_SERVICE_TEST_HOOKS)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC UV_SERVICE_TEST_HOOKS)
endif()
