idf_component_register(SRCS "i2c_bus_manager.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_i2c esp_driver_gpio board_config hal_contract freertos)
