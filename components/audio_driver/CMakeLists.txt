idf_component_register(SRCS "audio_driver.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES board_config esp_driver_i2s esp_driver_gpio)
