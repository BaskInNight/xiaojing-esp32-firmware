idf_component_register(SRCS "paj7620_adapter.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES i2c_bus_manager mcp23017_driver board_config freertos)
