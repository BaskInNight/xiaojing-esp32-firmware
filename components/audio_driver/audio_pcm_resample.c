#include "audio_pcm_resample.h"

/* Q15 windowed-sinc coefficients: cutoff=7.2 kHz at 24 kHz input,
 * Hann window, unit DC gain. Two fractional phases implement ratio 2/3. */
static const int16_t taps[2][24] = {
    {15,0,-161,192,325,-827,0,1860,-1745,-2861,9753,19666,
     9753,-2861,-1745,1860,0,-827,325,192,-161,0,15,0},
    {1,31,-92,-74,429,-216,-867,1297,741,-3742,2068,16807,
     16809,2068,-3742,741,1297,-867,-216,429,-74,-92,31,1}
};

size_t audio_pcm_24k_to_16k_count(size_t count)
{
    return (count / 3U) * 2U + ((count % 3U) * 2U) / 3U;
}

int16_t audio_pcm_24k_to_16k_sample(const int16_t *input,
                                   size_t count, size_t output_index)
{
    if (!input || count == 0 ||
        output_index >= audio_pcm_24k_to_16k_count(count)) return 0;
    size_t center = (output_index / 2U) * 3U + (output_index & 1U);
    const int16_t *coeff = taps[output_index & 1U];
    int64_t sum = 0;
    for (size_t k = 0; k < 24U; k++) {
        /* Clamp the short filter boundary to the first/last input sample. */
        size_t index;
        if (k < 11U && center < 11U - k) index = 0;
        else {
            index = k < 11U ? center - (11U - k) : center + k - 11U;
            if (index >= count) index = count - 1U;
        }
        sum += (int64_t)input[index] * coeff[k];
    }
    sum = (sum >= 0 ? sum + 16384 : sum - 16384) / 32768;
    if (sum > INT16_MAX) sum = INT16_MAX;
    if (sum < INT16_MIN) sum = INT16_MIN;
    return (int16_t)sum;
}
