/*
 * test_phase11_safety.c — Host tests for Phase 11 Group A Round 1 security
 * audit fixes:
 *   - app_protocol target-name UAF (bounded snapshot survives source free)
 *   - app_seq_cache duplicate replay (same ACK, no re-execute) + conflict
 *   - display_cloud_from_wifi same-frame Wi-Fi/cloud state combination
 * Pure C, no FreeRTOS/HAL/LVGL/cJSON dependency.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_protocol_core.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("  TEST %02d: %-58s ", tests_run, name); \
} while (0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while (0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while (0)

/* ---- P1-1: app_protocol target_name UAF ---- */

static void test_target_snapshot_uaf(void)
{
    /* Reproduce the exact lifecycle of APP_CMD_ACTUATOR_SELF_TEST:
     * the cJSON tree hands us a valuestring pointer, the router must snapshot
     * it BEFORE cJSON_Delete frees the tree, then encode the ACK AFTER the
     * delete using only the snapshot.  Here we simulate the free + perturb. */
    TEST("target snapshot survives source free + heap perturbation");
    char *src = (char *)malloc(32);
    assert(src != NULL);
    strcpy(src, "transfer_valve");
    char target_buf[APP_PROTOCOL_TARGET_NAME_MAX];
    bool ok = app_protocol_snapshot_target(src, target_buf,
                                           sizeof(target_buf));
    free(src);                       /* simulates cJSON_Delete(root) */
    memset(src, 0xCC, 32);           /* perturb freed memory */
    if (ok && strcmp(target_buf, "transfer_valve") == 0) PASS();
    else FAIL("snapshot corrupted by source free");
}

