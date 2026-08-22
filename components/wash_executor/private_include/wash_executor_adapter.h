#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "wash_contract.h"
#include "machine_types.h"
#include "wash_executor_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Adapter dispatch result ---- */

typedef struct {
    bool accepted;
    uint32_t error_code;
} adapter_dispatch_result_t;

/* ---- Adapter interface (vtable) ---- */

/* Forward typedef for use in function signatures */
typedef struct wash_executor_adapter wash_executor_adapter_t;

struct wash_executor_adapter {
    adapter_dispatch_result_t (*dispatch_step)(
        void *ctx,
        const wash_step_t *step,
        const void *token_buf,
        uint32_t token_size);

    adapter_dispatch_result_t (*dispatch_final_reset)(
        void *ctx,
        const void *token_buf,
        uint32_t token_size);

    bool (*cancel_step)(
        void *ctx,
        const void *token_buf,
        uint32_t token_size);

    void (*stop_all)(void *ctx);
    void *ctx;
};

/* ---- Fake adapter (per-instance, caller-owned) ---- */

#define FAKE_ADAPTER_CTX_SIZE 4096

/* Forward declare typed records */
typedef struct {
    wash_step_t step;
    terminal_token_t token;
} fake_dispatch_record_t;

/**
 * Initialize a fake adapter. Caller provides the adapter struct
 * and a ctx buffer of at least FAKE_ADAPTER_CTX_SIZE bytes.
 * No dynamic allocation. Caller owns both.
 */
void fake_adapter_init(wash_executor_adapter_t *adapter, void *ctx);

/* Count accessors */
uint32_t fake_adapter_dispatch_count(const void *ctx);
uint32_t fake_adapter_cancel_count(const void *ctx);
uint32_t fake_adapter_reset_count(const void *ctx);

/* Record accessors (typed) */
const fake_dispatch_record_t *fake_adapter_get_dispatch(const void *ctx, uint32_t index);
const terminal_token_t *fake_adapter_get_cancel_token(const void *ctx, uint32_t index);
const terminal_token_t *fake_adapter_get_reset_token(const void *ctx, uint32_t index);

/* Legacy accessors for backward compatibility */
uint16_t fake_adapter_dispatch_request_id(const void *ctx, uint32_t index);
wash_step_type_t fake_adapter_dispatch_step_type(const void *ctx, uint32_t index);

/* Configuration */
void fake_adapter_set_next_accept(void *ctx, bool accept);
void fake_adapter_set_next_error(void *ctx, uint32_t error_code);
void fake_adapter_set_cancel_result(void *ctx, bool success);
void fake_adapter_set_reset_result(void *ctx, bool success);
void fake_adapter_reset(void *ctx);

/**
 * Wait for at least min_count dispatches. Returns true if reached within timeout.
 */
bool fake_adapter_wait_dispatch(const void *ctx, uint32_t min_count, uint32_t timeout_ms);
bool fake_adapter_wait_cancel(const void *ctx, uint32_t min_count, uint32_t timeout_ms);
bool fake_adapter_wait_reset(const void *ctx, uint32_t min_count, uint32_t timeout_ms);

/* Real adapter: see wash_executor_real_adapter.h for public API */

#ifdef __cplusplus
}
#endif
