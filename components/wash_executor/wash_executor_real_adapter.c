/*
 * wash_executor_real_adapter.c — Real service adapter (Round 2)
 *
 * Bridges wash_executor's terminal_token_t protocol with
 * services' machine_event_sink_t protocol via a shared event queue.
 *
 * Services publish machine_event_t to a shared queue (set up by bootstrap).
 * The adapter consumer task reads from this queue and routes events to
 * the executor's publish_terminal() with correct token matching.
 *
 * Only calls service public APIs — never touches HAL directly.
 */

#include "wash_executor_adapter.h"
#include "wash_executor.h"
#include "wash_executor_real_adapter.h"
#include "real_adapter_internal.h"
#include "position_service.h"
#include "bl50_service.h"
#include "water_service.h"
#include "dry_service.h"
#include "drain_service.h"
#include "detergent_service.h"
#include "uv_service.h"
#include "safety_manager.h"
#include "machine_types.h"
#include "esp_log.h"
#include <string.h>
#include <stdatomic.h>

static const char *TAG = "real_adapt";

/* ---- BL50 action-specific default PWM ---- */

#define BL50_PWM_DEFAULT_PULSATOR  40
#define BL50_PWM_DEFAULT_DRUM      30
#define BL50_PWM_DEFAULT_SPIN      80
#define BL50_PWM_DEFAULT_HOME      40

#define ADAPTER_CONSUMER_STACK   4096
#define ADAPTER_CONSUMER_PRIO    3
#define ADAPTER_EVENT_QUEUE_DEPTH 16

static bool adapter_state_lock(real_adapter_ctx_t *ctx, TickType_t ticks)
{
    return ctx && ctx->state_mutex &&
           xSemaphoreTake(ctx->state_mutex, ticks) == pdTRUE;
}

static void adapter_state_unlock(real_adapter_ctx_t *ctx)
{
    xSemaphoreGive(ctx->state_mutex);
}

static bool token_identity_equal(const terminal_token_t *a,
                                 const terminal_token_t *b)
{
    return a->program_id == b->program_id &&
           a->step_id == b->step_id &&
           a->request_id == b->request_id &&
           a->generation == b->generation &&
           a->service_kind == b->service_kind &&
           a->terminal_kind == b->terminal_kind;
}

static void prepare_slot_locked(active_dispatch_t *slot,
                                const terminal_token_t *token,
                                wash_step_type_t step_type,
                                machine_request_id_t request_id)
{
    slot->token = *token;
    slot->step_type = step_type;
    slot->machine_request_id = request_id;
    slot->in_flight = true;
}

static void rollback_slot_locked(active_dispatch_t *slot,
                                 const terminal_token_t *token)
{
    if (slot->in_flight && token_identity_equal(&slot->token, token)) {
        memset(slot, 0, sizeof(*slot));
    }
}

/* ---- Result mapping ---- */

static adapter_dispatch_result_t map_esp_err(esp_err_t err)
{
    adapter_dispatch_result_t r = {
        .accepted = (err == ESP_OK),
        .error_code = (uint32_t)err,
    };
    return r;
}

/* ---- Expected machine_event_type for a step ---- */

static machine_event_type_t expected_event_for_step(wash_step_type_t step)
{
    switch (step) {
    case STEP_MOVE_POSITION: return MACHINE_EVENT_POSITION_DONE;
    case STEP_HOME_BL50:
    case STEP_PULSATOR_WASH:
    case STEP_DRUM_WASH:
    case STEP_SPIN:          return MACHINE_EVENT_BL50_DONE;
    case STEP_WATER_IN:      return MACHINE_EVENT_WATER_DONE;
    case STEP_DETERGENT:     return MACHINE_EVENT_DETERGENT_DONE;
    case STEP_DRAIN:         return MACHINE_EVENT_DRAIN_DONE;
    case STEP_DRY:           return MACHINE_EVENT_DRY_DONE;
    case STEP_UV:            return MACHINE_EVENT_UV_DONE;
    default:                 return (machine_event_type_t)-1;
    }
}

/* ---- Route a single machine_event_t to publish_terminal ---- */

