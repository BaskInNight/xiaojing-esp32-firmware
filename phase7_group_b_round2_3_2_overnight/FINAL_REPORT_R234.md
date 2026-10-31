# Phase 7 Group B Round 2.3.4 — Final Report

## SKILLS_ACTUALLY_INVOKED
- /embedded-systems: FreeRTOS task stop/join lifecycle, terminal delivery contract
- /esp32: ESP-IDF service architecture, queue/semaphore patterns
- /debugging-code: Serial diagnostic analysis, stop-drain infinite loop root cause
- /code-review: Structured manual review (skill unavailable, equivalent analysis performed)

## ROUND233_VALIDITY: INVALID
- run6.log: 2 BEGIN, 0 COMPLETE, double-boot merge

## REGISTERED_TESTS_FROM_ELF: 687

## CURRENT_EXPECTED_SINGLE_RUN
- PASS=672, FAIL=0, IGNORE=15, TOTAL=687

## Diagnostic Results (Single Boot)
- BEGIN: 1, COMPLETE: 1, VALID: True
- PASS: 669, FAIL: 3, IGNORE: 15, TOTAL: 687
- NON_HW_IGNORE: 0

## FOUR_FAILURES_EXACT
1. R2: cancel ACK matches correctly — Expected TERM_RESULT_CANCELED(1) Was TERM_RESULT_SKIPPED(3)
2. R2: UV skip waits for SKIPPED terminal — Expected 0 Was 3
3. R2: dispatch failure enters FAULT — Expected TRUE Was FALSE

## Fixes Applied (R2.3.3 + R2.3.4)

### Terminal Delivery Contract
- Pending terminal preserved when drain retries exhausted
- stop() returns ESP_ERR_TIMEOUT until terminal delivered
- Second stop() retries terminal publish from caller context
- Only cleanup when terminal successfully published

### Null Sink Cascade Fix
- Services now publish to machine_event_queue (was null_sink)
- Eliminates 16 "Leaked service cleanup" cascade failures

### Stop-Drain Counter Fix
- drain/detergent/uv: counter increment before `continue`
- water: forced exit after 90×50ms
- dry: forced exit after 60×50ms

### UAF Fixes
- setup_services: stop services before destroying leaked HAL
- bl50 test_reset: wait for task join before resetting
- test_reset void→esp_err_t for drain/detergent/uv/bl50

## 3 Remaining Failures Analysis
1. **cancel ACK**: Adapter routing priority fixed but terminal still matches wrong slot
2. **UV skip**: Error code 3 instead of 0 — needs investigation
3. **dispatch failure**: Position service stop might interfere with expected fault

## DANGEROUS_CLEANUP_SCAN
- All test_reset calls now return esp_err_t (drain/detergent/uv/bl50)
- setup_services checks all stop() return values
- teardown_services preserves HAL on stop failure

## Gate Status
```
PHASE7_GROUP_B_ROUND2_3_4_EXIT_GATE: NOT_YET (3 FAIL remaining)
PHASE7_GROUP_B_ROUND2_EXIT_GATE: NOT_YET
READY_FOR_PHASE8: NO
```

## Next Steps
1. Fix 3 remaining integration test failures
2. Run formal 3-round validation
3. Start 4-5 hour soak
