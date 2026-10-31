idf_component_register(
    SRCS "program_control_core.c" "program_control_service.c"
    INCLUDE_DIRS "include"
    REQUIRES machine_types hal_contract machine_config wash_planner wash_executor
             freertos esp_timer)
