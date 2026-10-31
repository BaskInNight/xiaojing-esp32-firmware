#include "voice_cloud.h"

#include <ctype.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_log.h"
#else
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#endif
#include "cJSON.h"

#include "voice_tool_parser.h"

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool bounded_string(const char *value, size_t max_length,
                           bool (*allowed)(unsigned char))
{
    if (!value) return false;
    size_t length = 0;
    while (value[length] != '\0') {
        unsigned char ch = (unsigned char)value[length];
        if (length >= max_length || !allowed(ch)) return false;
        length++;
    }
    return length > 0;
}

static bool endpoint_char_allowed(unsigned char ch)
{
    return ch >= 0x21 && ch <= 0x7e && ch != '#';
}

static bool key_char_allowed(unsigned char ch)
{
    return ch >= 0x21 && ch <= 0x7e && ch != '\r' && ch != '\n';
}

static bool model_char_allowed(unsigned char ch)
{
    return isalnum(ch) || ch == '-' || ch == '_' || ch == '.';
}

static bool endpoint_valid(const char *endpoint)
{
    static const char prefix[] = "https://";
    if (!bounded_string(endpoint, 255, endpoint_char_allowed) ||
        strncmp(endpoint, prefix, sizeof(prefix) - 1) != 0) {
        return false;
    }
    const char *authority = endpoint + sizeof(prefix) - 1;
    if (*authority == '\0' || *authority == '/' || *authority == ':')
        return false;
    const char *path = strchr(authority, '/');
    const char *authority_end = path ? path : endpoint + strlen(endpoint);
    for (const char *p = authority; p < authority_end; p++) {
        if (*p == '@') return false;
    }
    return true;
}

bool voice_cloud_config_valid(const voice_cloud_config_t *config)
{
    if (!config || !endpoint_valid(config->endpoint) ||
        !bounded_string(config->api_key, 256, key_char_allowed) ||
        !bounded_string(config->model, 64, model_char_allowed) ||
        !config->transport) {
        return false;
    }
    if (config->auth != VOICE_CLOUD_AUTH_API_KEY &&
        config->auth != VOICE_CLOUD_AUTH_BEARER) {
        return false;
    }
    if (config->connect_timeout_ms < 100 ||
        config->connect_timeout_ms > 120000 ||
        config->overall_timeout_ms < config->connect_timeout_ms ||
        config->overall_timeout_ms > 300000) {
        return false;
    }
    if (config->max_wav_bytes < 44 ||
        config->max_wav_bytes > VOICE_CLOUD_DEFAULT_MAX_WAV_BYTES ||
        config->max_response_bytes < 128 ||
        config->max_response_bytes > VOICE_CLOUD_DEFAULT_MAX_RESPONSE_BYTES) {
        return false;
    }
    return true;
}

bool voice_cloud_wav_valid(const uint8_t *wav, size_t length)
{
    if (!wav || length < 44 ||
        memcmp(wav, "RIFF", 4) != 0 ||
        memcmp(wav + 8, "WAVE", 4) != 0) {
        return false;
    }

    bool fmt_found = false;
    bool data_found = false;
    size_t offset = 12;
    while (offset + 8 <= length) {
        const uint8_t *chunk = wav + offset;
        uint32_t chunk_size = read_le32(chunk + 4);
        size_t data_offset = offset + 8;
        if (chunk_size > length - data_offset) return false;

        if (memcmp(chunk, "fmt ", 4) == 0) {
            if (chunk_size < 16) return false;
            const uint8_t *fmt = wav + data_offset;
            uint16_t format = read_le16(fmt);
            uint16_t channels = read_le16(fmt + 2);
            uint32_t sample_rate = read_le32(fmt + 4);
            uint32_t byte_rate = read_le32(fmt + 8);
            uint16_t block_align = read_le16(fmt + 12);
            uint16_t bits = read_le16(fmt + 14);
            if (format != 1 || channels != 1 || sample_rate != 16000 ||
                byte_rate != 32000 || block_align != 2 || bits != 16) {
                return false;
            }
            fmt_found = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            if (chunk_size == 0 || (chunk_size & 1U) != 0) return false;
            data_found = true;
        }

        size_t padded = (size_t)chunk_size + (chunk_size & 1U);
        if (padded > length - data_offset) return false;
        offset = data_offset + padded;
    }
    return fmt_found && data_found;
}

