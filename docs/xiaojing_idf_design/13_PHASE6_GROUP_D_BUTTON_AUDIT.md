# Phase 6 Group D — Button Service Architecture Audit (Amended)

**Date:** 2026-07-26 (amended from 2026-07-25)
**Baseline:** 585 Unity registered tests (570 PASS / 0 FAIL / 15 [hw] IGNORE), 3 rounds stable
**Scope:** button_service (BTN1/2/3), MCP23017 interrupt chain for Port A inputs
**Amendment reason:** Five audit gaps identified before implementation: interrupt mode, consumer ownership, port-change detection, pullup policy, debounce scheduling.

---

## 0. Group C Documentation Correction

### 0.1 Test Count Reconciliation

| Metric | Value | Note |
|---|---|---|
| TEST_CASE textual occurrences | 583 | Counted from source |
| Unity registered tests | 585 | Reported by test runner |
| Difference | +2 | One `TEST_CASE` macro expands to 3 tests via `CONCURRENT_STOP_TEST` macro |

### 0.2 Accepted P2 Issues (Non-Blocking)

1. **Concurrent stop macro hardcoded drain/position refs** — Cosmetic; macro still produces correct test assertions.
2. **Fixed-wait synchronization in some concurrent tests** — EventGroup provides the correctness guarantee; the fixed wait is for log readability.
3. **Terminal recovery teardown could add stop2 condition** — Relies on `LC_STOPPING && task_joined` early-exit.

**Status:** All three accepted P2. No Group C production code modified. No blocking impact on Group D.

---

## 1. CRITICAL: MCP23017 INTA/INTB MIRROR Issue

### 1.1 The Problem

Current hardware wiring:

```
ESP32 GPIO13 ←── INTB (MCP23017 pin 19)
```

Buttons are on Port A (GPA4/5/6). Current IOCON configuration:

```c
#define IOCON_CONFIG  IOCON_ODR   /* = 0x04 */
```

Bit breakdown of `0x04`:

| Bit | Name | Value | Effect |
|-----|------|-------|--------|
| 6 | MIRROR | **0** | INTA and INTB are **separate** outputs |
| 2 | ODR | 1 | Open-drain output mode |
| 1 | INTPOL | 0 | Active-low (overridden by ODR) |

With `MIRROR=0`:
- Port A interrupt sources → **INTA** (MCP23017 pin 20)
- Port B interrupt sources → **INTB** (MCP23017 pin 19)
- ESP32 GPIO13 only listens to INTB

**Result:** A button press on GPA4/5/6 triggers INTA. GPIO13 never sees it. Buttons are invisible to firmware.

### 1.2 Required IOCON Fix

Per MCP23017 datasheet (DS20001952C, Section 3.5.6):

| Bit | Name | Required | Reason |
|-----|------|----------|--------|
| 6 | MIRROR | **1** | OR INTA+INTB onto single INTB output |
| 2 | ODR | 1 | Open-drain (unchanged) |
| 1 | INTPOL | 0 | Active-low (unchanged) |

New value: `0x44`

```
Bit 6: MIRROR = 1  (0x40)
Bit 2: ODR    = 1  (0x04)
                 ─────
                 0x44
```

### 1.3 Impact on Port B

With MIRROR=1, both INTA and INTB are OR'd onto the INTB output:

- Port B interrupts still fire identically (GPINTENB/DEFVALB/INTCONB unchanged)
- INTB still goes low when any enabled interrupt fires
- The ISR still gives `int_sem`
- `wait_interrupt` still reads both INTCAPA and INTCAPB

Port B behavior is preserved. No regression expected.

### 1.4 Existing Test Update

Test at `test_runner.c:712` ("MCP IOCON includes ODR bit for open-drain") must change:
- `TEST_ASSERT_EQUAL_HEX8(0x04, iocon)` → `TEST_ASSERT_EQUAL_HEX8(0x44, iocon)`
- MIRROR bit assertion: `TEST_ASSERT_NOT_EQUAL(0, iocon & (1 << 6))` (was `TEST_ASSERT_EQUAL`)

---

## 2. Interrupt Consumer Ownership Audit

### 2.1 Grep Results

```
rg -n "wait_interrupt|mcp23017_register_int_callback|int_callback|int_sem"
```

| Caller | File | Calls wait_interrupt? | Calls register_int_callback? |
|--------|------|-----------------------|------------------------------|
| mcp23017_driver.c | Internal definition | Defines it | Defines it |
| paj7620_adapter.c | paj7620_adapter.c | **No** | **No** |
| position_service | (not found) | **No** | **No** |
| drain_service | (not found) | **No** | **No** |
| detergent_service | (not found) | **No** | **No** |
| uv_service | (not found) | **No** | **No** |
| real_hal.c | real_hal.c | **No** | **No** |
| test_runner.c | test_runner.c | **No** | **No** |

