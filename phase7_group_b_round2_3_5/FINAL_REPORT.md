# Phase 7 Group B Round 2.3.5 - Final Report

Date: 2026-07-28

## Gate status

- `PHASE7_GROUP_B_ROUND2_3_5_EXIT_GATE: PASS`
- `PHASE7_GROUP_B_ROUND2_EXIT_GATE: PASS`
- `READY_FOR_PHASE8: YES`

## Root causes fixed

1. Normal service dispatch incorrectly populated the final-reset slot. Because terminal
   routing checks cancel/reset before the active slot, normal terminals could be
   misclassified as reset acknowledgements.
2. UV cancel/skip received during `VALIDATING` was silently ignored, creating a
   request/command timing race.
3. Real-adapter dispatch slots and counters were accessed across tasks without one
   synchronized snapshot/commit model.
4. Integration-test cleanup could delete sinks, queues, or HAL instances before all
   producer/consumer tasks had joined. Unity longjmp paths amplified the leak into
   `PC=0xA5A5A5A5` use-after-free crashes.
5. Adapter-consumer start timeout and bootstrap teardown ignored stop failures and
   could destroy live-task resources.
6. Application deinit logged service/button/safety stop failures but continued to
   destroy downstream queues and HAL resources.

## Safety/lifecycle corrections

- Normal dispatch, cancel, and final reset now use separate two-phase slots.
- Slot state and diagnostics are protected by the adapter state mutex.
- UV cancel/skip is accepted in both `VALIDATING` and `RUNNING`, and still performs
  the two-phase lamp-OFF action before terminal delivery.
- Test cleanup order is producer services -> executor -> adapter consumer -> queues
  -> safety -> HAL, with errors propagated and resources preserved for retry.
- Adapter consumer stack increased to 4096 after the board exposed a stack overflow.
- Adapter start timeout preserves semaphores/task ownership until a successful retry.
- `real_adapter_destroy_consumer()` and bootstrap destroy APIs return checked errors.
- Bootstrap detaches the executor before destruction and never destroys a live
  consumer.
- Button/service/safety stop failure now halts rollback/deinit before downstream
  resource destruction.

## Build and host verification

- Main ESP-IDF build: PASS (`xiaojing_idf.bin` size `0x656d0`)
- Unit ESP-IDF build: PASS (`xiaojing_unit_test.bin` size `0xa26f0`)
- Host C: 201/201 PASS
- Python host: 23/23 PASS
- Runner offline tests: 16/16 PASS
- Safety bypass scan: PASS (only documented position-service IBT-2 exceptions)
- GPIO map scan: PASS
- Ignored lifecycle-return scan: 0 findings

## Board verification (COM7, ESP32-S3)

Strict acceptance for every cycle:

- BEGIN marker: exactly 1
- COMPLETE marker: exactly 1
- PASS/FAIL/IGNORE/TOTAL: 674/0/15/689
- Valid single boot: true

Results:

- Diagnostic: PASS
- Formal run 1: PASS
- Formal run 2: PASS
- Formal run 3: PASS
- Soak: 13/13 valid rounds, 0 failed rounds, 1833.8 seconds (30.6 min)
- Soak aggregate: 8762 PASS, 0 FAIL, 195 IGNORE
- All latest-binary board cycles: 17/17 valid, 0 FAIL
- Crash/reset signature scan: 0 findings

All 15 ignored cases are explicitly tagged `[hw]` (IBT-2, BL50, fan, flow PCNT,
water level, turbidity, audio, and real-HAL emergency tests). `NON_HW_IGNORE: 0`.

## Artifact hashes (SHA-256)

- Main BIN: `6BF70A53349BF9CD89BDD6DA4B1C42FA3625A544B7349544748D6D3054F870B9`
- Main ELF: `556BA332AED98E651562CD380C5C1C330DF8FBFA6E95FAA14EB07B368E7BB603`
- Unit BIN: `8B6E4E4E34FD2A0D8FA2A55F2977E7ED2E089F72A0A36FF54635BA5EC09A1ACB`
- Unit ELF: `2E58C983920254CC86F463008C4B5F3784F7D20499008AD14D5A5BA2A2049A05`

## Evidence files

- `codex_round235_lifecycle_diag.log/.json`
- `codex_round235_lifecycle_formal_run1.log/.json`
- `codex_round235_lifecycle_formal_run2.log/.json`
- `codex_round235_lifecycle_formal_run3.log/.json`
- `codex_round235_soak/run_01.log` through `run_13.log`
- `codex_round235_soak/checkpoint.json`
- `codex_round235_soak/summary.json`

## Review

- Structured lifecycle/concurrency review: complete
- P0 remaining: 0
- P1 remaining: 0
- Real peripheral bring-up remains hardware-gated and is not part of this Fake/logic
  exit gate.

The repository already contained extensive modified and untracked user work. No
unrelated files were reset, deleted, staged, or committed.
