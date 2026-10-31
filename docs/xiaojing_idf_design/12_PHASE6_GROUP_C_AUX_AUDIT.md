# Phase 6 Group C — Auxiliary Services Audit

**Date:** 2026-07-25
**Baseline:** 460 TEST_CASE, 445 PASS / 0 FAIL / 15 [hw] IGNORE
**Scope:** drain_service, detergent_service, uv_service (NOT button_service)

---

## 1. safety_manager Audit

### 1.1 Authorization Path

All three operations are classified as MCP output ops:

| Operation | safety_operation_t | safe_output_t | MCP Port/Pin |
|---|---|---|---|
| Drain valve | SAFETY_OP_DRAIN_VALVE | SAFE_OUTPUT_DRAIN_VALVE | GPA3 (A bit 3) |
| Detergent pump | SAFETY_OP_DETERGENT_PUMP | SAFE_OUTPUT_DETERGENT_PUMP | GPB7 (B bit 7) |
| UV lamp | SAFETY_OP_UV | SAFE_OUTPUT_UV | GPA2 (A bit 2) |

All three route through `safety_manager_apply()` → `apply_mcp_output()`.

### 1.2 Interlock Requirements (safety_interlocks.c)

| Property | DRAIN_VALVE | DETERGENT_PUMP | UV |
|---|---|---|---|
| Required position | DRUM_POS_180 | DRUM_POS_0 | DRUM_POS_0 |
| Position must be | fresh + known + stable | fresh + known + stable | fresh + known + stable |
| Motor must not be moving | Yes | Yes | Yes |
| All MCP KNOWN_OFF required | Yes | Yes | Yes |
| Valve conflict check | Yes (excludes source_inlet, transfer) | No | No |
| Blocked by fault/emergency | Yes (ON only) | Yes (ON only) | Yes |

OFF requests bypass all interlock checks (always allowed).

### 1.3 Active-State Validation

`safety_validate_active()` has been extended to support `SAFETY_OP_DRAIN_VALVE`, `SAFETY_OP_DETERGENT_PUMP`, and `SAFETY_OP_UV`.

**DRAIN_VALVE active validation:**
- Self output must be KNOWN_ON
- Position must be valid, fresh, stable, DRUM_POS_180
- Motor must not be moving
- GPA0 (source_inlet) and GPA1 (transfer) must be KNOWN_OFF
- Blocked by fault/emergency

**DETERGENT_PUMP active validation:**
- Self output must be KNOWN_ON
- Position must be valid, fresh, stable, DRUM_POS_0
- Motor must not be moving
- Blocked by fault/emergency

**UV active validation:**
- Self output must be KNOWN_ON
- Position must be valid, fresh, stable, DRUM_POS_0
- Motor must not be moving
- Blocked by fault/emergency

All three services call `safety_manager_validate_active()` periodically during RUNNING state. If validation fails, the service initiates an emergency stop: output OFF via `safety_manager_apply()`, then reports fault.

**Note:** UV default duration is 10 minutes (600000ms). Active-state validation is mandatory — "short duration" is not a valid safety argument.

### 1.4 Emergency Stop Coverage

All three operations are explicitly included in the emergency shutdown sequence (safety_manager.c lines 906-923). Each output is individually closed with per-output known/unknown tracking.

### 1.5 Baseline Establishment

`safety_manager_start()` writes OFF to all 6 outputs including GPA2/GPA3/GPB7. Success marks KNOWN; failure marks UNKNOWN and latches FAULT_MCP_IO.

### 1.6 Lifecycle Safety (R3)

- **Stop timeout**: Deterministic join via task_done_sem (StaticSemaphore_t). Timeout returns ESP_ERR_TIMEOUT, preserves resources. Lifecycle stays STOPPING for retry.
- **Stop drain**: Task exits only after active request has terminal published.
- **Queue-full emergency**: Atomic mailbox. API caller saves OFF result; service task consumes. No cross-task FSM writes.
- **run_async TOCTOU**: atomic_compare_exchange_strong for reservation.
- **Terminal retry**: pending_terminal_t persists terminal event. Each tick retries publish. No silent loss.
- **Chain limit**: MAX_CHAIN_ACTIONS=8 triggers FAULT_INTERNAL + emergency OFF.

---

## 2. GPIO Ownership Audit

### 2.1 Pin Mapping

| Pin | Expander | Address | IODIR | Whitelist | Current Owner |
|---|---|---|---|---|---|
| GPA2 (UV) | MCP23017 #0 | 0x20 | OUTPUT | MCP_OUTPUT_WHITELIST_A | **NONE** |
| GPA3 (Drain) | MCP23017 #0 | 0x20 | OUTPUT | MCP_OUTPUT_WHITELIST_A | **NONE** |
| GPB7 (Detergent) | MCP23017 #0 | 0x20 | OUTPUT | MCP_OUTPUT_WHITELIST_B | **NONE** |

