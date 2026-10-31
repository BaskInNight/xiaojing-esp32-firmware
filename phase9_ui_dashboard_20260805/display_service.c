#include "display_service.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "board_config.h"
#include "driver/spi_master.h"
#include "esp_lcd_ili9341.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "sdkconfig.h"

extern const lv_font_t telemetry_font;

#define LCD_HOST SPI2_HOST
#define LCD_HRES 320U
#define LCD_VRES 240U
#define LCD_BUFFER_LINES 20U
/* Reserve two RGB565 scan lines before BLE starts.  The LCD itself is run in
 * polling mode (see hardware_prepare), so this is only a short-lived
 * fragmentation guard while the panel is constructed. */
#define LCD_DMA_RESERVE_BYTES (2U * LCD_HRES * sizeof(lv_color_t))
#define MODEL_LOCK_TIMEOUT_MS 100U

/* High-contrast industrial HMI palette.  Geometry and the existing CJK font
 * keep flash usage low while remaining legible on the 320x240 ILI9341. */
#define CLR_BG_DEEP      0x07111f
#define CLR_GLASS_BG     0x111d2e
#define CLR_GLASS_BORDER 0x31506b
#define CLR_CYAN         0x00c8ff
#define CLR_WHITE        0xf2f7fb
#define CLR_ON_PRIMARY   0x03131d
#define CLR_DIM          0x93a9bb
#define CLR_GREEN        0x2ee58b
#define CLR_ORANGE       0xffb020
#define CLR_RED          0xff5b66

typedef enum {
    DISPLAY_LC_UNINITIALIZED = 0,
    DISPLAY_LC_INITIALIZED,
    DISPLAY_LC_RUNNING,
    DISPLAY_LC_STOPPING,
} display_lifecycle_t;

typedef struct {
    display_lifecycle_t lifecycle;
    bool hardware_enabled;
    bool spi_owned;
    bool lvgl_owned;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
    lv_display_t *display;
    void *dma_reserve;
    lv_timer_t *refresh_timer;
    lv_obj_t *pages[DISPLAY_PAGE_COUNT];
    lv_obj_t *status_wifi;
    lv_obj_t *status_ble;
    lv_obj_t *status_wash;
    lv_obj_t *status_program;
    lv_obj_t *status_fault;
    lv_obj_t *status_voice;
    lv_obj_t *status_env;
    lv_obj_t *status_position;
    lv_obj_t *wash_bar;
    lv_obj_t *wash_percent;
    lv_obj_t *wash_step;
    lv_obj_t *wash_position;
    lv_obj_t *wash_timing;
    lv_obj_t *peripheral_water;
    lv_obj_t *peripheral_drive;
    lv_obj_t *peripheral_outputs;
    lv_obj_t *peripheral_memory;
    lv_obj_t *peripheral_env;
    lv_obj_t *peripheral_water_bar;
    lv_obj_t *peripheral_drive_bar;
    lv_obj_t *peripheral_memory_bar;
    lv_obj_t *peripheral_water_arc;
    lv_obj_t *peripheral_drive_arc;
    lv_obj_t *peripheral_output_leds[6];
    lv_obj_t *peripheral_wifi_led;
    lv_obj_t *peripheral_ble_led;
    lv_obj_t *heartbeat[DISPLAY_PAGE_COUNT];
    lv_obj_t *voice_orbit;
    lv_obj_t *voice_core;
    lv_obj_t *voice_state;
    lv_obj_t *voice_backend;
    lv_obj_t *voice_transcript;
    lv_obj_t *voice_reply;
    uint32_t rendered_revision;
    uint8_t heartbeat_phase;
} display_runtime_t;

static const char *TAG = "display_svc";
static StaticSemaphore_t s_model_lock_storage;
static SemaphoreHandle_t s_model_lock;
static display_model_t s_model;
static display_runtime_t s_rt;
static _Atomic int s_init_state;
static _Atomic int s_requested_page;
static _Atomic int s_current_page;

static bool model_lock(void)
{
    return s_model_lock &&
           xSemaphoreTake(s_model_lock,
                          pdMS_TO_TICKS(MODEL_LOCK_TIMEOUT_MS)) == pdTRUE;
}

static uint32_t next_revision(uint32_t revision)
{
    revision++;
    return revision == 0U ? 1U : revision;
}

static void copy_text(char *dst, size_t capacity, const char *src)
{
    if (capacity == 0U) return;
    if (!src) src = "";
    size_t length = strnlen(src, capacity - 1U);
    memcpy(dst, src, length);
    dst[length] = '\0';
}

/* Render-only bounded copy.  The cloud model keeps its full UTF-8 payload;
 * the LCD receives a short, character-boundary-safe string so a long reply
 * can never resize or push other widgets out of the fixed 320x240 layout. */
static void copy_display_text(char *dst, size_t capacity, const char *src,
                              size_t max_bytes)
{
    if (capacity == 0U) return;
    if (!src) src = "";
    size_t limit = capacity - 1U;
    if (max_bytes < limit) limit = max_bytes;
    size_t n = strnlen(src, limit);
    bool truncated = src[n] != '\0';
    while (n > 0U && ((src[n] & 0xc0U) == 0x80U)) n--;
    if (truncated && n + 3U <= limit) {
        memcpy(dst, src, n);
        memcpy(dst + n, "...", 3U);
        n += 3U;
    } else {
        memcpy(dst, src, n);
    }
    dst[n] = '\0';
}

