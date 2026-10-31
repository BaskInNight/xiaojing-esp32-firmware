# Phase 7 Round 0.2 — Architecture Audit (Corrected)

> Date: 2026-07-26
> Status: AUDIT_CORRECTED_R0_3
> Baseline: 635 Unity (620 PASS / 0 FAIL / 15 [hw] IGNORE), 3 rounds stable
> Scope: 12 corrections from Round 0.2 review

---

## Skills Invoked

| Skill | Status | Findings |
|-------|--------|----------|
| `/embedded-systems` | INVOKED | Pure function: planner must not hold static state. QueueSet for multi-queue wakeup. Four-phase executor shutdown (quiesce→stop producers→join→destroy). Terminal delivery: position_service uses fire-and-forget (timeout=0), water/bl50/dry/drain/det/uv use pending_terminal with retry. |
| `/esp32` | INVOKED | QueueSet: available in ESP-IDF 5.5.4. FreeRTOSConfig.h already has `#define configUSE_QUEUE_SETS 1` (fixed, not configurable). Do NOT add `CONFIG_FREERTOS_QUEUE_SETS=y` to sdkconfig.defaults — this option does not exist and will cause build warnings. Three queues in one set is within limits. MCP int_sem owned by mcp23017_driver, created/destroyed with handle. mcp23017_read_gpio takes both gpio_a and gpio_b output params. |
| `/code-review` | STRUCTURED_MANUAL | 12 items verified against real source files. No auto-invocation. |

---

## Correction 1: Planner Pure Function

### Problem

Round 0.1 proposed `static uint32_t s_planner_submit_counter` inside `wash_planner_compile()`. This violates:
- Pure function (side effect on static state)
- Reentrant (concurrent calls race on counter)
- Deterministic (same input → different program_id across calls)

### Solution: Option B — Executor Owns program_id

```c
/* wash_planner.h — pure function, no static state */
planner_report_t wash_planner_compile(
    const wash_intent_t *intent,
    const machine_config_t *config,
    uint32_t program_id,           /* provided by caller */
    wash_program_t *out_program    /* fixed-capacity output */
);
```

**Planner**: Sets `out_program->program_id = program_id`. No static mutable state. Reentrant. Pure function: same inputs → identical output bytes.

**Executor**: Generates program_id at submit time:

```c
/* In wash_executor_ctx_t */
static uint32_t s_program_counter;  /* session-scoped, reset on reboot */

esp_err_t wash_executor_submit_program(const wash_program_t *program) {
    lc_lock();
    if (s_state != EXEC_STATE_IDLE || s_pending.valid) {
        lc_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t pid = ++s_program_counter;
    if (pid == 0) pid = 1;  /* skip INVALID */
    s_pending.program = *program;
    s_pending.program.program_id = pid;
    s_pending.valid = true;
    /* enqueue small token */
    executor_msg_t msg = { .type = EXEC_MSG_SUBMIT_TOKEN };
    if (xQueueSend(s_cmd_queue, &msg, 0) != pdTRUE) {
        s_pending.valid = false;  /* rollback reservation */
        lc_unlock();
        return ESP_ERR_TIMEOUT;
    }
    lc_unlock();
    return ESP_OK;
}
```

**program_id linearization point**: `lc_lock()` ensures only one submit succeeds when IDLE + no pending. Counter is session-scoped (reboots restart from 1). UINT32_MAX → 1 (skip 0 = INVALID). Concurrent submit: second caller gets `ESP_ERR_INVALID_STATE`.

**PLAN-T14 update**: Tests `wash_planner_compile()` with fixed program_id input. Asserts identical `steps[]`, `step_count`, `estimated_total_ms`. No internal counter manipulation needed.

---

## Correction 2: Non-existent machine_config.bl50

### Problem

Round 0.1 referenced `config->bl50.home_pwm_percent`, `config->bl50.pulsator_default_pwm`, etc. These fields **do not exist** in `machine_config_t`.

### Verified Facts

```c
/* machine_config_t actual fields (machine_config.h:89-97) */
typedef struct {
    uint32_t schema_version;
    position_params_t position;
    water_params_t water;
    dry_params_t dry;
    detergent_params_t detergent;
    uv_params_t uv;
    drain_params_t drain;
    /* NO bl50_params_t */
} machine_config_t;
```

bl50_service uses `config->position.direction_calibrated` only (bl50_service.c:303). All other parameters come from compile-time defaults in `bl50_fsm.h`:

| Constant | Value | Source |
|----------|-------|--------|
| BL50_DEFAULT_HOME_PWM | 15% | bl50_fsm.h:141 |
| BL50_DEFAULT_HOME_TIMEOUT_MS | 10000ms | bl50_fsm.h:142 |
| BL50_DEFAULT_RAMP_STEP_PERCENT | 5% | bl50_fsm.h:143 |
| BL50_DEFAULT_RAMP_INTERVAL_MS | 100ms | bl50_fsm.h:144 |
| BL50_DEFAULT_REVERSAL_STOP_MS | 500ms | bl50_fsm.h:145 |
| BL50_DEFAULT_PULSATOR_PWM | 40% | bl50_fsm.h:146 |
| BL50_DEFAULT_PULSATOR_DIR_MS | 5000ms | bl50_fsm.h:147 |
| BL50_DEFAULT_DRUM_PWM | 30% | bl50_fsm.h:148 |
| BL50_DEFAULT_DRUM_DIR_MS | 8000ms | bl50_fsm.h:149 |
| BL50_DEFAULT_SPIN_PWM | 80% | bl50_fsm.h:150 |
| BL50_DEFAULT_MAX_ACTION_MS | 1800000ms (30min) | bl50_fsm.h:151 |

