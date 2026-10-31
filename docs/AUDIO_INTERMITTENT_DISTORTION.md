# Intermittent speaker distortion investigation

Both fixed prompts and cloud speech were reported as intermittently noisy.
Audition files are preserved; no voice style or gain changes were made here.

## Software findings

Playback uses I2S0, 16-bit stereo slots; the microphone uses a retained I2S1
channel, 32-bit stereo slots. They share GPIO15/16 (WS/BCLK).
`audio_driver_resync_rx()` restores GPIO routing to I2S1 after every playback.
Previously `audio_driver_stop_tx()` only muted the amplifier and left I2S0
enabled. The next `start_tx()` skipped GPIO reconfiguration because that TX
channel was still enabled. Thus a later playback could have its serial data
clocked by the capture controller's differently sized slots, despite successful
DMA writes. This is a concrete routing defect, consistent with intermittent
distortion; listening comparison is required to establish its contribution.

The ESP-IDF 5.5.4 I2S channel defaults also disable DMA auto-clear. Without
auto-clear, producer pauses can replay old DMA samples. Successful writes only
confirm enqueueing, so immediately muting after the last write could truncate
the final queued speech.

## Changes

- Stop standalone playback TX while muted, preserving its DMA allocation.
  The next playback restores TX GPIO routing; legacy same-controller duplex
  retains its clock requirement.
- Enable `auto_clear_after_cb` so empty TX buffers send silence.
- Preload silence while muted before restarting the amplifier/TX channel.
- Queue more than a DMA ring of silent samples before normal playback stops.
  Cancellation/error still stops immediately.
- Replace the 10ms busy wait with FreeRTOS `vTaskDelay`.
- Log source PCM peak/clipped sample counts and longest producer gap, to help
  distinguish clipping or scheduling starvation from GPIO routing failures.

## Validation

Compare repeated wake prompts, PTT prompts, cloud replies, and a prompt after
each cloud reply. Logs should show TX stopped before RX resync, followed by
TX output enabled on every new playback. A zero clipped count rules out digital
full-scale saturation of the source, not analog amplifier/power clipping.
If distortion remains with these transitions correct, inspect speaker load,
power rail under playback, ground continuity, and BCLK/WS/DIN with instruments.
No voltage, load, or wiring changes are authorized by this software fix.

## Board observations after flashing

ESP-IDF build/size passed; incremental compilation had no warnings. Application
image: 4,152,192 bytes, with 304,256 bytes left in the application partition.
DIRAM: 229,163 / 341,760 bytes (67.05%). COM8 app flash and hash verification passed.

The reset capture is in `tmp/audio_diagnostics_after_clock_fix.log` (filtered and
local credentials redacted by `tools/monitor_audio_diagnostics.py`).

- Boot B: 71,680 PCM bytes at 16kHz, 2,316ms including silent drain,
  peak 18,956, zero clipped samples, longest producer gap 22us.
- Wake A1 after RX resumed: 51,200 PCM bytes at 16kHz, 1,664ms including
  silent drain, peak 22,373, zero clipped samples, longest producer gap 17us.
- Both returned ESP_OK, stopped TX, then resynchronized RX. Wake recording
  started only after prompt playback ended.
- Thinking cue: 2,467ms, peak 19,386, zero clipped source samples, longest
  producer gap 227us. Subsequent cloud TTS returned HTTP200 and played
  138,240 output PCM bytes at 16kHz in 4,387ms (source 24kHz), ESP_OK,
  source peak 15,909, zero clipped source samples, longest producer gap
  1,821us. TX stop and RX resync also succeeded after this cloud response.
- Startup still reports shared GPIO15/16 allocation warnings, a core-dump data
  validation error, ESP-SR CPU/cache recommendations, and missing turbidity ADC
  calibration. No new panic was observed in this initial capture. The GPIO
  warnings reflect the existing two-controller shared-pin architecture; the
  runtime routing handoff now occurs on every observed playback.

These logs verify software transitions, not the analog waveform or subjective
sound quality. Further repeated user listening remains needed to confirm
whether all reported distortion is resolved.
