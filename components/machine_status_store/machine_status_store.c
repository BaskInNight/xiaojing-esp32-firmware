/*
 * machine_status_store.c — 一致快照存储实现
 * mutex 保护读写一致性，revision 内部维护、单调递增
 * UINT32_MAX 回绕到 1（跳过 0 = "从未更新"）
 */

#include <string.h>
#include "machine_status_store.h"
#include "machine_revision_next.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "status_store";

static machine_status_t s_status;
static uint32_t s_internal_revision = 0;
static SemaphoreHandle_t s_mutex = NULL;
static bool s_initialized = false;

esp_err_t machine_status_store_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    memset(&s_status, 0, sizeof(s_status));
    s_internal_revision = 0;
    s_status.state = MACHINE_STATE_BOOTING;
    s_status.position = DRUM_POS_UNKNOWN;
    s_status.target_position = DRUM_POS_UNKNOWN;
    s_status.fault.code = FAULT_NONE;
    s_initialized = true;

    ESP_LOGI(TAG, "Initialized");
    return ESP_OK;
}

#ifdef XIAOJING_TESTING
esp_err_t machine_status_store_reset(void)
{
    if (!s_initialized || !s_mutex) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(&s_status, 0, sizeof(s_status));
    s_internal_revision = 0;
    s_status.state = MACHINE_STATE_BOOTING;
    s_status.position = DRUM_POS_UNKNOWN;
    s_status.target_position = DRUM_POS_UNKNOWN;
    s_status.fault.code = FAULT_NONE;
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Reset for testing");
    return ESP_OK;
}
#endif

esp_err_t machine_status_store_update(const machine_status_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;
    if (!s_initialized || !s_mutex) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status = *status;
    s_internal_revision = machine_revision_next(s_internal_revision);
    s_status.revision = s_internal_revision;
    xSemaphoreGive(s_mutex);

    return ESP_OK;
}

esp_err_t machine_status_store_update_execution(
    const machine_execution_status_t *execution)
{
    if (!execution) return ESP_ERR_INVALID_ARG;
    if (!s_initialized || !s_mutex) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.state = execution->state;
    s_status.phase = execution->phase;
    s_status.program_id = execution->program_id;
    s_status.current_step = execution->current_step;
    s_status.total_steps = execution->total_steps;
    s_status.progress_percent = execution->progress_percent;
    s_status.elapsed_ms = execution->elapsed_ms;
    s_status.remaining_ms = execution->remaining_ms;
    s_status.target_position = execution->target_position;
    s_status.fault = execution->fault;
    s_internal_revision = machine_revision_next(s_internal_revision);
    s_status.revision = s_internal_revision;
    xSemaphoreGive(s_mutex);

    return ESP_OK;
}

esp_err_t machine_status_store_set_ble_connected(bool connected)
{
    if (!s_initialized || !s_mutex) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_status.ble_connected != connected) {
        s_status.ble_connected = connected;
        s_internal_revision = machine_revision_next(s_internal_revision);
        s_status.revision = s_internal_revision;
    }
    xSemaphoreGive(s_mutex);

    return ESP_OK;
}

esp_err_t machine_status_store_get(machine_status_t *out_status)
{
    if (!out_status) return ESP_ERR_INVALID_ARG;
    if (!s_initialized || !s_mutex) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out_status = s_status;
    xSemaphoreGive(s_mutex);

    return ESP_OK;
}

uint32_t machine_status_store_get_revision(void)
{
    if (!s_initialized || !s_mutex) return 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t rev = s_internal_revision;
    xSemaphoreGive(s_mutex);
    return rev;
}
