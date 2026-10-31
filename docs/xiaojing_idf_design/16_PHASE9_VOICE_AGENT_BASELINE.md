# Phase 9 Voice Agent Baseline

## Status

This phase establishes the bounded voice-to-machine framework. It does not
claim that either wake model or cloud TTS is already deployed.

## Independent control axes

1. Wake backend
   - Edge Impulse: custom `小净小净` model.
   - ESP-SR: Espressif wake model.
   - BTN1 long press requests a switch only while the wash executor is IDLE.
   - A switch is rejected until a real backend selector is wired.
2. Invocation source
   - Wake word starts AUTO routing.
   - BTN2 long press/release starts and ends push-to-talk.
3. Cloud route policy
   - AUTO may route chat, status, clarification, or a bounded wash intent.
   - CHAT_PRIORITY is read-only. Status is allowed; mutating wash commands
     require a separate confirmation and are not executed immediately.

These axes are deliberately orthogonal. Changing the wake backend does not
change the cloud provider and does not grant machine permissions.

## End-to-end boundary

```text
INMP441 -> 16 kHz mono WAV -> HTTPS cloud route
         -> bounded JSON parser -> local authorization
         -> wash_planner -> wash_executor -> safety_manager -> HAL
```

Cloud output never contains raw GPIO, PWM, relay, valve, pump, heater, motor,
or posture operations. Unknown, duplicate, malformed, or inconsistent JSON
fields fail closed.

## AI Gateway credentials

AI Gateway uses one gateway API key:

```http
Authorization: Bearer <XIAOJING_VOICE_API_KEY>
```

The request also contains `model`. Firmware defaults it to `mimo-v2.5`; it
is a non-secret routing name, not another credential. If the console exposes
a different alias, it can be overridden without changing the auth model.

Create the ignored local file `main/include/voice_secrets.h`:

```c
#pragma once
#define XIAOJING_VOICE_PROVIDER VOICE_PROVIDER_ESPRESSIF_AI_GATEWAY
#define XIAOJING_VOICE_API_KEY  "replace-with-gateway-key"
/* Optional: #define XIAOJING_VOICE_MODEL "console-model-alias" */
```

The firmware sends requests to:

```text
https://ai-gateway.vei.volces.com/v1/chat/completions
```

MiMo direct remains a selectable development path:

```c
#define XIAOJING_VOICE_PROVIDER VOICE_PROVIDER_MIMO_DIRECT
#define XIAOJING_VOICE_API_KEY  "replace-with-mimo-key"
#define XIAOJING_VOICE_MODEL    "mimo-v2.5"
```

MiMo audio input uses a WAV data URL:

```text
data:audio/wav;base64,<payload>
```

## Enablement prerequisites

- `CONFIG_XIAOJING_VOICE_ENABLED=y`
- valid Wi-Fi credentials provisioned over BLE or already stored in the
  `xj_wifi` NVS namespace
- ESP32-S3 N16R8 with healthy PSRAM
- INMP441 connected to the configured I2S RX pins
- one Gateway API key

FAKE output mode remains safe for bare-board development. Actuator outputs stay
inside `fake_hal`, while microphone input uses the independent
`audio_driver_init_capture_only()` path. Capture-only mode creates I2S RX only:
it does not configure the amplifier, I2S TX, or GPIO47. Unit tests continue to
use fake recording/cloud callbacks and do not need a microphone.

## Implemented now

- bounded PSRAM WAV recorder and lifecycle
- HTTPS upload, cancellation, timeouts, response-size bounds
- MiMo `api-key` and Gateway Bearer authentication
- AUTO route schema and CHAT_PRIORITY policy
- strict local parsing and authorization
- wash planner/executor gateway
- BTN1/BTN2 gesture contracts
- stable prompt IDs and TTS asset placeholders
- secret file ignored by Git
- BLE Wi-Fi provisioning (`provision_wifi`) with bounded SSID/password input
- missing-key DORMANT mode: BLE/local control still boots and advertises
- Gateway key-only default: `mimo-v2.5` is selected automatically unless a
  non-secret model alias override is supplied

## Deliberately pending

- Edge Impulse inference adapter
- ESP-SR inference adapter and concrete supported wake model
- real prompt audio assets
- streaming/cloud TTS playback
- first authenticated Gateway call with the user's actual key
- optional Volcengine hardware conversational-agent Realtime integration

The hardware conversational-agent product is an advanced, separate protocol.
It is not required for the baseline AI Gateway request and must not be treated
as additional Gateway authentication.

## Bring-up sequence

1. Add `voice_secrets.h` containing the Gateway API key.
2. In the mini-program Profile page, provision Wi-Fi over the existing BLE V1
   session. The firmware replies `WIFI_CONNECTING` and stores the credentials
   in NVS without echoing the password.
3. Enable voice in menuconfig and build.
4. Verify boot without exposing the key in logs.
5. Use BTN2 PTT for the first cloud test; it does not depend on a wake model.
6. Verify chat, status, confirmation-required, and allowed program routes.
7. Bind Edge Impulse, then ESP-SR, and only then enable BTN1 switching.
8. Add prompt/TTS audio and run long-duration capture/upload tests.
