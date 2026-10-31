# Phase 9 Combined: Build Status (Honest)

## Build Result
- **Status**: BUILD SUCCEEDED
- **Firmware size**: 3,827,184 bytes (3.65 MB)
- **Partition size**: 4,194,304 bytes (4.00 MB)
- **Free space**: 267,120 bytes (6%)

## Symbols in ELF

### Edge Impulse ✅
- `run_classifier` — PRESENT
- `ei_classifier_init` — PRESENT
- `ei_classify_audio` — PRESENT
- `tflite_learn_982269_4_compiled` — PRESENT (model data)
- TFLite Micro runtime — PRESENT
- CMSIS-DSP/NN — PRESENT

### ESP-SR ⚠️ (Placeholder)
- `voice_wake_esp_sr` wrapper — PRESENT
- AFE/WakeNet functions — NOT LINKED (wrapper is placeholder)

### voice_frontend ✅
- `voice_frontend_init` — PRESENT
- `voice_frontend_start_backend` — PRESENT
- `voice_frontend_stop` — PRESENT
- Bootstrap callbacks — PRESENT

## Stage Status

### Stage 3: Edge Impulse ✅ (Build Complete)
- Model compiled and linked
- Classifier wrapper functional
- Resampler implemented (16000→16062 Hz)
- **NOTE**: Model requires Edge Impulse subscription (LEGAL_REVIEW documented)
- **NOTE**: Real inference not tested on device yet

### Stage 4: ESP-SR ⚠️ (Component Added, Wrapper Only)
- `espressif/esp-sr` v1.9.0 installed via Component Registry
- Compatible with ESP-IDF ≥ 4.4
- Wrapper exists but AFE/WakeNet not initialized
- **Next**: Implement real AFE initialization

### Stage 5: NOT_STARTED
- BTN1 switching not implemented

### Stage 6: NOT_STARTED
- No tests written

## Partition Concern
With 6% free space, adding ESP-SR AFE + WakeNet9 models may exceed the 4MB partition. May need:
1. Custom partition table with larger app partition
2. Model partition for WakeNet9
3. Or reduce Edge Impulse SDK footprint

## Files Modified
- `components/voice_wake_edge/` — Full Edge Impulse integration
- `components/voice_wake_esp_sr/` — ESP-SR placeholder
- `components/voice_frontend/` — Unified frontend
- `main/app_bootstrap.c` — Frontend + task_done integration
- `main/idf_component.yml` — Added esp-dsp, esp-sr dependencies