static void route_machine_event(real_adapter_ctx_t *ra,
                                 const machine_event_t *ev)
{
    if (!ra || !ev || !adapter_state_lock(ra, pdMS_TO_TICKS(100))) return;

    wash_executor_t *exec = ra->exec;
    if (!exec) {
        adapter_state_unlock(ra);
        return;
    }

    /* Match against active dispatches.
     * Cancel/reset checked FIRST — when cancel is dispatched, both active and
     * cancel slots share the same request_id. The CANCELED terminal must match
     * the cancel slot (not active) so the executor gets TERMINAL_KIND_CANCEL_ACK. */
    active_dispatch_t *slot = NULL;
    terminal_token_t token;
    memset(&token, 0, sizeof(token));

    /* Check cancel first — same request_id as active during cancel flow */
    if (ra->cancel.in_flight &&
        ra->cancel.machine_request_id == ev->request_id) {
        slot = &ra->cancel;
        token = slot->token;
        token.terminal_kind = TERMINAL_KIND_CANCEL_ACK;
    }
    /* Check reset */
    else if (ra->reset.in_flight &&
             ra->reset.machine_request_id == ev->request_id) {
        slot = &ra->reset;
        token = slot->token;
        token.terminal_kind = TERMINAL_KIND_RESET_ACK;
    }
    /* Check active last */
    else if (ra->active.in_flight &&
             ra->active.machine_request_id == ev->request_id) {
        slot = &ra->active;
        token = slot->token;
    }

    if (!slot) {
        ra->stale_terminal_drops++;
        adapter_state_unlock(ra);
        ESP_LOGD(TAG, "stale terminal: type=%d req=%"PRIu32,
                 ev->type, ev->request_id);
        return;
    }

    /* Verify event type matches expected service */
    machine_event_type_t expected = expected_event_for_step(slot->step_type);
    if (expected != (machine_event_type_t)-1 && ev->type != expected) {
        ra->wrong_service_drops++;
        adapter_state_unlock(ra);
        ESP_LOGW(TAG, "wrong service: expected=%d got=%d req=%"PRIu32,
                 expected, ev->type, ev->request_id);
        return;
    }

    /* Map result */
    bool success = false;
    uint32_t error_code = 0;
    switch (ev->result) {
    case SERVICE_RESULT_OK:
        success = true;
        break;
    case SERVICE_RESULT_CANCELED:
        success = (token.terminal_kind == TERMINAL_KIND_CANCEL_ACK);
        if (!success) error_code = 200;
        break;
    case SERVICE_RESULT_SKIPPED:
        success = true;  /* UV skip → success for executor */
        break;
    case SERVICE_RESULT_TIMEOUT:
    case SERVICE_RESULT_REJECTED:
        error_code = 200;
        break;
    case SERVICE_RESULT_FAULT:
        error_code = ev->fault.code > 0 ? (uint32_t)ev->fault.code : 200;
        break;
    default:
        error_code = 200;
        break;
    }

    bool was_cancel = (slot == &ra->cancel);
    slot->in_flight = false;
    if (was_cancel && ra->active.in_flight &&
        ra->active.machine_request_id == ev->request_id) {
        ra->active.in_flight = false;
    }
    int routed_kind = (int)token.terminal_kind;
    adapter_state_unlock(ra);

    ESP_LOGD(TAG, "terminal routed: kind=%d req=%"PRIu32" ok=%d err=%"PRIu32,
             routed_kind, ev->request_id, success, error_code);

    wash_executor_publish_terminal(exec, &token, sizeof(token),
                                   success, error_code);
}

/* ---- Consumer task ---- */

