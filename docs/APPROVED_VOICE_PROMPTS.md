# Approved local voice prompts

User selection: boot B (`honey_v4/boot_ready_sunny_b.wav`), wake A1
(`honey_v3/wake_ready_a_sunny.wav`), wash-start A
(`honey_v4/wash_started_reassuring_a.wav`). Thinking now uses the user-approved
`honey_v8/chat_thinking_wait.wav` (稍等，我想一想哦。); remaining six prompts use honey_v1.
The original audition WAVs are preserved. No additional cloud synthesis is needed.

`tools/package_approved_voice_prompts.py` creates `assets/approved_honey`.
Its manifest records the selected sources and SHA-256 hashes.
The replacement thinking prompt is packaged in `assets/approved_honey_r2`,
preserving the earlier asset. Generate it using `--only chat_thinking` and
`--output-dir components/voice_agent/assets/approved_honey_r2`.
These copies use the same Q15 FIR conversion as the existing 24kHz-to-16kHz playback path, saving
Flash without changing speed or pitch. Runtime supports both 16kHz and legacy
24kHz prompts. The cloud TTS model remains mimo-v2.5-tts.

| Event | Prompt | Trigger |
| --- | --- | --- |
| Boot ready | 你好呀，小净准备好啦。 | Successful bootstrap, once |
| Wake | 小净在听。 | Audio controller begins dialog, before recorder starts |
| PTT | 我在，你说。 | Frontend stopped, before capture starts |
| Empty recording | 没听清，再说一次吧。 | Empty timeout or capture rejected as invalid size |
| Thinking | 稍等，我想一想哦。 | Cloud waiting for at least 3 seconds, once per utterance |
| Offline | 网络暂时没连上，请检查无线网络。 | Cloud reports network unavailable |
| Wash start | 开始洗涤啦，交给小净吧。 | Active executor enters RUNNING, once per program/generation |
| Wash complete | 洗好啦，记得取出衣物哦。 | Executor confirms PROGRAM_COMPLETE |
| Fault | 设备出现异常，请停止使用，查看屏幕提示。 | Executor enters FAULT, once per fault episode |
| Conversation failure | 这次没处理好，请再试一次。 | Main model request/parse failure |

Wash notices use a bounded presentation queue and a worker, never playback from
the timer or executor callback. Busy voice sessions defer notices for up to
30 seconds. Stale starts/faults are discarded; presentation failure only logs
and does not change wash state. Local safety gates, homing, posture changes,
and motor timeouts remain in the existing executor and safety services.

Board checks: confirm boot wording and pitch, repeated wake acknowledgements
without self-recording, PTT release, empty input, single thinking cue on a slow
request, offline/error separation, and wash start/complete/fault announcements.
Do not induce a physical hazard to test the fault announcement.
