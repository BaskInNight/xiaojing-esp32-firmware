/*
 * wash_executor_fake_adapter.c — Per-instance fake adapter (Round 1.3)
 *
 * Typed records. Proper alignment. Thread-safe counters.
 * Caller-owned. No dynamic allocation. Stack-allocatable context.
 */

#include "wash_executor_adapter.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#define FAKE_ATOMIC_UINT atomic_uint
#define FAKE_ATOMIC_LOAD(p) atomic_load(p)
#define FAKE_ATOMIC_STORE(p, v) atomic_store((p), (v))
#else
/* MSVC: use Interlocked for count fields + SRWLOCK for record/config consistency */
#include <stdint.h>
#include <windows.h>
typedef volatile LONG fake_atomic_uint_t;
#define FAKE_ATOMIC_UINT fake_atomic_uint_t
#define FAKE_ATOMIC_LOAD(p) (*(p))
static inline void fake_atomic_store_release(fake_atomic_uint_t *p, LONG v) {
    InterlockedExchange(p, v);
}
static inline LONG fake_atomic_load_acquire(fake_atomic_uint_t *p) {
    LONG v = InterlockedCompareExchange(p, 0, 0);
    _ReadBarrier();
    return v;
}
#define FAKE_ATOMIC_STORE(p, v) fake_atomic_store_release((p), (v))
/* Redefine load to use acquire on Windows */
#undef FAKE_ATOMIC_LOAD
#define FAKE_ATOMIC_LOAD(p) fake_atomic_load_acquire(p)
#endif

#define FAKE_MAX_DISPATCHES 32
#define FAKE_MAX_CANCELS    8
#define FAKE_MAX_RESETS     4

/* ---- Typed cancel/reset records ---- */

typedef struct {
    terminal_token_t token;
} fake_cancel_record_t;

typedef struct {
    terminal_token_t token;
} fake_reset_record_t;

/* ---- Internal context layout ---- */

typedef struct {
    /* Dispatch records */
    fake_dispatch_record_t dispatches[FAKE_MAX_DISPATCHES];
    FAKE_ATOMIC_UINT dispatch_count;

    /* Cancel records */
    fake_cancel_record_t cancels[FAKE_MAX_CANCELS];
    FAKE_ATOMIC_UINT cancel_count;

    /* Reset records */
    fake_reset_record_t resets[FAKE_MAX_RESETS];
    FAKE_ATOMIC_UINT reset_count;

    /* Configurable results — protected by config_lock on MSVC */
    bool next_accept;
    uint32_t next_error_code;
    bool cancel_success;
    bool reset_success;

#ifndef ESP_PLATFORM
    SRWLOCK config_lock;
#endif

    /* Alignment padding */
    char _pad[8];
} fake_ctx_internal_t;

/* Verify it fits in the declared size */
_Static_assert(sizeof(fake_ctx_internal_t) <= FAKE_ADAPTER_CTX_SIZE,
               "fake adapter context too large");
/* Verify alignment — double has max_align_t-like requirements */
_Static_assert(_Alignof(fake_ctx_internal_t) <= _Alignof(double),
               "fake adapter alignment too large");

static inline fake_ctx_internal_t *to_internal(void *ctx)
{
    return (fake_ctx_internal_t *)ctx;
}

static inline const fake_ctx_internal_t *to_const_internal(const void *ctx)
{
    return (const fake_ctx_internal_t *)ctx;
}

/* ---- Adapter callbacks ---- */

static adapter_dispatch_result_t fake_dispatch(
    void *ctx, const wash_step_t *step,
    const void *token_buf, uint32_t token_size)
{
    fake_ctx_internal_t *f = to_internal(ctx);
    adapter_dispatch_result_t result;

#ifndef ESP_PLATFORM
    AcquireSRWLockExclusive(&f->config_lock);
#endif
    result.accepted = f->next_accept;
    result.error_code = f->next_accept ? 0 : f->next_error_code;

    if (FAKE_ATOMIC_LOAD(&f->dispatch_count) < FAKE_MAX_DISPATCHES) {
        uint32_t idx = FAKE_ATOMIC_LOAD(&f->dispatch_count);
        f->dispatches[idx].step = *step;
        if (token_buf && token_size >= sizeof(terminal_token_t)) {
            memcpy(&f->dispatches[idx].token, token_buf, sizeof(terminal_token_t));
        } else {
            memset(&f->dispatches[idx].token, 0, sizeof(terminal_token_t));
        }
        FAKE_ATOMIC_STORE(&f->dispatch_count, idx + 1);
    }
#ifndef ESP_PLATFORM
    ReleaseSRWLockExclusive(&f->config_lock);
#endif
    return result;
}