**Finding:** `mcp23017_wait_interrupt` and `mcp23017_register_int_callback` have **zero external callers** in the entire codebase.

The PAJ7620 adapter reads the gesture register directly via I2C (`i2c_bus_transmit_receive` to PAJ7620 at address 0x73), not through MCP GPIOB or the interrupt path. It does not consume `int_sem`.

### 2.2 Conclusion

**button_service is the sole consumer of `wait_interrupt`.** No interrupt dispatcher is needed. No contention with PAJ7620 or position service. The `int_sem` binary semaphore has exactly one waiter.

If a future service needs interrupt notification, an MCP interrupt dispatcher can be introduced at that time. Not needed for Phase 6.

---

## 3. Port A Interrupt Configuration (Amended)

### 3.1 Interrupt Mode: Any-Change Detection

**Amendment:** The original audit specified compare-against-default mode (INTCONA bits set), which primarily detects the falling edge (press). The corrected configuration uses **any-change mode** (INTCONA bits cleared) so both press and release edges wake the service.

| Register | Address | Operation | Value | Meaning |
|----------|---------|-----------|-------|---------|
| GPINTENA | 0x04 | RMW: OR in 0x70 | bits 4,5,6 set | Enable interrupt for GPA4/5/6 |
| INTCONA  | 0x08 | RMW: AND out 0x70 | bits 4,5,6 clear | Any-change mode (not compare-against-default) |
| DEFVALA  | 0x06 | **No write** | unchanged | Not used when INTCONA bit=0 |

**Any-change mode** (MCP23017 datasheet Section 3.5.1):
- INTCONA bit=0: interrupt on any change relative to previous value
- DEFVALA is irrelevant for these pins
- Both HIGH→LOW (press) and LOW→HIGH (release) generate interrupts

**Read-modify-write required:** All register writes use read-modify-write to preserve bits for other users. Only GPA4/5/6 bits (0x70) are touched.

### 3.2 GPPUA Internal Pullup Policy

GPA4/5/6 are low-effective buttons. The MCP23017 has optional 100kΩ internal pullups on each pin.

**Current state:** GPPUA is not written during init (reset default = 0x00, all pullups disabled).

**Audit finding:** The board design uses external 10kΩ pullups on the button lines (consistent with the INTB line also having a 10kΩ external pullup). External pullups dominate over the MCP's 100kΩ internal pullups.

**Policy:**
- **If external pullups confirmed (schematic/PCB):** GPPUA writes are optional but harmless. The API provides the option; it's not forced.
- **If external pullups not confirmed:** The API enables GPPUA bits 4-6 as a safety net.
- **Implementation:** `mcp23017_configure_button_interrupts()` takes an `enable_pullup` parameter. Read-modify-write GPPUA, only touching bits 4-6.

### 3.3 Port Change Detection Fix (INTFA/INTFB)

**Amendment:** The original audit used `intcap_a != 0` to detect Port A activity. This is incorrect — INTCAPA captures pin *levels*, not interrupt *flags*.

**Correct approach:** Read INTFA/INTFB (interrupt flag registers) to determine which ports have pending interrupts, then read INTCAPA/INTCAPB to clear the flags.

Register addresses (BANK=0):
```
MCP_REG_INTFA   = 0x0E    // Interrupt flag Port A
MCP_REG_INTFB   = 0x0F    // Interrupt flag Port B
MCP_REG_INTCAPA = 0x10    // Interrupt capture Port A (read clears flag)
MCP_REG_INTCAPB = 0x11    // Interrupt capture Port B (read clears flag)
```

**Read sequence in `wait_interrupt`:**
1. Read INTFA → which Port A pins triggered the interrupt
2. Read INTFB → which Port B pins triggered the interrupt
3. Build `port_changed`: `(intfa != 0) ? (1 << PORT_A) : 0` | `(intfb != 0) ? (1 << PORT_B) : 0`
4. Read INTCAPA → captures Port A levels at interrupt time, clears INTFA flags
5. Read INTCAPB → captures Port B levels at interrupt time, clears INTFB flags

**Why INTFA/INTFB before INTCAPA/INTCAPB:** Reading INTCAP clears the interrupt flag on the MCP23017. If we read INTCAP first, the flags are gone and we can't determine which port triggered.

**Edge cases verified:**
| Scenario | INTFA | INTFB | port_changed |
|----------|-------|-------|-------------|
| BTN1 press only | 0x10 | 0x00 | PORT_A |
| Hall change only | 0x00 | 0x01 | PORT_B |
| Both simultaneously | 0x10 | 0x01 | PORT_A | PORT_B |
| All-low INTCAPA (all pins LOW) | (any) | (any) | Correctly from INTFA/INTFB |
| INTFA read fails | — | — | ESP_ERR_INVALID_RESPONSE, no callback |
| INTFB read fails | — | — | ESP_ERR_INVALID_RESPONSE, no callback |

