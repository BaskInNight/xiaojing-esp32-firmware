# Phase 9 Round 0 — voice_agent Architecture Audit

Date: 2026-07-30

## Scope

This round selects the Phase 9 technical route. It does not connect a
microphone, amplifier, speaker, display, motor, valve, pump, fan, UV lamp, or
PTC load. Hardware-dependent verification remains `DEFERRED_HARDWARE`.

## Current Facts

- Target: ESP32-S3-WROOM-1 N16R8, ESP-IDF 5.5.4, 16 MiB flash, 8 MiB PSRAM.
- NimBLE is enabled and Bluedroid is disabled. The Phase 8 FFF0/FFF1/FFF2
  protocol and phone reconnection path are already operational.
- The IDF tree already contains a native `audio_driver` using
  `driver/i2s_std.h`, INMP441 RX, MAX98357 TX, and an AMP_SD hardware mute.
- `audio_driver` currently creates RX but never enables it. It also has no
  explicit RX lifecycle/state contract. It is not ready for production
  capture without correction and lifecycle tests.
- The 16 MiB partition table reserves `storage` = `0x6E0000` (7040 KiB) for
  voice/display assets.
- Old Arduino assets:
  - `LocalAudio.h`: 1,476,180 bytes of Base64-embedded prompts.
  - `esp32_raw_audio.wav`: 94,550 bytes.
  - Edge Impulse compiled model source: 40,548 bytes.
  - Edge labels: `noise`, `unknown`, `xiao_jing`.
  - Edge sample frequency: 16062 Hz, not the 16000 Hz used by ESP-SR and the
    cloud recording path.
- Old cloud code contains useful product behavior, but also Arduino
  `WiFiClientSecure`, `setInsecure()`, hand-written HTTP framing, large
  Base64/JSON copies, blocking I2S writes, and direct pipeline translation.
  These wrappers must not be copied into the new IDF architecture.

## Technology Decisions

| Technology | Decision | Reason |
|---|---|---|
| Existing synthesized prompts | REUSE AS SOURCE ASSETS | The wording and voice are already product-tested. Decode offline to WAV/PCM files and place them in `storage`; do not keep the 1.4 MiB Base64 C header in the app image. |
| Existing Edge Impulse wake model | A/B BASELINE ONLY | It preserves an existing “小净” baseline, but its 16062 Hz input does not match the new 16000 Hz pipeline and its bundled SDK expands maintenance surface. Do not make it the production default without retraining/re-exporting at exactly 16 kHz. |
| ESP-SR AFE | ADOPT | Native ESP32-S3 AFE provides VAD/noise suppression and integrates WakeNet. It consumes the same 16 kHz, signed 16-bit mono format required by the new capture/cloud boundary. |
| ESP-SR WakeNet9/9l | PRODUCTION TARGET | Optimized for ESP32-S3, integrated with AFE, and has a maintained resource/flash model. A custom “小净” model must be obtained/trained and verified in the final enclosure. |
| ESP-SR MultiNet | ADOPT AS OFFLINE FALLBACK | Maps a small fixed command allowlist to product intents when the network is unavailable. It must never emit GPIO, heater, valve, motor, or posture commands directly. |
| Full ESP-ADF framework | NOT IN PHASE 9 CORE | ADF v2.8 supports IDF 5.5, but it introduces additional tasks, ring buffers, board HAL assumptions, and lifecycle surface into a controller that already has a verified task model. Keep it as an optional later media pipeline. |
| `espressif/esp_player` | ADOPT LATER FOR MUSIC/STREAMED TTS | It supports IDF >=5.3 and provides maintained demux/decode/playback. Use it for MP3/AAC/HTTPS music after the basic prompt and voice path is stable. Simple local PCM prompts should use the small native player first. |
| ESP-RainMaker | DO NOT ADOPT NOW | RainMaker is an end-to-end device cloud/control platform. The product already has a WeChat mini-program, BLE V1 protocol, order backend, and planned MiMo tools. Adding RainMaker would duplicate identity, provisioning, state, and cloud ownership without improving speech recognition. Re-evaluate only if a future product requirement demands RainMaker fleet management. |
| Provisioning | ADOPT SECURE SOFTAP PROVISIONING | Use Espressif unified/network provisioning with proof-of-possession. Prefer SoftAP because the existing NimBLE GATT service is a product protocol and must remain stable. Do not run a second independent BLE provisioning GATT stack beside FFF0/FFF1/FFF2. |

