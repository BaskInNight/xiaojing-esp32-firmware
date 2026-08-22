idf_component_register(SRCS "coupled_hot_air_fsm.c" "coupled_hot_air_service.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "private_include"
                       REQUIRES machine_types hal_contract safety_contract
                                machine_config safety_manager board_config
                       PRIV_REQUIRES freertos)