### Solution: Executor Passes 0, FSM Defaults Apply

`bl50_request_t.target_pwm_percent` and `bl50_request_t.direction_interval_ms`: executor passes 0. FSM falls back to compile-time defaults (bl50_fsm.c:231: `ctx->target_pwm_percent > 0 ? ctx->target_pwm_percent : 40`).

**No machine_config schema change needed for Phase 7.** The executor uses `BL50_DEFAULT_*` constants directly or passes 0. Phase 10+ can add `bl50_params_t` to machine_config if runtime tuning is needed (schema 3→4, NVS migration, validate, host tests).

**BL50_ACTION_SPECIFIC_DEFAULT_FIX: DEFERRED_TO_PHASE7_GROUP_B**

Current `bl50_fsm.c` behavior: for non-HOME actions with `target_pwm_percent=0`, the FSM actually falls back to 40% (`BL50_DEFAULT_PULSATOR_PWM`), regardless of action type. `BL50_DEFAULT_DRUM_PWM` (30%) and `BL50_DEFAULT_SPIN_PWM` (80%) are defined but NOT used in the fallback path. This means DRUM and SPIN actions with `target_pwm_percent=0` both get 40% PWM instead of their action-specific defaults. This will be fixed in Phase 7 Group B when the executor adapter is implemented.

### Corrected Service Adapter (Section 13)

All bl50 steps now use:

```c
bl50_request_t req = {
    .request_id = step_rid,
    .action = BL50_ACTION_PULSATOR, /* or DRUM, SPIN, HOME */
    .intensity = step->intensity,
    .duration_ms = step->duration_ms,
    .target_pwm_percent = 0,         /* 0 = use FSM default */
    .direction_interval_ms = 0,      /* 0 = use FSM default */
};
```

Executor-level overrides (if needed) come from `wash_executor_config_t`, not from non-existent machine_config fields.

---

## Correction 3: rounds Semantics

### Problem

`bl50_service_run_async()` does NOT accept `rounds`. `bl50_request_t` has no `rounds` field. "rounds * direction_interval_ms" has no production code basis.

### Solution: Planner Expands rounds into Duration

The planner is responsible for converting `wash_action_t.rounds` into `wash_step_t.duration_ms`:

```c
/* In wash_planner_compile() */
case WASH_ACTION_PULSATOR_WASH:
    step.duration_ms = action->rounds * action->duration_ms;
    /* Overflow check */
    if (action->rounds > 0 && action->duration_ms > UINT32_MAX / action->rounds) {
        report->result = PLAN_RESULT_REJECTED;
        report->reject_reason = PLAN_ERROR_DURATION_OVERFLOW;
        return *report;
    }
    step.type = STEP_PULSATOR_WASH;
    step.rounds = 0;  /* consumed: duration already reflects total */
    break;
```

**request_id**: One per step (one per BL50 request). No multi-round request_id generation.

**duration_ms**: Total duration = rounds × per-round-duration. Passed to bl50_service as single `duration_ms`.

**BL50 direction interval**: BL50 FSM internally toggles direction at `direction_interval_ms` intervals for the entire `duration_ms`. One request, one duration, automatic direction changes within.

**cancel**: Cancel during any direction cycle. Service handles it, publishes CANCELED terminal once.

**progress**: Executor updates `elapsed_ms / step.duration_ms` for this step.

**64-step limit**: Each wash_action with rounds > 0 becomes ONE wash_step_t. No step expansion needed. The 64-step limit is on distinct steps, not on direction cycles within a step.

---

## Correction 4: Executor Wakeup Model

### Problem

Round 0.1 three-queue loop: urgent(non-blocking) → cmd(non-blocking) → svc(blocking). If executor blocks on `xQueueReceive(s_svc_event_queue)`, BTN3 in urgent queue won't wake it until svc timeout.

### Solution: FreeRTOS QueueSet

**ESP-IDF 5.5.4 supports QueueSet** (`configUSE_QUEUE_SETS`). Already enabled by default in ESP-IDF 5.5.4's `components/freertos/config/include/freertos/FreeRTOSConfig.h` (`#define configUSE_QUEUE_SETS 1`). Do NOT add any sdkconfig option for this — no `CONFIG_FREERTOS_QUEUE_SETS` Kconfig entry exists.

```c
/* Executor init */
s_urgent_queue = xQueueCreate(4, sizeof(button_sink_event_t));
s_cmd_queue = xQueueCreate(16, sizeof(executor_msg_t));
s_svc_event_queue = xQueueCreate(8, sizeof(machine_event_t));

s_queue_set = xQueueCreateSet(4 + 16 + 8);  /* sum of all queue depths */
xQueueAddToSet(s_urgent_queue, s_queue_set);
xQueueAddToSet(s_cmd_queue, s_queue_set);
xQueueAddToSet(s_svc_event_queue, s_queue_set);
```

