idf_component_register(
    SRCS
        "safety_manager.c"
        "safety_interlocks.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "private_include"
    REQUIRES
        safety_contract
        machine_types
        hal_contract
        machine_config
        machine_status_store
        board_config
)

if(CONFIG_SAFETY_MGR_TEST_HOOKS)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC SAFETY_MANAGER_TEST_HOOKS)
endif()
