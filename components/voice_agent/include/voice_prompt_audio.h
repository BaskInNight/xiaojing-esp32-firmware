#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "voice_service.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const int16_t *pcm;
    size_t pcm_bytes;
    uint32_t sample_rate;
    uint32_t duration_ms;
} voice_prompt_pcm_t;

/* Returns a validated view into a read-only embedded WAV asset. */
bool voice_prompt_audio_get(voice_prompt_id_t id, voice_prompt_pcm_t *out);

/* Special accessor for task completion prompt (not mapped to VOICE_PROMPT_*). */
bool voice_prompt_audio_get_task_done(voice_prompt_pcm_t *out);

#ifdef __cplusplus
}
#endif
