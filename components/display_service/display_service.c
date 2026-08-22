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
#define LCD_BUFFER_LINES 8U
    /* Reserve two RGB565 scan lines before BLE starts. */
#define LCD_DMA_RESERVE_BYTES (2U * LCD_HRES * sizeof(lv_color_t))
#define MODEL_LOCK_TIMEOUT_MS 100U

/* High-contrast industrial HMI palette. */
#define CLR_BG_DEEP      0x07111f
#define CLR_GLASS_BG     0x111d2e
#define CLR_GLASS_BORDER 0x31506b
#define CLR_CARD_BORDER  0x1c3a56   /* 普通卡片暗蓝 1px 边框 */
#define CLR_CYAN         0x00c8ff
#define CLR_BLUE         0x00a2ff
#define CLR_BLUEGRAY     0x7a93b5
#define CLR_WHITE        0xf2f7fb
#define CLR_ON_PRIMARY   0x03131d
#define CLR_DIM          0x93a9bb
#define CLR_GREEN        0x2ee58b
#define CLR_DARKGREEN    0x1c6b45   /* 流程已完成=暗绿 */
#define CLR_ORANGE       0xffb020
#define CLR_RED          0xff5b66

/* Header geometry is identical on all three pages so the progress bar starts
 * and ends at the same pixel column.  Total header width 304 px < 320.
 * Heartbeat / Wi-Fi / BLE keep >= 6 px net separation and share a unified
 * small visual weight (heartbeat 10, icons 12). */
#define HEADER_X 8
#define HEADER_Y 6
#define HEADER_W 304
#define HEADER_H 36
#define HEADER_TITLE_X 12
#define HEADER_TITLE_W 86
#define HEADER_BAR_X 102
#define HEADER_BAR_Y 12
#define HEADER_BAR_W 84
#define HEADER_BAR_H 8
#define HEADER_PCT_X 190
#define HEADER_PCT_W 26
#define HEADER_HB_X 220
#define HEADER_HB_Y 11
#define HEADER_HB_W 10
#define HEADER_HB_H 10
#define HEADER_WIFI_X 236
#define HEADER_BLE_X 254
#define HEADER_ICON_Y 12
#define HEADER_ICON_SIZE 12

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
    /* Per-page unified header widgets. */
    lv_obj_t *hdr_bar[DISPLAY_PAGE_COUNT];
    lv_obj_t *hdr_pct[DISPLAY_PAGE_COUNT];
    lv_obj_t *hdr_wifi_dot[DISPLAY_PAGE_COUNT];
    lv_obj_t *hdr_ble_dot[DISPLAY_PAGE_COUNT];
    lv_obj_t *heartbeat[DISPLAY_PAGE_COUNT];
    /* Page 1: overview. */
    lv_obj_t *ov_task_title;
    lv_obj_t *ov_task_remaining;
    lv_obj_t *ov_task_now;
    lv_obj_t *ov_task_next;
    lv_obj_t *ov_flow_nodes[6];
    lv_obj_t *ov_m1;
    lv_obj_t *ov_m2;
    lv_obj_t *ov_m3;
    lv_obj_t *ov_b1;
    lv_obj_t *ov_b2;
    lv_obj_t *ov_b3;
    lv_obj_t *ov_fault_title;
    lv_obj_t *ov_fault_code;
    lv_obj_t *ov_fault_detail;
    /* Page 2: telemetry. */
    lv_obj_t *tl_water;
    lv_obj_t *tl_water_meta;
    lv_obj_t *tl_quality;
    lv_obj_t *tl_quality_raw;
    lv_obj_t *tl_env;
    lv_obj_t *tl_env_meta;
    lv_obj_t *tl_motion;
    lv_obj_t *tl_motion_meta;
    lv_obj_t *tl_input_leds[6];
    lv_obj_t *tl_output_leds[6];
    lv_obj_t *tl_sys_mem;
    lv_obj_t *tl_sys_paj;
    lv_obj_t *tl_sys_mcp;
    lv_obj_t *tl_sys_fault;
    /* Page 3: voice. */
    lv_obj_t *vo_state_dot;
    lv_obj_t *vo_state;
    lv_obj_t *vo_backend;
    lv_obj_t *vo_mic_bars[5];
    lv_obj_t *vo_cloud;
    lv_obj_t *vo_transcript;
    lv_obj_t *vo_reply;
    uint32_t rendered_revision;
    uint8_t heartbeat_phase;
    /* Header icon state per page: [page][0]=Wi-Fi [1]=BLE.  Storing the link
     * state (not just a color) lets the draw callbacks switch between outline,
     * filled and fault shapes. */
    uint8_t wifi_icon_state[DISPLAY_PAGE_COUNT];
    uint8_t ble_icon_state[DISPLAY_PAGE_COUNT];
} display_runtime_t;

static const char *TAG = "display_svc";
static StaticSemaphore_t s_model_lock_storage;
static SemaphoreHandle_t s_model_lock;
static display_model_t s_model;
static display_runtime_t s_rt;
static _Atomic int s_init_state;
static _Atomic int s_requested_page;
static _Atomic int s_current_page;
static _Atomic uint32_t s_refresh_ticks;
static _Atomic uint32_t s_refresh_lock_misses;

/* Four outline phases inside a fixed box; ~250 ms per frame. */
static const uint8_t s_heartbeat_border_width[] = { 1U, 3U, 2U, 3U };
static const lv_coord_t s_heartbeat_radius[] = { 2, 6, 7, 4 };
static const lv_opa_t s_heartbeat_fill_opa[] = {
    LV_OPA_10, LV_OPA_50, LV_OPA_20, LV_OPA_60
};

static void heartbeat_draw_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_DRAW_MAIN) return;
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(event);
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(event);
    if (!draw_ctx || !obj) return;
    uint8_t count = (uint8_t)(sizeof(s_heartbeat_border_width) /
                              sizeof(s_heartbeat_border_width[0]));
    uint8_t phase = (uint8_t)(s_rt.heartbeat_phase % count);
    lv_draw_rect_dsc_t draw_dsc;
    lv_draw_rect_dsc_init(&draw_dsc);
    draw_dsc.bg_color = lv_color_hex(CLR_GREEN);
    draw_dsc.bg_opa = s_heartbeat_fill_opa[phase];
    draw_dsc.border_color = lv_color_hex(CLR_GREEN);
    draw_dsc.border_opa = LV_OPA_COVER;
    draw_dsc.border_width = s_heartbeat_border_width[phase];
    draw_dsc.radius = s_heartbeat_radius[phase];
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    lv_draw_rect(draw_ctx, &draw_dsc, &coords);
}

static bool model_lock(void)
{
    return s_model_lock &&
           xSemaphoreTake(s_model_lock,
                          pdMS_TO_TICKS(MODEL_LOCK_TIMEOUT_MS)) == pdTRUE;
}

static bool model_try_lock(void)
{
    return s_model_lock && xSemaphoreTake(s_model_lock, 0) == pdTRUE;
}

static uint32_t next_revision(uint32_t revision)
{
    revision++;
    return revision == 0U ? 1U : revision;
}

static bool telemetry_equal_for_ui(const display_telemetry_t *a,
                                   const display_telemetry_t *b)
{
    display_telemetry_t lhs = *a;
    display_telemetry_t rhs = *b;
    lhs.elapsed_ms = (lhs.elapsed_ms / 1000U) * 1000U;
    rhs.elapsed_ms = (rhs.elapsed_ms / 1000U) * 1000U;
    lhs.remaining_ms = (lhs.remaining_ms / 1000U) * 1000U;
    rhs.remaining_ms = (rhs.remaining_ms / 1000U) * 1000U;
    lhs.internal_free_bytes = (lhs.internal_free_bytes / 1024U) * 1024U;
    rhs.internal_free_bytes = (rhs.internal_free_bytes / 1024U) * 1024U;
    return memcmp(&lhs, &rhs, sizeof(lhs)) == 0;
}

static void copy_text(char *dst, size_t capacity, const char *src)
{
    if (capacity == 0U) return;
    if (!src) src = "";
    size_t length = strnlen(src, capacity - 1U);
    memcpy(dst, src, length);
    dst[length] = '\0';
}

/* Glyph whitelist for the telemetry font.  A missing glyph would render as a
 * tofu box, so dynamic text is sanitized before it reaches LVGL. */
