/*
 * wash_executor_bootstrap.c — Executor lifecycle integration
 *
 * Creates and manages the real adapter + executor instances.
 * Has access to private adapter headers for struct definitions.
 */

#include "wash_executor_adapter.h"
#include "wash_executor_real_adapter.h"
#include "real_adapter_internal.h"
#include "wash_executor.h"
#include "wash_executor_bootstrap.h"
#include <string.h>

/* Adapter and context are statically allocated here */
static wash_executor_adapter_t s_adapter;
static real_adapter_ctx_t s_adapter_ctx;
static wash_executor_t *s_executor = NULL;
static bool s_adapter_initialized = false;

esp_err_t wash_exec_bootstrap_create(void *machine_event_queue)
{
    if (!machine_event_queue) return ESP_ERR_INVALID_ARG;
    if (s_executor || s_adapter_initialized) return ESP_ERR_INVALID_STATE;

    esp_err_t err = real_adapter_init(&s_adapter, &s_adapter_ctx,
                                       machine_event_queue);
    if (err != ESP_OK) return err;
    s_adapter_initialized = true;

    err = real_adapter_start_consumer(&s_adapter_ctx);
    if (err != ESP_OK) {
        esp_err_t stop_err = real_adapter_stop_consumer(&s_adapter_ctx);
        if (stop_err != ESP_OK) return stop_err;
        esp_err_t destroy_err = real_adapter_destroy_consumer(&s_adapter_ctx);
        if (destroy_err != ESP_OK) return destroy_err;
        s_adapter_initialized = false;
        return err;
    }

    wash_executor_config_t cfg = {
        .step_timeout_default_ms = 60000,
        .reset_timeout_ms = 30000,
        .cancel_timeout_ms = 30000,
        .terminal_retry_ms = 1000,
    };
    s_executor = wash_executor_create(&cfg, &s_adapter);
    if (!s_executor) {
        esp_err_t stop_err = real_adapter_stop_consumer(&s_adapter_ctx);
        if (stop_err != ESP_OK) return stop_err;
        esp_err_t destroy_err = real_adapter_destroy_consumer(&s_adapter_ctx);
        if (destroy_err != ESP_OK) return destroy_err;
        s_adapter_initialized = false;
        return ESP_ERR_NO_MEM;
    }

    real_adapter_set_executor(&s_adapter_ctx, s_executor);

    err = wash_executor_start(s_executor);
    if (err != ESP_OK) {
        real_adapter_set_executor(&s_adapter_ctx, NULL);
        esp_err_t destroy_exec_err = wash_executor_destroy(s_executor);
        if (destroy_exec_err != ESP_OK) {
            real_adapter_set_executor(&s_adapter_ctx, s_executor);
            return destroy_exec_err;
        }
        s_executor = NULL;
        esp_err_t stop_err = real_adapter_stop_consumer(&s_adapter_ctx);
        if (stop_err != ESP_OK) return stop_err;
        esp_err_t destroy_err = real_adapter_destroy_consumer(&s_adapter_ctx);
        if (destroy_err != ESP_OK) return destroy_err;
        s_adapter_initialized = false;
        return err;
    }

    return ESP_OK;
}

wash_executor_t *wash_exec_bootstrap_get_executor(void)
{
    return s_executor;
}

real_adapter_ctx_t *wash_exec_bootstrap_get_adapter_ctx(void)
{
    return &s_adapter_ctx;
}

void wash_exec_bootstrap_quiesce(void)
{
    if (s_executor) {
        wash_executor_quiesce(s_executor);
    }
}

esp_err_t wash_exec_bootstrap_stop(uint32_t timeout_ms)
{
    if (!s_executor) return ESP_OK;
    return wash_executor_stop(s_executor, timeout_ms);
}

esp_err_t wash_exec_bootstrap_destroy(void)
{
    if (s_executor) {
        real_adapter_set_executor(&s_adapter_ctx, NULL);
        esp_err_t err = wash_executor_destroy(s_executor);
        if (err != ESP_OK) {
            real_adapter_set_executor(&s_adapter_ctx, s_executor);
            return err;
        }
        s_executor = NULL;
    }
    if (!s_adapter_initialized) return ESP_OK;

    esp_err_t err = real_adapter_stop_consumer(&s_adapter_ctx);
    if (err != ESP_OK) return err;
    err = real_adapter_destroy_consumer(&s_adapter_ctx);
    if (err != ESP_OK) return err;
    s_adapter_initialized = false;
    return ESP_OK;
}

esp_err_t wash_exec_bootstrap_stop_adapter_consumer(void)
{
    if (!s_adapter_initialized) return ESP_OK;
    return real_adapter_stop_consumer(&s_adapter_ctx);
}