```c
/* Executor task loop */
while (!stop_requested) {
    /* Block until ANY queue has data */
    QueueHandle_t active = xQueueSelectFromSet(s_queue_set, pdMS_TO_TICKS(200));
    if (!active) continue;  /* timeout: check stop_requested */

    /* Priority drain: urgent first, then cmd, then svc */
    /* Urgent: drain all */
    button_sink_event_t urgent;
    while (xQueueReceive(s_urgent_queue, &urgent, 0) == pdTRUE) {
        handle_btn3_urgent(&urgent);
    }
    /* Cmd: drain all */
    executor_msg_t cmd;
    while (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
        handle_command(&cmd);
    }
    /* Svc: drain all */
    machine_event_t ev;
    while (xQueueReceive(s_svc_event_queue, &ev, 0) == pdTRUE) {
        handle_service_event(&ev);
    }
}
```

**Max BTN3 dispatch latency**: `xQueueSelectFromSet` returns immediately when urgent queue is non-empty. If executor is blocked in `xQueueSelectFromSet`, any queue arrival wakes it. Latency = context switch time (~10-50μs on ESP32-S3).

**Urgent priority**: After wakeup, urgent queue is always drained first (before cmd and svc). Even if wakeup was triggered by a service event.

**Starvation protection**: Each wakeup drains ALL queues completely. No partial processing. Service events cannot starve urgent events because urgent is checked first every iteration.

**Queue full behavior**: Same as before (services retry via pending_terminal_t). QueueSet doesn't change producer behavior.

**Stop wakeup**: `xQueueSelectFromSet` has a 200ms timeout. Stop sets `stop_requested = true` and the task checks it after timeout. Alternatively, send a sentinel message to `s_cmd_queue` for immediate wakeup.

**QueueSet limitation**: A queue can only belong to one QueueSet. Since no other task reads from these queues, this is not a problem.

**Fake test method**: Tests create the queues and QueueSet, send messages to individual queues, verify executor processes them in priority order.

---

## Correction 5: Executor Quiesce Model (Three-Phase Stop)

### Problem

Round 0.1 two-phase stop (quiesce + destroy): after executor task joins, services still running may try to publish terminals to the now-unread queue, potentially causing service stop to hang on terminal publish failure.

### Solution: Four-Phase Shutdown

```c
/* Phase 1: Quiesce — reject new work, cancel active, but KEEP TASK RUNNING */
esp_err_t wash_executor_quiesce(void) {
    lc_lock();
    s_quiesced = true;   /* reject new submit/command */
    if (s_active_step.step_phase == STEP_PHASE_WAITING) {
        s_active_step.step_phase = STEP_PHASE_CANCELING;
        service_cancel(s_active_step.active_rid);
    }
    lc_unlock();
    /* Task continues running and draining queues */
    return ESP_OK;
}

/* Phase 2: Stop producers (button, then services) */
// Called externally by bootstrap deinit:
//   button_service_stop()
//   uv/det/drain/dry/water/bl50/position_service_stop()
// During this phase, executor task still reads s_svc_event_queue
// Services publish terminals → executor consumes → no deadlock

/* Phase 3: Join task — all producers stopped, safe to stop reading */
esp_err_t wash_executor_stop(void) {
    atomic_store(&s_stop_requested, true);
    /* Wake executor from QueueSet block */
    executor_msg_t wake = { .type = EXEC_MSG_STOP_TOKEN };
    xQueueSend(s_cmd_queue, &wake, pdMS_TO_TICKS(100));
    /* Wait for task to exit */
    if (xSemaphoreTake(s_task_done_sem, pdMS_TO_TICKS(3000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;  /* lifecycle stays STOPPING, can retry */
    }
    s_task_handle = NULL;
    s_lifecycle = LC_STOPPED;
    return ESP_OK;
}

/* Phase 4: Destroy — all references gone, safe to free */
void wash_executor_destroy(void) {
    if (s_queue_set) { vQueueDelete(s_queue_set); s_queue_set = NULL; }
    if (s_urgent_queue) { vQueueDelete(s_urgent_queue); s_urgent_queue = NULL; }
    if (s_cmd_queue) { vQueueDelete(s_cmd_queue); s_cmd_queue = NULL; }
    if (s_svc_event_queue) { vQueueDelete(s_svc_event_queue); s_svc_event_queue = NULL; }
    if (s_lc_mtx) { vSemaphoreDelete(s_lc_mtx); s_lc_mtx = NULL; }
    s_lifecycle = LC_UNINITIALIZED;
}
```

### Bootstrap Deinit Order (Corrected)

```
1. button_service_stop()              // external producer stops first
2. wash_executor_quiesce()            // reject new work, cancel active
3. uv_service_stop()                  // executor still draining
4. detergent_service_stop()           // executor still draining
5. drain_service_stop()               // executor still draining
6. dry_service_stop()                 // executor still draining
7. water_service_stop()               // executor still draining
8. bl50_service_stop()                // executor still draining
9. position_service_stop()            // executor still draining
10. wash_executor_stop()              // join task (all producers stopped)
11. wash_executor_destroy()           // delete queues/mutex
12. safety_manager_stop()
13. destroy_event_queue()             // old bootstrap event queue
14. destroy_hal()
```

