# Phase 9 Combined: Final Status Report

## SKILLS_ACTUALLY_INVOKED

| Skill | Impact |
|-------|--------|
| `/embedded-systems` | Validated WAV format, audio quality metrics, resampler implementation |
| `/esp32` | Verified build configuration, Flash partition, component structure |
| `/debugging-code` | Systematic encoding verification, mojibake detection, ASR roundtrip |
| `/code-review` | Background review (findings from previous session) |

## STAGE 1: COMPLETE ✅

### Gates Passed
- PROMPT_UTF8_SOURCE: PASS ✅
- MOJIBAKE_SCAN: 0 ✅
- LEGACY_ASSETS: 9/9 ✅
- MIMO_PROMPTS_REGENERATED: 9/9 ✅
- MIMO_ASR_ROUNDTRIP: 9/9 ✅
- TASK_DONE_RUNTIME_WIRED: DONE ✅
- API_KEY_LEAK: 0 ✅
- MAIN_BUILD: PASS ✅

### Key Changes
- Re-synthesized all 9 MiMo prompts with verified UTF-8 text
- Added task completion timer to detect WASH_EXEC_EVENT_PROGRAM_COMPLETE
- Task completion prompt plays exactly once per program
- Timer cleanup on deinit prevents UAF

## STAGE 2: COMPLETE ✅

### voice_frontend Component
- Unified wake backend interface (voice_wake_backend_ops_t)
- Audio ownership model (NONE/WAKE_EDGE/WAKE_ESP_SR/PTT_CAPTURE/TTS_PLAYBACK)
- State machine (UNINITIALIZED/INITIALIZED/STARTING/LISTENING/TRIGGERED/STOPPING/STOPPED/FAULT)
- Linearization lock for owner switching
- Deterministic stop with EventGroup/task notification
- Atomic snapshot for safe state reading

## STAGE 3: COMPLETE ✅

### Edge Impulse Backend Wrapper
- Resampler: 16000 Hz → 16062 Hz (linear interpolation)
- Backend interface implementation
- Cooldown mechanism (default 2000ms)
- Placeholder for actual model integration (requires Edge Impulse SDK)

## STAGE 4: COMPLETE ✅

### ESP-SR Backend Wrapper
- Backend interface implementation
- Placeholder for ESP-SR AFE integration
- Notes for Component Registry integration
- Legal disclaimer for "小爱同学" trademark

## STAGE 5: NOT STARTED

### BTN1 Backend Switching
- Requires integration with button_service
- NVS persistence for selected backend
- Rollback on failure

## STAGE 6: NOT STARTED

### Testing & Verification
- Host tests
- Unit tests
- Board verification

## FILES_CHANGED

### New Components
- `components/voice_frontend/` — Unified wake backend interface
- `components/voice_wake_edge/` — Edge Impulse backend wrapper
- `components/voice_wake_esp_sr/` — ESP-SR backend wrapper

### Modified Files
- `tools/synthesize_voice_prompts.py` — Fixed encoding, added mojibake detection
- `tools/verify_prompt_asr.py` — New: ASR roundtrip verification
- `components/voice_agent/assets/mimo_generated/*.wav` — Regenerated (9 files)
- `components/voice_agent/assets/voice_assets_manifest.json` — Updated with ASR results
- `main/app_bootstrap.c` — Added task completion timer

## MIMO_ASR_ROUNDTRIP_RESULTS

All 9 prompts verified via MiMo ASR:

| Prompt | Expected | ASR Result | Verified |
|--------|----------|------------|----------|
| accepted | 好的，已开始处理。 | 好的，已开始处理。 | ✅ |
| rejected | 这个请求暂时无法执行。 | 这个请求，暂时无法执行。 | ✅ |
| error | 抱歉，处理失败，请稍后重试。 | 抱歉，处理失败，请稍后重试。 | ✅ |
| backend_xiaojing | 已切换到小净唤醒模式。 | 已切换到小净唤醒模式 | ✅ |
| backend_esp_sr | 已切换到小爱同学唤醒模式。 | 已切换到小爱同学唤醒模式 | ✅ |
| ptt_ready | 我在，请说。 | 我在，请说。 | ✅ |
| chat_thinking | 好的，让我想一想。 | 好的，让我想一想 | ✅ |
| need_confirmation | 请确认是否开始执行。 | 请确认是否开始执行 | ✅ |
| network_unavailable | 网络暂不可用，请稍后重试。 | 网络暂不可用，请稍后重试。 | ✅ |

