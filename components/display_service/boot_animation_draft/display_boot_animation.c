/* ESP-IDF 5.5.4 / ESP32-S3 / ILI9341 320x240 landscape.
 * Standalone review draft; see docs/BOOT_ANIMATION_REVIEW.md before integration.
 * No LVGL objects/timers/animations, and no motor/valve/network side effects.
 * Only the existing uncompressed telemetry_font glyph table is borrowed.
 */
#include "display_boot_animation.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "lvgl.h"

#define W 320
#define H 240
#define BG 0x07111fU
#define CARD 0x111d2eU
#define CYAN 0x00c8ffU
#define DIM 0x93a9bbU
#define WHITE 0xf2f7fbU
extern const lv_font_t telemetry_font;
static const char *TAG = "boot_anim";
static struct {
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t io;
    uint16_t *frame;
    uint8_t *dma;
    size_t lines;
    SemaphoreHandle_t done;
    StaticSemaphore_t done_storage;
    bool active;
    bool joined;
    atomic_bool stop;
    atomic_uchar stage;
} s;

static uint16_t rgb565(uint32_t rgb)
{
    return (uint16_t)(((rgb >> 8) & 0xf800U) |
                      ((rgb >> 5) & 0x07e0U) | ((rgb >> 3) & 0x001fU));
}

static void pixel(int x, int y, uint32_t color)
{
    if (x >= 0 && x < W && y >= 0 && y < H) s.frame[y * W + x] = rgb565(color);
}

static void box(int x, int y, int w, int h, uint32_t color)
{
    for (int yy = y; yy < y + h; ++yy)
        for (int xx = x; xx < x + w; ++xx) pixel(xx, yy, color);
}

static void ring(int cx, int cy, int radius, uint32_t color)
{
    int outer = radius * radius, inner = (radius - 2) * (radius - 2);
    for (int y = -radius; y <= radius; ++y)
        for (int x = -radius; x <= radius; ++x) {
            int d = x * x + y * y;
            if (d <= outer && d >= inner) pixel(cx + x, cy + y, color);
        }
}

static uint32_t next_utf8(const unsigned char **p)
{
    uint32_t c = *(*p)++;
    if (c < 0x80U) return c;
    unsigned count = c < 0xe0U ? 1U : 2U; /* Only fixed BMP literals below. */
    c &= count == 1U ? 0x1fU : 0x0fU;
    while (count--) c = (c << 6) | (*(*p)++ & 0x3fU);
    return c;
}

/* Font format is intentionally pinned: telemetry_font, plain 4bpp, LVGL 8.4.
 * It has no decompression allocation or LVGL runtime dependency. Do not use
 * this renderer concurrently with LVGL (glyph descriptor cache is shared). */
static void label(const char *str, int cx, int top, int scale, uint32_t color)
{
    const unsigned char *p = (const unsigned char *)str;
    lv_font_glyph_dsc_t g;
    int width = 0;
    while (*p) {
        uint32_t cp = next_utf8(&p);
        if (lv_font_get_glyph_dsc(&telemetry_font, &g, cp, 0)) width += g.adv_w;
    }
    int x = cx - width * scale / 2;
    p = (const unsigned char *)str;
    while (*p) {
        uint32_t cp = next_utf8(&p);
        if (!lv_font_get_glyph_dsc(&telemetry_font, &g, cp, 0)) continue;
        const uint8_t *bitmap = lv_font_get_glyph_bitmap(&telemetry_font, cp);
        if (bitmap && g.bpp == 4) {
            int gy = top + (telemetry_font.line_height - telemetry_font.base_line -
                             g.box_h - g.ofs_y) * scale;
            for (unsigned yy = 0; yy < g.box_h; ++yy)
                for (unsigned xx = 0; xx < g.box_w; ++xx) {
                    unsigned i = yy * g.box_w + xx;
                    unsigned a = (bitmap[i / 2] >> ((i & 1U) ? 0 : 4)) & 15U;
                    /* Fixed opaque background; simple coverage threshold avoids
                     * a large software blending path during radio startup. */
                    if (a >= 5U) box(x + (g.ofs_x + (int)xx) * scale,
                                     gy + (int)yy * scale, scale, scale, color);
                }
        }
        x += g.adv_w * scale;
    }
}