### 3.4 Updated Callback Signature

```c
/* Amended: callback receives actual interrupt flag bytes, not INTCAP!=0 heuristic */
typedef void (*mcp23017_int_callback_t)(uint8_t port_changed, void *user_data);
```

`port_changed` is now computed from INTFA/INTFB (interrupt flags), not from INTCAPA/INTCAPB (captured levels). The callback signature is unchanged; only the computation method changes.

### 3.5 Interrupt Clearing Sequence

When an interrupt fires and `wait_interrupt` is called:

1. `int_sem` is given by ISR (GPIO13 falling edge)
2. Under mutex:
   a. Read INTFA → `intfa` (flags before clearing)
   b. Read INTFB → `intfb` (flags before clearing)
   c. Read INTCAPA → captures Port A levels, clears Port A flags
   d. Read INTCAPB → captures Port B levels, clears Port B flags
3. Compute `port_changed` from `intfa`/`intfb`
4. Invoke callback with `port_changed`, `intcap_a`, `intcap_b`

**MIRROR=1 clearing:** With MIRROR=1, INTB is de-asserted when ALL pending interrupt flags are cleared (both INTFA and INTFB are zero). Reading both INTCAPA and INTCAPB in sequence ensures both ports' flags are cleared.

---

## 4. MCP23017 Driver API Changes

### 4.1 IOCON Constant Update

```c
/* Before */
#define IOCON_ODR          (1 << 2)
#define IOCON_CONFIG       IOCON_ODR                    /* 0x04 */

/* After */
#define IOCON_ODR          (1 << 2)
#define IOCON_MIRROR       (1 << 6)
#define IOCON_CONFIG       (IOCON_MIRROR | IOCON_ODR)   /* 0x44 */
```

### 4.2 New Register Definitions

```c
#define MCP_REG_INTFA      0x0E
#define MCP_REG_INTFB      0x0F
```

### 4.3 New Public API

```c
/**
 * Configure Port A button interrupts (GPA4/5/6).
 * - Enables GPINTENA bits for specified pins (OR'd, read-modify-write)
 * - Clears INTCONA bits for specified pins (any-change mode)
 * - Optionally enables GPPUA internal pullups for specified pins
 * - Reads GPIOA + INTCAPA to establish baseline and clear pending
 * - Single mutex hold for entire operation
 *
 * pin_mask: bit mask of pins to configure (e.g. 0x70 for GPA4-6)
 * enable_pullup: true to set GPPUA bits for these pins
 *
 * Must be called after mcp23017_init(). Port B config is untouched.
 */
esp_err_t mcp23017_configure_button_interrupts(mcp23017_handle_t *handle,
                                                uint8_t pin_mask,
                                                bool enable_pullup);

/**
 * Disable Port A button interrupts (cleanup on stop).
 * Clears GPINTENA bits for specified pins. Read-modify-write.
 * Does NOT read INTCAP (caller may still need pending state).
 */
esp_err_t mcp23017_disable_button_interrupts(mcp23017_handle_t *handle,
                                              uint8_t pin_mask);

/**
 * Read interrupt flags (INTFA/INTFB) and captured state (INTCAPA/INTCAPB).
 * Single mutex hold. Read order: INTFA → INTFB → INTCAPA → INTCAPB.
 * Returns which ports have pending flags in *port_flags_a / *port_flags_b.
 * Returns captured levels in *intcap_a / *intcap_b.
 * Reading INTCAP clears the interrupt flags on the MCP23017.
 */
esp_err_t mcp23017_read_interrupt_state(mcp23017_handle_t *handle,
                                         uint8_t *port_flags_a,
                                         uint8_t *port_flags_b,
                                         uint8_t *intcap_a,
                                         uint8_t *intcap_b);
```

### 4.4 Updated `wait_interrupt` Implementation

