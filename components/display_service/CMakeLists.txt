idf_component_register(
    SRCS "display_model.c" "display_service.c" "telemetry_font.c"
    INCLUDE_DIRS "include"
    REQUIRES board_config freertos esp_driver_spi
             esp_lcd_ili9341 esp_lvgl_port lvgl
)
