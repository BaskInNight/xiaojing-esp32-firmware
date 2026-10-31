#include "ble_frame_assembler.h"

#include <ctype.h>
#include <string.h>

void ble_frame_assembler_init(ble_frame_assembler_t *assembler)
{
    if (assembler) memset(assembler, 0, sizeof(*assembler));
}

void ble_frame_assembler_reset(ble_frame_assembler_t *assembler)
{
    ble_frame_assembler_init(assembler);
}

static ble_frame_result_t fail_and_reset(
    ble_frame_assembler_t *assembler, ble_frame_result_t result)
{
    ble_frame_assembler_reset(assembler);
    return result;
}

static bool is_legacy_frame(const char *data, size_t len)
{
    while (len > 0 && isspace((unsigned char)*data)) { data++; len--; }
    while (len > 0 && isspace((unsigned char)data[len - 1])) len--;
    return (len == 5 && memcmp(data, "start", 5) == 0) ||
           (len == 4 && memcmp(data, "stop", 4) == 0);
}

ble_frame_result_t ble_frame_assembler_feed(
    ble_frame_assembler_t *assembler,
    const uint8_t *data,
    size_t data_len,
    uint32_t now_ms,
    const char **out_frame,
    size_t *out_len)
{
    if (!assembler || (!data && data_len > 0) || !out_frame || !out_len)
        return BLE_FRAME_ERR_INVALID_ARG;
    *out_frame = NULL;
    *out_len = 0;

    if (assembler->active &&
        (uint32_t)(now_ms - assembler->last_chunk_ms) > BLE_FRAME_TIMEOUT_MS) {
        ble_frame_assembler_reset(assembler);
        return BLE_FRAME_ERR_TIMEOUT;
    }
    if (data_len == 0) return BLE_FRAME_NEED_MORE;
    if (assembler->length + data_len > BLE_FRAME_MAX_BYTES)
        return fail_and_reset(assembler, BLE_FRAME_ERR_TOO_LARGE);

    if (!assembler->active && is_legacy_frame((const char *)data, data_len)) {
        memcpy(assembler->buffer, data, data_len);
        assembler->buffer[data_len] = '\0';
        assembler->length = data_len;
        *out_frame = assembler->buffer;
        *out_len = assembler->length;
        return BLE_FRAME_COMPLETE;
    }

    assembler->active = true;
    assembler->last_chunk_ms = now_ms;
    for (size_t i = 0; i < data_len; i++) {
        char ch = (char)data[i];
        assembler->buffer[assembler->length++] = ch;

        if (!assembler->root_started) {
            if (isspace((unsigned char)ch)) continue;
            if (ch != '{' && ch != '[')
                return fail_and_reset(assembler, BLE_FRAME_ERR_MALFORMED);
            assembler->root_started = true;
            assembler->root_char = ch;
            assembler->depth = 1;
            continue;
        }

        if (assembler->in_string) {
            if (assembler->escaped) assembler->escaped = false;
            else if (ch == '\\') assembler->escaped = true;
            else if (ch == '"') assembler->in_string = false;
            continue;
        }
        if (ch == '"') assembler->in_string = true;
        else if (ch == '{' || ch == '[') assembler->depth++;
        else if (ch == '}' || ch == ']') {
            char expected = assembler->root_char == '{' ? '}' : ']';
            if (assembler->depth == 0 || (assembler->depth == 1 && ch != expected))
                return fail_and_reset(assembler, BLE_FRAME_ERR_MALFORMED);
            assembler->depth--;
            if (assembler->depth == 0) {
                for (size_t j = i + 1; j < data_len; j++) {
                    if (!isspace((unsigned char)data[j]))
                        return fail_and_reset(assembler, BLE_FRAME_ERR_MALFORMED);
                }
                assembler->buffer[assembler->length] = '\0';
                *out_frame = assembler->buffer;
                *out_len = assembler->length;
                return BLE_FRAME_COMPLETE;
            }
        }
    }
    assembler->buffer[assembler->length] = '\0';
    return BLE_FRAME_NEED_MORE;
}
