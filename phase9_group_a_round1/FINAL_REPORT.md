# Phase 9 Group A Round 1 — Final Report

Date: 2026-07-30

## Scope

Phase 9 software-only voice chain. No microphone, amplifier, speaker, wake
model, Wi-Fi cloud request, or real actuator was used by the Unit gate.

## Implementation

- Added `voice_agent` component:
  - pure-C deterministic `voice_core`;
  - bounded strict JSON `voice_tool_parser`;
  - FreeRTOS `voice_service` orchestration and snapshots.
- Tool allowlist routes only through `wash_planner` and `wash_executor`.
- Direct GPIO/MCP/PWM/motor/valve/heater tools are rejected.
- Added BTN1 2-second long press and one-shot backend toggle semantics.
- Short BTN1 click and BTN3 urgent behavior remain unchanged.
- Added independent audio RX/TX lifecycle APIs.
- Voice startup is gated by `CONFIG_XIAOJING_VOICE_ENABLED`; disabled builds
  no longer report a fake running voice service.
- Added authoritative Unity final counters so concurrent UART log text loss
  cannot be confused with skipped tests.

## Software Chain Proven

```text
Fake wake
  -> recording adapter
  -> generated cloud response
  -> strict tool parser
  -> wash planner
  -> wash executor
  -> accepted/rejected prompt adapter
  -> frontend restart
```

Cancel and safety-fault events preempt the chain. Backend switching stops the
old frontend, selects the target backend, plays a prompt, and restarts; a
selection failure rolls back.

## Verification

- Main ESP-IDF build: PASS, app size `0xaaf60`, 83% partition free.
- Unit ESP-IDF build: PASS, app size `0xadb60`.
- Host C: 235/235 PASS.
- Python host: 23/23 PASS.
- Runner offline: 16/16 PASS.
- Safety bypass scan: PASS, no new bypass.
- GPIO map scan: PASS.
- Board Unit diagnostic: 709 PASS / 0 FAIL / 15 HW IGNORE / 724 TOTAL.
- Board formal run 1: 709 / 0 / 15 / 724.
- Board formal run 2: 709 / 0 / 15 / 724.
- Board formal run 3: 709 / 0 / 15 / 724.
- All formal runs: one BEGIN, one COMPLETE, no crash, no spontaneous reset.
- Unit binary SHA-256:
  `65A4D495D8A8241D3FD2D8EA1F0F69B1AFE33C1DB4966EFE956999323F71E5AB`
- Unit ELF SHA-256:
  `D396D6D5C0A207113144BC266A5A5FE27B6FF2D13797F088184C26506E86B292`

## Current Board State

Latest main firmware restored on COM7. Boot verification:

- PSRAM 80 MHz memory test: OK.
- BLE `JJTP-XIAOJING`: advertising.
- Button service: OK.
- Voice service: DISABLED by Kconfig, as expected until real adapters exist.

## Deferred to Later Phase 9 Groups

- Group B: Wi-Fi/provisioning boundary, verified TLS cloud client, generated
  WAV upload, network error handling, BLE+Wi-Fi coexistence.
- Group C: storage prompt migration, native prompt player, weather/music/TTS.
- Group D: ESP-SR AFE/WakeNet/MultiNet, 16 kHz Edge model, microphone and
  speaker measurements, enclosure tuning and physical audio soak.

## Gates

```text
PHASE9_GROUP_A_ROUND1_EXIT_GATE: PASS
SOFTWARE_CONTROL_CHAIN: PASS
REAL_AUDIO_CHAIN: NOT_STARTED
PHASE9_FULL_EXIT_GATE: NOT_YET
READY_FOR_PHASE9_GROUP_B: YES
READY_FOR_PHASE10: NO
```