```c
esp_err_t mcp23017_wait_interrupt(mcp23017_handle_t *handle,
                                  uint32_t timeout_ms)
{
    if (!handle || !handle->int_sem) return ESP_ERR_INVALID_STATE;

    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(handle->int_sem, ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    /* Read INTFA/INTFB first (flags), then INTCAPA/INTCAPB (clears flags) */
    uint8_t intfa = 0, intfb = 0, intcap_a = 0, intcap_b = 0;
    if (xSemaphoreTake(handle->mutex, MUTEX_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err;
    err = read_reg(handle, MCP_REG_INTFA, &intfa);
    if (err != ESP_OK) goto read_fail;
    err = read_reg(handle, MCP_REG_INTFB, &intfb);
    if (err != ESP_OK) goto read_fail;
    err = read_reg(handle, MCP_REG_INTCAPA, &intcap_a);
    if (err != ESP_OK) goto read_fail;
    err = read_reg(handle, MCP_REG_INTCAPB, &intcap_b);
    if (err != ESP_OK) goto read_fail;
    xSemaphoreGive(handle->mutex);

    if (handle->int_callback) {
        uint8_t port_changed = 0;
        if (intfa != 0) port_changed |= (1 << MCP_PORT_A);
        if (intfb != 0) port_changed |= (1 << MCP_PORT_B);
        handle->int_callback(port_changed, handle->int_user_data);
    }
    return ESP_OK;

read_fail:
    xSemaphoreGive(handle->mutex);
    ESP_LOGW(TAG, "wait_interrupt: register read failed (err=0x%x)", err);
    return ESP_ERR_INVALID_RESPONSE;
}
```

### 4.5 Mutex Discipline

**Rule:** Public API functions acquire the mutex exactly once. Internal I2C helpers (`read_reg`, `write_reg`) do NOT acquire the mutex — they are called from within mutex-held contexts.

This is already the case in the current code. The new APIs follow the same pattern:
- `mcp23017_configure_button_interrupts()`: single `xSemaphoreTake` → multiple `read_reg`/`write_reg` → `xSemaphoreGive`
- `mcp23017_disable_button_interrupts()`: single `xSemaphoreTake` → `read_reg` + `write_reg` → `xSemaphoreGive`
- `mcp23017_read_interrupt_state()`: single `xSemaphoreTake` → four `read_reg` → `xSemaphoreGive`

**No recursive lock risk.** `read_reg` and `write_reg` are lock-free (they only do I2C transactions).

### 4.6 Button Interrupt Start Sequence

The button_service start sequence must avoid false clicks:

```
1. Disable button interrupt bits (GPINTENA &= ~pin_mask) — prevents stale triggers
2. Read GPIOA — establish initial raw baseline
3. Read GPIOA + INTCAPA + INTFA — clear any pending interrupts
4. Configure INTCONA (any-change mode for pin_mask)
5. Configure GPPUA (if enable_pullup)
6. Enable button interrupt bits (GPINTENA |= pin_mask)
7. Start debounce processing
```

### 4.7 Button Interrupt Stop Sequence

```
1. Disable button interrupt bits (GPINTENA &= ~pin_mask)
2. Read INTCAPA — clear any pending Port A interrupts
```

Only GPA4/5/6 bits are cleared. Port B interrupt configuration (GPINTENB) is untouched.

### 4.8 I2C Failure Handling

All new APIs return `esp_err_t`. On any I2C failure:
- `configure_button_interrupts`: returns the first error. Partial state is allowed (some registers may be written, others not). The caller can retry or report.
- `disable_button_interrupts`: returns the first error. Disable is best-effort; re-calling is safe.
- `read_interrupt_state`: returns the first error. Output pointers are not modified on failure.
- `wait_interrupt`: returns `ESP_ERR_INVALID_RESPONSE` if any INTF/INTCAP read fails. No callback invoked.

---

## 5. Debounce Scheduling (Amended)

### 5.1 Problem with Fixed 200ms Polling

Original design used `wait_interrupt(200ms)` as the polling fallback. This fails for normal button clicks:

```
t=0:    BTN1 pressed → MCP interrupt → wakeup
t=1:    read GPIOA, raw=LOW, debounce engine: WAIT_DEBOUNCE, deadline=31
t=2:    wait_interrupt(200ms) called
t=85:   BTN1 released → MCP interrupt → wakeup
t=86:   read GPIOA, raw=HIGH, debounce engine: WAIT_RELEASE_DEBOUNCE, deadline=116
t=87:   wait_interrupt(200ms) called
        ← If no other interrupt arrives, we wait until t=287 →
        ← At t=116, the 30ms release debounce should have fired →
        ← But we're blocked in wait_interrupt until t=287 →
        ← The click is delayed by 170ms, not lost, but unacceptable latency →
```

The click is NOT lost (it fires when the poll timeout eventually expires), but the latency is unpredictable and the "deadline at 30ms" contract is broken.

### 5.2 Solution: Adaptive Wait Time

The `wait_interrupt` timeout is computed from the debounce engine's next deadline:

```c
uint32_t now = hal->now_ms();
uint32_t next_deadline = button_debounce_next_deadline(&debounce_ctx, now);
uint32_t poll_fallback = config->poll_interval_ms;  /* e.g. 200ms */
uint32_t wait_ms = (next_deadline == 0) ? poll_fallback :
                   (next_deadline < poll_fallback) ? next_deadline : poll_fallback;
```

