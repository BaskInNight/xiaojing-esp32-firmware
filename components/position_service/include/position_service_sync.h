/*
 * position_service_sync.h — Testable one-time sync state machine
 *
 * Four states: UNINITIALIZED → INITIALIZING → READY / FAILED
 * CAS selects one initializer; others wait (tick-based yield) for
 * READY (acquire barrier ensures pointers are visible) or FAILED.
 * Real error from initializer is propagated to all callers.
 *
 * Used by: position_service.c (global instance)
 *          test_runner.c (independent per-test instances)
 */
#pragma once

#include <stdatomic.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

enum ps_sync_phase {
    PS_SYNC_UNINITIALIZED = 0,
    PS_SYNC_INITIALIZING  = 1,
    PS_SYNC_READY         = 2,
    PS_SYNC_FAILED        = 3,
};

typedef struct {
    atomic_int state;       /* ps_sync_phase */
    atomic_int error;       /* real esp_err_t from initializer */
} ps_sync_ctx_t;

#define PS_SYNC_CTX_INIT  { ATOMIC_VAR_INIT(PS_SYNC_UNINITIALIZED), \
                            ATOMIC_VAR_INIT(ESP_OK) }

/* Default wait timeout for INITIALIZING spin */
#ifndef PS_SYNC_WAIT_TIMEOUT_MS
#define PS_SYNC_WAIT_TIMEOUT_MS  1000
#endif

/*
 * One-time init with CAS + release/acquire publish.
 *
 * Exactly one caller executes create_fn(user). Other callers
 * block (yielding) until READY or FAILED.
 *
 * create_fn must:
 *   - create all resources accessible via user
 *   - return ESP_OK on success, or a real error on failure
 *   - be safe to call exactly once
 *
 * Prerequisite: FreeRTOS scheduler must be running.
 *
 * Returns:
 *   ESP_OK           — initialization succeeded (this or prior caller)
 *   ESP_ERR_TIMEOUT  — INITIALIZING state persisted > timeout
 *   (other)          — real error from create_fn
 */
esp_err_t ps_sync_init(ps_sync_ctx_t *ctx,
                       esp_err_t (*create_fn)(void *user),
                       void *user);

/*
 * Wait for a prior ps_sync_init to complete.
 *
 * Returns:
 *   ESP_OK           — READY
 *   ESP_ERR_TIMEOUT  — INITIALIZING persisted > timeout_ms
 *   ESP_ERR_INVALID_STATE — UNINITIALIZED (init never called)
 *   (other)          — real error from failed initializer
 */
esp_err_t ps_sync_wait(ps_sync_ctx_t *ctx, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
