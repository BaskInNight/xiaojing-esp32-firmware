idf_component_register(SRCS "water_level_sensor.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES board_config esp_driver_gpio freertos)
