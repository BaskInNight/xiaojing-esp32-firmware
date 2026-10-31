#include "app_protocol_core.h"

#include <ctype.h>
#include <string.h>

uint32_t app_protocol_fingerprint(const char *frame, size_t frame_len)
{
    uint32_t hash = 2166136261U;
    if (!frame) return 0;
    for (size_t i = 0; i < frame_len; i++) {
        hash ^= (uint8_t)frame[i];
        hash *= 16777619U;
    }
    return hash;
}

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && isspace((unsigned char)*p)) p++;
    return p;
}

static bool parse_string(const char **cursor, const char *end,
                         char *out, size_t out_cap)
{
    const char *p = *cursor;
    if (p >= end || *p != '"' || out_cap == 0) return false;
    p++;
    size_t n = 0;
    bool escaped = false;
    while (p < end) {
        char ch = *p++;
        if (escaped) {
            if (ch != '"' && ch != '\\' && ch != '/' && ch != 'b' &&
                ch != 'f' && ch != 'n' && ch != 'r' && ch != 't') return false;
            if (n + 1 >= out_cap) return false;
            out[n++] = ch;
            escaped = false;
        } else if (ch == '\\') escaped = true;
        else if (ch == '"') {
            out[n] = '\0';
            *cursor = p;
            return true;
        } else {
            if ((unsigned char)ch < 0x20 || n + 1 >= out_cap) return false;
            out[n++] = ch;
        }
    }
    return false;
}

static bool parse_u32(const char **cursor, const char *end, uint32_t *out)
{
    const char *p = *cursor;
    if (p >= end || !isdigit((unsigned char)*p)) return false;
    uint32_t value = 0;
    do {
        uint32_t digit = (uint32_t)(*p - '0');
        if (value > (UINT32_MAX - digit) / 10U) return false;
        value = value * 10U + digit;
        p++;
    } while (p < end && isdigit((unsigned char)*p));
    *cursor = p;
    *out = value;
    return true;
}

static bool skip_value(const char **cursor, const char *end)
{
    const char *p = *cursor;
    if (p >= end) return false;
    if (*p == '"') {
        char discard[128];
        if (!parse_string(&p, end, discard, sizeof(discard))) return false;
    } else if (*p == '{' || *p == '[') {
        char open = *p++, close = open == '{' ? '}' : ']';
        unsigned depth = 1; bool in_string = false, escaped = false;
        while (p < end && depth) {
            char ch = *p++;
            if (in_string) {
                if (escaped) escaped = false;
                else if (ch == '\\') escaped = true;
                else if (ch == '"') in_string = false;
            } else if (ch == '"') in_string = true;
            else if (ch == open) depth++;
            else if (ch == close) depth--;
        }
        if (depth != 0) return false;
    } else {
        const char *start = p;
        while (p < end && *p != ',' && *p != '}') p++;
        if (p == start) return false;
    }
    *cursor = p;
    return true;
}

static app_command_t command_from_name(const char *cmd)
{
    static const struct { const char *name; app_command_t cmd; } map[] = {
        {"hello", APP_CMD_HELLO}, {"get_status", APP_CMD_GET_STATUS},
        {"start_formal", APP_CMD_START_FORMAL}, {"start_demo", APP_CMD_START_DEMO},
        {"submit_plan", APP_CMD_SUBMIT_PLAN}, {"ack_load", APP_CMD_ACK_LOAD},
        {"ack_unload", APP_CMD_ACK_UNLOAD}, {"skip_uv", APP_CMD_SKIP_UV},
        {"abort_reset", APP_CMD_ABORT_RESET}, {"pause", APP_CMD_PAUSE},
        {"ack_fault", APP_CMD_ACK_FAULT},
        {"provision_wifi", APP_CMD_PROVISION_WIFI},
        {"resume", APP_CMD_RESUME},
        {"uv_self_test", APP_CMD_UV_SELF_TEST},
        {"actuator_self_test", APP_CMD_ACTUATOR_SELF_TEST},
        {"motor_bench_start", APP_CMD_MOTOR_BENCH_START},
        {"motor_bench_confirm", APP_CMD_MOTOR_BENCH_CONFIRM},
        {"motor_bench_cancel", APP_CMD_MOTOR_BENCH_CANCEL},
        {"set_direction_calibration", APP_CMD_SET_DIRECTION_CALIBRATION},
        {"position_move_test", APP_CMD_POSITION_MOVE_TEST},
        {"position_swing_test", APP_CMD_POSITION_SWING_TEST},
        {"position_swing_cancel", APP_CMD_POSITION_SWING_CANCEL},
        {"set_diag_prefs", APP_CMD_SET_DIAG_PREFS},
        {"reset_diag_prefs", APP_CMD_RESET_DIAG_PREFS},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (strcmp(cmd, map[i].name) == 0) return map[i].cmd;
    static const char *forbidden[] = {
        "heater_on", "fan_on", "valve_on", "pump_on", "gpio", "mcp",
        "pwm", "relay", "motor_on", "ptc_on"
    };
    for (size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); i++)
        if (strcmp(cmd, forbidden[i]) == 0) return APP_CMD_FORBIDDEN_HARDWARE;
    return APP_CMD_INVALID;
}

