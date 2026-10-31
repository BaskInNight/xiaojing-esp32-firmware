# Phase 8 Round 0 — app_protocol / BLE Architecture Audit

Date: 2026-07-30

## Scope and frozen requirements

Phase 8 implements the phone/mini-program V1 protocol over ESP-IDF 5.5.4
NimBLE. It does not connect or energize real washing-machine peripherals.

- Device name: `JJTP-XIAOJING`
- Primary service: `FFF0`
- Write characteristic: `FFF1`
- Notify characteristic: `FFF2`
- Maximum complete JSON frame: 2048 bytes
- Fragment gap timeout: 3000 ms
- BLE callbacks copy bounded chunks only; they do not parse JSON or execute
  business commands.
- BLE disconnect never cancels a locally running program.
- Reconnect + hello returns ACK, capabilities, and a full status snapshot.
- Legacy `start` and `stop` mapping is controlled by
  `board_config_is_legacy_ble_enabled()`.

## Current repository facts

- ESP-IDF target is ESP32-S3, NimBLE is enabled in `sdkconfig.defaults`.
- No `app_protocol` or `ble_transport` component exists.
- No FFF0/FFF1/FFF2 GATT table, RX reassembly, JSON decoder, TX serializer,
  sequence cache, or reconnect logic exists.
- Phase 7 planner/executor and real service adapter are complete.
- `machine_status_store` exists and is concurrency-safe, but production code
  currently has no status writer.
- Executor has internal `INGRESS_SKIP`, but no public skip API.
- Current board is present on COM7. Unit-test firmware is currently flashed.

## Mandatory preconditions discovered by the audit

1. Executor must publish execution state to `machine_status_store` through a
   partial-update API that preserves sensor/connectivity fields.
2. Executor must expose `wash_executor_skip_current_step()`; app protocol must
   not access internal queues or UV service directly.
3. BLE/app status readers must use `machine_status_store_get()` only and must
   not read service internals.

## Component boundaries

### app_protocol

Responsibilities:

- Pure-C JSON frame parser/encoder and V1 command model.
- Convert allowed `submit_plan` actions into `wash_intent_t`.
- Invoke the existing planner and submit immutable programs to the executor.
- Route load/unload confirmation, UV skip, abort/reset, and fault ACK through
  executor public APIs.
- Maintain a bounded sequence-result cache. A duplicate identical `seq` returns
  the cached ACK and never executes twice.
- Encode ACK/NACK, capability, and full/compact status messages.
- Own the session-scoped monotonically increasing program ID and skip zero.

Forbidden:

- GPIO/MCP/PWM/HAL access.
- Direct service calls.
- Unbounded allocation or unbounded strings.
- Planner implementation internals.

### ble_transport

Responsibilities:

- NimBLE lifecycle, advertising, connection/subscription state, and GATT table.
- Copy FFF1 writes into a bounded chunk queue.
- RX task performs 2048-byte/3000-ms reassembly with JSON string/escape-aware
  brace tracking, then hands complete frames to app_protocol.
- TX task is the sole notifier. Critical ACK/NACK/fault queue is drained before
  normal status traffic. Periodic status uses latest-value coalescing.
- Fragment notifications according to negotiated ATT MTU.
- Continue advertising after disconnect; never change executor state on link
  loss.

Forbidden:

- Planner/executor/service/HAL dependencies.
- JSON parsing or business routing in NimBLE callbacks.

## RX ownership and framing model

- Callback copies at most one bounded ATT write into a fixed queue element.
- Decoder buffer capacity is 2048 bytes plus a trailing NUL.
- Leading whitespace is allowed; the first non-space byte must be `{`.
- Completion is brace depth returning to zero while not inside a JSON string.
- Escapes inside strings are tracked so `"`, `\\`, `{`, and `}` do not corrupt
  depth.
