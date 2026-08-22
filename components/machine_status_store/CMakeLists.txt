idf_component_register(SRCS "machine_status_store.c" "machine_revision_next.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES machine_types freertos)
