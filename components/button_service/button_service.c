/*
 * button_service.c — Button input service (Round 1.7)
 *
 * R1.7: Stop lifecycle short-circuit on join failure.
 *       C11 atomic_init after memset.
 *       BTN_HOOK_BEFORE_READY_SIGNAL, BTN_HOOK_URGENT_ATTEMPT_STARTED.
 *       Read-only test APIs: get_lifecycle, get_current_attempt, inject_completion.
 *
 * C11 atomic urgent handoff: release/acquire on completed_attempt/completed_result.
 * Join: 3-phase with double-check + join_commit_once (exactly-once).
 * EventGroup: clearOnExit=pdFALSE; bits are persistent facts.
 * IRQ: MAYBE_ENABLED always calls disable API.
 * Lock order: s_stop_mtx → s_lc_mtx → state_mtx.
 * Target: ESP32-S3 Xtensa (not ARM). stdatomic.h via GCC.
 */

#include "button_service.h"
#include "button_debounce.h"
#include "board_config.h"
#include "esp_log.h"
#include <stdatomic.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_heap_caps.h"

static const char *TAG = "btn_svc";

#define TASK_STACK_SIZE     4096
#define TASK_PRIORITY       15
#define TASK_CORE           1
#define STOP_TIMEOUT_MS     3000
#define STATE_MTX_TIMEOUT   pdMS_TO_TICKS(10)
#define INIT_SPIN_YIELD_MS  2
#define INIT_SPIN_TIMEOUT   5000
#define COMMIT_WAIT_MS      200

#define BTN_PIN_MASK        XIAOJING_MCP_INT_BTN_MASK_PORTA

/* EventGroup bits — persistent, never auto-cleared */
#define TASK_DONE_BIT       (1 << 0)
#define TASK_READY_OK_BIT   (1 << 1)
#define TASK_READY_FAIL_BIT (1 << 2)
#define TASK_ALL_BITS       (TASK_DONE_BIT | TASK_READY_OK_BIT | TASK_READY_FAIL_BIT)

typedef enum {
    BTN_IRQ_NOT_OWNED = 0,
    BTN_IRQ_MAYBE_ENABLED,
    BTN_IRQ_DISABLED,
} btn_irq_state_t;

/* Urgent delivery states */
typedef enum {
    URGENT_IDLE = 0,
    URGENT_PENDING,
    URGENT_IN_FLIGHT,
} urgent_state_t;

typedef enum {
    LC_UNINITIALIZED = 0,
    LC_INITIALIZED,
    LC_STARTING,
    LC_RUNNING,
    LC_STOPPING,
} lifecycle_t;

typedef struct {
    button_service_config_t cfg;
    lifecycle_t lifecycle;

    /* state_mtx protects: debounce, counters, urgent, io errors */
    SemaphoreHandle_t   state_mtx;
    button_debounce_ctx_t debounce;
    uint32_t total_events;
    uint32_t normal_sink_drops;
    esp_err_t last_normal_error;
    uint32_t urgent_delivered;
    /* Urgent state machine */
    urgent_state_t      urgent_state;
    button_sink_event_t urgent_event;          /* current event (PENDING/IN_FLIGHT) */
    /* R1.6: C11 atomic handoff — proper release/acquire, no data race.
     * Deliverer: store result (relaxed), then store completed_attempt (release).
     * Consumer: load completed_attempt (acquire), compare with current_attempt.
     * Xtensa aligned 32-bit loads/stores are naturally atomic. */
    _Atomic uint32_t    urgent_completed_attempt;
    _Atomic esp_err_t   urgent_completed_result;
    uint32_t            urgent_retry_count;
    uint32_t            urgent_coalesced;
    esp_err_t           urgent_last_error;
    uint32_t            urgent_last_attempt_ms;
    uint32_t            urgent_attempt_counter;  /* monotonic, under state_mtx */
    uint32_t            urgent_current_attempt;  /* snapshot at PENDING→IN_FLIGHT */
    uint32_t            i2c_read_errors;
    uint32_t            interrupt_wait_errors;

    /* R1.5: observability counters (under s_lc_mtx or atomic) */
    uint32_t            join_commit_count;
    uint32_t            irq_disable_count;
    uint32_t            urgent_publish_attempts;

    /* Task completion */
    EventGroupHandle_t  task_events;
    StaticEventGroup_t  task_events_storage;
    esp_err_t           start_error;

    /* Join tracking */
    bool task_done_observed;
    bool task_join_committed;
    btn_irq_state_t irq_state;
    esp_err_t interrupt_disable_error;
    bool urgent_flushed;
    esp_err_t urgent_flush_error;

    _Atomic bool stop_requested;
    TaskHandle_t task_handle;
} btn_runtime_t;

static btn_runtime_t s_rt;

/* Init state machine */
#define INIT_UNINIT       0
#define INIT_INITIALIZING 1
#define INIT_READY        2
#define INIT_FAILED       3
static _Atomic int s_init_state = INIT_UNINIT;
static _Atomic esp_err_t s_init_error = ESP_OK;

static SemaphoreHandle_t s_lc_mtx;
static StaticSemaphore_t s_lc_mtx_storage;
static SemaphoreHandle_t s_stop_mtx;
static StaticSemaphore_t s_stop_mtx_storage;

__attribute__((constructor))
static void auto_init(void) { button_service_global_init(); }

#ifdef XIAOJING_TESTING
static volatile bool s_hook_active[BTN_HOOK_COUNT];
static SemaphoreHandle_t s_hook_barriers[BTN_HOOK_COUNT];
static StaticSemaphore_t s_hook_storage[BTN_HOOK_COUNT];
static atomic_bool s_isolation_poisoned = false;
#endif

/* ================================================================
 * global_init
 * ================================================================ */

