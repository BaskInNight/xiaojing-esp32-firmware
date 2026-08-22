#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *storage;
    size_t storage_bytes;
    const int16_t *pcm;
    size_t pcm_bytes;
    uint32_t sample_rate;
    uint32_t duration_ms;
} voice_tts_audio_t;

/* Synthesize one bounded UTF-8 reply through MiMo's direct API. The returned
 * PCM points into storage and remains valid until voice_tts_idf_release(). */
esp_err_t voice_tts_idf_synthesize(const char *endpoint,
                                   const char *api_key,
                                   const char *utf8_text,
                                   voice_tts_audio_t *out);
void voice_tts_idf_release(voice_tts_audio_t *audio);

#ifdef __cplusplus
}
#endif