static int hex_digit(unsigned char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    ch = (unsigned char)tolower(ch);
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

static bool append_utf8(uint32_t cp, char *out, size_t capacity, size_t *used)
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
    if (*used + count >= capacity) return false;
    memcpy(out + *used, bytes, count);
    *used += count;
    return true;
}

static bool decode_u16(const char **cursor, const char *end, uint32_t *out)
{
    if ((size_t)(end - *cursor) < 4) return false;
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) {
        int h = hex_digit((unsigned char)(*cursor)[i]);
        if (h < 0) return false;
        value = (value << 4) | (uint32_t)h;
    }
    *cursor += 4;
    *out = value;
    return true;
}

static bool decode_json_string(const char **cursor, const char *end,
                               char *out, size_t capacity,
                               size_t *decoded_length)
{
    const char *p = *cursor;
    if (p >= end || *p++ != '"' || !out || capacity == 0) return false;
    size_t used = 0;
    while (p < end) {
        unsigned char ch = (unsigned char)*p++;
        if (ch == '"') {
            out[used] = '\0';
            *cursor = p;
            if (decoded_length) *decoded_length = used;
            return true;
        }
        if (ch < 0x20) return false;
        if (ch == '\\') {
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
                if (!decode_u16(&p, end, &cp)) return false;
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    if ((size_t)(end - p) < 6 ||
                        p[0] != '\\' || p[1] != 'u') return false;
                    p += 2;
                    uint32_t low = 0;
                    if (!decode_u16(&p, end, &low) ||
                        low < 0xdc00 || low > 0xdfff) return false;
                    cp = 0x10000 + ((cp - 0xd800) << 10) +
                         (low - 0xdc00);
                }
                if (!append_utf8(cp, out, capacity, &used)) return false;
                continue;
            }
            default:
                return false;
            }
        }
        if (used + 1 >= capacity) return false;
        out[used++] = (char)ch;
    }
    return false;
}

/* Advance over one valid JSON string without storing it.  The response
 * scanner must inspect object keys, but provider responses also contain long
 * IDs, reasoning text and transcripts.  Treating those values as 32-byte key
 * candidates made an otherwise valid response fail before the scanner ever
 * reached function.arguments. */
static bool skip_json_string(const char **cursor, const char *end)
{
    const char *p = *cursor;
    if (p >= end || *p++ != '"') return false;
    while (p < end) {
        unsigned char ch = (unsigned char)*p++;
        if (ch == '"') {
            *cursor = p;
            return true;
        }
        if (ch < 0x20) return false;
        if (ch != '\\') continue;
        if (p >= end) return false;
        ch = (unsigned char)*p++;
        if (ch == '"' || ch == '\\' || ch == '/' || ch == 'b' ||
            ch == 'f' || ch == 'n' || ch == 'r' || ch == 't') {
            continue;
        }
        if (ch != 'u') return false;
        uint32_t cp = 0;
        if (!decode_u16(&p, end, &cp)) return false;
        if (cp >= 0xd800 && cp <= 0xdbff) {
            if ((size_t)(end - p) < 6 || p[0] != '\\' || p[1] != 'u')
                return false;
            p += 2;
            uint32_t low = 0;
            if (!decode_u16(&p, end, &low) ||
                low < 0xdc00 || low > 0xdfff) return false;
        } else if (cp >= 0xdc00 && cp <= 0xdfff) {
            return false;
        }
    }
    return false;
}

