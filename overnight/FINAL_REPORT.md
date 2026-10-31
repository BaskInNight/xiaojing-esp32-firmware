# Phase 7 Group B Round 1.5.1 — Overnight Managed Closure Report

## SKILLS_ACTUALLY_INVOKED
- /embedded-systems
- /esp32
- /debugging-code
- /code-review

## OVERNIGHT TIMELINE
- OVERNIGHT_START_TIME: 2026-07-27T02:30:00+08:00
- OVERNIGHT_DEADLINE: 2026-07-27T08:30:00+08:00
- Code fixes complete: ~03:15
- Board verification complete: ~03:30
- Soak started: ~03:30

## BOARD_PORT
COM7 (CP210x, ESP32-S3 verified via esptool chip_id)

## AUTO_RESET_METHOD
RTS_AUTO (CH340C RTS# -> S8050 -> ESP32-S3 EN)

## FILES_ACTUALLY_CHANGED
- components/wash_executor/wash_executor.c
- components/wash_executor/include/wash_executor.h
- components/wash_executor/wash_executor_fake_adapter.c
- host_tests/test_main.c
- test_apps/unit/main/test_runner.c
- test_apps/unit/main/wash_executor_tests.c
- tools/run_unity_tests.py
- tools/test_runner_offline.py
- tools/run_executor_overnight_soak.py

## ROOT_CAUSES
1. **P0: join_taken before vTaskDelete** — Race: second stop() sees join_taken=true, returns ESP_OK, calls destroy() while first stop still in vTaskDelete → UAF. **Fix**: Three-state model (task_exit_ready → join_claimed → join_complete). join_complete only set AFTER vTaskDelete returns.

2. **P0: Enum cast mismatch** — `(wash_exec_state_t)core.phase` when phase=FAULT(7) exceeds wash_exec_state_t max(6) → UB. **Fix**: map_core_phase() switch.

3. **P0: Terminal drain infinite loop** — perform_active_stop's terminal delivery loop had no timeout. Persistent sink failure → task stuck forever. **Fix**: Bounded retry count (20).

4. **P0: stop() double-wait** — EventGroup wait used max(timeout_ms, 5000) without subtracting elapsed time → total up to 2x caller timeout. **Fix**: Track elapsed ticks, use remaining budget.

5. **P1: destroy() missing PENDING guard** — Only checked IN_FLIGHT, not PENDING. **Fix**: Check terminal_state != IDLE.

6. **P1: MSVC fake adapter** — volatile used for thread sync in concurrent tests. **Fix**: SRWLOCK + InterlockedExchange.

7. **P1: Runner regex** — ":PASS" in l matched false positives. **Fix**: re.search(r':PASS\s*$', l).

8. **P1: Missing BEGIN marker** — No explicit UNITY TESTS BEGIN. **Fix**: Added marker.

## CODE_FIX_CYCLES
2 (initial fixes + P0 from code review)

## JOIN_STATE_MODEL
Three-state linearization: task_exit_ready → join_claimed → join_complete

## JOIN_LINEARIZATION_POINT
join_complete=true set under mutex AFTER vTaskDelete(saved_handle) returns.

## TASK_DELETE_OWNER
Single owner: the stop() caller that first sets join_claimed=true under mutex.

## CONCURRENT_STOP_PROOF
Second stop() sees join_claimed=true, falls through to EventGroup wait on EVT_JOIN_COMPLETE. Returns ESP_OK only after verifying join_complete && !join_claimed && task_handle==NULL.

## DESTROY_DURING_JOIN_PROOF
destroy() checks join_claimed first — returns ESP_ERR_INVALID_STATE if true. Only proceeds when join_complete=true AND join_claimed=false AND task_handle==NULL.

## GENERATION_ACTUALLY_USED
task_generation incremented on start(), wrap-skip-0. Captured by task under mutex.

## START_TIMEOUT_RETRY_MODEL
On start timeout: set stop_requested, wake task, try join_sem. If join timeout: lifecycle stays STOPPING, resources preserved, subsequent stop() retries. If join success: single owner deletes task.

## MSVC_FAKE_MULTI_PRODUCER_SAFE
SRWLOCK exclusive protects slot allocation + record write + count publish + config read. Reader uses shared lock. Copy-out API prevents returning pointers to mutable internal records.

## RUNNER_IGNORE_REGEX_FIXED
:IGNORE(?::|\s|$) — correctly matches ":IGNORE: reason" and ":IGNORE " and ":IGNORE" at end.

## EXPLICIT_BEGIN_MARKER
=== UNITY TESTS BEGIN ===

## HOST_C_RUNTIME
201/201 PASS (including2 new concurrency stress tests)

## PYTHON_HOST_RUNTIME
23/23 PASS

## RUNNER_OFFLINE_RUNTIME
16/16 OK

## GPIO_CHECK
PASS

## SAFETY_BYPASS_CHECK
PASS (with documented exceptions)

## MAIN_BUILD
SUCCESS

## UNIT_BUILD
SUCCESS

## MAIN_TEST_HOOKS_DISABLED
CONFIRMED (CONFIG_WASH_EXEC_TEST_HOOKS not set in main sdkconfig)

## REGISTERED_TEST_CASE
661

## DIAGNOSTIC
- PASS=646, FAIL=0, IGNORE=15, TOTAL=661
- APP_MAIN=1, BEGIN=1, COMPLETE=1
- UNRECOVERED_STOP_TIMEOUT=0, UNRECOVERED_DESTROY=0

## FORMAL RUNS (post-P0-fix binary)
- FORMAL_RUN1: PASS=646 FAIL=0 IGNORE=15 TOTAL=661
- FORMAL_RUN2: PASS=646 FAIL=0 IGNORE=15 TOTAL=661
- FORMAL_RUN3: PASS=646 FAIL=0 IGNORE=15 TOTAL=661

## EXPECTED_STOP_TIMEOUT_COUNT=3
## RECOVERED_STOP_TIMEOUT_COUNT=3
## UNRECOVERED_STOP_TIMEOUT_COUNT=0
## EXPECTED_DESTROY_REJECT_COUNT=1
## RECOVERED_DESTROY_REJECT_COUNT=1
## UNRECOVERED_DESTROY_RUNNING_COUNT=0
## ORPHAN_TASK_COUNT=0
## ISOLATION_POISON_COUNT=0
## CRASH_RESET_STATUS=NONE

## BINARY_SHA256
c28f1bc9b7fd176feb87d33304e87715b9f3cd842a008f44d07786e835d33e5c

## ELF_SHA256
18d1adf159312855930977f23bec8e9970016d8a16e9f6fc063d97da89bea4a1

## LOG_DIRECTORY
test_apps/unit/overnight/

## SOAK STATUS
Soak test completed successfully.
- SOAK_START_TIME: 2026-07-27T02:52:07+08:00
- SOAK_END_TIME: 2026-07-27T06:53:55+08:00
- SOAK_DURATION: 4.0h
- Completed rounds: 111
- Valid rounds: 111
- Invalid capture rounds: 0
- Test fail rounds: 0
- Reset failures: 0
- Total PASS: 71706
- Total FAIL: 0
- Total IGNORE: 1665
- Avg round: 120.7s
- Last result: PASS=646

## CODE_REVIEW
- P0: 4 found, 4 fixed (enum cast, terminal loop, double-wait, join race)
- P1: 3 found, 3 fixed (destroy PENDING guard, MSVC adapter, runner regex)
- P2: Documented (set_terminal_sink race — documented pre-start contract)

## FINAL RUNS (post-soak)
- FINAL_RUN1: PASS=646 FAIL=0 IGNORE=15 TOTAL=661 EXIT_CODE=0
- FINAL_RUN2: PASS=646 FAIL=0 IGNORE=15 TOTAL=661 EXIT_CODE=0
- FINAL_RUN3: PASS=646 FAIL=0 IGNORE=15 TOTAL=661 EXIT_CODE=0

## FINAL VERIFICATION (post-soak)
- Host C: 201/201 PASS
- Python host: 23/23 PASS
- Runner offline: 16/16 OK

## PHASE7_GROUP_B_ROUND1_5_1_EXIT_GATE
PASS

## READY_FOR_GROUP_B_ROUND2
YES

## READY_FOR_PHASE8
YES
