idf_component_register(
    SRCS "ble_frame_assembler.c" "ble_transport.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos
    PRIV_REQUIRES bt
)

if(CONFIG_BLE_TRANSPORT_TEST_HOOKS)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC CONFIG_BLE_TRANSPORT_TEST_HOOKS=1)
endif()
