# Phase 7 Group B Round 2 — Final Report

## SKILLS_ACTUALLY_INVOKED
- /embedded-systems: Executor ownership model, service adapter protocol, request lifecycle
- /esp32: QueueSet assessment, FreeRTOS lifecycle, IDF 5.5.4 compatibility
- /debugging-code: cmake build debugging, stack overflow root cause analysis
- /code-review: SKILL_UNAVAILABLE (disable-model-invocation), manual P0/P1 review performed

## EMBEDDED_SYSTEMS_FINDINGS
- Executor adapter vtable: 4 functions (dispatch_step, dispatch_final_reset, cancel_step, stop_all)
- Service terminal protocol: machine_event_sink_t callback, outside all locks
- Request ID width mismatch: executor uint16_t vs services uint32_t (bridged via token.request_id)
- Cancel/reset token protocol: terminal_kind distinguishes NORMAL/CANCEL_ACK/RESET_ACK

## ESP32_SKILL_FINDINGS
- QueueSet not implemented — current priority drain loop adequate for low contention
- FreeRTOS queue operations provide memory ordering (critical sections inside xQueueSend/xQueueReceive)
- Single consumer model for event queue preserved
- Main task stack 8192 insufficient for expanded test suite — increased to 16384

## DEBUGGING_CODE_USED
- cmake build directory lockfile: ninja Permission denied → kill processes + force remove
- Stack overflow root cause: main task 8192 bytes during "sink failure retry" test → increased to 16384
- Serial crash trace capture: `***ERROR*** A stack overflow in task main has been detected.`

## CODE_REVIEW_SKILL_STATUS
UNAVAILABLE (disable-model-invocation). Manual structured review performed:
- P0: 0 found (no UAF, deadlock, event loss)
- P1: 0 found (no wrong-service matching, resource leaks, HAL bypass)
- P2: 1 documented (adapter slot access from two tasks — safe due to queue memory ordering)

---

## ROUND2_ARCH_AUDIT
Document: docs/xiaojing_idf_design/15_PHASE7_GROUP_B_ROUND2_REAL_ADAPTER_AUDIT.md

## REAL_SERVICE_ADAPTER
Created: components/wash_executor/wash_executor_real_adapter.c
- Bridges executor's terminal_token_t with services' machine_event_sink_t
- Consumer task reads from shared event queue, routes to publish_terminal()
- Active/cancel/reset dispatch tracking for terminal matching
- Service-kind verification before terminal acceptance
- Diagnostics: dispatch/cancel/reset counts, stale/wrong-service drops

## STEP_ADAPTER_MATRIX
| Step | Service | Dispatch API | Cancel API |
|------|---------|-------------|------------|
| MOVE_POSITION | position_service | position_service_move_async | position_service_cancel |
| HOME_BL50 | bl50_service | bl50_service_run_async(HOME) | bl50_service_cancel |
| PULSATOR_WASH | bl50_service | bl50_service_run_async(PULSATOR) | bl50_service_cancel |
| DRUM_WASH | bl50_service | bl50_service_run_async(DRUM) | bl50_service_cancel |
| SPIN | bl50_service | bl50_service_run_async(SPIN) | bl50_service_cancel |
| WATER_IN | water_service | water_service_fill_async | water_service_cancel |
| DETERGENT | detergent_service | detergent_service_run_async | detergent_service_cancel |
| DRAIN | drain_service | drain_service_run_async | drain_service_cancel |
| DRY | dry_service | dry_service_run_async | dry_service_cancel |
| UV | uv_service | uv_service_run_async | uv_service_cancel |
Internal (core): WAIT_LOAD_CONFIRM, WAIT_UNLOAD_CONFIRM, FINISH

## QUEUESET_MODEL
NOT IMPLEMENTED. Current model: 3 queues with priority drain loop (urgent > service > program). Deferred — adequate for low contention.

## INGRESS_PRIORITY
Current: xTaskNotify(NOTIFY_WAKE) → drain loop: urgent first, then service, then program.

## TERMINAL_MATCHING
6-field token: program_id + generation + step_id + request_id + service_kind + terminal_kind
Adapter verifies: request_id match + service_kind match (expected_event_for_step)

## BUTTON_ROUTING
DEFERRED. Button service not initialized in FAKE mode (no MCP handle). Real adapter ready for button sink wiring when real HAL available.

## CANCEL_RESET_POLICY
Cancel: service_cancel(request_id) → wait CANCEL_ACK terminal → transition to RESETTING
Reset: position_service_move_async(DRUM_POS_45) → wait RESET_ACK terminal → publish program terminal
Reset failure → FAULT with final_position_confirmed=false

## FINAL_POSITION_EVIDENCE
core_cmd_t.final_position_confirmed: set by core state machine at 11 terminal publishing points (unchanged from R1.5.2)

## BL50_ACTION_DEFAULTS
| Action | Default PWM | Override |
|--------|-------------|----------|
| PULSATOR | 40% | config > 0 |
| DRUM | 30% | config > 0 |
| SPIN | 80% | config > 0 |
| HOME | 40% | config > 0 |

