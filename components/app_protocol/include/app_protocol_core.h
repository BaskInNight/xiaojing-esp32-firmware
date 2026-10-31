#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_PROTOCOL_VERSION 1U
#define APP_PROTOCOL_SEQ_CACHE_SIZE 8U
#define APP_PROTOCOL_CACHED_REPLY_MAX 256U
#define APP_PROTOCOL_TARGET_NAME_MAX 24U

/* Default for start_formal's allow_uv when a client omits the field.
 * Deliberately false: a missing field must never silently enable the UV
 * lamp.  The miniapp always sends allow_uv explicitly. */
#define APP_PROTOCOL_ALLOW_UV_DEFAULT false

typedef enum {
    APP_CMD_INVALID = 0,
    APP_CMD_HELLO,
    APP_CMD_GET_STATUS,
    APP_CMD_START_FORMAL,
    APP_CMD_START_DEMO,
    APP_CMD_SUBMIT_PLAN,
    APP_CMD_ACK_LOAD,
    APP_CMD_ACK_UNLOAD,
    APP_CMD_SKIP_UV,
    APP_CMD_ABORT_RESET,
    APP_CMD_ACK_FAULT,
    APP_CMD_PROVISION_WIFI,
    APP_CMD_PAUSE,
    APP_CMD_RESUME,
    APP_CMD_UV_SELF_TEST,
    APP_CMD_ACTUATOR_SELF_TEST,
    APP_CMD_MOTOR_BENCH_START,
    APP_CMD_MOTOR_BENCH_CONFIRM,
    APP_CMD_MOTOR_BENCH_CANCEL,
    APP_CMD_SET_DIRECTION_CALIBRATION,
    APP_CMD_POSITION_MOVE_TEST,
    APP_CMD_POSITION_SWING_TEST,
    APP_CMD_POSITION_SWING_CANCEL,
    APP_CMD_SET_DIAG_PREFS,
    APP_CMD_RESET_DIAG_PREFS,
    APP_CMD_LEGACY_START,
    APP_CMD_LEGACY_STOP,
    APP_CMD_FORBIDDEN_HARDWARE,
} app_command_t;

typedef enum {
    APP_PARSE_OK = 0,
    APP_PARSE_BAD_ARG,
    APP_PARSE_BAD_JSON,
    APP_PARSE_BAD_VERSION,
    APP_PARSE_MISSING_FIELD,
    APP_PARSE_UNKNOWN_CMD,
    APP_PARSE_FORBIDDEN_CMD,
} app_parse_result_t;

typedef struct {
    uint32_t version;
    uint32_t seq;
    app_command_t command;
    uint32_t fingerprint;
    bool legacy;
} app_protocol_message_t;

app_parse_result_t app_protocol_parse_frame(
    const char *frame, size_t frame_len, app_protocol_message_t *out);
uint32_t app_protocol_fingerprint(const char *frame, size_t frame_len);

typedef enum {
    APP_SEQ_MISS = 0,
    APP_SEQ_DUPLICATE,
    APP_SEQ_CONFLICT,
} app_seq_lookup_t;

typedef struct {
    bool valid;
    uint32_t version;
    uint32_t seq;
    app_command_t command;
    uint32_t fingerprint;
    uint16_t reply_len;
    char reply[APP_PROTOCOL_CACHED_REPLY_MAX];
} app_seq_cache_entry_t;

typedef struct {
    app_seq_cache_entry_t entries[APP_PROTOCOL_SEQ_CACHE_SIZE];
    uint8_t next_slot;
} app_seq_cache_t;

void app_seq_cache_init(app_seq_cache_t *cache);
app_seq_lookup_t app_seq_cache_lookup(
    const app_seq_cache_t *cache,
    const app_protocol_message_t *message,
    const char **out_reply,
    size_t *out_reply_len);
bool app_seq_cache_store(
    app_seq_cache_t *cache,
    const app_protocol_message_t *message,
    const char *reply,
    size_t reply_len);

/* Snapshot a JSON string value into a bounded buffer whose lifetime is
 * independent of the cJSON tree (cJSON valuestring dies with cJSON_Delete).
 * Returns false when empty or too long. */
bool app_protocol_snapshot_target(const char *target, char *buf, size_t cap);

/* Route-level top-level string-field extraction (pure scanner, host-testable).
 * Reads a named top-level JSON string field from the raw frame. Used by the
 * diagnostic routes so a sub-target key ("target") is never confused with the
 * parser's top-level "type":"cmd" discriminator (P0-2). Returns true and
 * NUL-terminates buf when the field exists, is a non-empty string and fits. */
bool app_protocol_field_string(const char *frame, size_t frame_len,
                               const char *key, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