**Why no deadlock**: During steps 3-9, executor task is still running and reading `s_svc_event_queue`. Services publish terminals → executor consumes → service stop succeeds.

**Why no UAF**: Queues are deleted in step 11, after all producers (step 1) and consumers (step 10) are stopped.

**Stop failure rollback**: If any service stop fails (step 3-9), halt rollback, preserve remaining resources. Executor stays quiesced (not joined). Subsequent deinit can retry.

---

## Correction 6: Button HAL Adapter

### Problem

Round 0.1 adapter missed `wait_button_interrupt()`. Without it, button_service degrades to permanent polling.

### Corrected HAL Adapter

```c
/* Added to xiaojing_hal_t */
typedef struct {
    uint8_t gpio_a;
    uint8_t gpio_b;
} button_gpio_snapshot_t;

typedef struct {
    /* ... existing fields ... */
    /* Button input adapter */
    esp_err_t (*configure_button_interrupts)(uint8_t pin_mask, bool enable_pullup);
    esp_err_t (*disable_button_interrupts)(uint8_t pin_mask);
    esp_err_t (*wait_button_interrupt)(uint32_t timeout_ms);
    esp_err_t (*read_button_gpio)(button_gpio_snapshot_t *out);
} xiaojing_hal_t;
```

**REAL implementation**:
- `configure_button_interrupts` → `mcp23017_configure_button_interrupts(ctx->mcp, pin_mask, enable_pullup)`
- `disable_button_interrupts` → `mcp23017_disable_button_interrupts(ctx->mcp, pin_mask)`
- `wait_button_interrupt` → `mcp23017_wait_interrupt(ctx->mcp, timeout_ms)` (uses int_sem + GPIO13 ISR)
- `read_button_gpio` → `mcp23017_read_gpio(ctx->mcp, &out->gpio_a, &out->gpio_b)`

**MCP resource ownership**:
- `int_sem`: Created in `mcp23017_init()`, destroyed in `mcp23017_destroy()`. Owned by MCP handle.
- GPIO13 ISR: Registered in `mcp23017_init()`. ISR gives `int_sem`.
- `real_hal_create()` creates MCP (and thus int_sem). `real_hal_destroy()` destroys MCP (and thus int_sem).
- button_service borrows HAL pointer. Never owns MCP resources.

**FAKE implementation**:
- `configure_button_interrupts` → no-op, return ESP_OK
- `disable_button_interrupts` → no-op, return ESP_OK
- `wait_button_interrupt(timeout_ms)` → wait on `fake_btn_event_sem` (test-injectable binary semaphore) with timeout. On timeout, return ESP_ERR_TIMEOUT. On signal, return ESP_OK.
- `read_button_gpio(out)` → return injected `fake_btn_gpio_snapshot`

**Polling fallback**: If `wait_button_interrupt` returns `ESP_ERR_INVALID_STATE` (no interrupt configured), button_service falls back to `vTaskDelay(poll_interval_ms)` + `read_button_gpio`. This is the existing adaptive polling pattern.

**Fake testing**: Test sets `fake_btn_gpio_snapshot.gpio_a = 0x60` (BTN1 pressed), gives `fake_btn_event_sem`, button_service wakes and processes the press.

---

## Correction 7: Invalid Enum Must Reject

### Problem

Round 0.1 pseudo-code used `continue` (drop action) for invalid enum. But title says "reject". Inconsistent.

### Solution: Atomic Rejection

```c
/* In wash_planner_compile() */
for (size_t i = 0; i < intent->action_count; i++) {
    const wash_action_t *action = &intent->actions[i];
    if (action->type > WASH_ACTION_UV) {
        report->result = PLAN_RESULT_REJECTED;
        report->reject_reason = PLAN_ERROR_INVALID_ACTION;
        report->invalid_action_index = (uint16_t)i;
        memset(out_program, 0, sizeof(*out_program));  /* zero output */
        return *report;
    }
    /* ... normal processing ... */
}
```

**Contract**: On `PLAN_RESULT_REJECTED`:
- `out_program` is zeroed (not partially filled)
- `report->reject_reason` indicates the error
- `report->invalid_action_index` indicates which action failed
- Caller must NOT use `out_program`

**enabled=false** is the only legitimate skip. It does NOT set `reject_reason`.

**PLAN-T15**: Input has actions [0]=valid, [1]=invalid_enum(99), [2]=valid. Assert: REJECTED, `invalid_action_index==1`, `out_program` is zeroed, `step_count==0`.

---

## Correction 8: Submit Program Ownership

### Problem

`EXEC_MSG_SUBMIT_PROGRAM` had no actual payload definition. Risk: storing caller pointer, concurrent submit, queue full.

### Solution: Pre-validated Copy + Small Token

