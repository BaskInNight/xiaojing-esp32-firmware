# Phase 7 Group B Round 2 — Real Service Adapter Architecture Audit

## Skills Used

- `/embedded-systems`: Executor ownership model, service adapter protocol, request lifecycle
- `/esp32`: QueueSet semantics, FreeRTOS lifecycle, IDF 5.5.4 compatibility

---

## 1. Current Executor Ingress Model

**Three queues, no QueueSet.** Fixed priority drain loop (urgent > service > program).

| Queue | Depth | Contents |
|---|---|---|
| `queue_urgent` | 2 | CANCEL/ABORT, FAULT_ACK |
| `queue_service` | 8 | SERVICE_TERMINAL, LOAD_CONFIRM, UNLOAD_CONFIRM |
| `queue_program` | 1 | PROGRAM submission |

Wake mechanism: `xTaskNotify(NOTIFY_WAKE)` after every enqueue.

Drain: `while(got_any)` loop, `else if` chain — one queue per pass iteration. Can starve program queue if service queue has continuous flow.

## 2. Executor ↔ Adapter Interface

Defined in `wash_executor_adapter.h`:

```c
struct wash_executor_adapter {
    adapter_dispatch_result_t (*dispatch_step)(ctx, step, token_buf, token_size);
    adapter_dispatch_result_t (*dispatch_final_reset)(ctx, token_buf, token_size);
    bool (*cancel_step)(ctx, token_buf, token_size);
    void (*stop_all)(ctx);
    void *ctx;
};
```

Only `fake_adapter` exists. **No real adapter code.**

## 3. Two Disconnected Event Systems

| System | Token | ID width | Delivery |
|---|---|---|---|
| Executor | `terminal_token_t` (6 fields) | `uint16_t request_id` | `wash_executor_publish_terminal()` |
| Services | `machine_event_t` (4 fields) | `uint32_t request_id` (machine_request_id_t) | `machine_event_sink_t` callback |

The real adapter must bridge these. Adapter stores active dispatch metadata, translates `machine_event_t` → `publish_terminal()`.

## 4. Step-to-Service Mapping Matrix

| Step Type | Service | Dispatch API | Cancel API | Terminal Event |
|---|---|---|---|---|
| STEP_WAIT_LOAD_CONFIRM | Internal (core) | N/A | N/A | N/A |
| STEP_MOVE_POSITION | position_service | `position_service_move_async()` | `position_service_cancel()` | MACHINE_EVENT_POSITION_DONE |
| STEP_HOME_BL50 | bl50_service | `bl50_service_run_async(HOME)` | `bl50_service_cancel()` | MACHINE_EVENT_BL50_DONE |
| STEP_WATER_IN | water_service | `water_service_fill_async()` | `water_service_cancel()` | MACHINE_EVENT_WATER_DONE |
| STEP_DETERGENT | detergent_service | `detergent_service_run_async()` | `detergent_service_cancel()` | MACHINE_EVENT_DETERGENT_DONE |
| STEP_PULSATOR_WASH | bl50_service | `bl50_service_run_async(PULSATOR)` | `bl50_service_cancel()` | MACHINE_EVENT_BL50_DONE |
| STEP_DRUM_WASH | bl50_service | `bl50_service_run_async(DRUM)` | `bl50_service_cancel()` | MACHINE_EVENT_BL50_DONE |
| STEP_DRAIN | drain_service | `drain_service_run_async()` | `drain_service_cancel()` | MACHINE_EVENT_DRAIN_DONE |
| STEP_SPIN | bl50_service | `bl50_service_run_async(SPIN)` | `bl50_service_cancel()` | MACHINE_EVENT_BL50_DONE |
| STEP_DRY | dry_service | `dry_service_run_async()` | `dry_service_cancel()` | MACHINE_EVENT_DRY_DONE |
| STEP_WAIT_UNLOAD_CONFIRM | Internal (core) | N/A | N/A | N/A |
| STEP_UV | uv_service | `uv_service_run_async()` | `uv_service_cancel()` / `uv_service_skip()` | MACHINE_EVENT_UV_DONE |
| STEP_FINISH | Internal (core) | N/A | N/A | N/A |

Internal steps (WAIT_LOAD, WAIT_UNLOAD, FINISH) are handled by core state machine without adapter dispatch.

## 5. Resource Ownership Diagram

```
Bootstrap owns:
  NVS → Config → StatusStore → Global mutexes → HAL → Safety
  → Position → BL50 → Water → Dry → Drain → Detergent → UV
  → Executor → Button

Executor owns:
  task_handle, queues, mutex, events, join_sem
  Pending/active program (deep copy)
  Terminal delivery state machine

Adapter owns:
  Nothing — borrowed references only
  Active dispatch tracking (token + metadata)

Services own:
  Each service owns its task, resources, stop hook
  Terminal delivered via machine_event_sink_t callback (outside locks)
```

## 6. Event Flow

```
Service dispatch:
  executor task → adapter.dispatch_step() → service_*_async()
  → core enters ACTIVE, arms deadline

Service completion:
  service task → machine_event_sink_t.publish()
  → real_adapter callback → wash_executor_publish_terminal()
  → executor task wakes → core processes SERVICE_TERMINAL

Cancel flow:
  BTN3/urgent → executor queue_urgent → core CANCELING
  → adapter.cancel_step() → service_cancel()
  → cancel ACK terminal → core RESETTING
  → adapter.dispatch_final_reset() → position_service_move_async(DRUM_POS_45)
  → reset ACK terminal → core IDLE, publish program terminal
```

## 7. Rollback Order

Init: NVS → Config → Store → Global → Executor ingress → HAL → Safety → Position → BL50 → Water → Dry → Drain → Detergent → UV → Executor → Button

Deinit: Button stop → Executor quiesce → UV stop → Detergent stop → Drain stop → Dry stop → Water stop → BL50 stop → Position stop → Safety stop → Executor stop/join → Executor destroy → Queue destroy → HAL destroy

## 8. Known Issues

1. Request ID width mismatch: executor `uint16_t` vs services `uint32_t`
2. No QueueSet: drain loop can starve service terminal consumption
3. Button service not in bootstrap — needs MCP handle
4. Old event consumer still exists in bootstrap (placeholder logger)
5. BL50 all actions use same default PWM
6. `stop_all` is a no-op in fake adapter
