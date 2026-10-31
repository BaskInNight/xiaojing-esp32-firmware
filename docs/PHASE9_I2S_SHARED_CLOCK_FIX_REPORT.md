# Phase 9 I2S TX/RX shared-clock fix

## Scope

MAX98357A playback and INMP441 capture share GPIO15 (WS) and GPIO16 (BCLK).
The fix keeps the no-hardware-change architecture from the handover document:
playback is muted after a prompt, and capture RX is recreated at its recovery
boundary instead of reusing stale DMA state.

## Production changes

1. `components/audio_driver/audio_driver.c`
   - Added `tx_channel_enabled`, distinct from `output_enabled` (AMP mute).
   - A repeated prompt no longer calls `i2s_channel_enable()` on an already
     enabled TX channel (`ESP_ERR_INVALID_STATE`, 0x103).
   - TX remains alive while muted to preserve the shared clock domain.
   - Capture-only RX disables and deletes the old channel before recreating it
     on the next start, clearing stale DMA/clock state after playback.

2. `components/voice_wake_esp_sr/src/voice_wake_esp_sr.c`
   - Model init/deinit worker now uses a reusable static internal-RAM stack and
     TCB. Backend switching no longer depends on a contiguous internal-heap
     allocation after AFE/Wi-Fi have fragmented the heap.

## Evidence

Firmware r269 was built with the autonomous test enabled and flashed to COM8.
Twenty cycles completed:

- 20/20 backend switches: `err=0x0`
- 20/20 tone/playback operations: `err=0x0`
- 20/20 RX restart calls: `err=0x0`
- `PHASE9_AUTOTEST COMPLETE`
- No `0x101`, `0x103`, Guru Meditation, assertion, WDT, or reset marker

The normal r270 firmware was then rebuilt with the autonomous task disabled and
flashed to COM8:

- Build: PASS, binary `0x3f93b0`, 7% app partition free
- Flash: PASS, esptool hash verified
- Boot: BLE controller/advertising initialized; ESP-SR AFE initialized;
  VoiceSvc started; no crash/reset in the capture window

The 20-cycle logs are `phase9_r269_20cycle.log` and
`phase9_r269_20cycle_tail.log`. The production boot capture is
`phase9_r270_boot.log`.

## Remaining hardware note

The firmware is ready for normal two-backend use. A physical speaker/mic test
is still needed to judge acoustic output and wake sensitivity; the software
state-machine and shared-clock recovery path are covered by the automated
cycle evidence above.