```c
typedef struct {
    bool valid;
    uint32_t generation;           /* program_id assigned at submit */
    wash_program_t program;        /* full copy (fixed 64 steps) */
} exec_pending_t;

/* In executor context */
exec_pending_t s_pending;          /* executor-owned storage */

/* Submit API (called by BLE/voice/button) */
esp_err_t wash_executor_submit_program(const wash_program_t *caller_program) {
    if (!caller_program) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_lc_mtx, pdMS_TO_TICKS(10)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    if (s_state != EXEC_STATE_IDLE || s_pending.valid) {
        xSemaphoreGive(s_lc_mtx);
        return ESP_ERR_INVALID_STATE;  /* BUSY */
    }
    /* Copy to executor-owned storage */
    s_pending.program = *caller_program;
    s_pending.generation = ++s_program_counter;
    if (s_pending.generation == 0) s_pending.generation = 1;
    s_pending.program.program_id = s_pending.generation;
    s_pending.valid = true;
    /* Enqueue small token (not the whole program) */
    executor_msg_t msg = { .type = EXEC_MSG_SUBMIT_TOKEN };
    if (xQueueSend(s_cmd_queue, &msg, 0) != pdTRUE) {
        s_pending.valid = false;  /* rollback */
        xSemaphoreGive(s_lc_mtx);
        return ESP_ERR_TIMEOUT;
    }
    xSemaphoreGive(s_lc_mtx);
    return ESP_OK;
    /* Caller's buffer can be freed immediately after return */
}
```

**Linearization point**: `s_lc_mtx` hold + IDLE check + pending check. Exactly one submit succeeds.

**Enqueue failure**: Atomic rollback of `s_pending.valid`.

**Caller buffer lifetime**: Function copies entire program into `s_pending.program` before returning. Caller can free/reuse buffer immediately.

**Generation**: Monotonic counter, session-scoped. Reboots restart from 1.

---

## Correction 9: UV / BTN3 State Semantics

### Unified Rules

| State | BTN1 | BTN2 | BTN3 |
|-------|------|------|------|
| IDLE | start Formal | start Demo | **ignored** |
| WAIT_LOAD | confirm | confirm | **abort_reset** |
| RUNNING | ignored | ignored | **abort_reset** |
| WAIT_UNLOAD | confirm→UV/IDLE | confirm→UV/IDLE | **abort_reset** |
| UV | **skip_uv** | **skip_uv** | **abort_reset** |
| RESETTING | ignored | ignored | **ignored** (idempotent) |
| FAULT | **ack→IDLE** | **ack→IDLE** | **ack→IDLE** |

**UV skip (BTN1/BTN2)**:
1. `uv_service_skip(active_rid)` — non-emergency, graceful
2. Wait for `MACHINE_EVENT_UV_DONE` with `result=SERVICE_RESULT_SKIPPED`
3. UV output confirmed OFF by service
4. Transition to IDLE

**UV BTN3 (abort_reset)**:
1. `uv_service_emergency_stop()` — immediate OFF, may not get SKIPPED terminal
2. Full abort_reset sequence (45° + BL50 home)
3. Transition to IDLE or FAULT

**UV timeout**:
1. UV service has internal `max_duration_ms` timeout
2. Service publishes `MACHINE_EVENT_UV_DONE` with `result=SERVICE_RESULT_TIMEOUT`
3. Executor receives terminal, UV output already OFF
4. Transition to IDLE (timeout is not a fault for UV)

**UV OFF failure**:
1. UV service can't turn off output → `result=SERVICE_RESULT_FAULT`
2. Executor receives FAULT terminal
3. Transition to EXEC_STATE_FAULT

**FAULT ack (BTN1/BTN2/BTN3)**:
1. Check `safety_manager_request_fault_clear()`
2. If clear succeeds: transition to IDLE
3. If clear fails (conditions not met): stay in FAULT, log reason

**BTN3 during FAULT**: Same as ack (NOT another abort_reset). The system is already shut down. Just try to clear fault.

---

## Correction 10: Formal and Demo Golden Sequences

### Formal Golden Sequence

```
Step  0: STEP_WAIT_LOAD_CONFIRM     @ any     dur=0       timeout=config
Step  1: STEP_HOME_BL50             @ 45      dur=0       timeout=10000
Step  2: STEP_MOVE_POSITION         @ 0       dur=0       timeout=config
Step  3: STEP_WATER_IN              @ 0       dur=0       timeout=config
Step  4: STEP_DETERGENT             @ 0       dur=config  timeout=config
Step  5: STEP_PULSATOR_WASH         @ 0       dur=config  timeout=config
Step  6: STEP_MOVE_POSITION         @ 180     dur=0       timeout=config
Step  7: STEP_DRAIN                 @ 180     dur=config  timeout=config
Step  8: STEP_SPIN                  @ 180     dur=config  timeout=config
Step  9: STEP_MOVE_POSITION         @ 270     dur=0       timeout=config
Step 10: STEP_DRY                   @ 270     dur=config  timeout=config
Step 11: STEP_MOVE_POSITION         @ 45      dur=0       timeout=config
Step 12: STEP_WAIT_UNLOAD_CONFIRM   @ any     dur=0       timeout=config
Step 13: STEP_MOVE_POSITION         @ 0       dur=0       timeout=config
Step 14: STEP_UV                    @ 0       dur=config  timeout=config
Step 15: STEP_MOVE_POSITION         @ 45      dur=0       timeout=config
Step 16: STEP_FINISH                @ 45      dur=0       timeout=0
```

**UV after unload**: User has removed laundry. UV operates with drum at 0°. After UV, return to 45° (resting position).

