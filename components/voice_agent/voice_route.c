#include "voice_route.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && isspace((unsigned char)*p)) p++;
    return p;
}

static int hex_value(unsigned char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    ch = (unsigned char)tolower(ch);
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

static bool append_utf8(uint32_t cp, char *out, size_t cap, size_t *used)
{
    unsigned char bytes[4];
    size_t count = 0;
    if (cp <= 0x7f) {
        bytes[0] = (unsigned char)cp; count = 1;
    } else if (cp <= 0x7ff) {
        bytes[0] = (unsigned char)(0xc0 | (cp >> 6));
        bytes[1] = (unsigned char)(0x80 | (cp & 0x3f)); count = 2;
    } else if (cp >= 0xd800 && cp <= 0xdfff) {
        return false;
    } else if (cp <= 0xffff) {
        bytes[0] = (unsigned char)(0xe0 | (cp >> 12));
        bytes[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
        bytes[2] = (unsigned char)(0x80 | (cp & 0x3f)); count = 3;
    } else if (cp <= 0x10ffff) {
        bytes[0] = (unsigned char)(0xf0 | (cp >> 18));
        bytes[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3f));
        bytes[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
        bytes[3] = (unsigned char)(0x80 | (cp & 0x3f)); count = 4;
    } else {
        return false;
    }
    if (*used + count >= cap) return false;
    memcpy(out + *used, bytes, count);
    *used += count;
    return true;
}

static bool parse_u16_escape(const char **cursor, const char *end,
                             uint32_t *out)
{
    if ((size_t)(end - *cursor) < 4) return false;
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) {
        int h = hex_value((unsigned char)(*cursor)[i]);
        if (h < 0) return false;
        value = (value << 4) | (uint32_t)h;
    }
    *cursor += 4;
    *out = value;
    return true;
}

static bool parse_string(const char **cursor, const char *end,
                         char *out, size_t capacity)
{
    const char *p = *cursor;
    if (!out || capacity == 0 || p >= end || *p++ != '"') return false;
    size_t used = 0;
    while (p < end) {
        unsigned char ch = (unsigned char)*p++;
        if (ch == '"') {
            out[used] = '\0';
            *cursor = p;
            return true;
        }
        if (ch < 0x20) return false;
        if (ch != '\\') {
            if (used + 1 >= capacity) return false;
            out[used++] = (char)ch;
            continue;
        }
        if (p >= end) return false;
        ch = (unsigned char)*p++;
        switch (ch) {
        case '"': case '\\': case '/': break;
        case 'b': ch = '\b'; break;
        case 'f': ch = '\f'; break;
        case 'n': ch = '\n'; break;
        case 'r': ch = '\r'; break;
        case 't': ch = '\t'; break;
        case 'u': {
            uint32_t cp = 0;
            if (!parse_u16_escape(&p, end, &cp)) return false;
            if (cp >= 0xd800 && cp <= 0xdbff) {
                if ((size_t)(end - p) < 6 || p[0] != '\\' || p[1] != 'u')
                    return false;
                p += 2;
                uint32_t low = 0;
                if (!parse_u16_escape(&p, end, &low) ||
                    low < 0xdc00 || low > 0xdfff) return false;
                cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
            }
            if (!append_utf8(cp, out, capacity, &used)) return false;
            continue;
        }
        default:
            return false;
        }
        if (used + 1 >= capacity) return false;
        out[used++] = (char)ch;
    }
    return false;
}

static bool parse_bool(const char **cursor, const char *end, bool *out)
{
    if ((size_t)(end - *cursor) >= 4 &&
        memcmp(*cursor, "true", 4) == 0) {
        *out = true; *cursor += 4; return true;
    }
    if ((size_t)(end - *cursor) >= 5 &&
        memcmp(*cursor, "false", 5) == 0) {
        *out = false; *cursor += 5; return true;
    }
    return false;
}

static bool parse_u16(const char **cursor, const char *end, uint16_t *out)
{
    const char *p = *cursor;
    if (p >= end || !isdigit((unsigned char)*p)) return false;
    uint32_t value = 0;
    do {
        value = value * 10U + (uint32_t)(*p - '0');
        if (value > 1000U) return false;
        p++;
    } while (p < end && isdigit((unsigned char)*p));
    *out = (uint16_t)value;
    *cursor = p;
    return true;
}

static bool domain_from_name(const char *name, voice_route_domain_t *out)
{
    static const struct { const char *name; voice_route_domain_t value; } map[] = {
        {"control", VOICE_ROUTE_DOMAIN_CONTROL},
        {"status", VOICE_ROUTE_DOMAIN_STATUS},
        {"chat", VOICE_ROUTE_DOMAIN_CHAT},
        {"clarify", VOICE_ROUTE_DOMAIN_CLARIFY},
        {"reject", VOICE_ROUTE_DOMAIN_REJECT},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(name, map[i].name) == 0) {
            *out = map[i].value; return true;
        }
    }
    return false;
}

