/*
 * audio_driver.c — I2S audio driver with AMP_SD hardware mute
 *
 * AMP_SD (GPIO47): default LOW (hardware mute, matching 10k pull-down).
 * I2S TX for MAX98357 amp, RX for INMP441 mic.
 * No business logic (MiMo/cloud/wake/TTS) — raw PCM only.
 */

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include "audio_driver.h"
#include "board_config.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "audio";

struct audio_ctx {
    i2s_chan_handle_t tx_handle;
    i2s_chan_handle_t rx_handle;
    bool output_enabled;
    /* TX remains enabled while muted to preserve the shared BCLK/WS clock.
     * Keep channel state separate from amplifier mute state: enabling an
     * already-enabled IDF channel returns ESP_ERR_INVALID_STATE (0x103). */
    bool tx_channel_enabled;
    bool input_enabled;
    int sample_rate;
    int bits_per_sample;
    bool capture_only;
    bool rx_level_logged;
    size_t mic_channel;  /* 0=LEFT, 1=RIGHT, auto-detected */
    uint32_t read_counter; /* Frames read, for periodic re-detection */
};

static i2s_std_gpio_config_t tx_gpio_config(void)
{
    return (i2s_std_gpio_config_t) {
        .mclk = I2S_GPIO_UNUSED,
        .bclk = XIAOJING_GPIO_I2S_BCLK,
        .ws = XIAOJING_GPIO_I2S_WS,
        .dout = XIAOJING_GPIO_AMP_DIN,
        .din = I2S_GPIO_UNUSED,
        .invert_flags = {
            .mclk_inv = false,
            .bclk_inv = false,
            .ws_inv = false,
        },
    };
}

static i2s_std_gpio_config_t duplex_gpio_config(void)
{
    i2s_std_gpio_config_t cfg = tx_gpio_config();
    cfg.din = XIAOJING_GPIO_MIC_SD;
    return cfg;
}

static i2s_std_gpio_config_t rx_gpio_config(void)
{
    i2s_std_gpio_config_t cfg = duplex_gpio_config();
    cfg.dout = I2S_GPIO_UNUSED;
    return cfg;
}

static esp_err_t create_tx_only_channel(audio_handle_t *handle)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(
        I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = 240;
    esp_err_t err = i2s_new_channel(&chan_cfg, &handle->tx_handle, NULL);
    if (err != ESP_OK) return err;

    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(handle->sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            (handle->bits_per_sample == 32) ? I2S_DATA_BIT_WIDTH_32BIT :
                                              I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO),
        .gpio_cfg = tx_gpio_config(),
    };
    err = i2s_channel_init_std_mode(handle->tx_handle, &cfg);
    if (err != ESP_OK) {
        (void)i2s_del_channel(handle->tx_handle);
        handle->tx_handle = NULL;
    }
    return err;
}

static audio_handle_t *audio_driver_init_internal(
    const audio_config_t *config, bool capture_only)
{
    if (!config || config->sample_rate != 16000 ||
        (config->bits_per_sample != 16 &&
         config->bits_per_sample != 32)) return NULL;

    /* GPIO47 (AMP_SD) requires board identity confirmation. Capture-only
     * instances never configure or drive that output. */
    if (!capture_only && !board_config_is_board_identity_confirmed()) {
        ESP_LOGW(TAG, "GPIO47 (AMP_SD) blocked — board identity not confirmed");
        return NULL;
    }

    audio_handle_t *handle = calloc(1, sizeof(audio_handle_t));
    if (!handle) return NULL;

    handle->sample_rate = config->sample_rate;
    handle->bits_per_sample = config->bits_per_sample;
    handle->capture_only = capture_only;
    handle->output_enabled = false;
    handle->input_enabled = false;

    esp_err_t err = ESP_OK;
    if (!capture_only) {
        /* 1. Configure AMP_SD GPIO as output, default LOW (muted) */
        gpio_config_t sd_conf = {
            .pin_bit_mask = (1ULL << XIAOJING_GPIO_AMP_SD),
            /* Keep input enabled so bring-up can verify the actual pad level,
             * not only the output latch, while driving AMP_SD. */
            .mode = GPIO_MODE_INPUT_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&sd_conf);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "AMP_SD GPIO config failed: 0x%x", err);
            free(handle);
            return NULL;
        }
        gpio_set_level(XIAOJING_GPIO_AMP_SD, 0);
    }

    /* 2. Allocate playback only during bootstrap. Microphone capture uses a
     * temporary I2S1 channel created on PTT so its 32-bit slot requirement
     * does not consume BLE/display resources during startup. */
    if (!capture_only) {
        err = create_tx_only_channel(handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S TX channel creation failed: 0x%x", err);
            free(handle);
            return NULL;
        }
    }

    /* Channels remain disabled until explicitly enabled */
    if (capture_only) {
        ESP_LOGI(TAG, "Audio capture initialized (RX only, %dHz)",
                 config->sample_rate);
    } else {
        ESP_LOGI(TAG, "Audio driver initialized (AMP_SD LOW/muted, %dHz)",
                 config->sample_rate);
    }
    return handle;
}

