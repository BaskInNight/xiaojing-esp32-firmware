#pragma once

#include <stddef.h>
#include <stdint.h>

/* Mono PCM16 24 kHz -> 16 kHz, 24-tap low-pass polyphase FIR.
 * Output indices are absolute, so DMA chunk boundaries do not reset phase. */
size_t audio_pcm_24k_to_16k_count(size_t input_samples);
int16_t audio_pcm_24k_to_16k_sample(const int16_t *input,
                                   size_t input_samples, size_t output_index);