**UV disabled** (`allow_uv=false`): Steps 13-15 are omitted. Step 12 (WAIT_UNLOAD) is followed directly by MOVE 45 → FINISH.

### Demo Golden Sequence

```
Step 0: STEP_HOME_BL50             @ 45      dur=0       timeout=10000
Step 1: STEP_MOVE_POSITION         @ 0       dur=0       timeout=config
Step 2: STEP_WATER_IN              @ 0       dur=0       timeout=config
Step 3: STEP_DETERGENT             @ 0       dur=short   timeout=config
Step 4: STEP_PULSATOR_WASH         @ 0       dur=short   timeout=config
Step 5: STEP_MOVE_POSITION         @ 180     dur=0       timeout=config
Step 6: STEP_DRAIN                 @ 180     dur=config  timeout=config
Step 7: STEP_SPIN                  @ 180     dur=short   timeout=config
Step 8: STEP_MOVE_POSITION         @ 270     dur=0       timeout=config
Step 9: STEP_DRY                   @ 270     dur=short   timeout=config
Step 10: STEP_MOVE_POSITION        @ 45      dur=0       timeout=config
Step 11: STEP_FINISH               @ 45      dur=0       timeout=0
```

**Demo differences**: No WAIT_LOAD, no WAIT_UNLOAD, no UV step. All durations short (budget <= 180,000ms total for automatic actions).

### Final Position Invariant

**After every program completes (normal, cancel, or abort_reset)**: Drum position = DRUM_POS_45.

This is enforced by:
1. Planner always ends with MOVE 45 before FINISH
2. abort_reset includes MOVE 45 + BL50 HOME
3. Executor verifies position after reset before entering IDLE

---

## Correction 11: Profile Parameter Sources

### machine_config_t Fields Used by Planner

| Parameter | Source | Field Path |
|-----------|--------|------------|
| Position move timeout | machine_config | `config->position.move_timeout_ms` |
| Water batch target | machine_config | `config->water.source_batch_target_pulses` |
| Water total timeout | machine_config | `config->water.total_inlet_timeout_ms` |
| Dry max total | machine_config | `config->dry.max_total_ms` |
| Detergent duration | machine_config | `config->detergent.formal_duration_ms` or `demo_duration_ms` |
| UV default duration | machine_config | `config->uv.default_duration_ms` |
| Drain max duration | machine_config | `config->drain.max_duration_ms` |
| BL50 direction calibrated | machine_config | `config->position.direction_calibrated` |

### Planner Policy Constants (Compile-Time, Not in machine_config)

These are planner-level policy decisions, NOT hardware calibration:

```c
/* wash_planner_policy.h — compile-time defaults */
#define PLANNER_FORMAL_WASH_ROUNDS          3
#define PLANNER_FORMAL_WASH_DURATION_MS     30000    /* per round */
#define PLANNER_FORMAL_WASH_INTENSITY       WASH_INTENSITY_NORMAL
#define PLANNER_FORMAL_SPIN_DURATION_MS     120000
#define PLANNER_FORMAL_SPIN_INTENSITY       WASH_INTENSITY_STRONG

#define PLANNER_DEMO_WASH_ROUNDS            1
#define PLANNER_DEMO_WASH_DURATION_MS       10000
#define PLANNER_DEMO_WASH_INTENSITY         WASH_INTENSITY_LOW
#define PLANNER_DEMO_SPIN_DURATION_MS       15000
#define PLANNER_DEMO_SPIN_INTENSITY         WASH_INTENSITY_NORMAL
#define PLANNER_DEMO_DRY_DURATION_MS        10000
#define PLANNER_DEMO_DETERGENT_DURATION_MS  3000

#define PLANNER_TOTAL_DURATION_MAX_MS       3600000   /* 1 hour */
#define PLANNER_DRY_CUMULATIVE_MAX_MS       1800000   /* 30 min */
#define PLANNER_DEMO_AUTO_BUDGET_MS         180000    /* 3 min */

#define PLANNER_STEP_TIMEOUT_DEFAULT_MS     300000    /* 5 min per step */
#define PLANNER_LOAD_UNLOAD_TIMEOUT_MS      0         /* 0 = no timeout */
```

**Validation boundaries** (planner rejects if config values are out of machine_config_validate range):
- All `*_ms` fields: > 0, finite
- Detergent `ml_per_second`: >= 0, finite
- Water `pulses_per_liter`: >= 0, finite

---

## Correction 12: Service Terminal Delivery Matrix

### Verified from Source Code

| Service | Terminal Delivery | Retry | Max Retries | Stop Pending Behavior | Permanent Failure |
|---------|------------------|-------|-------------|----------------------|-------------------|
| **position_service** | `event_sink.publish(&ev, 0, ctx)` — **fire-and-forget**, timeout=0 | **NO RETRY** | 0 | Published once in task loop, after that task exits | Lost if queue full. Position service does not use pending_terminal. |
| **water_service** | `pending_terminal_t` + `try_publish_pending()` | **YES, unbounded** per tick | Every tick until success | Task stays alive until pending published or stop timeout | Service stays RUNNING, stop will timeout |
| **bl50_service** | Same pattern as water (pending_terminal) | **YES, bounded per tick** | Retry every tick | Same as water | Same as water |
| **dry_service** | Same pattern as water | **YES** | Retry every tick | Same | Same |
| **drain_service** | Same pattern as water | **YES** | Retry every tick | Same | Same |
| **detergent_service** | Same pattern as water | **YES** | Retry every tick | Same | Same |
| **uv_service** | Same pattern as water | **YES** | Retry every tick | Same | Same |

