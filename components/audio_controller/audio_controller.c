/*
 * audio_controller.c — Single I2S owner with state machine
 *
 * One permanent capture task reads I2S and routes PCM based on state.
 * All state transitions are async via event queue.
 */

#include "audio_controller.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "audio_ctrl";

/* ---- Internal State ---- */

typedef struct {
    audio_controller_config_t config;
    audio_state_t state;
    uint32_t generation;        /* Incremented per wake detection, skip 0 */
    bool running;
    bool i2s_started;

    /* Synchronization */
    SemaphoreHandle_t mutex;    /* Protects state transitions */
    QueueHandle_t event_queue;  /* Async events */

    /* Task */
    TaskHandle_t capture_task;

    /* PCM buffer for I2S reads */
    int16_t *pcm_buffer;
    size_t pcm_buffer_samples;

    /* Cooldown */
    int64_t last_wake_us;
    uint32_t wake_cooldown_ms;

    /* Dialog timeout */
    int64_t dialog_start_us;
    uint32_t dialog_timeout_ms; /* Max dialog duration before auto-recovery */

    /* Diagnostics */
    audio_controller_diag_t diag;
} audio_controller_t;

static audio_controller_t s_ctrl = {0};

/* The audio controller is started after Wi-Fi/BLE/AFE on the ESP32-S3.
 * A dynamic 3072-word task stack then competes for the last internal-RAM
 * fragments and can fail with ESP_ERR_NO_MEM even though PSRAM is available.
 * Keep the TCB internal (FreeRTOS requirement) and place the large stack in
 * PSRAM, just like the gesture and ESP-SR model tasks. */
#define AUDIO_CONTROLLER_STACK_WORDS 4096U
static StaticTask_t s_capture_tcb;
static EXT_RAM_BSS_ATTR StackType_t s_capture_stack[AUDIO_CONTROLLER_STACK_WORDS];

/* ---- State Names ---- */

static const char *state_name(audio_state_t state)
{
    switch (state) {
    case AUDIO_STATE_STOPPED: return "STOPPED";
    case AUDIO_STATE_WAKE: return "WAKE";
    case AUDIO_STATE_TRANSITION_TO_DIALOG: return "TRANS_TO_DIALOG";
    case AUDIO_STATE_DIALOG: return "DIALOG";
    case AUDIO_STATE_TRANSITION_TO_WAKE: return "TRANS_TO_WAKE";
    case AUDIO_STATE_PTT: return "PTT";
    case AUDIO_STATE_ERROR_RECOVERY: return "ERROR_RECOVERY";
    default: return "UNKNOWN";
    }
}

static const char *event_name(audio_event_type_t type)
{
    switch (type) {
    case AUDIO_EVENT_NONE: return "NONE";
    case AUDIO_EVENT_WAKE_DETECTED: return "WAKE_DETECTED";
    case AUDIO_EVENT_DIALOG_START: return "DIALOG_START";
    case AUDIO_EVENT_DIALOG_TERMINAL: return "DIALOG_TERMINAL";
    case AUDIO_EVENT_PTT_START: return "PTT_START";
    case AUDIO_EVENT_PTT_RELEASE: return "PTT_RELEASE";
    case AUDIO_EVENT_ERROR: return "ERROR";
    case AUDIO_EVENT_STOP: return "STOP";
    default: return "UNKNOWN";
    }
}

/* ---- State Transitions ---- */

