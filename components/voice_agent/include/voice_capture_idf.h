#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "xiaojing_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VOICE_CAPTURE_DEFAULT_PCM_BYTES (320U * 1024U + 1024U)

typedef struct {
    size_t raw_pcm_bytes;
    size_t speech_pcm_bytes;
    uint16_t peak_amplitude;
    uint32_t rms_amplitude;
    bool speech_found;
    uint32_t completed_captures;
    uint32_t rejected_silence;
} voice_capture_idf_snapshot_t;

esp_err_t voice_capture_idf_init(
    const xiaojing_hal_t *hal, size_t max_pcm_bytes);
esp_err_t voice_capture_idf_deinit(void);

/* Signatures are compatible with voice_cloud_adapter_config_t. */
esp_err_t voice_capture_idf_start(void *context);
/* Import PCM captured by the shared wake/dialog audio controller.  The next
 * finish() leases it to the existing cloud worker exactly like a PTT capture. */
esp_err_t voice_capture_idf_import_pcm(
    const uint8_t *pcm, size_t pcm_bytes);
esp_err_t voice_capture_idf_finish(
    const uint8_t **wav, size_t *wav_length,
    uint32_t *token, void *context);
esp_err_t voice_capture_idf_cancel(void *context);
void voice_capture_idf_release(uint32_t token, void *context);
esp_err_t voice_capture_idf_get_snapshot(voice_capture_idf_snapshot_t *out);

#ifdef __cplusplus
}
#endif