- A frame exceeding 2048 bytes is rejected and the decoder is reset.
- A gap greater than 3000 ms drops the partial frame before accepting new data.
- Malformed complete JSON produces `BAD_JSON`; it never reaches planner/executor.

## Command routing

| Command | Route |
|---|---|
| hello | ACK + capability + full status |
| get_status | ACK + full status |
| start_formal | planner formal -> executor submit |
| start_demo | planner demo -> executor submit |
| submit_plan | bounded JSON -> wash_intent -> planner -> executor submit |
| ack_load | executor confirm_load |
| ack_unload | executor confirm_unload |
| skip_uv | executor skip_current_step; executor decides validity |
| abort_reset | executor urgent abort |
| pause | V1 NACK `NOT_SUPPORTED`; no hidden stop semantics |
| resume | V1 NACK `NOT_SUPPORTED` |
| legacy start | start_formal when compatibility is enabled |
| legacy stop | abort_reset when compatibility is enabled |

Any `heater_on`, `fan_on`, `valve_on`, `pump_on`, GPIO, MCP, PWM, relay, or
raw-motor command is rejected before planner and executor.

## Sequence idempotency

- Fixed-size cache, no dynamic allocation.
- Cache key includes protocol version, sequence, command kind, and a bounded
  frame fingerprint.
- Exact duplicate returns the cached ACK/NACK bytes.
- Same sequence with different content returns `SEQ_CONFLICT` and executes
  nothing.
- Cache is connection-independent for its retention window, preventing a quick
  reconnect from starting the same program twice.

## TX priority and congestion

Priority order:

1. fault / abort ACK / command NACK
2. normal command ACK and hello capability
3. explicit full status
4. periodic compact status

Critical messages use a bounded persistent queue. Periodic status is a
single-slot latest-value mailbox and may be overwritten by newer status. A full
critical queue returns an error to app_protocol; it is never silently reported
as delivered.

## Lifecycle order

Init/start:

`existing bootstrap -> executor -> app_protocol -> ble_transport`

Stop/deinit:

`ble_transport -> app_protocol -> button -> executor quiesce -> services -> executor/adapter -> safety -> event queue -> HAL`

Any stop timeout preserves all downstream resources for retry.

## Test gate

Implement BLE-T01 through BLE-T17 plus failure-path extensions:

- 1/20/21/2048-byte frames, 2049-byte rejection, 3000-ms boundary and wrap.
- Strings containing braces, escaped quotes/backslashes, malformed JSON.
- Duplicate identical seq, same-seq conflict, cache eviction and wrap.
- All V1 routes, BUSY and planner rejection, forbidden raw hardware commands.
- Critical-over-status priority, queue full, disconnect while running, reconnect
  full snapshot.
- GATT name/UUID/properties and callback-copy-only structural checks.
- Start/stop timeout resource preservation, three lifecycle rounds, heap/stack
  diagnostics, no non-hardware IGNORE.

Required verification:

- Main and unit ESP-IDF builds.
- Host C/Python protocol tests.
- Existing Phase 7 regression unchanged.
- COM7 diagnostic and three formal board rounds with zero FAIL.
- Repeated soak after all fixes; no Guru Meditation, watchdog, heap corruption,
  unexpected reset, or task/queue leak.

## Hardware status

- ESP32-S3 board connection: available on COM7.
- Real washing-machine peripherals: not required and must remain disconnected for
  the Fake/logic gate.
- A phone/PC BLE central is required only for the final real GATT discovery and
  reconnect evidence. If unavailable, that item remains explicitly
  `DEFERRED_BLE_CENTRAL`, not silently replaced by a Fake claim.

## Round 0 result

- `PHASE8_ROUND0_AUDIT: PASS`
- `PRODUCTION_CODE_CHANGED: NO`
- `UNRESOLVED_ARCHITECTURE_DECISIONS: 0`
- `READY_FOR_PHASE8_IMPLEMENTATION: YES`