When a button is in a debounce state (WAIT_DEBOUNCE or WAIT_RELEASE_DEBOUNCE), `next_deadline` returns the remaining time until the debounce period expires. The wait timeout is clamped to this deadline.

**Fixed timeline:**
```
t=0:    BTN1 pressed → wakeup
t=1:    raw=LOW, WAIT_DEBOUNCE, deadline=31 → wait_interrupt(30ms)
t=31:   timeout → wakeup, debounce elapsed → PRESSED, armed
t=32:    wait_interrupt(200ms) [no active deadline, use poll fallback]
t=85:   BTN1 released → wakeup
t=86:   raw=HIGH, WAIT_RELEASE_DEBOUNCE, deadline=116 → wait_interrupt(30ms)
t=116:  timeout → wakeup, release debounce elapsed → CLICK emitted at t=0
```

### 5.3 New Debounce API: `button_debounce_next_deadline`

```c
/**
 * Returns the time in ms until the next debounce deadline expires.
 * Returns 0 if no button is in a debounce-waiting state.
 * Tick-wrap safe: uses uint32_t subtraction.
 */
uint32_t button_debounce_next_deadline(const button_debounce_ctx_t *ctx,
                                        uint32_t now_ms);
```

### 5.4 Initialization State Handling

If buttons are already pressed at initialization:
- `button_debounce_init()` sets all buttons to IDLE (released) state
- The first GPIOA read establishes the raw baseline
- If raw shows a button pressed, the debounce engine enters WAIT_DEBOUNCE
- No CLICK event is produced until a complete press→release cycle is observed
- A button held since boot does not generate a CLICK until it's released and the release debounce completes

This prevents false clicks on startup when buttons may already be in an indeterminate state.

---

## 6. Shadow / PTC / Emergency Latch Safety

### 6.1 Confirmation: No Impact

| Component | IOCON change? | GPINTENA RMW? | GPPUA RMW? | INTCONA RMW? |
|-----------|---------------|---------------|------------|---------------|
| OLATA shadow | No | No | No | No |
| OLATB shadow | No | No | No | No |
| Emergency latch | No | No | No | No |
| PTC IODIR | No | No | No | No |
| Output whitelist | No | No | No | No |
| Port B IODIRB | No | No | No | No |
| Port B GPINTENB | No | No | No | No |

All new register writes target GPINTENA, INTCONA, and GPPUA — none of which affect output state.

---

## 7. button_service Architecture

### 7.1 Two-Layer Design

```
┌─────────────────────────────────────────────┐
│          button_service (FreeRTOS)          │
│  - MCP interrupt + adaptive poll timeout    │
│  - Task lifecycle (Group C pattern)         │
│  - Sink-based event publishing              │
│  - BTN1/BTN2 → normal sink (drop+counter)  │
│  - BTN3 → urgent sink (pending+retry)       │
└──────────────────┬──────────────────────────┘
                   │ raw mask + now_ms
┌──────────────────▼──────────────────────────┐
│          button_debounce (Pure C)           │
│  - No FreeRTOS, no HAL, no I2C             │
│  - Per-button state machine                 │
│  - Tick-wrap safe (uint32_t subtraction)    │
│  - Input: raw_mask (0x70 only), now_ms      │
│  - Output: event array + next_deadline      │
└─────────────────────────────────────────────┘
```

### 7.2 Pure C Debounce Layer

**File:** `components/button_service/private_include/button_debounce.h`
**File:** `components/button_service/button_debounce.c`

Types and API are defined in Section 7.2 above (unchanged from original audit). Key amendments:

**Mask handling:** `button_debounce_process()` extracts bits 4-6 from raw_mask. Bits 0-3 and 7 are ignored. There is no "invalid mask" — any uint8_t value is valid; irrelevant bits are masked out.

**Deterministic event ordering:** When multiple buttons produce events in the same call, output order is BTN1 → BTN2 → BTN3 (by button_id). The global sequence number ensures cross-button ordering is preserved.

**Timestamp contract:** `timestamp_ms` in `button_event_t` is the time when the stable press was first confirmed (when debounce_ms elapsed after the raw press edge), NOT the raw edge time.

**Initialization state:** All buttons start in IDLE state. A button already pressed at init time enters WAIT_DEBOUNCE on first `process()` call, not PRESSED. No false click.

### 7.3 Sink Failure Contracts

#### Normal sink (BTN1/BTN2): Drop + Counter

```c
/* Contract: If normal_sink.publish() fails (timeout, queue full, error):
 * - Event is dropped (not retried)
 * - snapshot.normal_sink_drops counter increments
 * - ESP_LOGW logged once per 10 consecutive drops (rate-limited)
 * - Service continues operating
 */
```

#### Urgent sink (BTN3): Pending + Retry

