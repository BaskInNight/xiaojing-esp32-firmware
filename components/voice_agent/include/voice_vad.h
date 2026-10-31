#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t sample_rate_hz;
    uint16_t frame_ms;
    uint16_t amplitude_threshold;
    uint16_t min_active_frames;
    uint16_t pre_roll_ms;
    uint16_t post_roll_ms;
} voice_vad_config_t;

typedef struct {
    bool speech_found;
    size_t start_sample;
    size_t end_sample;
    uint16_t peak_amplitude;
    uint32_t rms_amplitude;
    uint32_t active_frames;
} voice_vad_result_t;

#define VOICE_VAD_DEFAULT_CONFIG() { \
    .sample_rate_hz = 16000U, \
    .frame_ms = 20U, \
    .amplitude_threshold = 600U, \
    .min_active_frames = 6U, \
    .pre_roll_ms = 160U, \
    .post_roll_ms = 240U, \
}

bool voice_vad_config_valid(const voice_vad_config_t *config);
bool voice_vad_analyze(const int16_t *samples, size_t sample_count,
                       const voice_vad_config_t *config,
                       voice_vad_result_t *out);

#ifdef __cplusplus
}
#endif