app_parse_result_t app_protocol_parse_frame(
    const char *frame, size_t frame_len, app_protocol_message_t *out)
{
    if (!frame || frame_len == 0 || !out) return APP_PARSE_BAD_ARG;
    memset(out, 0, sizeof(*out));
    const char *begin = skip_ws(frame, frame + frame_len);
    const char *end = frame + frame_len;
    while (end > begin && isspace((unsigned char)end[-1])) end--;
    if ((size_t)(end - begin) == 5 && memcmp(begin, "start", 5) == 0) {
        out->legacy = true; out->command = APP_CMD_LEGACY_START;
        out->fingerprint = app_protocol_fingerprint(frame, frame_len); return APP_PARSE_OK;
    }
    if ((size_t)(end - begin) == 4 && memcmp(begin, "stop", 4) == 0) {
        out->legacy = true; out->command = APP_CMD_LEGACY_STOP;
        out->fingerprint = app_protocol_fingerprint(frame, frame_len); return APP_PARSE_OK;
    }
    if (begin >= end || *begin++ != '{') return APP_PARSE_BAD_JSON;

    bool have_v = false, have_seq = false, have_cmd = false;
    char cmd_name[40] = {0};
    char type_name[16] = {0}; bool have_type = false;
    const char *p = skip_ws(begin, end);
    while (p < end && *p != '}') {
        char key[24];
        if (!parse_string(&p, end, key, sizeof(key))) return APP_PARSE_BAD_JSON;
        p = skip_ws(p, end);
        if (p >= end || *p++ != ':') return APP_PARSE_BAD_JSON;
        p = skip_ws(p, end);
        if (strcmp(key, "v") == 0) {
            if (have_v) return APP_PARSE_BAD_JSON;
            if (!parse_u32(&p, end, &out->version)) return APP_PARSE_BAD_JSON;
            have_v = true;
        } else if (strcmp(key, "seq") == 0) {
            if (have_seq) return APP_PARSE_BAD_JSON;
            if (!parse_u32(&p, end, &out->seq)) return APP_PARSE_BAD_JSON;
            have_seq = true;
        } else if (strcmp(key, "cmd") == 0) {
            if (have_cmd) return APP_PARSE_BAD_JSON;
            if (!parse_string(&p, end, cmd_name, sizeof(cmd_name))) return APP_PARSE_BAD_JSON;
            have_cmd = true;
        } else if (strcmp(key, "type") == 0) {
            if (have_type) return APP_PARSE_BAD_JSON;
            if (!parse_string(&p, end, type_name, sizeof(type_name))) return APP_PARSE_BAD_JSON;
            have_type = true;
        } else if (!skip_value(&p, end)) return APP_PARSE_BAD_JSON;
        p = skip_ws(p, end);
        if (p < end && *p == ',') { p = skip_ws(p + 1, end); continue; }
        if (p >= end || *p != '}') return APP_PARSE_BAD_JSON;
    }
    if (p >= end || *p++ != '}' || skip_ws(p, end) != end) return APP_PARSE_BAD_JSON;
    if (!have_v || !have_seq || !have_cmd || out->seq == 0) {
        if (have_cmd && !have_v && !have_seq &&
            (strcmp(cmd_name, "start") == 0 || strcmp(cmd_name, "stop") == 0)) {
            out->legacy = true;
            out->command = strcmp(cmd_name, "start") == 0
                ? APP_CMD_LEGACY_START : APP_CMD_LEGACY_STOP;
            out->fingerprint = app_protocol_fingerprint(frame, frame_len);
            return APP_PARSE_OK;
        }
        return APP_PARSE_MISSING_FIELD;
    }
    if (out->version != APP_PROTOCOL_VERSION) return APP_PARSE_BAD_VERSION;
    if (have_type && strcmp(type_name, "cmd") != 0) return APP_PARSE_BAD_JSON;
    out->command = command_from_name(cmd_name);
    out->fingerprint = app_protocol_fingerprint(frame, frame_len);
    if (out->command == APP_CMD_FORBIDDEN_HARDWARE) return APP_PARSE_FORBIDDEN_CMD;
    if (out->command == APP_CMD_INVALID) return APP_PARSE_UNKNOWN_CMD;
    return APP_PARSE_OK;
}

