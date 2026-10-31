#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_FRAME_MAX_BYTES 2048U
#define BLE_FRAME_TIMEOUT_MS 3000U

typedef enum {
    BLE_FRAME_NEED_MORE = 0,
    BLE_FRAME_COMPLETE,
    BLE_FRAME_ERR_INVALID_ARG,
    BLE_FRAME_ERR_TOO_LARGE,
    BLE_FRAME_ERR_TIMEOUT,
    BLE_FRAME_ERR_MALFORMED,
} ble_frame_result_t;

typedef struct {
    char buffer[BLE_FRAME_MAX_BYTES + 1U];
    size_t length;
    uint32_t last_chunk_ms;
    uint16_t depth;
    bool active;
    bool root_started;
    bool in_string;
    bool escaped;
    char root_char;
} ble_frame_assembler_t;

void ble_frame_assembler_init(ble_frame_assembler_t *assembler);
void ble_frame_assembler_reset(ble_frame_assembler_t *assembler);
ble_frame_result_t ble_frame_assembler_feed(
    ble_frame_assembler_t *assembler,
    const uint8_t *data,
    size_t data_len,
    uint32_t now_ms,
    const char **out_frame,
    size_t *out_len);

#ifdef __cplusplus
}
#endif