audio_handle_t *audio_driver_init(const audio_config_t *config)
{
    return audio_driver_init_internal(config, false);
}

audio_handle_t *audio_driver_init_capture_only(
    const audio_config_t *config)
{
    return audio_driver_init_internal(config, true);
}

void audio_driver_destroy(audio_handle_t *handle)
{
    if (!handle) return;

    if (!handle->capture_only) gpio_set_level(XIAOJING_GPIO_AMP_SD, 0);

    audio_driver_stop_tx(handle);
    audio_driver_stop_rx(handle);

    if (handle->tx_handle) {
        i2s_del_channel(handle->tx_handle);
    }
    if (handle->rx_handle) {
        i2s_del_channel(handle->rx_handle);
    }
    free(handle);
}

esp_err_t audio_driver_start_rx(audio_handle_t *handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (handle->input_enabled) return ESP_OK;
    if (handle->output_enabled) return ESP_ERR_INVALID_STATE;

    /* Capture-only I2S1 is allocated before BLE/Wi-Fi so its DMA descriptors
     * come from a large contiguous internal block.  Do not delete/recreate it
     * here: after the radio stacks start there may be no block large enough
     * for another DMA allocation.  Disable/enable resets the RX queue while
     * preserving the reserved descriptors and is safe after prompt playback. */
    if (handle->capture_only && handle->rx_handle) {
        esp_err_t err = i2s_channel_enable(handle->rx_handle);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
        handle->input_enabled = true;
        handle->rx_level_logged = false;
        ESP_LOGI(TAG, "Audio input enabled (I2S1 retained DMA)");
        return ESP_OK;
    }
    if (handle->rx_handle) return ESP_ERR_INVALID_STATE;

    /* The microphone and amplifier share BCLK/WS.  ESP-IDF full duplex
     * requires TX and RX to be allocated together on the same controller
     * with identical clock/slot/GPIO settings.  Two independent controllers
     * can both report success while GPIO matrix ownership leaves RX reading
     * all zeroes.  Rebuild the disabled TX-only channel as an on-demand I2S0
     * duplex pair for capture, then restore TX-only in stop_rx(). */
    if (!handle->capture_only && handle->tx_handle) {
        esp_err_t del_err = i2s_del_channel(handle->tx_handle);
        if (del_err != ESP_OK) return del_err;
        handle->tx_handle = NULL;
    }

    i2s_port_t port = handle->capture_only ? I2S_NUM_1 : I2S_NUM_0;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(
        port, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = 120;
    esp_err_t err = i2s_new_channel(
        &chan_cfg,
        handle->capture_only ? NULL : &handle->tx_handle,
        &handle->rx_handle);
    if (err != ESP_OK) {
        return err;
    }

    i2s_std_config_t rx_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(handle->sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = handle->capture_only ? rx_gpio_config()
                                         : duplex_gpio_config(),
    };
    if (!handle->capture_only)
        err = i2s_channel_init_std_mode(handle->tx_handle, &rx_cfg);
    if (err == ESP_OK)
        err = i2s_channel_init_std_mode(handle->rx_handle, &rx_cfg);
    /* In ESP-IDF full-duplex mode the paired TX channel is the clock owner
     * after the second channel is constituted as the duplex slave.  Enabling
     * RX alone leaves reads waiting forever.  This mirrors the IDF duplex
     * tests and the legacy RX|TX driver used by the proven Arduino build. */
    if (err == ESP_OK && !handle->capture_only)
        err = i2s_channel_enable(handle->tx_handle);
    if (err == ESP_OK) err = i2s_channel_enable(handle->rx_handle);
    if (err != ESP_OK) {
        if (!handle->capture_only && handle->tx_handle)
            (void)i2s_channel_disable(handle->tx_handle);
        (void)i2s_del_channel(handle->rx_handle);
        handle->rx_handle = NULL;
        if (handle->tx_handle) {
            (void)i2s_del_channel(handle->tx_handle);
            handle->tx_handle = NULL;
        }
        return err;
    }
    handle->input_enabled = true;
    handle->rx_level_logged = false;

    /* Log I2S runtime configuration */
    ESP_LOGI(TAG, "I2S_CONFIG:");
    ESP_LOGI(TAG, "  hardware_sample_rate=%u Hz", (unsigned)handle->sample_rate);
    ESP_LOGI(TAG, "  slot_mode=STEREO (I2S_SLOT_MODE_STEREO)");
    ESP_LOGI(TAG, "  bits_per_sample=32 (I2S_DATA_BIT_WIDTH_32BIT)");
    ESP_LOGI(TAG, "  bytes_per_frame=%u (32-bit * 2 channels)",
             (unsigned)(4 * 2));
    ESP_LOGI(TAG, "  output_bits_per_sample=%u", (unsigned)handle->bits_per_sample);
    ESP_LOGI(TAG, "  capture_only=%d", handle->capture_only ? 1 : 0);

    ESP_LOGI(TAG, "Audio input enabled (I2S%d 32-bit stereo, SD=GPIO%d)",
             (int)port, XIAOJING_GPIO_MIC_SD);
    return ESP_OK;
}

esp_err_t audio_driver_stop_rx(audio_handle_t *handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (!handle->input_enabled && !handle->rx_handle) return ESP_OK;
    esp_err_t err = ESP_OK;
    if (handle->input_enabled && handle->rx_handle)
        err = i2s_channel_disable(handle->rx_handle);
    if (err != ESP_OK) return err;
    handle->input_enabled = false;
    if (handle->capture_only) {
        ESP_LOGI(TAG, "Audio input disabled (I2S1 retained)");
        return ESP_OK;
    }
    if (!handle->capture_only && handle->tx_handle) {
        err = i2s_channel_disable(handle->tx_handle);
        if (err != ESP_OK) return err;
        handle->tx_channel_enabled = false;
    }
    if (handle->rx_handle) {
        err = i2s_del_channel(handle->rx_handle);
        if (err != ESP_OK) return err;
        handle->rx_handle = NULL;
    }
    if (!handle->capture_only && handle->tx_handle) {
        err = i2s_del_channel(handle->tx_handle);
        if (err != ESP_OK) return err;
        handle->tx_handle = NULL;
    }
    ESP_LOGI(TAG, "Audio input disabled");
    return ESP_OK;
}

esp_err_t audio_driver_resync_rx(audio_handle_t *handle)
{
    if (!handle || !handle->capture_only || !handle->rx_handle ||
        !handle->input_enabled)
        return ESP_ERR_INVALID_STATE;

    /* Keep the existing I2S1 DMA allocation; only reset its clock/slot/GPIO
     * configuration after I2S0 playback used the shared BCLK/WS pins. */
    esp_err_t err = i2s_channel_disable(handle->rx_handle);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    handle->input_enabled = false;

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(
        handle->sample_rate);
    i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
    err = i2s_channel_reconfig_std_clock(handle->rx_handle, &clk_cfg);
    if (err == ESP_OK)
        err = i2s_channel_reconfig_std_slot(handle->rx_handle, &slot_cfg);
    if (err == ESP_OK) {
        i2s_std_gpio_config_t gpio_cfg = rx_gpio_config();
        err = i2s_channel_reconfig_std_gpio(handle->rx_handle, &gpio_cfg);
    }
    if (err == ESP_OK) err = i2s_channel_enable(handle->rx_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S RX resync failed: 0x%x", (unsigned)err);
        return err;
    }
    handle->input_enabled = true;
    handle->rx_level_logged = false;
    ESP_LOGI(TAG, "I2S RX resynchronized (I2S1 retained DMA)");
    return ESP_OK;
}

esp_err_t audio_driver_start_tx(audio_handle_t *handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (handle->capture_only)
        return ESP_ERR_NOT_SUPPORTED;
    if (handle->output_enabled) return ESP_OK;  /* Idempotent */

    /* Capture deliberately leaves TX/DMA unallocated so TLS can use scarce
     * internal RAM. Recreate playback lazily only when audio is requested. */
    if (!handle->tx_handle) {
        esp_err_t create_err = create_tx_only_channel(handle);
        if (create_err != ESP_OK) return create_err;
    }

    /* MAX98357A startup sequencing requires SD_MODE to leave shutdown
     * before BCLK/LRCLK start.  Starting I2S first can leave the amplifier
     * output stage producing common-mode switching without differential
     * audio on some devices. */
    gpio_set_level(XIAOJING_GPIO_AMP_SD, 1);
    esp_rom_delay_us(100U); /* datasheet minimum is 10 us */

    esp_err_t err = ESP_OK;
    if (!handle->tx_channel_enabled) {
        /* I2S1 capture temporarily owns the shared BCLK/WS GPIO matrix.
         * Restore I2S0 TX routing only when enabling the channel. */
        i2s_std_gpio_config_t gpio_cfg = tx_gpio_config();
        err = i2s_channel_reconfig_std_gpio(handle->tx_handle, &gpio_cfg);
        if (err == ESP_OK) err = i2s_channel_enable(handle->tx_handle);
        if (err != ESP_OK) {
            gpio_set_level(XIAOJING_GPIO_AMP_SD, 0);
            return err;
        }
        handle->tx_channel_enabled = true;
    }

    /* Allow clocks and the Class-D output stage to settle before PCM. */
    esp_rom_delay_us(10000U);
    handle->output_enabled = true;

    ESP_LOGI(TAG, "Audio output enabled (AMP_SD GPIO%d readback=%d, "
                  "BCLK=%d WS=%d DIN=%d)",
             XIAOJING_GPIO_AMP_SD, gpio_get_level(XIAOJING_GPIO_AMP_SD),
             XIAOJING_GPIO_I2S_BCLK, XIAOJING_GPIO_I2S_WS,
             XIAOJING_GPIO_AMP_DIN);
    return ESP_OK;
}

esp_err_t audio_driver_set_sample_rate(audio_handle_t *handle,
                                       int sample_rate)
{
    if (!handle || (sample_rate != 16000 && sample_rate != 24000))
        return ESP_ERR_INVALID_ARG;
    /* TX and RX share WS (GPIO15) and BCLK (GPIO16) on the same I2S port.
     * Reconfiguring the TX clock while RX is active corrupts the microphone
     * signal (saturation to 2^30).  Keep TX at 16 kHz permanently — the
     * prompt WAV plays slightly slower but remains intelligible, and the
     * microphone stays functional through all backend switches. */
    if (sample_rate != 16000) {
        ESP_LOGD(TAG, "Sample rate change to %d Hz suppressed (shared I2S bus)",
                 sample_rate);
        return ESP_OK;
    }
    return ESP_OK;
}

esp_err_t audio_driver_stop_tx(audio_handle_t *handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (!handle->output_enabled) return ESP_OK;  /* Idempotent */

    /* Mute amplifier. TX channel stays alive to keep WS/BCLK clock running
     * for RX (they share GPIO15/16 on the same I2S port). */
    gpio_set_level(XIAOJING_GPIO_AMP_SD, 0);
    handle->output_enabled = false;

    ESP_LOGI(TAG, "Audio output disabled (AMP_SD LOW, TX kept alive)");
    return ESP_OK;
}

esp_err_t audio_driver_enable_output(audio_handle_t *handle)
{
    return audio_driver_start_tx(handle);
}

esp_err_t audio_driver_disable_output(audio_handle_t *handle)
{
    return audio_driver_stop_tx(handle);
}

esp_err_t audio_driver_write(audio_handle_t *handle,
                             const void *data,
                             size_t size,
                             size_t *bytes_written,
                             uint32_t timeout_ms)
{
    if (!handle || !data) return ESP_ERR_INVALID_ARG;
    if (!handle->output_enabled) return ESP_ERR_INVALID_STATE;

    const size_t sample_bytes = (size_t)handle->bits_per_sample / 8U;
    if (sample_bytes == 0U || (size % sample_bytes) != 0U)
        return ESP_ERR_INVALID_SIZE;

    /* The public playback contract is mono PCM. MAX98357A SD_MODE is driven
     * HIGH by GPIO47 and therefore listens to the right I2S slot. Emit a real
     * stereo frame with the same sample in both slots so channel strapping can
     * never turn a valid stream into silence. */
    const uint8_t *src = (const uint8_t *)data;
    uint8_t stereo[640];
    size_t input_done = 0U;
    while (input_done < size) {
        size_t frames = (size - input_done) / sample_bytes;
        const size_t max_frames = sizeof(stereo) / (2U * sample_bytes);
        if (frames > max_frames) frames = max_frames;
        for (size_t i = 0; i < frames; i++) {
            const uint8_t *sample = src + input_done + i * sample_bytes;
            memcpy(stereo + (2U * i) * sample_bytes, sample, sample_bytes);
            memcpy(stereo + (2U * i + 1U) * sample_bytes, sample, sample_bytes);
        }

        const size_t output_size = frames * 2U * sample_bytes;
        size_t output_written = 0U;
        esp_err_t err = i2s_channel_write(handle->tx_handle, stereo,
                                          output_size, &output_written,
                                          timeout_ms);
        if (err != ESP_OK || output_written != output_size) {
            if (bytes_written) *bytes_written = input_done;
            memset(stereo, 0, sizeof(stereo));
            return err != ESP_OK ? err : ESP_ERR_TIMEOUT;
        }
        input_done += frames * sample_bytes;
    }
    memset(stereo, 0, sizeof(stereo));
    if (bytes_written) *bytes_written = input_done;
    return ESP_OK;
}

esp_err_t audio_driver_read(audio_handle_t *handle,
                            void *buf,
                            size_t size,
                            size_t *bytes_read,
                            uint32_t timeout_ms)
{
    if (!handle || !buf) return ESP_ERR_INVALID_ARG;
    if (!handle->input_enabled) return ESP_ERR_INVALID_STATE;
    const size_t sample_bytes = (size_t)handle->bits_per_sample / 8U;
    if (sample_bytes == 0U || (size % sample_bytes) != 0U)
        return ESP_ERR_INVALID_SIZE;

    uint8_t *dst = (uint8_t *)buf;
    size_t produced = 0U;
    int32_t raw[256];
    while (produced < size) {
        size_t frames = (size - produced) / sample_bytes;
        if (frames > 128U) frames = 128U;
        size_t got = 0U;
        esp_err_t err = i2s_channel_read(
            handle->rx_handle, raw,
            frames * 2U * sizeof(int32_t), &got, timeout_ms);
        if (err != ESP_OK) {
            if (bytes_read) *bytes_read = produced;
            memset(raw, 0, sizeof(raw));
            return err;
        }
        size_t got_frames = got / (2U * sizeof(int32_t));
        if (got_frames == 0U) break;

        uint64_t left_energy = 0U, right_energy = 0U;
        uint32_t left_peak = 0U, right_peak = 0U;
        for (size_t i = 0; i < got_frames; i++) {
            int64_t left = raw[2U * i];
            int64_t right = raw[2U * i + 1U];
            uint32_t left_abs = (uint32_t)(left < 0 ? -left : left);
            uint32_t right_abs = (uint32_t)(right < 0 ? -right : right);
            left_energy += left_abs;
            right_energy += right_abs;
            if (left_abs > left_peak) left_peak = left_abs;
            if (right_abs > right_peak) right_peak = right_abs;
        }
        handle->read_counter++;
        if (!handle->rx_level_logged || (handle->read_counter % 200 == 0)) {
            unsigned sd_high = 0U, bclk_high = 0U, ws_high = 0U;
            for (unsigned probe = 0U; probe < 8192U; probe++) {
                sd_high += gpio_get_level(XIAOJING_GPIO_MIC_SD) ? 1U : 0U;
                bclk_high += gpio_get_level(XIAOJING_GPIO_I2S_BCLK) ? 1U : 0U;
                ws_high += gpio_get_level(XIAOJING_GPIO_I2S_WS) ? 1U : 0U;
            }

            /* Calculate RMS for both channels */
            double left_rms = 0.0, right_rms = 0.0;
            if (got_frames > 0) {
                double left_sum_sq = 0.0, right_sum_sq = 0.0;
                for (size_t i = 0; i < got_frames; i++) {
                    double left = (double)raw[2U * i];
                    double right = (double)raw[2U * i + 1U];
                    left_sum_sq += left * left;
                    right_sum_sq += right * right;
                }
                left_rms = sqrt(left_sum_sq / got_frames);
                right_rms = sqrt(right_sum_sq / got_frames);
            }

            ESP_LOGI(TAG, "I2S raw level: frames=%u Lpeak=%lu Rpeak=%lu "
                     "Lrms=%.0f Rrms=%.0f GPIO highs SD=%u BCLK=%u WS=%u",
                     (unsigned)got_frames,
                     (unsigned long)left_peak, (unsigned long)right_peak,
                     left_rms, right_rms,
                     sd_high, bclk_high, ws_high);

            /* Determine which channel has the microphone (higher RMS)
             * and log it for hardware verification */
            if (right_rms > left_rms * 10.0) {
                ESP_LOGI(TAG, "I2S: Microphone detected on RIGHT channel (Rrms/Lrms=%.1f)",
                         right_rms / (left_rms > 0 ? left_rms : 1.0));
                handle->mic_channel = 1U;
            } else if (left_rms > right_rms * 10.0) {
                ESP_LOGI(TAG, "I2S: Microphone detected on LEFT channel (Lrms/Rrms=%.1f)",
                         left_rms / (right_rms > 0 ? right_rms : 1.0));
                handle->mic_channel = 0U;
            } else {
                ESP_LOGW(TAG, "I2S: Cannot determine microphone channel (Lrms=%.0f Rrms=%.0f)",
                         left_rms, right_rms);
            }

            handle->rx_level_logged = true;
        }
        /* Channel selection: use RIGHT channel (hardware verified).
         * Boot diagnostics show Rpeak>>Lpeak and Rrms>>Lrms on this board,
         * indicating the INMP441 data arrives in the RIGHT I2S slot. */
        const size_t channel = handle->mic_channel;
        (void)left_energy;  /* Suppress unused warning */
        (void)right_energy;
        for (size_t i = 0; i < got_frames; i++) {
            int32_t sample = raw[2U * i + channel];
            if (sample_bytes == sizeof(int16_t)) {
                /* NEUTRAL PCM: raw32 >> 14 → int16 saturated
                 * No Edge-specific gain applied here.
                 * Edge wake path applies its own 8x gain internally.
                 * Dialog/PTT/ASR paths use this 1x PCM directly. */
                int32_t scaled = sample >> 14;
                if (scaled > INT16_MAX) scaled = INT16_MAX;
                if (scaled < INT16_MIN) scaled = INT16_MIN;
                int16_t out_sample = (int16_t)scaled;
                memcpy(dst + produced, &out_sample, sizeof(out_sample));
            } else {
                memcpy(dst + produced, &sample, sizeof(sample));
            }
            produced += sample_bytes;
        }
        if (got_frames < frames) break;
    }
    memset(raw, 0, sizeof(raw));
    if (bytes_read) *bytes_read = produced;
    return produced > 0U ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool audio_driver_is_output_enabled(audio_handle_t *handle)
{
    return handle ? handle->output_enabled : false;
}

bool audio_driver_is_input_enabled(audio_handle_t *handle)
{
    return handle ? handle->input_enabled : false;
}
