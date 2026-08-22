#include "voice_capture_idf.h"
#include "voice_vad.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define WAV_HEADER_BYTES 44U
#define CAPTURE_DONE_BIT BIT0
#define CAPTURE_JOIN_TIMEOUT_MS 2500U
#define CAPTURE_READ_TIMEOUT_MS 100U
#define CAPTURE_TASK_STACK_BYTES 4096U

static const char *TAG = "voice_capture";

typedef struct {
    const xiaojing_hal_t *hal;
    uint8_t *wav;
    size_t capacity;
    _Atomic size_t pcm_bytes;
    _Atomic bool stop_requested;
    TaskHandle_t task;
    bool initialized;
    bool recording;
    bool leased;
    uint32_t token;
    voice_capture_idf_snapshot_t snapshot;
} capture_runtime_t;

static capture_runtime_t s_rt;
static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
static StaticEventGroup_t s_events_storage;
static EventGroupHandle_t s_events;

static bool take_lock(void)
{
    return s_lock &&
           xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE;
}

static void put_le16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void write_wav_header(uint8_t *wav, size_t pcm_bytes)
{
    memcpy(wav, "RIFF", 4);
    put_le32(wav + 4, (uint32_t)(36U + pcm_bytes));
    memcpy(wav + 8, "WAVEfmt ", 8);
    put_le32(wav + 16, 16);
    put_le16(wav + 20, 1);
    put_le16(wav + 22, 1);
    put_le32(wav + 24, 16000);
    put_le32(wav + 28, 32000);
    put_le16(wav + 32, 2);
    put_le16(wav + 34, 16);
    memcpy(wav + 36, "data", 4);
    put_le32(wav + 40, (uint32_t)pcm_bytes);
}

static void capture_task(void *arg)
{
    (void)arg;
    uint32_t timeout_count = 0U;
    for (;;) {
        if (atomic_load_explicit(
                &s_rt.stop_requested, memory_order_acquire))
            break;
        size_t used = atomic_load_explicit(
            &s_rt.pcm_bytes, memory_order_acquire);
        if (used >= s_rt.capacity - WAV_HEADER_BYTES) break;
        size_t remaining = s_rt.capacity - WAV_HEADER_BYTES - used;
        size_t chunk = remaining > 1024U ? 1024U : remaining;
        size_t got = 0;
        esp_err_t err = s_rt.hal->audio_read_pcm(
            s_rt.wav + WAV_HEADER_BYTES + used, chunk, &got,
            CAPTURE_READ_TIMEOUT_MS);
        if (err == ESP_ERR_TIMEOUT) {
            timeout_count++;
            if (timeout_count == 1U || (timeout_count % 20U) == 0U) {
                ESP_LOGW(TAG, "I2S capture timeout count=%u pcm=%u",
                         (unsigned)timeout_count, (unsigned)used);
            }
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S capture read failed: %s (0x%x), pcm=%u",
                     esp_err_to_name(err), (unsigned)err, (unsigned)used);
            break;
        }
        timeout_count = 0U;
        if (got > chunk) got = chunk;
        atomic_store_explicit(
            &s_rt.pcm_bytes, used + got, memory_order_release);
    }
    (void)s_rt.hal->audio_stop_capture();
    xEventGroupSetBits(s_events, CAPTURE_DONE_BIT);
    vTaskDeleteWithCaps(NULL);
}

static esp_err_t join_capture(void)
{
    if (!s_rt.task) {
        s_rt.recording = false;
        return ESP_OK;
    }
    atomic_store_explicit(
        &s_rt.stop_requested, true, memory_order_release);
    EventBits_t bits = xEventGroupWaitBits(
        s_events, CAPTURE_DONE_BIT, pdFALSE, pdTRUE,
        pdMS_TO_TICKS(CAPTURE_JOIN_TIMEOUT_MS));
    if ((bits & CAPTURE_DONE_BIT) == 0) return ESP_ERR_TIMEOUT;
    s_rt.task = NULL;
    s_rt.recording = false;
    return ESP_OK;
}

