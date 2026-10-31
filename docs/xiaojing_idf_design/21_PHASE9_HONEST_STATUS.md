# Phase 9 Combined: Honest Status Report

## Actual Status (Not Inflated)

| Stage | Status | Evidence |
|-------|--------|----------|
| Stage 1 | DONE | ASR 9/9 verified, task_done wired via queue+worker |
| Stage 2 | WIRED | voice_frontend in app_bootstrap with real callbacks |
| Stage 3 | SCAFFOLD | Wrapper exists, NO real Edge Impulse model linked |
| Stage 4 | SCAFFOLD | Wrapper exists, NO esp-sr dependency or AFE |
| Stage 5 | NOT_STARTED | BTN1 switching not implemented |
| Stage 6 | NOT_STARTED | No tests written |

## Stage 1: DONE ✅

### task_done Design Fix
- **Removed**: Timer callback that called blocking `real_hal_audio_play_pcm16()`
- **Added**: Dedicated `task_complete_worker` task with bounded queue
- **Flow**: `poll_wash_completion()` (timer context) → queue → worker task → PCM playback
- **Exactly-once**: Keyed on `(program_id, generation)`
- **Queue full**: Event dropped with warning (wash executor cannot block)
- **Playback failure**: Logged, does not affect wash COMPLETE terminal
- **Shutdown**: Worker stops before HAL destroyed

### ASR Evidence
- Machine-readable JSON: `components/voice_agent/assets/asr_evidence.json`
- 9/9 prompts verified with required keywords
- `backend_xiaojing`: required=[切换,唤醒,模式], alternatives=[小净,小静]

## Stage 2: WIRED ✅

### voice_frontend Integration
- `voice_frontend_init()` called in app_bootstrap
- `voice_wake_edge_get_ops()` and `voice_wake_esp_sr_get_ops()` registered
- `bootstrap_select_backend` → `voice_frontend_switch_backend()`
- `bootstrap_start_frontend` → `voice_frontend_start_backend()`
- `bootstrap_stop_frontend` → `voice_frontend_stop()`
- Detection callback → `voice_service_handle_event(VOICE_EVENT_WAKE)`
- Frontend stopped before voice_service_stop in rollback

### What's Missing
- BTN2 PTT does NOT yet call `voice_frontend_stop()` before capture
- TTS does NOT yet call `voice_frontend_disable_wake()`
- No test proving two backends cannot simultaneously own I2S

## Stage 3: SCAFFOLD ❌

### What Exists
- `voice_wake_edge.c` with backend interface
- 16000→16062 Hz resampler (linear interpolation)
- Placeholder `edge_feed()` with comment "placeholder for model"

### What's Missing
- NO Edge Impulse SDK copied from old project
- NO `run_classifier()` call
- NO real model binary linked
- NO `xiao_jing` label detection
- Model NOT in ELF/map

### Blocker
- Edge Impulse SDK is ~21MB with licensing restrictions
- Cannot copy without verifying license compliance
- Model binary needs to be compiled into firmware

## Stage 4: SCAFFOLD ❌

### What Exists
- `voice_wake_esp_sr.c` with backend interface
- Placeholder init/start/stop/feed

### What's Missing
- NO `esp-sr` component added via Component Registry
- NO AFE configuration
- NO WakeNet9 model
- NO real feed/fetch
- Prints "placeholder" in init

### Blocker
- Need to run `idf.py add-dependency "espressif/esp-sr"`
- Need to verify ESP-IDF 5.5.4 compatibility
- Need model partition configuration

## Stage 5: NOT_STARTED ❌

- BTN1 long-press handler not implemented
- NVS persistence not implemented
- Rollback on failure not implemented

## Stage 6: NOT_STARTED ❌

- No host tests
- No unit tests
- No board verification

## Build Evidence

```
xiaojing_idf.bin: 3.7MB (8% partition free)
Symbols in map: voice_frontend, voice_wake_edge, voice_wake_esp_sr, 
                task_complete_worker, poll_wash_completion (272 matches)
```

## SKILLS_ACTUALLY_INVOKED

| Skill | Real Impact |
|-------|-------------|
| `/embedded-systems` | Validated resampler math, queue-based IPC design |
| `/esp32` | Identified CMake dependency issues, fixed enum conflicts |
| `/debugging-code` | Traced task_done UAF risk, identified Timer callback blocking |
| `/code-review` | Caught placeholder status inflation |

## Honest Assessment

**What's real**: Stage 1 (prompts + ASR), Stage 2 (frontend wired to bootstrap)
**What's scaffold**: Stage 3 (Edge Impulse), Stage 4 (ESP-SR)
**What's missing**: Stage 5 (BTN1), Stage 6 (tests)

The voice_frontend component provides a real abstraction, but the backends are empty shells. The integration into app_bootstrap is real but incomplete (PTT/TTS interaction not wired).

## Next Steps (Requires Actual Work)

1. **Stage 3**: Copy Edge Impulse SDK from old project, link real model
2. **Stage 4**: Add esp-sr component, configure AFE and WakeNet9
3. **Stage 5**: Implement BTN1 switching with NVS persistence
4. **Stage 6**: Write and run tests
5. Wire PTT to stop frontend before capture
6. Wire TTS to disable wake during playback
