# Phase 7 Group B Round 2.3.2 — Pre-Check

## Date: 2026-07-28

## Binary
- File: test_apps/unit/build/xiaojing_unit_test.bin
- SHA256: 16204714897FA680F01E6EBF41CCC14F373B7C7A447206CB41E8073F01568862

## Build
- ESP-IDF v5.5.4
- Target: ESP32-S3
- Build: SUCCESS (no errors, warnings only)

## Diagnostic Results
- PASS: 659
- FAIL: 1
- IGNORE: 15
- TOTAL: 675
- NON_HW_IGNORE: 0

## Single Failure
- `R2: cancel ACK matches correctly` — Expected TERM_RESULT_CANCELED(1) Was TERM_RESULT_SKIPPED(3)
- Root cause: Cancel ACK request_id mismatch in executor cancel flow
- Impact: Isolated test, does not affect other tests

## Fixes Applied (R2.3.2)
1. **btn_teardown HAL order**: Stop service BEFORE destroy HAL (P0 use-after-free fix)
2. **Button test timing**: Set button state BEFORE reading gen counter
3. **fake_hal ctx_wait_btn_irq**: Always wait full timeout (debounce needs it)
4. **Safety manager in setup**: Start safety_manager BEFORE stopping services (water_service_stop needs it)
5. **Service lifecycle reset**: Call *_test_reset() before *_init() for bl50/drain/detergent/uv
6. **Water service config**: Add required fields (source_batch_max_ms, default_transfer_ms, transfer_timeout_ms)
7. **Deterministic dispatch**: Replace vTaskDelay(300) with wait_for_dispatch(5000)
8. **Service readiness**: wait_all_services_ready() with snapshot polling
9. **Executor cleanup**: setup_real_adapter() cleans leaked resources

## Production Code Fixes (F5/F6)
- F5: Button bootstrap init/start failure now propagates (goto fail)
- F6: Per-component global_init tracking replaces single bool

## Code Review Findings
- F2: CONTRACTUALLY_SAFE (executor timeout preserves resources)
- F3: CONTRACTUALLY_SAFE (rollback_halted protects downstream)
- F5: FIXED (button init/start failure now fatal)
- F6: FIXED (per-component tracking)
- cancel ACK: 1 remaining test issue (request_id mismatch)

## Gate Status
- PHASE7_GROUP_B_ROUND2_3_2_EXIT_GATE: NOT_YET (1 FAIL remaining)
- PHASE7_GROUP_B_ROUND2_EXIT_GATE: NOT_YET
- READY_FOR_PHASE8: NO