## Recommended Product Voice Model

Use a hybrid local/cloud design:

1. Local AFE continuously processes short 16 kHz frames.
2. WakeNet wakes on “小净”.
3. Local VAD bounds the utterance and stops recording.
4. MultiNet handles a deliberately small offline command set:
   - start the default/demo program;
   - cancel/stop;
   - query current status;
   - acknowledge load/unload only when the executor phase permits it.
5. Open conversation, weather, program explanation, and music selection go to
   the cloud agent.
6. Cloud output is parsed into an allowlisted intent schema.
7. `wash_planner` validates the intent.
8. `wash_executor` remains the only whole-machine flow owner.
9. Spoken confirmation happens only after planner/executor acceptance.

The cloud agent can request weather and music metadata, but cannot request
`heater_on`, `valve_on`, GPIO writes, raw motor motion, or posture changes.

## Interaction Mode

Phase 9 starts half-duplex:

- Wake detection is disabled while prompt/TTS/music output is active.
- AMP_SD is asserted only around actual playback.
- Playback is stopped immediately on BTN3, safety fault, or executor abort.
- Full-duplex barge-in/AEC is deferred until microphone, amplifier, speaker,
  enclosure, and digital playback-reference routing are physically verified.

This avoids self-wake and echo loops before AEC has real acoustic calibration.

## Component Boundaries

### `audio_driver`

Owns only I2S RX/TX and AMP_SD.

- Add explicit `start_rx`, `stop_rx`, `start_tx`, and `stop_tx`.
- Track RX and TX state independently.
- Make stop/destroy idempotent and timeout-safe.
- No cloud, JSON, wake model, planner, or prompt selection.

### `voice_frontend`

Adapter over ESP-SR AFE/WakeNet/MultiNet.

- Input/output: fixed-size 16 kHz signed 16-bit mono frames.
- Exposes wake, VAD, and optional offline-command events.
- Model partition and model selection stay outside the app binary.

### `voice_core`

Pure C deterministic state machine:

```text
DISABLED
IDLE
LISTENING
WAKE_DETECTED
RECORDING
UPLOADING
WAITING_CLOUD
PARSING
SUBMITTING
SPEAKING
ERROR
```

It owns no FreeRTOS handle, I2S handle, HTTP client, JSON DOM, or dynamic
allocation.

### `voice_cloud_client`

- Uses `esp_http_client`/`esp-tls`.
- Uses the ESP certificate bundle or an explicitly pinned CA.
- Never uses an insecure TLS mode.
- Applies bounded connect/read/overall timeouts.
- Does not log credentials, bearer tokens, complete recordings, or full
  Base64 payloads.
- Prefer bounded streaming/chunked request construction. If the provider
  requires Base64 inside JSON, allocate bounded PSRAM buffers and avoid
  simultaneous PCM + WAV + Base64 + full JSON duplicates where possible.

### `voice_tool_parser`

- Parses into `wash_intent_t` or a non-hardware assistant action.
- Enforces schema, UTF-8 length, array count, enum, duration, and numeric
  overflow limits.
- Has no service/driver includes.
- Weather and music tools are separate from wash intent tools.

### `voice_service`

- Owns tasks, queues, cancellation, and adapters.
- Cloud/dialogue task priority target: 5.
- Audio feeder performs only bounded frame transfer; no HTTP or parser work.
- It must remain below safety/position/service priorities.
- BTN3 and safety fault preempt every phase.

### `prompt_player`

- Starts as a small PCM/WAV reader from the FAT `storage` partition.
- Uses `audio_driver` and bounded buffers.
- Later, `esp_player` is added for MP3/AAC/HTTPS music and streamed TTS.

## Asset Migration

1. Extract every `AUDIO_*` Base64 blob from `LocalAudio.h` on the host.
2. Decode and validate RIFF/WAVE headers offline.
3. Normalize prompts to 16 kHz, 16-bit PCM, mono unless playback quality
   requires a separately documented rate.
4. Generate a manifest containing name, SHA-256, format, sample rate,
   channels, byte length, and duration.
5. Pack files into `storage`; application code refers to symbolic asset IDs.
6. Keep the old source file read-only as migration evidence. Do not compile it
   into the IDF app.

## Core/Memory Rules

- Safety and position timing always outrank voice.
- No busy loops on Core 0; NimBLE, Wi-Fi, TCP/IP, and TLS already consume it.
- Task pinning is selected after profiling, not copied from Arduino.
- Record minimum internal heap, PSRAM free space, largest block, and task
  high-water marks at every voice phase transition.
