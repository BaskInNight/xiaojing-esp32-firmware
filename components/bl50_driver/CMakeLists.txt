idf_component_register(SRCS "bl50_driver.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES board_config hal_contract esp_driver_ledc esp_driver_gpio)