### Key Difference: position_service

Position service publishes terminal with `timeout=0` (non-blocking). If the queue is full, the event is **silently lost**. Position service does NOT have `pending_terminal_t`. This is a known architectural decision: position service is the highest-priority service and its terminal should rarely be lost.

**Risk**: If executor queue fills up (e.g., executor stuck), position terminal is lost. Executor has per-step timeout as fallback.

### Implications for Executor Quiesce

During quiesce (Phase 2: services stopping while executor drains):
- water/bl50/dry/drain/det/uv: Pending terminals retry each tick. Executor task reads queue → terminal delivered → service exits.
- position: Terminal published once. If queue has space (executor is draining), it succeeds. If queue is full (unlikely since executor just drained), it's lost. Executor timeout catches this.

**No bootstrap deinit deadlock**: Executor task is running during service stops. Queue is being drained. Services' pending terminals eventually succeed.

---

## Revised Test Plan Additions

### Planner (Additional)

| ID | Assertion | Category |
|----|-----------|----------|
| **PLAN-T24** | `rg` scan: 0 `static` mutable variables in planner source | Pure function |
| **PLAN-T25** | Two concurrent `wash_planner_compile()` → both succeed, no crash | Concurrent |
| **PLAN-T26** | External program_id=42 → `out_program->program_id==42` | External ID |
| **PLAN-T27** | Invalid enum type=99 at index 3 → REJECTED, index=3, out zeroed | Atomic rejection |
| **PLAN-T28** | rounds=1000 × duration=5000000 → REJECTED (overflow) | Rounds overflow |
| **PLAN-T29** | rounds=3 × duration=30000 → step.duration_ms=90000 | Exact expansion |
| **PLAN-T30** | No `machine_config.h` bl50 reference → `rg` returns 0 | Config guard |
| **PLAN-T31** | Formal: exact 17-step golden sequence | Golden |
| **PLAN-T32** | Demo: exact 12-step golden sequence | Golden |
| **PLAN-T33** | UV disabled: no MOVE 0/UV/MOVE 45 steps | UV position |
| **PLAN-T34** | Final step is MOVE 45 or FINISH at 45 | Final position |
| **PLAN-T35** | REJECTED → out_program is zeroed | No partial output |

### Executor (Additional)

| ID | Assertion | Category |
|----|-----------|----------|
| **EXE-T41** | QueueSet wakeup: send to urgent while blocked on svc → <5ms dispatch | Wakeup |
| **EXE-T42** | Blocked on QueueSet, BTN3 arrives → immediate wakeup | Wakeup |
| **EXE-T43** | Urgent flood (10 rapid) doesn't starve svc terminal drain | Starvation |
| **EXE-T44** | Quiesce: service publishes terminal → executor still consumes | Quiesce drain |
| **EXE-T45** | Service stop during quiesce: terminal publish not deadlocked | No deadlock |
| **EXE-T46** | All producers stopped → executor join succeeds | Join safety |
| **EXE-T47** | After join: send to queue → ESP_ERR_INVALID_STATE (queue deleted) | Post-join safety |
| **EXE-T48** | Concurrent submit: one ESP_OK, one INVALID_STATE | Linearization |
| **EXE-T49** | Submit enqueue fail → pending rolled back | Rollback |
| **EXE-T50** | Caller buffer freed after submit return | Buffer lifetime |
| **EXE-T51** | UV skip: BTN1 → uv_service_skip → SKIPPED terminal → IDLE | UV skip |
| **EXE-T52** | UV OFF failure → FAULT terminal → EXEC_STATE_FAULT | UV fault |
| **EXE-T53** | BTN3 during UV → abort_reset (not skip) | UV BTN3 |
| **EXE-T54** | Terminal sink permanent failure → executor step timeout → FAULT | Sink failure |
| **EXE-T55** | Button interrupt + polling fallback both covered | Button adapter |
| **EXE-T56** | FAULT ack: safety_manager clear succeeds → IDLE | Fault clear |
| **EXE-T57** | FAULT ack: safety_manager clear fails → stay FAULT | Fault clear fail |

---

## Final Report

