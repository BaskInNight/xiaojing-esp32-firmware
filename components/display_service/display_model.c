#include "display_service.h"

#include <stdio.h>
#include <string.h>

/* Pure-C formatting / state-label helpers, host-testable (no LVGL/FreeRTOS
 * dependency).  The synchronized model itself lives in display_service.c. */

/* UTF-8-safe bounded copy with "..." suffix.  Never truncates a multi-byte
 * character; max_bytes caps the payload before the ellipsis is appended. */
size_t display_utf8_truncate(char *dst, size_t capacity, const char *src,
                             size_t max_bytes)
{
    if (!dst || capacity == 0U) return 0U;
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
    return n;
}

typedef bool (*display_glyph_ok_fn)(uint32_t codepoint);

/* Sanitize a dynamic UTF-8 string for the telemetry font: codepoints the font
 * cannot render are replaced with '?' so the HMI never shows a tofu box.
 * Truncation is UTF-8-safe (never splits a multi-byte sequence). */
size_t display_utf8_sanitize(char *dst, size_t capacity, const char *src,
                             display_glyph_ok_fn is_supported)
{
    if (!dst || capacity == 0U) return 0U;
    if (!src) src = "";
    size_t out = 0U;
    size_t cap = capacity - 1U;
    const unsigned char *p = (const unsigned char *)src;
    while (*p != 0U && out < cap) {
        uint32_t cp;
        size_t len;
        unsigned char c = *p;
        if (c < 0x80U) {
            cp = c; len = 1U;
        } else if ((c & 0xe0U) == 0xc0U && p[1] != 0U) {
            cp = ((uint32_t)(c & 0x1fU) << 6U) | (uint32_t)(p[1] & 0x3fU);
            len = 2U;
        } else if ((c & 0xf0U) == 0xe0U && p[1] != 0U && p[2] != 0U) {
            cp = ((uint32_t)(c & 0x0fU) << 12U) |
                 ((uint32_t)(p[1] & 0x3fU) << 6U) |
                 (uint32_t)(p[2] & 0x3fU);
            len = 3U;
        } else if ((c & 0xf8U) == 0xf0U && p[1] != 0U && p[2] != 0U &&
                   p[3] != 0U) {
            cp = ((uint32_t)(c & 0x07U) << 18U) |
                 ((uint32_t)(p[1] & 0x3fU) << 12U) |
                 ((uint32_t)(p[2] & 0x3fU) << 6U) |
                 (uint32_t)(p[3] & 0x3fU);
            len = 4U;
        } else {
            cp = 0xFFFD; len = 1U; /* invalid byte */
        }
        bool ok = cp <= 0xFFFFU &&
                  (!is_supported || is_supported((uint32_t)cp));
        if (!ok) {
            dst[out++] = '?';
        } else if (out + len <= cap) {
            memcpy(dst + out, p, len);
            out += len;
        } else {
            break;
        }
        p += len;
    }
    dst[out] = '\0';
    return out;
}

/* Voice backend status line.  Handles committed backend, in-flight switching
 * (target must NOT be committed early), and failure/timeout rollback. */
void display_backend_label(display_wake_backend_t committed, bool switching,
                           display_wake_backend_t target, uint8_t result,
                           char *buf, size_t capacity)
{
    const char *edge_name = "小净小净";
    const char *sr_name = "小爱同学";
    const char *edge_label = "EDGE";
    const char *sr_label = "ESP-SR";
    if (!buf || capacity == 0U) return;

    if (switching) {
        const char *target_name = target == DISPLAY_WAKE_ESP_SR
            ? sr_name : edge_name;
        snprintf(buf, capacity, "正在切换 → %s", target_name);
    } else if (result == DISPLAY_BACKEND_SUCCESS) {
        /* 切换成功：目标已成为生效 backend。 */
        const char *name = committed == DISPLAY_WAKE_ESP_SR
            ? sr_name : edge_name;
        snprintf(buf, capacity, "已切换：%s", name);
    } else if (result == DISPLAY_BACKEND_FAILURE) {
        const char *name = committed == DISPLAY_WAKE_ESP_SR
            ? sr_name : edge_name;
        snprintf(buf, capacity, "切换失败，仍为%s", name);
    } else if (result == DISPLAY_BACKEND_TIMEOUT) {
        const char *name = committed == DISPLAY_WAKE_ESP_SR
            ? sr_name : edge_name;
        snprintf(buf, capacity, "切换超时，保持%s", name);
    } else if (committed == DISPLAY_WAKE_ESP_SR) {
        snprintf(buf, capacity, "%s - %s", sr_label, sr_name);
    } else {
        snprintf(buf, capacity, "%s - %s", edge_label, edge_name);
    }
}