static esp_err_t transition_to(audio_state_t new_state)
{
    audio_state_t old_state = s_ctrl.state;

    /* Validate transition */
    bool valid = false;
    switch (old_state) {
    case AUDIO_STATE_STOPPED:
        valid = (new_state == AUDIO_STATE_WAKE);
        break;
    case AUDIO_STATE_WAKE:
        valid = (new_state == AUDIO_STATE_TRANSITION_TO_DIALOG ||
                 new_state == AUDIO_STATE_PTT ||
                 new_state == AUDIO_STATE_STOPPED ||
                 new_state == AUDIO_STATE_ERROR_RECOVERY);
        break;
    case AUDIO_STATE_TRANSITION_TO_DIALOG:
        valid = (new_state == AUDIO_STATE_DIALOG ||
                 new_state == AUDIO_STATE_ERROR_RECOVERY);
        break;
    case AUDIO_STATE_DIALOG:
        valid = (new_state == AUDIO_STATE_TRANSITION_TO_WAKE ||
                 new_state == AUDIO_STATE_ERROR_RECOVERY);
        break;
    case AUDIO_STATE_TRANSITION_TO_WAKE:
        valid = (new_state == AUDIO_STATE_WAKE ||
                 new_state == AUDIO_STATE_ERROR_RECOVERY);
        break;
    case AUDIO_STATE_PTT:
        valid = (new_state == AUDIO_STATE_TRANSITION_TO_WAKE ||
                 new_state == AUDIO_STATE_ERROR_RECOVERY);
        break;
    case AUDIO_STATE_ERROR_RECOVERY:
        valid = (new_state == AUDIO_STATE_WAKE ||
                 new_state == AUDIO_STATE_STOPPED);
        break;
    default:
        valid = false;
        break;
    }

    if (!valid) {
        ESP_LOGW(TAG, "Invalid transition: %s -> %s",
                 state_name(old_state), state_name(new_state));
        s_ctrl.diag.transition_errors++;
        s_ctrl.diag.last_transition_error = ESP_ERR_INVALID_STATE;
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "State: %s -> %s (gen=%"PRIu32")",
             state_name(old_state), state_name(new_state), s_ctrl.generation);
    s_ctrl.state = new_state;

    /* Track transition timestamps */
    int64_t now = esp_timer_get_time();
    switch (new_state) {
    case AUDIO_STATE_WAKE:
        s_ctrl.diag.wake_resumes++;
        s_ctrl.diag.last_wake_resume_us = now;
        break;
    case AUDIO_STATE_DIALOG:
        s_ctrl.diag.dialog_starts++;
        s_ctrl.diag.last_dialog_start_us = now;
        break;
    default:
        break;
    }

    return ESP_OK;
}

/* ---- Event Handlers ---- */

static void handle_wake_detected(audio_event_t *event)
{
    /* Check generation - reject stale events */
    if (event->generation != s_ctrl.generation) {
        s_ctrl.diag.stale_events_rejected++;
        ESP_LOGD(TAG, "Stale WAKE_DETECTED: event_gen=%"PRIu32" current_gen=%"PRIu32,
                 event->generation, s_ctrl.generation);
        return;
    }

    /* Check cooldown */
    int64_t now = esp_timer_get_time();
    if (s_ctrl.last_wake_us > 0) {
        int64_t elapsed_ms = (now - s_ctrl.last_wake_us) / 1000;
        if (elapsed_ms < (int64_t)s_ctrl.wake_cooldown_ms) {
            ESP_LOGD(TAG, "Wake cooldown active (%lld ms < %"PRIu32" ms)",
                     elapsed_ms, s_ctrl.wake_cooldown_ms);
            return;
        }
    }

    s_ctrl.diag.wake_events++;
    s_ctrl.last_wake_us = now;
    s_ctrl.diag.last_wake_detection_us = now;

    /* Increment generation for new session */
    s_ctrl.generation++;
    if (s_ctrl.generation == 0) s_ctrl.generation = 1; /* Skip 0 */

    ESP_LOGI(TAG, "WAKE_DETECTED: gen=%"PRIu32, s_ctrl.generation);

    /* Transition to TRANSITION_TO_DIALOG */
    if (s_ctrl.state == AUDIO_STATE_WAKE) {
        transition_to(AUDIO_STATE_TRANSITION_TO_DIALOG);

        /* In TRANSITION_TO_DIALOG state:
         * - Wake feed is paused (Edge doesn't get PCM)
         * - Waiting for DIALOG_START event to begin recording
         * - PCM is discarded during transition */

        /* Immediately send DIALOG_START to proceed */
        audio_event_t dialog_start = {
            .type = AUDIO_EVENT_DIALOG_START,
            .generation = s_ctrl.generation,
            .timestamp_us = esp_timer_get_time(),
        };
        audio_controller_send_event(&dialog_start);
    }
}

