idf_component_register(SRCS "turbidity_sensor.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES board_config esp_adc)
