#pragma once

#include "xiaojing_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * real_hal.h — Real hardware HAL assembly
 * Creates xiaojing_hal_t from actual ESP-IDF drivers.
 * Lifecycle: init I2C → MCP → peripherals → assemble function table.
 * On failure, all already-initialized resources are torn down.
 * ================================================================ */

typedef struct real_hal_ctx real_hal_ctx_t;

/* Create real HAL. Returns NULL on any failure. */
real_hal_ctx_t *real_hal_create(void);

/* Get function table pointer (NULL if ctx is NULL). */
const xiaojing_hal_t *real_hal_get_interface(real_hal_ctx_t *ctx);

/* True only after PAJ7620 ID verification and gesture-mode setup. */
bool real_hal_gesture_available(real_hal_ctx_t *ctx);

/* Initialize the optional PAJ7620 after radio/display memory reservation. */
esp_err_t real_hal_start_gesture(real_hal_ctx_t *ctx);

/* Initialize SHT30 after radio/display memory reservation. */
esp_err_t real_hal_start_sht(real_hal_ctx_t *ctx);

/* Update IBT-2 direction calibration at runtime. */
esp_err_t real_hal_set_ibt2_calibration(real_hal_ctx_t *ctx,
                                        bool calibrated, bool rpwm_is_cw);

/* Bounded local diagnostic/prompt tone.  This is intentionally not a TTS
 * implementation; it verifies the MAX98357A/I2S/AMP_SD path and provides
 * audible interaction feedback during Phase 9 hardware bring-up. */
esp_err_t real_hal_audio_play_tone(real_hal_ctx_t *ctx,
                                   uint16_t frequency_hz,
                                   uint16_t duration_ms,
                                   int16_t amplitude);

/* Play validated signed mono PCM16. The rate is intentionally bounded to the
 * two formats used by the product (16 kHz legacy and 24 kHz MiMo/local WAV). */
esp_err_t real_hal_audio_play_pcm16(real_hal_ctx_t *ctx,
                                    const int16_t *pcm,
                                    size_t pcm_bytes,
                                    uint32_t sample_rate,
                                    uint32_t timeout_ms);

/* Optional TTS is preemptible by PTT. Prepare once before synthesis, then a
 * button task may cancel while HTTPS or PCM playback is in progress. */
esp_err_t real_hal_audio_prepare_playback(real_hal_ctx_t *ctx);
esp_err_t real_hal_audio_cancel_playback(real_hal_ctx_t *ctx);

/* Destroy: emergency stop → deinit all drivers → free. NULL-safe. */
void real_hal_destroy(real_hal_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