static void handle_dialog_start(audio_event_t *event)
{
    if (event->generation != s_ctrl.generation) {
        s_ctrl.diag.stale_events_rejected++;
        return;
    }

    if (s_ctrl.state == AUDIO_STATE_TRANSITION_TO_DIALOG) {
        transition_to(AUDIO_STATE_DIALOG);
        s_ctrl.dialog_start_us = esp_timer_get_time();

        /* Begin dialog recording */
        if (s_ctrl.config.dialog_begin) {
            s_ctrl.config.dialog_begin(s_ctrl.generation,
                                        s_ctrl.config.dialog_begin_context);
        }

        ESP_LOGI(TAG, "Dialog recording started (gen=%"PRIu32")", s_ctrl.generation);
    }
}

static void handle_dialog_terminal(audio_event_t *event)
{
    if (event->generation != s_ctrl.generation) {
        s_ctrl.diag.stale_events_rejected++;
        return;
    }

    s_ctrl.diag.dialog_terminals++;
    s_ctrl.diag.last_dialog_terminal_us = esp_timer_get_time();
    s_ctrl.dialog_start_us = 0; /* Clear dialog timer */

    /* End dialog recording */
    if (s_ctrl.config.dialog_end) {
        s_ctrl.config.dialog_end(s_ctrl.generation,
                                  s_ctrl.config.dialog_end_context);
    }

    ESP_LOGI(TAG, "DIALOG_TERMINAL: gen=%"PRIu32, event->generation);

    if (s_ctrl.state == AUDIO_STATE_DIALOG) {
        transition_to(AUDIO_STATE_TRANSITION_TO_WAKE);

        /* Recovery sequence:
         * 1. Clear recorder buffers (handled by voice_service)
         * 2. Reset Edge resampler/window (handled by wake_feed callback)
         * 3. Apply cooldown
         * 4. Route PCM to Edge
         * 5. state=WAKE */

        /* Apply cooldown before transitioning to WAKE */
        s_ctrl.last_wake_us = esp_timer_get_time();

        transition_to(AUDIO_STATE_WAKE);
    }
}

static void handle_error(audio_event_t *event)
{
    ESP_LOGW(TAG, "ERROR event: gen=%"PRIu32" err=0x%x",
             event->generation, (unsigned)event->error);

    s_ctrl.diag.transition_errors++;
    s_ctrl.diag.last_transition_error = event->error;

    /* Force recovery to WAKE from any state */
    if (s_ctrl.state != AUDIO_STATE_STOPPED) {
        transition_to(AUDIO_STATE_ERROR_RECOVERY);

        /* Recovery sequence */
        s_ctrl.last_wake_us = esp_timer_get_time();

        transition_to(AUDIO_STATE_WAKE);
    }
}

static void handle_stop(void)
{
    ESP_LOGI(TAG, "STOP event");
    transition_to(AUDIO_STATE_STOPPED);
    s_ctrl.running = false;
}

/* ---- Event Processing ---- */

static void process_event(audio_event_t *event)
{
    switch (event->type) {
    case AUDIO_EVENT_WAKE_DETECTED:
        handle_wake_detected(event);
        break;
    case AUDIO_EVENT_DIALOG_START:
        handle_dialog_start(event);
        break;
    case AUDIO_EVENT_DIALOG_TERMINAL:
        handle_dialog_terminal(event);
        break;
    case AUDIO_EVENT_PTT_START:
        /* TODO: Implement PTT */
        ESP_LOGW(TAG, "PTT_START not implemented");
        break;
    case AUDIO_EVENT_PTT_RELEASE:
        /* TODO: Implement PTT */
        ESP_LOGW(TAG, "PTT_RELEASE not implemented");
        break;
    case AUDIO_EVENT_ERROR:
        handle_error(event);
        break;
    case AUDIO_EVENT_STOP:
        handle_stop();
        break;
    default:
        ESP_LOGW(TAG, "Unknown event type: %d", (int)event->type);
        break;
    }
}

/* ---- Capture Task ---- */