/* Route-level top-level string-field extraction for diagnostic commands.
 * Top-level "type" is the parser's command discriminator and must be "cmd";
 * a diagnostic sub-target (motor_bench_start / actuator_self_test) rides on a
 * distinct key ("target"). This pure top-level scanner (no cJSON) lets the
 * route layer read "target" from the raw frame, pinning the contract in CI
 * (P0-2: reading "type" made motor_bench_start unreachable). Returns true and
 * NUL-terminates buf when the field exists, is a non-empty JSON string and
 * fits in cap. */
bool app_protocol_field_string(const char *frame, size_t frame_len,
                               const char *key, char *buf, size_t cap)
{
    if (!frame || frame_len == 0 || !key || !buf || cap == 0) return false;
    const char *begin = skip_ws(frame, frame + frame_len);
    const char *end = frame + frame_len;
    while (end > begin && isspace((unsigned char)end[-1])) end--;
    if (begin >= end || *begin != '{') return false;
    const char *p = skip_ws(begin + 1, end);
    while (p < end && *p != '}') {
        char k[24];
        if (!parse_string(&p, end, k, sizeof(k))) return false;
        p = skip_ws(p, end);
        if (p >= end || *p++ != ':') return false;
        p = skip_ws(p, end);
        if (strcmp(k, key) == 0) {
            if (!parse_string(&p, end, buf, cap)) return false;
            return buf[0] != '\0';
        }
        if (!skip_value(&p, end)) return false;
        p = skip_ws(p, end);
        if (p < end && *p == ',') { p = skip_ws(p + 1, end); continue; }
        if (p >= end || *p != '}') return false;
    }
    return false;
}

void app_seq_cache_init(app_seq_cache_t *cache)
{
    if (cache) memset(cache, 0, sizeof(*cache));
}

app_seq_lookup_t app_seq_cache_lookup(
    const app_seq_cache_t *cache, const app_protocol_message_t *message,
    const char **out_reply, size_t *out_reply_len)
{
    if (out_reply) *out_reply = NULL;
    if (out_reply_len) *out_reply_len = 0;
    if (!cache || !message || message->legacy) return APP_SEQ_MISS;
    for (size_t i = 0; i < APP_PROTOCOL_SEQ_CACHE_SIZE; i++) {
        const app_seq_cache_entry_t *e = &cache->entries[i];
        if (!e->valid || e->version != message->version || e->seq != message->seq) continue;
        if (e->command != message->command || e->fingerprint != message->fingerprint)
            return APP_SEQ_CONFLICT;
        if (out_reply) *out_reply = e->reply;
        if (out_reply_len) *out_reply_len = e->reply_len;
        return APP_SEQ_DUPLICATE;
    }
    return APP_SEQ_MISS;
}

bool app_seq_cache_store(
    app_seq_cache_t *cache, const app_protocol_message_t *message,
    const char *reply, size_t reply_len)
{
    if (!cache || !message || message->legacy || !reply ||
        reply_len > APP_PROTOCOL_CACHED_REPLY_MAX) return false;
    app_seq_cache_entry_t *e = &cache->entries[cache->next_slot];
    memset(e, 0, sizeof(*e));
    e->valid = true; e->version = message->version; e->seq = message->seq;
    e->command = message->command; e->fingerprint = message->fingerprint;
    e->reply_len = (uint16_t)reply_len;
    memcpy(e->reply, reply, reply_len);
    cache->next_slot = (uint8_t)((cache->next_slot + 1U) % APP_PROTOCOL_SEQ_CACHE_SIZE);
    return true;
}

/* Snapshot a JSON string value into a bounded buffer whose lifetime is
 * independent of the cJSON tree.  cJSON valuestring pointers die with
 * cJSON_Delete(), so the router must never keep them across the delete.
 * Returns false when the value is empty or does not fit (caller rejects). */
bool app_protocol_snapshot_target(const char *target, char *buf, size_t cap)
{
    if (!buf || cap == 0U) return false;
    if (!target || target[0] == '\0') return false;
    size_t n = strnlen(target, cap);
    if (n >= cap) return false;   /* 超长：拒绝，不静默截断 */
    memcpy(buf, target, n);
    buf[n] = '\0';
    return true;
}
