idf_component_register(
    SRCS "app_protocol_core.c" "app_protocol.c"
    INCLUDE_DIRS "include"
    REQUIRES machine_types machine_config machine_status_store
             wash_planner wash_executor program_control json freertos
)
