# Phase 7 Group B Round 2.3.3 — Final Report

## Date: 2026-07-28

## Binary
- SHA256: 2F444763F03276D7F02DD2C97472786725DF9E1B864BE454E820F325186E41E0
- ESP-IDF v5.5.4, ESP32-S3

## SKILLS_ACTUALLY_INVOKED
- /embedded-systems: FreeRTOS task stop/join lifecycle audit
- /esp32: ESP-IDF task/queue/notification patterns
- /debugging-code: PC=0xA5A5A5A5 root cause analysis
- /code-review: Production code diff review (via structured manual review)

## ROOT_CAUSE_CALL_CHAIN
1. Test N fails mid-assertion (Unity longjmp)
2. teardown_services() never runs
3. Services still running, hold HAL references
4. Test N+1 starts → setup_services() calls fake_hal_destroy(leaked)
5. Service tasks from test N access freed HAL → PC=0xA5A5A5A5

## PC_A5_CLASSIFICATION
- Type: Use-After-Free (UAF)
- Location: Service task accessing destroyed fake HAL
- Trigger: Unity longjmp skipping teardown, then setup destroying HAL while tasks alive

## TEST_RESET_CONTRACT
- **Changed from void to esp_err_t**: drain_service_test_reset, detergent_service_test_reset, uv_service_test_reset, bl50_service_test_reset
- **bl50_service_test_reset**: Now waits for task join (9s timeout) before resetting

## TASK_WAKE_MODEL
- drain/detergent/uv: atomic bool polling (50ms tick) + xTaskNotify for stop
- water: xTaskNotifyWait + atomic bool polling (50ms tick)
- dry: xTaskNotifyWait + atomic bool polling (50ms tick)

## TASK_DONE_MODEL
- All services: Binary semaphore (task give, stop take)
- Not generation-aware (consumable token)

## JOIN_OWNER_MODEL
- Single join owner via stop_mtx serialization
- task_joined flag prevents double-join

## STOP_TIMEOUT_RESOURCE_POLICY
- On timeout: preserve all resources, return ESP_ERR_TIMEOUT
- Allow retry on next stop() call

## SECOND_STOP_RETRY
- If task already exited: stale semaphore token consumed immediately
- Target: <500ms for retry

## GLOBAL_TEARDOWN_POLICY
- setup_services(): Stop ALL services BEFORE destroying leaked HAL
- teardown_services(): Check all stop() return values, don't destroy HAL if any fail

## Fixes Applied (R2.3.3)

### P0: UAF in setup_services
- **Root cause**: fake_hal_destroy(leaked) called while services still running
- **Fix**: Stop all services first, then destroy HAL
- **File**: wash_executor_round2_integration_tests.c

### P0: Stop-drain infinite loop bug
- **Root cause**: `continue` statement in stop-drain loop skipped retry counter increment
- **Impact**: drain/detergent/uv tasks never exit when terminal sink blocked
- **Fix**: Increment counter before `continue` in all 3 services
- **Files**: drain_service.c, detergent_service.c, uv_service.c

### P0: Water/dry service stop-drain forced exit
- **Root cause**: Water/dry services had no stop-drain retry limit
- **Fix**: Added WATER_STOP_DRAIN_MAX=90, DRY_STOP_DRAIN_MAX=60 counters
- **Files**: water_service.c, dry_service.c

### P0: bl50_service_test_reset UAF
- **Root cause**: test_reset() zeroed state without waiting for task join
- **Fix**: Now waits for task done semaphore (9s timeout) before resetting
- **File**: bl50_service.c

### test_reset contract
- Changed drain/detergent/uv/bl50 test_reset from void to esp_err_t
- Test code now checks return values via TEST_ASSERT_EQUAL

### Teardown resource ownership
- setup_services(): Stops all services with leaked HAL before destroying
- teardown_services(): Checks stop() return values, preserves HAL on failure

## Diagnostic Results (run5)
- PASS: 652
- FAIL: 4 (test expectation issues, not bugs)
- IGNORE: 15
- TOTAL: 671

## Remaining Issues (4 tests)
- WATER-R5-07, DRAIN-R42-08, DETG-R42-08, UV-R42-08: Tests designed for OLD behavior (stop timeout). New behavior (service exits via drain retries) is correct but tests need further updates.

## Gate Status
```
PHASE7_GROUP_B_ROUND2_3_3_EXIT_GATE: NOT_YET (4 test expectations need updating)
PHASE7_GROUP_B_ROUND2_EXIT_GATE: NOT_YET
READY_FOR_PHASE8: NO
```

## Next Steps
1. Fix 4 remaining test expectations
2. Run full diagnostic (0 FAIL)
3. Run 3 formal rounds
4. Start 4-5 hour soak
