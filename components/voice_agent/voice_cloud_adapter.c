#include "voice_cloud_adapter.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "voice_service.h"

/* TLS certificate validation plus the streamed OpenAI-compatible request
 * traverses mbedTLS, HTTP client and JSON parsing on this task.  8 KiB was
 * proven insufficient on ESP32-S3 (stack canary fired immediately after
 * certificate validation).  The task stack is allocated from PSRAM, so a
 * 32 KiB safety margin does not consume scarce internal DMA/socket memory. */
#define VOICE_CLOUD_ADAPTER_STACK_DEFAULT 32768U
#define VOICE_CLOUD_ADAPTER_PRIORITY_DEFAULT 5U
#define VOICE_CLOUD_ADAPTER_STOP_TIMEOUT_MS 35000U
#define VOICE_CLOUD_ADAPTER_LOCK_TIMEOUT_MS 100U
#define VOICE_CLOUD_ADAPTER_RESPONSE_CAPACITY \
    (VOICE_CLOUD_DEFAULT_MAX_RESPONSE_BYTES + 1U)

static const char *TAG = "voice_cloud_adapter";

typedef struct {
    const uint8_t *wav;
    size_t wav_length;
    uint32_t token;
    uint32_t generation;
    voice_invocation_source_t invocation_source;
    voice_route_policy_t route_policy;
} voice_cloud_job_t;

typedef struct {
    voice_cloud_adapter_config_t config;
    voice_cloud_adapter_snapshot_t snapshot;
    TaskHandle_t task;
    _Atomic bool stop_requested;
    _Atomic bool cancel_requested;
    bool joined;
    char response[VOICE_CLOUD_ADAPTER_RESPONSE_CAPACITY];
} voice_cloud_adapter_runtime_t;

static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_done_storage;
static SemaphoreHandle_t s_done;
static StaticQueue_t s_queue_storage;
static uint8_t s_queue_bytes[sizeof(voice_cloud_job_t)];
static QueueHandle_t s_queue;
static voice_cloud_adapter_runtime_t s_rt;
static _Atomic int s_global_state;

enum {
    GLOBAL_UNINITIALIZED = 0,
    GLOBAL_INITIALIZING,
    GLOBAL_READY,
    GLOBAL_FAILED,
};

static bool take_lock(void)
{
    if (!s_lock) return false;
    TickType_t ticks = pdMS_TO_TICKS(VOICE_CLOUD_ADAPTER_LOCK_TIMEOUT_MS);
    if (ticks == 0) ticks = 1;
    return xSemaphoreTake(s_lock, ticks) == pdTRUE;
}

static uint32_t next_nonzero(uint32_t value)
{
    value++;
    return value == 0 ? 1 : value;
}

static void release_job(const voice_cloud_job_t *job,
                        const voice_cloud_adapter_config_t *config)
{
    if (job && job->wav && config && config->record_release)
        config->record_release(job->token, config->record_context);
}

static void default_completion(voice_cloud_result_t result,
                               const voice_cloud_route_output_t *output)
{
    if (result != VOICE_CLOUD_OK) {
        (void)voice_service_handle_event(
            VOICE_EVENT_FAILURE, (uint32_t)result);
        return;
    }
    if (!output ||
        voice_service_handle_event(VOICE_EVENT_CLOUD_RESPONSE, 0) != ESP_OK)
        return;
    (void)voice_service_process_route_result(&output->route);
}

static void finish_job(const voice_cloud_job_t *job,
                       voice_cloud_result_t result,
                       const voice_cloud_route_output_t *output,
                       const voice_cloud_adapter_config_t *job_config)
{
    bool deliver = false;
    if (take_lock()) {
        bool canceled = atomic_load_explicit(
            &s_rt.cancel_requested, memory_order_acquire);
        deliver = !canceled &&
                  job->generation == s_rt.snapshot.generation;
        s_rt.snapshot.job_active = false;
        s_rt.snapshot.active_token = 0;
        s_rt.snapshot.last_result = canceled ?
            VOICE_CLOUD_CANCELED : result;
        if (canceled) {
            s_rt.snapshot.canceled_jobs++;
        } else {
            s_rt.snapshot.completed_jobs++;
        }
        xSemaphoreGive(s_lock);
    }

    release_job(job, job_config);
    if (!deliver) return;
    if (job_config->completion) {
        job_config->completion(
            result, result == VOICE_CLOUD_OK ? output : NULL,
            job->token, job_config->completion_context);
    } else {
        default_completion(result, output);
    }
}