static voice_cloud_result_t extract_arguments(
    const char *response, size_t response_length,
    char *out, size_t out_capacity, size_t *out_length,
    bool validate_tool)
{
    if (!response || response_length == 0 || !out ||
        out_capacity < 2 || !out_length) {
        return VOICE_CLOUD_BAD_ARG;
    }
    *out_length = 0;
    out[0] = '\0';

    if (!validate_tool) {
        cJSON *root = cJSON_ParseWithLength(response, response_length);
        const cJSON *choice = cJSON_GetArrayItem(
            cJSON_GetObjectItemCaseSensitive(root, "choices"), 0);
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(choice, "message");
        const cJSON *call = cJSON_GetArrayItem(
            cJSON_GetObjectItemCaseSensitive(message, "tool_calls"), 0);
        const cJSON *function = cJSON_GetObjectItemCaseSensitive(call, "function");
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(function, "name");
        const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(function, "arguments");
        const cJSON *finish = cJSON_GetObjectItemCaseSensitive(choice, "finish_reason");
        const char *reason = cJSON_IsString(finish) ? finish->valuestring : "missing";
        /* Whitelist metadata; never log provider-controlled content. */
        ESP_LOGI("voice_cloud", "response finish=%s tool_calls=%d",
                 strcmp(reason, "tool_calls") == 0 ? "tool_calls" :
                 strcmp(reason, "stop") == 0 ? "stop" :
                 strcmp(reason, "length") == 0 ? "length" : "other",
                 cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(message, "tool_calls")));
        bool valid = cJSON_IsString(name) &&
                     strcmp(name->valuestring, "generate_wash_pipeline") == 0 &&
                     cJSON_IsString(arguments);
        size_t length = valid ? strlen(arguments->valuestring) : 0;
        voice_cloud_result_t result = VOICE_CLOUD_BAD_RESPONSE;
        if (valid && length > 0 && length < out_capacity) {
            memcpy(out, arguments->valuestring, length + 1U);
            *out_length = length;
            result = VOICE_CLOUD_OK;
        } else if (valid && length >= out_capacity) {
            result = VOICE_CLOUD_TOOL_TOO_LARGE;
        }
        cJSON_Delete(root);
        return result;
    }

    voice_tool_request_t direct;
    if (validate_tool && response_length < out_capacity &&
        voice_tool_parse_json(response, response_length, &direct) ==
            VOICE_PARSE_OK) {
        memcpy(out, response, response_length);
        out[response_length] = '\0';
        *out_length = response_length;
        return VOICE_CLOUD_OK;
    }

    const char *p = response;
    const char *end = response + response_length;
    while (p < end) {
        if (*p != '"') {
            p++;
            continue;
        }
        char token[32];
        size_t token_length = 0;
        const char *string_start = p;
        const char *after = p;
        if (!decode_json_string(&after, end, token, sizeof(token),
                                &token_length)) {
            /* A valid but long JSON string cannot be an \"arguments\" key.
             * Skip it and continue scanning. Malformed strings still fail. */
            after = string_start;
            if (!skip_json_string(&after, end))
                return VOICE_CLOUD_BAD_RESPONSE;
            p = after;
            continue;
        }
        const char *colon = after;
        while (colon < end && isspace((unsigned char)*colon)) colon++;
        bool is_key = colon < end && *colon == ':';
        if (is_key && token_length == 9 &&
            memcmp(token, "arguments", 9) == 0) {
            const char *value = colon + 1;
            while (value < end && isspace((unsigned char)*value)) value++;
            if (value >= end || *value != '"')
                return VOICE_CLOUD_BAD_RESPONSE;
            size_t decoded = 0;
            if (!decode_json_string(&value, end, out, out_capacity,
                                    &decoded)) {
                return VOICE_CLOUD_TOOL_TOO_LARGE;
            }
            if (validate_tool) {
                voice_tool_request_t parsed;
                if (voice_tool_parse_json(out, decoded, &parsed) !=
                    VOICE_PARSE_OK) {
                    out[0] = '\0';
                    return VOICE_CLOUD_BAD_RESPONSE;
                }
            }
            *out_length = decoded;
            return VOICE_CLOUD_OK;
        }
        p = after;
    }
    return VOICE_CLOUD_BAD_RESPONSE;
}

