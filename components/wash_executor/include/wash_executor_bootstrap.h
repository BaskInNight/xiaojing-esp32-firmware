/*
 * wash_executor_bootstrap.h — Executor lifecycle integration for bootstrap
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declare */
struct wash_executor;
typedef struct wash_executor wash_executor_t;
struct real_adapter_ctx;
typedef struct real_adapter_ctx real_adapter_ctx_t;

/**
 * Create executor + real adapter, start consumer and executor.
 * machine_event_queue: shared queue from app_bootstrap_get_event_queue().
 */
esp_err_t wash_exec_bootstrap_create(void *machine_event_queue);

/** Get the executor instance (NULL if not created). */
wash_executor_t *wash_exec_bootstrap_get_executor(void);

/** Get the adapter context (NULL if not created). */
real_adapter_ctx_t *wash_exec_bootstrap_get_adapter_ctx(void);

/** Quiesce executor (reject new programs, drain terminals). */
void wash_exec_bootstrap_quiesce(void);

/** Stop executor (bounded wait). */
esp_err_t wash_exec_bootstrap_stop(uint32_t timeout_ms);

/**
 * Full teardown. On failure, related resources remain available for retry.
 * The executor must have been stopped before this call.
 */
esp_err_t wash_exec_bootstrap_destroy(void);

/** Stop adapter consumer only; does not destroy its resources. */
esp_err_t wash_exec_bootstrap_stop_adapter_consumer(void);

#ifdef __cplusplus
}
#endif
