#include "voice_wake_esp_sr.h"

#include <string.h>

#include "esp_attr.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* ESP-SR headers for ESP32-S3 */
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_wn_models.h"
#include "model_path.h"

static const char *TAG = "wake_esp_sr";

/* ---- State ---- */

typedef struct {
    bool initialized;
    bool running;
    bool stop_requested;
    float threshold;
    uint32_t cooldown_ms;
    int64_t last_detection_us;
    uint32_t detection_count;
    uint32_t frame_count;
    uint32_t feed_count;

    /* ESP-SR model mapping and AFE handle */
    srmodel_list_t *models;
    esp_afe_sr_data_t *afe_handle;
    int chunk_size;         /* Samples per feed call */
    int16_t *chunk_buffer;  /* Exact AFE feed chunk, allocated in PSRAM */
    size_t chunk_fill;

    /* Statistics */
    int64_t create_time_us;
    int64_t start_time_us;
    int64_t stop_time_us;
    uint32_t create_duration_ms;
    uint32_t stop_duration_ms;

    SemaphoreHandle_t mutex;
    TaskHandle_t fetch_task;
} esp_sr_state_t;

static esp_sr_state_t s_esp_sr = {0};

/* esp_srmodel_init()/deinit() temporarily freeze the instruction/data
 * caches while mapping the model partition.  Those ESP-IDF paths require
 * the calling task's stack to be in internal RAM.  The button service task
 * deliberately uses a PSRAM stack, so run only the model lifecycle operation
 * on this short-lived internal-stack worker. */
#define ESP_SR_MODEL_TASK_STACK_BYTES 8192U
#define ESP_SR_MODEL_TASK_STACK_WORDS \
    (ESP_SR_MODEL_TASK_STACK_BYTES / sizeof(StackType_t))
#define ESP_SR_MODEL_TASK_TIMEOUT_MS 60000U
#define ESP_SR_MODEL_TASK_CORE      0

typedef enum {
    ESP_SR_MODEL_OP_INIT = 0,
    ESP_SR_MODEL_OP_DEINIT,
} esp_sr_model_op_t;

static StaticSemaphore_t s_model_done_storage;
static SemaphoreHandle_t s_model_done;
/* Model mapping/deinit briefly disables instruction/data caches and must run
 * on an internal-RAM stack.  A dynamically allocated worker worked only on
 * the first backend switch: after AFE/WiFi allocations the heap could not
 * provide another contiguous stack block.  Reuse a fixed static worker
 * stack/TCB so switching does not depend on internal heap fragmentation. */
static StaticTask_t s_model_task_tcb;
static StackType_t s_model_task_stack[
    ESP_SR_MODEL_TASK_STACK_WORDS];
static voice_wake_config_t s_model_config;
static esp_sr_model_op_t s_model_op;
static esp_err_t s_model_result;

#define ESP_SR_FETCH_TASK_STACK_WORDS 3072U
static StaticTask_t s_fetch_task_tcb;
static EXT_RAM_BSS_ATTR StackType_t s_fetch_task_stack[
    ESP_SR_FETCH_TASK_STACK_WORDS];
static StaticSemaphore_t s_fetch_done_storage;
static SemaphoreHandle_t s_fetch_done;

static void esp_sr_model_task(void *arg);
static esp_err_t esp_sr_init_impl(const voice_wake_config_t *config);
static esp_err_t esp_sr_deinit_impl(void);
static esp_err_t esp_sr_feed(const int16_t *samples, size_t count);
static void esp_sr_fetch_task(void *arg);

static esp_err_t run_model_op_internal(esp_sr_model_op_t op,
                                        const voice_wake_config_t *config)
{
    if (!s_model_done) {
        s_model_done = xSemaphoreCreateBinaryStatic(&s_model_done_storage);
        if (!s_model_done) return ESP_ERR_NO_MEM;
    }
    while (xSemaphoreTake(s_model_done, 0) == pdTRUE) { }
    s_model_op = op;
    if (config) s_model_config = *config;
    s_model_result = ESP_FAIL;

    TaskHandle_t task = xTaskCreateStaticPinnedToCore(
        esp_sr_model_task, "esp_sr_model", ESP_SR_MODEL_TASK_STACK_WORDS,
        NULL, 6, s_model_task_stack, &s_model_task_tcb,
        ESP_SR_MODEL_TASK_CORE);
    if (!task) {
        ESP_LOGE(TAG, "model worker create failed (internal heap)");
        return ESP_ERR_NO_MEM;
    }

    TickType_t wait_ticks = pdMS_TO_TICKS(ESP_SR_MODEL_TASK_TIMEOUT_MS);
    if (wait_ticks == 0) wait_ticks = 1;
    if (xSemaphoreTake(s_model_done, wait_ticks) != pdTRUE) {
        ESP_LOGE(TAG, "model worker timeout; preserving lifecycle state");
        return ESP_ERR_TIMEOUT;
    }
    return s_model_result;
}

