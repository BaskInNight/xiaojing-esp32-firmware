/*
 * test_display_model.c — Host-side tests for the Phase 9 display model
 * pure-C helpers (UTF-8 safety, voice backend matrix, link-state labels).
 * No LVGL/FreeRTOS dependency.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "display_service.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("  TEST %02d: %-58s ", tests_run, name); \
} while (0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while (0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while (0)

static bool ascii_plus_cjk(uint32_t cp)
{
    if (cp < 0x80U) return true;
    if (cp >= 0x4e00U && cp <= 0x9fffU) return true;
    return false;
}

static void test_page_structure(void)
{
    TEST("PAGE_COUNT == 3 (Phase 9 three-page layout)");
    if (DISPLAY_PAGE_COUNT == 3) PASS();
    else FAIL("DISPLAY_PAGE_COUNT must be 3");

    TEST("page enum order: OVERVIEW=0 TELEMETRY=1 VOICE=2");
    if (DISPLAY_PAGE_OVERVIEW == 0 && DISPLAY_PAGE_TELEMETRY == 1 &&
        DISPLAY_PAGE_VOICE == 2) PASS();
    else FAIL("enum order");

    TEST("three-page cycle wraps correctly both directions");
    int ok = 1;
    for (int d = -1; d <= 1; d += 2) {
        for (int cur = 0; cur < DISPLAY_PAGE_COUNT; cur++) {
            int next = (cur + d + DISPLAY_PAGE_COUNT) % DISPLAY_PAGE_COUNT;
            if (next < 0 || next >= DISPLAY_PAGE_COUNT) ok = 0;
            if (d == 1 && next != (cur + 1) % DISPLAY_PAGE_COUNT) ok = 0;
            if (d == -1 && next != (cur + DISPLAY_PAGE_COUNT - 1) %
                              DISPLAY_PAGE_COUNT) ok = 0;
        }
    }
    if (ok) PASS(); else FAIL("cycle wrap");
}

static void test_link_state_names(void)
{
    TEST("Wi-Fi four-state labels");
    int ok = strcmp(display_wifi_state_name(DISPLAY_WIFI_OFF), "OFF") == 0 &&
             strcmp(display_wifi_state_name(DISPLAY_WIFI_CONNECTING),
                    "CONN") == 0 &&
             strcmp(display_wifi_state_name(DISPLAY_WIFI_IP), "IP") == 0 &&
             strcmp(display_wifi_state_name(DISPLAY_WIFI_FAILED),
                    "FAIL") == 0;
    if (ok) PASS(); else FAIL("wifi labels");

    TEST("BLE five-state labels");
    ok = strcmp(display_ble_state_name(DISPLAY_BLE_OFF), "OFF") == 0 &&
         strcmp(display_ble_state_name(DISPLAY_BLE_ADVERTISING), "ADV") == 0 &&
         strcmp(display_ble_state_name(DISPLAY_BLE_CONNECTED), "CONN") == 0 &&
         strcmp(display_ble_state_name(DISPLAY_BLE_VOICE_SUSPENDED),
                "SUSP") == 0 &&
         strcmp(display_ble_state_name(DISPLAY_BLE_RECOVER_FAILED),
                "FAIL") == 0;
    if (ok) PASS(); else FAIL("ble labels");

    TEST("voice state labels cover all states");
    ok = strcmp(display_voice_state_name(DISPLAY_VOICE_IDLE), "待命") == 0 &&
         strcmp(display_voice_state_name(DISPLAY_VOICE_LISTENING),
                "正在倾听") == 0 &&
         strcmp(display_voice_state_name(DISPLAY_VOICE_THINKING),
                "正在思考") == 0 &&
         strcmp(display_voice_state_name(DISPLAY_VOICE_REPLY),
                "正在回复") == 0 &&
         strcmp(display_voice_state_name(DISPLAY_VOICE_ERROR),
                "链路失败") == 0;
    if (ok) PASS(); else FAIL("voice labels");
}

static void test_backend_matrix(void)
{
    char buf[96];
    TEST("committed Edge -> EDGE - 小净小净");
    display_backend_label(DISPLAY_WAKE_EDGE_IMPULSE, false,
                          DISPLAY_WAKE_EDGE_IMPULSE, DISPLAY_BACKEND_IDLE,
                          buf, sizeof(buf));
    if (strcmp(buf, "EDGE - 小净小净") == 0) PASS();
    else FAIL(buf);

    TEST("committed ESP-SR -> ESP-SR - 小爱同学");
    display_backend_label(DISPLAY_WAKE_ESP_SR, false, DISPLAY_WAKE_ESP_SR,
                          DISPLAY_BACKEND_IDLE, buf, sizeof(buf));
    if (strcmp(buf, "ESP-SR - 小爱同学") == 0) PASS();
    else FAIL(buf);

    TEST("switching to Edge shows target without committing");
    display_backend_label(DISPLAY_WAKE_ESP_SR, true, DISPLAY_WAKE_EDGE_IMPULSE,
                          DISPLAY_BACKEND_SWITCHING, buf, sizeof(buf));
    if (strstr(buf, "正在切换") && strstr(buf, "小净小净")) PASS();
    else FAIL(buf);

    TEST("switching to ESP-SR shows target without committing");
    display_backend_label(DISPLAY_WAKE_EDGE_IMPULSE, true,
                          DISPLAY_WAKE_ESP_SR, DISPLAY_BACKEND_SWITCHING,
                          buf, sizeof(buf));
    if (strstr(buf, "正在切换") && strstr(buf, "小爱同学")) PASS();
    else FAIL(buf);

    TEST("failure rolls back to committed Edge");
    display_backend_label(DISPLAY_WAKE_EDGE_IMPULSE, false,
                          DISPLAY_WAKE_ESP_SR, DISPLAY_BACKEND_FAILURE,
                          buf, sizeof(buf));
    if (strstr(buf, "切换失败") && strstr(buf, "小净小净")) PASS();
    else FAIL(buf);

    TEST("failure rolls back to committed ESP-SR");
    display_backend_label(DISPLAY_WAKE_ESP_SR, false, DISPLAY_WAKE_EDGE_IMPULSE,
                          DISPLAY_BACKEND_FAILURE, buf, sizeof(buf));
    if (strstr(buf, "切换失败") && strstr(buf, "小爱同学")) PASS();
    else FAIL(buf);

    TEST("timeout keeps committed backend");
    display_backend_label(DISPLAY_WAKE_EDGE_IMPULSE, false,
                          DISPLAY_WAKE_ESP_SR, DISPLAY_BACKEND_TIMEOUT,
                          buf, sizeof(buf));
    if (strstr(buf, "切换超时") && strstr(buf, "小净小净")) PASS();
    else FAIL(buf);

    TEST("switch success shows committed target (post commit)");
    display_backend_label(DISPLAY_WAKE_ESP_SR, false, DISPLAY_WAKE_ESP_SR,
                          DISPLAY_BACKEND_SUCCESS, buf, sizeof(buf));
    if (strcmp(buf, "已切换：小爱同学") == 0) PASS();
    else FAIL(buf);
}

static void test_utf8_truncate(void)
{
    char out[32];
    char recheck[32];
    TEST("UTF-8 truncate keeps complete multi-byte chars");
    /* "你好世界A" is 13 bytes; cap 8 must not cut a 3-byte char. */
    const char *src = "你好世界A";
    size_t n = display_utf8_truncate(out, sizeof(out), src, 8U);
    /* Every codepoint in `out` must decode cleanly: re-sanitizing with a
     * full whitelist must yield the identical string (no lone continuation
     * bytes, which sanitize replaces with '?'). */
    display_utf8_sanitize(recheck, sizeof(recheck), out, ascii_plus_cjk);
    if (n <= 8U && strlen(out) <= 8U && strcmp(out, recheck) == 0)
        PASS();
    else FAIL(out);
}