/* MiMo can return a normal message instead of the requested tool call.  Do
 * not pass its JSON through as speech: only a string content or the
 * speech_reply member of structured content may become a local chat reply.
 * This route deliberately has no tool, so the existing local authorization
 * path cannot be bypassed by a provider fallback. */
static bool set_content_chat_route(const char *text, size_t length,
                                   voice_cloud_route_output_t *out)
{
    if (!text || !out || length == 0 ||
        length >= sizeof(out->route.reply_text)) return false;
    memset(&out->route, 0, sizeof(out->route));
    out->route.domain = VOICE_ROUTE_DOMAIN_CHAT;
    out->route.confidence_milli = 1000;
    out->route.tool.tool = VOICE_TOOL_NONE;
    memcpy(out->route.reply_text, text, length);
    out->route.reply_text[length] = '\0';
    return true;
}

static bool find_json_string_member(const char *json, size_t length,
                                    const char *member, char *out,
                                    size_t out_capacity, size_t *out_length)
{
    cJSON *root = cJSON_ParseWithLength(json, length);
    const cJSON *object = root;
    if (cJSON_IsArray(root) && cJSON_GetArraySize(root) == 1)
        object = cJSON_GetArrayItem(root, 0);
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, member);
    size_t used = cJSON_IsString(value) ? strlen(value->valuestring) : 0;
    bool valid = cJSON_IsString(value) && used < out_capacity;
    if (valid) {
        memcpy(out, value->valuestring, used + 1U);
        *out_length = used;
    }
    cJSON_Delete(root);
    return valid;
}

static void preserve_transcript(const char *json, size_t length,
                                voice_cloud_route_output_t *out)
{
    size_t transcript_length = 0;
    if (!find_json_string_member(json, length, "transcript",
                                 out->route.transcript,
                                 sizeof(out->route.transcript),
                                 &transcript_length)) {
        out->route.transcript[0] = '\0';
    }
}

static voice_cloud_result_t extract_content_route(const char *response,
                                                  size_t response_length,
                                                  voice_cloud_route_output_t *out)
{
    char content[VOICE_ROUTE_JSON_MAX_BYTES + 1U];
    size_t content_length = 0;
    cJSON *root = cJSON_ParseWithLength(response, response_length);
    const cJSON *choice = cJSON_GetArrayItem(
        cJSON_GetObjectItemCaseSensitive(root, "choices"), 0);
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(choice, "message");
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(message, "content");
    bool valid = false;
    if (cJSON_IsString(value)) {
        content_length = strlen(value->valuestring);
        valid = content_length < sizeof(content);
        if (valid) memcpy(content, value->valuestring, content_length + 1U);
    } else if (cJSON_IsObject(value) ||
               (cJSON_IsArray(value) && cJSON_GetArraySize(value) == 1)) {
        valid = cJSON_PrintPreallocated((cJSON *)value, content,
                                        sizeof(content), false);
        if (valid) content_length = strlen(content);
    }
    cJSON_Delete(root);
    if (!valid) return VOICE_CLOUD_BAD_RESPONSE;
    const char *p = content;
    size_t remaining = content_length;
    while (remaining > 0 && isspace((unsigned char)*p)) { p++; remaining--; }
    /* Some providers wrap a structured fallback in Markdown. Strip only a
     * complete JSON fence; never send its field names to speech synthesis. */
    if (remaining >= 6U && memcmp(p, "```", 3U) == 0) {
        p += 3U; remaining -= 3U;
        if (remaining >= 4U && memcmp(p, "json", 4U) == 0) {
            p += 4U; remaining -= 4U;
        }
        while (remaining && isspace((unsigned char)*p)) { p++; remaining--; }
        while (remaining && isspace((unsigned char)p[remaining - 1U])) remaining--;
        if (remaining < 3U || memcmp(p + remaining - 3U, "```", 3U) != 0)
            return VOICE_CLOUD_BAD_RESPONSE;
        remaining -= 3U;
        if (!remaining || (*p != '{' && *p != '[')) return VOICE_CLOUD_BAD_RESPONSE;
    }
    if (remaining > 0 && (*p == '{' || *p == '[')) {
        char speech[VOICE_ROUTE_REPLY_CAPACITY];
        size_t speech_length = 0;
        if (!find_json_string_member(p, remaining, "speech_reply", speech,
                                     sizeof(speech), &speech_length) ||
            !set_content_chat_route(speech, speech_length, out)) {
            return VOICE_CLOUD_BAD_RESPONSE;
        }
        preserve_transcript(p, remaining, out);
        return VOICE_CLOUD_OK;
    }
    return set_content_chat_route(p, remaining, out) ? VOICE_CLOUD_OK
                                                       : VOICE_CLOUD_BAD_RESPONSE;
}