static adapter_dispatch_result_t fake_final_reset(
    void *ctx, const void *token_buf, uint32_t token_size)
{
    fake_ctx_internal_t *f = to_internal(ctx);
    adapter_dispatch_result_t result;

#ifndef ESP_PLATFORM
    AcquireSRWLockShared(&f->config_lock);
#endif
    result.accepted = f->reset_success;
    result.error_code = f->reset_success ? 0 : f->next_error_code;
#ifndef ESP_PLATFORM
    ReleaseSRWLockShared(&f->config_lock);
#endif

    if (FAKE_ATOMIC_LOAD(&f->reset_count) < FAKE_MAX_RESETS) {
        uint32_t idx = FAKE_ATOMIC_LOAD(&f->reset_count);
        if (token_buf && token_size >= sizeof(terminal_token_t)) {
            memcpy(&f->resets[idx].token, token_buf, sizeof(terminal_token_t));
        } else {
            memset(&f->resets[idx].token, 0, sizeof(terminal_token_t));
        }
        FAKE_ATOMIC_STORE(&f->reset_count, idx + 1);
    }
    return result;
}

static bool fake_cancel(void *ctx, const void *token_buf, uint32_t token_size)
{
    fake_ctx_internal_t *f = to_internal(ctx);

    if (FAKE_ATOMIC_LOAD(&f->cancel_count) < FAKE_MAX_CANCELS) {
        uint32_t idx = FAKE_ATOMIC_LOAD(&f->cancel_count);
        if (token_buf && token_size >= sizeof(terminal_token_t)) {
            memcpy(&f->cancels[idx].token, token_buf, sizeof(terminal_token_t));
        } else {
            memset(&f->cancels[idx].token, 0, sizeof(terminal_token_t));
        }
        FAKE_ATOMIC_STORE(&f->cancel_count, idx + 1);
    }

#ifndef ESP_PLATFORM
    AcquireSRWLockShared(&f->config_lock);
    bool rv = f->cancel_success;
    ReleaseSRWLockShared(&f->config_lock);
    return rv;
#else
    return f->cancel_success;
#endif
}

static void fake_stop_all(void *ctx) { (void)ctx; }

/* ---- Init ---- */

void fake_adapter_init(wash_executor_adapter_t *adapter, void *ctx)
{
    fake_ctx_internal_t *f = (fake_ctx_internal_t *)ctx;
    memset(f, 0, sizeof(*f));
    f->next_accept = true;
    f->cancel_success = true;
    f->reset_success = true;

#ifndef ESP_PLATFORM
    InitializeSRWLock(&f->config_lock);
#endif

    memset(adapter, 0, sizeof(*adapter));
    adapter->dispatch_step = fake_dispatch;
    adapter->dispatch_final_reset = fake_final_reset;
    adapter->cancel_step = fake_cancel;
    adapter->stop_all = fake_stop_all;
    adapter->ctx = ctx;
}

/* ---- Query API ---- */

uint32_t fake_adapter_dispatch_count(const void *ctx)
{
    return FAKE_ATOMIC_LOAD(&to_const_internal(ctx)->dispatch_count);
}

uint32_t fake_adapter_cancel_count(const void *ctx)
{
    return FAKE_ATOMIC_LOAD(&to_const_internal(ctx)->cancel_count);
}

uint32_t fake_adapter_reset_count(const void *ctx)
{
    return FAKE_ATOMIC_LOAD(&to_const_internal(ctx)->reset_count);
}

const fake_dispatch_record_t *fake_adapter_get_dispatch(const void *ctx, uint32_t index)
{
    const fake_ctx_internal_t *f = to_const_internal(ctx);
    if (index >= FAKE_ATOMIC_LOAD(&f->dispatch_count)) return NULL;
    return &f->dispatches[index];
}

const terminal_token_t *fake_adapter_get_cancel_token(const void *ctx, uint32_t index)
{
    const fake_ctx_internal_t *f = to_const_internal(ctx);
    if (index >= FAKE_ATOMIC_LOAD(&f->cancel_count)) return NULL;
    return &f->cancels[index].token;
}

const terminal_token_t *fake_adapter_get_reset_token(const void *ctx, uint32_t index)
{
    const fake_ctx_internal_t *f = to_const_internal(ctx);
    if (index >= FAKE_ATOMIC_LOAD(&f->reset_count)) return NULL;
    return &f->resets[index].token;
}