static bool glyph_supported(uint32_t codepoint)
{
    lv_font_glyph_dsc_t dsc;
    return lv_font_get_glyph_dsc(&telemetry_font, &dsc, (uint32_t)codepoint,
                                 0);
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
    /* 普通卡片：暗蓝 1px。顶栏单独覆盖为青色 2px。 */
    lv_obj_set_style_border_color(card, lv_color_hex(CLR_CARD_BORDER), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 5, 0);
    lv_obj_set_style_pad_all(card, 2, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static lv_obj_t *dot_create(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                            lv_coord_t size, uint32_t color)
{
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_set_pos(dot, x, y);
    lv_obj_set_size(dot, size, size);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return dot;
}

static void set_dot_color(lv_obj_t *dot, uint32_t color)
{
    if (dot) lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
}

/* ---- Header Wi-Fi / BLE icons (drawn with LVGL primitives, no glyphs/images) ---- */

static uint8_t icon_page_of(lv_obj_t *obj)
{
    uint8_t page = (uint8_t)(uintptr_t)lv_obj_get_user_data(obj);
    return page < DISPLAY_PAGE_COUNT ? page : DISPLAY_PAGE_OVERVIEW;
}

/* Wi-Fi: a base dot + three concentric arcs opening upward.  The shape encodes
 * link state: OFF/UNKNOWN = outline only, CONNECTING = outline + a marker that
 * sweeps the arc on the heartbeat frame, GOT_IP = filled solid, FAILED =
 * outline + an X.  Never just a color change. */
static void wifi_fail_x_draw(lv_draw_ctx_t *ctx, const lv_area_t *coords,
                             lv_color_t color)
{
    lv_draw_line_dsc_t ln;
    lv_draw_line_dsc_init(&ln);
    ln.color = color;
    ln.width = 2;
    ln.opa = LV_OPA_COVER;
    lv_point_t a = { coords->x1 + 1, coords->y1 + 1 };
    lv_point_t b = { coords->x2 - 1, coords->y2 - 1 };
    lv_point_t c = { coords->x1 + 1, coords->y2 - 1 };
    lv_point_t d = { coords->x2 - 1, coords->y1 + 1 };
    lv_draw_line(ctx, &ln, &a, &b);
    lv_draw_line(ctx, &ln, &c, &d);
}

/* Marker positions for the CONNECTING sweep, one per heartbeat frame (angle
 * 225..315 deg, radius 6).  Integer offsets from the icon center. */
static const int8_t s_wifi_sweep_dx[4] = { -4, -2, 2, 4 };
static const int8_t s_wifi_sweep_dy[4] = { -4, -6, -6, -4 };

static void wifi_icon_draw_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_DRAW_MAIN) return;
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(event);
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(event);
    if (!ctx || !obj) return;
    uint8_t page = icon_page_of(obj);
    uint8_t state = s_rt.wifi_icon_state[page];
    uint8_t style = display_wifi_icon_style(state);
    lv_color_t color = lv_color_hex(display_wifi_color(state));
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    lv_coord_t cx = coords.x1 + (lv_coord_t)(lv_obj_get_width(obj) / 2);
    lv_coord_t cy = coords.y2 - 1;
    bool filled = style == DISPLAY_ICON_STYLE_ON;

    /* Base dot: filled when connected, outline ring otherwise. */
    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.radius = LV_RADIUS_CIRCLE;
    lv_area_t dot_area = { cx - 2, cy - 3, cx + 2, cy - 1 };
    if (filled) {
        dot.bg_color = color;
        dot.bg_opa = LV_OPA_COVER;
    } else {
        dot.bg_opa = LV_OPA_TRANSP;
        dot.border_color = color;
        dot.border_opa = LV_OPA_COVER;
        dot.border_width = 1;
    }
    lv_draw_rect(ctx, &dot, &dot_area);

    /* Concentric arcs: solid when filled, thin outline otherwise. */
    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.color = color;
    arc.width = filled ? 2 : 1;
    arc.opa = LV_OPA_COVER;
    arc.rounded = 1;
    lv_point_t center = { cx, cy };
    for (uint8_t i = 0; i < 3; ++i) {
        lv_draw_arc(ctx, &arc, &center, (lv_coord_t)(3 + (int)i * 3), 225, 315);
    }

    if (style == DISPLAY_ICON_STYLE_ADV) {
        /* Moving marker riding the outer arc, stepped by the refresh tick. */
        uint8_t phase = (uint8_t)(s_rt.heartbeat_phase % 4);
        lv_draw_rect_dsc_t m;
        lv_draw_rect_dsc_init(&m);
        m.bg_color = color;
        m.bg_opa = LV_OPA_COVER;
        m.radius = LV_RADIUS_CIRCLE;
        lv_area_t ma = { cx + s_wifi_sweep_dx[phase] - 1,
                         cy + s_wifi_sweep_dy[phase] - 1,
                         cx + s_wifi_sweep_dx[phase] + 1,
                         cy + s_wifi_sweep_dy[phase] + 1 };
        lv_draw_rect(ctx, &m, &ma);
    } else if (style == DISPLAY_ICON_STYLE_FAIL) {
        wifi_fail_x_draw(ctx, &coords, color);
    }
}

/* BLE: a stem + two wings (bow-tie) resembling the Bluetooth glyph.  Shape
 * encodes link state: STOPPED/UNKNOWN = outline, ADVERTISING = outline + a
 * pulsing tip marker, CONNECTED = filled solid, VOICE_SUSPENDED = stem filled
 * + wings outline (temporarily paused), RECOVER_FAILED = outline + X. */
static void ble_outline_triangle(lv_draw_ctx_t *ctx, lv_color_t color,
                                 const lv_point_t *pts)
{
    lv_draw_line_dsc_t ln;
    lv_draw_line_dsc_init(&ln);
    ln.color = color;
    ln.width = 1;
    ln.opa = LV_OPA_COVER;
    for (int i = 0; i < 3; ++i) {
        lv_point_t a = pts[i];
        lv_point_t b = pts[(i + 1) % 3];
        lv_draw_line(ctx, &ln, &a, &b);
    }
}

static void ble_icon_draw_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_DRAW_MAIN) return;
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(event);
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(event);
    if (!ctx || !obj) return;
    uint8_t page = icon_page_of(obj);
    uint8_t state = s_rt.ble_icon_state[page];
    uint8_t style = display_ble_icon_style(state);
    lv_color_t color = lv_color_hex(display_ble_color(state));
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    lv_coord_t cx = coords.x1 + (lv_coord_t)(lv_obj_get_width(obj) / 2);
    lv_coord_t cy = coords.y1 + (lv_coord_t)(lv_obj_get_height(obj) / 2);
    lv_coord_t h2 = (lv_coord_t)(lv_obj_get_width(obj) / 2);

    bool stem_filled = style == DISPLAY_ICON_STYLE_ON ||
                       style == DISPLAY_ICON_STYLE_SUSPEND;
    bool wings_filled = style == DISPLAY_ICON_STYLE_ON;

    if (stem_filled) {
        lv_draw_rect_dsc_t stem;
        lv_draw_rect_dsc_init(&stem);
        stem.bg_color = color;
        stem.bg_opa = LV_OPA_COVER;
        lv_area_t stem_area = { cx - 1, cy - h2, cx + 1, cy + h2 };
        lv_draw_rect(ctx, &stem, &stem_area);
    } else {
        lv_draw_line_dsc_t stem;
        lv_draw_line_dsc_init(&stem);
        stem.color = color;
        stem.width = 1;
        stem.opa = LV_OPA_COVER;
        lv_point_t a = { cx, cy - h2 };
        lv_point_t b = { cx, cy + h2 };
        lv_draw_line(ctx, &stem, &a, &b);
    }

    lv_point_t top_wing[3] = { { cx, cy - h2 }, { cx + h2, cy }, { cx, cy } };
    lv_point_t bot_wing[3] = { { cx, cy + h2 }, { cx - h2, cy }, { cx, cy } };
    if (wings_filled) {
        lv_draw_rect_dsc_t wing;
        lv_draw_rect_dsc_init(&wing);
        wing.bg_color = color;
        wing.bg_opa = LV_OPA_COVER;
        lv_draw_triangle(ctx, &wing, top_wing);
        lv_draw_triangle(ctx, &wing, bot_wing);
    } else {
        ble_outline_triangle(ctx, color, top_wing);
        ble_outline_triangle(ctx, color, bot_wing);
    }

    if (style == DISPLAY_ICON_STYLE_ADV) {
        /* Pulsing tip marker on even heartbeat frames (broadcasting, not
         * yet connected). */
        if ((s_rt.heartbeat_phase & 1U) == 0U) {
            lv_draw_rect_dsc_t m;
            lv_draw_rect_dsc_init(&m);
            m.bg_color = color;
            m.bg_opa = LV_OPA_COVER;
            m.radius = LV_RADIUS_CIRCLE;
            lv_area_t ma = { cx - 1, cy - h2, cx + 1, cy - h2 + 2 };
            lv_draw_rect(ctx, &m, &ma);
        }
    } else if (style == DISPLAY_ICON_STYLE_FAIL) {
        wifi_fail_x_draw(ctx, &coords, color);
    }
}

static lv_obj_t *header_icon_create(lv_obj_t *parent, lv_coord_t x,
                                    lv_coord_t y, uint8_t page_index,
                                    bool is_ble)
{
    lv_obj_t *icon = lv_obj_create(parent);
    lv_obj_set_size(icon, HEADER_ICON_SIZE, HEADER_ICON_SIZE);
    lv_obj_set_style_bg_opa(icon, LV_OPA_0, 0);
    lv_obj_set_style_border_width(icon, 0, 0);
    lv_obj_set_style_pad_all(icon, 0, 0);
    lv_obj_set_pos(icon, x, y);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(icon, (void *)(uintptr_t)page_index);
    lv_obj_add_event_cb(icon,
                        is_ble ? ble_icon_draw_event_cb : wifi_icon_draw_event_cb,
                        LV_EVENT_DRAW_MAIN, NULL);
    return icon;
}

/* ---- 形状+颜色双编码指示灯（ON/OFF/UNKNOWN/FAULT） ----
 * user_data tag: bits0-3=index; bit6=INPUTS 数组; bit7=图例（固定状态）。 */
static uint8_t s_output_led_state[6];
static uint8_t s_input_led_state[6];
static const uint8_t s_legend_led_state[4] = { 2U, 1U, 0U, 3U };  /* ON OFF ? ERR */