voice_cloud_result_t voice_cloud_extract_tool_json(
    const char *response, size_t response_length,
    char *out, size_t out_capacity, size_t *out_length)
{
    return extract_arguments(response, response_length, out, out_capacity,
                             out_length, true);
}

voice_cloud_result_t voice_cloud_extract_arguments_json(
    const char *response, size_t response_length,
    char *out, size_t out_capacity, size_t *out_length)
{
    return extract_arguments(response, response_length, out, out_capacity,
                             out_length, false);
}

voice_cloud_result_t voice_cloud_run(
    const voice_cloud_config_t *config,
    const uint8_t *wav, size_t wav_length,
    char *response_scratch, size_t response_capacity,
    voice_cloud_output_t *out)
{
    if (!config || !wav || !response_scratch || !out)
        return VOICE_CLOUD_BAD_ARG;
    memset(out, 0, sizeof(*out));
    if (!voice_cloud_config_valid(config))
        return VOICE_CLOUD_BAD_CONFIG;
    if (wav_length > config->max_wav_bytes)
        return VOICE_CLOUD_WAV_TOO_LARGE;
    if (!voice_cloud_wav_valid(wav, wav_length))
        return VOICE_CLOUD_BAD_WAV;
    if (response_capacity < config->max_response_bytes)
        return VOICE_CLOUD_BAD_ARG;
    if (config->network_ready &&
        !config->network_ready(config->network_context))
        return VOICE_CLOUD_NETWORK_UNAVAILABLE;

    voice_cloud_transport_request_t request = {
        .endpoint = config->endpoint,
        .api_key = config->api_key,
        .model = config->model,
        .auth = config->auth,
        .kind = VOICE_CLOUD_REQUEST_TOOL_ONLY,
        .invocation_source = VOICE_INVOCATION_WAKE_WORD,
        .route_policy = VOICE_ROUTE_POLICY_AUTO,
        .wav = wav,
        .wav_length = wav_length,
        .connect_timeout_ms = config->connect_timeout_ms,
        .overall_timeout_ms = config->overall_timeout_ms,
        .max_response_bytes = config->max_response_bytes,
    };
    voice_cloud_transport_response_t response = {
        .body = response_scratch,
        .body_capacity = config->max_response_bytes,
    };
    voice_cloud_result_t result = config->transport(
        &request, &response, config->transport_context);
    out->http_status = response.http_status;
    if (result != VOICE_CLOUD_OK) return result;
    if (response.body_length >= response.body_capacity)
        return VOICE_CLOUD_RESPONSE_TOO_LARGE;
    response.body[response.body_length] = '\0';
    if (response.http_status < 200 || response.http_status >= 300)
        return VOICE_CLOUD_HTTP_ERROR;

    return voice_cloud_extract_tool_json(
        response.body, response.body_length,
        out->tool_json, sizeof(out->tool_json),
        &out->tool_json_length);
}

