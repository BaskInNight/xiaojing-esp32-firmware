#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * audio_driver.h — I2S audio driver with AMP_SD hardware mute
 * AMP_SD (GPIO47) defaults LOW (muted, matching 10k pull-down).
 * No MiMo, cloud, wake model, or TTS business logic.
 * ================================================================ */

typedef struct audio_ctx audio_handle_t;

typedef struct {
    int sample_rate;      /* Hz, default 16000 */
    int bits_per_sample;  /* 16 or 32, default 16 */
} audio_config_t;

#define AUDIO_DEFAULT_CONFIG() { \
    .sample_rate = 16000, \
    .bits_per_sample = 16, \
}

/* Initialize audio driver. I2S TX+RX channels created but disabled.
 * AMP_SD (GPIO47) set LOW (muted). */
audio_handle_t *audio_driver_init(const audio_config_t *config);
/* Input-only variant for safe FAKE-mode voice demos. It never configures
 * AMP_SD/GPIO47 or a TX channel and therefore does not require output-board
 * identity confirmation. */
audio_handle_t *audio_driver_init_capture_only(
    const audio_config_t *config);

/* Destroy driver. Stops RX/TX, sets AMP_SD LOW. NULL-safe. */
void audio_driver_destroy(audio_handle_t *handle);

/* Start/stop capture independently from playback. Idempotent. */
esp_err_t audio_driver_start_rx(audio_handle_t *handle);
esp_err_t audio_driver_stop_rx(audio_handle_t *handle);

/* Re-synchronize a retained capture channel after another I2S controller
 * temporarily drove the shared BCLK/WS pins for playback. */
esp_err_t audio_driver_resync_rx(audio_handle_t *handle);

/* Start/stop playback independently from capture. Idempotent.
 * AMP_SD is HIGH only while TX is started. */
esp_err_t audio_driver_start_tx(audio_handle_t *handle);
esp_err_t audio_driver_stop_tx(audio_handle_t *handle);

/* Change the clock used by the next RX/TX session. Both directions must be
 * stopped; active DMA is never reconfigured in place. Supported rates are
 * 16 kHz capture and 24 kHz MiMo/local-prompt playback. */
esp_err_t audio_driver_set_sample_rate(audio_handle_t *handle,
                                       int sample_rate);

/* Enable audio output: set AMP_SD HIGH, enable I2S TX. */
esp_err_t audio_driver_enable_output(audio_handle_t *handle);

/* Disable audio output: disable I2S TX, set AMP_SD LOW. */
esp_err_t audio_driver_disable_output(audio_handle_t *handle);

/* Write mono PCM data to I2S TX (blocking with timeout). The driver duplicates
 * every sample to the left and right slots for MAX98357A channel safety. */
esp_err_t audio_driver_write(audio_handle_t *handle,
                             const void *data,
                             size_t size,
                             size_t *bytes_written,
                             uint32_t timeout_ms);

/* Read PCM data from I2S RX (blocking with timeout). */
esp_err_t audio_driver_read(audio_handle_t *handle,
                            void *buf,
                            size_t size,
                            size_t *bytes_read,
                            uint32_t timeout_ms);

/* Check if output is enabled. */
bool audio_driver_is_output_enabled(audio_handle_t *handle);
bool audio_driver_is_input_enabled(audio_handle_t *handle);

#ifdef __cplusplus
}
#endif
