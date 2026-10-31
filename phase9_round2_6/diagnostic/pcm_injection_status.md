# Stage 5: Local PCM Injection Testing

## Status: PARTIAL - NEEDS_USER_LIVE_TEST

### Available Test Audio
- **esp32_raw_audio.wav**: 16kHz, 16-bit, mono, 2.95 seconds
  - Location: `phase9_round2_6/replay/esp32_raw_audio.wav`
  - Unknown content (may or may not contain "小净小净")

### Missing Test Audio
- No verified "小净小净" positive samples found in old project
- No Edge Impulse training/test data exported

### PCM Injection Mechanism
Due to time constraints and lack of verified positive samples, PCM injection testing is deferred.

**Required for TRUE_REPLAY testing:**
1. Export positive "小净小净" samples from Edge Impulse studio
2. Record user saying "小净小净" in test environment
3. Create TEST_ONLY injection point in voice_wake_edge

### Negative Testing (Can be done without user)
- Silence: 3 seconds of zeros → should NOT trigger wake
- White noise: Random samples → should NOT trigger wake
- Similar words: Other speech → should NOT trigger wake

### Conclusion
**TRUE_LIVE_WAKE_REQUIRES_USER=YES**
Must wait for user to provide or speak "小净小净" samples.
