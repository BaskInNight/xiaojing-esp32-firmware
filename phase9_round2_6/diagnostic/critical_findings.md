# Round 2.6.1 - Critical Findings

## P1-1: Audio Layering (FIXED)
**Problem**: 8x gain was applied in shared audio_driver, affecting ALL paths.
**Fix**: audio_driver outputs neutral >>14 PCM. Edge applies 8x with int64 safe multiply.

## P1-4: False Wake at 7 seconds (OBSERVED)
**Finding**: False wake detected at 7s with score=0.746
- Current threshold: 0.60 (EI_CLASSIFIER_THRESHOLD)
- Old project threshold: 0.80
- Score 0.746 < 0.80, would NOT trigger in old project

**Action**: Continue 10-minute test. Threshold adjustment pending evidence.

## WiFi Status
**ASSOC_EXPIRE** resolved after retries. WiFi connected at 12s with IP 10.196.41.88.

## Current Status
- Monitoring continuing...
- Will report full statistics at end of 10-minute test
