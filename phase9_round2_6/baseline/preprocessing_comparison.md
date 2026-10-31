# Stage 3: Edge Preprocessing Alignment Analysis

## Key Finding: 8x Amplitude Mismatch

### Old Project (Arduino/PlatformIO)
```cpp
int32_t amp = (i2s_data[i * 2] >> 14) << 3;
if (amp > 32767) amp = 32767; else if (amp < -32768) amp = -32768;
```
**Net shift: >> 11 (8x gain)**

### Current Project (ESP-IDF)
```c
int32_t scaled = sample >> 14;
if (scaled > INT16_MAX) scaled = INT16_MAX;
if (scaled < INT16_MIN) scaled = INT16_MIN;
```
**Net shift: >> 14 (1x gain)**

### Impact
- Old project: threshold=0.8, typical xj scores 0.8+ for real wake word
- Current project: threshold=0.6, false wakes with xj=0.71-0.73

**Root cause: 8x lower amplitude means the classifier sees quieter audio, leading to false positives.**

## Other Differences

| Parameter | Old Project | Current Project | Match? |
|-----------|-------------|-----------------|--------|
| I2S Sample Rate | 16000 Hz | 16000 Hz | ✓ |
| I2S Format | 32-bit stereo | 32-bit stereo | ✓ |
| Channel Selection | LEFT (i*2) | LEFT (channel=0) | ✓ |
| Right Shift | >> 14 | >> 14 | ✓ |
| Left Shift | << 3 | (missing) | ❌ |
| Window Size | EI_CLASSIFIER_RAW_SAMPLE_COUNT (16062) | 16000 | ❌ |
| Stride | WINDOW_SIZE / 4 (~4015) | 500ms (8000 samples) | ❌ |
| Threshold | 0.8 | 0.6 | ❌ |
| Classifier | run_classifier | run_classifier | ✓ |
| Resampler | None (window fills at model rate) | 16000→16062 | Different |

## Required Fixes

1. **Add << 3 shift**: Match old project's 8x gain
2. **Use EI_CLASSIFIER_RAW_SAMPLE_COUNT**: Window size should be 16062, not 16000
3. **Implement sliding window with stride**: Old uses WINDOW_SIZE / 4
4. **Consider threshold**: Old uses 0.8, but may need adjustment after fixing amplitude