- Audio DMA and control structures stay in internal DMA-capable memory.
- Large recordings, encoded request buffers, and decoded media use bounded
  PSRAM allocations.
- A voice/network failure cannot change actuator state.

## Provisioning Route

Development can continue with existing credentials, but production must use:

1. NVS-backed Wi-Fi credentials.
2. Secure SoftAP provisioning with proof-of-possession.
3. Explicit provisioning/reset state and rate limits.
4. No secrets in source control or logs.
5. The mini-program may guide users through provisioning, but provisioning
   transport remains independent of the Phase 8 machine-control GATT service.

## Phase 9 Implementation Groups

### Group A — software-only core

- Fix and test `audio_driver` lifecycle without requiring real audio quality.
- Implement pure-C `voice_core`.
- Implement strict tool parser and fixtures.
- Add Fake frontend/cloud/player adapters.
- Host tests for all state/error/overflow/preemption paths.

### Group B — cloud path on the bare ESP32-S3

- Wi-Fi manager and secure provisioning boundary.
- `esp_http_client` + verified TLS.
- Upload a generated/reference WAV, parse tool response, submit to planner.
- Test HTTP 4xx/5xx, TLS failure, timeout, disconnect, truncated JSON.
- Verify simultaneous BLE connection and Wi-Fi traffic.

### Group C — playback and assistant tools

- Migrate local prompt assets into `storage`.
- Native WAV/PCM prompt player.
- Weather tool through an allowlisted backend.
- Add `esp_player` for music/streamed TTS only after prompt playback is stable.
- Playback priority: safety prompt > interaction prompt > TTS > music.

### Group D — physical audio, deferred

- INMP441 capture polarity/channel/level.
- Wake word false accept/false reject tests in enclosure.
- MAX98357 and speaker playback.
- Gain, noise suppression, VAD, and acoustic tuning.
- AEC/barge-in evaluation.
- Long-duration real audio soak.

## Required Tests Before Software Gate

- Pure core state and every invalid transition.
- Wake/cancel/fault collision priority.
- Recording max duration and buffer overflow.
- Parser fuzz corpus and malformed UTF-8/JSON.
- Forbidden hardware tool rejection.
- Planner reject/accept response ordering.
- TLS verification and all network error classes.
- BLE connected during repeated generated-WAV cloud requests.
- Heap/PSRAM/stack stability.
- Prompt asset manifest and hash verification.
- No secret/audio payload leakage in logs.

## Gate Policy

```text
PHASE9_ROUND0_AUDIT: PASS
PHASE9_SOFTWARE_IMPLEMENTATION: NOT_STARTED
PHASE9_SOFTWARE_GATE: NOT_YET
PHASE9_AUDIO_HARDWARE_GATE: DEFERRED_HARDWARE
READY_FOR_PHASE9_GROUP_A: YES
READY_FOR_PHASE10: NO
```

Phase 9 may proceed through Groups A-C without physical peripherals. It must
not claim the full Phase 9 exit gate until Group D hardware audio validation
and the required stability soak are complete.

## 2026-08-01 Runtime Provider Correction

The implemented Group B baseline does not require AI Gateway. Direct MiMo is
the default provider and AI Gateway is an optional allowlisted runtime choice.
Provider/model/API-key configuration is stored in NVS and is replaceable from
the mini-program over the encrypted `FFF3` GATT characteristic. Credential
writes require an active physical-presence window; the key is never echoed or
stored by the mini-program. Existing machine commands remain on `FFF1/FFF2`.

This prototype protects credentials in transit but does not yet enable NVS or
flash encryption. Production provisioning must add encrypted at-rest storage
and an authenticated device ownership policy. A corrupt voice credential is a
voice-only failure: it must leave washer safety and local/BLE control running.

## Official References

- ESP-SR ESP32-S3 getting started:
  https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/getting_started/readme.html
- WakeNet:
  https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/wake_word_engine/README.html
- MultiNet:
  https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/speech_command_recognition/README.html
- AFE/AEC:
  https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/acoustic_echo_cancellation/README.html
- ESP-ADF:
  https://github.com/espressif/esp-adf
- ESP Player:
  https://components.espressif.com/components/espressif/esp_player
- ESP RainMaker:
  https://docs.espressif.com/projects/esp-rainmaker/en/latest/esp32/
- ESP-IDF provisioning:
  https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/provisioning/index.html