All three pins are whitelisted and configured as outputs. No service currently owns them.

### 2.2 MCP Known-State Tracking

The safety_manager maintains `mcp_known_mask` (bitmask per safe_output_t) and `mcp_outputs` struct. Both layers (MCP shadow OLAT + safety_manager shadow) are consistent:
- Successful write → KNOWN
- I2C failure → UNKNOWN + FAULT_MCP_IO latch
- Init state: all UNKNOWN until baseline established

### 2.3 HAL Access Path

The ONLY authorized path is:
`service → safety_manager_apply() → apply_mcp_output() → hal->set_safe_output() → mcp23017_set_output()`

Direct MCP HAL calls are forbidden by architecture. All three services will use this path exclusively.

---

## 3. Historical Issue Assessment

### Issue H1: GPIO11 VDD_SPI misidentification (Phase 0)
**Severity:** P2 (documentation)
**Blocks Group C?** NO. GPIO11 is not used by any Group C service.

### Issue H2: sdkconfig.defaults manual editing vs idf.py set-target (Phase 0)
**Severity:** P1 (workflow)
**Blocks Group C?** NO. Build verification will use `idf.py build` correctly. Group C does not modify sdkconfig.defaults.

### Issue H3: fake_hal singleton init order (Phase 1)
**Severity:** P1 (fixed)
**Blocks Group C?** NO. Already fixed. Group C services use the same init/start pattern as dry/bl50.

### Issue H4: Coredump sizing based on max task stack (Phase 0)
**Severity:** P2 (resource planning)
**Blocks Group C?** NO. Three new tasks at 4096 stack each = 12KiB additional. Within current coredump budget.

### Issue H5: NVS blob CRC32 validation (Phase 1)
**Severity:** P1 (fixed)
**Blocks Group C?** NO. Already fixed. Group C reads config via `machine_config_get()`.

### Issue H6: SAFETY_OP_FAN gap: no fault/emergency intercept for active-state (Phase 6 Group A)
**Severity:** P2 (design gap, same as 1.3 above)
**Blocks Group C?** NO. Same reasoning as 1.3. Short-duration services don't need continuous active-state monitoring.

**Result: 0 P0 blocking issues, 0 unhandled P1 blocking issues.**

---

## 4. Group C Component Design

### 4.1 drain_service

- **Purpose:** Timed drain valve operation at DRUM_POS_180
- **Output:** GPA3 via SAFETY_OP_DRAIN_VALVE
- **FSM States:** IDLE → VALIDATING → RUNNING → STOPPING → COMPLETE | CANCELING → COMPLETE | FAULT
- **Config:** `drain_params_t` (max_duration_ms)
- **Terminal:** MACHINE_EVENT_DRAIN_DONE
- **Cancel/Skip API:** `drain_service_cancel(request_id)`

### 4.2 detergent_service

- **Purpose:** Timed detergent pump operation at DRUM_POS_0
- **Output:** GPB7 via SAFETY_OP_DETERGENT_PUMP
- **FSM States:** IDLE → VALIDATING → RUNNING → STOPPING → COMPLETE | CANCELING → COMPLETE | FAULT
- **Config:** `detergent_params_t` (demo_duration_ms, formal_duration_ms, max_single_ms, ml_per_second)
- **Terminal:** MACHINE_EVENT_DETERGENT_DONE
- **Cancel/Skip API:** `detergent_service_cancel(request_id)`

### 4.3 uv_service

- **Purpose:** Timed UV lamp operation at DRUM_POS_0 with skip/cancel
- **Output:** GPA2 via SAFETY_OP_UV
- **FSM States:** IDLE → VALIDATING → RUNNING → STOPPING → COMPLETE | CANCELING → COMPLETE | SKIPPED → COMPLETE | FAULT
- **Config:** `uv_params_t` (default_duration_ms, max_duration_ms)
- **Terminal:** MACHINE_EVENT_UV_DONE
- **Cancel/Skip API:** `uv_service_cancel(request_id)`, `uv_service_skip(request_id)`

---

## 5. Exit Gate Criteria

- All three services use safety_manager_apply() exclusively
- No direct MCP HAL calls
- All three support cancel/skip API for future BTN3 integration
- All three follow two-layer FSM + service pattern
- All three support terminal event exactly once
- All three support busy request linearization
- 0 P0, 0 unhandled P1
