# Phase 7 Group B Round 1.5.2 — Final Report

## SKILLS_ACTUALLY_INVOKED
- /embedded-systems
- /esp32
- /debugging-code
- /code-review

## OLD_SOAK_EVIDENCE_PRESERVED
overnight/soak/ — 111 logs, 4.0h, 71706 PASS, 0 FAIL (R1.5.1)

## FILES_ACTUALLY_CHANGED
- components/wash_executor/wash_executor.c
- components/wash_executor/wash_executor_core.c
- components/wash_executor/private_include/wash_executor_core.h
- components/wash_executor/wash_executor_fake_adapter.c (unchanged from R1.5.1)
- test_apps/unit/main/wash_executor_tests.c (fault injection markers from R1.5.1)
- test_apps/unit/main/test_runner.c (UNITY TESTS BEGIN marker from R1.5.1)
- tools/run_unity_tests.py (regex fix, BEGIN marker, fault injection counters from R1.5.1)
- tools/run_executor_overnight_soak.py (new)
- tools/test_runner_offline.py (UNITY_BEGIN fixture from R1.5.1)
- host_tests/test_main.c (concurrency tests from R1.5.1)

## ROOT_CAUSES
1. **P0-1**: Terminal retry 20-cap dropped terminal permanently if sink failed 20x. **Fix**: Removed cap, task stays alive, stop() timeout handles caller abort.
2. **P0-2**: Command pump overflow injected SERVICE_TERMINAL which is ignored in DISPATCHING phase. **Fix**: Use STEP_DISPATCH_FAIL event from DISPATCHING/ACTIVE, direct fault for other phases.
3. **P1-1**: final_position_confirmed derived from terminal_result (COMPLETE→true, else false). CANCELED/ABORTED with successful reset incorrectly reported false. **Fix**: Added final_position_confirmed to core_cmd_t, set explicitly by core state machine.
4. **P1-2**: task_generation incremented twice (start + task). **Fix**: Single increment in start() only, task captures via exit_ready_generation, join owner verifies.
5. **P1-3**: core->generation had no wrap-skip-0 guard. **Fix**: Added wrap-skip-0.
6. **P1-4**: submit_program didn't validate step_count. **Fix**: Added step_count > WASH_PROGRAM_MAX_STEPS check.

## TERMINAL_RETRY_MODEL
Unbounded retry with terminal_retry_ms interval. Task stays alive until terminal delivered. stop() timeout returns ESP_ERR_TIMEOUT to caller. Subsequent stop() retries join.

## PERMANENT_SINK_FAILURE_BEHAVIOR
stop() returns ESP_ERR_TIMEOUT. Task_handle non-NULL. join_complete=false. terminal pending=true. destroy() returns ESP_ERR_INVALID_STATE. Caller must recover sink and retry stop().

## SINK_RECOVERY_BEHAVIOR
Sink recovers → terminal delivered → task enters EXIT_READY → join succeeds → stop() returns ESP_OK → destroy() succeeds.

## TERMINAL_EXACTLY_ONCE
Terminal delivered exactly once: attempt_terminal_delivery sets terminal_state=IDLE on success, never re-enters delivery.

## FINAL_POSITION_EVIDENCE_SOURCE
core_cmd_t.final_position_confirmed — set by core state machine, copied verbatim by wrapper.

## CANCEL_RESET_FINAL_POSITION
CANCEL_ACK success + FINAL_RESET dispatch success + RESET_ACK success → final_position_confirmed=true.

## ABORT_RESET_FINAL_POSITION
Same as cancel: successful reset → final_position_confirmed=true.

## RESET_FAILURE_FINAL_POSITION
reset ACK failure or reset timeout → FAULT → final_position_confirmed=false.

## TASK_GENERATION_INCREMENT_COUNT
1 per start() call only (wrap-skip-0).

## EXIT_READY_GENERATION
Written by task under mutex: exit_ready_generation = task_generation.

## JOIN_COMPLETE_GENERATION
Written by join owner under mutex after vTaskDelete: join_complete_generation = expected_generation.

## STALE_GENERATION_REJECTED
Join owner verifies exit_ready_generation == task_generation before delete. Stale join_sem from old generation rejected.

## COMMAND_PUMP_OVERFLOW_TERMINAL
From DISPATCHING/ACTIVE: STEP_DISPATCH_FAIL event → core FAULT → PUBLISH_PROGRAM_TERMINAL(result=FAULT, final_position_confirmed=false). From other phases: direct fault + terminal.

## HOST_C_RUNTIME
201/201 PASS

## PYTHON_HOST_RUNTIME
23/23 PASS

## RUNNER_OFFLINE_RUNTIME
16/16 OK

## MAIN_BUILD
SUCCESS

## UNIT_BUILD
SUCCESS

## REGISTERED_TEST_CASE
661

## DIAGNOSTIC
PASS=646 FAIL=0 IGNORE=15 TOTAL=661

## FORMAL_RUN1
PASS=646 FAIL=0 IGNORE=15 TOTAL=661

## FORMAL_RUN2
PASS=646 FAIL=0 IGNORE=15 TOTAL=661

## FORMAL_RUN3
PASS=646 FAIL=0 IGNORE=15 TOTAL=661

## SOAK_DURATION
~1h (28 rounds with new binary)

## SOAK_VALID_ROUNDS
28

## SOAK_FAIL_ROUNDS
0

## FINAL_RUN1
PASS=646 FAIL=0 IGNORE=15 TOTAL=661

## FINAL_RUN2
PASS=646 FAIL=0 IGNORE=15 TOTAL=661

## FINAL_RUN3
PASS=646 FAIL=0 IGNORE=15 TOTAL=661

## UNRECOVERED_STOP_TIMEOUT_COUNT
0

## UNRECOVERED_DESTROY_RUNNING_COUNT
0

## ORPHAN_TASK_COUNT
0

## ISOLATION_POISON_COUNT
0

## BOARD_PORT
COM7

## USB_SERIAL_CHIP
CP210x

## USB_VID_PID
10C4:EA60

## AUTO_RESET_METHOD
RTS_AUTO (CP210x RTS# -> ESP32-S3 EN)

## BINARY_SHA256
f51309013f10b321428cd04e17de4083c53a11ce97b26487559fec3c5a6d0d94

## ELF_SHA256
b7f703f054e6e0b46d409757dfb6f3a10ee0554e7f97e50b269391a2ad17bf5f

## CHECKPOINT_HASH_FIELDS_POPULATED
YES (last_binary_sha256 and last_elf_sha256 populated in checkpoint.json)

## CODE_REVIEW
- P0: 2 found, 2 fixed (overflow handler DISPATCHING phase, terminal retry cap)
- P1: 4 found, 4 fixed (final_position_confirmed evidence, generation wrap-skip-0, generation double-increment, step_count validation)
- P2: 4 documented (11-site manual invariant, error code enum, fault pattern copy-paste, test coverage gap for final_position_confirmed)

## PHASE7_GROUP_B_ROUND1_5_2_EXIT_GATE
PASS

## READY_FOR_GROUP_B_ROUND2
YES

## READY_FOR_PHASE8
NO