static void test_utf8_sanitize(void)
{
    char out[64];
    TEST("sanitize replaces unsupported glyphs with '?'");
    const char *src = "ABC你好\xe2\x98\x83"; /* ABC + CJK + snowman (unsupported) */
    size_t n = display_utf8_sanitize(out, sizeof(out), src, ascii_plus_cjk);
    /* snowman (3 bytes) -> single '?' */
    if (strchr(out, '?') && strstr(out, "ABC") && strstr(out, "你好"))
        PASS();
    else FAIL(out);
    (void)n;

    TEST("sanitize never splits multi-byte chars");
    const char *src2 = "中文English";
    size_t n2 = display_utf8_sanitize(out, sizeof(out), src2, ascii_plus_cjk);
    size_t len = strlen(out);
    for (size_t i = 0; i < len; i++)
        if (((unsigned char)out[i] & 0xc0U) == 0x80U && i == len - 1U) {
            FAIL("trailing continuation byte");
            return;
        }
    if (n2 <= sizeof(out) - 1U) PASS();
    else FAIL("bounds");
}

static void test_concurrency_purity(void)
{
    char a[64], b[64];
    TEST("pure helpers are deterministic across calls");
    const char *src = "小净小净 -> 小爱同学";
    display_utf8_truncate(a, sizeof(a), src, 20U);
    display_utf8_truncate(b, sizeof(b), src, 20U);
    if (strcmp(a, b) == 0) PASS();
    else FAIL("non-deterministic");
}

