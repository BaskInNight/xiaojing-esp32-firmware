# Phase 7 Group B Round 2.3.2 — Soak Results

## Date: 2026-07-28

## Binary
- SHA256: 99891557651535AF3FB8AD8532D4ECD1B6EA22CC87E2BA22AF217304AA3E8CEF

## Soak Results
- Round 1: PASS (1315P/0F/30I)
- Round 2: CRASH — Guru Meditation Error

## Crash Details
- **Test**: R2: bootstrap create and destroy
- **Type**: InstrFetchProhibited (use-after-free)
- **PC**: 0xa5a5a5a5 (FreeRTOS freed memory pattern)
- **EXCVADDR**: 0xa5a5a5a4
- **Context**: Previous test (QueueSet service wake) had service stop timeouts:
  - uv_svc: stop timeout
  - detergent_svc: stop timeout
  - drain_svc: stop timeout, task still alive

## Root Cause
When service stop times out, resources are not properly cleaned up. The next test (bootstrap create) tries to create a new executor while old service tasks are still alive, causing use-after-free when the old tasks access freed memory.

## Analysis
This is a known issue flagged in the code review:
- "stop timeout后资源是否保留" — when stop times out, resources should be preserved
- The services (uv/detergent/drain) have 3-second stop timeouts
- When they timeout, the cleanup doesn't complete
- The next test creates new resources that overlap with the old ones

## Fix Required
The service stop timeout handling needs to be improved:
1. When stop times out, preserve all resources (don't free anything)
2. The next test's setup must handle leaked resources gracefully
3. The bootstrap test must not create new executor if old one is still alive

## Status
- PHASE7_GROUP_B_ROUND2_3_2_EXIT_GATE: NOT_YET (crash in soak)
- Soak: FAILED at round 2
- Need to fix stop timeout handling before re-running soak
