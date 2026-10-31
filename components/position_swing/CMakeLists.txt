idf_component_register(
    SRCS "position_swing_core.c" "position_swing_service.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES machine_types safety_manager position_service bl50_service water_service
             detergent_service drain_service dry_service coupled_hot_air uv_self_test
             actuator_self_test motor_bench wash_executor hal_contract esp_driver_gpio
             esp_timer freertos app_protocol board_config machine_config)