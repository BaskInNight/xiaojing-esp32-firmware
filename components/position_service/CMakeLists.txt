idf_component_register(SRCS "position_service.c"
                       INCLUDE_DIRS "include"
                       REQUIRES machine_types hal_contract safety_manager
                       PRIV_REQUIRES freertos)

if(CONFIG_POS_SERVICE_TEST_HOOKS)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC POSITION_SERVICE_TEST_HOOKS)
endif()