static void capture_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Capture task started");

    /* Start I2S capture */
    if (s_ctrl.config.i2s_start) {
        esp_err_t err = s_ctrl.config.i2s_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S start failed: 0x%x", err);
            s_ctrl.running = false;
            vTaskDelete(NULL);
            return;
        }
        s_ctrl.i2s_started = true;
    }

    /* Transition from STOPPED to WAKE */
    transition_to(AUDIO_STATE_WAKE);

    int64_t last_read_time_us = esp_timer_get_time();

    while (s_ctrl.running) {
        /* Process pending events (non-blocking) */
        audio_event_t event;
        while (xQueueReceive(s_ctrl.event_queue, &event, 0) == pdTRUE) {
            process_event(&event);
            if (!s_ctrl.running) break;
        }

        if (!s_ctrl.running) break;

        /* Check dialog timeout */
        if (s_ctrl.state == AUDIO_STATE_DIALOG && s_ctrl.dialog_start_us > 0) {
            int64_t elapsed_ms = (esp_timer_get_time() - s_ctrl.dialog_start_us) / 1000;
            if (elapsed_ms > (int64_t)s_ctrl.dialog_timeout_ms) {
                ESP_LOGW(TAG, "Dialog timeout (%lld ms > %"PRIu32" ms), recovering to WAKE",
                         elapsed_ms, s_ctrl.dialog_timeout_ms);

                /* Send terminal event to recover */
                audio_event_t terminal = {
                    .type = AUDIO_EVENT_DIALOG_TERMINAL,
                    .generation = s_ctrl.generation,
                    .timestamp_us = esp_timer_get_time(),
                };
                audio_controller_send_event(&terminal);
            }

            /* Check VAD/timeout-based termination */
            if (s_ctrl.config.dialog_check_terminate &&
                s_ctrl.config.dialog_check_terminate(
                    s_ctrl.config.dialog_check_terminate_context)) {
                ESP_LOGI(TAG, "Dialog termination requested by VAD/timeout");

                /* Send terminal event */
                audio_event_t terminal = {
                    .type = AUDIO_EVENT_DIALOG_TERMINAL,
                    .generation = s_ctrl.generation,
                    .timestamp_us = esp_timer_get_time(),
                };
                audio_controller_send_event(&terminal);
            }
        }

        /* Read PCM from I2S */
        if (!s_ctrl.pcm_buffer || !s_ctrl.config.i2s_read) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        size_t bytes_to_read = s_ctrl.pcm_buffer_samples * sizeof(int16_t);
        size_t bytes_read = 0;
        s_ctrl.diag.i2s_read_calls++;

        int64_t read_start_us = esp_timer_get_time();
        esp_err_t read_err = s_ctrl.config.i2s_read(
            s_ctrl.pcm_buffer, bytes_to_read, &bytes_read, 100);

        if (read_err != ESP_OK) {
            s_ctrl.diag.i2s_errors++;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Track I2S read intervals */
        int64_t now_us = esp_timer_get_time();
        int64_t interval_us = now_us - last_read_time_us;
        last_read_time_us = now_us;

        if (interval_us < s_ctrl.diag.i2s_read_interval_min_us || s_ctrl.diag.i2s_read_interval_count == 0) {
            s_ctrl.diag.i2s_read_interval_min_us = interval_us;
        }
        if (interval_us > s_ctrl.diag.i2s_read_interval_max_us) {
            s_ctrl.diag.i2s_read_interval_max_us = interval_us;
        }
        s_ctrl.diag.i2s_read_interval_sum_us += (uint64_t)interval_us;
        s_ctrl.diag.i2s_read_interval_count++;

        s_ctrl.diag.i2s_read_ok++;
        s_ctrl.diag.i2s_bytes += bytes_read;

        size_t samples_read = bytes_read / sizeof(int16_t);
        if (samples_read == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        /* Route PCM based on state */
        switch (s_ctrl.state) {
        case AUDIO_STATE_WAKE:
            if (s_ctrl.config.wake_feed) {
                s_ctrl.config.wake_feed(
                    s_ctrl.pcm_buffer, samples_read, s_ctrl.config.wake_context);
                s_ctrl.diag.pcm_to_wake += samples_read;
            }
            break;

        case AUDIO_STATE_DIALOG:
        case AUDIO_STATE_PTT:
            if (s_ctrl.config.dialog_feed) {
                s_ctrl.config.dialog_feed(
                    s_ctrl.pcm_buffer, samples_read, s_ctrl.config.dialog_context);
                s_ctrl.diag.pcm_to_dialog += samples_read;
            }
            break;

        case AUDIO_STATE_TRANSITION_TO_DIALOG:
        case AUDIO_STATE_TRANSITION_TO_WAKE:
        case AUDIO_STATE_ERROR_RECOVERY:
            /* Discard PCM during transitions */
            break;

        default:
            break;
        }

        /* Delay to prevent watchdog timeout and yield to other tasks.
         * 10ms allows idle task to feed watchdog. */
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    /* Cleanup */
    if (s_ctrl.i2s_started && s_ctrl.config.i2s_stop) {
        s_ctrl.config.i2s_stop();
        s_ctrl.i2s_started = false;
    }

    s_ctrl.running = false;
    ESP_LOGI(TAG, "Capture task exiting");
    vTaskDelete(NULL);
}

/* ---- API Implementation ---- */

esp_err_t audio_controller_init(const audio_controller_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    if (s_ctrl.running) return ESP_ERR_INVALID_STATE;

    memset(&s_ctrl, 0, sizeof(s_ctrl));
    memcpy(&s_ctrl.config, config, sizeof(*config));

    /* Validate required callbacks */
    if (!config->i2s_read) {
        ESP_LOGE(TAG, "i2s_read callback required");
        return ESP_ERR_INVALID_ARG;
    }

    /* Create mutex */
    s_ctrl.mutex = xSemaphoreCreateMutex();
    if (!s_ctrl.mutex) return ESP_ERR_NO_MEM;

    /* Create event queue */
    uint32_t queue_depth = config->event_queue_depth > 0 ?
                           config->event_queue_depth : 8;
    s_ctrl.event_queue = xQueueCreate(queue_depth, sizeof(audio_event_t));
    if (!s_ctrl.event_queue) {
        vSemaphoreDelete(s_ctrl.mutex);
        return ESP_ERR_NO_MEM;
    }

    /* Allocate PCM buffer in PSRAM */
    uint32_t frame_samples = config->frame_samples > 0 ?
                             config->frame_samples : 512;
    s_ctrl.pcm_buffer_samples = frame_samples;
    s_ctrl.pcm_buffer = heap_caps_malloc(
        frame_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_ctrl.pcm_buffer) {
        ESP_LOGE(TAG, "Failed to allocate PCM buffer");
        vQueueDelete(s_ctrl.event_queue);
        vSemaphoreDelete(s_ctrl.mutex);
        return ESP_ERR_NO_MEM;
    }
    memset(s_ctrl.pcm_buffer, 0, frame_samples * sizeof(int16_t));

    /* Initialize state */
    s_ctrl.state = AUDIO_STATE_STOPPED;
    s_ctrl.generation = 1; /* Start at 1, skip 0 */
    s_ctrl.wake_cooldown_ms = config->wake_cooldown_ms > 0 ?
                              config->wake_cooldown_ms : 2000;
    s_ctrl.dialog_timeout_ms = 10000; /* 10 second dialog timeout */

    ESP_LOGI(TAG, "Initialized (frame_samples=%"PRIu32" queue_depth=%"PRIu32
             " cooldown=%"PRIu32" ms dialog_timeout=%"PRIu32" ms)",
             frame_samples, queue_depth, s_ctrl.wake_cooldown_ms,
             s_ctrl.dialog_timeout_ms);

    return ESP_OK;
}

esp_err_t audio_controller_start(void)
{
    if (!s_ctrl.mutex) return ESP_ERR_INVALID_STATE;
    if (s_ctrl.running) return ESP_ERR_INVALID_STATE;

    s_ctrl.running = true;

    /* Create capture task with a PSRAM stack.  The configured size is kept as
     * a safety check; this build's default is 3072 words. */
    uint32_t requested_stack = s_ctrl.config.stack_size > 0 ?
                               s_ctrl.config.stack_size : AUDIO_CONTROLLER_STACK_WORDS;
    if (requested_stack > AUDIO_CONTROLLER_STACK_WORDS) {
        s_ctrl.running = false;
        ESP_LOGE(TAG, "Configured stack (%" PRIu32 ") exceeds static stack", requested_stack);
        return ESP_ERR_INVALID_SIZE;
    }
    s_ctrl.capture_task = xTaskCreateStaticPinnedToCore(
        capture_task, "audio_capture", AUDIO_CONTROLLER_STACK_WORDS,
        NULL,
        s_ctrl.config.task_priority > 0 ? s_ctrl.config.task_priority : 5,
        s_capture_stack, &s_capture_tcb, s_ctrl.config.task_core);

    if (!s_ctrl.capture_task) {
        s_ctrl.running = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Started");
    return ESP_OK;
}

esp_err_t audio_controller_stop(void)
{
    if (!s_ctrl.running) return ESP_OK;

    /* Send stop event */
    audio_event_t event = {
        .type = AUDIO_EVENT_STOP,
        .timestamp_us = esp_timer_get_time(),
    };
    esp_err_t err = audio_controller_send_event(&event);

    /* Wait for task to exit */
    if (s_ctrl.capture_task) {
        /* Wait up to 2 seconds */
        for (int i = 0; i < 200 && s_ctrl.running; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        s_ctrl.capture_task = NULL;
    }

    return err;
}

esp_err_t audio_controller_send_event(const audio_event_t *event)
{
    if (!s_ctrl.event_queue || !event) return ESP_ERR_INVALID_STATE;

    if (xQueueSend(s_ctrl.event_queue, event, 0) != pdPASS) {
        s_ctrl.diag.queue_drops++;
        ESP_LOGW(TAG, "Event queue full, dropped %s", event_name(event->type));
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

void audio_controller_get_diag(audio_controller_diag_t *out)
{
    if (!out) return;

    /* Copy under mutex for consistency */
    if (s_ctrl.mutex && xSemaphoreTake(s_ctrl.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        memcpy(out, &s_ctrl.diag, sizeof(*out));
        out->state = s_ctrl.state;
        out->generation = s_ctrl.generation;
        xSemaphoreGive(s_ctrl.mutex);
    }
}

void audio_controller_log_diag(void)
{
    audio_controller_diag_t diag;
    audio_controller_get_diag(&diag);

    ESP_LOGI(TAG, "DIAG: state=%s gen=%"PRIu32,
             state_name(diag.state), diag.generation);
    ESP_LOGI(TAG, "DIAG_I2S: calls=%"PRIu32" ok=%"PRIu32" bytes=%"PRIu32
             " err=%"PRIu32,
             diag.i2s_read_calls, diag.i2s_read_ok, diag.i2s_bytes, diag.i2s_errors);
    ESP_LOGI(TAG, "DIAG_PCM: wake=%"PRIu32" dialog=%"PRIu32,
             diag.pcm_to_wake, diag.pcm_to_dialog);
    ESP_LOGI(TAG, "DIAG_EVENTS: wake=%"PRIu32" dialog_start=%"PRIu32
             " terminal=%"PRIu32" resume=%"PRIu32,
             diag.wake_events, diag.dialog_starts, diag.dialog_terminals,
             diag.wake_resumes);
    ESP_LOGI(TAG, "DIAG_REJECT: stale=%"PRIu32" queue_drop=%"PRIu32
             " trans_err=%"PRIu32,
             diag.stale_events_rejected, diag.queue_drops,
             diag.transition_errors);
}

audio_state_t audio_controller_get_state(void)
{
    return s_ctrl.state;
}

uint32_t audio_controller_get_generation(void)
{
    return s_ctrl.generation;
}

esp_err_t audio_controller_notify_wake(void)
{
    audio_event_t event = {
        .type = AUDIO_EVENT_WAKE_DETECTED,
        .generation = s_ctrl.generation,
        .timestamp_us = esp_timer_get_time(),
    };
    return audio_controller_send_event(&event);
}

esp_err_t audio_controller_notify_dialog_terminal(void)
{
    audio_event_t event = {
        .type = AUDIO_EVENT_DIALOG_TERMINAL,
        .generation = s_ctrl.generation,
        .timestamp_us = esp_timer_get_time(),
    };
    return audio_controller_send_event(&event);
}
