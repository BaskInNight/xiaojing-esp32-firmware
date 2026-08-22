idf_component_register(SRCS "real_hal.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES hal_contract machine_types board_config
                                     i2c_bus_manager mcp23017_driver
                                     ibt2_driver bl50_driver fan_driver
                                     flow_meter water_level_sensor
                                     sht_sensor paj7620_adapter
                                     audio_driver esp_timer)
