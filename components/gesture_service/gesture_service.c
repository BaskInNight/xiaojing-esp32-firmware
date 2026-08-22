#include "gesture_service.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define GESTURE_TASK_STACK       3072U
#define GESTURE_TASK_PRIORITY    4U
#define GESTURE_START_TIMEOUT_MS 2000U
#define GESTURE_STOP_TIMEOUT_MS  3000U

typedef enum {
    GESTURE_LC_UNINITIALIZED = 0,
    GESTURE_LC_INITIALIZED,
    GESTURE_LC_RUNNING,
} gesture_lifecycle_t;

static const char *TAG = "gesture_svc";
static gesture_service_config_t s_cfg;
static gesture_lifecycle_t s_lifecycle;
static TaskHandle_t s_task;
static _Atomic bool s_stop_requested;
static SemaphoreHandle_t s_ready;
static SemaphoreHandle_t s_done;
static StaticSemaphore_t s_ready_storage;
static StaticSemaphore_t s_done_storage;
/* The task is started after ESP-SR and the radios.  Keep both its stack and
 * task control block owned by this component so late startup never needs a
 * contiguous internal heap block. */
static StaticTask_t s_task_tcb;
static EXT_RAM_BSS_ATTR StackType_t s_task_stack[GESTURE_TASK_STACK];

static const char *gesture_name(hal_gesture_t gesture)
{
    switch (gesture) {
    case HAL_GESTURE_LEFT: return "LEFT";
    case HAL_GESTURE_RIGHT: return "RIGHT";
    case HAL_GESTURE_UP: return "UP";
    case HAL_GESTURE_DOWN: return "DOWN";
    case HAL_GESTURE_FORWARD: return "FORWARD";
    case HAL_GESTURE_BACKWARD: return "BACKWARD";
    case HAL_GESTURE_CLOCKWISE: return "CLOCKWISE";
    case HAL_GESTURE_COUNTER_CW: return "COUNTER_CW";
    default: return "NONE";
    }
}

static void gesture_task(void *context)
{
    (void)context;
    TickType_t last_event = 0;
    bool have_event = false;
    xSemaphoreGive(s_ready);
    while (!atomic_load_explicit(&s_stop_requested, memory_order_acquire)) {
        hal_gesture_t gesture = HAL_GESTURE_NONE;
        esp_err_t err = s_cfg.hal->read_gesture(&gesture);
        if (err == ESP_OK && gesture != HAL_GESTURE_NONE) {
            TickType_t now = xTaskGetTickCount();
            TickType_t cooldown = pdMS_TO_TICKS(s_cfg.cooldown_ms);
            if (!have_event || (now - last_event) >= cooldown) {
                ESP_LOGI(TAG, "gesture=%s", gesture_name(gesture));
                esp_err_t sink_err = s_cfg.sink(gesture, s_cfg.sink_context);
                if (sink_err != ESP_OK)
                    ESP_LOGW(TAG, "gesture sink failed: 0x%x", sink_err);
                last_event = now;
                have_event = true;
            }
        } else if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "gesture read failed: 0x%x", err);
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(s_cfg.poll_interval_ms));
    }
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

esp_err_t gesture_service_global_init(void)
{
    /* These semaphores are created statically because gesture startup is
     * intentionally late, after BLE/Wi-Fi/AFE have consumed most internal
     * RAM.  Dynamic semaphore allocation there can fail even though the
     * gesture task stack itself is safely placed in PSRAM. */
    if (!s_ready)
        s_ready = xSemaphoreCreateBinaryStatic(&s_ready_storage);
    if (!s_done)
        s_done = xSemaphoreCreateBinaryStatic(&s_done_storage);
    return s_ready && s_done ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t gesture_service_init(const gesture_service_config_t *config)
{
    if (!config || !config->hal || !config->hal->read_gesture ||
        !config->sink || config->poll_interval_ms == 0U)
        return ESP_ERR_INVALID_ARG;
    if (s_lifecycle == GESTURE_LC_RUNNING) return ESP_ERR_INVALID_STATE;
    s_cfg = *config;
    if (s_cfg.cooldown_ms == 0U) s_cfg.cooldown_ms = 400U;
    s_lifecycle = GESTURE_LC_INITIALIZED;
    return ESP_OK;
}

esp_err_t gesture_service_start(void)
{
    if (s_lifecycle != GESTURE_LC_INITIALIZED || s_task)
        return ESP_ERR_INVALID_STATE;
    while (xSemaphoreTake(s_ready, 0) == pdTRUE) {}
    while (xSemaphoreTake(s_done, 0) == pdTRUE) {}
    atomic_store_explicit(&s_stop_requested, false, memory_order_release);
    s_task = xTaskCreateStaticPinnedToCore(
        gesture_task, "gesture", GESTURE_TASK_STACK, NULL,
        GESTURE_TASK_PRIORITY, s_task_stack, &s_task_tcb, 0);
    if (!s_task)
        return ESP_ERR_NO_MEM;
    if (xSemaphoreTake(s_ready, pdMS_TO_TICKS(GESTURE_START_TIMEOUT_MS)) != pdTRUE) {
        atomic_store_explicit(&s_stop_requested, true, memory_order_release);
        xTaskNotifyGive(s_task);
        return ESP_ERR_TIMEOUT;
    }
    s_lifecycle = GESTURE_LC_RUNNING;
    ESP_LOGI(TAG, "running (poll=%ums cooldown=%ums)",
             (unsigned)s_cfg.poll_interval_ms, (unsigned)s_cfg.cooldown_ms);
    return ESP_OK;
}

esp_err_t gesture_service_stop(void)
{
    if (s_lifecycle == GESTURE_LC_UNINITIALIZED) return ESP_OK;
    if (s_task) {
        atomic_store_explicit(&s_stop_requested, true, memory_order_release);
        xTaskNotifyGive(s_task);
        if (xSemaphoreTake(s_done, pdMS_TO_TICKS(GESTURE_STOP_TIMEOUT_MS)) != pdTRUE)
            return ESP_ERR_TIMEOUT;
        s_task = NULL;
    }
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_lifecycle = GESTURE_LC_UNINITIALIZED;
    return ESP_OK;
}