static lv_obj_t *telemetry_label_create(lv_obj_t *parent, const char *text,
                                        uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &telemetry_font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static lv_obj_t *card_create(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                             lv_coord_t width, lv_coord_t height)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, width, height);
    lv_obj_set_style_bg_color(card, lv_color_hex(CLR_GLASS_BG), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(CLR_GLASS_BORDER), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 3, 0);
    lv_obj_set_style_radius(card, 5, 0);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static lv_obj_t *brand_header_create(lv_obj_t *page, const char *title,
                                     const char *page_number,
                                     uint8_t page_index)
{
    lv_obj_t *header = card_create(page, 8, 6, 304, 38);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x0c2639), 0);
    lv_obj_set_style_border_color(header, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_border_width(header, 2, 0);
    /* Header children use fixed pixel positions.  Do not let the generic
     * card padding move the heartbeat into the page-number area. */
    lv_obj_set_style_pad_all(header, 0, 0);

    const lv_font_t *header_font = &telemetry_font;
    lv_obj_t *heading = lv_label_create(header);
    lv_label_set_text(heading, title);
    lv_obj_set_style_text_font(heading, header_font, 0);
    lv_obj_set_style_text_color(heading, lv_color_hex(CLR_WHITE), 0);
    lv_obj_set_pos(heading, 12, 8);
    lv_obj_set_width(heading, 190);
    lv_label_set_long_mode(heading, LV_LABEL_LONG_CLIP);
    lv_obj_t *number = lv_label_create(header);
    lv_label_set_text(number, page_number);
    lv_obj_set_style_text_font(number, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(number, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_pos(number, 238, 8);
    lv_obj_set_width(number, 58);
    lv_obj_set_style_text_align(number, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(number, LV_LABEL_LONG_CLIP);
    lv_obj_t *heartbeat = lv_obj_create(header);
    lv_obj_set_size(heartbeat, 10, 10);
    lv_obj_set_style_radius(heartbeat, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(heartbeat, lv_color_hex(CLR_GREEN), 0);
    lv_obj_set_style_bg_opa(heartbeat, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(heartbeat, 1, 0);
    lv_obj_set_style_border_color(heartbeat, lv_color_hex(CLR_GREEN), 0);
    /* Fixed gap between LIVE dot and page number; no alignment arithmetic. */
    lv_obj_set_pos(heartbeat, 214, 12);
    lv_obj_clear_flag(heartbeat, LV_OBJ_FLAG_SCROLLABLE);
    if (page_index < DISPLAY_PAGE_COUNT)
        s_rt.heartbeat[page_index] = heartbeat;
    return header;
}

static lv_obj_t *divider_create(lv_obj_t *parent, lv_coord_t y,
                                lv_coord_t width)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_set_pos(line, 0, y);
    lv_obj_set_size(line, width, 3);
    lv_obj_set_style_bg_color(line, lv_color_hex(CLR_GLASS_BORDER), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_pad_all(line, 0, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    return line;
}

static lv_obj_t *divider_vertical_create(lv_obj_t *parent, lv_coord_t x,
                                          lv_coord_t y, lv_coord_t height)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_set_pos(line, x, y);
    lv_obj_set_size(line, 2, height);
    lv_obj_set_style_bg_color(line, lv_color_hex(CLR_GLASS_BORDER), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_pad_all(line, 0, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    return line;
}

static void add_page_indicator(lv_obj_t *page, uint8_t active)
{
    static const lv_coord_t x_pos[DISPLAY_PAGE_COUNT] = {132, 150, 168, 186};
    for (uint8_t i = 0; i < DISPLAY_PAGE_COUNT; ++i) {
        lv_obj_t *dot = lv_obj_create(page);
        lv_obj_set_pos(dot, x_pos[i] - (i == active ? 3 : 0), 228);
        lv_obj_set_size(dot, i == active ? 14 : 8, 6);
        lv_obj_set_style_radius(dot, 2, 0);
        lv_obj_set_style_bg_color(dot,
            lv_color_hex(i == active ? CLR_CYAN : CLR_GLASS_BORDER), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    }
}

static lv_obj_t *page_create(void)
{
    lv_obj_t *page = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(page, lv_color_hex(CLR_BG_DEEP), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    return page;
}

static void build_status_page(void)
{
    lv_obj_t *page = s_rt.pages[0] = page_create();
    brand_header_create(page, "小净 / 系统状态", "01 / 04", 0);

    lv_obj_t *network = card_create(page, 8, 52, 149, 70);
    lv_obj_set_style_pad_all(network, 2, 0);
    s_rt.status_wifi = telemetry_label_create(network, "WiFi  --", CLR_DIM);
    lv_obj_set_width(s_rt.status_wifi, 127);
    lv_label_set_long_mode(s_rt.status_wifi, LV_LABEL_LONG_DOT);
    lv_obj_align(s_rt.status_wifi, LV_ALIGN_TOP_LEFT, 1, 0);
    s_rt.status_ble = telemetry_label_create(network, "BLE   --", CLR_DIM);
    lv_obj_set_width(s_rt.status_ble, 127);
    lv_label_set_long_mode(s_rt.status_ble, LV_LABEL_LONG_DOT);
    lv_obj_align(s_rt.status_ble, LV_ALIGN_TOP_LEFT, 1, 31);

    lv_obj_t *environment = card_create(page, 163, 52, 149, 70);
    lv_obj_set_style_pad_all(environment, 2, 0);
    s_rt.status_env = telemetry_label_create(environment, "温湿度  --", CLR_DIM);
    lv_obj_set_width(s_rt.status_env, 127);
    lv_label_set_long_mode(s_rt.status_env, LV_LABEL_LONG_DOT);
    lv_obj_align(s_rt.status_env, LV_ALIGN_TOP_LEFT, 1, 0);
    s_rt.status_position = telemetry_label_create(environment, "姿态    --", CLR_DIM);
    lv_obj_set_width(s_rt.status_position, 127);
    lv_label_set_long_mode(s_rt.status_position, LV_LABEL_LONG_DOT);
    lv_obj_align(s_rt.status_position, LV_ALIGN_TOP_LEFT, 1, 31);

    lv_obj_t *machine = card_create(page, 8, 128, 304, 92);
    lv_obj_set_style_pad_all(machine, 2, 0);
    s_rt.status_wash = telemetry_label_create(machine, "状态  待机", CLR_GREEN);
    lv_obj_set_width(s_rt.status_wash, 132);
    lv_label_set_long_mode(s_rt.status_wash, LV_LABEL_LONG_DOT);
    lv_obj_align(s_rt.status_wash, LV_ALIGN_TOP_LEFT, 2, 2);
    s_rt.status_program = telemetry_label_create(machine, "进度  0%", CLR_CYAN);
    lv_obj_set_width(s_rt.status_program, 132);
    lv_label_set_long_mode(s_rt.status_program, LV_LABEL_LONG_DOT);
    lv_obj_align(s_rt.status_program, LV_ALIGN_TOP_LEFT, 144, 2);
    divider_create(machine, 32, 276);
    divider_vertical_create(machine, 138, 2, 78);
    s_rt.status_fault = telemetry_label_create(machine, "安全  正常", CLR_GREEN);
    lv_obj_set_width(s_rt.status_fault, 132);
    lv_label_set_long_mode(s_rt.status_fault, LV_LABEL_LONG_DOT);
    lv_obj_align(s_rt.status_fault, LV_ALIGN_TOP_LEFT, 2, 43);
    s_rt.status_voice = telemetry_label_create(machine, "唤醒  Edge", CLR_CYAN);
    lv_obj_set_width(s_rt.status_voice, 132);
    lv_label_set_long_mode(s_rt.status_voice, LV_LABEL_LONG_DOT);
    lv_obj_align(s_rt.status_voice, LV_ALIGN_TOP_LEFT, 144, 43);
    add_page_indicator(page, 0);
}

static void build_wash_page(void)
{
    lv_obj_t *page = s_rt.pages[1] = page_create();
    brand_header_create(page, "洗涤流程", "02 / 04", 1);

    lv_obj_t *card = card_create(page, 8, 52, 304, 164);
    lv_obj_set_style_pad_all(card, 2, 0);
    lv_obj_t *progress_title = telemetry_label_create(card, "程序进度", CLR_DIM);
    lv_obj_align(progress_title, LV_ALIGN_TOP_LEFT, 3, 4);
    s_rt.wash_percent = telemetry_label_create(card, "0%", CLR_CYAN);
    lv_obj_set_width(s_rt.wash_percent, 60);
    lv_obj_set_style_text_align(s_rt.wash_percent, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_rt.wash_percent, LV_ALIGN_TOP_RIGHT, -3, 4);

    s_rt.wash_bar = lv_bar_create(card);
    lv_obj_set_size(s_rt.wash_bar, 270, 16);
    lv_obj_align(s_rt.wash_bar, LV_ALIGN_TOP_MID, 0, 37);
    lv_bar_set_range(s_rt.wash_bar, 0, 100);
    lv_obj_set_style_bg_color(s_rt.wash_bar, lv_color_hex(CLR_GLASS_BORDER),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_rt.wash_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_rt.wash_bar, lv_color_hex(0xcfbaa1),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_width(s_rt.wash_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(s_rt.wash_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_rt.wash_bar, lv_color_hex(CLR_CYAN),
                              LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_rt.wash_bar, LV_RADIUS_CIRCLE,
                            LV_PART_INDICATOR);

    divider_create(card, 67, 276);
    s_rt.wash_step = telemetry_label_create(card, "步骤  待机", CLR_WHITE);
    lv_obj_set_width(s_rt.wash_step, 270);
    lv_label_set_long_mode(s_rt.wash_step, LV_LABEL_LONG_DOT);
    lv_obj_align(s_rt.wash_step, LV_ALIGN_TOP_LEFT, 3, 77);
    s_rt.wash_position = telemetry_label_create(card, "姿态  -- → --", CLR_CYAN);
    lv_obj_set_width(s_rt.wash_position, 270);
    lv_label_set_long_mode(s_rt.wash_position, LV_LABEL_LONG_DOT);
    lv_obj_align(s_rt.wash_position, LV_ALIGN_TOP_LEFT, 3, 105);
    s_rt.wash_timing = telemetry_label_create(card, "时间  00:00 / --:--", CLR_DIM);
    lv_obj_set_width(s_rt.wash_timing, 270);
    lv_label_set_long_mode(s_rt.wash_timing, LV_LABEL_LONG_DOT);
    lv_obj_align(s_rt.wash_timing, LV_ALIGN_TOP_LEFT, 3, 133);
    add_page_indicator(page, 1);
}

#if 0
static void build_peripheral_page_legacy(void)
{
    lv_obj_t *page = s_rt.pages[2] = page_create();
    brand_header_create(page, "外设遥测", "03 / 04", 2);

    lv_obj_t *water = card_create(page, 8, 52, 149, 76);
    lv_obj_set_style_pad_all(water, 2, 0);
    s_rt.peripheral_water = label_create(water, "水量  --ml", CLR_WHITE);
    lv_obj_set_width(s_rt.peripheral_water, 127);
    lv_obj_align(s_rt.peripheral_water, LV_ALIGN_TOP_LEFT, 1, 0);
    s_rt.peripheral_water_bar = lv_bar_create(water);
    lv_obj_set_size(s_rt.peripheral_water_bar, 127, 14);
    lv_obj_align(s_rt.peripheral_water_bar, LV_ALIGN_TOP_LEFT, 1, 29);
    lv_bar_set_range(s_rt.peripheral_water_bar, 0, 1000);
    lv_obj_set_style_bg_color(s_rt.peripheral_water_bar,
                              lv_color_hex(CLR_GLASS_BORDER), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_rt.peripheral_water_bar,
                              lv_color_hex(CLR_CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_rt.peripheral_water_bar, LV_RADIUS_CIRCLE,
                            LV_PART_MAIN);
    lv_obj_set_style_radius(s_rt.peripheral_water_bar, LV_RADIUS_CIRCLE,
                            LV_PART_INDICATOR);
    lv_obj_t *water_meta = label_create(water, "流量  ----   液位 --",
                                        CLR_DIM);
    lv_obj_set_width(water_meta, 127);
    lv_obj_align(water_meta, LV_ALIGN_TOP_LEFT, 1, 48);

    lv_obj_t *drive = card_create(page, 163, 52, 149, 76);
    lv_obj_set_style_pad_all(drive, 2, 0);
    s_rt.peripheral_drive = label_create(drive, "位置  -- / --",
                                         CLR_WHITE);
    lv_obj_set_width(s_rt.peripheral_drive, 127);
    lv_obj_align(s_rt.peripheral_drive, LV_ALIGN_TOP_LEFT, 1, 0);
    s_rt.peripheral_drive_bar = lv_bar_create(drive);
    lv_obj_set_size(s_rt.peripheral_drive_bar, 127, 14);
    lv_obj_align(s_rt.peripheral_drive_bar, LV_ALIGN_TOP_LEFT, 1, 29);
    lv_bar_set_range(s_rt.peripheral_drive_bar, 0, 360);
    lv_obj_set_style_bg_color(s_rt.peripheral_drive_bar,
                              lv_color_hex(CLR_GLASS_BORDER), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_rt.peripheral_drive_bar,
                              lv_color_hex(CLR_GREEN), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_rt.peripheral_drive_bar, LV_RADIUS_CIRCLE,
                            LV_PART_MAIN);
    lv_obj_set_style_radius(s_rt.peripheral_drive_bar, LV_RADIUS_CIRCLE,
                            LV_PART_INDICATOR);
    lv_obj_t *drive_meta = label_create(drive, "PWM --%  BL50 --%",
                                        CLR_DIM);
    lv_obj_set_width(drive_meta, 127);
    lv_obj_align(drive_meta, LV_ALIGN_TOP_LEFT, 1, 48);

    lv_obj_t *outputs = card_create(page, 8, 134, 149, 82);
    lv_obj_set_style_pad_all(outputs, 2, 0);
    s_rt.peripheral_outputs = label_create(outputs,
        "进水 --  转移 --\n排水 --  洗剂 --\nUV --   加热 --", CLR_WHITE);
    lv_obj_set_width(s_rt.peripheral_outputs, 127);
    lv_obj_set_height(s_rt.peripheral_outputs, 65);
    lv_label_set_long_mode(s_rt.peripheral_outputs, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_rt.peripheral_outputs, LV_ALIGN_TOP_LEFT, 1, 0);

    lv_obj_t *system = card_create(page, 163, 134, 149, 82);
    lv_obj_set_style_pad_all(system, 2, 0);
    s_rt.peripheral_memory = label_create(system,
        "内存 --K\n温度 --.-C 湿度 --%\n风扇 --% 网络 --", CLR_WHITE);
    lv_obj_set_width(s_rt.peripheral_memory, 127);
    lv_obj_set_height(s_rt.peripheral_memory, 65);
    lv_label_set_long_mode(s_rt.peripheral_memory, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_rt.peripheral_memory, LV_ALIGN_TOP_LEFT, 1, 0);
    s_rt.peripheral_memory_bar = lv_bar_create(system);
    lv_obj_set_size(s_rt.peripheral_memory_bar, 127, 8);
    lv_obj_align(s_rt.peripheral_memory_bar, LV_ALIGN_TOP_LEFT, 1, 66);
    lv_bar_set_range(s_rt.peripheral_memory_bar, 0, 64);
    lv_obj_set_style_bg_color(s_rt.peripheral_memory_bar,
                              lv_color_hex(CLR_GLASS_BORDER), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_rt.peripheral_memory_bar,
                              lv_color_hex(CLR_ORANGE), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_rt.peripheral_memory_bar, LV_RADIUS_CIRCLE,
                            LV_PART_MAIN);
    lv_obj_set_style_radius(s_rt.peripheral_memory_bar, LV_RADIUS_CIRCLE,
                            LV_PART_INDICATOR);
    add_page_indicator(page, 2);
}

#endif

static lv_obj_t *telemetry_arc_create(lv_obj_t *parent, lv_coord_t x,
                                      lv_coord_t y, uint32_t color,
                                      int32_t min, int32_t max)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_pos(arc, x, y);
    lv_obj_set_size(arc, 44, 44);
    lv_arc_set_range(arc, min, max);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_value(arc, min);
    lv_obj_set_style_arc_width(arc, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(CLR_GLASS_BORDER),
                               LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_border_width(arc, 0, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return arc;
}

static lv_obj_t *telemetry_led_create(lv_obj_t *parent, lv_coord_t x,
                                      lv_coord_t y, const char *caption)
{
    lv_obj_t *led = lv_obj_create(parent);
    lv_obj_set_pos(led, x, y);
    lv_obj_set_size(led, 10, 10);
    lv_obj_set_style_radius(led, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(led, lv_color_hex(CLR_DIM), 0);
    lv_obj_set_style_bg_opa(led, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(led, lv_color_hex(CLR_GLASS_BORDER), 0);
    lv_obj_set_style_border_width(led, 1, 0);
    lv_obj_clear_flag(led, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *label = telemetry_label_create(parent, caption, CLR_DIM);
    lv_obj_set_pos(label, x + 13, y - 2);
    lv_obj_set_size(label, 30, 15);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    return led;
}

static void build_peripheral_page(void)
{
    lv_obj_t *page = s_rt.pages[2] = page_create();
    brand_header_create(page, "外设遥测 / 仪表", "03 / 04", 2);

    lv_obj_t *water = card_create(page, 8, 52, 149, 76);
    lv_obj_set_style_pad_all(water, 2, 0);
    s_rt.peripheral_water_arc = telemetry_arc_create(
        water, 7, 14, CLR_CYAN, 0, 1000);
    s_rt.peripheral_water = telemetry_label_create(water, "水量 --ml", CLR_WHITE);
    lv_obj_set_pos(s_rt.peripheral_water, 59, 16);
    lv_obj_set_size(s_rt.peripheral_water, 80, 16);
    lv_label_set_long_mode(s_rt.peripheral_water, LV_LABEL_LONG_CLIP);
    lv_obj_t *water_meta = telemetry_label_create(water, "流量 --", CLR_DIM);
    lv_obj_set_pos(water_meta, 59, 51);
    lv_obj_set_size(water_meta, 80, 16);

    lv_obj_t *drive = card_create(page, 163, 52, 149, 76);
    lv_obj_set_style_pad_all(drive, 2, 0);
    s_rt.peripheral_drive_arc = telemetry_arc_create(
        drive, 7, 14, CLR_GREEN, 0, 360);
    s_rt.peripheral_drive = telemetry_label_create(drive, "位置 --/--", CLR_WHITE);
    lv_obj_set_pos(s_rt.peripheral_drive, 59, 16);
    lv_obj_set_size(s_rt.peripheral_drive, 80, 16);
    lv_label_set_long_mode(s_rt.peripheral_drive, LV_LABEL_LONG_CLIP);
    lv_obj_t *drive_meta = telemetry_label_create(drive, "PWM --%", CLR_DIM);
    lv_obj_set_pos(drive_meta, 59, 51);
    lv_obj_set_size(drive_meta, 80, 16);

    lv_obj_t *outputs = card_create(page, 8, 134, 149, 82);
    lv_obj_set_style_pad_all(outputs, 2, 0);
    lv_obj_t *out_title = telemetry_label_create(outputs, "执行器", CLR_CYAN);
    lv_obj_set_pos(out_title, 4, 2);
    lv_obj_set_size(out_title, 125, 18);
    static const char *const output_names[6] = {
        "进", "转", "排", "剂", "UV", "热"
    };
    static const lv_coord_t output_x[6] = {4, 49, 94, 4, 49, 94};
    static const lv_coord_t output_y[6] = {27, 27, 27, 54, 54, 54};
    for (uint8_t i = 0; i < 6; ++i) {
        s_rt.peripheral_output_leds[i] = telemetry_led_create(
            outputs, output_x[i], output_y[i], output_names[i]);
    }

    lv_obj_t *system = card_create(page, 163, 134, 149, 82);
    lv_obj_set_style_pad_all(system, 2, 0);
    lv_obj_t *sys_title = telemetry_label_create(system, "系统", CLR_CYAN);
    lv_obj_set_pos(sys_title, 4, 2);
    lv_obj_set_size(sys_title, 70, 18);
    s_rt.peripheral_memory = telemetry_label_create(system, "内存 --K", CLR_WHITE);
    lv_obj_set_pos(s_rt.peripheral_memory, 4, 20);
    lv_obj_set_size(s_rt.peripheral_memory, 125, 18);
    s_rt.peripheral_memory_bar = lv_bar_create(system);
    lv_obj_set_pos(s_rt.peripheral_memory_bar, 4, 40);
    lv_obj_set_size(s_rt.peripheral_memory_bar, 125, 10);
    lv_bar_set_range(s_rt.peripheral_memory_bar, 0, 64);
    lv_obj_set_style_bg_color(s_rt.peripheral_memory_bar,
                              lv_color_hex(CLR_GLASS_BORDER), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_rt.peripheral_memory_bar,
                              lv_color_hex(CLR_ORANGE), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_rt.peripheral_memory_bar, LV_RADIUS_CIRCLE,
                            LV_PART_MAIN);
    lv_obj_set_style_radius(s_rt.peripheral_memory_bar, LV_RADIUS_CIRCLE,
                            LV_PART_INDICATOR);
    lv_obj_t *env = telemetry_label_create(system, "温湿 --", CLR_DIM);
    lv_obj_set_pos(env, 4, 54);
    lv_obj_set_size(env, 70, 18);
    s_rt.peripheral_env = env;
    s_rt.peripheral_wifi_led = telemetry_led_create(system, 86, 55, "网");
    s_rt.peripheral_ble_led = telemetry_led_create(system, 116, 55, "蓝");
    add_page_indicator(page, 2);
}

static void build_voice_page(void)
{
    lv_obj_t *page = s_rt.pages[3] = page_create();
    brand_header_create(page, "语音助手", "04 / 04", 3);

    lv_obj_t *state_card = card_create(page, 8, 52, 304, 60);
    lv_obj_set_style_pad_all(state_card, 2, 0);

    s_rt.voice_orbit = lv_obj_create(state_card);
    lv_obj_set_size(s_rt.voice_orbit, 42, 42);
    lv_obj_set_style_radius(s_rt.voice_orbit, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_rt.voice_orbit, LV_OPA_20, 0);
    lv_obj_set_style_bg_color(s_rt.voice_orbit, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_border_color(s_rt.voice_orbit, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_border_width(s_rt.voice_orbit, 3, 0);
    lv_obj_align(s_rt.voice_orbit, LV_ALIGN_LEFT_MID, 1, 0);
    lv_obj_clear_flag(s_rt.voice_orbit, LV_OBJ_FLAG_SCROLLABLE);

    s_rt.voice_core = lv_obj_create(state_card);
    lv_obj_set_size(s_rt.voice_core, 22, 22);
    lv_obj_set_style_radius(s_rt.voice_core, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_rt.voice_core, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_bg_opa(s_rt.voice_core, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_rt.voice_core, 0, 0);
    lv_obj_align(s_rt.voice_core, LV_ALIGN_LEFT_MID, 11, 0);
    lv_obj_clear_flag(s_rt.voice_core, LV_OBJ_FLAG_SCROLLABLE);

    s_rt.voice_state = telemetry_label_create(state_card, "待命", CLR_DIM);
    lv_obj_set_width(s_rt.voice_state, 234);
    lv_label_set_long_mode(s_rt.voice_state, LV_LABEL_LONG_DOT);
    lv_obj_align(s_rt.voice_state, LV_ALIGN_TOP_LEFT, 56, 2);
    s_rt.voice_backend = telemetry_label_create(state_card, "后端  Edge / 小净小净", CLR_CYAN);
    lv_obj_set_width(s_rt.voice_backend, 234);
    lv_label_set_long_mode(s_rt.voice_backend, LV_LABEL_LONG_DOT);
    lv_obj_align(s_rt.voice_backend, LV_ALIGN_TOP_LEFT, 56, 27);

    lv_obj_t *card = card_create(page, 8, 118, 304, 98);
    lv_obj_set_style_pad_all(card, 2, 0);
    s_rt.voice_transcript = telemetry_label_create(card, "你：--", CLR_DIM);
    lv_obj_set_width(s_rt.voice_transcript, 276);
    lv_obj_set_height(s_rt.voice_transcript, 18);
    lv_label_set_long_mode(s_rt.voice_transcript, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_rt.voice_transcript, LV_ALIGN_TOP_LEFT, 2, 0);
    divider_create(card, 22, 276);
    s_rt.voice_reply = telemetry_label_create(card, "小净：等待语音输入", CLR_WHITE);
    lv_obj_set_width(s_rt.voice_reply, 276);
    lv_obj_set_height(s_rt.voice_reply, 64);
    lv_label_set_long_mode(s_rt.voice_reply, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_rt.voice_reply, LV_ALIGN_TOP_LEFT, 2, 28);
    add_page_indicator(page, 3);
}

static const char *machine_state_name(uint8_t state)
{
    static const char *const names[] = {
        "启动", "待机", "进料", "运行", "取出",
        "消毒", "暂停", "复位", "故障"
    };
    return state < (sizeof(names) / sizeof(names[0])) ? names[state] : "未知";
}

static const char *output_state_name(uint8_t state)
{
    return state == 2U ? "ON" : state == 1U ? "OFF" : "--";
}

static void set_telemetry_led(lv_obj_t *led, uint8_t state)
{
    if (!led) return;
    uint32_t color = state == 2U ? CLR_GREEN
                    : state == 1U ? CLR_DIM : CLR_ORANGE;
    lv_obj_set_style_bg_color(led, lv_color_hex(color), 0);
}

static void format_position(char *buffer, size_t capacity, int16_t degrees)
{
    if (degrees < 0) snprintf(buffer, capacity, "--");
    else snprintf(buffer, capacity, "%d", (int)degrees);
}

static void format_duration(char *buffer, size_t capacity, uint32_t ms)
{
    uint32_t seconds = ms / 1000U;
    snprintf(buffer, capacity, "%02" PRIu32 ":%02" PRIu32,
             seconds / 60U, seconds % 60U);
}

static void format_environment(char *buffer, size_t capacity,
                               float temperature, float humidity, bool valid)
{
    if (!valid) {
        snprintf(buffer, capacity, "--");
        return;
    }
    int temp10 = (int)(temperature * 10.0f);
    int temp_whole = temp10 / 10;
    int temp_fraction = temp10 % 10;
    if (temp_fraction < 0) temp_fraction = -temp_fraction;
    int rh = (int)(humidity + (humidity >= 0.0f ? 0.5f : -0.5f));
    snprintf(buffer, capacity, "%d.%dC %d%%",
             temp_whole, temp_fraction, rh);
}

static void set_voice_visual(display_voice_state_t state)
{
    const char *name = "待命";
    uint32_t color = CLR_DIM;
    if (state == DISPLAY_VOICE_LISTENING) {
        name = "正在倾听"; color = CLR_CYAN;
    } else if (state == DISPLAY_VOICE_THINKING) {
        name = "思考中"; color = CLR_ORANGE;
    } else if (state == DISPLAY_VOICE_REPLY) {
        name = "小净回复"; color = CLR_GREEN;
    } else if (state == DISPLAY_VOICE_ERROR) {
        name = "语音异常"; color = CLR_RED;
    }
    lv_label_set_text(s_rt.voice_state, name);
    lv_obj_set_style_text_color(s_rt.voice_state, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(s_rt.voice_core, lv_color_hex(color), 0);
    lv_obj_set_style_border_color(s_rt.voice_orbit, lv_color_hex(color), 0);
}

static void update_heartbeat(void)
{
    s_rt.heartbeat_phase = (uint8_t)((s_rt.heartbeat_phase + 1U) % 12U);
    uint8_t phase = s_rt.heartbeat_phase;
    uint8_t level = phase <= 6U ? (uint8_t)(90U + phase * 27U)
                                : (uint8_t)(90U + (12U - phase) * 27U);
    for (uint8_t i = 0; i < DISPLAY_PAGE_COUNT; ++i) {
        if (!s_rt.heartbeat[i]) continue;
        lv_obj_set_style_opa(s_rt.heartbeat[i], (lv_opa_t)level, 0);
    }
}

#if 0
static void refresh_ui_legacy(lv_timer_t *timer)
{
    (void)timer;
    update_heartbeat();
    display_model_t model;
    if (!model_lock()) return;
    model = s_model;
    xSemaphoreGive(s_model_lock);
    int requested_page = atomic_load_explicit(&s_requested_page,
                                              memory_order_acquire);
    bool page_pending = requested_page >= DISPLAY_PAGE_STATUS &&
                        requested_page < DISPLAY_PAGE_COUNT &&
                        requested_page != atomic_load_explicit(
                            &s_current_page, memory_order_acquire);
    /* A page request is an independent UI mutation.  Do not let the model
     * revision fast-path suppress a gesture/button page change. */
    if (model.revision == s_rt.rendered_revision && !page_pending) return;

    lv_label_set_text_fmt(s_rt.status_wifi, "WiFi  %s",
                          model.wifi_connected ? "ONLINE" : "OFFLINE");
    lv_obj_set_style_text_color(s_rt.status_wifi,
        lv_color_hex(model.wifi_connected ? CLR_GREEN : CLR_RED), 0);
    lv_label_set_text_fmt(s_rt.status_ble, "BLE   %s",
                          model.ble_connected ? "ONLINE" : "OFFLINE");
    lv_obj_set_style_text_color(s_rt.status_ble,
        lv_color_hex(model.ble_connected ? CLR_GREEN : CLR_RED), 0);
    lv_label_set_text_fmt(s_rt.status_wash, "系统  %s",
        machine_state_name(model.telemetry.machine_state));
    lv_obj_set_style_text_color(s_rt.status_wash,
        lv_color_hex(model.washing ? CLR_ORANGE : CLR_GREEN), 0);

    if (model.telemetry.environment_valid) {
        lv_label_set_text_fmt(s_rt.status_env, "环境  %.1fC  %.0f%%",
                              (double)model.telemetry.temperature_c,
                              (double)model.telemetry.humidity_rh);
        lv_obj_set_style_text_color(s_rt.status_env, lv_color_hex(CLR_WHITE), 0);
    } else {
        lv_label_set_text(s_rt.status_env, "环境  --");
        lv_obj_set_style_text_color(s_rt.status_env, lv_color_hex(CLR_DIM), 0);
    }
    char current_pos[12];
    char target_pos[12];
    format_position(current_pos, sizeof(current_pos),
                    model.telemetry.position_degrees);
    format_position(target_pos, sizeof(target_pos),
                    model.telemetry.target_position_degrees);
    lv_label_set_text_fmt(s_rt.status_position, "姿态  %s", current_pos);
    lv_obj_set_style_text_color(s_rt.status_position,
        lv_color_hex(model.telemetry.position_degrees < 0 ? CLR_DIM : CLR_CYAN), 0);
    if (model.telemetry.emergency_stop) {
        lv_label_set_text(s_rt.status_fault, "安全  急停锁定");
        lv_obj_set_style_text_color(s_rt.status_fault, lv_color_hex(CLR_RED), 0);
    } else if (model.telemetry.safety_fault) {
        lv_label_set_text_fmt(s_rt.status_fault, "安全  故障 E%u",
                              (unsigned)model.telemetry.fault_code);
        lv_obj_set_style_text_color(s_rt.status_fault, lv_color_hex(CLR_RED), 0);
    } else {
        lv_label_set_text(s_rt.status_fault, "安全  联锁正常");
        lv_obj_set_style_text_color(s_rt.status_fault, lv_color_hex(CLR_GREEN), 0);
    }

    lv_bar_set_value(s_rt.wash_bar, model.wash_progress, LV_ANIM_ON);
    lv_label_set_text_fmt(s_rt.wash_percent, "%u%%", model.wash_progress);
    lv_label_set_text_fmt(s_rt.wash_step, "步骤  %s",
                          model.wash_step[0] ? model.wash_step : "待机");
    lv_label_set_text_fmt(s_rt.wash_position, "姿态  %s → %s",
                          current_pos, target_pos);
    char elapsed[16];
    char remaining[16];
    format_duration(elapsed, sizeof(elapsed), model.telemetry.elapsed_ms);
    format_duration(remaining, sizeof(remaining), model.telemetry.remaining_ms);
    lv_label_set_text_fmt(s_rt.wash_timing, "时间  %s / %s",
                          elapsed, remaining);

    lv_label_set_text_fmt(s_rt.peripheral_water,
        "水路\n液位 %s\n流量 %" PRIu32 " ml",
        !model.telemetry.water_level_valid ? "--" :
        model.telemetry.water_full ? "FULL" : "NORMAL",
        model.telemetry.water_ml);
    lv_label_set_text_fmt(s_rt.peripheral_drive,
        "驱动\n姿态 %u%%  BL50 %u%%\n风扇 %u%%",
        (unsigned)model.telemetry.position_pwm,
        (unsigned)model.telemetry.bl50_pwm,
        (unsigned)model.telemetry.fan_percent);
    lv_label_set_text_fmt(s_rt.peripheral_outputs,
        "执行器\n进水 %s  转移 %s\n排水 %s  洗剂 %s\nUV   %s  加热 %s",
        output_state_name(model.telemetry.source_valve_state),
        output_state_name(model.telemetry.transfer_valve_state),
        output_state_name(model.telemetry.drain_state),
        output_state_name(model.telemetry.detergent_state),
        output_state_name(model.telemetry.uv_state),
        output_state_name(model.telemetry.heater_state));
    lv_label_set_text_fmt(s_rt.peripheral_memory, "内存  %" PRIu32 " KB",
                          model.telemetry.internal_free_bytes / 1024U);

    set_voice_visual(model.voice_state);
    if (model.telemetry.backend_switching) {
        lv_label_set_text(s_rt.voice_backend, "后端  切换中…");
        lv_obj_set_style_text_color(s_rt.voice_backend,
                                    lv_color_hex(CLR_ORANGE), 0);
    } else if (model.telemetry.wake_backend == DISPLAY_WAKE_ESP_SR) {
        lv_label_set_text(s_rt.voice_backend, "后端  ESP-SR / 小爱同学");
        lv_obj_set_style_text_color(s_rt.voice_backend,
                                    lv_color_hex(CLR_CYAN), 0);
    } else {
        lv_label_set_text(s_rt.voice_backend, "后端  Edge / 小净小净");
        lv_obj_set_style_text_color(s_rt.voice_backend,
                                    lv_color_hex(CLR_CYAN), 0);
    }
    lv_label_set_text_fmt(s_rt.voice_transcript, "你：%s",
        model.transcript[0] ? model.transcript : "--");
    const char *reply = model.voice_state == DISPLAY_VOICE_ERROR
        ? model.error : model.reply;
    lv_label_set_text_fmt(s_rt.voice_reply, "小净：%s",
        reply[0] ? reply : "等待语音输入");
    int page_index = requested_page;
    if (page_index < 0 || page_index >= DISPLAY_PAGE_COUNT) {
        page_index = DISPLAY_PAGE_STATUS;
        if (model.voice_state != DISPLAY_VOICE_IDLE)
            page_index = DISPLAY_PAGE_VOICE;
        else if (model.washing)
            page_index = DISPLAY_PAGE_WASH;
    }
    lv_obj_t *target_page = s_rt.pages[page_index];
    if (lv_scr_act() != target_page) lv_scr_load(target_page);
    atomic_store_explicit(&s_current_page, page_index, memory_order_release);
    s_rt.rendered_revision = model.revision;
}

#endif
static void refresh_ui(lv_timer_t *timer)
{
    (void)timer;
    update_heartbeat();

    display_model_t model;
    if (!model_lock()) return;
    model = s_model;
    xSemaphoreGive(s_model_lock);

    int requested_page = atomic_load_explicit(&s_requested_page,
                                              memory_order_acquire);
    bool page_pending = requested_page >= DISPLAY_PAGE_STATUS &&
                        requested_page < DISPLAY_PAGE_COUNT &&
                        requested_page != atomic_load_explicit(
                            &s_current_page, memory_order_acquire);
    if (model.revision == s_rt.rendered_revision && !page_pending) return;

    lv_label_set_text_fmt(s_rt.status_wifi, "WiFi  %s",
                          model.wifi_connected ? "ONLINE" : "OFFLINE");
    lv_obj_set_style_text_color(s_rt.status_wifi,
        lv_color_hex(model.wifi_connected ? CLR_GREEN : CLR_RED), 0);
    lv_label_set_text_fmt(s_rt.status_ble, "BLE   %s",
                          model.ble_connected ? "ONLINE" : "OFFLINE");
    lv_obj_set_style_text_color(s_rt.status_ble,
        lv_color_hex(model.ble_connected ? CLR_GREEN : CLR_RED), 0);
    lv_label_set_text_fmt(s_rt.status_wash, "状态  %s",
                          machine_state_name(model.telemetry.machine_state));
    lv_obj_set_style_text_color(s_rt.status_wash,
        lv_color_hex(model.washing ? CLR_ORANGE : CLR_GREEN), 0);
    lv_label_set_text_fmt(s_rt.status_program, "进度  %u%%",
                          (unsigned)model.wash_progress);
    lv_obj_set_style_text_color(s_rt.status_program,
        lv_color_hex(model.washing ? CLR_CYAN : CLR_DIM), 0);

    char env_text[24];
    format_environment(env_text, sizeof(env_text),
                       model.telemetry.temperature_c,
                       model.telemetry.humidity_rh,
                       model.telemetry.environment_valid);
    if (model.telemetry.environment_valid) {
        lv_label_set_text_fmt(s_rt.status_env, "环境  %s", env_text);
        lv_obj_set_style_text_color(s_rt.status_env,
                                    lv_color_hex(CLR_WHITE), 0);
    } else {
        lv_label_set_text(s_rt.status_env, "环境  --");
        lv_obj_set_style_text_color(s_rt.status_env,
                                    lv_color_hex(CLR_DIM), 0);
    }

    char current_pos[12];
    char target_pos[12];
    format_position(current_pos, sizeof(current_pos),
                    model.telemetry.position_degrees);
    format_position(target_pos, sizeof(target_pos),
                    model.telemetry.target_position_degrees);
    lv_label_set_text_fmt(s_rt.status_position, "姿态  %s", current_pos);
    lv_obj_set_style_text_color(s_rt.status_position,
        lv_color_hex(model.telemetry.position_degrees < 0
                         ? CLR_DIM : CLR_CYAN), 0);

    if (model.telemetry.emergency_stop) {
        lv_label_set_text(s_rt.status_fault, "安全  急停");
        lv_obj_set_style_text_color(s_rt.status_fault, lv_color_hex(CLR_RED), 0);
    } else if (model.telemetry.safety_fault) {
        lv_label_set_text_fmt(s_rt.status_fault, "安全  故障 E%u",
                              (unsigned)model.telemetry.fault_code);
        lv_obj_set_style_text_color(s_rt.status_fault, lv_color_hex(CLR_RED), 0);
    } else {
        lv_label_set_text(s_rt.status_fault, "安全  正常");
        lv_obj_set_style_text_color(s_rt.status_fault,
                                    lv_color_hex(CLR_GREEN), 0);
    }
    if (model.telemetry.backend_switching) {
        lv_label_set_text(s_rt.status_voice, "唤醒  切换中");
        lv_obj_set_style_text_color(s_rt.status_voice,
                                    lv_color_hex(CLR_ORANGE), 0);
    } else if (model.telemetry.wake_backend == DISPLAY_WAKE_ESP_SR) {
        lv_label_set_text(s_rt.status_voice, "唤醒  小爱");
        lv_obj_set_style_text_color(s_rt.status_voice,
                                    lv_color_hex(CLR_CYAN), 0);
    } else {
        lv_label_set_text(s_rt.status_voice, "唤醒  小净");
        lv_obj_set_style_text_color(s_rt.status_voice,
                                    lv_color_hex(CLR_CYAN), 0);
    }

    lv_bar_set_value(s_rt.wash_bar, model.wash_progress, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_rt.wash_percent, "%u%%", model.wash_progress);
    lv_label_set_text_fmt(s_rt.wash_step, "步骤  %s",
                          model.wash_step[0] ? model.wash_step : "IDLE");
    lv_label_set_text_fmt(s_rt.wash_position, "姿态  %s -> %s",
                          current_pos, target_pos);
    char elapsed[16];
    char remaining[16];
    format_duration(elapsed, sizeof(elapsed), model.telemetry.elapsed_ms);
    format_duration(remaining, sizeof(remaining), model.telemetry.remaining_ms);
    lv_label_set_text_fmt(s_rt.wash_timing, "时间  %s / %s",
                          elapsed, remaining);

    lv_label_set_text_fmt(s_rt.peripheral_water,
        "水量 %" PRIu32 "ml",
        model.telemetry.water_ml);
    lv_label_set_text_fmt(s_rt.peripheral_drive,
        "位置 %s/%s",
        current_pos, target_pos);
    if (s_rt.peripheral_outputs) {
        lv_label_set_text_fmt(s_rt.peripheral_outputs,
            "进:%s 转:%s\n排:%s 剂:%s\nUV:%s 热:%s",
            output_state_name(model.telemetry.source_valve_state),
            output_state_name(model.telemetry.transfer_valve_state),
            output_state_name(model.telemetry.drain_state),
            output_state_name(model.telemetry.detergent_state),
            output_state_name(model.telemetry.uv_state),
            output_state_name(model.telemetry.heater_state));
    }
    char peripheral_env[24];
    format_environment(peripheral_env, sizeof(peripheral_env),
                       model.telemetry.temperature_c,
                       model.telemetry.humidity_rh,
                       model.telemetry.environment_valid);
    lv_label_set_text_fmt(s_rt.peripheral_memory, "内存 %" PRIu32 "K",
        model.telemetry.internal_free_bytes / 1024U);
    if (s_rt.peripheral_env)
        lv_label_set_text_fmt(s_rt.peripheral_env, "温湿 %s", peripheral_env);
    if (s_rt.peripheral_water_arc) {
        uint32_t water = model.telemetry.water_ml > 1000U
            ? 1000U : model.telemetry.water_ml;
        lv_arc_set_value(s_rt.peripheral_water_arc, (int32_t)water);
    }
    if (s_rt.peripheral_drive_arc) {
        int32_t pos = model.telemetry.position_degrees < 0
            ? 0 : model.telemetry.position_degrees;
        if (pos > 360) pos = 360;
        lv_arc_set_value(s_rt.peripheral_drive_arc, pos);
    }
    set_telemetry_led(s_rt.peripheral_output_leds[0],
                      model.telemetry.source_valve_state);
    set_telemetry_led(s_rt.peripheral_output_leds[1],
                      model.telemetry.transfer_valve_state);
    set_telemetry_led(s_rt.peripheral_output_leds[2],
                      model.telemetry.drain_state);
    set_telemetry_led(s_rt.peripheral_output_leds[3],
                      model.telemetry.detergent_state);
    set_telemetry_led(s_rt.peripheral_output_leds[4],
                      model.telemetry.uv_state);
    set_telemetry_led(s_rt.peripheral_output_leds[5],
                      model.telemetry.heater_state);
    set_telemetry_led(s_rt.peripheral_wifi_led, model.wifi_connected ? 2U : 1U);
    set_telemetry_led(s_rt.peripheral_ble_led, model.ble_connected ? 2U : 1U);
    if (s_rt.peripheral_water_bar) {
        uint32_t water = model.telemetry.water_ml > 1000U
            ? 1000U : model.telemetry.water_ml;
        lv_bar_set_value(s_rt.peripheral_water_bar, (int32_t)water,
                         LV_ANIM_OFF);
    }
    if (s_rt.peripheral_drive_bar) {
        int32_t pos = model.telemetry.position_degrees < 0
            ? 0 : model.telemetry.position_degrees;
        if (pos > 360) pos = 360;
        lv_bar_set_value(s_rt.peripheral_drive_bar, pos, LV_ANIM_OFF);
    }
    if (s_rt.peripheral_memory_bar) {
        uint32_t free_kb = model.telemetry.internal_free_bytes / 1024U;
        if (free_kb > 64U) free_kb = 64U;
        lv_bar_set_value(s_rt.peripheral_memory_bar, (int32_t)free_kb,
                         LV_ANIM_OFF);
    }

    set_voice_visual(model.voice_state);
    if (model.telemetry.backend_switching) {
        lv_label_set_text(s_rt.voice_backend, "后端切换中");
        lv_obj_set_style_text_color(s_rt.voice_backend,
                                    lv_color_hex(CLR_ORANGE), 0);
    } else if (model.telemetry.wake_backend == DISPLAY_WAKE_ESP_SR) {
        lv_label_set_text(s_rt.voice_backend, "后端 ESP-SR / 小爱同学");
        lv_obj_set_style_text_color(s_rt.voice_backend,
                                    lv_color_hex(CLR_CYAN), 0);
    } else {
        lv_label_set_text(s_rt.voice_backend, "后端 Edge / 小净小净");
        lv_obj_set_style_text_color(s_rt.voice_backend,
                                    lv_color_hex(CLR_CYAN), 0);
    }

    char transcript[100];
    char reply_text[132];
    copy_display_text(transcript, sizeof(transcript),
                      model.transcript[0] ? model.transcript : "--", 92U);
    copy_display_text(reply_text, sizeof(reply_text),
                      model.voice_state == DISPLAY_VOICE_ERROR
                          ? model.error : model.reply,
                      124U);
    lv_label_set_text_fmt(s_rt.voice_transcript, "你：%s", transcript);
    if (reply_text[0] == '\0')
        copy_text(reply_text, sizeof(reply_text), "READY");
    lv_label_set_text_fmt(s_rt.voice_reply, "小净：%s", reply_text);

    int page_index = requested_page;
    if (page_index < 0 || page_index >= DISPLAY_PAGE_COUNT) {
        page_index = DISPLAY_PAGE_STATUS;
        if (model.voice_state != DISPLAY_VOICE_IDLE)
            page_index = DISPLAY_PAGE_VOICE;
        else if (model.washing)
            page_index = DISPLAY_PAGE_WASH;
    }
    lv_obj_t *target_page = s_rt.pages[page_index];
    if (lv_scr_act() != target_page) lv_scr_load(target_page);
    atomic_store_explicit(&s_current_page, page_index, memory_order_release);
    s_rt.rendered_revision = model.revision;
}

static esp_err_t hardware_prepare(void)
{
    spi_bus_config_t bus = ILI9341_PANEL_BUS_SPI_CONFIG(
        XIAOJING_GPIO_LCD_SCK, XIAOJING_GPIO_LCD_MOSI,
        LCD_HRES * LCD_BUFFER_LINES * sizeof(lv_color_t));
    /* Do not enable SPI DMA for the LCD.  With a PSRAM LVGL canvas the DMA
     * driver otherwise allocates an internal bounce buffer for every flush;
     * after BLE/Wi-Fi/ESP-SR there may be only a few hundred bytes left.
     * Polling transfers are safe for this small 320x240 status UI and keep
     * the audio I2S DMA path completely independent. */
    esp_err_t err = spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_DISABLED);
    if (err != ESP_OK) return err;
    s_rt.spi_owned = true;

    esp_lcd_panel_io_spi_config_t io_cfg = ILI9341_PANEL_IO_SPI_CONFIG(
        XIAOJING_GPIO_LCD_CS, XIAOJING_GPIO_LCD_DC, NULL, NULL);
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                   &io_cfg, &s_rt.io);
    if (err != ESP_OK) return err;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_endian = LCD_RGB_ENDIAN_BGR,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_ili9341(s_rt.io, &panel_cfg, &s_rt.panel);
    if (err != ESP_OK) return err;
    if ((err = esp_lcd_panel_reset(s_rt.panel)) != ESP_OK ||
        (err = esp_lcd_panel_init(s_rt.panel)) != ESP_OK ||
        (err = esp_lcd_panel_disp_on_off(s_rt.panel, true)) != ESP_OK)
        return err;

    s_rt.dma_reserve = heap_caps_malloc(LCD_DMA_RESERVE_BYTES,
                                        MALLOC_CAP_DMA);
    if (!s_rt.dma_reserve) return ESP_ERR_NO_MEM;

    return ESP_OK;
}

static esp_err_t hardware_build_ui(void)
{
    if (!s_rt.panel || !s_rt.io) return ESP_ERR_INVALID_STATE;
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_affinity = 1;
    port_cfg.task_priority = 4;
    port_cfg.task_stack_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    esp_err_t err = lvgl_port_init(&port_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed: 0x%x (largest internal=%u)",
                 err, (unsigned)heap_caps_get_largest_free_block(
                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        return err;
    }
    s_rt.lvgl_owned = true;
    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_rt.io,
        .panel_handle = s_rt.panel,
        .buffer_size = LCD_HRES * LCD_BUFFER_LINES,
        .double_buffer = false,
        /* LVGL can flush a full-width rectangle.  The managed lvgl_port
         * computes max_line = trans_size / width, so trans_size must be at
         * least one full display row; smaller values can divide by zero. */
        .trans_size = LCD_HRES,
        .hres = LCD_HRES,
        .vres = LCD_VRES,
        .monochrome = false,
        .rotation = {
            .swap_xy = true,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
        },
    };
    /* Release the early reservation immediately before esp_lvgl_port
     * allocates its one-line internal DMA transport buffer. */
    free(s_rt.dma_reserve);
    s_rt.dma_reserve = NULL;
    s_rt.display = lvgl_port_add_disp(&disp_cfg);
    if (!s_rt.display) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed (largest DMA=%u)",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
        return ESP_ERR_NO_MEM;
    }
    if (!lvgl_port_lock(1000U)) return ESP_ERR_TIMEOUT;
    build_status_page();
    build_wash_page();
    build_peripheral_page();
    build_voice_page();
    lv_scr_load(s_rt.pages[0]);
    atomic_store_explicit(&s_current_page, DISPLAY_PAGE_STATUS,
                          memory_order_release);
    s_rt.refresh_timer = lv_timer_create(refresh_ui, 100U, NULL);
    lvgl_port_unlock();
    if (!s_rt.refresh_timer) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

static esp_err_t hardware_stop(void)
{
    esp_err_t first = ESP_OK;
    if (s_rt.dma_reserve) {
        free(s_rt.dma_reserve);
        s_rt.dma_reserve = NULL;
    }
    if (s_rt.lvgl_owned && lvgl_port_lock(1000U)) {
        if (s_rt.refresh_timer) {
            lv_timer_del(s_rt.refresh_timer);
            s_rt.refresh_timer = NULL;
        }
        lvgl_port_unlock();
    }
    if (s_rt.display) {
        esp_err_t err = lvgl_port_remove_disp(s_rt.display);
        if (first == ESP_OK) first = err;
        if (err == ESP_OK) s_rt.display = NULL;
    }
    if (s_rt.lvgl_owned) {
        esp_err_t err = lvgl_port_deinit();
        if (first == ESP_OK) first = err;
        if (err == ESP_OK) s_rt.lvgl_owned = false;
    }
    if (s_rt.panel) {
        esp_err_t err = esp_lcd_panel_del(s_rt.panel);
        if (first == ESP_OK) first = err;
        if (err == ESP_OK) s_rt.panel = NULL;
    }
    if (s_rt.io) {
        esp_err_t err = esp_lcd_panel_io_del(s_rt.io);
        if (first == ESP_OK) first = err;
        if (err == ESP_OK) s_rt.io = NULL;
    }
    if (s_rt.spi_owned) {
        esp_err_t err = spi_bus_free(LCD_HOST);
        if (first == ESP_OK) first = err;
        if (err == ESP_OK) s_rt.spi_owned = false;
    }
    if (first == ESP_OK) {
        memset(s_rt.pages, 0, sizeof(s_rt.pages));
        s_rt.status_wifi = NULL;
        s_rt.status_ble = NULL;
        s_rt.status_wash = NULL;
        s_rt.status_program = NULL;
        s_rt.status_fault = NULL;
        s_rt.status_voice = NULL;
        s_rt.status_env = NULL;
        s_rt.status_position = NULL;
        s_rt.wash_bar = NULL;
        s_rt.wash_percent = NULL;
        s_rt.wash_step = NULL;
        s_rt.wash_position = NULL;
        s_rt.wash_timing = NULL;
        s_rt.peripheral_water = NULL;
        s_rt.peripheral_drive = NULL;
        s_rt.peripheral_outputs = NULL;
        s_rt.peripheral_memory = NULL;
        s_rt.peripheral_env = NULL;
        s_rt.peripheral_water_bar = NULL;
        s_rt.peripheral_drive_bar = NULL;
        s_rt.peripheral_memory_bar = NULL;
        s_rt.peripheral_water_arc = NULL;
        s_rt.peripheral_drive_arc = NULL;
        memset(s_rt.peripheral_output_leds, 0,
               sizeof(s_rt.peripheral_output_leds));
        s_rt.peripheral_wifi_led = NULL;
        s_rt.peripheral_ble_led = NULL;
        memset(s_rt.heartbeat, 0, sizeof(s_rt.heartbeat));
        s_rt.voice_orbit = NULL;
        s_rt.voice_core = NULL;
        s_rt.voice_state = NULL;
        s_rt.voice_backend = NULL;
        s_rt.voice_transcript = NULL;
        s_rt.voice_reply = NULL;
        s_rt.rendered_revision = 0U;
        s_rt.heartbeat_phase = 0U;
    }
    return first;
}

esp_err_t display_service_init(void)
{
    int expected = 0;
    if (atomic_compare_exchange_strong(&s_init_state, &expected, 1)) {
        s_model_lock = xSemaphoreCreateMutexStatic(&s_model_lock_storage);
        if (!s_model_lock) {
            atomic_store(&s_init_state, -1);
            return ESP_ERR_NO_MEM;
        }
        memset(&s_model, 0, sizeof(s_model));
        memset(&s_rt, 0, sizeof(s_rt));
        s_model.revision = 1U;
        atomic_init(&s_requested_page, -1);
        atomic_init(&s_current_page, DISPLAY_PAGE_STATUS);
        s_rt.lifecycle = DISPLAY_LC_INITIALIZED;
        s_rt.hardware_enabled = board_config_is_display_enabled();
        atomic_store(&s_init_state, 2);
        return ESP_OK;
    }
    return atomic_load(&s_init_state) == 2 ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t display_service_start(void)
{
    if (atomic_load(&s_init_state) != 2) return ESP_ERR_INVALID_STATE;
    if (s_rt.lifecycle == DISPLAY_LC_RUNNING) return ESP_OK;
    if (s_rt.lifecycle != DISPLAY_LC_INITIALIZED)
        return ESP_ERR_INVALID_STATE;
    esp_err_t err = display_service_prepare();
    if (err == ESP_OK && s_rt.hardware_enabled)
        err = hardware_build_ui();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ILI9341 start failed: 0x%x", err);
        (void)hardware_stop();
        return err;
    }
    s_rt.lifecycle = DISPLAY_LC_RUNNING;
    ESP_LOGI(TAG, "display running (hardware=%s CJK=%s)",
             s_rt.hardware_enabled ? "on" : "off",
             LV_FONT_SIMSUN_16_CJK ? "on" : "off");
    return ESP_OK;
}

esp_err_t display_service_prepare(void)
{
    if (atomic_load(&s_init_state) != 2) return ESP_ERR_INVALID_STATE;
    if (!s_rt.hardware_enabled || s_rt.panel) return ESP_OK;
    if (s_rt.lifecycle != DISPLAY_LC_INITIALIZED)
        return ESP_ERR_INVALID_STATE;
    esp_err_t err = hardware_prepare();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ILI9341 prepare failed: 0x%x", err);
        (void)hardware_stop();
    }
    return err;
}

esp_err_t display_service_stop(void)
{
    if (atomic_load(&s_init_state) != 2) return ESP_OK;
    if (s_rt.lifecycle == DISPLAY_LC_INITIALIZED) {
        return s_rt.display || s_rt.spi_owned || s_rt.lvgl_owned
            ? hardware_stop() : ESP_OK;
    }
    if (s_rt.lifecycle != DISPLAY_LC_RUNNING &&
        s_rt.lifecycle != DISPLAY_LC_STOPPING)
        return ESP_ERR_INVALID_STATE;
    s_rt.lifecycle = DISPLAY_LC_STOPPING;
    esp_err_t err = s_rt.hardware_enabled ? hardware_stop() : ESP_OK;
    if (err == ESP_OK) s_rt.lifecycle = DISPLAY_LC_INITIALIZED;
    return err;
}

esp_err_t display_service_set_voice_state(display_voice_state_t state)
{
    if (state > DISPLAY_VOICE_ERROR) return ESP_ERR_INVALID_ARG;
    if (!model_lock()) return ESP_ERR_INVALID_STATE;
    s_model.voice_state = state;
    if (state != DISPLAY_VOICE_ERROR) s_model.error[0] = '\0';
    if (state == DISPLAY_VOICE_LISTENING) {
        /* A new utterance owns the page.  Never present stale text or an old
         * failure while the microphone is recording the next turn. */
        s_model.transcript[0] = '\0';
        s_model.reply[0] = '\0';
    }
    s_model.revision = next_revision(s_model.revision);
    xSemaphoreGive(s_model_lock);
    return ESP_OK;
}

esp_err_t display_service_set_voice_text(const char *transcript,
                                         const char *reply)
{
    if (!transcript || !reply) return ESP_ERR_INVALID_ARG;
    if (!model_lock()) return ESP_ERR_INVALID_STATE;
    copy_text(s_model.transcript, sizeof(s_model.transcript), transcript);
    copy_text(s_model.reply, sizeof(s_model.reply), reply);
    s_model.error[0] = '\0';
    s_model.voice_state = DISPLAY_VOICE_REPLY;
    s_model.revision = next_revision(s_model.revision);
    xSemaphoreGive(s_model_lock);
    return ESP_OK;
}

esp_err_t display_service_set_error(const char *message)
{
    if (!message) return ESP_ERR_INVALID_ARG;
    if (!model_lock()) return ESP_ERR_INVALID_STATE;
    copy_text(s_model.error, sizeof(s_model.error), message);
    s_model.voice_state = DISPLAY_VOICE_ERROR;
    s_model.revision = next_revision(s_model.revision);
    xSemaphoreGive(s_model_lock);
    return ESP_OK;
}

esp_err_t display_service_set_connectivity(bool wifi_connected,
                                           bool ble_connected)
{
    if (!model_lock()) return ESP_ERR_INVALID_STATE;
    s_model.wifi_connected = wifi_connected;
    s_model.ble_connected = ble_connected;
    s_model.revision = next_revision(s_model.revision);
    xSemaphoreGive(s_model_lock);
    return ESP_OK;
}

bool display_service_connectivity_matches(bool wifi_connected,
                                          bool ble_connected)
{
    if (!model_lock()) return false;
    bool matches = s_model.wifi_connected == wifi_connected &&
                   s_model.ble_connected == ble_connected;
    xSemaphoreGive(s_model_lock);
    return matches;
}

esp_err_t display_service_set_wash(bool washing, uint8_t progress,
                                   const char *step)
{
    if (!step || progress > 100U) return ESP_ERR_INVALID_ARG;
    if (!model_lock()) return ESP_ERR_INVALID_STATE;
    s_model.washing = washing;
    s_model.wash_progress = progress;
    copy_text(s_model.wash_step, sizeof(s_model.wash_step), step);
    s_model.revision = next_revision(s_model.revision);
    xSemaphoreGive(s_model_lock);
    return ESP_OK;
}

esp_err_t display_service_set_telemetry(const display_telemetry_t *telemetry)
{
    if (!telemetry || telemetry->wake_backend > DISPLAY_WAKE_ESP_SR)
        return ESP_ERR_INVALID_ARG;
    if (!model_lock()) return ESP_ERR_INVALID_STATE;
    s_model.telemetry = *telemetry;
    s_model.revision = next_revision(s_model.revision);
    xSemaphoreGive(s_model_lock);
    return ESP_OK;
}

esp_err_t display_service_show_page(display_page_t page)
{
    if (page < DISPLAY_PAGE_STATUS || page >= DISPLAY_PAGE_COUNT)
        return ESP_ERR_INVALID_ARG;
    if (atomic_load_explicit(&s_init_state, memory_order_acquire) != 2)
        return ESP_ERR_INVALID_STATE;
    atomic_store_explicit(&s_requested_page, (int)page,
                          memory_order_release);
    if (!model_lock()) return ESP_ERR_TIMEOUT;
    s_model.revision = next_revision(s_model.revision);
    xSemaphoreGive(s_model_lock);
    return ESP_OK;
}

esp_err_t display_service_step_page(int8_t direction)
{
    if (direction != -1 && direction != 1) return ESP_ERR_INVALID_ARG;
    int current = atomic_load_explicit(&s_current_page, memory_order_acquire);
    int requested = atomic_load_explicit(&s_requested_page,
                                         memory_order_acquire);
    if (requested >= DISPLAY_PAGE_STATUS && requested < DISPLAY_PAGE_COUNT)
        current = requested;
    int next = (current + direction + DISPLAY_PAGE_COUNT) % DISPLAY_PAGE_COUNT;
    return display_service_show_page((display_page_t)next);
}

esp_err_t display_service_get_model(display_model_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!model_lock()) return ESP_ERR_INVALID_STATE;
    *out = s_model;
    xSemaphoreGive(s_model_lock);
    return ESP_OK;
}

#ifdef XIAOJING_TESTING
void display_service_test_reset(void)
{
    (void)display_service_stop();
    if (model_lock()) {
        memset(&s_model, 0, sizeof(s_model));
        s_model.revision = 1U;
        xSemaphoreGive(s_model_lock);
    }
}
#endif