esp_err_t button_service_global_init(void)
{
    int expected = INIT_UNINIT;
    if (!atomic_compare_exchange_strong(&s_init_state, &expected, INIT_INITIALIZING)) {
        if (expected == INIT_READY) return ESP_OK;
        if (expected == INIT_FAILED) return atomic_load(&s_init_error);
        uint32_t t0 = xTaskGetTickCount() * portTICK_PERIOD_MS;
        while (atomic_load(&s_init_state) == INIT_INITIALIZING) {
            vTaskDelay(pdMS_TO_TICKS(INIT_SPIN_YIELD_MS));
            if ((xTaskGetTickCount() * portTICK_PERIOD_MS - t0) > INIT_SPIN_TIMEOUT)
                return ESP_ERR_TIMEOUT;
        }
        return (atomic_load(&s_init_state) == INIT_READY) ? ESP_OK : atomic_load(&s_init_error);
    }
    s_lc_mtx = xSemaphoreCreateMutexStatic(&s_lc_mtx_storage);
    s_stop_mtx = xSemaphoreCreateMutexStatic(&s_stop_mtx_storage);
    if (!s_lc_mtx || !s_stop_mtx) {
        atomic_store(&s_init_error, ESP_ERR_NO_MEM);
        atomic_store(&s_init_state, INIT_FAILED);
        return ESP_ERR_NO_MEM;
    }
#ifdef XIAOJING_TESTING
    for (int i = 0; i < BTN_HOOK_COUNT; i++) {
        s_hook_barriers[i] = xSemaphoreCreateBinaryStatic(&s_hook_storage[i]);
        if (!s_hook_barriers[i]) {
            atomic_store(&s_init_error, ESP_ERR_NO_MEM);
            atomic_store(&s_init_state, INIT_FAILED);
            return ESP_ERR_NO_MEM;
        }
    }
#endif
    atomic_store(&s_init_error, ESP_OK);
    atomic_store(&s_init_state, INIT_READY);
    return ESP_OK;
}

static inline bool is_ready(void) { return atomic_load(&s_init_state) == INIT_READY; }

#ifdef XIAOJING_TESTING
static void invoke_hook(button_hook_point_t hp)
{
    if (hp < BTN_HOOK_COUNT && s_hook_active[hp] && s_hook_barriers[hp])
        xSemaphoreTake(s_hook_barriers[hp], portMAX_DELAY);
}
#endif

/* ================================================================
 * Helpers
 * ================================================================ */

/* R1.6: Unified join commit — exactly-once, increments count only on false→true.
 * Must hold s_lc_mtx. Returns true if this call committed. */
static bool join_commit_once(btn_runtime_t *rt)
{
    if (rt->task_join_committed) return false;
    rt->task_done_observed = true;
    rt->task_join_committed = true;
    rt->task_handle = NULL;
    rt->join_commit_count++;
    return true;
}

static uint8_t compute_stable_mask(const button_debounce_ctx_t *ctx)
{
    uint8_t mask = 0;
    for (int i = 0; i < BUTTON_ID_COUNT; i++) {
        btn_state_t st = ctx->buttons[i].state;
        if (st == BTN_STATE_PRESSED || st == BTN_STATE_WAIT_RELEASE_DEBOUNCE)
            mask |= (1U << (BUTTON_RAW_BIT_BTN1 + i));
    }
    return mask;
}

static void build_sink_event(const button_event_t *ev, uint8_t mask,
                              button_sink_event_t *out)
{
    out->button_id = ev->button_id;
    out->event_type = ev->event_type;
    out->timestamp_ms = ev->timestamp_ms;
    out->sequence = ev->sequence;
    out->stable_mask = mask;
}

/* ================================================================
 * Normal delivery. NO lock during callback.
 * ================================================================ */

static void deliver_normal(btn_runtime_t *rt, const button_sink_event_t *ev)
{
    if (!rt->cfg.normal_sink.publish) {
        if (xSemaphoreTake(rt->state_mtx, STATE_MTX_TIMEOUT) == pdTRUE) {
            rt->normal_sink_drops++;
            rt->last_normal_error = ESP_ERR_INVALID_STATE;
            xSemaphoreGive(rt->state_mtx);
        }
        return;
    }
    esp_err_t err = rt->cfg.normal_sink.publish(
        ev, BTN_SINK_TIMEOUT_MS, rt->cfg.normal_sink.context);
    if (xSemaphoreTake(rt->state_mtx, STATE_MTX_TIMEOUT) == pdTRUE) {
        if (err == ESP_OK) rt->total_events++;
        else { rt->normal_sink_drops++; rt->last_normal_error = err; }
        xSemaphoreGive(rt->state_mtx);
    }
}

/* ================================================================
 * Urgent delivery. C11 atomic result handoff.
 *
 * State transitions:
 *   IDLE → IN_FLIGHT (start delivery, reset atomics, snapshot attempt)
 *   IN_FLIGHT → IDLE (success, atomic match)
 *   IN_FLIGHT → PENDING (fail, atomic match)
 *   IN_FLIGHT → stays IN_FLIGHT (no match yet, will be picked up later)
 *   PENDING → IN_FLIGHT (retry, new attempt)
 *   PENDING/PENDING → PENDING (coalesce)
 * ================================================================ */

/* Apply atomic result if state is IN_FLIGHT and completed_attempt matches.
 * Returns true if committed. Must hold state_mtx. */
static bool try_apply_handoff(btn_runtime_t *rt)
{
    if (rt->urgent_state != URGENT_IN_FLIGHT) return false;
    uint32_t completed = atomic_load_explicit(&rt->urgent_completed_attempt,
                                               memory_order_acquire);
    if (completed != rt->urgent_current_attempt) return false;
    esp_err_t result = atomic_load_explicit(&rt->urgent_completed_result,
                                             memory_order_relaxed);
    if (result == ESP_OK) {
        rt->urgent_state = URGENT_IDLE;
        rt->urgent_delivered++;
        rt->total_events++;
        rt->urgent_retry_count = 0;
        rt->urgent_last_error = ESP_OK;
    } else {
        rt->urgent_state = URGENT_PENDING;
        rt->urgent_retry_count++;
        rt->urgent_last_error = result;
    }
    /* Invalidate so stale reads don't re-trigger */
    atomic_store_explicit(&rt->urgent_completed_attempt, 0, memory_order_relaxed);
    return true;
}

