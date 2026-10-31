# MiMo v2.6 response compatibility

The AUTO_ROUTE request uses `mimo-v2.6-flash` and forces the
`generate_wash_pipeline` function. `speech_reply` is the single presentation
field; legacy `reply_text` remains accepted by the local route parser.
Chat/clarification/rejection requests use an empty pipeline and `tool=none`.
TTS remains `mimo-v2.5-tts`.

`voice_cloud.c` uses ESP-IDF's `json` (cJSON) component to select
`choices[0].message.tool_calls[0].function.arguments`. It checks the function
name before applying the existing strict local route parser. If the route
is inconsistent, only its `speech_reply` may become a chat reply, with no
executable tool. If no supported tool call is present, `message.content` may
be plain text, an object, or a single-element object array (including these
structures encoded as a string). Structured content must contain
`speech_reply`; raw JSON is never used as speech.

Pipeline data is a proposal, not a hardware execution interface. Executable
legacy route fields still go through the local authorization, planner and
executor. A speech-only fallback does not execute a proposed pipeline.

Diagnostic logs contain only allowlisted finish reasons, tool-call counts,
parse result numbers and reply byte counts. They do not log response bodies,
transcripts, credentials or spoken text.

Content fallback preserves the structured `transcript` field when supplied.
If the provider omits a transcript, the field stays empty; it is never replaced
by a fabricated utterance. The request explicitly asks for the spoken words
both in tool arguments and in a structured content fallback.

TTS keeps the v2.5 model. TLS RX fragments are configured as 16 KiB through
menuconfig (external PSRAM allocation), and the base64 audio response has a
bounded 2 MiB PSRAM buffer. Audio data is selected via cJSON, independent of
JSON whitespace. Speech failures retain the committed text reply.

The shared I2S hardware clock is fixed at 16 kHz. `real_hal_audio_play_pcm16`
resamples 24 kHz source WAVs using the audio driver's 24-tap polyphase FIR
before writing to I2S. Absolute output indices preserve phase across DMA
chunks, keeping the source duration and pitch. The driver now rejects an
unsupported clock request instead of silently claiming success. TTS style
requests normal-rate, bright, soft adult feminine speech with a sweet tone;
the provider voice remains `mimo_default`.

BLE TX retains its fragment offset during queue backpressure and paces
notifications. A partly sent JSON frame must finish before another frame is
sent. The miniapp waits for both the get_status ACK and a complete fresh status
notification before displaying the motor bench state; status loss is shown as
waiting for synchronization rather than as a safety-check result.

Regression fixtures in `host_tests/test_main.c` cover long transcript fields,
plain/object/array content, JSON-encoded content and inconsistent tool routes.
Host builds require the same cJSON source as ESP-IDF: set `IDF_PATH`, or pass
`-DCJSON_INCLUDE_DIR=<ESP-IDF>/components/json/cJSON` to CMake.