const char *display_wifi_state_name(uint8_t state)
{
    switch (state) {
    case DISPLAY_WIFI_OFF: return "OFF";
    case DISPLAY_WIFI_CONNECTING: return "CONN";
    case DISPLAY_WIFI_IP: return "IP";
    case DISPLAY_WIFI_FAILED: return "FAIL";
    default: return "--";
    }
}

const char *display_ble_state_name(uint8_t state)
{
    switch (state) {
    case DISPLAY_BLE_OFF: return "OFF";
    case DISPLAY_BLE_ADVERTISING: return "ADV";
    case DISPLAY_BLE_CONNECTED: return "CONN";
    case DISPLAY_BLE_VOICE_SUSPENDED: return "SUSP";
    case DISPLAY_BLE_RECOVER_FAILED: return "FAIL";
    default: return "--";
    }
}

const char *display_voice_state_name(display_voice_state_t state)
{
    switch (state) {
    case DISPLAY_VOICE_IDLE: return "待命";
    case DISPLAY_VOICE_LISTENING: return "正在倾听";
    case DISPLAY_VOICE_THINKING: return "正在思考";
    case DISPLAY_VOICE_REPLY: return "正在回复";
    case DISPLAY_VOICE_ERROR: return "链路失败";
    default: return "待命";
    }
}

/* Same-frame voice-cloud state from the current Wi-Fi link state.  There is
 * no real cloud-readiness API on the bench build: Wi-Fi with an IP proves the
 * network is reachable but NOT that the cloud route is ready, so the mapping
 * yields "网络就绪" (2) — never a fabricated "云端 READY".  Any other Wi-Fi
 * state is honest "离线" (1).  Computing this from the SAME frame's Wi-Fi
 * snapshot prevents "Wi-Fi connected but cloud shows OFFLINE". */
uint8_t display_cloud_from_wifi(uint8_t wifi_state)
{
    return wifi_state == DISPLAY_WIFI_IP ? 2U : 1U;
}

/* ---- Header icon link colors (RGB565-hex, mirror of display_service.c CLR) ---- */

#define ICON_CLR_DIM      0x93a9bb
#define ICON_CLR_CYAN     0x00c8ff
#define ICON_CLR_BLUE     0x00a2ff
#define ICON_CLR_BLUEGRAY 0x7a93b5
#define ICON_CLR_ORANGE   0xffb020
#define ICON_CLR_RED      0xff5b66

uint32_t display_wifi_color(uint8_t state)
{
    switch (state) {
    case DISPLAY_WIFI_CONNECTING: return ICON_CLR_ORANGE;
    case DISPLAY_WIFI_IP:         return ICON_CLR_CYAN;
    case DISPLAY_WIFI_FAILED:     return ICON_CLR_RED;
    default:                      return ICON_CLR_DIM;
    }
}

uint32_t display_ble_color(uint8_t state)
{
    switch (state) {
    case DISPLAY_BLE_ADVERTISING:    return ICON_CLR_BLUEGRAY;
    case DISPLAY_BLE_CONNECTED:      return ICON_CLR_BLUE;
    case DISPLAY_BLE_VOICE_SUSPENDED:return ICON_CLR_ORANGE;
    case DISPLAY_BLE_RECOVER_FAILED: return ICON_CLR_RED;
    default:                         return ICON_CLR_DIM;
    }
}