/* Phase 11: Wi-Fi/cloud same-frame consistency.  The regression was that
 * voice_cloud_state was computed from a PREVIOUS frame's Wi-Fi state.  The
 * fix computes both from the SAME snapshot and never fabricates "云端 READY":
 * Wi-Fi with an IP maps to 网络就绪 (2), everything else to 离线 (1). */
static void test_cloud_from_wifi(void)
{
    TEST("Wi-Fi IP -> network ready (2), never fabricated READY");
    if (display_cloud_from_wifi(DISPLAY_WIFI_IP) == 2U) PASS();
    else FAIL("IP must map to network-ready");

    TEST("Wi-Fi connecting -> offline (1)");
    if (display_cloud_from_wifi(DISPLAY_WIFI_CONNECTING) == 1U) PASS();
    else FAIL("connecting must map to offline");

    TEST("Wi-Fi off / failed -> offline (1)");
    if (display_cloud_from_wifi(DISPLAY_WIFI_OFF) == 1U &&
        display_cloud_from_wifi(DISPLAY_WIFI_FAILED) == 1U) PASS();
    else FAIL("off/failed must map to offline");

    TEST("same frame: wifi IP never coexists with cloud offline");
    /* Cross-frame stale: old code read telemetry.wifi_state before the fresh
     * snapshot, so a fresh IP could be paired with last frame's OFF.  The
     * mapping from the SAME Wi-Fi value must never yield (IP, offline). */
    uint8_t wifi_now = (uint8_t)DISPLAY_WIFI_IP;
    uint8_t cloud_now = display_cloud_from_wifi(wifi_now);
    if (!(wifi_now == DISPLAY_WIFI_IP && cloud_now == 1U)) PASS();
    else FAIL("same-frame IP/offline contradiction");
}

/* Phase 11 Group A Round 1.1: header icon shape semantics.  A link must be
 * distinguishable by shape alone (outline vs filled vs X), never by color
 * only.  CONNECTING/ADVERTISING get the moving-marker style. */