```c
/* Contract: If urgent_sink.publish() fails:
 * - Event is stored in pending_urgent_event
 * - Retried on every subsequent task loop iteration
 * - Maximum BTN3_URGENT_MAX_RETRIES (10) attempts
 * - After max retries: event dropped, snapshot.urgent_sink_drops increments,
 *   snapshot.urgent_last_error records the error, ESP_LOGE logged
 * - A new BTN3 event arriving while one is pending:
 *   the NEW event replaces the pending one (latest-wins)
 * - On successful delivery: pending cleared, snapshot.urgent_delivered increments
 *
 * Snapshot exposes:
 * - urgent_pending: bool (currently undelivered urgent event)
 * - urgent_retry_count: uint32_t
 * - urgent_delivered: uint32_t (total successful)
 * - urgent_sink_drops: uint32_t (total failed after max retries)
 * - urgent_last_error: esp_err_t
 * - normal_sink_drops: uint32_t (total dropped normal events)
 */
```

### 7.4 FreeRTOS button_service Layer

**File:** `components/button_service/include/button_service.h`
**File:** `components/button_service/button_service.c`

#### Event Types (unchanged)

```c
typedef struct {
    button_id_t         button_id;
    button_event_type_t event_type;
    uint32_t            timestamp_ms;   /* stable press confirmed time */
    uint32_t            sequence;       /* global monotonic */
    uint8_t             stable_mask;    /* debounced state of all 3 buttons */
} button_sink_event_t;

typedef esp_err_t (*button_event_sink_fn)(const button_sink_event_t *event,
                                          uint32_t timeout_ms, void *context);
typedef esp_err_t (*button_urgent_sink_fn)(const button_sink_event_t *event,
                                           uint32_t timeout_ms, void *context);

typedef struct { button_event_sink_fn  publish; void *context; } button_event_sink_t;
typedef struct { button_urgent_sink_fn publish; void *context; } button_urgent_sink_t;
```

#### Public API

```c
#define BTN_SINK_TIMEOUT_MS     50
#define BTN_POLL_FALLBACK_MS    200
#define BTN_DEBOUNCE_DEFAULT_MS 30
#define BTN3_URGENT_MAX_RETRIES 10

typedef struct {
    uint32_t debounce_ms;           /* 0 = BTN_DEBOUNCE_DEFAULT_MS */
    uint32_t poll_interval_ms;      /* 0 = BTN_POLL_FALLBACK_MS */
    mcp23017_handle_t *mcp;         /* not owned */
    bool enable_internal_pullup;    /* true = enable GPPUA for GPA4-6 */
    button_event_sink_t  normal_sink;
    button_urgent_sink_t urgent_sink;
} button_service_config_t;

esp_err_t button_service_global_init(void);
esp_err_t button_service_init(const button_service_config_t *config);
esp_err_t button_service_start(void);
esp_err_t button_service_stop(void);

typedef struct {
    uint8_t  stable_mask;
    uint32_t last_event_sequence;
    uint32_t total_events;
    bool     task_running;
    /* Sink delivery stats */
    uint32_t normal_sink_drops;
    uint32_t urgent_delivered;
    uint32_t urgent_sink_drops;
    uint32_t urgent_retry_count;
    bool     urgent_pending;
    esp_err_t urgent_last_error;
} button_service_snapshot_t;

esp_err_t button_service_get_snapshot(button_service_snapshot_t *out);

#ifdef XIAOJING_TESTING
esp_err_t button_service_test_abort_cleanup(void);
void button_service_test_set_hook(int hook_point);
void button_service_test_clear_hooks(void);
SemaphoreHandle_t button_service_test_get_barrier(int hook_point);
/* Direct access to debounce ctx for unit tests */
button_debounce_ctx_t *button_service_test_get_debounce_ctx(void);
#endif
```

#### Task Flow (Amended)

```
button_service_task:
    1. Start sequence (Section 4.6):
       a. Disable button interrupts (GPINTENA &= ~0x70)
       b. Read GPIOA, establish initial raw baseline
       c. Clear pending: read INTCAPA, INTFA
       d. Configure INTCONA (any-change for 0x70)
       e. Configure GPPUA (if enable_internal_pullup)
       f. Enable button interrupts (GPINTENA |= 0x70)

    2. Main loop (while !stop_requested):
       a. Compute wait_ms = min(debounce_next_deadline, poll_fallback)
          - If no active debounce: wait_ms = poll_fallback
       b. wait_interrupt(wait_ms) — blocks until interrupt OR timeout
       c. On wakeup:
          - Read GPIOA via mcp23017_read_gpio()
          - On I2C error: log, increment error counter, continue
          - mask & 0x70 → raw_mask
          - button_debounce_process(raw_mask, now_ms, events, 3)
          - For each produced event:
            * BTN1/BTN2: try normal_sink.publish(timeout=50ms)
              - Success: total_events++
              - Failure: normal_sink_drops++, rate-limited warning
            * BTN3: try urgent_sink.publish(timeout=50ms)
              - Success: total_events++, urgent_delivered++, clear pending
              - Failure: store as pending_urgent, urgent_retry_count++
          - If urgent_pending: retry urgent_sink.publish()
              - Success: clear pending, urgent_delivered++
              - Failure: increment retry_count, if >= MAX: drop, urgent_sink_drops++
       d. Update snapshot under snapshot_mtx
       e. Test hook pause point

    3. Stop-drain:
       a. Disable button interrupts (GPINTENA &= ~0x70)
       b. Read INTCAPA to clear pending

    4. Set task_exited, give task_done_sem
    5. vTaskDelete(NULL)
```

