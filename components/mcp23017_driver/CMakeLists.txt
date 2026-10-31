idf_component_register(SRCS "mcp23017_driver.c"
                       INCLUDE_DIRS "include"
                       REQUIRES hal_contract i2c_bus_manager
                       PRIV_REQUIRES board_config
                                     esp_driver_gpio freertos)