static voice_tool_t tool_from_name(const char *name)
{
    static const struct { const char *name; voice_tool_t value; } map[] = {
        {"none", VOICE_TOOL_NONE},
        {"start_program", VOICE_TOOL_START_PROGRAM},
        {"cancel", VOICE_TOOL_CANCEL},
        {"status", VOICE_TOOL_STATUS},
        {"ack_load", VOICE_TOOL_ACK_LOAD},
        {"ack_unload", VOICE_TOOL_ACK_UNLOAD},
        {"skip_uv", VOICE_TOOL_SKIP_UV},
        {"ack_fault", VOICE_TOOL_ACK_FAULT},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (strcmp(name, map[i].name) == 0) return map[i].value;
    return (voice_tool_t)-1;
}

voice_route_parse_result_t voice_route_parse_json(
    const char *json, size_t length, voice_route_result_t *out)
{
    if (!json || !out || length == 0) return VOICE_ROUTE_PARSE_BAD_ARG;
    if (length > VOICE_ROUTE_JSON_MAX_BYTES) return VOICE_ROUTE_PARSE_TOO_LARGE;

    voice_route_result_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.tool.allow_uv = true;
    tmp.tool.allow_dry = true;

    bool have_domain = false, have_confidence = false, have_reply = false;
    bool have_confirm = false, have_tool = false, have_program = false;
    bool have_uv = false, have_dry = false, have_transcript = false;
    char domain[16] = {0}, tool[24] = {0}, program[16] = {0};

    const char *end = json + length;
    const char *p = skip_ws(json, end);
    if (p >= end || *p++ != '{') return VOICE_ROUTE_PARSE_BAD_JSON;
    p = skip_ws(p, end);
    while (p < end && *p != '}') {
        char key[32];
        if (!parse_string(&p, end, key, sizeof(key)))
            return VOICE_ROUTE_PARSE_BAD_JSON;
        p = skip_ws(p, end);
        if (p >= end || *p++ != ':') return VOICE_ROUTE_PARSE_BAD_JSON;
        p = skip_ws(p, end);

#define ROUTE_STRING_FIELD(key_, seen_, dst_) \
        if (strcmp(key, key_) == 0) { \
            if (seen_) return VOICE_ROUTE_PARSE_DUPLICATE_FIELD; \
            if (!parse_string(&p, end, dst_, sizeof(dst_))) \
                return VOICE_ROUTE_PARSE_INVALID_VALUE; \
            seen_ = true; \
        }
        ROUTE_STRING_FIELD("domain", have_domain, domain)
        else ROUTE_STRING_FIELD("transcript", have_transcript, tmp.transcript)
        else ROUTE_STRING_FIELD("reply_text", have_reply, tmp.reply_text)
        else ROUTE_STRING_FIELD("tool", have_tool, tool)
        else ROUTE_STRING_FIELD("program", have_program, program)
        else if (strcmp(key, "confidence_milli") == 0) {
            if (have_confidence) return VOICE_ROUTE_PARSE_DUPLICATE_FIELD;
            if (!parse_u16(&p, end, &tmp.confidence_milli))
                return VOICE_ROUTE_PARSE_INVALID_VALUE;
            have_confidence = true;
        } else if (strcmp(key, "requires_confirmation") == 0) {
            if (have_confirm) return VOICE_ROUTE_PARSE_DUPLICATE_FIELD;
            if (!parse_bool(&p, end, &tmp.requires_confirmation))
                return VOICE_ROUTE_PARSE_INVALID_VALUE;
            have_confirm = true;
        } else if (strcmp(key, "allow_uv") == 0) {
            if (have_uv) return VOICE_ROUTE_PARSE_DUPLICATE_FIELD;
            if (!parse_bool(&p, end, &tmp.tool.allow_uv))
                return VOICE_ROUTE_PARSE_INVALID_VALUE;
            have_uv = true;
        } else if (strcmp(key, "allow_dry") == 0) {
            if (have_dry) return VOICE_ROUTE_PARSE_DUPLICATE_FIELD;
            if (!parse_bool(&p, end, &tmp.tool.allow_dry))
                return VOICE_ROUTE_PARSE_INVALID_VALUE;
            have_dry = true;
        } else {
            return VOICE_ROUTE_PARSE_UNKNOWN_FIELD;
        }
#undef ROUTE_STRING_FIELD

        p = skip_ws(p, end);
        if (p < end && *p == ',') {
            p = skip_ws(p + 1, end);
            if (p >= end || *p == '}') return VOICE_ROUTE_PARSE_BAD_JSON;
        } else if (p >= end || *p != '}') {
            return VOICE_ROUTE_PARSE_BAD_JSON;
        }
    }
    if (p >= end || *p++ != '}' || skip_ws(p, end) != end)
        return VOICE_ROUTE_PARSE_BAD_JSON;
    if (!have_domain || !have_transcript || tmp.transcript[0] == '\0' ||
        !have_confidence || !have_reply ||
        !have_confirm || !have_tool)
        return VOICE_ROUTE_PARSE_MISSING_FIELD;
    if (!domain_from_name(domain, &tmp.domain))
        return VOICE_ROUTE_PARSE_INVALID_VALUE;

    voice_tool_t parsed_tool = tool_from_name(tool);
    if ((int)parsed_tool < 0) return VOICE_ROUTE_PARSE_INVALID_VALUE;
    tmp.tool.tool = parsed_tool;
    tmp.has_tool = parsed_tool != VOICE_TOOL_NONE;

    if (parsed_tool == VOICE_TOOL_START_PROGRAM) {
        if (!have_program) return VOICE_ROUTE_PARSE_MISSING_FIELD;
        if (strcmp(program, "formal") == 0)
            tmp.tool.program_kind = WASH_PROGRAM_FORMAL;
        else if (strcmp(program, "demo") == 0)
            tmp.tool.program_kind = WASH_PROGRAM_DEMO;
        else
            return VOICE_ROUTE_PARSE_INVALID_VALUE;
    } else if (have_program || have_uv || have_dry) {
        return VOICE_ROUTE_PARSE_INCONSISTENT_TOOL;
    }

    if ((tmp.domain == VOICE_ROUTE_DOMAIN_CONTROL &&
         (!tmp.has_tool || parsed_tool == VOICE_TOOL_STATUS)) ||
        (tmp.domain == VOICE_ROUTE_DOMAIN_STATUS &&
         parsed_tool != VOICE_TOOL_STATUS) ||
        ((tmp.domain == VOICE_ROUTE_DOMAIN_CHAT ||
          tmp.domain == VOICE_ROUTE_DOMAIN_CLARIFY ||
          tmp.domain == VOICE_ROUTE_DOMAIN_REJECT) && tmp.has_tool)) {
        return VOICE_ROUTE_PARSE_INCONSISTENT_TOOL;
    }

    *out = tmp;
    return VOICE_ROUTE_PARSE_OK;
}

voice_route_decision_t voice_route_authorize(
    const voice_route_result_t *route,
    voice_route_policy_t policy,
    uint16_t min_control_confidence_milli)
{
    if (!route || policy < VOICE_ROUTE_POLICY_AUTO ||
        policy >= VOICE_ROUTE_POLICY_COUNT ||
        min_control_confidence_milli > 1000U) {
        return VOICE_ROUTE_DECISION_REJECT;
    }

    if (route->domain == VOICE_ROUTE_DOMAIN_REJECT)
        return VOICE_ROUTE_DECISION_REJECT;
    if (route->domain == VOICE_ROUTE_DOMAIN_CHAT ||
        route->domain == VOICE_ROUTE_DOMAIN_CLARIFY)
        return VOICE_ROUTE_DECISION_RESPOND;
    if (!route->has_tool ||
        !voice_policy_tool_allowed(policy, route->tool.tool))
        return policy == VOICE_ROUTE_POLICY_CHAT_PRIORITY
            ? VOICE_ROUTE_DECISION_REQUIRE_CONFIRMATION
            : VOICE_ROUTE_DECISION_REJECT;
    if (route->domain == VOICE_ROUTE_DOMAIN_STATUS)
        return VOICE_ROUTE_DECISION_EXECUTE_TOOL;
    if (route->requires_confirmation ||
        route->confidence_milli < min_control_confidence_milli)
        return VOICE_ROUTE_DECISION_REQUIRE_CONFIRMATION;
    return VOICE_ROUTE_DECISION_EXECUTE_TOOL;
}
