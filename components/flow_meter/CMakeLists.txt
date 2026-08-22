idf_component_register(SRCS "flow_meter.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES board_config hal_contract esp_driver_pcnt freertos)