static void adapter_consumer_task(void *arg)
{
    real_adapter_ctx_t *ra = (real_adapter_ctx_t *)arg;
    xSemaphoreGive(ra->consumer_ready_sem);

    machine_event_t ev;
    while (!atomic_load_explicit(&ra->consumer_stop, memory_order_acquire)) {
        if (xQueueReceive(ra->event_queue, &ev, pdMS_TO_TICKS(200)) == pdTRUE) {
            route_machine_event(ra, &ev);
            if (adapter_state_lock(ra, portMAX_DELAY)) {
                ra->total_events_consumed++;
                adapter_state_unlock(ra);
            }
        }
    }
    /* Drain remaining */
    while (xQueueReceive(ra->event_queue, &ev, 0) == pdTRUE) {
        route_machine_event(ra, &ev);
        if (adapter_state_lock(ra, portMAX_DELAY)) {
            ra->total_events_consumed++;
            adapter_state_unlock(ra);
        }
    }
    xSemaphoreGive(ra->consumer_done_sem);
    vTaskDelete(NULL);
}

/* ---- BL50 default PWM ---- */

static uint8_t get_bl50_pwm(real_adapter_ctx_t *ra, bl50_action_t action)
{
    switch (action) {
    case BL50_ACTION_PULSATOR: return ra->bl50_pwm_pulsator;
    case BL50_ACTION_DRUM:     return ra->bl50_pwm_drum;
    case BL50_ACTION_SPIN:     return ra->bl50_pwm_spin;
    case BL50_ACTION_HOME:     return ra->bl50_pwm_home;
    default:                   return BL50_PWM_DEFAULT_PULSATOR;
    }
}

/* ---- dispatch_step ---- */

static adapter_dispatch_result_t real_dispatch_step(
    void *ctx, const wash_step_t *step,
    const void *token_buf, uint32_t token_size)
{
    real_adapter_ctx_t *ra = (real_adapter_ctx_t *)ctx;
    if (!ra || !step || !token_buf || token_size != sizeof(terminal_token_t)) {
        adapter_dispatch_result_t r = { .accepted = false, .error_code = 1 };
        return r;
    }

    const terminal_token_t *token = (const terminal_token_t *)token_buf;
    machine_request_id_t mrid = (machine_request_id_t)token->request_id;
    if (mrid == MACHINE_REQUEST_ID_INVALID) mrid = 1;

    if (!adapter_state_lock(ra, pdMS_TO_TICKS(100))) {
        return map_esp_err(ESP_ERR_TIMEOUT);
    }
    if (ra->active.in_flight) {
        ra->dispatch_reject_count++;
        adapter_state_unlock(ra);
        return map_esp_err(ESP_ERR_INVALID_STATE);
    }
    prepare_slot_locked(&ra->active, token, step->type, mrid);
    adapter_state_unlock(ra);

    esp_err_t err = ESP_ERR_NOT_SUPPORTED;

    switch (step->type) {
    case STEP_MOVE_POSITION: {
        position_move_request_t req = {
            .request_id = mrid,
            .target = step->required_position,
            .direction_policy = POSITION_DIR_AUTO_SHORTEST,
            .pwm_percent = 0,
            .timeout_ms = step->timeout_ms,
        };
        err = position_service_move_async(&req);
        break;
    }
    case STEP_HOME_BL50: {
        bl50_request_t req = {
            .request_id = mrid,
            .action = BL50_ACTION_HOME,
            .intensity = step->intensity,
            .duration_ms = step->duration_ms,
            .target_pwm_percent = get_bl50_pwm(ra, BL50_ACTION_HOME),
            .direction_interval_ms = 0,
        };
        err = bl50_service_run_async(&req);
        break;
    }
    case STEP_PULSATOR_WASH: {
        bl50_request_t req = {
            .request_id = mrid,
            .action = BL50_ACTION_PULSATOR,
            .intensity = step->intensity,
            .duration_ms = step->duration_ms,
            .target_pwm_percent = get_bl50_pwm(ra, BL50_ACTION_PULSATOR),
            .direction_interval_ms = 0,
        };
        err = bl50_service_run_async(&req);
        break;
    }
    case STEP_DRUM_WASH: {
        bl50_request_t req = {
            .request_id = mrid,
            .action = BL50_ACTION_DRUM,
            .intensity = step->intensity,
            .duration_ms = step->duration_ms,
            .target_pwm_percent = get_bl50_pwm(ra, BL50_ACTION_DRUM),
            .direction_interval_ms = 0,
        };
        err = bl50_service_run_async(&req);
        break;
    }
    case STEP_SPIN: {
        bl50_request_t req = {
            .request_id = mrid,
            .action = BL50_ACTION_SPIN,
            .intensity = step->intensity,
            .duration_ms = step->duration_ms,
            .target_pwm_percent = get_bl50_pwm(ra, BL50_ACTION_SPIN),
            .direction_interval_ms = 0,
        };
        err = bl50_service_run_async(&req);
        break;
    }
    case STEP_WATER_IN: {
        water_in_request_t req = {
            .request_id = mrid,
            .target_volume_ml = 0,
            .source_batch_max_ms = 0,
            .complete_on_drum_full = true,
        };
        err = water_service_fill_async(&req);
        break;
    }
    case STEP_DETERGENT: {
        detergent_request_t req = {
            .request_id = mrid,
            .duration_ms = step->duration_ms,
        };
        err = detergent_service_run_async(&req);
        break;
    }
    case STEP_DRAIN: {
        drain_request_t req = {
            .request_id = mrid,
            .duration_ms = step->duration_ms,
        };
        err = drain_service_run_async(&req);
        break;
    }
    case STEP_DRY: {
        dry_request_t req = {
            .request_id = mrid,
            .duration_ms = step->duration_ms,
            /* 热风（加热）/冷却（仅风机）由 step flag 驱动，默认关闭加热。 */
            .heater_requested =
                (step->flags & WASH_STEP_FLAG_HEATER_ON) != 0,
        };
        err = dry_service_run_async(&req);
        break;
    }
    case STEP_UV: {
        uv_request_t req = {
            .request_id = mrid,
            .duration_ms = step->duration_ms,
        };
        err = uv_service_run_async(&req);
        break;
    }
    default:
        ESP_LOGE(TAG, "unknown step type: %d", step->type);
        break;
    }

    if (err == ESP_OK) {
        if (adapter_state_lock(ra, portMAX_DELAY)) {
            ra->dispatch_count++;
            adapter_state_unlock(ra);
        }
    } else if (adapter_state_lock(ra, portMAX_DELAY)) {
        rollback_slot_locked(&ra->active, token);
        ra->dispatch_reject_count++;
        adapter_state_unlock(ra);
    }

    return map_esp_err(err);
}

