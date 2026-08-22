idf_component_register(SRCS "machine_config.c" "machine_config_validate.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES nvs_flash machine_types)
