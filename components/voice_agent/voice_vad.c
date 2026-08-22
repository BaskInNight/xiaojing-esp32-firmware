#include "voice_vad.h"

#include <limits.h>
#include <string.h>

bool voice_vad_config_valid(const voice_vad_config_t *config)
{
    if (!config || config->sample_rate_hz < 8000U ||
        config->sample_rate_hz > 48000U || config->frame_ms < 5U ||
        config->frame_ms > 100U || config->amplitude_threshold == 0U ||
        config->min_active_frames == 0U)
        return false;
    uint64_t frame_samples =
        (uint64_t)config->sample_rate_hz * config->frame_ms / 1000U;
    return frame_samples > 0U && frame_samples <= SIZE_MAX;
}

static uint32_t isqrt_u64(uint64_t value)
{
    uint64_t result = 0;
    uint64_t bit = UINT64_C(1) << 62;
    while (bit > value) bit >>= 2;
    while (bit != 0U) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result > UINT32_MAX ? UINT32_MAX : (uint32_t)result;
}

bool voice_vad_analyze(const int16_t *samples, size_t sample_count,
                       const voice_vad_config_t *config,
                       voice_vad_result_t *out)
{
    if (!samples || !out || sample_count == 0U ||
        !voice_vad_config_valid(config))
        return false;

    voice_vad_result_t result;
    memset(&result, 0, sizeof(result));
    size_t frame_samples =
        (size_t)config->sample_rate_hz * config->frame_ms / 1000U;
    uint64_t total_square = 0U;
    size_t total_for_rms = 0U;
    size_t first_active = SIZE_MAX;
    size_t last_active = 0U;
    uint32_t consecutive = 0U;
    size_t run_start = 0U;

    for (size_t frame_start = 0U; frame_start < sample_count;
         frame_start += frame_samples) {
        size_t frame_end = frame_start + frame_samples;
        if (frame_end > sample_count) frame_end = sample_count;
        uint64_t frame_square = 0U;
        for (size_t i = frame_start; i < frame_end; ++i) {
            int32_t value = samples[i];
            uint32_t magnitude = value < 0 ? (uint32_t)(-value) :
                                              (uint32_t)value;
            if (magnitude > result.peak_amplitude)
                result.peak_amplitude = (uint16_t)magnitude;
            frame_square += (uint64_t)(value * value);
        }
        size_t count = frame_end - frame_start;
        total_square += frame_square;
        total_for_rms += count;
        uint32_t frame_rms = isqrt_u64(frame_square / count);
        if (frame_rms >= config->amplitude_threshold) {
            if (consecutive == 0U) run_start = frame_start;
            consecutive++;
            result.active_frames++;
            if (consecutive >= config->min_active_frames) {
                if (first_active == SIZE_MAX) first_active = run_start;
                last_active = frame_end;
            }
        } else {
            if (first_active != SIZE_MAX &&
                consecutive < config->min_active_frames) {
                result.active_frames -= consecutive;
            }
            consecutive = 0U;
        }
    }
    result.rms_amplitude = total_for_rms == 0U ? 0U :
        isqrt_u64(total_square / total_for_rms);

    if (first_active != SIZE_MAX) {
        size_t pre = (size_t)config->sample_rate_hz *
                     config->pre_roll_ms / 1000U;
        size_t post = (size_t)config->sample_rate_hz *
                      config->post_roll_ms / 1000U;
        result.start_sample = first_active > pre ? first_active - pre : 0U;
        result.end_sample = last_active > sample_count - post ?
                            sample_count : last_active + post;
        result.speech_found = result.end_sample > result.start_sample;
    }
    *out = result;
    return true;
}