/* ---- cancel_step ---- */

static bool real_cancel_step(
    void *ctx, const void *token_buf, uint32_t token_size)
{
    real_adapter_ctx_t *ra = (real_adapter_ctx_t *)ctx;
    if (!ra || !token_buf || token_size != sizeof(terminal_token_t)) return false;

    const terminal_token_t *token = (const terminal_token_t *)token_buf;
    machine_request_id_t mrid = (machine_request_id_t)token->request_id;
    if (mrid == MACHINE_REQUEST_ID_INVALID) return false;

    if (!adapter_state_lock(ra, pdMS_TO_TICKS(100))) return false;
    if (ra->cancel.in_flight) {
        ra->cancel_reject_count++;
        adapter_state_unlock(ra);
        return false;
    }
    prepare_slot_locked(&ra->cancel, token,
                        (wash_step_type_t)token->service_kind, mrid);
    adapter_state_unlock(ra);

    esp_err_t err = ESP_ERR_NOT_SUPPORTED;

    switch ((wash_step_type_t)token->service_kind) {
    case STEP_MOVE_POSITION:
        err = position_service_cancel(mrid);
        break;
    case STEP_HOME_BL50:
    case STEP_PULSATOR_WASH:
    case STEP_DRUM_WASH:
    case STEP_SPIN:
        err = bl50_service_cancel(mrid);
        break;
    case STEP_WATER_IN:
        err = water_service_cancel(mrid);
        break;
    case STEP_DETERGENT:
        err = detergent_service_cancel(mrid);
        break;
    case STEP_DRAIN:
        err = drain_service_cancel(mrid);
        break;
    case STEP_DRY:
        err = dry_service_cancel(mrid);
        break;
    case STEP_UV:
        err = uv_service_cancel(mrid);
        break;
    default:
        break;
    }

    if (adapter_state_lock(ra, portMAX_DELAY)) {
        if (err == ESP_OK) {
            ra->cancel_count++;
        } else {
            rollback_slot_locked(&ra->cancel, token);
            ra->cancel_reject_count++;
        }
        adapter_state_unlock(ra);
    }
    return (err == ESP_OK);
}