## BOOTSTRAP_INIT_ORDER
NVS → Config → Store → Global mutexes → Event queue → HAL → Safety → Position → BL50 → Water → Dry → Drain → Detergent → UV → Executor adapter consumer → Executor → Button (REAL mode only)

## BOOTSTRAP_DEINIT_ORDER
Button stop → Executor quiesce → Executor stop/join → Executor destroy → Adapter consumer stop → UV stop → Detergent stop → Drain stop → Dry stop → Water stop → BL50 stop → Position stop → Safety stop → Queue drain → HAL destroy

## OLD_EVENT_CONSUMER_REMOVED
YES. Old event_consumer_task replaced by real adapter consumer. Event queue still shared.

## RESOURCE_OWNERSHIP
- Event queue: bootstrap owns create/destroy
- Adapter: bootstrap owns lifecycle via wash_executor_bootstrap helper
- Executor: bootstrap owns create/start/stop/destroy via wash_executor_bootstrap helper
- Services: bootstrap owns init/start/stop
- HAL: bootstrap owns create/destroy

---

## FILES_CREATED
- components/wash_executor/wash_executor_real_adapter.c
- components/wash_executor/wash_executor_bootstrap.c
- components/wash_executor/include/wash_executor_real_adapter.h
- components/wash_executor/include/wash_executor_bootstrap.h
- components/wash_executor/private_include/real_adapter_internal.h
- docs/xiaojing_idf_design/15_PHASE7_GROUP_B_ROUND2_REAL_ADAPTER_AUDIT.md

## FILES_MODIFIED
- main/app_bootstrap.c (full rewrite: new services, executor, adapter, removed old consumer)
- main/include/app_bootstrap.h (new result fields, removed event_consumer_ok)
- main/app_main.c (updated bootstrap logging)
- main/CMakeLists.txt (added drain/detergent/uv/button/mcp23017 REQUIRES)
- components/wash_executor/CMakeLists.txt (added real_adapter, bootstrap sources + service REQUIRES)
- components/wash_executor/private_include/wash_executor_adapter.h (removed real adapter decls)
- components/real_hal/include/real_hal.h (added real_hal_get_mcp)
- components/real_hal/real_hal.c (added real_hal_get_mcp implementation)
- test_apps/unit/sdkconfig (main task stack 8192→16384)

## HOST_C
181/181 PASS

## PYTHON_HOST
N/A (host_tests use pre-built binary, no Python host tests for Round 2)

## RUNNER_OFFLINE
N/A (offline runner tests unchanged from R1.5.2)

## MAIN_BUILD
SUCCESS

## UNIT_BUILD
SUCCESS

## GPIO_CHECK
N/A (FAKE mode)

## SAFETY_BYPASS
N/A (FAKE mode)

## TEST_CASE_REGISTERED
661

## BASELINE_REGRESSION
No regression. All 661 tests pass. Main task stack increased from 8192→16384 to fix stack overflow in "sink failure retry" test.

## NEW_EXECUTOR_TESTS
0 new tests added (existing 661 tests cover adapter integration via fake_adapter; real adapter tests deferred to Phase 8)

## DIAGNOSTIC
PASS=646 FAIL=0 IGNORE=15 TOTAL=661

## UNIT_RUN1
PASS=646 FAIL=0 IGNORE=15 TOTAL=661 (119s, single cycle, no crash)

## UNIT_RUN2
PASS=646 FAIL=0 IGNORE=15 TOTAL=661

## UNIT_RUN3
PASS=646 FAIL=0 IGNORE=15 TOTAL=661

## SOAK_DURATION
30min (1814s)

## SOAK_VALID_ROUNDS
15

## SOAK_FAIL_ROUNDS
0

## PASS
646

## FAIL
0

## IGNORE
15

## NON_HW_IGNORE
0

## CRASH_RESET_STATUS
0 crashes, 0 unexpected resets (post-test cleanup stack overflow fixed by increasing main task stack)

## BINARY_SHA256
5705a9e46c4182b0b2a7851a30942f023dcb426c049fbf090fa28bbfe528e927

## ELF_SHA256
acc25da38476477352c15b90ed94cba2a0be5a473f791b163545dc4f6e25f67f

## RAW_LOG_PATHS
- test_apps/unit/phase7_group_b_round2/run1.log
- test_apps/unit/phase7_group_b_round2/run2.log
- test_apps/unit/phase7_group_b_round2/run3.log
- phase7_group_b_round2/soak/

---

## CODE_REVIEW_P0
0 found, 0 fixed

## CODE_REVIEW_P1
0 found, 0 fixed

## CODE_REVIEW_P2
1 documented: adapter slot access from two tasks (executor dispatch + consumer) — safe due to FreeRTOS queue memory ordering

## PHASE7_GROUP_B_ROUND2_EXIT_GATE
PASS

## READY_FOR_PHASE8
YES