static void output_led_draw_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_DRAW_MAIN) return;
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(event);
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(event);
    if (!ctx || !obj) return;
    uint8_t tag = (uint8_t)(uintptr_t)lv_obj_get_user_data(obj);
    uint8_t idx = (uint8_t)(tag & 0x0fU);
    uint8_t st;
    if (tag & 0x80U) {
        st = idx < 4U ? s_legend_led_state[idx] : 0U;
    } else if (tag & 0x40U) {
        st = idx < 6U ? s_input_led_state[idx] : 0U;
    } else {
        st = idx < 6U ? s_output_led_state[idx] : 0U;
    }
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);

    if (st == 2U) {
        /* ON: 绿色实心圆 */
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = lv_color_hex(CLR_GREEN);
        d.bg_opa = LV_OPA_COVER;
        d.radius = LV_RADIUS_CIRCLE;
        lv_draw_rect(ctx, &d, &coords);
    } else if (st == 1U) {
        /* OFF: 灰色空心圆 */
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = lv_color_hex(CLR_GLASS_BG);
        d.bg_opa = LV_OPA_COVER;
        d.radius = LV_RADIUS_CIRCLE;
        lv_draw_rect(ctx, &d, &coords);
        lv_draw_rect_dsc_t b;
        lv_draw_rect_dsc_init(&b);
        b.bg_opa = LV_OPA_TRANSP;
        b.border_color = lv_color_hex(CLR_DIM);
        b.border_opa = LV_OPA_COVER;
        b.border_width = 2;
        b.radius = LV_RADIUS_CIRCLE;
        lv_draw_rect(ctx, &b, &coords);
    } else if (st == 3U) {
        /* FAULT: 红色叉 */
        lv_draw_line_dsc_t ln;
        lv_draw_line_dsc_init(&ln);
        ln.color = lv_color_hex(CLR_RED);
        ln.width = 2;
        ln.opa = LV_OPA_COVER;
        lv_point_t a = { coords.x1, coords.y1 };
        lv_point_t b = { coords.x2, coords.y2 };
        lv_point_t c = { coords.x1, coords.y2 };
        lv_point_t d = { coords.x2, coords.y1 };
        lv_draw_line(ctx, &ln, &a, &b);
        lv_draw_line(ctx, &ln, &c, &d);
    } else if (st == 4U) {
        /* UNSUPPORTED: 灰色 "--"（两短横）。未支持/未初始化，绝不伪装 UNKNOWN。 */
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_opa = LV_OPA_TRANSP;
        d.border_color = lv_color_hex(CLR_GLASS_BORDER);
        d.border_opa = LV_OPA_COVER;
        d.border_width = 1;
        lv_draw_rect(ctx, &d, &coords);
        lv_draw_line_dsc_t ln;
        lv_draw_line_dsc_init(&ln);
        ln.color = lv_color_hex(CLR_DIM);
        ln.width = 1;
        ln.opa = LV_OPA_COVER;
        lv_coord_t mid = (lv_coord_t)((coords.y1 + coords.y2) / 2);
        lv_point_t h1a = { coords.x1 + 1, mid - 1 };
        lv_point_t h1b = { coords.x2 - 1, mid - 1 };
        lv_point_t h2a = { coords.x1 + 1, mid + 1 };
        lv_point_t h2b = { coords.x2 - 1, mid + 1 };
        lv_draw_line(ctx, &ln, &h1a, &h1b);
        lv_draw_line(ctx, &ln, &h2a, &h2b);
    } else if (st == 5U) {
        /* COOLDOWN: 蓝色实心圆（冷却锁定/倒计时）。 */
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = lv_color_hex(CLR_BLUE);
        d.bg_opa = LV_OPA_COVER;
        d.radius = LV_RADIUS_CIRCLE;
        lv_draw_rect(ctx, &d, &coords);
        lv_draw_rect_dsc_t b;
        lv_draw_rect_dsc_init(&b);
        b.bg_opa = LV_OPA_TRANSP;
        b.border_color = lv_color_hex(CLR_WHITE);
        b.border_opa = LV_OPA_50;
        b.border_width = 1;
        b.radius = LV_RADIUS_CIRCLE;
        lv_draw_rect(ctx, &b, &coords);
    } else {
        /* UNKNOWN: 黄色方框 */
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_opa = LV_OPA_TRANSP;
        d.border_color = lv_color_hex(CLR_ORANGE);
        d.border_opa = LV_OPA_COVER;
        d.border_width = 2;
        lv_draw_rect(ctx, &d, &coords);
    }
}