/* ---- dispatch_final_reset ---- */

static adapter_dispatch_result_t real_dispatch_final_reset(
    void *ctx, const void *token_buf, uint32_t token_size)
{
    real_adapter_ctx_t *ra = (real_adapter_ctx_t *)ctx;
    if (!ra || !token_buf || token_size != sizeof(terminal_token_t)) {
        adapter_dispatch_result_t r = { .accepted = false, .error_code = 1 };
        return r;
    }

    const terminal_token_t *token = (const terminal_token_t *)token_buf;
    machine_request_id_t mrid = (machine_request_id_t)token->request_id;
    if (mrid == MACHINE_REQUEST_ID_INVALID) mrid = 1;

    if (!adapter_state_lock(ra, pdMS_TO_TICKS(100))) {
        return map_esp_err(ESP_ERR_TIMEOUT);
    }
    if (ra->reset.in_flight) {
        adapter_state_unlock(ra);
        return map_esp_err(ESP_ERR_INVALID_STATE);
    }
    prepare_slot_locked(&ra->reset, token, STEP_MOVE_POSITION, mrid);
    adapter_state_unlock(ra);

    position_move_request_t req = {
        .request_id = mrid,
        .target = DRUM_POS_45,
        .direction_policy = POSITION_DIR_AUTO_SHORTEST,
        .pwm_percent = 0,
        .timeout_ms = 0,
    };

    esp_err_t err = position_service_move_async(&req);

    if (err == ESP_OK) {
        if (adapter_state_lock(ra, portMAX_DELAY)) {
            ra->reset_count++;
            adapter_state_unlock(ra);
        }
    } else if (adapter_state_lock(ra, portMAX_DELAY)) {
        rollback_slot_locked(&ra->reset, token);
        adapter_state_unlock(ra);
    }

    return map_esp_err(err);
}

/* ---- stop_all ---- */

static void real_stop_all(void *ctx)
{
    real_adapter_ctx_t *ra = (real_adapter_ctx_t *)ctx;
    if (!ra) return;

    /* Emergency stop: heater-critical first */
    dry_service_emergency_stop();
    bl50_service_emergency_stop();
    position_service_emergency_stop();
    water_service_emergency_close();
    drain_service_emergency_stop();
    detergent_service_emergency_stop();
    uv_service_emergency_stop();

    if (adapter_state_lock(ra, portMAX_DELAY)) {
        ra->active.in_flight = false;
        ra->cancel.in_flight = false;
        ra->reset.in_flight = false;
        adapter_state_unlock(ra);
    }
}

/* ---- Public API ---- */

esp_err_t real_adapter_init(wash_executor_adapter_t *adapter,
                            real_adapter_ctx_t *ctx,
                            void *machine_event_queue)
{
    if (!adapter || !ctx || !machine_event_queue) return ESP_ERR_INVALID_ARG;
    memset(adapter, 0, sizeof(*adapter));
    memset(ctx, 0, sizeof(*ctx));

    ctx->state_mutex = xSemaphoreCreateMutex();
    if (!ctx->state_mutex) return ESP_ERR_NO_MEM;
    atomic_init(&ctx->consumer_stop, false);

    ctx->event_queue = (QueueHandle_t)machine_event_queue;
    ctx->bl50_pwm_pulsator = BL50_PWM_DEFAULT_PULSATOR;
    ctx->bl50_pwm_drum = BL50_PWM_DEFAULT_DRUM;
    ctx->bl50_pwm_spin = BL50_PWM_DEFAULT_SPIN;
    ctx->bl50_pwm_home = BL50_PWM_DEFAULT_HOME;

    adapter->ctx = ctx;
    adapter->dispatch_step = real_dispatch_step;
    adapter->dispatch_final_reset = real_dispatch_final_reset;
    adapter->cancel_step = real_cancel_step;
    adapter->stop_all = real_stop_all;

    return ESP_OK;
}

