idf_component_register(
    SRCS "src/voice_wake_esp_sr.c"
    INCLUDE_DIRS "include"
    REQUIRES
        voice_frontend
        freertos
        esp_timer
        espressif__esp-sr
)
