#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "voice_profile.h"
#include "voice_tool_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VOICE_ROUTE_JSON_MAX_BYTES       4096U
/* JSON Schema maxLength counts Unicode characters, while the C parser stores
 * UTF-8 bytes.  Reserve four bytes per permitted character plus NUL so a
 * long Chinese transcript is not rejected merely because it is multibyte. */
#define VOICE_ROUTE_TRANSCRIPT_CAPACITY  1025U
#define VOICE_ROUTE_REPLY_CAPACITY        513U

typedef enum {
    VOICE_ROUTE_DOMAIN_CONTROL = 0,
    VOICE_ROUTE_DOMAIN_STATUS,
    VOICE_ROUTE_DOMAIN_CHAT,
    VOICE_ROUTE_DOMAIN_CLARIFY,
    VOICE_ROUTE_DOMAIN_REJECT,
    VOICE_ROUTE_DOMAIN_COUNT,
} voice_route_domain_t;

typedef enum {
    VOICE_ROUTE_PARSE_OK = 0,
    VOICE_ROUTE_PARSE_BAD_ARG,
    VOICE_ROUTE_PARSE_TOO_LARGE,
    VOICE_ROUTE_PARSE_BAD_JSON,
    VOICE_ROUTE_PARSE_MISSING_FIELD,
    VOICE_ROUTE_PARSE_DUPLICATE_FIELD,
    VOICE_ROUTE_PARSE_UNKNOWN_FIELD,
    VOICE_ROUTE_PARSE_INVALID_VALUE,
    VOICE_ROUTE_PARSE_INCONSISTENT_TOOL,
} voice_route_parse_result_t;

typedef struct {
    voice_route_domain_t domain;
    uint16_t confidence_milli;
    bool requires_confirmation;
    bool has_tool;
    voice_tool_request_t tool;
    char transcript[VOICE_ROUTE_TRANSCRIPT_CAPACITY];
    char reply_text[VOICE_ROUTE_REPLY_CAPACITY];
} voice_route_result_t;

typedef enum {
    VOICE_ROUTE_DECISION_RESPOND = 0,
    VOICE_ROUTE_DECISION_EXECUTE_TOOL,
    VOICE_ROUTE_DECISION_REQUIRE_CONFIRMATION,
    VOICE_ROUTE_DECISION_REJECT,
} voice_route_decision_t;

voice_route_parse_result_t voice_route_parse_json(
    const char *json,
    size_t length,
    voice_route_result_t *out);

/*
 * Authorization is local and independent of the model. AUTO control requires
 * confidence >= min_control_confidence_milli and no confirmation flag.
 * CHAT_PRIORITY never executes a mutating tool.
 */
voice_route_decision_t voice_route_authorize(
    const voice_route_result_t *route,
    voice_route_policy_t policy,
    uint16_t min_control_confidence_milli);

#ifdef __cplusplus
}
#endif