static void deliver_urgent(btn_runtime_t *rt, const button_sink_event_t *ev)
{
    if (xSemaphoreTake(rt->state_mtx, STATE_MTX_TIMEOUT) != pdTRUE) return;

    if (rt->urgent_state != URGENT_IDLE) {
        rt->urgent_coalesced++;
        xSemaphoreGive(rt->state_mtx);
        return;
    }

    /* IDLE → IN_FLIGHT: snapshot attempt, reset atomic handoff */
    rt->urgent_state = URGENT_IN_FLIGHT;
    rt->urgent_event = *ev;
    rt->urgent_last_attempt_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    rt->urgent_attempt_counter++;
    rt->urgent_current_attempt = rt->urgent_attempt_counter;
    uint32_t saved_attempt = rt->urgent_current_attempt;
    rt->urgent_publish_attempts++;
    /* Reset atomic handoff — attempt_id=0 means no result yet */
    atomic_store_explicit(&rt->urgent_completed_attempt, 0, memory_order_relaxed);
    atomic_store_explicit(&rt->urgent_completed_result, ESP_OK, memory_order_relaxed);
    xSemaphoreGive(rt->state_mtx);

    /* Callback outside lock */
#ifdef XIAOJING_TESTING
    invoke_hook(BTN_HOOK_URGENT_ATTEMPT_STARTED);
#endif
    esp_err_t err = rt->cfg.urgent_sink.publish(
        ev, BTN_SINK_TIMEOUT_MS, rt->cfg.urgent_sink.context);

    /* R1.6: C11 atomic handoff — release semantics.
     * result stored first (relaxed), then attempt (release) — consumer
     * loads attempt (acquire) which synchronizes-with this release,
     * guaranteeing the result store is visible. */
    atomic_store_explicit(&rt->urgent_completed_result, err, memory_order_relaxed);
    atomic_store_explicit(&rt->urgent_completed_attempt, saved_attempt,
                          memory_order_release);

    /* Try to commit immediately */
    if (xSemaphoreTake(rt->state_mtx, STATE_MTX_TIMEOUT) == pdTRUE) {
        try_apply_handoff(rt);
        bool now_pending = (rt->urgent_state == URGENT_PENDING);
        xSemaphoreGive(rt->state_mtx);
#ifdef XIAOJING_TESTING
        /* R1.8: Fire after PENDING committed, outside lock */
        if (now_pending) invoke_hook(BTN_HOOK_URGENT_PENDING_COMMITTED);
#endif
    } else {
        /* First lock failed — try longer wait */
        if (xSemaphoreTake(rt->state_mtx, pdMS_TO_TICKS(COMMIT_WAIT_MS)) == pdTRUE) {
            try_apply_handoff(rt);
            xSemaphoreGive(rt->state_mtx);
        } else {
            ESP_LOGW(TAG, "deliver_urgent: state_mtx timeout, deferred");
        }
    }
}

/* Commit deferred result from atomic handoff. Called from loop and stop.
 * R1.6: uses C11 acquire/release for proper happens-before. */
static void try_commit_result(btn_runtime_t *rt)
{
    if (xSemaphoreTake(rt->state_mtx, STATE_MTX_TIMEOUT) != pdTRUE) return;
    try_apply_handoff(rt);
    xSemaphoreGive(rt->state_mtx);
}

static void retry_pending_urgent(btn_runtime_t *rt, uint32_t now_ms)
{
    button_sink_event_t ev_copy;
    bool should_retry = false;

    if (xSemaphoreTake(rt->state_mtx, STATE_MTX_TIMEOUT) != pdTRUE) return;

    if (rt->urgent_state == URGENT_PENDING) {
        uint32_t elapsed = now_ms - rt->urgent_last_attempt_ms;
        if (elapsed >= BTN_URGENT_MIN_RETRY_MS) {
            ev_copy = rt->urgent_event;
            rt->urgent_state = URGENT_IN_FLIGHT;
            rt->urgent_last_attempt_ms = now_ms;
            rt->urgent_attempt_counter++;
            rt->urgent_current_attempt = rt->urgent_attempt_counter;
            rt->urgent_publish_attempts++;
            /* Reset atomic handoff for new attempt */
            atomic_store_explicit(&rt->urgent_completed_attempt, 0, memory_order_relaxed);
            atomic_store_explicit(&rt->urgent_completed_result, ESP_OK, memory_order_relaxed);
            should_retry = true;
        }
    }
    xSemaphoreGive(rt->state_mtx);

    if (!should_retry) return;

    uint32_t saved_attempt = rt->urgent_current_attempt;

    esp_err_t err = rt->cfg.urgent_sink.publish(
        &ev_copy, BTN_SINK_TIMEOUT_MS, rt->cfg.urgent_sink.context);

    /* R1.6: C11 atomic handoff */
    atomic_store_explicit(&rt->urgent_completed_result, err, memory_order_relaxed);
    atomic_store_explicit(&rt->urgent_completed_attempt, saved_attempt,
                          memory_order_release);

    if (xSemaphoreTake(rt->state_mtx, STATE_MTX_TIMEOUT) == pdTRUE) {
        try_apply_handoff(rt);
        xSemaphoreGive(rt->state_mtx);
    } else {
        if (xSemaphoreTake(rt->state_mtx, pdMS_TO_TICKS(COMMIT_WAIT_MS)) == pdTRUE) {
            try_apply_handoff(rt);
            xSemaphoreGive(rt->state_mtx);
        }
    }
}

/* ================================================================
 * Task function
 * ================================================================ */