/* Legacy accessors for backward compatibility */
uint16_t fake_adapter_dispatch_request_id(const void *ctx, uint32_t index)
{
    const fake_dispatch_record_t *r = fake_adapter_get_dispatch(ctx, index);
    return r ? r->token.request_id : 0;
}

wash_step_type_t fake_adapter_dispatch_step_type(const void *ctx, uint32_t index)
{
    const fake_dispatch_record_t *r = fake_adapter_get_dispatch(ctx, index);
    return r ? r->step.type : STEP_FINISH;
}

/* ---- Config API ---- */

void fake_adapter_set_next_accept(void *ctx, bool accept)
{
    fake_ctx_internal_t *f = to_internal(ctx);
#ifndef ESP_PLATFORM
    AcquireSRWLockExclusive(&f->config_lock);
    f->next_accept = accept;
    ReleaseSRWLockExclusive(&f->config_lock);
#else
    f->next_accept = accept;
#endif
}

void fake_adapter_set_next_error(void *ctx, uint32_t error_code)
{
    fake_ctx_internal_t *f = to_internal(ctx);
#ifndef ESP_PLATFORM
    AcquireSRWLockExclusive(&f->config_lock);
    f->next_error_code = error_code;
    ReleaseSRWLockExclusive(&f->config_lock);
#else
    f->next_error_code = error_code;
#endif
}

void fake_adapter_set_cancel_result(void *ctx, bool success)
{
    fake_ctx_internal_t *f = to_internal(ctx);
#ifndef ESP_PLATFORM
    AcquireSRWLockExclusive(&f->config_lock);
    f->cancel_success = success;
    ReleaseSRWLockExclusive(&f->config_lock);
#else
    f->cancel_success = success;
#endif
}

void fake_adapter_set_reset_result(void *ctx, bool success)
{
    fake_ctx_internal_t *f = to_internal(ctx);
#ifndef ESP_PLATFORM
    AcquireSRWLockExclusive(&f->config_lock);
    f->reset_success = success;
    ReleaseSRWLockExclusive(&f->config_lock);
#else
    f->reset_success = success;
#endif
}

void fake_adapter_reset(void *ctx)
{
    fake_ctx_internal_t *f = to_internal(ctx);
    FAKE_ATOMIC_STORE(&f->dispatch_count, 0);
    FAKE_ATOMIC_STORE(&f->cancel_count, 0);
    FAKE_ATOMIC_STORE(&f->reset_count, 0);
#ifndef ESP_PLATFORM
    AcquireSRWLockExclusive(&f->config_lock);
    f->next_accept = true;
    f->next_error_code = 0;
    f->cancel_success = true;
    f->reset_success = true;
    ReleaseSRWLockExclusive(&f->config_lock);
#else
    f->next_accept = true;
    f->next_error_code = 0;
    f->cancel_success = true;
    f->reset_success = true;
#endif
}

/* ---- Wait API (polling with atomic counters) ---- */

bool fake_adapter_wait_dispatch(const void *ctx, uint32_t min_count, uint32_t timeout_ms)
{
    const fake_ctx_internal_t *f = to_const_internal(ctx);
    uint32_t elapsed = 0;
    while (FAKE_ATOMIC_LOAD(&f->dispatch_count) < min_count && elapsed < timeout_ms) {
#ifdef _WIN32
        Sleep(1);
#else
        vTaskDelay(pdMS_TO_TICKS(1));
#endif
        elapsed++;
    }
    return FAKE_ATOMIC_LOAD(&f->dispatch_count) >= min_count;
}

bool fake_adapter_wait_cancel(const void *ctx, uint32_t min_count, uint32_t timeout_ms)
{
    const fake_ctx_internal_t *f = to_const_internal(ctx);
    uint32_t elapsed = 0;
    while (FAKE_ATOMIC_LOAD(&f->cancel_count) < min_count && elapsed < timeout_ms) {
#ifdef _WIN32
        Sleep(1);
#else
        vTaskDelay(pdMS_TO_TICKS(1));
#endif
        elapsed++;
    }
    return FAKE_ATOMIC_LOAD(&f->cancel_count) >= min_count;
}

bool fake_adapter_wait_reset(const void *ctx, uint32_t min_count, uint32_t timeout_ms)
{
    const fake_ctx_internal_t *f = to_const_internal(ctx);
    uint32_t elapsed = 0;
    while (FAKE_ATOMIC_LOAD(&f->reset_count) < min_count && elapsed < timeout_ms) {
#ifdef _WIN32
        Sleep(1);
#else
        vTaskDelay(pdMS_TO_TICKS(1));
#endif
        elapsed++;
    }
    return FAKE_ATOMIC_LOAD(&f->reset_count) >= min_count;
}
