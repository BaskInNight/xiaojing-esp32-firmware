# Phase 9 Round 2.6.1 — Correction Report

## Executive Summary

Completed Round 2.6.1 corrections addressing P1 production regression issues. All critical fixes applied and verified.

## P1 Fixes Applied

### P1-1: Audio Layering (FIXED)
**Problem**: 8x gain was applied in shared audio_driver, affecting ALL paths (Edge, DIALOG, PTT, ASR).

**Fix**:
- `audio_driver.c`: Reverted to neutral `>>14` PCM (1x gain)
- `voice_wake_edge.c`: Added 8x gain with safe `int64` multiply
- `voice_recorder.c`: Receives neutral 1x PCM

**Verification**:
```
PCM_LAYERING_SAFE=PASS
SIGNED_SHIFT_UB=PASS (int64 multiply, no left shift)
EDGE_GAIN_ONLY=PASS
DIALOG_GAIN_1X=PASS
```

### P1-3: Diagnostic Task Join (FIXED)
**Problem**: Polling `eTaskGetState` and forcing handle to NULL.

**Fix**:
- Added `s_diag_done_sem` (binary semaphore for TASK_DONE)
- Added `s_diag_lifecycle_lock` (serializes start/stop)
- Added `_Atomic bool s_diag_stop_requested`
- Task signals DONE before `vTaskDelete`
- Stop waits on semaphore (2s bounded), preserves resources on timeout
- Retry-safe: second stop can succeed after first timeout

**Verification**:
```
DIAG_TASK_JOIN_SAFE=PASS
```

## 10-Minute False Wake Test Results

### Test Configuration
- Duration: 602 seconds (10+ minutes)
- BLE: Advertising
- WiFi: Connected (10.196.41.88)
- Display: Active
- Edge classifier: Running continuously
- Cloud upload: Disabled

### Results
```
classifier_calls: 893
false_wake_count: 1
max_false_xj: 0.746
threshold: 0.60
old_project_threshold: 0.80
reset_count: 0
wdt_count: 0
panic_count: 0
```

### Key Finding
**False wake at 7 seconds with score=0.746**

The old project uses threshold=0.80, which would reject this detection. Current threshold=0.60 is too low.

**Recommendation**: Raise threshold to 0.80 to match old project behavior.

## Recorder Capacity Verification

### Test Results
```
session_2_pcm_bytes: 321536
session_2_duration_ms: 10036
session_2_end_reason: VAD_SILENCE
session_2_peak: 2679
session_2_rms: 1163
buffer_capacity: 328704
```

**Status**: RECORDER_10S_RUNTIME=PASS

## WiFi Status

```
WIFI_GOT_IP=PASS
IP: 10.196.41.88
Initial disconnects: 2 (during connection)
Disconnects after connect: 0
```

## SHA256 Evidence

```
BIN_SHA256: 032FB7A074A886BF8D5288D0F24D0BBCAF39E8DD4DB4F93A465CCD3AC505EC35
ELF_SHA256: 5809A875617C52CAF710F4F4F1883DDEBED88F36B65C78D843B1EFAEB15AED1B
```

## Gate Status Summary

| Gate | Status | Evidence |
|------|--------|----------|
| PCM_LAYERING_SAFE | PASS | audio_driver=1x, edge=8x, dialog=1x |
| SIGNED_SHIFT_UB | PASS | int64 multiply, no left shift |
| RECORDER_10S_RUNTIME | PASS | 10036ms, 321536 bytes |
| DIAG_TASK_JOIN_SAFE | PASS | semaphore-based, lifecycle lock |
| FALSE_WAKE_DURATION>=600 | PASS | 602 seconds |
| WIFI_GOT_IP | PASS | 10.196.41.88 |
| RESET_WDT | PASS | 0 resets, 0 WDT |

## Remaining Gates

| Gate | Status | Action Required |
|------|--------|-----------------|
| RECORDER_8S_RUNTIME | PENDING | Need 8-second speech test |
| AUTO_CYCLES>=20 | PENDING | Need 20-round state machine test |
| SOAK>=4h | PENDING | Need 4-hour continuous soak |

## Final Conclusion

**PHASE9_EDGE_SOFTWARE_GATE: NOT_YET**

Remaining blockers:
1. 20-round automatic state machine test
2. 4-hour soak test

**PHASE9_EDGE_ACOUSTIC_GATE: NEEDS_USER_LIVE_TEST**

The false wake at score=0.746 with threshold=0.60 indicates threshold should be raised to 0.80 to match old project behavior. This requires user decision.

## Files Modified

1. `components/audio_driver/audio_driver.c` — Reverted to neutral PCM
2. `components/voice_wake_edge/src/voice_wake_edge.c` — Added 8x gain with int64
3. `components/voice_wake_edge/include/voice_wake_edge.h` — Added amp stats API
4. `main/app_bootstrap.c` — Diagnostic task lifecycle fix

## Checkpoints

```
phase9_round2_6/checkpoints/
├── checkpoint_r261_p1fix.json
├── checkpoint_r261_false_wake.json
└── final_r261.json
```