/* ---- Backend Operations ---- */

static esp_err_t esp_sr_init_impl(const voice_wake_config_t *config)
{
    if (s_esp_sr.initialized) return ESP_ERR_INVALID_STATE;
    if (!config) return ESP_ERR_INVALID_ARG;

    memset(&s_esp_sr, 0, sizeof(s_esp_sr));

    s_esp_sr.threshold = config->threshold > 0 ? config->threshold : 0.80f;
    s_esp_sr.cooldown_ms = config->cooldown_ms > 0 ? config->cooldown_ms : 2000;

    s_esp_sr.mutex = xSemaphoreCreateMutex();
    if (!s_esp_sr.mutex) return ESP_ERR_NO_MEM;
    s_fetch_done = xSemaphoreCreateBinaryStatic(&s_fetch_done_storage);
    if (!s_fetch_done) {
        vSemaphoreDelete(s_esp_sr.mutex);
        s_esp_sr.mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Load the generated WakeNet image lazily when ESP-SR is selected. */
    s_esp_sr.models = esp_srmodel_init("model");
    if (!s_esp_sr.models) {
        ESP_LOGE(TAG, "failed to map ESP-SR model partition");
        vSemaphoreDelete(s_esp_sr.mutex);
        s_esp_sr.mutex = NULL;
        return ESP_ERR_NOT_FOUND;
    }
    char *model_name = esp_srmodel_filter(
        s_esp_sr.models, "wn9", "xiaoaitongxue");
    if (!model_name) {
        ESP_LOGE(TAG, "WakeNet9 xiaoaitongxue model not found");
        esp_srmodel_deinit(s_esp_sr.models);
        s_esp_sr.models = NULL;
        vSemaphoreDelete(s_esp_sr.mutex);
        s_esp_sr.mutex = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    /* Configure AFE for single INMP441 microphone */
    afe_config_t afe_config = {
        .aec_init = false,          /* No echo cancellation (single mic) */
        .se_init = false,           /* No speech enhancement */
        .vad_init = false,          /* No VAD */
        .wakenet_init = true,       /* Enable WakeNet */
        .voice_communication_init = false,
        .voice_communication_agc_init = false,
        .voice_communication_agc_gain = 0,
        .vad_mode = VAD_MODE_3,
        .wakenet_model_name = model_name,
        .wakenet_model_name_2 = NULL,
        .wakenet_mode = DET_MODE_90,
        .afe_mode = SR_MODE_LOW_COST,
        .afe_perferred_core = 1,
        .afe_perferred_priority = 5,
        .afe_ringbuf_size = 50,
        .memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM,
        .afe_linear_gain = 1.0f,
        .agc_mode = AFE_MN_PEAK_NO_AGC,
        .pcm_config = {
            .total_ch_num = 1,
            .mic_num = 1,
            .ref_num = 0,
            .sample_rate = 16000,
        },
        .debug_init = false,
        .afe_ns_mode = NS_MODE_SSP,
        .afe_ns_model_name = NULL,
        .fixed_first_channel = true,
    };

    int64_t t0 = esp_timer_get_time();

    /* Create AFE instance */
    const esp_afe_sr_iface_t *afe_iface = &ESP_AFE_SR_HANDLE;
    s_esp_sr.afe_handle = afe_iface->create_from_config(&afe_config);
    if (!s_esp_sr.afe_handle) {
        ESP_LOGE(TAG, "AFE create failed");
        esp_srmodel_deinit(s_esp_sr.models);
        s_esp_sr.models = NULL;
        vSemaphoreDelete(s_esp_sr.mutex);
        s_esp_sr.mutex = NULL;
        return ESP_FAIL;
    }

    /* Get chunk size */
    s_esp_sr.chunk_size = afe_iface->get_feed_chunksize(s_esp_sr.afe_handle);
    if (s_esp_sr.chunk_size <= 0) {
        ESP_LOGE(TAG, "invalid AFE feed chunk size: %d", s_esp_sr.chunk_size);
        afe_iface->destroy(s_esp_sr.afe_handle);
        s_esp_sr.afe_handle = NULL;
        esp_srmodel_deinit(s_esp_sr.models);
        s_esp_sr.models = NULL;
        vSemaphoreDelete(s_esp_sr.mutex);
        s_esp_sr.mutex = NULL;
        return ESP_ERR_INVALID_SIZE;
    }
    s_esp_sr.chunk_buffer = heap_caps_calloc(
        (size_t)s_esp_sr.chunk_size, sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_esp_sr.chunk_buffer) {
        ESP_LOGE(TAG, "AFE chunk allocation failed (%d samples)",
                 s_esp_sr.chunk_size);
        afe_iface->destroy(s_esp_sr.afe_handle);
        s_esp_sr.afe_handle = NULL;
        esp_srmodel_deinit(s_esp_sr.models);
        s_esp_sr.models = NULL;
        vSemaphoreDelete(s_esp_sr.mutex);
        s_esp_sr.mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_esp_sr.create_time_us = esp_timer_get_time();
    s_esp_sr.create_duration_ms = (uint32_t)((s_esp_sr.create_time_us - t0) / 1000);
    s_esp_sr.initialized = true;

    ESP_LOGI(TAG, "ESP-SR AFE initialized");
    ESP_LOGI(TAG, "  model: %s", model_name);
    ESP_LOGI(TAG, "  chunk_size: %d samples", s_esp_sr.chunk_size);
    ESP_LOGI(TAG, "  memory_alloc: PSRAM preferred");
    ESP_LOGI(TAG, "  create_time: %u ms", (unsigned)s_esp_sr.create_duration_ms);

    return ESP_OK;
}

static esp_err_t esp_sr_start(void)
{
    if (!s_esp_sr.initialized) return ESP_ERR_INVALID_STATE;
    if (s_esp_sr.running) return ESP_OK;

    xSemaphoreTake(s_esp_sr.mutex, portMAX_DELAY);

    s_esp_sr.stop_requested = false;
    s_esp_sr.running = true;
    s_esp_sr.frame_count = 0;
    s_esp_sr.feed_count = 0;
    s_esp_sr.detection_count = 0;
    s_esp_sr.chunk_fill = 0;
    s_esp_sr.start_time_us = esp_timer_get_time();

    while (xSemaphoreTake(s_fetch_done, 0) == pdTRUE) { }
    s_esp_sr.fetch_task = xTaskCreateStaticPinnedToCore(
        esp_sr_fetch_task, "esp_sr_fetch", ESP_SR_FETCH_TASK_STACK_WORDS,
        NULL, 5, s_fetch_task_stack, &s_fetch_task_tcb, 1);
    if (!s_esp_sr.fetch_task) {
        s_esp_sr.running = false;
        xSemaphoreGive(s_esp_sr.mutex);
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreGive(s_esp_sr.mutex);

    ESP_LOGI(TAG, "started");
    return ESP_OK;
}

static esp_err_t esp_sr_stop(uint32_t timeout_ms)
{
    if (!s_esp_sr.initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_esp_sr.mutex, portMAX_DELAY);

    int64_t t0 = esp_timer_get_time();
    /* Keep running=true until the fetch task has joined.  The capture task
     * must still be allowed to feed one final AFE chunk so a blocked fetch
     * can return and observe the stop request. */
    s_esp_sr.stop_requested = true;
    s_esp_sr.stop_time_us = esp_timer_get_time();
    s_esp_sr.stop_duration_ms = (uint32_t)((s_esp_sr.stop_time_us - t0) / 1000);

    xSemaphoreGive(s_esp_sr.mutex);

    if (s_esp_sr.fetch_task && s_fetch_done) {
        TickType_t ticks = pdMS_TO_TICKS(timeout_ms ? timeout_ms : 2000U);
        if (ticks == 0) ticks = 1;
        TickType_t deadline = xTaskGetTickCount() + ticks;
        /* When the audio controller is in DIALOG it intentionally routes
         * PCM away from ESP-SR.  AFE fetch() can then be waiting forever on
         * its input ring, so merely setting stop_requested is insufficient.
         * Feed bounded silence chunks while joining; this wakes fetch(),
         * lets it observe stop_requested, and avoids the half-deinitialized
         * backend that made every subsequent start return 0x103. */
        int16_t silence[512] = {0};
        while (s_esp_sr.fetch_task) {
            TickType_t now = xTaskGetTickCount();
            if (xSemaphoreTake(s_fetch_done, pdMS_TO_TICKS(20)) == pdTRUE) {
                s_esp_sr.fetch_task = NULL;
                break;
            }
            if ((int32_t)(now - deadline) >= 0) {
                ESP_LOGW(TAG, "fetch join timeout");
                return ESP_ERR_TIMEOUT;
            }
            size_t chunk = s_esp_sr.chunk_size > 0
                ? (size_t)s_esp_sr.chunk_size : 160U;
            if (chunk > 512U) chunk = 512U;
            (void)esp_sr_feed(silence, chunk);
        }
    }

    xSemaphoreTake(s_esp_sr.mutex, portMAX_DELAY);
    s_esp_sr.running = false;
    xSemaphoreGive(s_esp_sr.mutex);

    ESP_LOGI(TAG, "stopped (took %u ms)", (unsigned)s_esp_sr.stop_duration_ms);
    return ESP_OK;
}

static esp_err_t esp_sr_deinit_impl(void)
{
    if (!s_esp_sr.initialized) return ESP_ERR_INVALID_STATE;
    if (s_esp_sr.running) return ESP_ERR_INVALID_STATE;
    if (s_esp_sr.fetch_task) return ESP_ERR_TIMEOUT;

    xSemaphoreTake(s_esp_sr.mutex, portMAX_DELAY);

    /* Destroy AFE instance */
    if (s_esp_sr.afe_handle) {
        const esp_afe_sr_iface_t *afe_iface = &ESP_AFE_SR_HANDLE;
        afe_iface->destroy(s_esp_sr.afe_handle);
        s_esp_sr.afe_handle = NULL;
    }
    if (s_esp_sr.chunk_buffer) {
        heap_caps_free(s_esp_sr.chunk_buffer);
        s_esp_sr.chunk_buffer = NULL;
    }
    if (s_esp_sr.models) {
        esp_srmodel_deinit(s_esp_sr.models);
        s_esp_sr.models = NULL;
    }

    s_esp_sr.initialized = false;
    SemaphoreHandle_t mutex = s_esp_sr.mutex;
    s_esp_sr.mutex = NULL;
    xSemaphoreGive(mutex);
    vSemaphoreDelete(mutex);

    ESP_LOGI(TAG, "deinitialized");
    return ESP_OK;
}

static void esp_sr_model_task(void *arg)
{
    (void)arg;
    if (s_model_op == ESP_SR_MODEL_OP_INIT)
        s_model_result = esp_sr_init_impl(&s_model_config);
    else
        s_model_result = esp_sr_deinit_impl();
    xSemaphoreGive(s_model_done);
    vTaskDelete(NULL);
}

static esp_err_t esp_sr_init(const voice_wake_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    return run_model_op_internal(ESP_SR_MODEL_OP_INIT, config);
}

static esp_err_t esp_sr_deinit(void)
{
    return run_model_op_internal(ESP_SR_MODEL_OP_DEINIT, NULL);
}

static esp_err_t esp_sr_feed(const int16_t *samples, size_t count)
{
    if (!s_esp_sr.initialized || !s_esp_sr.running) return ESP_ERR_INVALID_STATE;
    if (!samples || count == 0) return ESP_ERR_INVALID_ARG;
    if (!s_esp_sr.afe_handle) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_esp_sr.mutex, portMAX_DELAY) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    s_esp_sr.frame_count++;
    s_esp_sr.feed_count += count;

    /* ESP-SR requires exactly get_feed_chunksize() samples per feed call.
     * The shared audio controller uses independent 512-sample frames, so
     * bridge the contracts with a bounded accumulator. */
    const esp_afe_sr_iface_t *afe_iface = &ESP_AFE_SR_HANDLE;
    size_t offset = 0;
    while (offset < count) {
        size_t need = (size_t)s_esp_sr.chunk_size - s_esp_sr.chunk_fill;
        size_t copy = count - offset < need ? count - offset : need;
        memcpy(s_esp_sr.chunk_buffer + s_esp_sr.chunk_fill,
               samples + offset, copy * sizeof(int16_t));
        s_esp_sr.chunk_fill += copy;
        offset += copy;
        if (s_esp_sr.chunk_fill != (size_t)s_esp_sr.chunk_size) continue;

        /* Feed directly to AFE — no separate worker task needed.
         * The AFE model lives in PSRAM and its feed/fetch run from flash,
         * so no internal-RAM stack constraint applies here. */
        int feed_result = afe_iface->feed(
            s_esp_sr.afe_handle, s_esp_sr.chunk_buffer);
        s_esp_sr.chunk_fill = 0;
        /* ESP-SR feed() returns bytes written to its input ring
         * buffer (e.g. 320 for a 160-sample mono frame), not an ESP
         * error code.  Only negative values indicate failure. */
        if (feed_result < 0) {
            ESP_LOGW(TAG, "AFE feed failed: %d", feed_result);
            xSemaphoreGive(s_esp_sr.mutex);
            return ESP_FAIL;
        }

    }

    xSemaphoreGive(s_esp_sr.mutex);
    return ESP_OK;
}

static void esp_sr_fetch_task(void *arg)
{
    (void)arg;
    const esp_afe_sr_iface_t *afe_iface = &ESP_AFE_SR_HANDLE;
    while (s_esp_sr.afe_handle) {
        afe_fetch_result_t *result = afe_iface->fetch(s_esp_sr.afe_handle);
        if (!result) continue;
        int64_t now = esp_timer_get_time();
        if (xSemaphoreTake(s_esp_sr.mutex, pdMS_TO_TICKS(20)) != pdTRUE)
            continue;
        bool stop_requested = s_esp_sr.stop_requested;
        bool running = s_esp_sr.running;
        if (!running || stop_requested) {
            xSemaphoreGive(s_esp_sr.mutex);
            break;
        }
        if (result->wakeup_state != WAKENET_DETECTED) {
            xSemaphoreGive(s_esp_sr.mutex);
            continue;
        }
        if (s_esp_sr.last_detection_us <= 0 ||
            now - s_esp_sr.last_detection_us >=
                (int64_t)s_esp_sr.cooldown_ms * 1000) {
            s_esp_sr.detection_count++;
            s_esp_sr.last_detection_us = now;
            ESP_LOGI(TAG,
                     "DETECTED xiao_ai_tong_xue (count=%u, word=%d)",
                     (unsigned)s_esp_sr.detection_count,
                     result->wake_word_index);
        }
        xSemaphoreGive(s_esp_sr.mutex);
    }
    if (s_fetch_done) xSemaphoreGive(s_fetch_done);
    vTaskDelete(NULL);
}

static esp_err_t esp_sr_get_snapshot(voice_wake_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_esp_sr.initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_esp_sr.mutex, portMAX_DELAY);

    out->backend = VOICE_WAKE_BACKEND_ESP_SR;
    out->state = s_esp_sr.running ? VOICE_WAKE_STATE_LISTENING : VOICE_WAKE_STATE_STOPPED;
    out->audio_owner = s_esp_sr.running ? VOICE_AUDIO_OWNER_WAKE_ESP_SR : VOICE_AUDIO_OWNER_NONE;
    out->frame_count = s_esp_sr.frame_count;
    out->detection_count = s_esp_sr.detection_count;
    out->sequence = s_esp_sr.detection_count;
    out->last_detection_us = s_esp_sr.last_detection_us;
    out->last_start_us = s_esp_sr.start_time_us;
    out->last_stop_us = s_esp_sr.stop_time_us;
    out->stop_duration_ms = s_esp_sr.stop_duration_ms;
    out->create_duration_ms = s_esp_sr.create_duration_ms;
    out->timestamp_us = esp_timer_get_time();

    xSemaphoreGive(s_esp_sr.mutex);
    return ESP_OK;
}

/* ---- Static Operations Table ---- */

static const voice_wake_backend_ops_t s_esp_sr_ops = {
    .init = esp_sr_init,
    .start = esp_sr_start,
    .stop = esp_sr_stop,
    .deinit = esp_sr_deinit,
    .feed = esp_sr_feed,
    .get_snapshot = esp_sr_get_snapshot,
    .name = "esp_sr_xiao_ai_tong_xue",
};

/* ---- Public API ---- */

const voice_wake_backend_ops_t *voice_wake_esp_sr_get_ops(void)
{
    return &s_esp_sr_ops;
}

esp_err_t voice_wake_esp_sr_init(void)
{
    ESP_LOGI(TAG, "ESP-SR wake backend ready");
    ESP_LOGI(TAG, "Legal: '小爱同学' is a trademark of Xiaomi Inc.");
    ESP_LOGI(TAG, "This implementation is for demonstration/competition use only.");
    return ESP_OK;
}