static void worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (atomic_load_explicit(
                &s_rt.stop_requested, memory_order_acquire))
            break;

        voice_cloud_job_t job;
        if (xQueueReceive(s_queue, &job, pdMS_TO_TICKS(50)) != pdTRUE)
            continue;

        voice_cloud_adapter_config_t config;
        bool execute = false;
        if (take_lock()) {
            s_rt.snapshot.job_pending = false;
            execute = !atomic_load_explicit(
                &s_rt.cancel_requested, memory_order_acquire);
            if (execute) {
                s_rt.snapshot.job_active = true;
                s_rt.snapshot.active_token = job.token;
            }
            config = s_rt.config;
            xSemaphoreGive(s_lock);
        } else {
            memset(&config, 0, sizeof(config));
        }

        if (!execute) {
            finish_job(&job, VOICE_CLOUD_CANCELED, NULL, &config);
            continue;
        }

        voice_cloud_route_output_t output;
        voice_cloud_result_t result = voice_cloud_run_auto_route(
            &config.cloud, job.invocation_source, job.route_policy,
            job.wav, job.wav_length,
            s_rt.response, sizeof(s_rt.response), &output);
        bool retryable = result == VOICE_CLOUD_TRANSPORT_ERROR ||
                         result == VOICE_CLOUD_TLS_ERROR ||
                         result == VOICE_CLOUD_BAD_RESPONSE;
        if (retryable &&
            !atomic_load_explicit(
                &s_rt.cancel_requested, memory_order_acquire) &&
            !atomic_load_explicit(
                &s_rt.stop_requested, memory_order_acquire) &&
            (!config.cloud.network_ready ||
             config.cloud.network_ready(config.cloud.network_context))) {
            ESP_LOGW(TAG, "cloud result %d incomplete/transient; retrying once",
                     (int)result);
            vTaskDelay(pdMS_TO_TICKS(250));
            result = voice_cloud_run_auto_route(
                &config.cloud, job.invocation_source, job.route_policy,
                job.wav, job.wav_length,
                s_rt.response, sizeof(s_rt.response), &output);
        }
        finish_job(&job, result, &output, &config);
    }

    if (take_lock()) {
        s_rt.snapshot.running = false;
        xSemaphoreGive(s_lock);
    }
    xSemaphoreGive(s_done);
    vTaskDeleteWithCaps(NULL);
}

