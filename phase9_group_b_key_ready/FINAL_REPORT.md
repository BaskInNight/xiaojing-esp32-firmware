# Phase 9 Group B - Runtime Provider/Credential Baseline

Date: 2026-08-01

## Gate

`PHASE9_RUNTIME_CREDENTIAL_GATE: PASS`

This gate proves that the bounded audio/network/cloud/control framework builds,
boots safely without credentials, accepts an encrypted and physically
authorized credential update, and preserves all existing board regressions. It
does not claim a live cloud request or microphone electrical test before a real
API key and INMP441 are available.

## Provider policy

- Default provider: direct MiMo (`mimo_direct`).
- Default model: `mimo-v2.5`.
- Optional provider: Espressif AI Gateway (`ai_gateway`).
- The Gateway is not a boot, runtime, or update dependency.
- Provider, model, and API key are runtime configuration; firmware reflashing is
  not required when changing them.
- Endpoints are firmware allowlisted. The mini-program cannot inject an
  arbitrary URL.

With no key, voice cloud remains `DORMANT`; BLE, local control, the wash
executor, and all safety services continue to run.

## Credential transport and lifecycle

- New BLE credential characteristic: UUID `FFF3`, encrypted-write only.
- Existing machine protocol remains on `FFF1/FFF2` and is wire-compatible.
- NimBLE bonding and Secure Connections are enabled.
- First boot without a key opens a five-minute credential window.
- Later maintenance requires a 60-second physical-presence window opened by a
  BTN1 long press.
- Successful save emits a redacted ACK and schedules a reboot.
- The key is never returned in snapshots, ACKs, logs, or mini-program storage.
- Corrupt stored credentials fail to voice-DORMANT and do not stop local washer
  bootstrap.

Current prototype limitation: NVS encryption and flash encryption are not yet
enabled, so the key is encrypted in transit but is plaintext at rest to a
physical flash reader. Production hardening must enable encrypted NVS/flash and
use a provisioned device security policy. BLE uses Just Works encryption (no
MITM-authenticated passkey); the physical authorization window is therefore a
required additional control, not optional UI.

## Mini-program

- `我的` page can select direct MiMo or optional Gateway, enter model and key,
  and send the credential over `FFF3`.
- The API key input is password-masked and never persisted with `wx.setStorage`.
- Credential commands are not automatically replayed after an ACK timeout.
- Version: `voice-provider-p9-20260801-001`.

## Verification

- Host C: 272/272 PASS.
- Python host: 23/23 PASS.
- Runner offline: 16/16 PASS.
- Mini-program: BLE protocol 8/8, BLE transport 19/19, booking 23/23,
  page contracts 68/68 PASS.
- Main build: PASS, app size `0x1547d0`, 67% partition free.
- Unit build: PASS, app size `0x133540`, 70% partition free.
- Source and ELF registered Unity tests: 760/760.
- Diagnostic: 745 PASS / 0 FAIL / 15 IGNORE / 760.
- Formal run 1: 745 / 0 / 15 / 760.
- Formal run 2: 745 / 0 / 15 / 760.
- Formal run 3: 745 / 0 / 15 / 760.
- Every run: one BEGIN, one COMPLETE, zero crash/reset signatures.
- All 15 ignored cases are pre-existing real-peripheral `[hw]` gates.

The board runner was also corrected to configure RTS/DTR before opening COM7
and use an EN-only Espressif HardReset. This prevents accidental download mode
and removes the need to press EN manually.

## Main-firmware smoke test

The current main firmware was restored to COM7 after unit verification.

- ESP32-S3 PSRAM: 8 MB, 80 MHz, memory test OK.
- output mode: FAKE.
- Wi-Fi absent: local/BLE control remains available.
- voice: `DORMANT (ADD API KEY)`.
- encrypted credential window: open for five minutes.
- BLE advertising: `JJTP-XIAOJING`.
- bootstrap: complete; no Guru/WDT/brownout/reset.

## Artifact hashes

- Main BIN: `EBEA18D3F285AD32CF9909A1ECF36C948788F255539934DC75D17396A424680B`
- Main ELF: `36BB360CD75D3DE0BFA2A5FA124CDA944EFD1F78362FBE5C70240B67F7644C23`
- Unit BIN: `BBFDBA0C7BB942A82F7F7F463062E96AF0CA123F64FB31B818E163B208781541`

## Remaining live checks

- `MIMO_LIVE_AUTH: BLOCKED_BY_API_KEY`
- `SECURE_GATT_PHONE_PROVISION: READY_FOR_USER_ACCEPTANCE`
- `INMP441_CAPTURE_RUNTIME: BLOCKED_BY_MICROPHONE_CONNECTION`
- `EDGE_IMPULSE_WAKE_ADAPTER: DEFERRED_ADVANCED`
- `ESP_SR_WAKE_ADAPTER: DEFERRED_ADVANCED`
- `TTS_AUDIO_ASSETS: PLACEHOLDER_APPROVED`

After adding a MiMo key, the first live voice test should use BTN2
push-to-talk, independently verifying capture, direct cloud transport, intent
routing, and local planner authorization before either wake-word backend.
