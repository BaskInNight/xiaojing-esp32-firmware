# Phase 7 Group B Round 2.3.2 — Final Report

## Date: 2026-07-28 (Overnight Unattended)

## Binary
- SHA256: 99891557651535AF3FB8AD8532D4ECD1B6EA22CC87E2BA22AF217304AA3E8CEF
- ESP-IDF v5.5.4, ESP32-S3

## Diagnostic Results
- **PASS: 1311 / FAIL: 0 / IGNORE: 30 / TOTAL: 1341**
- Per run: ~655 PASS, 0 FAIL, 15 IGNORE
- **NON_HW_IGNORE: 0**
- All IGNOREs are legitimate [hw] hardware gate tests

## Fixes Applied (12 total)

### P0: btn_teardown use-after-free
- `btn_teardown()` destroyed HAL BEFORE stopping button service
- Fix: Stop service first, then destroy HAL

### Button test timing (9 tests)
- `gen` counter read BEFORE setting button state → race condition
- Fix: Set state FIRST, then read counter, then wait

### fake_hal ctx_wait_btn_irq
- Returned early on state change → debounce never confirmed
- Fix: Always wait full timeout (debounce needs it)

### Safety manager lifecycle
- `water_service_stop()` calls `safety_manager_unregister_stop_hook()` which fails if safety_manager not running
- Fix: Start safety_manager BEFORE stopping services in setup

### Service lifecycle reset
- `bl50/drain/detergent/uv_service_stop()` sets STOPPED, but `init()` requires UNINITIALIZED
- Fix: Call `*_test_reset()` before `*_init()` for services that have it

### Water service config
- Missing required fields: `source_batch_max_ms`, `default_transfer_ms`, `transfer_timeout_ms`
- Fix: Set all required config fields

### Deterministic dispatch
- Replaced `vTaskDelay(300)` with `wait_for_dispatch(5000)` in 15 tests

### Service readiness
- `wait_all_services_ready()` polls `get_snapshot()` with bounded timeout

### Executor cleanup
- `setup_real_adapter()` cleans leaked resources from failed tests

### Adapter cancel/reset routing priority
- Cancel/reset checked BEFORE active (same request_id during cancel flow)
- Fix: Reorder checks in `route_machine_event()`

### F5 Button bootstrap failure (production)
- Button init/start failure was logged as warning and ignored
- Fix: Now propagates via `goto fail`

### F6 Global init state (production)
- Single `global_mutexes` bool for 7 services
- Fix: Per-component tracking

## Code Review Findings
- F2: CONTRACTUALLY_SAFE (executor timeout preserves resources)
- F3: CONTRACTUALLY_SAFE (rollback_halted protects downstream)
- F5: FIXED (button init/start failure now fatal)
- F6: FIXED (per-component tracking)

## Skills Used
- /esp32: ESP32-S3 lifecycle, FreeRTOS task/queue/EventGroup
- /embedded-systems: stop/join ordering, HAL ownership, service lifecycle
- /debugging-code: Serial diagnostic capture for service start errors
- /code-review: Production code F5/F6 findings

## Gate Status
```
PHASE7_GROUP_B_ROUND2_3_2_EXIT_GATE: PASS (diagnostic)
PHASE7_GROUP_B_ROUND2_EXIT_GATE: NOT_YET (pending formal rounds + soak)
READY_FOR_PHASE8: NO (pending soak completion)
```

## Next Steps
1. Run formal 3-round test (672/0/15/687 per round)
2. Start 5-hour soak
3. Post-soak final regression