static void button_service_task(void *arg)
{
    btn_runtime_t *rt = (btn_runtime_t *)arg;
    const xiaojing_hal_t *hal = rt->cfg.hal;
    uint32_t poll_fallback = rt->cfg.poll_interval_ms;

    /* 1. Configure interrupts */
    esp_err_t err = hal->configure_button_interrupts();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "configure failed: 0x%x", err);
        rt->start_error = err;
        if (xSemaphoreTake(rt->state_mtx, STATE_MTX_TIMEOUT) == pdTRUE) {
            rt->irq_state = BTN_IRQ_NOT_OWNED;
            xSemaphoreGive(rt->state_mtx);
        }
        xEventGroupSetBits(rt->task_events, TASK_READY_FAIL_BIT | TASK_DONE_BIT);
        vTaskDeleteWithCaps(NULL);
        return;
    }

    if (xSemaphoreTake(rt->state_mtx, STATE_MTX_TIMEOUT) == pdTRUE) {
        rt->irq_state = BTN_IRQ_MAYBE_ENABLED;
        xSemaphoreGive(rt->state_mtx);
    }

    /* 2. Read GPIO baseline */
    button_gpio_snapshot_t btn_snap;
    err = hal->read_button_gpio(&btn_snap);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "initial GPIO failed: 0x%x", err);
        esp_err_t derr = hal->disable_button_interrupts();
        if (xSemaphoreTake(rt->state_mtx, STATE_MTX_TIMEOUT) == pdTRUE) {
            rt->irq_state = (derr == ESP_OK) ? BTN_IRQ_DISABLED : BTN_IRQ_MAYBE_ENABLED;
            rt->interrupt_disable_error = derr;
            rt->i2c_read_errors++;
            xSemaphoreGive(rt->state_mtx);
        }
        rt->start_error = err;
        xEventGroupSetBits(rt->task_events, TASK_READY_FAIL_BIT | TASK_DONE_BIT);
        vTaskDeleteWithCaps(NULL);
        return;
    }

    /* 3. Seed */
    if (xSemaphoreTake(rt->state_mtx, STATE_MTX_TIMEOUT) != pdTRUE) {
        ESP_LOGE(TAG, "seed lock timeout");
        esp_err_t derr = hal->disable_button_interrupts();
        if (xSemaphoreTake(rt->state_mtx, STATE_MTX_TIMEOUT) == pdTRUE) {
            rt->irq_state = (derr == ESP_OK) ? BTN_IRQ_DISABLED : BTN_IRQ_MAYBE_ENABLED;
            rt->interrupt_disable_error = derr;
            xSemaphoreGive(rt->state_mtx);
        }
        rt->start_error = ESP_ERR_TIMEOUT;
        xEventGroupSetBits(rt->task_events, TASK_READY_FAIL_BIT | TASK_DONE_BIT);
        vTaskDeleteWithCaps(NULL);
        return;
    }
    button_debounce_seed(&rt->debounce, btn_snap.gpio_a & BTN_PIN_MASK,
                          xTaskGetTickCount() * portTICK_PERIOD_MS);
    xSemaphoreGive(rt->state_mtx);
    uint8_t last_raw_mask = btn_snap.gpio_a & BTN_PIN_MASK;
    ESP_LOGI(TAG, "button raw baseline: GPA=0x%02x mask=0x%02x",
             (unsigned)btn_snap.gpio_a, (unsigned)last_raw_mask);

    /* 4. Signal READY */
#ifdef XIAOJING_TESTING
    invoke_hook(BTN_HOOK_BEFORE_READY_SIGNAL);
