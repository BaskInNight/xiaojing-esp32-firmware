#pragma once

/* Review draft: not registered in the production CMakeLists.txt. */
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

/* All lifecycle calls belong to the bootstrap/display owner task. Never call
 * start/stop concurrently. No LVGL display may own this panel while active.
 * dma is the existing INTERNAL|DMA reservation, borrowed until stop succeeds.
 * start requires an initialized ILI9341 in display-OFF state. */
esp_err_t display_boot_animation_start(esp_lcd_panel_handle_t panel,
                                      esp_lcd_panel_io_handle_t io,
                                      void *dma, size_t dma_bytes);

/* Real completed bootstrap milestone, 0..95. No fabricated elapsed-time %. */
void display_boot_animation_stage(uint8_t percent);

/* Bounded task join, then SPI drain. On failure keep all resources alive;
 * do NOT free dma/panel/io or start LVGL; retry stop or abort the display path.
 * Success ends ALL boot writes and frees its PSRAM frame. Last image remains
 * visible until the main UI has been fully flushed. */
esp_err_t display_boot_animation_stop(void);
