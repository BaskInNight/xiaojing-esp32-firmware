#pragma once

#include <stdint.h>

#include "voice_service.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    voice_prompt_id_t id;
    const char *asset_key;
    uint16_t fallback_tone_hz;
    uint16_t fallback_duration_ms;
} voice_prompt_asset_t;

/* Symbolic asset keys only. Production WAV/Opus material can be supplied
 * later without changing voice state-machine code. */
const voice_prompt_asset_t *voice_prompt_catalog_get(
    voice_prompt_id_t id);

#ifdef __cplusplus
}
#endif
