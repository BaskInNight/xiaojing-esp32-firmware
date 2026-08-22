idf_component_register(SRCS "water_service.c" "water_fsm.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "private_include"
                       REQUIRES machine_types hal_contract safety_contract
                                machine_config safety_manager
                       PRIV_REQUIRES freertos)

if(CONFIG_WATER_SERVICE_TEST_HOOKS)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC WATER_SERVICE_TEST_HOOKS)
endif()
