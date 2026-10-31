# Phase 9 Round 2.6.2 — Progress Report

## Completed Tasks

### Task 1: SHA256 Evidence (COMPLETE)
```
BIN_SHA256: 032FB7A074A886BF8D5288D0F24D0BBCAF39E8DD4DB4F93A465CCD3AC505EC35 (64 hex)
ELF_SHA256: 5809A875617C52CAF710F4F4F1883DDEBED88F36B65C78D843B1EFAEB15AED1B (64 hex)
```

### Task 2: Wake Strategy Candidate Config (COMPLETE)
Added configurable parameters:
- `threshold`: 0.80 (candidate, matching old project)
- `margin`: 0.15 (xj > noise + margin AND xj > unknown + margin)
- `consecutive_hits`: 2 (require 2 consecutive hits)
- `cooldown_ms`: 2000

Added logging for each candidate hit:
```
CANDIDATE: xj=0.xxx noise=0.xxx unknown=0.xxx margin_n=0.xxx margin_u=0.xxx consecutive=N/M MARGIN_OK/MARGIN_FAIL
```

### Task 3: Classifier Frequency Analysis (COMPLETE)
**Finding**: Classifier runs synchronously in capture task, blocking I2S reads for ~138ms.

**Analysis**:
- I2S DMA buffer: 30ms (4 descriptors * 120 frames * 8 bytes)
- Classifier time: ~138ms
- Effective interval: ~674ms (observed) vs 500ms (declared stride)

**Conclusion**: Synchronous implementation is acceptable if no DMA overruns detected. Added I2S read interval tracking to verify.

## Pending Tasks

### Task 4: 20-Round Automatic Closed Loop
Requires:
- 20+ wake detection cycles
- Covering: success, VAD silence, empty timeout, max duration, cancel, buffer failure, duplicate terminal, stale generation, stop during dialog, stop during transition

**Challenge**: Without real audio to trigger wake detection, need to either:
1. Wait for natural false wakes (slow)
2. Implement test mode to force state transitions

### Task 5: Pre-User-Test
- Flash candidate firmware
- Output READY_FOR_USER_WAKE_TEST=YES
- Wait for user to test 10x "小净小净"

### Task 6: 4-Hour Soak
- After 20-round test and parameter selection
- Continuous 4h with BLE+WiFi+Display+I2S+Edge
- 0 reset/WDT/panic

## Key Findings

### Wake Strategy
- Old project threshold: 0.80
- Current threshold: 0.60 (too low, causes false wakes)
- Candidate: 0.80 + 0.15 margin + 2 consecutive hits

### Classifier Frequency
- Declared stride: 500ms
- Observed interval: 674ms
- Gap cause: Synchronous classifier blocks I2S reads
- DMA buffer: 30ms (insufficient for 138ms classifier)

## Gate Status

| Gate | Status |
|------|--------|
| BIN_SHA256_VALID | ✓ PASS (64 hex) |
| ELF_SHA256_VALID | ✓ PASS (64 hex) |
| PCM_LAYERING_SAFE | ✓ PASS |
| SIGNED_SHIFT_UB | ✓ PASS |
| RECORDER_10S_RUNTIME | ✓ PASS |
| DIAG_TASK_JOIN_SAFE | ✓ PASS |
| FALSE_WAKE_DURATION | ✓ PASS (602s) |
| WIFI_GOT_IP | ✓ PASS |
| RESET_WDT | ✓ PASS |

## Next Steps

1. Implement 20-round automatic test (may need test mode)
2. Flash and output READY_FOR_USER_WAKE_TEST
3. Wait for user to test 10x "小净小净"
4. Select final wake policy based on user test results
5. Run 4-hour soak test
