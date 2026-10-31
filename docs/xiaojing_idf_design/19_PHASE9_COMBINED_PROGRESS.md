# Phase 9 Combined: Progress Report

## Stage 0: Audit Complete ✅

### Current State
- Git status: Dirty (many untracked files from previous work)
- Voice agent components: Present and functional
- Legacy assets: 9/9 extracted
- MiMo prompts: 9/9 regenerated (encoding fixed)
- Build: PASS (0 errors, 0 warnings)

### Key Files SHA256
- voice_prompt_audio.c: Updated with proper mapping
- voice_tts_idf.c: Style prompt fixed (UTF-8 verified)
- synthesize_voice_prompts.py: Fixed with mojibake detection

## Stage 1: Prompt Encoding Fixed ✅

### 1.1 Encoding Fix
- **Problem**: Manifest and scripts had Windows console display issues (not actual mojibake)
- **Fix**: 
  - Used `ensure_ascii=True` in JSON output
  - Added `sys.stdout.reconfigure(encoding='utf-8')` for Windows
  - Added mojibake detection patterns
  - Verified all Chinese text is valid UTF-8

### 1.2 MiMo Prompts Regenerated
All 9 prompts re-synthesized with verified text:

| ID | Text | Duration | SHA256 (16 char) |
|----|------|----------|------------------|
| accepted | 好的，已开始处理。 | 2400ms | a706852e7ea7069b... |
| rejected | 这个请求暂时无法执行。 | 3200ms | c958358b22a5d7ef... |
| error | 抱歉，处理失败，请稍后重试。 | 3840ms | 84d04ee0c8183b7a... |
| backend_xiaojing | 已切换到小净唤醒模式。 | 3520ms | c16ac9c5ae17fc3d... |
| backend_esp_sr | 已切换到小爱同学唤醒模式。 | 3840ms | 4b8394b73601450a... |
| ptt_ready | 我在，请说。 | 1760ms | 01f461b74b3d1f47... |
| chat_thinking | 好的，让我想一想。 | 2560ms | c210989fd7a7c2b7... |
| need_confirmation | 请确认是否开始执行。 | 2880ms | c703e8a6f7bbd4c0... |
| network_unavailable | 网络暂不可用，请稍后重试。 | 3840ms | 328aa1f51a188b80... |

### 1.3 Audio Quality
- Sample rate: 24000Hz ✅
- Channels: 1 (mono) ✅
- Bits: 16 ✅
- Peak: 11632-28104 (good dynamic range)
- RMS: 1657-3675 (adequate volume)
- DC offset: 6-13 (minimal)
- Clipping: None ✅

### 1.4 Task Done Wiring
- Added `play_task_completion_prompt()` function
- Function calls `voice_prompt_audio_get_task_done()` to get legacy task_done.wav
- Plays via `real_hal_audio_play_pcm16()`
- **Status**: Function added, full wiring to wash completion event requires additional work

### 1.5 Mojibake Scan
- Manifest: PASS (0 patterns)
- C source: PASS (0 patterns)
- Python script: PASS (0 patterns)

## Stage 2-6: Remaining Work

### Stage 2: Unified Voice Frontend
**Status**: NOT STARTED

Required:
- Create `components/voice_frontend/` with unified wake backend interface
- Implement `voice_wake_backend_ops_t` with init/start/stop/deinit/get_snapshot
- Audio ownership model (NONE/WAKE_EDGE/WAKE_ESP_SR/PTT_CAPTURE/TTS_PLAYBACK)
- State machine (UNINITIALIZED/INITIALIZED/STARTING/LISTENING/TRIGGERED/STOPPING/STOPPED/FAULT)
- Linearization lock for owner switching
- Deterministic stop with EventGroup/task notification

### Stage 3: Edge Impulse Migration
**Status**: NOT STARTED

Required:
- Migrate Edge Impulse "小净小净" model from old project
- Implement 16kHz → 16062Hz resampler (linear interpolation)
- Threshold 0.80, cooldown mechanism
- C interface wrapper (no SDK type leakage)
- Host tests for resampler

### Stage 4: ESP-SR Integration
**Status**: NOT STARTED

Required:
- Add esp-sr component via Component Registry
- Enable wn9_xiaoaitongxue model only
- AFE configuration for single INMP441 mic
- 16kHz/PCM16/mono
- Model partition and Flash size tracking

### Stage 5: BTN1 Switching & Persistence
**Status**: NOT STARTED

Required:
- BTN1 long-press backend switching
- NVS persistence for selected backend
- Rollback on failure
- Display updates

### Stage 6: Testing & Verification
**Status**: NOT STARTED

Required:
- Host tests (UTF-8, manifest, WAV format, state machine)
- Unit tests (Fake PCM, lifecycle, switching)
- Build verification
- Board verification (if connected)

## Exit Gate Status

| Gate | Status |
|------|--------|
| PROMPT_UTF8_SOURCE | PASS |
| MOJIBAKE_SCAN | 0 |
| LEGACY_ASSETS | 9/9 |
| MIMO_PROMPTS_REGENERATED | 9/9 |
| MIMO_ASR_ROUNDTRIP | NOT_DONE |
| TASK_DONE_RUNTIME_WIRED | PARTIAL (function added, event hook pending) |
| EDGE_MODEL_MIGRATED | NOT_STARTED |
| ESP_SR_VERSION_PINNED | NOT_STARTED |
| AUDIO_SINGLE_OWNER | NOT_STARTED |
| BTN1_REAL_BACKEND_SWITCH | NOT_STARTED |
| API_KEY_LEAK | 0 |
| MAIN_BUILD | PASS |
| UNIT_BUILD | N/A |
| HOST_TESTS | N/A |
| PYTHON_TESTS | N/A |
| CODE_REVIEW_P0 | 0 |
| CODE_REVIEW_P1 | 0 |

## Skills Actually Invoked

| Skill | Impact |
|-------|--------|
| `/embedded-systems` | Validated WAV format, audio quality metrics |
| `/esp32` | Verified build configuration, Flash partition |
| `/debugging-code` | Systematic encoding verification, mojibake detection |
| `/code-review` | Background review (findings from previous session) |

## Files Changed

| File | Change |
|------|--------|
| tools/synthesize_voice_prompts.py | Fixed encoding, added mojibake detection |
| components/voice_agent/assets/mimo_generated/*.wav | Regenerated (9 files) |
| components/voice_agent/assets/voice_assets_manifest.json | Updated with ensure_ascii=True |
| main/app_bootstrap.c | Added play_task_completion_prompt() |

## Next Steps

1. **Immediate**: Wire play_task_completion_prompt() to wash PROGRAM_COMPLETE event
2. **Stage 2**: Create voice_frontend component with unified wake backend interface
3. **Stage 3**: Migrate Edge Impulse model
4. **Stage 4**: Integrate ESP-SR
5. **Stage 5**: Implement BTN1 switching
6. **Stage 6**: Comprehensive testing

## Blockers

None for Stage 1. Stages 2-6 require significant new component development.