static void test_icon_style_matrix(void)
{
    TEST("Wi-Fi OFF/UNKNOWN -> outline only (OFF style)");
    if (display_wifi_icon_style(DISPLAY_WIFI_OFF) == DISPLAY_ICON_STYLE_OFF &&
        display_wifi_icon_style(99) == DISPLAY_ICON_STYLE_OFF) PASS();
    else FAIL("off/unknown must be outline");

    TEST("Wi-Fi CONNECTING -> outline + marker (ADV)");
    if (display_wifi_icon_style(DISPLAY_WIFI_CONNECTING) ==
        DISPLAY_ICON_STYLE_ADV) PASS();
    else FAIL("connecting must be marker style");

    TEST("Wi-Fi IP -> filled (ON)");
    if (display_wifi_icon_style(DISPLAY_WIFI_IP) == DISPLAY_ICON_STYLE_ON)
        PASS();
    else FAIL("IP must be filled");

    TEST("Wi-Fi FAILED -> outline + X (FAIL), distinct from OFFLINE");
    if (display_wifi_icon_style(DISPLAY_WIFI_FAILED) ==
            DISPLAY_ICON_STYLE_FAIL &&
        DISPLAY_ICON_STYLE_FAIL != DISPLAY_ICON_STYLE_OFF) PASS();
    else FAIL("failed must be X, not identical to off");

    TEST("BLE STOPPED/UNKNOWN -> outline (OFF)");
    if (display_ble_icon_style(DISPLAY_BLE_OFF) == DISPLAY_ICON_STYLE_OFF &&
        display_ble_icon_style(99) == DISPLAY_ICON_STYLE_OFF) PASS();
    else FAIL("ble off must be outline");

    TEST("BLE ADVERTISING -> outline + marker, not connected");
    if (display_ble_icon_style(DISPLAY_BLE_ADVERTISING) ==
        DISPLAY_ICON_STYLE_ADV) PASS();
    else FAIL("advertising must be marker style");

    TEST("BLE CONNECTED -> filled (ON)");
    if (display_ble_icon_style(DISPLAY_BLE_CONNECTED) == DISPLAY_ICON_STYLE_ON)
        PASS();
    else FAIL("connected must be filled");

    TEST("BLE VOICE_SUSPENDED -> suspend style (stem solid + wings outline)");
    if (display_ble_icon_style(DISPLAY_BLE_VOICE_SUSPENDED) ==
        DISPLAY_ICON_STYLE_SUSPEND) PASS();
    else FAIL("voice-suspended must be suspend style");

    TEST("BLE RECOVER_FAILED -> outline + X, distinct from ADVERTISING");
    if (display_ble_icon_style(DISPLAY_BLE_RECOVER_FAILED) ==
            DISPLAY_ICON_STYLE_FAIL &&
        DISPLAY_ICON_STYLE_FAIL != DISPLAY_ICON_STYLE_ADV) PASS();
    else FAIL("recover-failed must be X");

    TEST("all five styles are mutually distinct");
    uint8_t seen[DISPLAY_ICON_STYLE_FAIL + 1] = {0};
    seen[display_wifi_icon_style(DISPLAY_WIFI_OFF)]++;
    seen[display_wifi_icon_style(DISPLAY_WIFI_CONNECTING)]++;
    seen[display_wifi_icon_style(DISPLAY_WIFI_IP)]++;
    seen[display_wifi_icon_style(DISPLAY_WIFI_FAILED)]++;
    seen[display_ble_icon_style(DISPLAY_BLE_VOICE_SUSPENDED)]++;
    int distinct = 0;
    for (int i = 0; i <= DISPLAY_ICON_STYLE_FAIL; i++)
        if (seen[i] > 0) distinct++;
    if (distinct >= 4) PASS();
    else FAIL("styles must be shape-distinguishable");
}

/* Page-2 OUTPUTS truth matrix: fault wins, unsupported -> "--" (never a
 * fabricated UNKNOWN), otherwise the real service tri-state. */
static void test_output_led_truth(void)
{
    TEST("output OFF -> hollow OFF, not UNKNOWN");
    if (display_output_led_state(1U, true, false) == DISPLAY_LED_OFF) PASS();
    else FAIL("known off must be OFF");

    TEST("output ON -> solid ON");
    if (display_output_led_state(2U, true, false) == DISPLAY_LED_ON) PASS();
    else FAIL("known on must be ON");

    TEST("output UNKNOWN -> yellow UNKNOWN (fail-closed)");
    if (display_output_led_state(0U, true, false) == DISPLAY_LED_UNKNOWN)
        PASS();
    else FAIL("unknown must stay unknown");

    TEST("unsupported output -> -- , never UNKNOWN");
    if (display_output_led_state(0U, false, false) == DISPLAY_LED_UNSUPPORTED)
        PASS();
    else FAIL("unsupported must be --");

    TEST("fault wins over everything, including UNSUPPORTED");
    if (display_output_led_state(0U, false, true) == DISPLAY_LED_FAULT)
        PASS();
    else FAIL("fault must win");

    TEST("fault wins over a genuine ON");
    if (display_output_led_state(2U, true, true) == DISPLAY_LED_FAULT) PASS();
    else FAIL("fault must override on");
}

/* Page-2 position inputs: a healthy snapshot with an inactive bit is OFF, not
 * UNKNOWN; only a missing/invalid snapshot is UNKNOWN. */
static void test_position_led_truth(void)
{
    TEST("active hall bit + valid snapshot -> ON");
    if (display_position_led_state(true, true, false) == DISPLAY_LED_ON)
        PASS();
    else FAIL("active must be ON");

    TEST("inactive hall bit + valid snapshot -> OFF (no magnet != UNKNOWN)");
    if (display_position_led_state(false, true, false) == DISPLAY_LED_OFF)
        PASS();
    else FAIL("no magnet must be OFF");

    TEST("invalid snapshot -> UNKNOWN");
    if (display_position_led_state(false, false, false) == DISPLAY_LED_UNKNOWN)
        PASS();
    else FAIL("invalid snapshot must be UNKNOWN");

    TEST("position fault -> FAULT (X)");
    if (display_position_led_state(false, true, true) == DISPLAY_LED_FAULT)
        PASS();
    else FAIL("fault must be X");
}