void real_adapter_set_executor(real_adapter_ctx_t *ctx, wash_executor_t *exec)
{
    if (adapter_state_lock(ctx, portMAX_DELAY)) {
        ctx->exec = exec;
        adapter_state_unlock(ctx);
    }
}

void real_adapter_set_bl50_defaults(real_adapter_ctx_t *ctx,
                                     uint8_t pulsator, uint8_t drum,
                                     uint8_t spin, uint8_t home)
{
    if (!ctx) return;
    if (pulsator > 0) ctx->bl50_pwm_pulsator = pulsator;
    if (drum > 0) ctx->bl50_pwm_drum = drum;
    if (spin > 0) ctx->bl50_pwm_spin = spin;
    if (home > 0) ctx->bl50_pwm_home = home;
}

esp_err_t real_adapter_start_consumer(real_adapter_ctx_t *ctx)
{
    if (!ctx || !ctx->event_queue) return ESP_ERR_INVALID_STATE;

    atomic_store_explicit(&ctx->consumer_stop, false, memory_order_release);
    ctx->consumer_ready_sem = xSemaphoreCreateBinary();
    if (!ctx->consumer_ready_sem) return ESP_ERR_NO_MEM;

    ctx->consumer_done_sem = xSemaphoreCreateBinary();
    if (!ctx->consumer_done_sem) {
        vSemaphoreDelete(ctx->consumer_ready_sem);
        ctx->consumer_ready_sem = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(adapter_consumer_task, "adapt_consume",
                                 ADAPTER_CONSUMER_STACK, ctx,
                                 ADAPTER_CONSUMER_PRIO,
                                 &ctx->consumer_task, 0) != pdPASS) {
        vSemaphoreDelete(ctx->consumer_done_sem);
        ctx->consumer_done_sem = NULL;
        vSemaphoreDelete(ctx->consumer_ready_sem);
        ctx->consumer_ready_sem = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(ctx->consumer_ready_sem, pdMS_TO_TICKS(2000)) != pdTRUE) {
        atomic_store_explicit(&ctx->consumer_stop, true, memory_order_release);
        if (xSemaphoreTake(ctx->consumer_done_sem,
                           pdMS_TO_TICKS(3000)) != pdTRUE) {
            /* The task may still signal both semaphores.  Preserve every
             * resource so real_adapter_stop_consumer() can join it later. */
            ESP_LOGE(TAG, "consumer start timeout; task still live");
            return ESP_ERR_TIMEOUT;
        }
        ctx->consumer_task = NULL;
        /* Semaphores remain owned until destroy_consumer(). */
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t real_adapter_stop_consumer(real_adapter_ctx_t *ctx)
{
    if (!ctx) return ESP_ERR_INVALID_STATE;
    if (!ctx->consumer_task) return ESP_OK;

    atomic_store_explicit(&ctx->consumer_stop, true, memory_order_release);
    if (ctx->consumer_done_sem) {
        if (xSemaphoreTake(ctx->consumer_done_sem, pdMS_TO_TICKS(3000)) != pdTRUE) {
            ESP_LOGE(TAG, "consumer stop timeout");
            return ESP_ERR_TIMEOUT;
        }
    }
    ctx->consumer_task = NULL;
    return ESP_OK;
}

esp_err_t real_adapter_destroy_consumer(real_adapter_ctx_t *ctx)
{
    if (!ctx) return ESP_ERR_INVALID_ARG;
    if (ctx->consumer_task) {
        ESP_LOGE(TAG, "destroy_consumer refused: task still live");
        return ESP_ERR_INVALID_STATE;
    }
    if (ctx->consumer_ready_sem) {
        vSemaphoreDelete(ctx->consumer_ready_sem);
        ctx->consumer_ready_sem = NULL;
    }
    if (ctx->consumer_done_sem) {
        vSemaphoreDelete(ctx->consumer_done_sem);
        ctx->consumer_done_sem = NULL;
    }
    if (ctx->state_mutex) {
        vSemaphoreDelete(ctx->state_mutex);
        ctx->state_mutex = NULL;
    }
    ctx->consumer_task = NULL;
    return ESP_OK;
}

/* ---- Diagnostics ---- */

uint32_t real_adapter_dispatch_count(const real_adapter_ctx_t *ctx)
{
    real_adapter_snapshot_t snap;
    return (real_adapter_get_snapshot(ctx, &snap) == ESP_OK)
           ? snap.dispatch_count : 0;
}

uint32_t real_adapter_cancel_count(const real_adapter_ctx_t *ctx)
{
    real_adapter_snapshot_t snap;
    return (real_adapter_get_snapshot(ctx, &snap) == ESP_OK)
           ? snap.cancel_count : 0;
}

uint32_t real_adapter_reset_count(const real_adapter_ctx_t *ctx)
{
    real_adapter_snapshot_t snap;
    return (real_adapter_get_snapshot(ctx, &snap) == ESP_OK)
           ? snap.reset_count : 0;
}

uint32_t real_adapter_stale_drops(const real_adapter_ctx_t *ctx)
{
    real_adapter_snapshot_t snap;
    return (real_adapter_get_snapshot(ctx, &snap) == ESP_OK)
           ? snap.stale_terminal_drops : 0;
}

uint32_t real_adapter_wrong_service_drops(const real_adapter_ctx_t *ctx)
{
    real_adapter_snapshot_t snap;
    return (real_adapter_get_snapshot(ctx, &snap) == ESP_OK)
           ? snap.wrong_service_drops : 0;
}

uint32_t real_adapter_events_consumed(const real_adapter_ctx_t *ctx)
{
    real_adapter_snapshot_t snap;
    return (real_adapter_get_snapshot(ctx, &snap) == ESP_OK)
           ? snap.events_consumed : 0;
}

bool real_adapter_active_in_flight(const real_adapter_ctx_t *ctx)
{
    real_adapter_snapshot_t snap;
    return (real_adapter_get_snapshot(ctx, &snap) == ESP_OK)
           && snap.active_in_flight;
}

bool real_adapter_cancel_in_flight(const real_adapter_ctx_t *ctx)
{
    real_adapter_snapshot_t snap;
    return (real_adapter_get_snapshot(ctx, &snap) == ESP_OK)
           && snap.cancel_in_flight;
}

bool real_adapter_reset_in_flight(const real_adapter_ctx_t *ctx)
{
    real_adapter_snapshot_t snap;
    return (real_adapter_get_snapshot(ctx, &snap) == ESP_OK)
           && snap.reset_in_flight;
}

esp_err_t real_adapter_get_snapshot(const real_adapter_ctx_t *ctx,
                                    real_adapter_snapshot_t *out)
{
    if (!ctx || !out) return ESP_ERR_INVALID_ARG;
    real_adapter_ctx_t *mutable_ctx = (real_adapter_ctx_t *)ctx;
    if (!adapter_state_lock(mutable_ctx, pdMS_TO_TICKS(100)))
        return ESP_ERR_TIMEOUT;

    real_adapter_snapshot_t tmp = {
        .active_in_flight = ctx->active.in_flight,
        .cancel_in_flight = ctx->cancel.in_flight,
        .reset_in_flight = ctx->reset.in_flight,
        .active_request_id = ctx->active.machine_request_id,
        .cancel_request_id = ctx->cancel.machine_request_id,
        .reset_request_id = ctx->reset.machine_request_id,
        .active_step_type = (uint32_t)ctx->active.step_type,
        .dispatch_count = ctx->dispatch_count,
        .dispatch_reject_count = ctx->dispatch_reject_count,
        .cancel_count = ctx->cancel_count,
        .cancel_reject_count = ctx->cancel_reject_count,
        .reset_count = ctx->reset_count,
        .stale_terminal_drops = ctx->stale_terminal_drops,
        .wrong_service_drops = ctx->wrong_service_drops,
        .events_consumed = ctx->total_events_consumed,
    };
    adapter_state_unlock(mutable_ctx);
    *out = tmp;
    return ESP_OK;
}
