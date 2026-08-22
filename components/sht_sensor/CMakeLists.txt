idf_component_register(SRCS "sht_sensor.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES i2c_bus_manager hal_contract freertos)
