#include "voice_tool_parser.h"

#include <ctype.h>
#include <string.h>

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && isspace((unsigned char)*p)) p++;
    return p;
}

static bool parse_string(const char **cursor, const char *end,
                         char *out, size_t capacity)
{
    const char *p = *cursor;
    if (p >= end || *p++ != '"' || capacity == 0) return false;
    size_t used = 0;
    while (p < end) {
        unsigned char ch = (unsigned char)*p++;
        if (ch == '"') {
            out[used] = '\0';
            *cursor = p;
            return true;
        }
        if (ch == '\\' || ch < 0x20 || used + 1 >= capacity) return false;
        out[used++] = (char)ch;
    }
    return false;
}

static bool parse_bool(const char **cursor, const char *end, bool *out)
{
    const char *p = *cursor;
    if ((size_t)(end - p) >= 4 && memcmp(p, "true", 4) == 0) {
        *out = true;
        *cursor = p + 4;
        return true;
    }
    if ((size_t)(end - p) >= 5 && memcmp(p, "false", 5) == 0) {
        *out = false;
        *cursor = p + 5;
        return true;
    }
    return false;
}

static bool forbidden_tool(const char *name)
{
    static const char *const names[] = {
        "heater_on", "fan_on", "valve_on", "pump_on", "gpio",
        "mcp", "pwm", "relay", "motor_on", "ptc_on",
        "move_position", "home_motor",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(name, names[i]) == 0) return true;
    }
    return false;
}

static voice_tool_t tool_from_name(const char *name)
{
    static const struct {
        const char *name;
        voice_tool_t tool;
    } map[] = {
        {"start_program", VOICE_TOOL_START_PROGRAM},
        {"cancel", VOICE_TOOL_CANCEL},
        {"status", VOICE_TOOL_STATUS},
        {"ack_load", VOICE_TOOL_ACK_LOAD},
        {"ack_unload", VOICE_TOOL_ACK_UNLOAD},
        {"skip_uv", VOICE_TOOL_SKIP_UV},
        {"ack_fault", VOICE_TOOL_ACK_FAULT},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(name, map[i].name) == 0) return map[i].tool;
    }
    return VOICE_TOOL_NONE;
}

voice_parse_result_t voice_tool_parse_json(const char *json, size_t length,
                                            voice_tool_request_t *out)
{
    if (!json || !out || length == 0) return VOICE_PARSE_BAD_ARG;
    memset(out, 0, sizeof(*out));
    if (length > VOICE_TOOL_JSON_MAX_BYTES) return VOICE_PARSE_TOO_LARGE;

    const char *end = json + length;
    const char *p = skip_ws(json, end);
    if (p >= end || *p++ != '{') return VOICE_PARSE_BAD_JSON;

    bool have_tool = false;
    bool have_program = false;
    bool have_uv = false;
    bool have_dry = false;
    char tool_name[32] = {0};
    char program_name[16] = {0};
    out->allow_uv = true;
    out->allow_dry = true;

    p = skip_ws(p, end);
    while (p < end && *p != '}') {
        char key[24];
        if (!parse_string(&p, end, key, sizeof(key)))
            return VOICE_PARSE_BAD_JSON;
        p = skip_ws(p, end);
        if (p >= end || *p++ != ':') return VOICE_PARSE_BAD_JSON;
        p = skip_ws(p, end);

        if (strcmp(key, "tool") == 0) {
            if (have_tool) return VOICE_PARSE_DUPLICATE_FIELD;
            if (!parse_string(&p, end, tool_name, sizeof(tool_name)))
                return VOICE_PARSE_INVALID_VALUE;
            have_tool = true;
        } else if (strcmp(key, "program") == 0) {
            if (have_program) return VOICE_PARSE_DUPLICATE_FIELD;
            if (!parse_string(&p, end, program_name, sizeof(program_name)))
                return VOICE_PARSE_INVALID_VALUE;
            have_program = true;
        } else if (strcmp(key, "allow_uv") == 0) {
            if (have_uv) return VOICE_PARSE_DUPLICATE_FIELD;
            if (!parse_bool(&p, end, &out->allow_uv))
                return VOICE_PARSE_INVALID_VALUE;
            have_uv = true;
        } else if (strcmp(key, "allow_dry") == 0) {
            if (have_dry) return VOICE_PARSE_DUPLICATE_FIELD;
            if (!parse_bool(&p, end, &out->allow_dry))
                return VOICE_PARSE_INVALID_VALUE;
            have_dry = true;
        } else {
            return VOICE_PARSE_UNKNOWN_FIELD;
        }

        p = skip_ws(p, end);
        if (p < end && *p == ',') {
            p = skip_ws(p + 1, end);
            if (p >= end || *p == '}') return VOICE_PARSE_BAD_JSON;
            continue;
        }
        if (p >= end || *p != '}') return VOICE_PARSE_BAD_JSON;
    }
    if (p >= end || *p++ != '}' || skip_ws(p, end) != end)
        return VOICE_PARSE_BAD_JSON;
    if (!have_tool) return VOICE_PARSE_MISSING_FIELD;
    if (forbidden_tool(tool_name)) return VOICE_PARSE_FORBIDDEN_HARDWARE;

    out->tool = tool_from_name(tool_name);
    if (out->tool == VOICE_TOOL_NONE) return VOICE_PARSE_UNKNOWN_TOOL;

    if (out->tool == VOICE_TOOL_START_PROGRAM) {
        if (!have_program) return VOICE_PARSE_MISSING_FIELD;
        if (strcmp(program_name, "formal") == 0) {
            out->program_kind = WASH_PROGRAM_FORMAL;
        } else if (strcmp(program_name, "demo") == 0) {
            out->program_kind = WASH_PROGRAM_DEMO;
        } else {
            return VOICE_PARSE_INVALID_VALUE;
        }
    } else if (have_program || have_uv || have_dry) {
        return VOICE_PARSE_INVALID_VALUE;
    }
    return VOICE_PARSE_OK;
}

bool voice_tool_to_wash_intent(const voice_tool_request_t *request,
                               wash_intent_t *out)
{
    if (!request || !out || request->tool != VOICE_TOOL_START_PROGRAM)
        return false;
    memset(out, 0, sizeof(*out));
    out->source = APP_SOURCE_VOICE;
    out->kind = request->program_kind;
    out->allow_uv = request->allow_uv;
    out->allow_dry = request->allow_dry;
    return true;
}
