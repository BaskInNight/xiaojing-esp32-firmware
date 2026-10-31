# Edge Impulse Integration - Legal Review & Technical Status

## LEGAL_REVIEW

### SDK License
- **File**: `ei-sdk/edge-impulse-sdk/classifier/ei_run_classifier.h` (line 1-33)
- **License**: The Clear BSD License (BSD-3-Clause-Clear)
- **Status**: Open source, free to use for local development

### Model License
- **File**: `model/tflite_learn_982269_4_compiled.cpp` (line 1-28)
- **License**: Edge Impulse Terms of Service (subscription-based)
- **Key clause** (line 12): "requires an active paid Edge Impulse subscription to use this Software for any purpose"
- **Key clause** (line 16): "You may NOT use this Software unless you have an active paid Edge Impulse subscription"
- **Status**: REQUIRES ACTIVE SUBSCRIPTION for local demo deployment

### Assessment
- SDK (Clear BSD): ✅ Free for local demo use
- Model (EI ToS): ⚠️ Requires active Edge Impulse subscription
- **Recommendation**: Verify user has active EI subscription before deploying

## Technical Status

### Build Errors
The Edge Impulse SDK integration into ESP-IDF 5.5.4 fails with linker errors:
- `TfLiteTypeGetName` undefined reference
- Missing TFLite Micro runtime functions

### Root Cause
The EON compiled model (`EI_CLASSIFIER_COMPILED=1`) still depends on the full TFLite Micro runtime, which has many source files and complex dependencies.

### Attempted Fixes
1. Added TFLite Micro source files to build
2. Added CMSIS-DSP/NN source files
3. Added esp-dsp FFT sources
4. Suppressed compiler warnings

### Remaining Issues
- Linker errors with TFLite Micro runtime
- Complex dependency chain requires more source files
- GCC 14.2.0 compatibility issues with older SDK code

## Recommendation

Given the license requirements and technical complexity:
1. **License**: Verify Edge Impulse subscription status
2. **Technical**: Consider using a pre-compiled static library instead of source compilation
3. **Alternative**: Use a simpler wake word approach if EI integration is blocked

## Files Created
- `components/voice_wake_edge/model/` — Model files with license headers preserved
- `components/voice_wake_edge/ei-sdk/` — Full SDK with Clear BSD license
- `components/voice_wake_edge/src/ei_classifier_wrapper.cpp` — C++ wrapper
- `components/voice_wake_edge/include/ei_classifier_wrapper.h` — C API header