voice_cloud_result_t voice_cloud_run_auto_route(
    const voice_cloud_config_t *config,
    voice_invocation_source_t invocation_source,
    voice_route_policy_t route_policy,
    const uint8_t *wav, size_t wav_length,
    char *response_scratch, size_t response_capacity,
    voice_cloud_route_output_t *out)
{
    if (!config || !wav || !response_scratch || !out ||
        invocation_source < VOICE_INVOCATION_WAKE_WORD ||
        invocation_source >= VOICE_INVOCATION_COUNT ||
        route_policy < VOICE_ROUTE_POLICY_AUTO ||
        route_policy >= VOICE_ROUTE_POLICY_COUNT) {
        return VOICE_CLOUD_BAD_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (!voice_cloud_config_valid(config)) return VOICE_CLOUD_BAD_CONFIG;
    if (wav_length > config->max_wav_bytes)
        return VOICE_CLOUD_WAV_TOO_LARGE;
    if (!voice_cloud_wav_valid(wav, wav_length))
        return VOICE_CLOUD_BAD_WAV;
    if (response_capacity < config->max_response_bytes)
        return VOICE_CLOUD_BAD_ARG;
    if (config->network_ready &&
        !config->network_ready(config->network_context))
        return VOICE_CLOUD_NETWORK_UNAVAILABLE;

    voice_cloud_transport_request_t request = {
        .endpoint = config->endpoint,
        .api_key = config->api_key,
        .model = config->model,
        .auth = config->auth,
        .kind = VOICE_CLOUD_REQUEST_AUTO_ROUTE,
        .invocation_source = invocation_source,
        .route_policy = route_policy,
        .wav = wav,
        .wav_length = wav_length,
        .connect_timeout_ms = config->connect_timeout_ms,
        .overall_timeout_ms = config->overall_timeout_ms,
        .max_response_bytes = config->max_response_bytes,
    };
    voice_cloud_transport_response_t response = {
        .body = response_scratch,
        .body_capacity = config->max_response_bytes,
    };
    voice_cloud_result_t result = config->transport(
        &request, &response, config->transport_context);
    out->http_status = response.http_status;
    if (result != VOICE_CLOUD_OK) return result;
    if (response.body_length >= response.body_capacity)
        return VOICE_CLOUD_RESPONSE_TOO_LARGE;
    response.body[response.body_length] = '\0';
    if (response.http_status < 200 || response.http_status >= 300)
        return VOICE_CLOUD_HTTP_ERROR;

    char arguments[VOICE_ROUTE_JSON_MAX_BYTES + 1U];
    size_t arguments_length = 0;
    result = voice_cloud_extract_arguments_json(
        response.body, response.body_length, arguments, sizeof(arguments),
        &arguments_length);
    if (result != VOICE_CLOUD_OK) {
        ESP_LOGW("voice_cloud", "tool arguments unavailable: result=%d content_fallback", result);
        return extract_content_route(response.body, response.body_length, out);
    }
    voice_route_parse_result_t parse_result =
        voice_route_parse_json(arguments, arguments_length, &out->route);
    if (parse_result != VOICE_ROUTE_PARSE_OK) {
        char speech[VOICE_ROUTE_REPLY_CAPACITY];
        size_t speech_length = 0;
        ESP_LOGW("voice_cloud", "tool route rejected: parse=%d speech_fallback", parse_result);
        if (find_json_string_member(arguments, arguments_length,
                                    "speech_reply", speech, sizeof(speech),
                                    &speech_length) &&
            set_content_chat_route(speech, speech_length, out)) {
            preserve_transcript(arguments, arguments_length, out);
            ESP_LOGI("voice_cloud", "speech fallback accepted bytes=%u", (unsigned)speech_length);
            return VOICE_CLOUD_OK;
        }
        return VOICE_CLOUD_BAD_RESPONSE;
    }
    return VOICE_CLOUD_OK;
}