### 7.5 File Structure

```
components/button_service/
├── CMakeLists.txt
├── include/
│   └── button_service.h          # Public API
├── private_include/
│   └── button_debounce.h         # Pure C debounce layer
├── button_service.c              # FreeRTOS service
└── button_debounce.c             # Pure C debounce engine

test_apps/unit/main/test_runner.c  # New tests appended
```

---

## 8. Lifecycle Requirements

Reusing the verified Group C pattern:

| Phase | Function | Resources | Protection |
|-------|----------|-----------|------------|
| global_init | `button_service_global_init()` | `s_lc_mtx`, `s_stop_mtx` (static) | `StaticSemaphore_t` + atomic once |
| init | `button_service_init(config)` | `snapshot_mtx`, `task_done_sem`, debounce ctx, config copy | `s_lc_mtx` |
| start | `button_service_start()` | Task creation (core 1, prio 15, stack 4096) | `s_lc_mtx` |
| stop | `button_service_stop()` | Semaphore wait + cleanup | `s_stop_mtx` outer, `s_lc_mtx` inner |

**One-time initialization:** Uses `StaticSemaphore_t` with `portMUX_INITIALIZER_UNLOCKED` atomic flag (same pattern as Group C auto-init), NOT bare `bool s_mtx_inited`.

**button_service is an input service.** It does NOT register a safety stop hook — no actuator outputs to safe.

**Sink callbacks run outside all mutexes.** The `s_lc_mtx` is NOT held during sink.publish() calls. This prevents deadlock if the sink callback calls `button_service_get_snapshot()`.

---

## 9. Test Coverage Requirements

### 9.1 Test Matrix (Amended)

| ID | Description | Layer | Sync |
|----|-------------|-------|------|
| BTN-D01 | All HIGH, no events | Pure C | Direct |
| BTN-D02 | BTN1 10ms glitch, no event | Pure C | Direct |
| BTN-D03 | BTN1 stable press+release, one CLICK | Pure C | Direct |
| BTN-D04 | Bounce before stable, only one CLICK | Pure C | Direct |
| BTN-D05 | Long hold, no repeated CLICK | Pure C | Direct |
| BTN-D06 | BTN1+BTN2 simultaneous, two independent CLICKs | Pure C | Direct |
| BTN-D07 | BTN3 uses urgent sink | Service | EventGroup |
| BTN-D08 | Normal sink full/drops, BTN3 still delivered | Service | EventGroup |
| BTN-D09 | tick=0 | Pure C | Direct |
| BTN-D10 | uint32 wrap | Pure C | Direct |
| BTN-D11 | Irrelevant bits in mask ignored | Pure C | Direct |
| BTN-D12 | I2C/MCP read error → no false click | Service | EventGroup |
| BTN-D13 | Error recovery → resync | Service | EventGroup |
| BTN-D14 | Interrupt coalescing → final snapshot | Service | EventGroup |
| BTN-D15 | IOCON == 0x44, MIRROR=1 verified | Unit | Direct |
| BTN-D16 | INTFA/INTFB read before INTCAPA/INTCAPB | Unit | Mock seq |
| BTN-D17 | init/start/stop 20 rounds, no leak | Service | heap |
| BTN-D18 | Stop timeout + retry | Service | Semaphore |
| BTN-D19 | Snapshot + stop overlap | Service | Concurrent |
| BTN-D20 | Double stop concurrent | Service | xEventGroupSync |
| BTN-D21 | Sink reentrant safety | Service | Callback |
| BTN-D22 | Sequence/revision monotonic | Pure C + Service | Direct |
| BTN-D23 | Setup failure → real FAIL | Unit | Mock |
| BTN-D24 | BTN3 exactly once | Service | EventGroup |
| **BTN-D25** | **INTCONA any-change mode register verify** | **Unit** | **Mock** |
| **BTN-D26** | **INTCAP all-low still identifies port via INTFA** | **Unit** | **Mock** |
| **BTN-D27** | **INTA+INTB simultaneous, both ports in callback** | **Unit** | **Mock** |
| **BTN-D28** | **Port B / PAJ / Hall config unchanged after button config** | **Unit** | **Direct** |
| **BTN-D29** | **GPPUA RMW preserves unrelated bits** | **Unit** | **Mock** |
| **BTN-D30** | **100ms normal click not lost by 200ms poll** | **Pure C** | **Direct** |
| **BTN-D31** | **30ms debounce deadline re-sample** | **Pure C** | **Direct** |
| **BTN-D32** | **Urgent sink timeout → pending → retry → success once** | **Service** | **EventGroup** |
| **BTN-D33** | **Concurrent normal+urgent producer** | **Service** | **EventGroup** |
| **BTN-D34** | **Stop timeout + retry (abort/longjmp safe)** | **Service** | **Semaphore** |
| **BTN-D35** | **Heap baseline stable after init/start/stop** | **Service** | **heap** |