static lv_obj_t *output_led_create(lv_obj_t *parent, lv_coord_t x,
                                   lv_coord_t y, uint8_t tag)
{
    lv_obj_t *led = lv_obj_create(parent);
    lv_obj_set_size(led, 12, 12);
    lv_obj_set_pos(led, x, y);
    lv_obj_set_style_bg_opa(led, LV_OPA_0, 0);
    lv_obj_set_style_border_width(led, 0, 0);
    lv_obj_set_style_pad_all(led, 0, 0);
    lv_obj_clear_flag(led, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(led, (void *)(uintptr_t)tag);
    lv_obj_add_event_cb(led, output_led_draw_event_cb, LV_EVENT_DRAW_MAIN,
                        NULL);
    return led;
}

static void add_page_indicator(lv_obj_t *page, uint8_t active)
{
    static const lv_coord_t x_pos[DISPLAY_PAGE_COUNT] = { 148, 160, 172 };
    for (uint8_t i = 0; i < DISPLAY_PAGE_COUNT; ++i) {
        lv_obj_t *dot = lv_obj_create(page);
        lv_obj_set_pos(dot, x_pos[i] - (i == active ? 3 : 0), 222);
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

/* Unified three-page header: fixed title + thin progress bar + heartbeat +
 * Wi-Fi/BLE dots.  Same geometry on every page. */
static void brand_header_create(lv_obj_t *page, const char *title,
                                uint8_t page_index)
{
    lv_obj_t *header = card_create(page, HEADER_X, HEADER_Y, HEADER_W,
                                   HEADER_H);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x0c2639), 0);
    lv_obj_set_style_border_color(header, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_border_width(header, 2, 0);
    lv_obj_set_style_pad_all(header, 0, 0);

    lv_obj_t *heading = lv_label_create(header);
    lv_label_set_text(heading, title);
    lv_obj_set_style_text_font(heading, &telemetry_font, 0);
    lv_obj_set_style_text_color(heading, lv_color_hex(CLR_WHITE), 0);
    lv_obj_set_pos(heading, HEADER_TITLE_X, 9);
    lv_obj_set_width(heading, HEADER_TITLE_W);
    lv_label_set_long_mode(heading, LV_LABEL_LONG_CLIP);

    lv_obj_t *bar = lv_bar_create(header);
    lv_obj_set_pos(bar, HEADER_BAR_X, HEADER_BAR_Y);
    lv_obj_set_size(bar, HEADER_BAR_W, HEADER_BAR_H);
    lv_bar_set_range(bar, 0, 100);
    lv_obj_set_style_bg_color(bar, lv_color_hex(CLR_GLASS_BORDER),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(CLR_CYAN),
                              LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);

    lv_obj_t *pct = lv_label_create(header);
    lv_label_set_text(pct, "");
    lv_obj_set_style_text_font(pct, &telemetry_font, 0);
    lv_obj_set_style_text_color(pct, lv_color_hex(CLR_DIM), 0);
    lv_obj_set_pos(pct, HEADER_PCT_X, 9);
    lv_obj_set_width(pct, HEADER_PCT_W);
    lv_obj_set_style_text_align(pct, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(pct, LV_LABEL_LONG_CLIP);

    lv_obj_t *heartbeat = lv_obj_create(header);
    lv_obj_set_size(heartbeat, HEADER_HB_W, HEADER_HB_H);
    lv_obj_set_style_bg_opa(heartbeat, LV_OPA_0, 0);
    lv_obj_set_style_border_width(heartbeat, 0, 0);
    lv_obj_set_style_border_opa(heartbeat, LV_OPA_0, 0);
    lv_obj_set_style_pad_all(heartbeat, 0, 0);
    lv_obj_set_pos(heartbeat, HEADER_HB_X, HEADER_HB_Y);
    lv_obj_clear_flag(heartbeat, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(heartbeat, heartbeat_draw_event_cb,
                        LV_EVENT_DRAW_MAIN, NULL);
    if (page_index < DISPLAY_PAGE_COUNT)
        s_rt.heartbeat[page_index] = heartbeat;

    /* Wi-Fi / BLE icons (LVGL primitives; >= 5-6 px apart). */
    lv_obj_t *wifi_icon = header_icon_create(header, HEADER_WIFI_X,
                                             HEADER_ICON_Y, page_index, false);
    lv_obj_t *ble_icon = header_icon_create(header, HEADER_BLE_X,
                                            HEADER_ICON_Y, page_index, true);
    if (page_index < DISPLAY_PAGE_COUNT) {
        s_rt.hdr_wifi_dot[page_index] = wifi_icon;
        s_rt.hdr_ble_dot[page_index] = ble_icon;
        s_rt.wifi_icon_state[page_index] = DISPLAY_WIFI_OFF;
        s_rt.ble_icon_state[page_index] = DISPLAY_BLE_OFF;
    }
}

/* Page 1: run overview.  Task card uses a compact two-column layout:
 * (当前阶段, 剩余时间) / (下一步, 姿态).  Pose is merged into the task card
 * so the flow row can run full width and no card is squashed. */
static void build_overview_page(void)
{
    lv_obj_t *page = s_rt.pages[DISPLAY_PAGE_OVERVIEW] = page_create();
    brand_header_create(page, "运行总览", DISPLAY_PAGE_OVERVIEW);

    lv_obj_t *task = card_create(page, 8, 48, 304, 50);
    lv_obj_set_style_pad_all(task, 2, 0);
    s_rt.ov_task_title = telemetry_label_create(task, "空闲", CLR_WHITE);
    lv_obj_set_pos(s_rt.ov_task_title, 4, 3);
    lv_obj_set_size(s_rt.ov_task_title, 150, 18);
    lv_obj_set_style_text_color(s_rt.ov_task_title, lv_color_hex(CLR_DIM), 0);
    /* 百分比只在顶栏显示；任务卡不重复。 */
    s_rt.ov_task_remaining = telemetry_label_create(task, "剩余 --", CLR_DIM);
    lv_obj_set_pos(s_rt.ov_task_remaining, 154, 3);
    lv_obj_set_size(s_rt.ov_task_remaining, 146, 18);
    lv_obj_set_style_text_align(s_rt.ov_task_remaining, LV_TEXT_ALIGN_RIGHT, 0);
    s_rt.ov_task_now = telemetry_label_create(task, "下一步 --", CLR_WHITE);
    lv_obj_set_pos(s_rt.ov_task_now, 4, 28);
    lv_obj_set_size(s_rt.ov_task_now, 150, 18);
    s_rt.ov_task_next = telemetry_label_create(task, "姿态 --", CLR_DIM);
    lv_obj_set_pos(s_rt.ov_task_next, 154, 28);
    lv_obj_set_size(s_rt.ov_task_next, 146, 18);
    lv_obj_set_style_text_align(s_rt.ov_task_next, LV_TEXT_ALIGN_RIGHT, 0);

    /* Fault card overlays the task card area when a fault is active. */
    s_rt.ov_fault_title = telemetry_label_create(task, "设备故障", CLR_RED);
    lv_obj_set_pos(s_rt.ov_fault_title, 4, 3);
    lv_obj_set_size(s_rt.ov_fault_title, 150, 18);
    s_rt.ov_fault_code = telemetry_label_create(task, "故障码 --", CLR_RED);
    lv_obj_set_pos(s_rt.ov_fault_code, 160, 3);
    lv_obj_set_size(s_rt.ov_fault_code, 140, 18);
    lv_obj_set_style_text_align(s_rt.ov_fault_code, LV_TEXT_ALIGN_RIGHT, 0);
    s_rt.ov_fault_detail = telemetry_label_create(task, "说明 --", CLR_ORANGE);
    lv_obj_set_pos(s_rt.ov_fault_detail, 4, 28);
    lv_obj_set_size(s_rt.ov_fault_detail, 296, 18);
    lv_obj_set_style_bg_color(s_rt.ov_fault_title, lv_color_hex(0x3a1016), 0);
    lv_obj_set_style_bg_opa(s_rt.ov_fault_title, LV_OPA_0, 0);

    lv_obj_t *flow = card_create(page, 8, 102, 304, 50);
    lv_obj_set_style_pad_all(flow, 0, 0);
    lv_obj_t *flow_title = telemetry_label_create(flow, "流程", CLR_CYAN);
    lv_obj_set_pos(flow_title, 3, 2);
    lv_obj_set_size(flow_title, 44, 14);
    static const char *const flow_names[6] = {
        "进", "洗", "排", "脱", "烘", "UV"
    };
    /* 六节点一行居中：6×32=192 宽，居中于 304 → 起点 56。 */
    for (uint8_t i = 0; i < 6; ++i) {
        lv_obj_t *n = telemetry_label_create(flow, flow_names[i], CLR_DIM);
        lv_obj_set_pos(n, (lv_coord_t)(56 + (int)i * 32), 24);
        lv_obj_set_size(n, 30, 18);
        lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
        s_rt.ov_flow_nodes[i] = n;
    }

    lv_obj_t *core = card_create(page, 8, 156, 304, 62);
    lv_obj_set_style_pad_all(core, 0, 0);
    lv_obj_t *core_title = telemetry_label_create(core, "核心指标", CLR_CYAN);
    lv_obj_set_pos(core_title, 4, 2);
    lv_obj_set_size(core_title, 80, 14);
    s_rt.ov_m1 = telemetry_label_create(core, "水量 --", CLR_WHITE);
    lv_obj_set_pos(s_rt.ov_m1, 4, 20);
    lv_obj_set_size(s_rt.ov_m1, 96, 18);
    s_rt.ov_m2 = telemetry_label_create(core, "流量 --", CLR_WHITE);
    lv_obj_set_pos(s_rt.ov_m2, 104, 20);
    lv_obj_set_size(s_rt.ov_m2, 96, 18);
    s_rt.ov_m3 = telemetry_label_create(core, "温度 --", CLR_WHITE);
    lv_obj_set_pos(s_rt.ov_m3, 204, 20);
    lv_obj_set_size(s_rt.ov_m3, 96, 18);
    s_rt.ov_b1 = telemetry_label_create(core, "湿度 --", CLR_DIM);
    lv_obj_set_pos(s_rt.ov_b1, 4, 42);
    lv_obj_set_size(s_rt.ov_b1, 96, 18);
    s_rt.ov_b2 = telemetry_label_create(core, "水位 --", CLR_DIM);
    lv_obj_set_pos(s_rt.ov_b2, 104, 42);
    lv_obj_set_size(s_rt.ov_b2, 96, 18);
    s_rt.ov_b3 = telemetry_label_create(core, "UV --", CLR_DIM);
    lv_obj_set_pos(s_rt.ov_b3, 204, 42);
    lv_obj_set_size(s_rt.ov_b3, 96, 18);

    add_page_indicator(page, DISPLAY_PAGE_OVERVIEW);
}

/* Page 2: device telemetry dashboard. */
static void build_telemetry_page(void)
{
    lv_obj_t *page = s_rt.pages[DISPLAY_PAGE_TELEMETRY] = page_create();
    brand_header_create(page, "设备遥测", DISPLAY_PAGE_TELEMETRY);

    lv_obj_t *water = card_create(page, 8, 48, 98, 48);
    lv_obj_set_style_pad_all(water, 0, 0);
    lv_obj_t *water_title = telemetry_label_create(water, "WATER", CLR_CYAN);
    lv_obj_set_pos(water_title, 3, 2);
    lv_obj_set_size(water_title, 88, 14);
    s_rt.tl_water = telemetry_label_create(water, "N/A", CLR_WHITE);
    lv_obj_set_pos(s_rt.tl_water, 3, 17);
    lv_obj_set_size(s_rt.tl_water, 92, 15);
    lv_obj_set_style_text_color(s_rt.tl_water, lv_color_hex(CLR_DIM), 0);
    s_rt.tl_water_meta = telemetry_label_create(water, "FLOW --", CLR_DIM);
    lv_obj_set_pos(s_rt.tl_water_meta, 3, 30);
    lv_obj_set_size(s_rt.tl_water_meta, 92, 13);

    lv_obj_t *quality = card_create(page, 111, 48, 98, 48);
    lv_obj_set_style_pad_all(quality, 0, 0);
    lv_obj_t *quality_title = telemetry_label_create(quality, "QUALITY", CLR_CYAN);
    lv_obj_set_pos(quality_title, 3, 2);
    lv_obj_set_size(quality_title, 88, 14);
    s_rt.tl_quality = telemetry_label_create(quality, "N/A", CLR_WHITE);
    lv_obj_set_pos(s_rt.tl_quality, 3, 17);
    lv_obj_set_size(s_rt.tl_quality, 92, 15);
    lv_obj_set_style_text_color(s_rt.tl_quality, lv_color_hex(CLR_DIM), 0);
    s_rt.tl_quality_raw = telemetry_label_create(quality, "RAW --", CLR_DIM);
    lv_obj_set_pos(s_rt.tl_quality_raw, 3, 30);
    lv_obj_set_size(s_rt.tl_quality_raw, 92, 13);

    lv_obj_t *env = card_create(page, 214, 48, 98, 48);
    lv_obj_set_style_pad_all(env, 0, 0);
    lv_obj_t *env_title = telemetry_label_create(env, "ENV", CLR_CYAN);
    lv_obj_set_pos(env_title, 3, 2);
    lv_obj_set_size(env_title, 88, 14);
    s_rt.tl_env = telemetry_label_create(env, "N/A", CLR_WHITE);
    lv_obj_set_pos(s_rt.tl_env, 3, 17);
    lv_obj_set_size(s_rt.tl_env, 92, 15);
    lv_obj_set_style_text_color(s_rt.tl_env, lv_color_hex(CLR_DIM), 0);
    s_rt.tl_env_meta = telemetry_label_create(env, "SHT --", CLR_DIM);
    lv_obj_set_pos(s_rt.tl_env_meta, 3, 30);
    lv_obj_set_size(s_rt.tl_env_meta, 92, 13);

    lv_obj_t *motion = card_create(page, 8, 100, 98, 48);
    lv_obj_set_style_pad_all(motion, 0, 0);
    lv_obj_t *motion_title = telemetry_label_create(motion, "MOTION", CLR_CYAN);
    lv_obj_set_pos(motion_title, 3, 2);
    lv_obj_set_size(motion_title, 90, 14);
    s_rt.tl_motion = telemetry_label_create(motion, "POS --", CLR_WHITE);
    lv_obj_set_pos(s_rt.tl_motion, 3, 18);
    lv_obj_set_size(s_rt.tl_motion, 92, 14);
    s_rt.tl_motion_meta = telemetry_label_create(motion, "PWM --", CLR_DIM);
    lv_obj_set_pos(s_rt.tl_motion_meta, 3, 31);
    lv_obj_set_size(s_rt.tl_motion_meta, 92, 13);

    lv_obj_t *inputs = card_create(page, 111, 100, 201, 48);
    lv_obj_set_style_pad_all(inputs, 0, 0);
    lv_obj_t *inputs_title = telemetry_label_create(inputs, "位置输入", CLR_CYAN);
    lv_obj_set_pos(inputs_title, 3, 2);
    lv_obj_set_size(inputs_title, 100, 14);
    static const char *const input_names[6] = {
        "0", "45", "90", "180", "270", "BL"
    };
    for (uint8_t i = 0; i < 6; ++i) {
        lv_obj_t *led = output_led_create(inputs,
                                          (lv_coord_t)(4 + (int)i * 32), 18,
                                          (uint8_t)(0x40U | i));
        lv_obj_t *lbl = telemetry_label_create(inputs, input_names[i], CLR_DIM);
        lv_obj_set_pos(lbl, (lv_coord_t)(2 + (int)i * 32), 31);
        lv_obj_set_size(lbl, 32, 12);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        s_rt.tl_input_leds[i] = led;
    }

    lv_obj_t *outputs = card_create(page, 8, 152, 198, 66);
    lv_obj_set_style_pad_all(outputs, 0, 0);
    lv_obj_t *out_title = telemetry_label_create(outputs, "OUTPUTS", CLR_CYAN);
    lv_obj_set_pos(out_title, 4, 2);
    lv_obj_set_size(out_title, 184, 14);
    static const char *const output_names[6] = {
        "IN", "TR", "DR", "DT", "UV", "热风"
    };
    static const lv_coord_t output_x[6] = { 6, 38, 70, 102, 134, 166 };
    for (uint8_t i = 0; i < 6; ++i) {
        lv_obj_t *led = output_led_create(outputs, output_x[i], 16, (uint8_t)i);
        lv_obj_t *lbl = telemetry_label_create(outputs, output_names[i],
                                               CLR_DIM);
        lv_obj_set_pos(lbl, output_x[i] - 4, 30);
        lv_obj_set_size(lbl, 36, 12);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        s_rt.tl_output_leds[i] = led;
    }
    /* 紧凑图例（一行小字，不占整行大字）：●ON ○OFF □? ×ERR。 */
    static const char *const legend_text[4] = { "ON", "OFF", "?", "ERR" };
    static const lv_coord_t legend_x[4] = { 6, 52, 100, 134 };
    static const uint32_t legend_color[4] = {
        CLR_GREEN, CLR_DIM, CLR_ORANGE, CLR_RED
    };
    for (uint8_t i = 0; i < 4; ++i) {
        (void)output_led_create(outputs, legend_x[i], 46,
                                (uint8_t)(0x80U | i));
        lv_obj_t *lbl = telemetry_label_create(outputs, legend_text[i],
                                               legend_color[i]);
        lv_obj_set_pos(lbl, legend_x[i] + 12, 46);
        lv_obj_set_size(lbl, 40, 12);
    }

    lv_obj_t *system = card_create(page, 214, 152, 98, 66);
    lv_obj_set_style_pad_all(system, 0, 0);
    lv_obj_t *sys_title = telemetry_label_create(system, "SYSTEM", CLR_CYAN);
    lv_obj_set_pos(sys_title, 4, 2);
    lv_obj_set_size(sys_title, 88, 14);
    /* 四行均不裁切：行距 13px，最后一行底缘 52+13=65 < 70。 */
    s_rt.tl_sys_mem = telemetry_label_create(system, "MEM --", CLR_WHITE);
    lv_obj_set_pos(s_rt.tl_sys_mem, 4, 13);
    lv_obj_set_size(s_rt.tl_sys_mem, 90, 12);
    s_rt.tl_sys_paj = telemetry_label_create(system, "PAJ --", CLR_DIM);
    lv_obj_set_pos(s_rt.tl_sys_paj, 4, 26);
    lv_obj_set_size(s_rt.tl_sys_paj, 90, 12);
    s_rt.tl_sys_mcp = telemetry_label_create(system, "MCP --", CLR_DIM);
    lv_obj_set_pos(s_rt.tl_sys_mcp, 4, 39);
    lv_obj_set_size(s_rt.tl_sys_mcp, 90, 12);
    s_rt.tl_sys_fault = telemetry_label_create(system, "FAULT --", CLR_DIM);
    lv_obj_set_pos(s_rt.tl_sys_fault, 4, 52);
    lv_obj_set_size(s_rt.tl_sys_fault, 90, 12);

    add_page_indicator(page, DISPLAY_PAGE_TELEMETRY);
}

/* Page 3: voice assistant.  Geometry: status 36px / user 44px (2 行) /
 * reply 80px (4 行); page indicator at y=222 stays clear of the cards. */
static void build_voice_page(void)
{
    lv_obj_t *page = s_rt.pages[DISPLAY_PAGE_VOICE] = page_create();
    brand_header_create(page, "语音助手", DISPLAY_PAGE_VOICE);

    lv_obj_t *state_card = card_create(page, 8, 48, 304, 36);
    lv_obj_set_style_pad_all(state_card, 2, 0);
    s_rt.vo_state_dot = dot_create(state_card, 4, 3, 12, CLR_DIM);
    s_rt.vo_state = telemetry_label_create(state_card, "待命", CLR_DIM);
    lv_obj_set_pos(s_rt.vo_state, 20, 1);
    lv_obj_set_size(s_rt.vo_state, 90, 18);
    s_rt.vo_backend = telemetry_label_create(state_card, "EDGE - 小净小净",
                                             CLR_CYAN);
    lv_obj_set_pos(s_rt.vo_backend, 110, 1);
    /* UI-P2-3：右缘留 ≥6px 安全边距（卡片右缘 312，标签右缘 294），
     * 右对齐文本不贴边；超出以 LONG_CLIP 确定性截断。 */
    lv_obj_set_size(s_rt.vo_backend, 184, 18);
    lv_obj_set_style_text_align(s_rt.vo_backend, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_rt.vo_backend, LV_LABEL_LONG_CLIP);

    /* 第二行：麦克风活动（采集动画，非伪装音量表）+ 云端状态。 */
    lv_obj_t *mic_label = telemetry_label_create(state_card, "麦克风 采集中", CLR_DIM);
    lv_obj_set_pos(mic_label, 4, 20);
    lv_obj_set_size(mic_label, 80, 14);
    for (uint8_t i = 0; i < 5; ++i) {
        lv_obj_t *bar = lv_obj_create(state_card);
        lv_obj_set_pos(bar, (lv_coord_t)(88 + (int)i * 9), 20);
        lv_obj_set_size(bar, 5, 5);
        lv_obj_set_style_bg_color(bar, lv_color_hex(CLR_GLASS_BORDER), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        s_rt.vo_mic_bars[i] = bar;
    }
    s_rt.vo_cloud = telemetry_label_create(state_card, "云端 --", CLR_DIM);
    lv_obj_set_pos(s_rt.vo_cloud, 210, 20);
    lv_obj_set_size(s_rt.vo_cloud, 90, 14);
    lv_obj_set_style_text_align(s_rt.vo_cloud, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *you_card = card_create(page, 8, 88, 304, 44);
    lv_obj_set_style_pad_all(you_card, 2, 0);
    s_rt.vo_transcript = telemetry_label_create(you_card, "你：--", CLR_DIM);
    lv_obj_set_width(s_rt.vo_transcript, 296);
    lv_obj_set_height(s_rt.vo_transcript, 38);
    lv_label_set_long_mode(s_rt.vo_transcript, LV_LABEL_LONG_WRAP);

    lv_obj_t *reply_card = card_create(page, 8, 136, 304, 80);
    lv_obj_set_style_pad_all(reply_card, 2, 0);
    s_rt.vo_reply = telemetry_label_create(reply_card, "小净：等待语音输入",
                                           CLR_WHITE);
    lv_obj_set_width(s_rt.vo_reply, 296);
    lv_obj_set_height(s_rt.vo_reply, 72);
    lv_label_set_long_mode(s_rt.vo_reply, LV_LABEL_LONG_WRAP);

    add_page_indicator(page, DISPLAY_PAGE_VOICE);
}

static const char *phase_name(uint8_t phase)
{
    static const char *const names[] = {
        "空闲", "定位", "归位", "进水", "转移", "投放",
        "波轮洗", "滚筒洗", "排水", "脱水", "烘干", "UV", "完成"
    };
    return phase < (sizeof(names) / sizeof(names[0])) ? names[phase] : "--";
}

/* Short readable fault description for the overview fault card.  Display-only;
 * the fault handling pipeline itself is untouched. */
static const char *fault_short_text(uint16_t code)
{
    switch (code) {
    case 1: return "霍尔冲突";
    case 2: return "定位超时";
    case 3: return "定位驱动";
    case 4: return "进水无流";
    case 5: return "进水低流";
    case 6: return "进水超时";
    case 7: return "液位无效";
    case 8: return "SHT 无效";
    case 11: return "MCP 输出";
    case 14: return "I2C 总线";
    case 15: return "BL50 驱动";
    case 16: return "BL50 归位 IO";
    case 17: return "BL50 归位超时";
    case 18: return "程序超时";
    case 19: return "内部故障";
    default: return "故障详情";
    }
}

static void format_remaining(char *buffer, size_t capacity, uint32_t ms)
{
    uint32_t seconds = ms / 1000U;
    uint32_t minutes = seconds / 60U;
    if (minutes >= 100U) minutes = 99U;
    snprintf(buffer, capacity, "剩余 %" PRIu32 "min", minutes);
}

static void update_heartbeat(void)
{
    static const uint8_t phase_count = (uint8_t)(
        sizeof(s_heartbeat_border_width) /
        sizeof(s_heartbeat_border_width[0]));
    s_rt.heartbeat_phase = (uint8_t)((s_rt.heartbeat_phase + 1U) % phase_count);
    int page = atomic_load_explicit(&s_current_page, memory_order_acquire);
    if (page < 0 || page >= DISPLAY_PAGE_COUNT) page = DISPLAY_PAGE_OVERVIEW;
    lv_obj_t *heartbeat = s_rt.heartbeat[page];
    if (heartbeat) lv_obj_invalidate(heartbeat);
    /* Keep the CONNECTING / ADVERTISING marker animated: the draw callbacks
     * step on heartbeat_phase, so re-invalidate the icon on every frame. */
    if (s_rt.wifi_icon_state[page] == DISPLAY_WIFI_CONNECTING &&
        s_rt.hdr_wifi_dot[page])
        lv_obj_invalidate(s_rt.hdr_wifi_dot[page]);
    if (s_rt.ble_icon_state[page] == DISPLAY_BLE_ADVERTISING &&
        s_rt.hdr_ble_dot[page])
        lv_obj_invalidate(s_rt.hdr_ble_dot[page]);
}

/* Shared header: progress + percentage + Wi-Fi/BLE icons. */
static void refresh_header(const display_model_t *model, int page_index)
{
    if (page_index < 0 || page_index >= DISPLAY_PAGE_COUNT) return;
    lv_obj_t *bar = s_rt.hdr_bar[page_index];
    lv_obj_t *pct = s_rt.hdr_pct[page_index];
    bool washing = model->washing;
    bool fault = model->telemetry.safety_fault ||
                 model->telemetry.emergency_stop;
    bool paused = model->telemetry.machine_state == 6U;
    uint8_t progress = model->wash_progress;
    if (progress > 100U) progress = 100U;

    if (bar) {
        if (washing) {
            uint32_t color = fault ? CLR_RED
                           : paused ? CLR_ORANGE : CLR_CYAN;
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_color(bar, lv_color_hex(color),
                                      LV_PART_INDICATOR);
            lv_bar_set_value(bar, progress, LV_ANIM_OFF);
        } else {
            /* 空闲：隐藏轨道与填充（无亮条、无百分比），仅留暗色细轨。 */
            lv_obj_set_style_bg_opa(bar, LV_OPA_0, LV_PART_MAIN);
            lv_obj_set_style_bg_color(bar, lv_color_hex(CLR_GLASS_BORDER),
                                      LV_PART_INDICATOR);
            lv_bar_set_value(bar, 0, LV_ANIM_OFF);
        }
    }
    if (pct) {
        if (washing) {
            lv_label_set_text_fmt(pct, "%u%%", (unsigned)progress);
            lv_obj_set_style_text_color(pct,
                lv_color_hex(fault ? CLR_RED :
                             paused ? CLR_ORANGE : CLR_CYAN), 0);
        } else {
            /* 空闲：不显示百分比占位，进度条只保留细暗轨。 */
            lv_label_set_text(pct, "");
            lv_obj_set_style_text_color(pct, lv_color_hex(CLR_DIM), 0);
        }
    }
    if (page_index < DISPLAY_PAGE_COUNT) {
        lv_obj_t *wd = s_rt.hdr_wifi_dot[page_index];
        lv_obj_t *bd = s_rt.hdr_ble_dot[page_index];
        if (wd) {
            uint8_t ws = model->telemetry.wifi_state;
            if (s_rt.wifi_icon_state[page_index] != ws) {
                s_rt.wifi_icon_state[page_index] = ws;
                lv_obj_invalidate(wd);
            }
        }
        if (bd) {
            uint8_t bs = model->telemetry.ble_state;
            if (s_rt.ble_icon_state[page_index] != bs) {
                s_rt.ble_icon_state[page_index] = bs;
                lv_obj_invalidate(bd);
            }
        }
    }
}

static void refresh_overview(const display_model_t *model)
{
    bool fault = model->telemetry.safety_fault ||
                 model->telemetry.emergency_stop;
    bool washing = model->washing;

    lv_obj_add_flag(s_rt.ov_fault_title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_rt.ov_fault_code, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_rt.ov_fault_detail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_rt.ov_task_title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_rt.ov_task_remaining, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_rt.ov_task_now, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_rt.ov_task_next, LV_OBJ_FLAG_HIDDEN);

    if (fault) {
        lv_obj_add_flag(s_rt.ov_task_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_rt.ov_task_remaining, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_rt.ov_task_now, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_rt.ov_task_next, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_rt.ov_fault_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_rt.ov_fault_code, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_rt.ov_fault_detail, LV_OBJ_FLAG_HIDDEN);
        uint32_t code = model->telemetry.fault_code;
        lv_label_set_text(s_rt.ov_fault_title,
            model->telemetry.emergency_stop ? "紧急停止" : "设备故障");
        lv_label_set_text_fmt(s_rt.ov_fault_code, "故障 E%u",
                              (unsigned)code);
        lv_label_set_text(s_rt.ov_fault_detail,
                          fault_short_text((uint16_t)code));
        return;
    }

    if (washing) {
        const char *step = model->wash_step[0] ? model->wash_step : "运行中";
        lv_label_set_text(s_rt.ov_task_title, step);
        lv_obj_set_style_text_color(s_rt.ov_task_title,
                                    lv_color_hex(CLR_WHITE), 0);
        char remain[24];
        format_remaining(remain, sizeof(remain),
                         model->telemetry.remaining_ms);
        lv_label_set_text(s_rt.ov_task_remaining, remain);
        lv_obj_set_style_text_color(s_rt.ov_task_remaining,
                                    lv_color_hex(CLR_CYAN), 0);
        uint8_t next_phase = (uint8_t)(model->telemetry.wash_phase + 1U);
        lv_label_set_text_fmt(s_rt.ov_task_now, "下一步 %s",
                              phase_name(next_phase));
    } else {
        lv_label_set_text(s_rt.ov_task_title, "空闲");
        lv_obj_set_style_text_color(s_rt.ov_task_title,
                                    lv_color_hex(CLR_DIM), 0);
        lv_label_set_text(s_rt.ov_task_remaining, "剩余 --");
        lv_obj_set_style_text_color(s_rt.ov_task_remaining,
                                    lv_color_hex(CLR_DIM), 0);
        lv_label_set_text(s_rt.ov_task_now, "下一步 --");
    }

    /* Flow nodes: 已完成=暗绿, 当前=亮青填充, 未开始=灰, 故障=红。 */
    int node = -1;
    uint8_t phase = model->telemetry.wash_phase;
    if (phase >= 3U && phase <= 5U) node = 0;       /* 进水/转移/投放 */
    else if (phase == 6U || phase == 7U) node = 1;  /* 洗涤 */
    else if (phase == 8U) node = 2;                 /* 排水 */
    else if (phase == 9U) node = 3;                 /* 脱水 */
    else if (phase == 10U) node = 4;                /* 烘干 */
    else if (phase == 11U) node = 5;                /* UV */
    for (uint8_t i = 0; i < 6; ++i) {
        uint32_t c = CLR_DIM;
        if (fault) c = CLR_RED;
        else if (node >= 0 && (int)i < node) c = CLR_DARKGREEN; /* done */
        else if ((int)i == node) c = CLR_CYAN;                  /* current */
        lv_obj_set_style_text_color(s_rt.ov_flow_nodes[i],
                                    lv_color_hex(c), 0);
    }

    /* 姿态（并入任务卡第二行右列）：桶位度数 + 对齐状态。 */
    if (model->telemetry.position_degrees < 0) {
        lv_label_set_text(s_rt.ov_task_next, "姿态 --");
        lv_obj_set_style_text_color(s_rt.ov_task_next,
                                    lv_color_hex(CLR_DIM), 0);
    } else {
        lv_label_set_text_fmt(s_rt.ov_task_next, "姿态 %d",
                              (int)model->telemetry.position_degrees);
        lv_obj_set_style_text_color(
            s_rt.ov_task_next,
            lv_color_hex(model->telemetry.bucket_aligned
                             ? CLR_GREEN : CLR_WHITE), 0);
    }

    /* Core metrics row 1: water / flow / temperature (统一单位). */
    char buf[32];
    if (model->telemetry.water_level_valid) {
        snprintf(buf, sizeof(buf), "水量 %" PRIu32 " mL",
                 model->telemetry.water_ml);
        lv_label_set_text(s_rt.ov_m1, buf);
        lv_obj_set_style_text_color(s_rt.ov_m1, lv_color_hex(CLR_WHITE), 0);
    } else {
        lv_label_set_text(s_rt.ov_m1, "水量 --");
        lv_obj_set_style_text_color(s_rt.ov_m1, lv_color_hex(CLR_DIM), 0);
    }
    /* 流量以累计脉冲显示（固件无 L/min 速率接口，不虚构单位）。明确标注
     * "脉冲"避免出现歧义的 "0p" 之类字符串。 */
    snprintf(buf, sizeof(buf), "脉冲 %" PRIu32,
             model->telemetry.flow_pulses);
    lv_label_set_text(s_rt.ov_m2, buf);
    lv_obj_set_style_text_color(s_rt.ov_m2, lv_color_hex(CLR_DIM), 0);
    if (model->telemetry.environment_valid) {
        int t10 = (int)(model->telemetry.temperature_c * 10.0f);
        int tw = t10 / 10;
        int tf = t10 % 10;
        if (tf < 0) tf = -tf;
        snprintf(buf, sizeof(buf), "温度 %d.%dC", tw, tf);
        lv_label_set_text(s_rt.ov_m3, buf);
        lv_obj_set_style_text_color(s_rt.ov_m3, lv_color_hex(CLR_WHITE), 0);
    } else {
        lv_label_set_text(s_rt.ov_m3, "温度 --");
        lv_obj_set_style_text_color(s_rt.ov_m3, lv_color_hex(CLR_DIM), 0);
    }

    /* Core metrics row 2: humidity / water level / UV. */
    if (model->telemetry.environment_valid) {
        int rh = (int)(model->telemetry.humidity_rh + 0.5f);
        snprintf(buf, sizeof(buf), "湿度 %d%%RH", rh);
        lv_label_set_text(s_rt.ov_b1, buf);
        lv_obj_set_style_text_color(s_rt.ov_b1, lv_color_hex(CLR_WHITE), 0);
    } else {
        lv_label_set_text(s_rt.ov_b1, "湿度 --");
        lv_obj_set_style_text_color(s_rt.ov_b1, lv_color_hex(CLR_DIM), 0);
    }
    if (model->telemetry.water_level_valid) {
        lv_label_set_text(s_rt.ov_b2,
                          model->telemetry.water_full ? "水位 FULL"
                                                      : "水位 OK");
        lv_obj_set_style_text_color(s_rt.ov_b2, lv_color_hex(CLR_GREEN), 0);
    } else {
        lv_label_set_text(s_rt.ov_b2, "水位 --");
        lv_obj_set_style_text_color(s_rt.ov_b2, lv_color_hex(CLR_DIM), 0);
    }
    if (model->telemetry.uv_state == 2U) {
        lv_label_set_text(s_rt.ov_b3, "UV ON");
        lv_obj_set_style_text_color(s_rt.ov_b3, lv_color_hex(CLR_ORANGE), 0);
    } else if (model->telemetry.uv_state == 1U) {
        lv_label_set_text(s_rt.ov_b3, "UV OFF");
        lv_obj_set_style_text_color(s_rt.ov_b3, lv_color_hex(CLR_DIM), 0);
    } else {
        lv_label_set_text(s_rt.ov_b3, "UV UNKNOWN");
        lv_obj_set_style_text_color(s_rt.ov_b3, lv_color_hex(CLR_ORANGE), 0);
    }
}

static void refresh_telemetry(const display_model_t *model)
{
    const display_telemetry_t *t = &model->telemetry;
    char buf[32];

    /* WATER */
    if (t->water_level_valid) {
        snprintf(buf, sizeof(buf), "%s %" PRIu32 "mL",
                 t->water_full ? "FULL" : "OK", t->water_ml);
        lv_label_set_text(s_rt.tl_water, buf);
        lv_obj_set_style_text_color(s_rt.tl_water, lv_color_hex(CLR_WHITE), 0);
    } else {
        lv_label_set_text(s_rt.tl_water, "N/A");
        lv_obj_set_style_text_color(s_rt.tl_water, lv_color_hex(CLR_DIM), 0);
    }
    snprintf(buf, sizeof(buf), "FLOW %" PRIu32, t->flow_pulses);
    lv_label_set_text(s_rt.tl_water_meta, buf);

    /* QUALITY */
    if (t->turbidity_valid) {
        snprintf(buf, sizeof(buf), "~%u NTU",
                 (unsigned)(t->turbidity_ntu + 0.5f));
        lv_label_set_text(s_rt.tl_quality, buf);
        lv_obj_set_style_text_color(s_rt.tl_quality,
                                    lv_color_hex(CLR_WHITE), 0);
        snprintf(buf, sizeof(buf), "RAW %u", (unsigned)t->turbidity_raw);
        lv_label_set_text(s_rt.tl_quality_raw, buf);
    } else {
        lv_label_set_text(s_rt.tl_quality, "N/A");
        lv_obj_set_style_text_color(s_rt.tl_quality,
                                    lv_color_hex(CLR_DIM), 0);
        lv_label_set_text(s_rt.tl_quality_raw, "RAW --");
    }

    /* ENV */
    if (t->environment_valid) {
        int t10 = (int)(t->temperature_c * 10.0f);
        int tw = t10 / 10;
        int tf = t10 % 10;
        if (tf < 0) tf = -tf;
        int rh = (int)(t->humidity_rh + 0.5f);
        snprintf(buf, sizeof(buf), "%d.%dC %d%%RH", tw, tf, rh);
        lv_label_set_text(s_rt.tl_env, buf);
        lv_obj_set_style_text_color(s_rt.tl_env, lv_color_hex(CLR_WHITE), 0);
        lv_label_set_text(s_rt.tl_env_meta, "SHT OK");
    } else {
        lv_label_set_text(s_rt.tl_env, "N/A");
        lv_obj_set_style_text_color(s_rt.tl_env, lv_color_hex(CLR_DIM), 0);
        lv_label_set_text(s_rt.tl_env_meta, "SHT --");
    }

    /* MOTION */
    if (t->position_degrees < 0) {
        lv_label_set_text(s_rt.tl_motion, "POS --");
        lv_obj_set_style_text_color(s_rt.tl_motion,
                                    lv_color_hex(CLR_DIM), 0);
    } else {
        int32_t target = t->target_position_degrees;
        if (target < 0) {
            snprintf(buf, sizeof(buf), "POS %d", (int)t->position_degrees);
        } else {
            snprintf(buf, sizeof(buf), "POS %d>%d", (int)t->position_degrees,
                     (int)target);
        }
        lv_label_set_text(s_rt.tl_motion, buf);
        lv_obj_set_style_text_color(s_rt.tl_motion,
                                    lv_color_hex(CLR_WHITE), 0);
    }
    snprintf(buf, sizeof(buf), "PWM %u B%u", (unsigned)t->position_pwm,
             (unsigned)t->bl50_pwm);
    lv_label_set_text(s_rt.tl_motion_meta, buf);

    /* INPUTS: 六路霍尔 0/45/90/180/270/BL，来自 stable_hall_mask。
     * 快照有效且新鲜：激活=实心 ON、未激活=空心 OFF；快照缺失/读取失败
     * 才显示 UNKNOWN（黄框）；位置 FSM 故障=红叉。没有磁铁触发绝不是
     * UNKNOWN（hall_valid 由快照可读性决定，而非掩码非零）。 */
    bool output_fault = t->safety_fault;
    bool input_fault = output_fault || t->hall_fault;
    const uint8_t hall_bits[6] = {
        XIAOJING_MCP_INT_HALL_0, XIAOJING_MCP_INT_HALL_45,
        XIAOJING_MCP_INT_HALL_90, XIAOJING_MCP_INT_HALL_180,
        XIAOJING_MCP_INT_HALL_270, XIAOJING_MCP_INT_HALL_BLDC
    };
    for (uint8_t i = 0; i < 6; ++i) {
        bool active = (t->hall_stable_mask & hall_bits[i]) != 0U;
        s_input_led_state[i] = display_position_led_state(active,
                                                          t->hall_valid,
                                                          input_fault);
        lv_obj_invalidate(s_rt.tl_input_leds[i]);
    }

    /* OUTPUTS: 形状+颜色双编码（ON/OFF/UNKNOWN/FAULT/UNSUPPORTED/COOLDOWN）。
     * 状态来自服务/HAL 快照（bootstrap 已用 safety MCP 实际状态融合），
     * safety 的 known 位只表示"已知"，绝不据此推断 OFF。固件未支持的输出
     * 显示灰色 --。第 6 位 = 耦合热风模块（风扇+加热丝并联，单继电器），
     * 冷却锁定显示蓝色 COOLDOWN。 */
    uint8_t outs[6] = {
        t->source_valve_state, t->transfer_valve_state, t->drain_state,
        t->detergent_state, t->uv_state, t->hot_air_state
    };
    bool hot_air_cooldown = t->hot_air_cooldown_ms > 0U;
    for (uint8_t i = 0; i < 6; ++i) {
        bool supported = (t->output_supported_mask & (1U << i)) != 0U;
        if (i == 5) {
            s_output_led_state[i] = display_hot_air_led_state(
                outs[i], supported, output_fault, hot_air_cooldown);
        } else {
            s_output_led_state[i] = display_output_led_state(
                outs[i], supported, output_fault);
        }
        lv_obj_invalidate(s_rt.tl_output_leds[i]);
    }

    /* SYSTEM: MEM / PAJ / MCP / FAULT。 */
    snprintf(buf, sizeof(buf), "MEM %" PRIu32 "K",
             t->internal_free_bytes / 1024U);
    lv_label_set_text(s_rt.tl_sys_mem, buf);
    if (t->gesture_available) {
        lv_label_set_text(s_rt.tl_sys_paj, "PAJ OK");
        lv_obj_set_style_text_color(s_rt.tl_sys_paj,
                                    lv_color_hex(CLR_GREEN), 0);
    } else {
        lv_label_set_text(s_rt.tl_sys_paj, "PAJ --");
        lv_obj_set_style_text_color(s_rt.tl_sys_paj,
                                    lv_color_hex(CLR_DIM), 0);
    }
    if (t->mcp_unknown_mask == 0U) {
        lv_label_set_text(s_rt.tl_sys_mcp, "MCP OK");
        lv_obj_set_style_text_color(s_rt.tl_sys_mcp,
                                    lv_color_hex(CLR_GREEN), 0);
    } else {
        lv_label_set_text(s_rt.tl_sys_mcp, "MCP ?");
        lv_obj_set_style_text_color(s_rt.tl_sys_mcp,
                                    lv_color_hex(CLR_ORANGE), 0);
    }
    if (t->safety_fault) {
        lv_label_set_text_fmt(s_rt.tl_sys_fault, "FAULT E%u",
                              (unsigned)t->fault_code);
        lv_obj_set_style_text_color(s_rt.tl_sys_fault,
                                    lv_color_hex(CLR_RED), 0);
    } else {
        lv_label_set_text(s_rt.tl_sys_fault, "FAULT --");
        lv_obj_set_style_text_color(s_rt.tl_sys_fault,
                                    lv_color_hex(CLR_DIM), 0);
    }
}

static void refresh_voice(const display_model_t *model)
{
    const display_telemetry_t *t = &model->telemetry;
    display_voice_state_t vs = model->voice_state;
    uint32_t color = CLR_DIM;
    const char *state_name = display_voice_state_name(vs);
    if (vs == DISPLAY_VOICE_LISTENING) color = CLR_CYAN;
    else if (vs == DISPLAY_VOICE_THINKING) color = CLR_ORANGE;
    else if (vs == DISPLAY_VOICE_REPLY) color = CLR_GREEN;
    else if (vs == DISPLAY_VOICE_ERROR) color = CLR_RED;
    set_dot_color(s_rt.vo_state_dot, color);
    lv_label_set_text(s_rt.vo_state, state_name);
    lv_obj_set_style_text_color(s_rt.vo_state, lv_color_hex(color), 0);

    char backend[64];
    display_backend_label(t->wake_backend, t->backend_switching,
                          (display_wake_backend_t)t->backend_switch_target,
                          t->backend_switch_result, backend, sizeof(backend));
    lv_label_set_text(s_rt.vo_backend, backend);
    lv_obj_set_style_text_color(s_rt.vo_backend,
        lv_color_hex(t->backend_switching ? CLR_ORANGE : CLR_CYAN), 0);

    /* 麦克风：无真实 RMS/peak，仅"采集中"扫描动画，不伪装音量表。 */
    for (uint8_t i = 0; i < 5; ++i) {
        uint32_t mic_color = CLR_GLASS_BORDER;
        if (vs == DISPLAY_VOICE_LISTENING) {
            /* 扫描灯：以固定相位逐段点亮（非音量高度）。 */
            int rel = (int)((s_rt.heartbeat_phase + i) % 5);
            mic_color = (rel == 0) ? CLR_CYAN : CLR_GLASS_BORDER;
        } else if (vs == DISPLAY_VOICE_THINKING) {
            mic_color = CLR_ORANGE;
        }
        lv_obj_set_style_bg_color(s_rt.vo_mic_bars[i],
                                  lv_color_hex(mic_color), 0);
        lv_obj_set_height(s_rt.vo_mic_bars[i], 5);
    }

    /* 云端状态：2=网络就绪（Wi-Fi 有 IP，非云端确认）；1=离线；0=未知。
     * 绝不伪报"云端 READY"。 */
    if (t->voice_cloud_state == 2U) {
        lv_label_set_text(s_rt.vo_cloud, "网络就绪");
        lv_obj_set_style_text_color(s_rt.vo_cloud,
                                    lv_color_hex(CLR_CYAN), 0);
    } else if (t->voice_cloud_state == 1U) {
        lv_label_set_text(s_rt.vo_cloud, "云端 OFFLINE");
        lv_obj_set_style_text_color(s_rt.vo_cloud,
                                    lv_color_hex(CLR_ORANGE), 0);
    } else {
        lv_label_set_text(s_rt.vo_cloud, "云端 --");
        lv_obj_set_style_text_color(s_rt.vo_cloud,
                                    lv_color_hex(CLR_DIM), 0);
    }

    /* UTF-8 安全截断（含省略号）+ 字形兜底；换行交给 LVGL LONG_WRAP。
     * UI-P2-1：payload 若已带 "你："（或前导空白 + 你:），先剥离再统一前缀，
     * 绝不再现 "你：你：…" 重复。 */
    const char *tr_src = model->transcript[0]
        ? model->transcript : "你：--";
    tr_src = display_skip_speaker_prefix(tr_src, "你：");
    tr_src = display_skip_speaker_prefix(tr_src, "你:");
    char tr[112];
    char text[104];
    display_utf8_truncate(tr, sizeof(tr), tr_src, 96);
    display_utf8_sanitize(text, sizeof(text), tr, glyph_supported);
    lv_label_set_text_fmt(s_rt.vo_transcript, "你：%s", text);

    const char *reply_src = vs == DISPLAY_VOICE_ERROR
        ? model->error : model->reply;
    reply_src = display_skip_speaker_prefix(reply_src, "小净：");
    reply_src = display_skip_speaker_prefix(reply_src, "小净:");
    char rp[224];
    char reply[208];
    display_utf8_truncate(rp, sizeof(rp),
                          reply_src[0] ? reply_src : "等待语音输入",
                          200);
    display_utf8_sanitize(reply, sizeof(reply), rp, glyph_supported);
    lv_label_set_text_fmt(s_rt.vo_reply, "小净：%s", reply);
}

static void refresh_ui(lv_timer_t *timer)
{
    (void)timer;
    uint32_t tick = atomic_fetch_add_explicit(&s_refresh_ticks, 1U,
                                              memory_order_relaxed) + 1U;
    update_heartbeat();

    if (!model_try_lock()) {
        atomic_fetch_add_explicit(&s_refresh_lock_misses, 1U,
                                  memory_order_relaxed);
        return;
    }
    display_model_t model;
    model = s_model;
    xSemaphoreGive(s_model_lock);

    if ((tick % 100U) == 0U) {
        ESP_LOGI(TAG, "heartbeat tick=%" PRIu32 " phase=%u",
                 tick, (unsigned)s_rt.heartbeat_phase);
    }

    int requested_page = atomic_load_explicit(&s_requested_page,
                                              memory_order_acquire);
    bool page_pending = requested_page >= DISPLAY_PAGE_OVERVIEW &&
                        requested_page < DISPLAY_PAGE_COUNT &&
                        requested_page != atomic_load_explicit(
                            &s_current_page, memory_order_acquire);
    if (!page_pending && (tick % 16U) != 0U) return;
    if (model.revision == s_rt.rendered_revision && !page_pending) return;

    int page_index = requested_page;
    if (page_index < DISPLAY_PAGE_OVERVIEW || page_index >= DISPLAY_PAGE_COUNT) {
        page_index = DISPLAY_PAGE_OVERVIEW;
        if (model.voice_state != DISPLAY_VOICE_IDLE)
            page_index = DISPLAY_PAGE_VOICE;
        else if (model.washing)
            page_index = DISPLAY_PAGE_OVERVIEW;
    }

    refresh_header(&model, page_index);
    if (page_index == DISPLAY_PAGE_OVERVIEW) {
        refresh_overview(&model);
    } else if (page_index == DISPLAY_PAGE_TELEMETRY) {
        refresh_telemetry(&model);
    } else if (page_index == DISPLAY_PAGE_VOICE) {
        refresh_voice(&model);
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
    esp_err_t err = spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;
    s_rt.spi_owned = true;

    esp_lcd_panel_io_spi_config_t io_cfg = ILI9341_PANEL_IO_SPI_CONFIG(
        XIAOJING_GPIO_LCD_CS, XIAOJING_GPIO_LCD_DC, NULL, NULL);
    io_cfg.trans_queue_depth = 10;
    io_cfg.pclk_hz = 40 * 1000 * 1000;
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
        .trans_size = LCD_HRES * LCD_BUFFER_LINES,
        .hres = LCD_HRES,
        .vres = LCD_VRES,
        .monochrome = false,
        .rotation = {
            .swap_xy = true,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
        },
    };
    free(s_rt.dma_reserve);
    s_rt.dma_reserve = NULL;
    s_rt.display = lvgl_port_add_disp(&disp_cfg);
    if (!s_rt.display) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed (largest DMA=%u)",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
        return ESP_ERR_NO_MEM;
    }
    if (!lvgl_port_lock(1000U)) return ESP_ERR_TIMEOUT;
    build_overview_page();
    build_telemetry_page();
    build_voice_page();
    lv_scr_load(s_rt.pages[DISPLAY_PAGE_OVERVIEW]);
    atomic_store_explicit(&s_current_page, DISPLAY_PAGE_OVERVIEW,
                          memory_order_release);
    s_rt.refresh_timer = lv_timer_create(refresh_ui, 250U, NULL);
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
        memset(s_rt.hdr_bar, 0, sizeof(s_rt.hdr_bar));
        memset(s_rt.hdr_pct, 0, sizeof(s_rt.hdr_pct));
        memset(s_rt.hdr_wifi_dot, 0, sizeof(s_rt.hdr_wifi_dot));
        memset(s_rt.hdr_ble_dot, 0, sizeof(s_rt.hdr_ble_dot));
        memset(s_rt.heartbeat, 0, sizeof(s_rt.heartbeat));
        memset(s_rt.ov_flow_nodes, 0, sizeof(s_rt.ov_flow_nodes));
        memset(s_rt.tl_input_leds, 0, sizeof(s_rt.tl_input_leds));
        memset(s_rt.tl_output_leds, 0, sizeof(s_rt.tl_output_leds));
        memset(s_rt.vo_mic_bars, 0, sizeof(s_rt.vo_mic_bars));
        s_rt.rendered_revision = 0U;
        s_rt.heartbeat_phase = 0U;
        atomic_store_explicit(&s_refresh_ticks, 0U, memory_order_relaxed);
        atomic_store_explicit(&s_refresh_lock_misses, 0U,
                              memory_order_relaxed);
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
        atomic_init(&s_current_page, DISPLAY_PAGE_OVERVIEW);
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
    ESP_LOGI(TAG, "display running (hardware=%s pages=%u)",
             s_rt.hardware_enabled ? "on" : "off",
             (unsigned)DISPLAY_PAGE_COUNT);
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
    if (s_model.wifi_connected != wifi_connected ||
        s_model.ble_connected != ble_connected) {
        s_model.wifi_connected = wifi_connected;
        s_model.ble_connected = ble_connected;
        s_model.revision = next_revision(s_model.revision);
    }
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
    if (s_model.washing != washing ||
        s_model.wash_progress != progress ||
        strcmp(s_model.wash_step, step) != 0) {
        s_model.washing = washing;
        s_model.wash_progress = progress;
        copy_text(s_model.wash_step, sizeof(s_model.wash_step), step);
        s_model.revision = next_revision(s_model.revision);
    }
    xSemaphoreGive(s_model_lock);
    return ESP_OK;
}

esp_err_t display_service_set_telemetry(const display_telemetry_t *telemetry)
{
    if (!telemetry || telemetry->wake_backend > DISPLAY_WAKE_ESP_SR ||
        telemetry->wifi_state > DISPLAY_WIFI_FAILED ||
        telemetry->ble_state > DISPLAY_BLE_RECOVER_FAILED ||
        telemetry->backend_switch_result > DISPLAY_BACKEND_TIMEOUT ||
        telemetry->backend_switch_target > DISPLAY_WAKE_ESP_SR)
        return ESP_ERR_INVALID_ARG;
    if (!model_lock()) return ESP_ERR_INVALID_STATE;
    display_telemetry_t merged = *telemetry;
    if (!merged.environment_valid && s_model.telemetry.environment_valid) {
        merged.temperature_c = s_model.telemetry.temperature_c;
        merged.humidity_rh = s_model.telemetry.humidity_rh;
        merged.environment_valid = true;
    }
    if (!merged.water_level_valid && s_model.telemetry.water_level_valid) {
        merged.water_full = s_model.telemetry.water_full;
        merged.water_ml = s_model.telemetry.water_ml;
        merged.water_level_valid = true;
    }
    if (!telemetry_equal_for_ui(&merged, &s_model.telemetry)) {
        s_model.telemetry = merged;
        s_model.revision = next_revision(s_model.revision);
    }
    xSemaphoreGive(s_model_lock);
    return ESP_OK;
}

esp_err_t display_service_show_page(display_page_t page)
{
    if (page < DISPLAY_PAGE_OVERVIEW || page >= DISPLAY_PAGE_COUNT)
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
    if (requested >= DISPLAY_PAGE_OVERVIEW && requested < DISPLAY_PAGE_COUNT)
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

