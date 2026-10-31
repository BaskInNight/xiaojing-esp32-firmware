idf_component_register(
    SRCS "audio_controller.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES freertos log esp_timer
)
