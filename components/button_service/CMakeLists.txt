idf_component_register(
    SRCS "button_service.c" "button_debounce.c"
    INCLUDE_DIRS "include" "private_include"
    REQUIRES hal_contract machine_types freertos
    PRIV_REQUIRES board_config esp_driver_gpio
)