/* Icon shape semantics: a disconnected/unknown link is an outline; only a
 * live link is filled; a failed link is an outline + X.  CONNECTING /
 * ADVERTISING get a moving marker on top of the outline.  No state is ever
 * rendered as "connected" merely because it is reachable. */
uint8_t display_wifi_icon_style(uint8_t state)
{
    switch (state) {
    case DISPLAY_WIFI_CONNECTING: return DISPLAY_ICON_STYLE_ADV;
    case DISPLAY_WIFI_IP:         return DISPLAY_ICON_STYLE_ON;
    case DISPLAY_WIFI_FAILED:     return DISPLAY_ICON_STYLE_FAIL;
    default:                      return DISPLAY_ICON_STYLE_OFF;
    }
}

uint8_t display_ble_icon_style(uint8_t state)
{
    switch (state) {
    case DISPLAY_BLE_ADVERTISING:    return DISPLAY_ICON_STYLE_ADV;
    case DISPLAY_BLE_CONNECTED:      return DISPLAY_ICON_STYLE_ON;
    case DISPLAY_BLE_VOICE_SUSPENDED:return DISPLAY_ICON_STYLE_SUSPEND;
    case DISPLAY_BLE_RECOVER_FAILED: return DISPLAY_ICON_STYLE_FAIL;
    default:                         return DISPLAY_ICON_STYLE_OFF;
    }
}

/* Page-2 OUTPUTS truth: fault wins, then "not supported/uninitialized"
 * renders "--" instead of a fabricated UNKNOWN; otherwise the real service
 * tri-state (1=OFF hollow, 2=ON solid) is shown. */
uint8_t display_output_led_state(uint8_t output_state, bool supported,
                                 bool fault)
{
    if (fault) return DISPLAY_LED_FAULT;
    if (!supported) return DISPLAY_LED_UNSUPPORTED;
    if (output_state == 2U) return DISPLAY_LED_ON;
    if (output_state == 1U) return DISPLAY_LED_OFF;
    return DISPLAY_LED_UNKNOWN;
}

/* 耦合热风模块 LED：冷却锁定显示 COOLDOWN（蓝色，优先于 ON/OFF），
 * 故障仍为红叉。命令态 tri-state（ON=实心/OFF=空心/UNKNOWN=黄框）。 */
uint8_t display_hot_air_led_state(uint8_t output_state, bool supported,
                                  bool fault, bool cooldown_active)
{
    if (fault) return DISPLAY_LED_FAULT;
    if (!supported) return DISPLAY_LED_UNSUPPORTED;
    if (cooldown_active) return DISPLAY_LED_COOLDOWN;
    if (output_state == 2U) return DISPLAY_LED_ON;
    if (output_state == 1U) return DISPLAY_LED_OFF;
    return DISPLAY_LED_UNKNOWN;
}

/* Page-2 position inputs: a healthy snapshot means every inactive bit is a
 * known OFF (hollow), never UNKNOWN.  UNKNOWN only appears when the snapshot
 * itself is missing/invalid. */
uint8_t display_position_led_state(bool active, bool snapshot_valid,
                                   bool fault)
{
    if (fault) return DISPLAY_LED_FAULT;
    if (!snapshot_valid) return DISPLAY_LED_UNKNOWN;
    return active ? DISPLAY_LED_ON : DISPLAY_LED_OFF;
}

/* UI-P2-1: strip a leading speaker prefix so the HMI never renders a duplicate
 * "你：你：…" / "小净：小净：…".  Skips optional ASCII whitespace before the
 * prefix, then the exact prefix bytes; returns the pointer just past the prefix,
 * or the original pointer when there is no (whitespace-prefixed) match. */
const char *display_skip_speaker_prefix(const char *text, const char *prefix)
{
    if (!text) return NULL;
    if (!prefix || prefix[0] == '\0') return text;
    const char *p = text;
    while (*p == ' ' || *p == '\t') p++;
    size_t plen = strlen(prefix);
    if (strncmp(p, prefix, plen) == 0) return p + plen;
    return text;
}

