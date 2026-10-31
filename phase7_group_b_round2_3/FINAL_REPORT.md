# Phase 7 Group B Round 2.3.1 — Final Report

## Summary

**Status: NOT_YET** — Board disconnection prevented final verification. All code fixes complete and build succeeds.

## Changes Made

### 1. Fixed btn_teardown HAL Destruction Order (P0) ✅

**Root Cause:** `btn_teardown()` destroyed the fake HAL BEFORE stopping the button service, causing use-after-free. The button service task uses the HAL for GPIO reads — destroying the HAL while the task is still running causes undefined behavior.

**Fix:**
```c
static esp_err_t btn_teardown(void)
{
    /* 1. Stop button service FIRST — it uses the HAL */
    esp_err_t err = group_d_test_global_cleanup();
    if (err != ESP_OK) {
        group_d_test_set_poisoned(true);
        return err;
    }
    /* 2. Only destroy HAL after service confirmed stopped */
    fake_hal_destroy(fake_hal_get_singleton());
    return ESP_OK;
}
```

Also fixed `btn_setup()` to check cleanup return value before destroying leaked HAL.

### 2. Fixed Integration Test Service Readiness ✅

**Root Cause:** `setup_services()` used a fixed 100ms delay to wait for services to start. This was unreliable — services might not be ready within 100ms.

**Fix:** Replaced fixed delay with snapshot polling:
```c
static bool wait_service_ready(esp_err_t (*get_snap)(void *), void *snap_buf,
                                uint32_t timeout_ms)
{
    uint32_t t0 = xTaskGetTickCount() * portTICK_PERIOD_MS;
    while ((xTaskGetTickCount() * portTICK_PERIOD_MS - t0) < timeout_ms) {
        if (get_snap(snap_buf) == ESP_OK) return true;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}
```

Now `setup_services()` polls each service's `get_snapshot()` API with a 2-second timeout.

### 3. Removed Fixed vTaskDelay Guesses ✅

**Before:** 15+ tests used `vTaskDelay(pdMS_TO_TICKS(300))` as a guess for dispatch timing.

**After:** All replaced with deterministic `wait_for_dispatch(5000)`:
```c
static bool wait_for_dispatch(uint32_t timeout_ms)
{
    uint32_t t0 = xTaskGetTickCount() * portTICK_PERIOD_MS;
    while (real_adapter_dispatch_count(s_adapter_ctx) < 1) {
        if ((xTaskGetTickCount() * portTICK_PERIOD_MS - t0) > timeout_ms) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}
```

Tests updated:
- DETERGENT, DRAIN, DRY, UV dispatch
- wrong service terminal, stale request_id, duplicate terminal
- cancel ACK
- QueueSet program/urgent/service wake
- BTN3 abort, UV skip, dispatch failure, position timeout

**7 vTaskDelay(300) intentionally kept** in cancel/reset flow delays — these wait for state transitions, not dispatch.

### 4. Fixed teardown_services() ✅

Removed the 50ms delay after service stop. The `*_stop()` functions wait for task join before returning ESP_OK, so the delay was unnecessary.

### 5. C11 Atomic for fake_hal (from R2.3) ✅

- `btn_state_gen`: `volatile uint32_t` → `_Atomic uint32_t` with `memory_order_release`
- `btn_read_count`: `volatile uint32_t` → `_Atomic uint32_t` with `memory_order_acquire`
- Removed dead `btn_irq_sem` code

### 6. Removed 36 Illegal TEST_IGNORE_MESSAGE (from R2.3) ✅

- wash_executor_round2_integration_tests.c: 21 → 0
- group_d_tests.c: 15 → 0
- Only 21 legitimate hardware gate tests remain in test_runner.c

## Build Status

- ESP32 build: ✅ SUCCESS
- Binary size: 658,080 bytes (37% free)
- SHA256: 67743e0a20ff6c2a139aa0425aba44b568bfb92a2b31b6cd87f304430de00bfd

## Test Results

### Previous Run (R2.3, before fixes)
- 527 PASS, 22 FAIL
- 15 group_d button tests: cascade failure from btn_teardown use-after-free
- 7 integration tests: service not ready at dispatch time

### Expected After Fixes
- 15 group_d button tests: should PASS (teardown order fixed)
- 7 integration tests: should PASS (service readiness polling)
- Total: 672 PASS / 0 FAIL / 15 IGNORE / 687 TOTAL

## Production Code Review Findings

### F2: app_bootstrap_deinit executor timeout
- **Status:** CONTRACTUALLY_SAFE
- **Reason:** Executor stop timeout preserves resources for retry. This is correct design — not a leak.

### F3: rollback_halted cascade
- **Status:** CONTRACTUALLY_SAFE
- **Reason:** rollback_halted protects downstream resources. If a service stop fails, halting prevents cascading failures. Retry mechanism handles recovery.

### F5: button init/start failure ignored
- **Status:** CONFIRMED_BUG
- **File:** main/app_bootstrap.c:303
- **Issue:** Button service init/start failure is logged as warning but err is overwritten. Bootstrap returns ESP_OK with a dead button.
- **Fix:** Should propagate error or fail bootstrap.

### F6: global_mutexes single boolean
- **Status:** CONFIRMED_BUG
- **File:** main/app_bootstrap.c:337
- **Issue:** Single boolean for 7 independent `_global_init()` calls. Partial failure leaves earlier mutexes initialized but untracked.
- **Fix:** Should use per-service tracking or atomic bitmask.

**Note:** F5 and F6 are production code issues in `main/app_bootstrap.c`. They should be fixed in a future round but do not block this round's gate.

## Gate Status

```
PHASE7_GROUP_B_ROUND2_3_1_EXIT_GATE: NOT_YET
PHASE7_GROUP_B_ROUND2_EXIT_GATE: NOT_YET
READY_FOR_PHASE8: NO
```

**Reason:** Board disconnection prevented final verification. All code fixes are complete and build succeeds. Awaiting board reconnection for full test run.

## Files Modified

1. `components/fake_hal/fake_hal.c` — C11 atomic, removed btn_irq_sem
2. `test_apps/unit/main/group_d_test_helpers.h` — btn_teardown/btn_setup HAL order fix
3. `test_apps/unit/main/group_d_tests.c` — Removed 15 TEST_IGNORE_MESSAGE
4. `test_apps/unit/main/wash_executor_round2_integration_tests.c` — Removed 21 TEST_IGNORE_MESSAGE, deterministic sync, service readiness polling

## Next Steps

1. Reconnect ESP32 board (COM7)
2. Flash updated firmware (SHA256: 67743e0a...)
3. Run 15 button tests individually
4. Run 7 previous failure tests individually
5. Run all 26 Round 2 integration tests
6. Diagnostic full run
7. 3 formal rounds (672/0/15/687)
8. 30-minute soak