static void render(unsigned phase)
{
    uint16_t bg = rgb565(BG);
    for (int i = 0; i < W * H; ++i) s.frame[i] = bg;
    box(121,43,78,78,0x1c3a56U);
    box(122,44,76,76,CARD);
    box(131,56,12,2,DIM);
    box(183,55,3,3,CYAN);
    box(131,64,58,1,CYAN);
    ring(160,86,26,0x31506bU);
    ring(160,86,23,CYAN);
    /* Three integer water waves clipped to the circular drum. */
    static const int8_t wave[16] = {0,1,2,3,3,3,2,1,0,-1,-2,-3,-3,-3,-2,-1};
    for (int row = 0; row < 3; ++row)
        for (int x = -21; x <= 21; ++x) {
            int y = 3 + row * 6 + wave[((unsigned)(x + 32) / 3 + phase + row) % 16];
            if (x*x + y*y < 21*21) pixel(160+x,86+y,row == 0 ? CYAN : 0x008bb2U);
        }
    label("小净",160,134,2,WHITE);
    label("让洁净，自然发生",160,165,1,DIM);
    box(92,200,136,3,CARD);
    unsigned progress = atomic_load(&s.stage);
    box(92,200,3 + (int)(133U * progress / 100U),3,CYAN);
    /* Small travelling marker expresses activity while milestone stays fixed. */
    box(92+(int)(phase % 132U),205,4,1,DIM);
    label("正在启动",160,216,1,DIM);
}

/* IDF 5.5.4 SPI tx_param drains queued color transactions before a polling
 * command. ILI9341 0x00 is NOP. This is a transfer barrier, NOT a delay. */
static esp_err_t drain(void)
{
    return esp_lcd_panel_io_tx_param(s.io, 0x00, NULL, 0);
}

static esp_err_t flush(void)
{
    for (int y = 0; y < H; y += (int)s.lines) {
        int rows = H - y;
        if (rows > (int)s.lines) rows = (int)s.lines;
        for (int i = 0; i < W * rows; ++i) {
            uint16_t c = s.frame[y * W + i];
            s.dma[2*i] = (uint8_t)(c >> 8); /* ILI9341 wire RGB565 MSB first. */
            s.dma[2*i+1] = (uint8_t)c;
        }
        esp_err_t err = esp_lcd_panel_draw_bitmap(s.panel,0,y,W,y+rows,s.dma);
        /* Drain even if submit failed: never reuse an in-flight DMA buffer. */
        esp_err_t barrier = drain();
        if (barrier != ESP_OK) return barrier;
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

static void worker(void *arg)
{
    (void)arg;
    unsigned phase = 0;
    while (!atomic_load(&s.stop)) {
        render(phase++);
        esp_err_t err = flush();
        if (err != ESP_OK) {
            ESP_LOGE(TAG,"Animation transfer failed: 0x%x",(unsigned)err);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(80)); /* Cooperative; no busy loop. */
    }
    xSemaphoreGive(s.done); /* No frame/panel/dma access after this point. */
    vTaskDeleteWithCaps(NULL);
}

esp_err_t display_boot_animation_start(esp_lcd_panel_handle_t panel,
                                      esp_lcd_panel_io_handle_t io,
                                      void *dma, size_t bytes)
{
    if (s.active) return ESP_ERR_INVALID_STATE;
    if (!panel || !io || !dma || bytes < W*2U) return ESP_ERR_INVALID_ARG;
    s.panel=panel; s.io=io; s.dma=dma;
    s.lines=bytes/(W*2U);
    if (s.lines > 2U) s.lines=2U;
    s.frame=heap_caps_malloc(W*H*sizeof(uint16_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!s.frame) return ESP_ERR_NO_MEM;
    atomic_store(&s.stop,false); atomic_store(&s.stage,0);
    s.joined=false;
    s.done=xSemaphoreCreateBinaryStatic(&s.done_storage);
    esp_err_t err=esp_lcd_panel_swap_xy(panel,true);
    if (err == ESP_OK) err=esp_lcd_panel_mirror(panel,false,false);
    if (err == ESP_OK) { render(0); err=flush(); }
    if (err == ESP_OK) err=esp_lcd_panel_disp_on_off(panel,true);
    if (err != ESP_OK) {
        /* PSRAM frame is never DMA-owned, so safe to free even on IO failure.
         * The caller still owns dma and must drain before releasing it. */
        free(s.frame); s.frame=NULL;
        return err;
    }
    if (xTaskCreateWithCaps(worker,"boot_anim",4096,NULL,2,NULL,
                           MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT) != pdPASS) {
        free(s.frame); s.frame=NULL;
        return ESP_ERR_NO_MEM;
    }
    s.active=true;
    ESP_LOGI(TAG,"Started (PSRAM frame=153600, borrowed DMA=%u)",
             (unsigned)(s.lines*W*2U));
    return ESP_OK;
}

void display_boot_animation_stage(uint8_t percent)
{
    if (percent > 95U) percent=95U;
    atomic_store(&s.stage,percent);
}

esp_err_t display_boot_animation_stop(void)
{
    if (!s.active) return ESP_OK;
    atomic_store(&s.stop,true);
    if (!s.joined) {
        if (xSemaphoreTake(s.done,pdMS_TO_TICKS(2000)) != pdTRUE)
            return ESP_ERR_TIMEOUT;
        s.joined=true;
    }
    esp_err_t err=drain();
    if (err != ESP_OK) return err;
    free(s.frame); s.frame=NULL;
    s.active=false; s.dma=NULL; s.panel=NULL; s.io=NULL;
    ESP_LOGI(TAG,"Stopped and drained; panel ownership may pass to LVGL");
    return ESP_OK;
}