#endif
    rt->start_error = ESP_OK;
    xEventGroupSetBits(rt->task_events, TASK_READY_OK_BIT);

    /* 5. Main loop */
    while (!atomic_load(&rt->stop_requested)) {
#ifdef XIAOJING_TESTING
        invoke_hook(BTN_HOOK_PRE_READ_GPIO);
#endif

        try_commit_result(rt);

        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        button_deadline_t dl = {false, false, 0};
        if (xSemaphoreTake(rt->state_mtx, STATE_MTX_TIMEOUT) == pdTRUE) {
            dl = button_debounce_next_deadline(&rt->debounce, now);
            xSemaphoreGive(rt->state_mtx);
        }
        uint32_t wait_ms;
        if (dl.due_now) wait_ms = 1;
        else if (dl.active && dl.remaining_ms > 0 && dl.remaining_ms < poll_fallback)
            wait_ms = dl.remaining_ms;
        else wait_ms = poll_fallback;
        if (wait_ms < 1) wait_ms = 1;

        esp_err_t werr = hal->wait_button_interrupt(wait_ms);
        if (werr == ESP_ERR_INVALID_STATE) vTaskDelay(pdMS_TO_TICKS(wait_ms));
        if (werr != ESP_OK && werr != ESP_ERR_TIMEOUT) {
            if (xSemaphoreTake(rt->state_mtx, STATE_MTX_TIMEOUT) == pdTRUE) {
                rt->interrupt_wait_errors++;
                xSemaphoreGive(rt->state_mtx);
            }
        }

#ifdef XIAOJING_TESTING
        invoke_hook(BTN_HOOK_PRE_SINK);
#endif

        err = hal->read_button_gpio(&btn_snap);
        if (err != ESP_OK) {
            if (xSemaphoreTake(rt->state_mtx, STATE_MTX_TIMEOUT) == pdTRUE) {
                rt->i2c_read_errors++;
                xSemaphoreGive(rt->state_mtx);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        uint8_t raw_mask = btn_snap.gpio_a & BTN_PIN_MASK;
        if (raw_mask != last_raw_mask) {
            ESP_LOGI(TAG, "button raw change: GPA=0x%02x mask=0x%02x",
                     (unsigned)btn_snap.gpio_a, (unsigned)raw_mask);
            last_raw_mask = raw_mask;
        }

        now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        button_event_t events[BUTTON_ID_COUNT];
        int n;
        uint8_t stable_mask;
        if (xSemaphoreTake(rt->state_mtx, STATE_MTX_TIMEOUT) == pdTRUE) {
            n = button_debounce_process(&rt->debounce, raw_mask,
                                         now, events, BUTTON_ID_COUNT);
            stable_mask = compute_stable_mask(&rt->debounce);
            xSemaphoreGive(rt->state_mtx);
        } else { n = 0; stable_mask = 0; }

        bool urgent_attempted = false;
        for (int i = 0; i < n; i++) {
            button_sink_event_t sink_ev;
            build_sink_event(&events[i], stable_mask, &sink_ev);
            if (events[i].button_id == BUTTON_ID_3) {
                deliver_urgent(rt, &sink_ev);
                urgent_attempted = true;
            } else {
                deliver_normal(rt, &sink_ev);
            }
        }
        if (!urgent_attempted) {
            if (xSemaphoreTake(rt->state_mtx, STATE_MTX_TIMEOUT) == pdTRUE) {
                bool pending = (rt->urgent_state == URGENT_PENDING);
                xSemaphoreGive(rt->state_mtx);
                if (pending) retry_pending_urgent(rt, now);
            }
        }
    }

    /* 6. Disable interrupts — always call API when MAYBE_ENABLED */
    esp_err_t disable_err = ESP_OK;
    if (xSemaphoreTake(rt->state_mtx, STATE_MTX_TIMEOUT) == pdTRUE) {
        btn_irq_state_t irq = rt->irq_state;
        xSemaphoreGive(rt->state_mtx);
        if (irq == BTN_IRQ_MAYBE_ENABLED) {
            disable_err = hal->disable_button_interrupts();
        }
    }
    if (xSemaphoreTake(rt->state_mtx, STATE_MTX_TIMEOUT) == pdTRUE) {
        rt->interrupt_disable_error = disable_err;
        if (disable_err == ESP_OK) rt->irq_state = BTN_IRQ_DISABLED;
        /* On failure: stays MAYBE_ENABLED */
        xSemaphoreGive(rt->state_mtx);
    }

    /* 7. Final flush */
    esp_err_t flush_err = ESP_OK;
    try_commit_result(rt);  /* commit any deferred result */

    if (xSemaphoreTake(rt->state_mtx, STATE_MTX_TIMEOUT) == pdTRUE) {
        if (rt->urgent_state == URGENT_PENDING) {
            button_sink_event_t fev = rt->urgent_event;
            rt->urgent_state = URGENT_IN_FLIGHT;
            rt->urgent_attempt_counter++;
            rt->urgent_current_attempt = rt->urgent_attempt_counter;
            rt->urgent_publish_attempts++;
            uint32_t saved_attempt = rt->urgent_current_attempt;
            atomic_store_explicit(&rt->urgent_completed_attempt, 0, memory_order_relaxed);
            xSemaphoreGive(rt->state_mtx);

            flush_err = rt->cfg.urgent_sink.publish(
                &fev, BTN_SINK_TIMEOUT_MS, rt->cfg.urgent_sink.context);

            /* R1.6: C11 atomic handoff */
            atomic_store_explicit(&rt->urgent_completed_result, flush_err, memory_order_relaxed);
            atomic_store_explicit(&rt->urgent_completed_attempt, saved_attempt,
                                  memory_order_release);

            if (xSemaphoreTake(rt->state_mtx, STATE_MTX_TIMEOUT) == pdTRUE) {
                try_apply_handoff(rt);
                xSemaphoreGive(rt->state_mtx);
            }
        } else if (rt->urgent_state == URGENT_IN_FLIGHT) {
            /* Try to apply any completed handoff */
            if (!try_apply_handoff(rt)) {
                /* Callback truly still running (no completed result) */
                flush_err = ESP_ERR_TIMEOUT;
            }
            xSemaphoreGive(rt->state_mtx);
        } else {
            /* IDLE — nothing to flush */
            xSemaphoreGive(rt->state_mtx);
        }
    } else {
        flush_err = ESP_ERR_TIMEOUT;
    }

    rt->urgent_flush_error = flush_err;
    ESP_LOGI(TAG, "stopped (disable=0x%x flush=0x%x)", disable_err, flush_err);
    xEventGroupSetBits(rt->task_events, TASK_DONE_BIT);
    vTaskDeleteWithCaps(NULL);
}

/* ================================================================
 * init
 * ================================================================ */

esp_err_t button_service_init(const button_service_config_t *config)
{
    if (!config || !config->hal) return ESP_ERR_INVALID_ARG;
    if (!config->urgent_sink.publish) return ESP_ERR_INVALID_ARG;
    if (!is_ready()) return atomic_load(&s_init_error);

    if (xSemaphoreTake(s_lc_mtx, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (s_rt.lifecycle != LC_UNINITIALIZED) {
        xSemaphoreGive(s_lc_mtx);
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_rt, 0, sizeof(s_rt));
    memcpy(&s_rt.cfg, config, sizeof(*config));
    /* R1.7: Explicit C11 atomic_init after memset — memset does not
     * construct C11 atomic objects (C11 7.17.2.2). */
    atomic_init(&s_rt.stop_requested, false);
    atomic_init(&s_rt.urgent_completed_attempt, 0);
    atomic_init(&s_rt.urgent_completed_result, ESP_OK);
    if (!s_rt.cfg.debounce_ms) s_rt.cfg.debounce_ms = BTN_DEBOUNCE_DEFAULT_MS;
    if (!s_rt.cfg.poll_interval_ms) s_rt.cfg.poll_interval_ms = BTN_POLL_FALLBACK_MS;
    if (!s_rt.cfg.long_press_ms) s_rt.cfg.long_press_ms = BTN_LONG_PRESS_DEFAULT_MS;
    button_debounce_init(&s_rt.debounce, s_rt.cfg.debounce_ms);
    button_debounce_set_long_press(&s_rt.debounce,
                                   s_rt.cfg.long_press_mask,
                                   s_rt.cfg.long_press_ms);
    for (int i = 0; i < BUTTON_ID_COUNT; i++) {
        if (s_rt.cfg.long_press_ms_by_button[i] > 0) {
            button_debounce_set_long_press_for_button(
                &s_rt.debounce, (button_id_t)i,
                s_rt.cfg.long_press_ms_by_button[i]);
        }
    }

    s_rt.state_mtx = xSemaphoreCreateMutex();
    s_rt.task_events = xEventGroupCreateStatic(&s_rt.task_events_storage);
    if (!s_rt.state_mtx || !s_rt.task_events) {
        if (s_rt.state_mtx) vSemaphoreDelete(s_rt.state_mtx);
        s_rt.state_mtx = NULL;
        xSemaphoreGive(s_lc_mtx);
        return ESP_ERR_NO_MEM;
    }

    s_rt.lifecycle = LC_INITIALIZED;
    s_rt.irq_state = BTN_IRQ_NOT_OWNED;
    xSemaphoreGive(s_lc_mtx);
    return ESP_OK;
}

/* ================================================================
 * start
 * ================================================================ */

esp_err_t button_service_start(void)
{
    if (!is_ready()) return atomic_load(&s_init_error);
    if (xSemaphoreTake(s_lc_mtx, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (s_rt.lifecycle != LC_INITIALIZED) {
        xSemaphoreGive(s_lc_mtx);
        return ESP_ERR_INVALID_STATE;
    }

    s_rt.lifecycle = LC_STARTING;
    s_rt.start_error = ESP_OK;
    s_rt.task_done_observed = false;
    s_rt.task_join_committed = false;
    s_rt.irq_state = BTN_IRQ_NOT_OWNED;
    s_rt.interrupt_disable_error = ESP_OK;
    s_rt.urgent_flushed = false;
    s_rt.urgent_flush_error = ESP_OK;
    s_rt.urgent_attempt_counter = 0;
    s_rt.urgent_current_attempt = 0;
    s_rt.urgent_publish_attempts = 0;
    atomic_store(&s_rt.urgent_completed_attempt, 0);
    atomic_store(&s_rt.urgent_completed_result, ESP_OK);
    atomic_store(&s_rt.stop_requested, false);

    xEventGroupClearBits(s_rt.task_events, TASK_ALL_BITS);

    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
        button_service_task, "btn_svc", TASK_STACK_SIZE, &s_rt,
        TASK_PRIORITY, &s_rt.task_handle, TASK_CORE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (ok != pdPASS) {
        s_rt.task_handle = NULL;
        s_rt.lifecycle = LC_INITIALIZED;
        xSemaphoreGive(s_lc_mtx);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_lc_mtx);

    /* Wait for READY or DONE — clearOnExit=pdFALSE */
    EventBits_t bits = xEventGroupWaitBits(s_rt.task_events,
        TASK_READY_OK_BIT | TASK_READY_FAIL_BIT | TASK_DONE_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(STOP_TIMEOUT_MS));

    if (bits & TASK_READY_OK_BIT) {
        if (xSemaphoreTake(s_lc_mtx, portMAX_DELAY) == pdTRUE) {
            if (s_rt.lifecycle == LC_STARTING) {
                s_rt.lifecycle = LC_RUNNING;
                xSemaphoreGive(s_lc_mtx);
                return ESP_OK;
            }
            xSemaphoreGive(s_lc_mtx);
            return ESP_ERR_INVALID_STATE;
        }
        return ESP_ERR_TIMEOUT;
    }

    if ((bits & TASK_READY_FAIL_BIT) || (bits & TASK_DONE_BIT)) {
        /* Task failed or exited — wait for DONE if not yet observed */
        EventBits_t done_bits = bits;
        if (!(bits & TASK_DONE_BIT)) {
            done_bits = xEventGroupWaitBits(s_rt.task_events, TASK_DONE_BIT,
                                            pdFALSE, pdFALSE,
                                            pdMS_TO_TICKS(STOP_TIMEOUT_MS));
        }
        /* Only commit join if DONE actually observed for THIS task */
        if (done_bits & TASK_DONE_BIT) {
            if (xSemaphoreTake(s_lc_mtx, portMAX_DELAY) == pdTRUE) {
                join_commit_once(&s_rt);
                s_rt.lifecycle = LC_STOPPING;
                esp_err_t err = s_rt.start_error;
                xSemaphoreGive(s_lc_mtx);
                return err;
            }
            return ESP_ERR_TIMEOUT;
        }
        /* DONE not observed — timeout, don't fake join */
        if (xSemaphoreTake(s_lc_mtx, portMAX_DELAY) == pdTRUE) {
            s_rt.lifecycle = LC_STOPPING;
            xSemaphoreGive(s_lc_mtx);
        }
        return ESP_ERR_TIMEOUT;
    }

    /* Timeout */
    if (xSemaphoreTake(s_lc_mtx, portMAX_DELAY) == pdTRUE) {
        s_rt.lifecycle = LC_STOPPING;
        xSemaphoreGive(s_lc_mtx);
    }
    return ESP_ERR_TIMEOUT;
}

/* ================================================================
 * stop
 * ================================================================ */

/* R1.6: All join tracking under s_lc_mtx. Double-check in phase 3.
 * join_commit_once ensures exactly-once commit and count increment. */
static esp_err_t stop_join(btn_runtime_t *rt)
{
    /* Phase 1: check under lock */
    if (xSemaphoreTake(s_lc_mtx, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (rt->task_join_committed) {
        xSemaphoreGive(s_lc_mtx);
        return ESP_OK;
    }
    xSemaphoreGive(s_lc_mtx);

    /* Phase 2: wait outside lock */
    EventBits_t bits = xEventGroupWaitBits(rt->task_events, TASK_DONE_BIT,
                                            pdFALSE, pdFALSE,
                                            pdMS_TO_TICKS(STOP_TIMEOUT_MS));

    /* Phase 3: double-check + single commit under lock */
    if (xSemaphoreTake(s_lc_mtx, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;

    /* R1.6: Re-check — another caller may have committed between phase 1 and 3 */
    if (rt->task_join_committed) {
        xSemaphoreGive(s_lc_mtx);
        return ESP_OK;
    }

    if (bits & TASK_DONE_BIT) {
        join_commit_once(rt);  /* exactly-once: false→true only */
        xSemaphoreGive(s_lc_mtx);
        return ESP_OK;
    }
    xSemaphoreGive(s_lc_mtx);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t stop_disable(btn_runtime_t *rt)
{
    /* R1.8: Always increment counter to track that stop_disable phase was reached,
     * even if the task already disabled interrupts during its own cleanup. */
    if (xSemaphoreTake(s_lc_mtx, portMAX_DELAY) == pdTRUE) {
        rt->irq_disable_count++;
        xSemaphoreGive(s_lc_mtx);
    }

    /* NOT_OWNED or DISABLED: no API call needed */
    if (rt->irq_state == BTN_IRQ_DISABLED || rt->irq_state == BTN_IRQ_NOT_OWNED)
        return ESP_OK;

    /* MAYBE_ENABLED: MUST call the actual API — cannot infer from old error */
    esp_err_t derr = rt->cfg.hal->disable_button_interrupts();

    if (xSemaphoreTake(s_lc_mtx, portMAX_DELAY) == pdTRUE) {
        rt->interrupt_disable_error = derr;
        if (derr == ESP_OK) {
            rt->irq_state = BTN_IRQ_DISABLED;
        }
        /* On failure: stays MAYBE_ENABLED */
        xSemaphoreGive(s_lc_mtx);
    }
    return derr;
}

static esp_err_t stop_flush(btn_runtime_t *rt)
{
    try_commit_result(rt);

    if (xSemaphoreTake(rt->state_mtx, STATE_MTX_TIMEOUT) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    bool has_pending = (rt->urgent_state == URGENT_PENDING);
    /* R1.6: check if callback truly still running (no completed attempt) */
    bool in_flight_truly = (rt->urgent_state == URGENT_IN_FLIGHT &&
        atomic_load_explicit(&rt->urgent_completed_attempt, memory_order_acquire)
        != rt->urgent_current_attempt);
    button_sink_event_t ev_copy;
    if (has_pending) ev_copy = rt->urgent_event;
    xSemaphoreGive(rt->state_mtx);

    if (in_flight_truly) return ESP_ERR_TIMEOUT;

    if (!has_pending) {
        if (xSemaphoreTake(s_lc_mtx, portMAX_DELAY) == pdTRUE) {
            rt->urgent_flushed = true;
            xSemaphoreGive(s_lc_mtx);
        }
        return ESP_OK;
    }

    if (xSemaphoreTake(rt->state_mtx, STATE_MTX_TIMEOUT) == pdTRUE) {
        if (rt->urgent_state == URGENT_PENDING &&
            rt->urgent_event.sequence == ev_copy.sequence) {
            rt->urgent_state = URGENT_IN_FLIGHT;
            rt->urgent_attempt_counter++;
            rt->urgent_current_attempt = rt->urgent_attempt_counter;
            rt->urgent_publish_attempts++;
            atomic_store_explicit(&rt->urgent_completed_attempt, 0, memory_order_relaxed);
        } else {
            xSemaphoreGive(rt->state_mtx);
            return ESP_OK;
        }
        xSemaphoreGive(rt->state_mtx);
    }

    uint32_t saved_attempt = rt->urgent_current_attempt;

    esp_err_t ferr = rt->cfg.urgent_sink.publish(
        &ev_copy, BTN_SINK_TIMEOUT_MS, rt->cfg.urgent_sink.context);

    /* R1.6: C11 atomic handoff */
    atomic_store_explicit(&rt->urgent_completed_result, ferr, memory_order_relaxed);
    atomic_store_explicit(&rt->urgent_completed_attempt, saved_attempt,
                          memory_order_release);

    bool committed = false;
    if (xSemaphoreTake(rt->state_mtx, STATE_MTX_TIMEOUT) == pdTRUE) {
        if (try_apply_handoff(rt)) {
            committed = (rt->urgent_state == URGENT_IDLE);
        }
        xSemaphoreGive(rt->state_mtx);
    }

    if (xSemaphoreTake(s_lc_mtx, portMAX_DELAY) == pdTRUE) {
        rt->urgent_flush_error = ferr;
        rt->urgent_flushed = committed;
        xSemaphoreGive(s_lc_mtx);
    }
    return committed ? ESP_OK : ferr;
}

esp_err_t button_service_stop(void)
{
    if (!is_ready()) return atomic_load(&s_init_error);
    if (xSemaphoreTake(s_stop_mtx, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (xSemaphoreTake(s_lc_mtx, portMAX_DELAY) != pdTRUE) {
        xSemaphoreGive(s_stop_mtx);
        return ESP_ERR_TIMEOUT;
    }

    if (s_rt.lifecycle == LC_UNINITIALIZED) {
        xSemaphoreGive(s_lc_mtx); xSemaphoreGive(s_stop_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_rt.lifecycle == LC_INITIALIZED) {
        if (s_rt.state_mtx) { vSemaphoreDelete(s_rt.state_mtx); s_rt.state_mtx = NULL; }
        s_rt.lifecycle = LC_UNINITIALIZED;
        xSemaphoreGive(s_lc_mtx); xSemaphoreGive(s_stop_mtx);
        return ESP_OK;
    }

    if (s_rt.lifecycle == LC_STARTING || s_rt.lifecycle == LC_RUNNING || s_rt.lifecycle == LC_STOPPING) {
        s_rt.lifecycle = LC_STOPPING;
        atomic_store(&s_rt.stop_requested, true);
    }
    xSemaphoreGive(s_lc_mtx);

    /* R1.7: Ordered stop lifecycle with short-circuit on failure.
     * JOIN_PROVEN → IRQ_DISABLED → URGENT_FLUSHED → RESOURCE_CLEANUP
     * Each step only runs if the previous step succeeded.
     * Join failure skips disable/flush and preserves STOPPING for retry. */

    /* Phase 1: JOIN_PROVEN */
    esp_err_t jerr = stop_join(&s_rt);
    if (jerr != ESP_OK) {
        xSemaphoreGive(s_stop_mtx);
        return jerr;   /* STOPPING preserved, disable/flush NOT called, retry allowed */
    }

    /* R1.7: After join succeeds, another stop() may have already completed
     * cleanup. Check lifecycle under lock before proceeding. */
    if (xSemaphoreTake(s_lc_mtx, portMAX_DELAY) == pdTRUE) {
        if (s_rt.lifecycle == LC_UNINITIALIZED) {
            xSemaphoreGive(s_lc_mtx);
            xSemaphoreGive(s_stop_mtx);
            return ESP_ERR_INVALID_STATE;
        }
        xSemaphoreGive(s_lc_mtx);
    }

    /* Phase 2: IRQ_DISABLED — task confirmed exited, safe to access MCP */
    esp_err_t derr = stop_disable(&s_rt);

    /* Phase 3: URGENT_FLUSHED */
    stop_flush(&s_rt);

    /* Phase 4: RESOURCE_CLEANUP */
    if (xSemaphoreTake(s_lc_mtx, portMAX_DELAY) == pdTRUE) {
        if (derr != ESP_OK && s_rt.irq_state == BTN_IRQ_MAYBE_ENABLED) {
            /* Disable failed — stay STOPPING, allow retry */
            xSemaphoreGive(s_lc_mtx);
            xSemaphoreGive(s_stop_mtx);
            return derr;
        }
        if (s_rt.state_mtx) { vSemaphoreDelete(s_rt.state_mtx); s_rt.state_mtx = NULL; }
        s_rt.lifecycle = LC_UNINITIALIZED;
        xSemaphoreGive(s_lc_mtx);
    }
    xSemaphoreGive(s_stop_mtx);
    return ESP_OK;
}

/* ================================================================
 * get_snapshot
 * ================================================================ */

esp_err_t button_service_get_snapshot(button_service_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!is_ready()) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_lc_mtx, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;

    bool running = (s_rt.lifecycle != LC_UNINITIALIZED);
    SemaphoreHandle_t smtx = s_rt.state_mtx;

    if (!smtx || xSemaphoreTake(smtx, STATE_MTX_TIMEOUT) != pdTRUE) {
        xSemaphoreGive(s_lc_mtx);
        return ESP_ERR_TIMEOUT;
    }

    button_service_snapshot_t tmp = {0};
    tmp.stable_mask = compute_stable_mask(&s_rt.debounce);
    tmp.last_event_sequence = s_rt.debounce.global_sequence;
    tmp.total_events = s_rt.total_events;
    tmp.task_running = running;
    tmp.normal_sink_drops = s_rt.normal_sink_drops;
    tmp.last_normal_error = s_rt.last_normal_error;
    tmp.urgent_delivered = s_rt.urgent_delivered;
    tmp.urgent_coalesced = s_rt.urgent_coalesced;
    tmp.urgent_retry_count = s_rt.urgent_retry_count;
    tmp.urgent_pending = (s_rt.urgent_state != URGENT_IDLE);
    tmp.urgent_pending_sequence = tmp.urgent_pending ? s_rt.urgent_event.sequence : 0;
    tmp.urgent_last_error = s_rt.urgent_last_error;
    tmp.interrupt_disable_error = s_rt.interrupt_disable_error;
    tmp.urgent_flush_error = s_rt.urgent_flush_error;
    tmp.i2c_read_errors = s_rt.i2c_read_errors;
    tmp.interrupt_wait_errors = s_rt.interrupt_wait_errors;
    /* R1.5: observability */
    tmp.join_commit_count = s_rt.join_commit_count;
    tmp.irq_disable_count = s_rt.irq_disable_count;
    tmp.urgent_publish_attempts = s_rt.urgent_publish_attempts;

    xSemaphoreGive(smtx);
    xSemaphoreGive(s_lc_mtx);

    *out = tmp;
    return ESP_OK;
}

/* ================================================================
 * Test-only APIs
 * ================================================================ */

#ifdef XIAOJING_TESTING

esp_err_t button_service_test_abort_cleanup(void)
{
    if (!is_ready()) return ESP_ERR_INVALID_STATE;
    if (s_rt.lifecycle == LC_UNINITIALIZED) return ESP_OK;

    atomic_store(&s_rt.stop_requested, true);
    for (int i = 0; i < BTN_HOOK_COUNT; i++)
        if (s_hook_barriers[i]) xSemaphoreGive(s_hook_barriers[i]);

    EventBits_t bits = xEventGroupWaitBits(s_rt.task_events, TASK_DONE_BIT,
                                            pdFALSE, pdFALSE,
                                            pdMS_TO_TICKS(STOP_TIMEOUT_MS));
    if (!(bits & TASK_DONE_BIT)) {
        atomic_store(&s_isolation_poisoned, true);
        return ESP_ERR_TIMEOUT;
    }

    if (xSemaphoreTake(s_lc_mtx, portMAX_DELAY) == pdTRUE) {
        join_commit_once(&s_rt);
        if (s_rt.state_mtx) { vSemaphoreDelete(s_rt.state_mtx); s_rt.state_mtx = NULL; }
        s_rt.lifecycle = LC_UNINITIALIZED;
        xSemaphoreGive(s_lc_mtx);
    }
    return ESP_OK;
}

esp_err_t button_service_test_global_cleanup(void)
{
    esp_err_t first_err = ESP_OK;

    button_service_test_clear_hooks();

    if (s_rt.lifecycle != LC_UNINITIALIZED) {
        esp_err_t err = button_service_stop();
        if (err != ESP_OK) {
            err = button_service_test_abort_cleanup();
            if (err != ESP_OK && first_err == ESP_OK) first_err = err;
        }
    }

    if (first_err != ESP_OK) atomic_store(&s_isolation_poisoned, true);
    return first_err;
}

bool button_service_test_is_poisoned(void)
{
    return atomic_load(&s_isolation_poisoned);
}

void button_service_test_set_poisoned(bool poisoned)
{
    atomic_store(&s_isolation_poisoned, poisoned);
}

void button_service_test_set_hook(button_hook_point_t hp)
{
    if (hp < BTN_HOOK_COUNT) { s_hook_active[hp] = true; xSemaphoreTake(s_hook_barriers[hp], 0); }
}

void button_service_test_clear_hooks(void)
{
    for (int i = 0; i < BTN_HOOK_COUNT; i++) { s_hook_active[i] = false; xSemaphoreGive(s_hook_barriers[i]); }
}

SemaphoreHandle_t button_service_test_get_barrier(button_hook_point_t hp)
{
    return (hp < BTN_HOOK_COUNT) ? s_hook_barriers[hp] : NULL;
}

/* R1.7: Read-only lifecycle query — under s_lc_mtx only, no state_mtx dependency.
 * Safe to call even after stop when state_mtx is destroyed. */
esp_err_t button_service_test_get_lifecycle(int *out_lifecycle)
{
    if (!out_lifecycle) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_lc_mtx, pdMS_TO_TICKS(100)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    *out_lifecycle = (int)s_rt.lifecycle;
    xSemaphoreGive(s_lc_mtx);
    return ESP_OK;
}

/* R1.7: Get current urgent attempt ID — under state_mtx.
 * Returns 0 if state_mtx is NULL (after stop cleanup). */
uint32_t button_service_test_get_current_attempt(void)
{
    if (!s_rt.state_mtx) return 0;
    if (xSemaphoreTake(s_rt.state_mtx, pdMS_TO_TICKS(100)) != pdTRUE) return 0;
    uint32_t val = s_rt.urgent_current_attempt;
    xSemaphoreGive(s_rt.state_mtx);
    return val;
}

/* R1.7: Inject a completion result for a given attempt ID into the C11 atomic handoff.
 * Used to test stale-result rejection. */
void button_service_test_inject_completion(uint32_t attempt_id, esp_err_t result)
{
    atomic_store_explicit(&s_rt.urgent_completed_result, result, memory_order_relaxed);
    atomic_store_explicit(&s_rt.urgent_completed_attempt, attempt_id,
                          memory_order_release);
}

/* R1.7: Check if TASK_DONE_BIT is set — the task has actually exited.
 * Unlike snapshot.task_running (which depends on lifecycle), this checks
 * the EventGroup directly. Safe to call even when lifecycle is STOPPING. */
bool button_service_test_is_task_done(void)
{
    if (!s_rt.task_events) return false;
    EventBits_t bits = xEventGroupGetBits(s_rt.task_events);
    return (bits & TASK_DONE_BIT) != 0;
}

/* R1.8: Post-stop counter snapshot — uses s_lc_mtx, safe after state_mtx destroyed.
 * Counters are preserved across stop() so this works even in UNINITIALIZED state. */
esp_err_t button_service_test_get_counters(button_service_test_counters_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_lc_mtx, pdMS_TO_TICKS(100)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    out->join_commit_count = s_rt.join_commit_count;
    out->irq_disable_count = s_rt.irq_disable_count;
    out->urgent_publish_attempts = s_rt.urgent_publish_attempts;
    xSemaphoreGive(s_lc_mtx);
    return ESP_OK;
}

#endif