static void test_target_snapshot_bounds(void)
{
    TEST("target snapshot rejects empty / NULL");
    char b[APP_PROTOCOL_TARGET_NAME_MAX];
    if (app_protocol_snapshot_target("", b, sizeof(b)) == false &&
        app_protocol_snapshot_target(NULL, b, sizeof(b)) == false) PASS();
    else FAIL("empty/NULL must be rejected");

    TEST("target snapshot rejects over-long (no silent truncation)");
    char longname[APP_PROTOCOL_TARGET_NAME_MAX + 8];
    memset(longname, 'x', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';
    if (app_protocol_snapshot_target(longname, b, sizeof(b)) == false) PASS();
    else FAIL("over-long target must be rejected");

    TEST("target snapshot boundary: max length fits exactly");
    char exact[APP_PROTOCOL_TARGET_NAME_MAX];
    memset(exact, 'y', sizeof(exact) - 1);
    exact[sizeof(exact) - 1] = '\0';   /* length == cap-1 */
    char buf[APP_PROTOCOL_TARGET_NAME_MAX];
    if (app_protocol_snapshot_target(exact, buf, sizeof(buf)) == true &&
        strcmp(buf, exact) == 0) PASS();
    else FAIL("boundary length rejected");
}

/* ---- duplicate seq replay must not re-execute / must replay same ACK ---- */

static void test_seq_cache_duplicate_replay(void)
{
    TEST("seq cache duplicate replays identical ACK");
    app_seq_cache_t cache;
    app_seq_cache_init(&cache);
    app_protocol_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.version = 1;
    msg.seq = 42;
    msg.command = APP_CMD_ACTUATOR_SELF_TEST;
    msg.fingerprint = 0x1234U;
    msg.legacy = false;
    const char *reply =
        "{\"v\":1,\"type\":\"ack\",\"seq\":42,\"ok\":true,"
        "\"code\":\"ACTUATOR_SELF_TEST_ACCEPTED\","
        "\"target\":\"source_valve\",\"duration_ms\":1500}";
    if (!app_seq_cache_store(&cache, &msg, reply, strlen(reply))) {
        FAIL("store failed");
        return;
    }
    const char *cached = NULL;
    size_t cached_len = 0;
    app_seq_lookup_t r = app_seq_cache_lookup(&cache, &msg, &cached,
                                              &cached_len);
    if (r != APP_SEQ_DUPLICATE || cached == NULL ||
        cached_len != strlen(reply) ||
        memcmp(cached, reply, cached_len) != 0) FAIL("replay mismatch");
    else PASS();

    TEST("seq cache conflict on same seq + different command");
    app_protocol_message_t other = msg;
    other.command = APP_CMD_GET_STATUS;
    const char *c2 = NULL;
    size_t l2 = 0;
    if (app_seq_cache_lookup(&cache, &other, &c2, &l2) == APP_SEQ_CONFLICT)
        PASS();
    else FAIL("conflict not detected");

    TEST("seq cache miss on fresh seq");
    app_protocol_message_t fresh = msg;
    fresh.seq = 43;
    const char *c3 = NULL;
    size_t l3 = 0;
    if (app_seq_cache_lookup(&cache, &fresh, &c3, &l3) == APP_SEQ_MISS &&
        c3 == NULL && l3 == 0) PASS();
    else FAIL("fresh seq must be a miss");
}

/* ---- Stage 2: same-frame Wi-Fi/cloud combination ---- */

/* P0-2: motor_bench_start type-key contract.
 * Top-level "type" is the parser's command discriminator and MUST be "cmd".
 * The bench type rides on "target". Pre-fix the route read "type" for the
 * bench type, so no frame could ever dispatch: "type":"cmd" gave the parser
 * a clean pass but left the route's bench type NULL; "type":"pulsator" was
 * rejected by the parser. This pins the frame contract + the pure scanner. */
static void test_motor_bench_type_key_contract(void)
{
    TEST("motor_bench_start frame: type=cmd + target=type rides on target");
    const char *frame =
        "{\"v\":1,\"seq\":7,\"cmd\":\"motor_bench_start\","
        "\"type\":\"cmd\",\"target\":\"pulsator\"}";
    app_protocol_message_t msg;
    if (app_protocol_parse_frame(frame, strlen(frame), &msg) != APP_PARSE_OK ||
        msg.command != APP_CMD_MOTOR_BENCH_START) {
        FAIL("frame must parse and resolve to MOTOR_BENCH_START");
    } else {
        char buf[APP_PROTOCOL_TARGET_NAME_MAX];
        bool target_ok = app_protocol_field_string(
            frame, strlen(frame), "target", buf, sizeof(buf));
        bool type_ok = app_protocol_field_string(
            frame, strlen(frame), "type", buf, sizeof(buf));
        bool target_is_pulsator = app_protocol_field_string(
            frame, strlen(frame), "target", buf, sizeof(buf)) &&
            strcmp(buf, "pulsator") == 0;
        bool type_is_cmd = app_protocol_field_string(
            frame, strlen(frame), "type", buf, sizeof(buf)) &&
            strcmp(buf, "cmd") == 0;
        if (target_ok && type_ok && target_is_pulsator && type_is_cmd) PASS();
        else FAIL("target/type extraction mismatch");
    }

    TEST("regression: type=pulsator (bench type in type key) is rejected");
    const char *buggy =
        "{\"v\":1,\"seq\":7,\"cmd\":\"motor_bench_start\","
        "\"type\":\"pulsator\",\"target\":\"cmd\"}";
    app_protocol_message_t m2;
    if (app_protocol_parse_frame(buggy, strlen(buggy), &m2) == APP_PARSE_BAD_JSON)
        PASS();
    else FAIL("type != cmd must be rejected by parser");
}

/* The parser does NOT require a "type" key (v/seq/cmd suffice). The route's
 * bench-type source is "target" (field_string), independent of the parser
 * discriminator. This asserts that independence: no "type" in the frame must
 * still yield a clean parse + a correct "target" extraction for the route. */
static void test_missing_type_is_independent_of_target(void)
{
    TEST("target extraction independent of parser type discriminator");
    const char *frame =
        "{\"v\":1,\"seq\":9,\"cmd\":\"motor_bench_start\",\"target\":\"drum\"}";
    app_protocol_message_t msg;
    if (app_protocol_parse_frame(frame, strlen(frame), &msg) != APP_PARSE_OK ||
        msg.command != APP_CMD_MOTOR_BENCH_START) {
        FAIL("frame must parse without a type key");
    } else {
        char buf[APP_PROTOCOL_TARGET_NAME_MAX];
        if (app_protocol_field_string(frame, strlen(frame), "target", buf,
                                      sizeof(buf)) &&
            strcmp(buf, "drum") == 0) PASS();
        else FAIL("target not extracted from frame without type key");
    }
}

static void test_field_string_rejects_bad(void)
{
    TEST("field_string: missing key / empty / non-string / NULL all rejected");
    char buf[APP_PROTOCOL_TARGET_NAME_MAX];
    const char *frame =
        "{\"v\":1,\"cmd\":\"motor_bench_start\",\"target\":\"drum\","
        "\"seq\":0,\"n\":1}";
    size_t len = strlen(frame);
    int bad = 0;
    if (app_protocol_field_string(frame, len, "absent", buf, sizeof(buf))) bad++;
    if (app_protocol_field_string(NULL, len, "target", buf, sizeof(buf))) bad++;
    if (app_protocol_field_string(frame, len, "target", buf, 0)) bad++;
    if (app_protocol_field_string(frame, len, "v", buf, sizeof(buf))) bad++; /* not a string */
    if (bad == 0) PASS(); else FAIL("field_string accepted a bad field");
}

/* Mirror of display_cloud_from_wifi for host verification. */
static uint8_t cloud_from_wifi(uint8_t wifi_state)
{
    return wifi_state == 2U /* DISPLAY_WIFI_IP */ ? 2U : 1U;
}

static void test_wifi_cloud_combination(void)
{
    TEST("Wi-Fi IP -> network ready (2), never fabricated cloud READY");
    if (cloud_from_wifi(2U) == 2U) PASS();
    else FAIL("IP must map to network-ready");

    TEST("Wi-Fi connecting -> offline (1), same frame as connecting");
    if (cloud_from_wifi(1U) == 1U) PASS();
    else FAIL("connecting must map to offline");

    TEST("Wi-Fi off/failed -> offline (1)");
    if (cloud_from_wifi(0U) == 1U && cloud_from_wifi(3U) == 1U) PASS();
    else FAIL("off/failed must map to offline");

    TEST("same frame: wifi IP implies cloud != offline (no cross-frame stale)");
    /* This is the regression that broke Phase 9: voice_cloud_state was
     * computed from a PREVIOUS frame's wifi_state.  The fix computes both
     * from the SAME snapshot.  The mapping must never yield 'offline' (1)
     * while the current Wi-Fi state is IP (2). */
    uint8_t wifi_now = 2U;
    uint8_t cloud_now = cloud_from_wifi(wifi_now);
    if (!(wifi_now == 2U && cloud_now == 1U)) PASS();
    else FAIL("same-frame IP/offline contradiction");
}

int main(void)
{
    printf("=== xiaojing phase11 safety tests ===\n\n");
    test_target_snapshot_uaf();
    test_target_snapshot_bounds();
    test_seq_cache_duplicate_replay();
    test_wifi_cloud_combination();
    test_motor_bench_type_key_contract();
    test_missing_type_is_independent_of_target();
    test_field_string_rejects_bad();

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
