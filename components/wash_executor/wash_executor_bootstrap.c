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
#include "board_config.h"
#include "sdkconfig.h"
#include <string.h>

/* Adapter and context are statically allocated here */
static wash_executor_adapter_t s_adapter;
static real_adapter_ctx_t s_adapter_ctx;
static wash_executor_t *s_executor = NULL;
static bool s_adapter_initialized = false;
static bool s_using_sim_adapter = false;

/*
 * Interactive FAKE-mode adapter.
 *
 * It does not mutate the core directly. Every completion goes through the
 * normal executor ingress queue so token/generation/one-shot validation stays
 * identical to a real service terminal.
 */
typedef struct {
    wash_executor_t *executor;
} sim_adapter_ctx_t;

static sim_adapter_ctx_t s_sim_ctx;

static adapter_dispatch_result_t sim_publish(
    sim_adapter_ctx_t *ctx, const void *token_buf, uint32_t token_size)
{
    adapter_dispatch_result_t result = {
        .accepted = false,
        .error_code = (uint32_t)ESP_ERR_INVALID_STATE,
    };
    if (!ctx || !ctx->executor || !token_buf ||
        token_size < sizeof(terminal_token_t)) {
        return result;
    }
    esp_err_t err = wash_executor_publish_terminal(
        ctx->executor, token_buf, token_size, true, 0);
    result.accepted = (err == ESP_OK);
    result.error_code = (uint32_t)err;
    return result;
}

static adapter_dispatch_result_t sim_dispatch_step(
    void *ctx, const wash_step_t *step,
    const void *token_buf, uint32_t token_size)
{
    if (!step) {
        adapter_dispatch_result_t bad = {
            .accepted = false,
            .error_code = (uint32_t)ESP_ERR_INVALID_ARG,
        };
        return bad;
    }
    return sim_publish((sim_adapter_ctx_t *)ctx, token_buf, token_size);
}

static adapter_dispatch_result_t sim_dispatch_final_reset(
    void *ctx, const void *token_buf, uint32_t token_size)
{
    return sim_publish((sim_adapter_ctx_t *)ctx, token_buf, token_size);
}

static bool sim_cancel_step(
    void *ctx, const void *token_buf, uint32_t token_size)
{
    return sim_publish((sim_adapter_ctx_t *)ctx, token_buf, token_size).accepted;
}

static void sim_stop_all(void *ctx)
{
    (void)ctx;
}

static bool interactive_sim_enabled(void)
{
#if defined(CONFIG_XIAOJING_FAKE_RUNTIME_SIM)
    return board_config_get_output_mode() == XIAOJING_MODE_FAKE;
#else
    return false;
#endif
}

static void sim_adapter_init(void)
{
    memset(&s_sim_ctx, 0, sizeof(s_sim_ctx));
    memset(&s_adapter, 0, sizeof(s_adapter));
    s_adapter.ctx = &s_sim_ctx;
    s_adapter.dispatch_step = sim_dispatch_step;
    s_adapter.dispatch_final_reset = sim_dispatch_final_reset;
    s_adapter.cancel_step = sim_cancel_step;
    s_adapter.stop_all = sim_stop_all;
}

esp_err_t wash_exec_bootstrap_create(void *machine_event_queue)
{
    if (!machine_event_queue) return ESP_ERR_INVALID_ARG;
    if (s_executor || s_adapter_initialized) return ESP_ERR_INVALID_STATE;

    esp_err_t err = ESP_OK;
    s_using_sim_adapter = interactive_sim_enabled();
    if (s_using_sim_adapter) {
        sim_adapter_init();
        s_adapter_initialized = true;
    } else {
        err = real_adapter_init(&s_adapter, &s_adapter_ctx,
                                machine_event_queue);
        if (err != ESP_OK) return err;
        s_adapter_initialized = true;

        err = real_adapter_start_consumer(&s_adapter_ctx);
        if (err != ESP_OK) {
            esp_err_t stop_err = real_adapter_stop_consumer(&s_adapter_ctx);
            if (stop_err != ESP_OK) return stop_err;
            esp_err_t destroy_err =
                real_adapter_destroy_consumer(&s_adapter_ctx);
            if (destroy_err != ESP_OK) return destroy_err;
            s_adapter_initialized = false;
            return err;
        }
    }

    wash_executor_config_t cfg = {
        .step_timeout_default_ms = 60000,
        .reset_timeout_ms = 30000,
        .cancel_timeout_ms = 30000,
        .terminal_retry_ms = 1000,
    };
    s_executor = wash_executor_create(&cfg, &s_adapter);
    if (!s_executor) {
        if (!s_using_sim_adapter) {
            esp_err_t stop_err = real_adapter_stop_consumer(&s_adapter_ctx);
            if (stop_err != ESP_OK) return stop_err;
            esp_err_t destroy_err =
                real_adapter_destroy_consumer(&s_adapter_ctx);
            if (destroy_err != ESP_OK) return destroy_err;
        }
        s_adapter_initialized = false;
        s_using_sim_adapter = false;
        return ESP_ERR_NO_MEM;
    }

    if (s_using_sim_adapter) {
        s_sim_ctx.executor = s_executor;
    } else {
        real_adapter_set_executor(&s_adapter_ctx, s_executor);
    }

    err = wash_executor_start(s_executor);
    if (err != ESP_OK) {
        if (s_using_sim_adapter) s_sim_ctx.executor = NULL;
        else real_adapter_set_executor(&s_adapter_ctx, NULL);
        esp_err_t destroy_exec_err = wash_executor_destroy(s_executor);
        if (destroy_exec_err != ESP_OK) {
            if (s_using_sim_adapter) s_sim_ctx.executor = s_executor;
            else real_adapter_set_executor(&s_adapter_ctx, s_executor);
            return destroy_exec_err;
        }
        s_executor = NULL;
        if (!s_using_sim_adapter) {
            esp_err_t stop_err = real_adapter_stop_consumer(&s_adapter_ctx);
            if (stop_err != ESP_OK) return stop_err;
            esp_err_t destroy_err =
                real_adapter_destroy_consumer(&s_adapter_ctx);
            if (destroy_err != ESP_OK) return destroy_err;
        }
        s_adapter_initialized = false;
        s_using_sim_adapter = false;
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
    return s_using_sim_adapter ? NULL : &s_adapter_ctx;
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
        if (s_using_sim_adapter) s_sim_ctx.executor = NULL;
        else real_adapter_set_executor(&s_adapter_ctx, NULL);
        esp_err_t err = wash_executor_destroy(s_executor);
        if (err != ESP_OK) {
            if (s_using_sim_adapter) s_sim_ctx.executor = s_executor;
            else real_adapter_set_executor(&s_adapter_ctx, s_executor);
            return err;
        }
        s_executor = NULL;
    }
    if (!s_adapter_initialized) return ESP_OK;

    if (!s_using_sim_adapter) {
        esp_err_t err = real_adapter_stop_consumer(&s_adapter_ctx);
        if (err != ESP_OK) return err;
        err = real_adapter_destroy_consumer(&s_adapter_ctx);
        if (err != ESP_OK) return err;
    }
    memset(&s_sim_ctx, 0, sizeof(s_sim_ctx));
    memset(&s_adapter, 0, sizeof(s_adapter));
    s_adapter_initialized = false;
    s_using_sim_adapter = false;
    return ESP_OK;
}

esp_err_t wash_exec_bootstrap_stop_adapter_consumer(void)
{
    if (!s_adapter_initialized) return ESP_OK;
    if (s_using_sim_adapter) return ESP_OK;
    return real_adapter_stop_consumer(&s_adapter_ctx);
}