esp_err_t voice_cloud_adapter_global_init(void)
{
    int state = atomic_load_explicit(
        &s_global_state, memory_order_acquire);
    if (state == GLOBAL_READY) return ESP_OK;
    if (state == GLOBAL_FAILED) return ESP_ERR_NO_MEM;

    int expected = GLOBAL_UNINITIALIZED;
    if (atomic_compare_exchange_strong_explicit(
            &s_global_state, &expected, GLOBAL_INITIALIZING,
            memory_order_acq_rel, memory_order_acquire)) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
        s_done = xSemaphoreCreateBinaryStatic(&s_done_storage);
        s_queue = xQueueCreateStatic(
            1, sizeof(voice_cloud_job_t), s_queue_bytes, &s_queue_storage);
        bool ready = s_lock && s_done && s_queue;
        atomic_store_explicit(
            &s_global_state,
            ready ? GLOBAL_READY : GLOBAL_FAILED,
            memory_order_release);
        return ready ? ESP_OK : ESP_ERR_NO_MEM;
    }

    for (uint32_t i = 0; i < 1000; i++) {
        state = atomic_load_explicit(
            &s_global_state, memory_order_acquire);
        if (state == GLOBAL_READY) return ESP_OK;
        if (state == GLOBAL_FAILED) return ESP_ERR_NO_MEM;
        taskYIELD();
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t voice_cloud_adapter_init(
    const voice_cloud_adapter_config_t *config)
{
    if (!config || !voice_cloud_config_valid(&config->cloud) ||
        !config->record_start || !config->record_finish ||
        !config->record_release)
        return ESP_ERR_INVALID_ARG;
    esp_err_t err = voice_cloud_adapter_global_init();
    if (err != ESP_OK) return err;
    if (!take_lock()) return ESP_ERR_TIMEOUT;
    if (s_rt.snapshot.initialized || s_rt.task) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_rt, 0, sizeof(s_rt));
    s_rt.config = *config;
    s_rt.snapshot.initialized = true;
    s_rt.snapshot.generation = 1;
    s_rt.snapshot.last_result = VOICE_CLOUD_OK;
    s_rt.snapshot.invocation_source = VOICE_INVOCATION_WAKE_WORD;
    s_rt.snapshot.route_policy = VOICE_ROUTE_POLICY_AUTO;
    atomic_init(&s_rt.stop_requested, false);
    atomic_init(&s_rt.cancel_requested, false);
    xQueueReset(s_queue);
    (void)xSemaphoreTake(s_done, 0);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t voice_cloud_adapter_start(void)
{
    if (!take_lock()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.snapshot.initialized || s_rt.snapshot.running || s_rt.task) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store_explicit(
        &s_rt.stop_requested, false, memory_order_release);
    atomic_store_explicit(
        &s_rt.cancel_requested, false, memory_order_release);
    s_rt.joined = false;
    uint32_t stack = s_rt.config.task_stack_bytes ?
        s_rt.config.task_stack_bytes : VOICE_CLOUD_ADAPTER_STACK_DEFAULT;
    UBaseType_t priority = s_rt.config.task_priority ?
        (UBaseType_t)s_rt.config.task_priority :
        (UBaseType_t)VOICE_CLOUD_ADAPTER_PRIORITY_DEFAULT;
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        worker_task, "voice_cloud", stack, NULL, priority,
        &s_rt.task, s_rt.config.task_core,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        s_rt.task = NULL;
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    s_rt.snapshot.running = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t voice_cloud_adapter_start_recording(void *context)
{
    (void)context;
    if (!take_lock()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.snapshot.initialized || !s_rt.snapshot.running ||
        s_rt.snapshot.job_pending || s_rt.snapshot.job_active) {
        s_rt.snapshot.rejected_jobs++;
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    voice_cloud_adapter_config_t config = s_rt.config;
    atomic_store_explicit(
        &s_rt.cancel_requested, false, memory_order_release);
    s_rt.snapshot.cancel_requested = false;
    xSemaphoreGive(s_lock);
    return config.record_start(config.record_context);
}

esp_err_t voice_cloud_adapter_set_route_context(
    voice_invocation_source_t source,
    voice_route_policy_t policy,
    void *context)
{
    (void)context;
    if (source < VOICE_INVOCATION_WAKE_WORD ||
        source >= VOICE_INVOCATION_COUNT ||
        policy < VOICE_ROUTE_POLICY_AUTO ||
        policy >= VOICE_ROUTE_POLICY_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!take_lock()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.snapshot.initialized || !s_rt.snapshot.running ||
        s_rt.snapshot.job_pending || s_rt.snapshot.job_active) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_rt.snapshot.invocation_source = source;
    s_rt.snapshot.route_policy = policy;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t voice_cloud_adapter_stop_recording_and_upload(void *context)
{
    (void)context;
    if (!take_lock()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.snapshot.initialized || !s_rt.snapshot.running ||
        s_rt.snapshot.job_pending || s_rt.snapshot.job_active) {
        s_rt.snapshot.rejected_jobs++;
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    voice_cloud_adapter_config_t config = s_rt.config;
    uint32_t generation = s_rt.snapshot.generation;
    voice_invocation_source_t source =
        s_rt.snapshot.invocation_source;
    voice_route_policy_t policy = s_rt.snapshot.route_policy;
    xSemaphoreGive(s_lock);

    const uint8_t *wav = NULL;
    size_t wav_length = 0;
    uint32_t token = 0;
    esp_err_t err = config.record_finish(
        &wav, &wav_length, &token, config.record_context);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "record finish rejected: %s (0x%x)",
                 esp_err_to_name(err), (unsigned)err);
        return err;
    }
    if (!wav || !voice_cloud_wav_valid(wav, wav_length) ||
        wav_length > config.cloud.max_wav_bytes) {
        ESP_LOGE(TAG, "record payload invalid: wav=%p bytes=%u max=%u",
                 (const void *)wav, (unsigned)wav_length,
                 (unsigned)config.cloud.max_wav_bytes);
        config.record_release(token, config.record_context);
        return ESP_ERR_INVALID_SIZE;
    }

    voice_cloud_job_t job = {
        .wav = wav,
        .wav_length = wav_length,
        .token = token,
        .generation = generation,
        .invocation_source = source,
        .route_policy = policy,
    };
    if (!take_lock()) {
        config.record_release(token, config.record_context);
        return ESP_ERR_TIMEOUT;
    }
    if (!s_rt.snapshot.running ||
        atomic_load_explicit(
            &s_rt.cancel_requested, memory_order_acquire) ||
        s_rt.snapshot.generation != generation ||
        xQueueSend(s_queue, &job, 0) != pdTRUE) {
        ESP_LOGE(TAG,
                 "cloud job rejected: running=%u cancel=%u generation=%u/%u",
                 (unsigned)s_rt.snapshot.running,
                 (unsigned)atomic_load_explicit(
                     &s_rt.cancel_requested, memory_order_acquire),
                 (unsigned)s_rt.snapshot.generation,
                 (unsigned)generation);
        s_rt.snapshot.rejected_jobs++;
        xSemaphoreGive(s_lock);
        config.record_release(token, config.record_context);
        return ESP_ERR_INVALID_STATE;
    }
    s_rt.snapshot.job_pending = true;
    s_rt.snapshot.submitted_jobs++;
    ESP_LOGI(TAG, "cloud job queued: token=%u wav=%u bytes generation=%u",
             (unsigned)token, (unsigned)wav_length, (unsigned)generation);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t voice_cloud_adapter_cancel_io(void *context)
{
    (void)context;
    if (!take_lock()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.snapshot.initialized) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    voice_cloud_adapter_config_t config = s_rt.config;
    s_rt.snapshot.generation = next_nonzero(s_rt.snapshot.generation);
    s_rt.snapshot.cancel_requested = true;
    atomic_store_explicit(
        &s_rt.cancel_requested, true, memory_order_release);
    xSemaphoreGive(s_lock);

    esp_err_t first = config.record_cancel ?
        config.record_cancel(config.record_context) : ESP_OK;
    if (config.cloud_cancel)
        config.cloud_cancel(config.cloud.transport_context);
    return first;
}

esp_err_t voice_cloud_adapter_stop(void)
{
    if (!s_lock) return ESP_OK;
    if (!take_lock()) return ESP_ERR_TIMEOUT;
    if (!s_rt.snapshot.initialized) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    TaskHandle_t task = s_rt.task;
    bool joined = s_rt.joined;
    xSemaphoreGive(s_lock);

    (void)voice_cloud_adapter_cancel_io(NULL);
    atomic_store_explicit(&s_rt.stop_requested, true, memory_order_release);

    if (task && !joined) {
        TickType_t wait = pdMS_TO_TICKS(
            VOICE_CLOUD_ADAPTER_STOP_TIMEOUT_MS);
        if (xSemaphoreTake(s_done, wait) != pdTRUE)
            return ESP_ERR_TIMEOUT;
    }

    if (!take_lock()) return ESP_ERR_TIMEOUT;
    s_rt.joined = true;
    s_rt.task = NULL;
    voice_cloud_job_t queued;
    if (xQueueReceive(s_queue, &queued, 0) == pdTRUE) {
        voice_cloud_adapter_config_t config = s_rt.config;
        s_rt.snapshot.job_pending = false;
        xSemaphoreGive(s_lock);
        release_job(&queued, &config);
        if (!take_lock()) return ESP_ERR_TIMEOUT;
    }
    s_rt.snapshot.initialized = false;
    s_rt.snapshot.running = false;
    s_rt.snapshot.job_pending = false;
    s_rt.snapshot.job_active = false;
    s_rt.snapshot.active_token = 0;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t voice_cloud_adapter_get_snapshot(
    voice_cloud_adapter_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!take_lock()) return ESP_ERR_INVALID_STATE;
    voice_cloud_adapter_snapshot_t tmp = s_rt.snapshot;
    tmp.cancel_requested = atomic_load_explicit(
        &s_rt.cancel_requested, memory_order_acquire);
    *out = tmp;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

#ifdef XIAOJING_TESTING
void voice_cloud_adapter_test_reset(void)
{
    if (!s_lock) return;
    if (voice_cloud_adapter_stop() != ESP_OK) return;
    if (!take_lock()) return;
    memset(&s_rt, 0, sizeof(s_rt));
    xQueueReset(s_queue);
    (void)xSemaphoreTake(s_done, 0);
    xSemaphoreGive(s_lock);
}
#endif
