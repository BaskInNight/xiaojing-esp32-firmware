#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "voice_vad.h"

#define SAMPLE_RATE 16000U
#define SAMPLES (SAMPLE_RATE * 2U)

static int16_t s_pcm[SAMPLES];

static voice_vad_result_t analyze(void)
{
    voice_vad_config_t config = VOICE_VAD_DEFAULT_CONFIG();
    voice_vad_result_t result;
    assert(voice_vad_analyze(s_pcm, SAMPLES, &config, &result));
    return result;
}

static void test_silence(void)
{
    memset(s_pcm, 0, sizeof(s_pcm));
    voice_vad_result_t result = analyze();
    assert(!result.speech_found);
    assert(result.peak_amplitude == 0U);
    assert(result.rms_amplitude == 0U);
}

static void test_trimmed_speech(void)
{
    memset(s_pcm, 0, sizeof(s_pcm));
    for (size_t i = 8000U; i < 16000U; ++i)
        s_pcm[i] = (i & 1U) ? 4000 : -4000;
    voice_vad_result_t result = analyze();
    assert(result.speech_found);
    assert(result.start_sample <= 8000U);
    assert(result.start_sample >= 8000U - 2560U);
    assert(result.end_sample >= 16000U);
    assert(result.end_sample <= 16000U + 3840U);
    assert(result.peak_amplitude == 4000U);
}

static void test_short_noise_rejected(void)
{
    memset(s_pcm, 0, sizeof(s_pcm));
    for (size_t i = 8000U; i < 8320U; ++i) s_pcm[i] = 12000;
    voice_vad_result_t result = analyze();
    assert(!result.speech_found);
}

static void test_negative_full_scale(void)
{
    memset(s_pcm, 0, sizeof(s_pcm));
    for (size_t i = 6000U; i < 9000U; ++i) s_pcm[i] = INT16_MIN;
    voice_vad_result_t result = analyze();
    assert(result.speech_found);
    assert(result.peak_amplitude == 32768U);
}

int main(void)
{
    test_silence();
    test_trimmed_speech();
    test_short_noise_rejected();
    test_negative_full_scale();
    puts("voice_vad: 4/4 PASS");
    return 0;
}