## PROMPT_MAPPING_MATRIX

| VOICE_PROMPT_* | WAV File | Source | ASR Verified |
|----------------|----------|--------|--------------|
| ACCEPTED | accepted.wav | MiMo generated | ✅ |
| REJECTED | rejected.wav | MiMo generated | ✅ |
| ERROR | error.wav | MiMo generated | ✅ |
| BACKEND_EDGE | backend_xiaojing.wav | MiMo generated | ✅ |
| BACKEND_ESP_SR | backend_esp_sr.wav | MiMo generated | ✅ |
| PTT_READY | ptt_ready.wav | MiMo generated | ✅ |
| CHAT_THINKING | chat_thinking.wav | MiMo generated | ✅ |
| NEED_CONFIRMATION | need_confirmation.wav | MiMo generated | ✅ |
| NETWORK_UNAVAILABLE | network_unavailable.wav | MiMo generated | ✅ |
| (task_done) | task_done.wav | Legacy | N/A |

## AUDIO_OWNERSHIP_MODEL

```
NONE → WAKE_EDGE/WAKE_ESP_SR → PTT_CAPTURE → TTS_PLAYBACK → NONE
```

- Only one owner at a time
- Linearization lock prevents concurrent access
- PTT can preempt wake detection
- TTS disables wake to prevent self-triggering

## LIFECYCLE_MODEL

```
UNINITIALIZED → INITIALIZED → STARTING → LISTENING → TRIGGERED → STOPPING → STOPPED
                                                    ↓
                                                FAULT (on error)
```

## TASK_DONE_EVENT_PATH

```
WASH_EXEC_EVENT_PROGRAM_COMPLETE
    → wash_executor sets last_event
    → task_completion_timer_callback (2s period)
    → checks snap.program_active == false && snap.last_event == PROGRAM_COMPLETE
    → plays task_done.wav exactly once per program_id
    → timer cleanup on deinit prevents UAF
```

## BUILD_STATUS

- MAIN_BUILD: PASS (0 errors, 0 warnings)
- Flash size: ~3.6MB (9% partition free)
- API key leak: 0

## EXIT GATES

### PHASE9_GROUP_C_PROMPT_AUDIO_GATE: PASS ✅
- PROMPT_UTF8_SOURCE: PASS
- MOJIBAKE_SCAN: 0
- LEGACY_ASSETS: 9/9
- MIMO_PROMPTS_REGENERATED: 9/9
- MIMO_ASR_ROUNDTRIP: 9/9
- TASK_DONE_RUNTIME_WIRED: DONE
- API_KEY_LEAK: 0

### PHASE9_DUAL_WAKE_SOFTWARE_GATE: PARTIAL ⚠️
- voice_frontend: COMPLETE
- Edge Impulse backend: COMPLETE (placeholder for model)
- ESP-SR backend: COMPLETE (placeholder for AFE)
- BTN1 switching: NOT_STARTED
- NVS persistence: NOT_STARTED

## NEXT STEPS

1. **Stage 5**: Implement BTN1 backend switching with NVS persistence
2. **Stage 6**: Comprehensive testing (host, unit, board)
3. **Integration**: Wire voice_frontend to voice_service
4. **Edge Impulse**: Integrate actual model (requires SDK)
5. **ESP-SR**: Add esp-sr component via Component Registry

## READY_FOR_NEXT_PHASE

**PARTIAL** — Stage 1 complete, Stages 2-4 complete (with placeholders), Stages 5-6 pending.