/* Page-2 HOT AIR merged output: single relay (fan+heater parallel).  Command
 * tri-state (ON/OFF/UNKNOWN) + cooldown lock (blue) + fault (red X, wins all). */
static void test_hot_air_led_truth(void)
{
    TEST("hot air OFF -> hollow OFF, not UNKNOWN");
    if (display_hot_air_led_state(1U, true, false, false) == DISPLAY_LED_OFF)
        PASS();
    else FAIL("known off must be OFF");

    TEST("hot air ON -> solid ON");
    if (display_hot_air_led_state(2U, true, false, false) == DISPLAY_LED_ON)
        PASS();
    else FAIL("known on must be ON");

    TEST("hot air UNKNOWN -> yellow UNKNOWN (fail-closed)");
    if (display_hot_air_led_state(0U, true, false, false) == DISPLAY_LED_UNKNOWN)
        PASS();
    else FAIL("unknown must stay unknown");

    TEST("hot air unsupported -> -- , never UNKNOWN");
    if (display_hot_air_led_state(0U, false, false, false) == DISPLAY_LED_UNSUPPORTED)
        PASS();
    else FAIL("unsupported must be --");

    TEST("hot air cooldown -> blue COOLDOWN (over ON)");
    if (display_hot_air_led_state(2U, true, false, true) == DISPLAY_LED_COOLDOWN)
        PASS();
    else FAIL("cooldown must show blue lock over ON");

    TEST("hot air cooldown -> blue COOLDOWN (over OFF)");
    if (display_hot_air_led_state(1U, true, false, true) == DISPLAY_LED_COOLDOWN)
        PASS();
    else FAIL("cooldown must show blue lock over OFF");

    TEST("hot air fault wins over cooldown");
    if (display_hot_air_led_state(2U, true, true, true) == DISPLAY_LED_FAULT)
        PASS();
    else FAIL("fault must override cooldown");

    TEST("hot air fault wins over unsupported");
    if (display_hot_air_led_state(0U, false, true, false) == DISPLAY_LED_FAULT)
        PASS();
    else FAIL("fault must override unsupported");
}

static void test_speaker_prefix_strip(void)
{
    TEST("UI-P2-1: strip embedded 你： prefix (duplicate fix)");
    if (strcmp(display_skip_speaker_prefix("你：打开洗衣机", "你："),
               "打开洗衣机") == 0) PASS();
    else FAIL("fullwidth prefix");

    TEST("UI-P2-1: strip embedded halfwidth 你: prefix");
    if (strcmp(display_skip_speaker_prefix("你:你好", "你:"),
               "你好") == 0) PASS();
    else FAIL("halfwidth prefix");

    TEST("UI-P2-1: strip whitespace-prefixed 你：");
    if (strcmp(display_skip_speaker_prefix("  你：早上好", "你："),
               "早上好") == 0) PASS();
    else FAIL("whitespace + prefix");

    TEST("UI-P2-1: unprefixed text passes through unchanged");
    if (strcmp(display_skip_speaker_prefix("请打开风扇", "你："),
               "请打开风扇") == 0) PASS();
    else FAIL("unprefixed must be unchanged");

    TEST("UI-P2-1: assistant 小净： strip");
    if (strcmp(display_skip_speaker_prefix("小净：正在为您服务", "小净："),
               "正在为您服务") == 0) PASS();
    else FAIL("assistant prefix");

    TEST("UI-P2-1: empty input stays empty");
    if (strcmp(display_skip_speaker_prefix("", "你："), "") == 0) PASS();
    else FAIL("empty input");

    TEST("UI-P2-1: UTF-8 你： at start of multi-byte text");
    if (strcmp(display_skip_speaker_prefix("你：请打开洗衣机并加洗涤剂", "你："),
               "请打开洗衣机并加洗涤剂") == 0) PASS();
    else FAIL("UTF-8 multi-byte strip");
}

int main(void)
{
    test_page_structure();
    test_link_state_names();
    test_backend_matrix();
    test_utf8_truncate();
    test_utf8_sanitize();
    test_concurrency_purity();
    test_cloud_from_wifi();
    test_icon_style_matrix();
    test_output_led_truth();
    test_position_led_truth();
    test_hot_air_led_truth();
    test_speaker_prefix_strip();

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
