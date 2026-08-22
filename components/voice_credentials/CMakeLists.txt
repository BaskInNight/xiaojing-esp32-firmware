idf_component_register(
    SRCS "voice_credentials.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash freertos voice_agent json
)