```text
PHASE7_ROUND0_2_AUDIT_CORRECTED: YES

PLANNER_PURE_FUNCTION:
  wash_planner_compile() has zero static mutable state.
  Reentrant, deterministic, thread-safe.
  rg scan test PLAN-T24 enforces this.

PROGRAM_ID_OWNER:
  Executor owns program_id via static counter in wash_executor_submit_program().
  Planner receives program_id as parameter.
  Linearization: lc_mtx + IDLE + no-pending check.
  Session-scoped, reboot resets to 1. UINT32_MAX→1, 0=INVALID.

BL50_PARAMETER_SOURCE:
  machine_config has NO bl50_params_t.
  bl50_service uses compile-time BL50_DEFAULT_* constants from bl50_fsm.h.
  Executor passes target_pwm_percent=0, direction_interval_ms=0 (FSM defaults apply).
  No machine_config schema change needed for Phase 7.

ROUNDS_EXECUTION_MODEL:
  Planner expands rounds into duration_ms (rounds × per_round_duration).
  Overflow-safe multiplication with REJECTED on overflow.
  One wash_step_t per action, one bl50_request per step.
  BL50 FSM handles direction cycling internally within duration.

EXECUTOR_WAKEUP_MODEL:
  FreeRTOS QueueSet: s_urgent_queue + s_cmd_queue + s_svc_event_queue.
  configUSE_QUEUE_SETS is already 1 in ESP-IDF 5.5.4 FreeRTOSConfig.h (fixed).
  Do NOT add CONFIG_FREERTOS_QUEUE_SETS=y to sdkconfig.defaults.
  xQueueSelectFromSet blocks until any queue has data.
  After wakeup: drain urgent (priority) → cmd → svc (all non-blocking).

MAX_URGENT_DISPATCH_LATENCY_MS:
  <1ms (QueueSet wakes immediately on queue send, context switch ~10-50μs).
  If executor processing a batch: urgent still checked first after current batch.

EXECUTOR_QUIESCE_MODEL:
  Four-phase: quiesce (reject new, cancel active, task runs) → stop producers
  → join task → destroy ingress.
  Executor task runs during service stops, draining terminals.
  No deadlock: executor consumes while services publish.

SERVICE_TERMINAL_DELIVERY_MATRIX:
  position: fire-and-forget (timeout=0), no retry, may be lost if queue full.
  water/bl50/dry/drain/det/uv: pending_terminal_t, retry every tick, unbounded.
  Position loss caught by executor per-step timeout.

BUTTON_INTERRUPT_HAL_ADAPTER:
  Four functions: configure/disable/wait_button_interrupt + read_button_gpio.
  REAL: delegates to mcp23017_driver (MCP handle owned by real_hal).
  FAKE: injectable semaphore + snapshot, no busy loop.
  Polling fallback: if wait returns INVALID_STATE, vTaskDelay + read_button_gpio.
  MCP int_sem created/destroyed with mcp23017_handle_t (HAL owns lifecycle).

SUBMIT_PROGRAM_OWNERSHIP:
  Pre-validated copy into executor-owned s_pending.program under lc_mtx.
  Small token enqueued (not whole program). Enqueue fail → atomic rollback.
  Caller buffer safe to free immediately after return.

INVALID_ACTION_POLICY:
  PLAN_RESULT_REJECTED with PLAN_ERROR_INVALID_ACTION.
  out_program zeroed. reject_reason + invalid_action_index in report.
  No partial program produced. enabled=false is the only legitimate skip.

UV_BUTTON_SEMANTICS:
  BTN1/BTN2 in UV: uv_service_skip() → wait SKIPPED terminal → IDLE.
  BTN3 in UV: uv_service_emergency_stop() → abort_reset (not skip).
  UV timeout: service publishes TIMEOUT → executor → IDLE.
  UV OFF failure: FAULT terminal → EXEC_STATE_FAULT.
  FAULT ack: safety_manager clear attempt → success→IDLE, fail→stay FAULT.
  BTN3 in FAULT: same as ack (not another abort_reset).

FORMAL_GOLDEN_SEQUENCE:
  17 steps: WAIT_LOAD→HOME→MOVE0→WATER→DETERGENT→WASH→MOVE180→DRAIN→SPIN
  →MOVE270→DRY→MOVE45→WAIT_UNLOAD→MOVE0→UV→MOVE45→FINISH

DEMO_GOLDEN_SEQUENCE:
  12 steps: HOME→MOVE0→WATER→DETERGENT→WASH→MOVE180→DRAIN→SPIN
  →MOVE270→DRY→MOVE45→FINISH

FINAL_POSITION_INVARIANT:
  After every program (normal/abort_reset): DRUM_POS_45.
  Enforced by: planner final MOVE 45, abort_reset includes MOVE 45.

BOOTSTRAP_INIT_START_ORDER:
  NVS→Config→Store→Global→Ingress→HAL→Safety→Pos→BL50→Water→Dry
  →Drain→Det→UV→Executor→Button

BOOTSTRAP_DEINIT_ORDER:
  Button→Executor(quiesce)→UV→Det→Drain→Dry→Water→BL50→Pos
  →Executor(stop+destroy)→Safety→HAL

TEST_PLAN_UPDATED:
  Planner: 35 tests (PLAN-T01~T35)
  Executor: 57 tests (EXE-T01~T57)

CODE_REVIEW: STRUCTURED_MANUAL (12 items verified against real source)

UNRESOLVED_ARCHITECTURE_DECISIONS: 0

BL50_ACTION_SPECIFIC_DEFAULT_FIX: DEFERRED_TO_PHASE7_GROUP_B
  Current bl50_fsm.c: non-HOME with target_pwm_percent=0 always gets 40%.
  BL50_DEFAULT_DRUM_PWM (30%) and BL50_DEFAULT_SPIN_PWM (80%) not used in fallback.
  Fix in Phase 7 Group B executor adapter.

READY_FOR_PHASE7_IMPLEMENTATION: YES
```