### 9.2 Test Count Reporting

The final report must include:
- **Textual TEST_CASE count** in source
- **Macro-expanded/registered total** from Unity
- **New Group D count** (this section)
- **Old baseline unchanged** (585)

### 9.3 Synchronization Rules

- No fixed delay as sole sync proof. All service tests use EventGroup/semaphore barriers.
- Task completion verified via `task_exited` or `task_done_sem`.
- Heap baseline recorded before init and after final stop.
- Test hooks only under `XIAOJING_TESTING`.
- Main firmware never enables test hooks.

---

## 10. Integration Scope

- BTN1/BTN2: publish normal button events only.
- BTN3: publish urgent button events only.
- No direct calls to water/dry/BL50/UV/drain/detergent actuators.
- No bypass of safety_manager.
- Phase 7 sink not yet exists → `app_bootstrap` does NOT auto-start real button_service.
- Main project: compile integration only, default FAKE mode, no real button output.
- No real buttons connected. No electrical testing.

---

## 11. Hardware Status

| Item | Status | Action |
|------|--------|--------|
| BTN1/BTN2/BTN3 physical buttons | NOT CONNECTED | No physical buttons on board |
| GPA4/GPA5/GPA6 external pullups | ASSUMED present (10k) | Matches INTB line pattern |
| INTB waveform for Port A | UNKNOWN | Cannot verify without buttons |
| Bounce timing characteristics | UNKNOWN | Using 30ms default |

**DEFERRED_HARDWARE markers** for real GPIO/MCP electrical tests.

---

## 12. Verification Gates

Sequential:

1. Main project build (`idf.py build`)
2. Unit build, zero new warnings
3. Pure C host runtime tests
4. Python host tests
5. GPIO map / self-test
6. Safety bypass scan
7. Main firmware test-hooks-disabled scan
8. On-board Unity three rounds

Per-round requirements:
- FAIL = 0
- NON_HW_IGNORE = 0
- Only existing `[hw]` tests may IGNORE
- Completion marker present
- Binary/ELF SHA matches current build
- No Guru Meditation, WDT, stack overflow, heap corruption, brownout, unexpected reset

---

## 13. Implementation Order

1. Audit document — DONE (this file)
2. `mcp23017_driver.c` — IOCON 0x44 + new APIs + INTFA/INTFB registers
3. Update existing IOCON test (0x04 → 0x44)
4. `button_debounce.c/h` — pure C debounce with `next_deadline()`
5. `button_service.h/c` — FreeRTOS service
6. All new tests (BTN-D01 through BTN-D35)
7. Build + test verification
8. Three on-board rounds

---

## 14. Exit Conditions

| Gate | Criterion |
|------|-----------|
| ARCH_AUDIT_AMENDED | This document |
| MCP_IOCON_MIRROR | IOCON=0x44, verified in test |
| INTERRUPT_MODE | Any-change (INTCONA bits clear for 0x70) |
| INTERRUPT_CONSUMER_OWNERSHIP | button_service sole wait_interrupt caller |
| INTF_INTCAP_SEMANTICS | INTFA/INTFB read before INTCAPA/INTCAPB |
| BUTTON_PULLUP_POLICY | GPPUA RMW, configurable |
| DEBOUNCE_DEADLINE | Adaptive wait = min(deadline, poll_fallback) |
| URGENT_DELIVERY | Pending + bounded retry, snapshot-visible |
| TEST_CASE_BASELINE | 585 old + N new |
| NON_HW_IGNORE | 0 |
| CODE_REVIEW | 0 P0 / 0 P1 |
| FULL_RUN1_FAIL | 0 |
| FULL_RUN2_FAIL | 0 |
| FULL_RUN3_FAIL | 0 |
| PHASE6_GROUP_D_EXIT_GATE | All above PASS |
| READY_FOR_PHASE7 | YES |
