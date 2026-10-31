#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "wash_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VOICE_TOOL_JSON_MAX_BYTES 512

typedef enum {
    VOICE_TOOL_NONE = 0,
    VOICE_TOOL_START_PROGRAM,
    VOICE_TOOL_CANCEL,
    VOICE_TOOL_STATUS,
    VOICE_TOOL_ACK_LOAD,
    VOICE_TOOL_ACK_UNLOAD,
    VOICE_TOOL_SKIP_UV,
    VOICE_TOOL_ACK_FAULT,
} voice_tool_t;

typedef enum {
    VOICE_PARSE_OK = 0,
    VOICE_PARSE_BAD_ARG,
    VOICE_PARSE_TOO_LARGE,
    VOICE_PARSE_BAD_JSON,
    VOICE_PARSE_MISSING_FIELD,
    VOICE_PARSE_DUPLICATE_FIELD,
    VOICE_PARSE_UNKNOWN_FIELD,
    VOICE_PARSE_UNKNOWN_TOOL,
    VOICE_PARSE_FORBIDDEN_HARDWARE,
    VOICE_PARSE_INVALID_VALUE,
} voice_parse_result_t;

typedef struct {
    voice_tool_t tool;
    wash_program_kind_t program_kind;
    bool allow_uv;
    bool allow_dry;
} voice_tool_request_t;

voice_parse_result_t voice_tool_parse_json(
    const char *json,
    size_t length,
    voice_tool_request_t *out);

bool voice_tool_to_wash_intent(
    const voice_tool_request_t *request,
    wash_intent_t *out);

#ifdef __cplusplus
}
#endif
