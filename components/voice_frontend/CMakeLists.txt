idf_component_register(
    SRCS "src/voice_frontend.c"
    INCLUDE_DIRS "include"
    REQUIRES
        freertos
        esp_timer
        voice_agent
)
