# Phase 8 app_protocol / BLE Runtime Verification Report

## Scope completed

- Added the pure-C bounded JSON frame assembler and V1 protocol parser/cache.
- Added the FreeRTOS `app_protocol` runtime and planner/executor routing.
- Added the NimBLE `ble_transport` runtime with `JJTP-XIAOJING`, FFF0 service,
  FFF1 write/write-without-response RX, and FFF2 read/notify TX.
- Wired protocol and BLE lifecycle into production bootstrap after executor and
  before button/executor/service teardown.
- Main firmware remains `CONFIG_XIAOJING_OUTPUT_MODE_FAKE=y`; no washing-machine
  peripheral was connected or energized.

## Confirmed defects found and fixed

1. Duplicate envelope fields (`v`, `seq`, `cmd`, `type`) were accepted and could
   create ambiguous protocol interpretation. Duplicate fields now return
   `APP_PARSE_BAD_JSON`; P8-07 contains a regression assertion.
2. Real FFF1 traffic overflowed the 4096-byte `ble_rx` task stack because the
   task owns a 2048-byte assembler and enters planner/executor routing. RX stack
   is now 12288 bytes; the failure was reproduced as an ESP-IDF stack-overflow
   reset before the fix and is absent after it.
3. TX could block on the lower-priority status mailbox, allowing status to
   overtake a newly queued critical ACK. TX now checks both queues non-blocking
   with critical first. Failed critical notifications remain in a task-owned
   single message slot until successful delivery, eliminating the concurrent
   requeue-full loss window without adding a second 1KB stack copy.
4. Unit-only BLE worker barriers used `pdMS_TO_TICKS(1)`, which is zero at the
   configured tick rate and can starve the lower-priority Unity task. Test-hook
   waits now delay for one actual RTOS tick.
5. Lifecycle heap verification sampled immediately after worker DONE bits,
   before the Idle task reclaimed self-deleted task stacks. The test now
   performs bounded heap-quiescence polling and still enforces a 512-byte
   maximum delta.

## Build and host verification

- Main ESP-IDF build: PASS, `xiaojing_idf.bin` = `0xaa070`, 83% app partition free.
- Unit ESP-IDF build: PASS, `xiaojing_unit_test.bin` = `0xab950`, 33% app partition free.
- Host C: 216/216 PASS.
- Python host: 23/23 PASS.
- Serial runner offline tests: 16/16 PASS.
- Registered Unity tests: 716.
- Production `sdkconfig` does not enable `BLE_TRANSPORT_TEST_HOOKS`; unit
  `sdkconfig` enables it explicitly.

## Board regression evidence

- COM7: CP210x, ESP32-S3 N16R8, RTS auto-reset.
- Diagnostic: 701 PASS / 0 FAIL / 15 IGNORE / 716 TOTAL.
- Final formal run 1: 701 / 0 / 15 / 716.
- Final formal run 2: 701 / 0 / 15 / 716.
- Final formal run 3: 701 / 0 / 15 / 716.
- Every run: one app start, one Unity begin, one complete marker, no spontaneous
  reset, and all expected stop-timeout injections recovered.
- All 15 IGNORE are existing `[hw]` tests; non-hardware IGNORE = 0.

## Deterministic runtime-closure tests

- P8-17: fragmented RX passes through the real FreeRTOS RX worker into
  `app_protocol` and the executor.
- P8-18: the eight-entry critical queue saturates deterministically; the ninth
  enqueue times out and increments the drop counter.
- P8-19: three injected notify failures retain the oldest critical message,
  then deliver `A` followed by `B` in FIFO order.
- P8-20: critical output overtakes an already pending status mailbox item.
- P8-21: missing host synchronization times out, rolls back, and permits a
  clean re-init/start.
- P8-22/P8-23: host-stop and worker-exit timeout paths preserve resources and
  succeed on retry.
- P8-24: BLE disconnect while a program is active updates connectivity state
  but does not cancel or replace the active executor request.
- P8-25: twenty BLE lifecycle cycles preserve heap within 512 bytes and record
  nonzero RX/TX worker stack high-water marks.

## Real BLE central evidence

- Windows Bleak central discovered `JJTP-XIAOJING` at
  `DC:B4:D9:13:68:86`; FFF0 was present in advertising.
- GATT discovery verified FFF1 `write` + `write-without-response` and FFF2
  `read` + `notify`.
- Real fragmented notifications verified hello ACK, capabilities, full status,
  `ble_connected=true`, duplicate-seq cached replay, same-seq
  `SEQ_CONFLICT`, get_status ACK/status, and FFF2 read-back.
- 10/10 reconnect cycles passed during development and 3/3 final real-GATT
  cycles passed after all BLE fixes.
- Structured evidence: `real_ble_gatt.json`, `final_ble/run_1.json` through
  `run_3.json`, and `ble_cycles/cycle_1.json` through `cycle_10.json`.

## Runtime-closure bounded soak

- Duration: 1919.6 seconds (about 32 minutes).
- Valid rounds: 13/13.
- Per round: 701 PASS / 0 FAIL / 15 IGNORE / 716 TOTAL.
- Aggregate: 9113 PASS / 0 FAIL / 195 hardware IGNORE.
- Failed rounds: 0; stderr: empty; crash-signature scan: none.
- Evidence: `runtime_closure_soak/checkpoint.json` and 13 independent raw logs.

## Artifact hashes

- Main BIN: `ACF5BD0E237E785D45413A66CF291AB80A6D6BC642D3276CC63F05486137CCDD`
- Main ELF: `BDC6EFD61B7C25789CA5BB6421BFA018D18894485BDC9F6BAE23D18B1643AE06`
- Unit BIN: `6C8441CA4C2BBF78B8443DA7DB73C2F5E9CEC92CF9FCD293FBAAC5B905E187C2`
- Unit ELF: `CB3400105053D05E212D45200C42DA57F45D750BC305AF0C5101C83723380EF7`

## Gate status

PHASE8_PROTOCOL_RUNTIME: PASS

PHASE8_REAL_GATT: PASS

PHASE8_REGRESSION_AND_SOAK: PASS

PHASE8_FULL_EXIT_GATE: PASS

READY_FOR_PHASE9: YES

The board has been restored to the main production image in FAKE output mode.
Boot evidence confirms PSRAM memory test OK, `app_main()` reached, all services
bootstrapped, and `JJTP-XIAOJING` advertising started. The next boundary is
manual mobile mini-program protocol/UX validation or connection of real
washing-machine peripherals; neither is simulated by this automated gate.