esp_err_t voice_capture_idf_init(
    const xiaojing_hal_t *hal, size_t max_pcm_bytes)
{
    if (!hal || !hal->audio_start_capture ||
        !hal->audio_stop_capture || !hal->audio_read_pcm)
        return ESP_ERR_INVALID_ARG;
    if (max_pcm_bytes == 0)
        max_pcm_bytes = VOICE_CAPTURE_DEFAULT_PCM_BYTES;
    if (max_pcm_bytes > VOICE_CAPTURE_DEFAULT_PCM_BYTES ||
        max_pcm_bytes < 3200U)
        return ESP_ERR_INVALID_SIZE;
    if (!s_lock)
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    if (!s_events)
        s_events = xEventGroupCreateStatic(&s_events_storage);
    if (!s_lock || !s_events) return ESP_ERR_NO_MEM;
    if (!take_lock()) return ESP_ERR_TIMEOUT;
    if (s_rt.initialized) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t *wav = heap_caps_malloc(
        WAV_HEADER_BYTES + max_pcm_bytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wav) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    memset(&s_rt, 0, sizeof(s_rt));
    s_rt.hal = hal;
    s_rt.wav = wav;
    s_rt.capacity = WAV_HEADER_BYTES + max_pcm_bytes;
    s_rt.initialized = true;
    atomic_init(&s_rt.pcm_bytes, 0);
    atomic_init(&s_rt.stop_requested, false);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t voice_capture_idf_start(void *context)
{
    (void)context;
    if (!take_lock()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.initialized || s_rt.recording || s_rt.leased) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store_explicit(&s_rt.pcm_bytes, 0, memory_order_release);
    s_rt.snapshot.raw_pcm_bytes = 0U;
    s_rt.snapshot.speech_pcm_bytes = 0U;
    s_rt.snapshot.peak_amplitude = 0U;
    s_rt.snapshot.rms_amplitude = 0U;
    s_rt.snapshot.speech_found = false;
    atomic_store_explicit(
        &s_rt.stop_requested, false, memory_order_release);
    xEventGroupClearBits(s_events, CAPTURE_DONE_BIT);
    esp_err_t err = s_rt.hal->audio_start_capture();
    if (err == ESP_OK) {
        BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
            capture_task, "voice_capture", CAPTURE_TASK_STACK_BYTES,
            NULL, 6, &s_rt.task, 0,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (created != pdPASS) {
            s_rt.task = NULL;
            (void)s_rt.hal->audio_stop_capture();
            err = ESP_ERR_NO_MEM;
        } else {
            s_rt.recording = true;
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t voice_capture_idf_import_pcm(
    const uint8_t *pcm, size_t pcm_bytes)
{
    if (!pcm || pcm_bytes < 320U) return ESP_ERR_INVALID_ARG;
    if (!take_lock()) return ESP_ERR_TIMEOUT;
    if (!s_rt.initialized || s_rt.recording || s_rt.leased) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (pcm_bytes > s_rt.capacity - WAV_HEADER_BYTES) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(s_rt.wav + WAV_HEADER_BYTES, pcm, pcm_bytes);
    atomic_store_explicit(&s_rt.pcm_bytes, pcm_bytes, memory_order_release);
    s_rt.task = NULL;
    s_rt.recording = true;
    s_rt.snapshot.raw_pcm_bytes = pcm_bytes;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "imported shared recorder PCM: %u bytes",
             (unsigned)pcm_bytes);
    return ESP_OK;
}

esp_err_t voice_capture_idf_finish(
    const uint8_t **wav, size_t *wav_length,
    uint32_t *token, void *context)
{
    (void)context;
    if (!wav || !wav_length || !token) return ESP_ERR_INVALID_ARG;
    if (!take_lock()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.initialized || !s_rt.recording || s_rt.leased) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = join_capture();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "capture join failed: %s (0x%x)",
                 esp_err_to_name(err), (unsigned)err);
        xSemaphoreGive(s_lock);
        return err;
    }
    size_t pcm = atomic_load_explicit(
        &s_rt.pcm_bytes, memory_order_acquire);
    if (pcm < 320U) {
        ESP_LOGW(TAG, "capture rejected: too short (%u PCM bytes)",
                 (unsigned)pcm);
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_SIZE;
    }
    s_rt.snapshot.raw_pcm_bytes = pcm;
    voice_vad_config_t vad_config = VOICE_VAD_DEFAULT_CONFIG();
    voice_vad_result_t vad;
    if (!voice_vad_analyze((const int16_t *)(s_rt.wav + WAV_HEADER_BYTES),
                           pcm / sizeof(int16_t), &vad_config, &vad)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_rt.snapshot.peak_amplitude = vad.peak_amplitude;
    s_rt.snapshot.rms_amplitude = vad.rms_amplitude;
    s_rt.snapshot.speech_found = vad.speech_found;
    if (!vad.speech_found) {
        s_rt.snapshot.speech_pcm_bytes = 0U;
        s_rt.snapshot.rejected_silence++;
        ESP_LOGW(TAG,
                 "capture rejected: no speech raw=%u peak=%u rms=%u",
                 (unsigned)pcm, (unsigned)vad.peak_amplitude,
                 (unsigned)vad.rms_amplitude);
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_SIZE;
    }
    size_t speech_bytes =
        (vad.end_sample - vad.start_sample) * sizeof(int16_t);
    memmove(s_rt.wav + WAV_HEADER_BYTES,
            s_rt.wav + WAV_HEADER_BYTES +
                vad.start_sample * sizeof(int16_t),
            speech_bytes);
    pcm = speech_bytes;
    s_rt.snapshot.speech_pcm_bytes = pcm;
    s_rt.snapshot.completed_captures++;
    ESP_LOGI(TAG,
             "capture ready: raw=%u speech=%u peak=%u rms=%u",
             (unsigned)s_rt.snapshot.raw_pcm_bytes, (unsigned)pcm,
             (unsigned)vad.peak_amplitude, (unsigned)vad.rms_amplitude);
    write_wav_header(s_rt.wav, pcm);
    s_rt.token++;
    if (s_rt.token == 0) s_rt.token = 1;
    s_rt.leased = true;
    *wav = s_rt.wav;
    *wav_length = WAV_HEADER_BYTES + pcm;
    *token = s_rt.token;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t voice_capture_idf_cancel(void *context)
{
    (void)context;
    if (!take_lock()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.initialized) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    esp_err_t err = join_capture();
    xSemaphoreGive(s_lock);
    return err;
}

void voice_capture_idf_release(uint32_t token, void *context)
{
    (void)context;
    if (!take_lock()) return;
    if (s_rt.leased && token == s_rt.token) s_rt.leased = false;
    xSemaphoreGive(s_lock);
}

esp_err_t voice_capture_idf_get_snapshot(voice_capture_idf_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!take_lock()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.initialized) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    *out = s_rt.snapshot;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t voice_capture_idf_deinit(void)
{
    if (!s_lock || !take_lock()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.initialized) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    esp_err_t err = join_capture();
    if (err != ESP_OK || s_rt.leased) {
        xSemaphoreGive(s_lock);
        return err != ESP_OK ? err : ESP_ERR_INVALID_STATE;
    }
    uint8_t *wav = s_rt.wav;
    memset(&s_rt, 0, sizeof(s_rt));
    xSemaphoreGive(s_lock);
    heap_caps_free(wav);
    return ESP_OK;
}
