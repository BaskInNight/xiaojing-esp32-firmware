/*
 * position_service.c — 姿态定位服务 (Round 5)
 *
 * R5 fixes:
 * - Deadlock fix: stop() uses stop_mutex, releases lc_mtx before waiting
 * - TOCTOU fix: all public APIs hold lc_mtx during resource use
 * - global_init with atomic CAS + release/acquire publish
 * - Test hooks: #ifdef POSITION_SERVICE_TEST_HOOKS barrier injection
 * - Lock ordering: stop_mutex → lc_mtx → state_mutex (never reversed)
 */

#include "position_service.h"
#include "position_service_sync.h"
#include "safety_manager.h"
#include <stdatomic.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *TAG = "pos_svc";

/* ================================================================
 * Hall mapping
 * ================================================================ */

#define HALL_BIT_0    0
#define HALL_BIT_45   1
#define HALL_BIT_90   2
#define HALL_BIT_180  3
#define HALL_BIT_270  4
#define HALL_BIT_BUCKET_ALIGN 5

#define HALL_MASK_0    (1U << HALL_BIT_0)
#define HALL_MASK_45   (1U << HALL_BIT_45)
#define HALL_MASK_90   (1U << HALL_BIT_90)
#define HALL_MASK_180  (1U << HALL_BIT_180)
#define HALL_MASK_270  (1U << HALL_BIT_270)
#define HALL_MASK_BUCKET_ALIGN (1U << HALL_BIT_BUCKET_ALIGN)

#define HALL_ALL_MASK  (HALL_MASK_0 | HALL_MASK_45 | HALL_MASK_90 | \
                        HALL_MASK_180 | HALL_MASK_270)

typedef struct {
    uint8_t         mask;
    drum_position_t pos;
    int             angle_deg;
} hall_entry_t;

static const hall_entry_t s_hall_table[DRUM_POSITION_COUNT] = {
    { HALL_MASK_0,   DRUM_POS_0,     0 },
    { HALL_MASK_45,  DRUM_POS_45,   45 },
    { HALL_MASK_90,  DRUM_POS_90,   90 },
    { HALL_MASK_180, DRUM_POS_180, 180 },
    { HALL_MASK_270, DRUM_POS_270, 270 },
};

/* ---- Defaults ---- */

#define DEFAULT_DEBOUNCE_MS      30
#define DEFAULT_MOVE_TIMEOUT_MS  30000
#define DEFAULT_BRAKE_MS         500
#define DEFAULT_MOVE_PWM         30
#define POLL_INTERVAL_MS         10
#define TASK_STACK_SIZE          4096
#define TASK_PRIORITY            16
#define CMD_QUEUE_DEPTH          8

#define NOTIFY_EMERGENCY_BIT     (1U << 0)
#define NOTIFY_STOP_BIT          (1U << 1)

/* ================================================================
 * Lifecycle states
 * ================================================================ */

typedef enum {
    LC_UNINITIALIZED = 0,
    LC_INITIALIZED,
    LC_STARTING,
    LC_RUNNING,
    LC_STOPPING,
} lifecycle_t;

/* ================================================================
 * Internal types
 * ================================================================ */

typedef enum {
    CMD_MOVE = 0,
    CMD_CANCEL,
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    position_move_request_t move;
    machine_request_id_t cancel_id;
} pos_cmd_t;

/* ================================================================
 * Hall debounce
 * ================================================================ */

typedef struct {
    uint8_t  raw_active;
    uint8_t  stable_active;
    uint8_t  prev_raw;
    int64_t  raw_changed_at_ms;
    bool     initialized;
} hall_debounce_t;

static void debounce_init(hall_debounce_t *d)
{
    memset(d, 0, sizeof(*d));
    d->prev_raw = 0xFF;
}

static void debounce_update(hall_debounce_t *d, uint8_t raw_active,
                            int64_t now_ms, uint32_t debounce_ms)
{
    d->raw_active = raw_active;
    d->initialized = true;
    if (raw_active != d->prev_raw) {
        d->prev_raw = raw_active;
        d->raw_changed_at_ms = now_ms;
        return;
    }
    if ((now_ms - d->raw_changed_at_ms) >= (int64_t)debounce_ms) {
        d->stable_active = raw_active;
    }
}

static uint8_t count_set_bits(uint8_t mask)
{
    uint8_t count = 0;
    for (int i = 0; i < 8; i++) {
        if (mask & (1U << i)) count++;
    }
    return count;
}

/* ================================================================
 * Direction
 * ================================================================ */

static drum_position_t mask_to_position(uint8_t mask)
{
    for (int i = 0; i < DRUM_POSITION_COUNT; i++) {
        if (mask == s_hall_table[i].mask) return s_hall_table[i].pos;
    }
    return DRUM_POS_UNKNOWN;
}

static int position_angle(drum_position_t pos)
{
    for (int i = 0; i < DRUM_POSITION_COUNT; i++) {
        if (s_hall_table[i].pos == pos) return s_hall_table[i].angle_deg;
    }
    return -1;
}

static int angle_distance_cw(int from_deg, int to_deg)
{
    return (to_deg - from_deg + 360) % 360;
}

static bool shortest_is_cw(int from_deg, int to_deg)
{
    int cw = angle_distance_cw(from_deg, to_deg);
    return cw <= (360 - cw);
}

/* ================================================================
 * Test hooks (compile with -DPOSITION_SERVICE_TEST_HOOKS)
 * ================================================================ */

#ifdef POSITION_SERVICE_TEST_HOOKS

/* pos_hook_fn is declared in position_service.h */

typedef struct {
    pos_hook_fn  fn;
    void        *user_data;
} pos_hook_entry_t;

static volatile pos_hook_entry_t s_hooks[POS_HOOK_COUNT];
static SemaphoreHandle_t s_hook_barriers[POS_HOOK_COUNT];

/* non-static: callable from test_runner.c */
void pos_service_set_hook(pos_hook_point_t where,
                          pos_hook_fn fn, void *user_data)
{
    s_hooks[where].fn = fn;
    s_hooks[where].user_data = user_data;
}

void *pos_service_get_barrier(pos_hook_point_t where)
{
    return (void *)s_hook_barriers[where];
}

static void pos_hooks_init(void)
{
    for (int i = 0; i < POS_HOOK_COUNT; i++) {
        if (!s_hook_barriers[i]) {
            s_hook_barriers[i] = xSemaphoreCreateBinary();
        }
    }
}

static inline void pos_invoke_hook(pos_hook_point_t where)
{
    pos_hook_fn fn = s_hooks[where].fn;
    if (fn) fn(where, s_hooks[where].user_data);
}

#else

static inline void pos_invoke_hook(pos_hook_point_t where) { (void)where; }

#endif /* POSITION_SERVICE_TEST_HOOKS */

/* ================================================================
 * Service context
 *
 * s_lc_mtx is STATIC — lives outside s_ctx, never memset or destroyed.
 * s_stop_mtx serializes concurrent stop() callers.
 * ================================================================ */

typedef struct {
    /* Config (immutable after init) */
    position_service_config_t cfg;
    const xiaojing_hal_t     *hal;
    machine_event_sink_t      event_sink;

    /* Protected by state_mutex */
    position_state_t    state;
    drum_position_t     current;
    drum_position_t     target;
    machine_request_id_t active_request_id;
    bool                moving_cw;
    uint8_t             pwm_percent;
    machine_fault_t     fault;
    hall_debounce_t     debounce;
    /* GPB5 is a separate drum-home/alignment input.  It must not make the
     * five-position decoder report a multiple-Hall fault. */
    hall_debounce_t     alignment_debounce;

    /* Timing (task-local) */
    int64_t move_start_ms;
    int64_t move_timeout_effective_ms;
    int64_t brake_start_ms;

    /* FreeRTOS resources — created by init, destroyed by cleanup_resources */
    QueueHandle_t     cmd_queue;
    SemaphoreHandle_t state_mutex;
    SemaphoreHandle_t task_ready_sem;
    SemaphoreHandle_t task_done_sem;
    TaskHandle_t      task_handle;     /* protected by s_lc_mtx */
    esp_err_t         task_exit_result;/* written by task under s_lc_mtx */

    /* Protected by s_lc_mtx */
    lifecycle_t       lifecycle;
    atomic_bool       stop_requested;  /* C11 atomic: lock-free cross-core visibility */
    uint32_t          generation;      /* incremented on each init cycle */
} position_ctx_t;

static position_ctx_t s_ctx;

/* Static lifecycle mutex — binary lifetime, never destroyed */
static StaticSemaphore_t s_lc_mtx_storage;
static SemaphoreHandle_t s_lc_mtx = NULL;

/* Static stop mutex — serializes concurrent stop() callers */
static StaticSemaphore_t s_stop_mtx_storage;
static SemaphoreHandle_t s_stop_mtx = NULL;

/* ---- ps_sync helper implementation (see position_service_sync.h) ---- */

esp_err_t ps_sync_init(ps_sync_ctx_t *ctx,
                       esp_err_t (*create_fn)(void *user),
                       void *user)
{
    int expected = PS_SYNC_UNINITIALIZED;
    if (!atomic_compare_exchange_strong_explicit(
            &ctx->state, &expected, PS_SYNC_INITIALIZING,
            memory_order_acquire, memory_order_acquire)) {
        /* Another caller won the CAS. Wait for it to finish. */
        TickType_t deadline = xTaskGetTickCount()
                              + pdMS_TO_TICKS(PS_SYNC_WAIT_TIMEOUT_MS);
        while (atomic_load_explicit(&ctx->state, memory_order_acquire)
               == PS_SYNC_INITIALIZING) {
            if (xTaskGetTickCount() >= deadline) return ESP_ERR_TIMEOUT;
            vTaskDelay(1);
        }
        int st = atomic_load_explicit(&ctx->state, memory_order_acquire);
        if (st == PS_SYNC_READY) return ESP_OK;
        return (esp_err_t)atomic_load_explicit(&ctx->error,
                                               memory_order_acquire);
    }

    /* We are the initializer */
    esp_err_t err = create_fn(user);
    if (err == ESP_OK) {
        atomic_store_explicit(&ctx->state, PS_SYNC_READY,
                              memory_order_release);
        return ESP_OK;
    }
    atomic_store_explicit(&ctx->error, err, memory_order_relaxed);
    atomic_store_explicit(&ctx->state, PS_SYNC_FAILED,
                          memory_order_release);
    return err;
}

esp_err_t ps_sync_wait(ps_sync_ctx_t *ctx, uint32_t timeout_ms)
{
    int state = atomic_load_explicit(&ctx->state, memory_order_acquire);
    if (state == PS_SYNC_READY) return ESP_OK;

    if (state == PS_SYNC_INITIALIZING) {
        TickType_t deadline = xTaskGetTickCount()
                              + pdMS_TO_TICKS(timeout_ms);
        do {
            if (xTaskGetTickCount() >= deadline) return ESP_ERR_TIMEOUT;
            vTaskDelay(1);
            state = atomic_load_explicit(&ctx->state, memory_order_acquire);
        } while (state == PS_SYNC_INITIALIZING);
    }
    if (state == PS_SYNC_READY) return ESP_OK;
    if (state == PS_SYNC_FAILED) {
        return (esp_err_t)atomic_load_explicit(&ctx->error,
                                               memory_order_acquire);
    }
    return ESP_ERR_INVALID_STATE;
}

/* ---- Production global sync instance ---- */

static ps_sync_ctx_t s_sync = PS_SYNC_CTX_INIT;

static esp_err_t create_lc_mutexes(void *user)
{
    (void)user;
    s_lc_mtx = xSemaphoreCreateMutexStatic(&s_lc_mtx_storage);
    s_stop_mtx = xSemaphoreCreateMutexStatic(&s_stop_mtx_storage);
    return (s_lc_mtx && s_stop_mtx) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t position_service_global_init(void)
{
    return ps_sync_init(&s_sync, create_lc_mutexes, NULL);
}

static esp_err_t wait_for_ready(void)
{
    return ps_sync_wait(&s_sync, PS_SYNC_WAIT_TIMEOUT_MS);
}

static inline void lc_lock(void)   { xSemaphoreTake(s_lc_mtx, portMAX_DELAY); }
static inline void lc_unlock(void) { xSemaphoreGive(s_lc_mtx); }

static inline void stop_lock(void)   { xSemaphoreTake(s_stop_mtx, portMAX_DELAY); }
static inline void stop_unlock(void) { xSemaphoreGive(s_stop_mtx); }

/* ================================================================
 * State helpers
 * ================================================================ */

static void set_state_locked(position_state_t new_state)
{
    s_ctx.state = new_state;
}

static machine_request_id_t claim_active_request(void)
{
    machine_request_id_t rid = s_ctx.active_request_id;
    s_ctx.active_request_id = MACHINE_REQUEST_ID_INVALID;
    return rid;
}

/* ================================================================
 * Event publishing (outside ALL locks)
 * ================================================================ */

static void publish_done(machine_request_id_t request_id,
                         service_result_t result,
                         const machine_fault_t *fault_snap)
{
    if (!s_ctx.event_sink.publish) return;
    if (request_id == MACHINE_REQUEST_ID_INVALID) return;
    machine_event_t ev = {0};
    ev.type = MACHINE_EVENT_POSITION_DONE;
    ev.request_id = request_id;
    ev.result = result;
    if (fault_snap) ev.fault = *fault_snap;
    s_ctx.event_sink.publish(&ev, 0, s_ctx.event_sink.context);
}

static void publish_fault_evt(machine_request_id_t request_id,
                              const machine_fault_t *fault_snap)
{
    if (!s_ctx.event_sink.publish || !fault_snap) return;
    if (fault_snap->code == FAULT_NONE) return;
    machine_event_t ev = {0};
    ev.type = MACHINE_EVENT_FAULT;
    ev.request_id = request_id;
    ev.fault = *fault_snap;
    s_ctx.event_sink.publish(&ev, 0, s_ctx.event_sink.context);
}

/* ================================================================
 * Motor control
 * ================================================================ */

static esp_err_t start_motor(ibt2_command_type_t direction, uint8_t pwm)
{
    ibt2_command_t cmd = { .command = direction, .pwm_percent = pwm };
    return s_ctx.hal->set_ibt2(cmd);
}

static esp_err_t stop_motor(void)
{
    return s_ctx.hal->stop_ibt2();
}

/* ================================================================
 * enter_fault — calls stop_motor, uses stop result as detail.
 * ================================================================ */

static void enter_fault(service_result_t terminal_result,
                        machine_fault_code_t code,
                        fault_severity_t severity)
{
    esp_err_t stop_err = stop_motor();

    machine_request_id_t rid;
    machine_fault_t fault_snap;

    xSemaphoreTake(s_ctx.state_mutex, portMAX_DELAY);
    rid = claim_active_request();
    s_ctx.fault.code = code;
    s_ctx.fault.severity = severity;
    s_ctx.fault.timestamp_ms = s_ctx.hal->now_ms();
    s_ctx.fault.detail = (uint32_t)stop_err;
    set_state_locked(POSITION_STATE_FAULT);
    s_ctx.pwm_percent = 0;
    s_ctx.current = DRUM_POS_UNKNOWN;
    fault_snap = s_ctx.fault;
    xSemaphoreGive(s_ctx.state_mutex);

    publish_done(rid, terminal_result, &fault_snap);
    publish_fault_evt(rid, &fault_snap);
}

/* ================================================================
 * transition_to_fault — uses caller-provided detail (original error).
 * Does NOT call stop_motor again. Best-effort stop already done.
 * ================================================================ */

static void transition_to_fault(service_result_t terminal_result,
                                machine_fault_code_t code,
                                fault_severity_t severity,
                                esp_err_t original_err)
{
    machine_request_id_t rid;
    machine_fault_t fault_snap;

    xSemaphoreTake(s_ctx.state_mutex, portMAX_DELAY);
    rid = claim_active_request();
    s_ctx.fault.code = code;
    s_ctx.fault.severity = severity;
    s_ctx.fault.timestamp_ms = s_ctx.hal->now_ms();
    s_ctx.fault.detail = (uint32_t)original_err;
    set_state_locked(POSITION_STATE_FAULT);
    s_ctx.pwm_percent = 0;
    s_ctx.current = DRUM_POS_UNKNOWN;
    fault_snap = s_ctx.fault;
    xSemaphoreGive(s_ctx.state_mutex);

    publish_done(rid, terminal_result, &fault_snap);
    publish_fault_evt(rid, &fault_snap);
}

/* ================================================================
 * Input validation
 * ================================================================ */

static bool validate_move_request(const position_move_request_t *req)
{
    if (!req) return false;
    if (req->request_id == MACHINE_REQUEST_ID_INVALID) return false;
    if (!drum_position_is_valid(req->target)) return false;
    int policy = (int)req->direction_policy;
    if (policy < 0 || policy > (int)POSITION_DIR_USE_DEFAULT) return false;
    if (req->pwm_percent != 0 &&
        (req->pwm_percent < 5 || req->pwm_percent > 100)) return false;
    if (req->timeout_ms != 0 &&
        (req->timeout_ms < 1000 || req->timeout_ms > 120000)) return false;
    return true;
}

static bool validate_config(const position_service_config_t *cfg)
{
    if (!cfg) return false;
    int policy = (int)cfg->default_policy;
    if (policy < 0 || policy > (int)POSITION_DIR_MAX_VALID_FOR_CONFIG) return false;
    if (cfg->move_pwm_percent < 5 || cfg->move_pwm_percent > 100) return false;
    if (cfg->approach_pwm_percent < 5 || cfg->approach_pwm_percent > 100) return false;
    if (cfg->approach_pwm_percent > cfg->move_pwm_percent) return false;
    if (cfg->debounce_ms < 20 || cfg->debounce_ms > 200) return false;
    if (cfg->move_timeout_ms < 1000 || cfg->move_timeout_ms > 120000) return false;
    if (cfg->brake_ms < 100 || cfg->brake_ms > 5000) return false;
    return true;
}

static bool validate_hal(const xiaojing_hal_t *hal)
{
    if (!hal) return false;
    if (!hal->read_mcp_inputs) return false;
    if (!hal->set_ibt2) return false;
    if (!hal->stop_ibt2) return false;
    if (!hal->emergency_shutdown) return false;
    if (!hal->now_ms) return false;
    return true;
}

/* ================================================================
 * Command handlers
 * ================================================================ */

static position_direction_policy_t resolve_policy(position_direction_policy_t p)
{
    return (p == POSITION_DIR_USE_DEFAULT) ? s_ctx.cfg.default_policy : p;
}

static void handle_move(const pos_cmd_t *cmd)
{
    drum_position_t target = cmd->move.target;
    position_direction_policy_t policy = resolve_policy(cmd->move.direction_policy);
    if (policy > POSITION_DIR_MAX_VALID_FOR_CONFIG) return;

    uint8_t pwm = cmd->move.pwm_percent;
    if (pwm == 0) pwm = s_ctx.cfg.move_pwm_percent;
    if (pwm < 5 || pwm > 100) return;

    uint32_t timeout = cmd->move.timeout_ms;
    if (timeout == 0) timeout = s_ctx.cfg.move_timeout_ms;
    if (timeout == 0) timeout = DEFAULT_MOVE_TIMEOUT_MS;

    machine_request_id_t rid = cmd->move.request_id;

    xSemaphoreTake(s_ctx.state_mutex, portMAX_DELAY);
    position_state_t cur_state = s_ctx.state;
    drum_position_t  cur_pos   = s_ctx.current;
    bool calibrated = s_ctx.cfg.direction_calibrated;

    if (cur_state != POSITION_STATE_IDLE_KNOWN &&
        cur_state != POSITION_STATE_IDLE_UNKNOWN) {
        xSemaphoreGive(s_ctx.state_mutex);
        publish_done(rid, SERVICE_RESULT_REJECTED, NULL);
        return;
    }

    if (cur_pos == target && cur_pos != DRUM_POS_UNKNOWN) {
        xSemaphoreGive(s_ctx.state_mutex);
        publish_done(rid, SERVICE_RESULT_OK, NULL);
        return;
    }

    if (!calibrated) {
        xSemaphoreGive(s_ctx.state_mutex);
        publish_done(rid, SERVICE_RESULT_REJECTED, NULL);
        return;
    }

    bool cw = true;
    if (cur_pos == DRUM_POS_UNKNOWN) {
        switch (policy) {
        case POSITION_DIR_PREFER_CCW:
        case POSITION_DIR_FORCE_CCW:
            cw = false;
            break;
        default:
            cw = true;
            break;
        }
    } else {
        int from_deg = position_angle(cur_pos);
        int to_deg   = position_angle(target);
        switch (policy) {
        case POSITION_DIR_AUTO_SHORTEST:
            cw = shortest_is_cw(from_deg, to_deg);
            break;
        case POSITION_DIR_PREFER_CW:
        case POSITION_DIR_FORCE_CW:
            cw = true;
            break;
        case POSITION_DIR_PREFER_CCW:
        case POSITION_DIR_FORCE_CCW:
            cw = false;
            break;
        default:
            cw = true;
            break;
        }
    }

    s_ctx.active_request_id = rid;
    s_ctx.target = target;
    s_ctx.moving_cw = cw;
    s_ctx.pwm_percent = pwm;
    s_ctx.move_start_ms = s_ctx.hal->now_ms();
    s_ctx.move_timeout_effective_ms = (int64_t)timeout;
    set_state_locked(POSITION_STATE_MOVING);
    xSemaphoreGive(s_ctx.state_mutex);

    esp_err_t err = start_motor(cw ? IBT2_CMD_CW : IBT2_CMD_CCW, pwm);
    if (err != ESP_OK) {
        transition_to_fault(SERVICE_RESULT_FAULT, FAULT_POSITION_DRIVER,
                            FAULT_SEVERITY_RECOVERABLE, err);
    }
}

static void handle_cancel(const pos_cmd_t *cmd)
{
    machine_request_id_t cancel_id = cmd->cancel_id;
    if (cancel_id == MACHINE_REQUEST_ID_INVALID) return;

    xSemaphoreTake(s_ctx.state_mutex, portMAX_DELAY);
    position_state_t    st  = s_ctx.state;
    machine_request_id_t rid = s_ctx.active_request_id;

    if (cancel_id != rid || rid == MACHINE_REQUEST_ID_INVALID) {
        xSemaphoreGive(s_ctx.state_mutex);
        return;
    }
    if (st == POSITION_STATE_FAULT || st == POSITION_STATE_STOPPING) {
        xSemaphoreGive(s_ctx.state_mutex);
        return;
    }

    if (st == POSITION_STATE_MOVING || st == POSITION_STATE_BRAKING) {
        xSemaphoreGive(s_ctx.state_mutex);

        esp_err_t stop_err = stop_motor();

        xSemaphoreTake(s_ctx.state_mutex, portMAX_DELAY);
        set_state_locked(POSITION_STATE_STOPPING);
        s_ctx.pwm_percent = 0;
        s_ctx.brake_start_ms = s_ctx.hal->now_ms();
        s_ctx.fault.detail = (uint32_t)stop_err;
        xSemaphoreGive(s_ctx.state_mutex);

        if (stop_err != ESP_OK) {
            transition_to_fault(SERVICE_RESULT_FAULT, FAULT_POSITION_DRIVER,
                                FAULT_SEVERITY_RECOVERABLE, stop_err);
        } else {
            xSemaphoreTake(s_ctx.state_mutex, portMAX_DELAY);
            s_ctx.active_request_id = MACHINE_REQUEST_ID_INVALID;
            xSemaphoreGive(s_ctx.state_mutex);
            publish_done(rid, SERVICE_RESULT_CANCELED, NULL);
        }
        return;
    }

    /* IDLE states */
    s_ctx.active_request_id = MACHINE_REQUEST_ID_INVALID;
    xSemaphoreGive(s_ctx.state_mutex);
    publish_done(rid, SERVICE_RESULT_CANCELED, NULL);
}

/* ================================================================
 * MOVING state tick
 * ================================================================ */

static void tick_moving(void)
{
    xSemaphoreTake(s_ctx.state_mutex, portMAX_DELAY);
    uint8_t stable = s_ctx.debounce.stable_active;
    bool debounced = s_ctx.debounce.initialized;
    drum_position_t target = s_ctx.target;
    xSemaphoreGive(s_ctx.state_mutex);

    if (!debounced) return;
    drum_position_t pos = mask_to_position(stable);

    if (count_set_bits(stable) >= 2) {
        enter_fault(SERVICE_RESULT_FAULT, FAULT_HALL_CONFLICT,
                    FAULT_SEVERITY_LATCHED);
        return;
    }

    if (pos == target && pos != DRUM_POS_UNKNOWN) {
        esp_err_t stop_err = stop_motor();
        if (stop_err != ESP_OK) {
            transition_to_fault(SERVICE_RESULT_FAULT, FAULT_POSITION_DRIVER,
                                FAULT_SEVERITY_RECOVERABLE, stop_err);
            return;
        }
        xSemaphoreTake(s_ctx.state_mutex, portMAX_DELAY);
        s_ctx.current = pos;
        s_ctx.pwm_percent = 0;
        s_ctx.brake_start_ms = s_ctx.hal->now_ms();
        set_state_locked(POSITION_STATE_BRAKING);
        xSemaphoreGive(s_ctx.state_mutex);
        return;
    }

    if (pos != DRUM_POS_UNKNOWN) {
        xSemaphoreTake(s_ctx.state_mutex, portMAX_DELAY);
        s_ctx.current = pos;
        xSemaphoreGive(s_ctx.state_mutex);
    }

    int64_t now = s_ctx.hal->now_ms();
    if ((now - s_ctx.move_start_ms) >= s_ctx.move_timeout_effective_ms) {
        enter_fault(SERVICE_RESULT_TIMEOUT, FAULT_POSITION_TIMEOUT,
                    FAULT_SEVERITY_RECOVERABLE);
    }
}

/* ================================================================
 * Read and debounce halls
 * ================================================================ */

static bool update_halls(void)
{
    mcp_input_snapshot_t mcp = {0};
    esp_err_t err = s_ctx.hal->read_mcp_inputs(&mcp);
    if (err != ESP_OK) return false;

    uint8_t raw = (~mcp.gpio_b) & HALL_ALL_MASK;
    uint8_t raw_alignment = (~mcp.gpio_b) & HALL_MASK_BUCKET_ALIGN;
    /* Log only transitions: enough to diagnose wiring/polarity without
     * flooding the serial link from the 10 ms position polling loop. */
    static uint8_t last_logged_inputs = 0xFF;
    uint8_t logged_inputs = raw | raw_alignment;
    if (logged_inputs != last_logged_inputs) {
        ESP_LOGI(TAG, "hall raw_active=0x%02x align=%u gpio_b=0x%02x",
                 raw, raw_alignment ? 1U : 0U, mcp.gpio_b);
        last_logged_inputs = logged_inputs;
    }
    int64_t now = s_ctx.hal->now_ms();

    xSemaphoreTake(s_ctx.state_mutex, portMAX_DELAY);
    debounce_update(&s_ctx.debounce, raw, now, s_ctx.cfg.debounce_ms);
    debounce_update(&s_ctx.alignment_debounce, raw_alignment, now,
                    s_ctx.cfg.debounce_ms);
    xSemaphoreGive(s_ctx.state_mutex);
    return true;
}

/* ================================================================
 * Emergency handling (task context)
 * ================================================================ */

static void handle_emergency(void)
{
    esp_err_t emg_err = s_ctx.hal->emergency_shutdown();

    machine_request_id_t rid;
    machine_fault_t fault_snap = {0};

    xSemaphoreTake(s_ctx.state_mutex, portMAX_DELAY);
    rid = claim_active_request();

    if (emg_err != ESP_OK) {
        s_ctx.fault.code = FAULT_POSITION_DRIVER;
        s_ctx.fault.severity = FAULT_SEVERITY_RECOVERABLE;
        s_ctx.fault.timestamp_ms = s_ctx.hal->now_ms();
        s_ctx.fault.detail = (uint32_t)emg_err;
        set_state_locked(POSITION_STATE_FAULT);
        s_ctx.pwm_percent = 0;
        s_ctx.current = DRUM_POS_UNKNOWN;
        fault_snap = s_ctx.fault;
    } else {
        set_state_locked(POSITION_STATE_STOPPING);
        s_ctx.pwm_percent = 0;
        s_ctx.current = DRUM_POS_UNKNOWN;
        s_ctx.brake_start_ms = s_ctx.hal->now_ms();
    }
    xSemaphoreGive(s_ctx.state_mutex);

    /* Drain queue */
    pos_cmd_t drain;
    while (xQueueReceive(s_ctx.cmd_queue, &drain, 0) == pdTRUE) { }

    if (rid != MACHINE_REQUEST_ID_INVALID) {
        if (emg_err != ESP_OK) {
            publish_done(rid, SERVICE_RESULT_FAULT, &fault_snap);
            publish_fault_evt(rid, &fault_snap);
        } else {
            publish_done(rid, SERVICE_RESULT_CANCELED, NULL);
        }
    }
}

/* ================================================================
 * Main task
 * ================================================================ */

static void position_task(void *arg)
{
    (void)arg;

    /* Initial hall read with debounce */
    update_halls();
    vTaskDelay(pdMS_TO_TICKS(s_ctx.cfg.debounce_ms + POLL_INTERVAL_MS));
    update_halls();

    /* Determine initial state */
    machine_fault_t startup_fault = {0};
    bool has_startup_fault = false;

    xSemaphoreTake(s_ctx.state_mutex, portMAX_DELAY);
    uint8_t init_stable = s_ctx.debounce.stable_active;
    drum_position_t init_pos = mask_to_position(init_stable);
    if (count_set_bits(init_stable) >= 2) {
        s_ctx.fault.code = FAULT_HALL_CONFLICT;
        s_ctx.fault.severity = FAULT_SEVERITY_LATCHED;
        s_ctx.fault.timestamp_ms = s_ctx.hal->now_ms();
        s_ctx.current = DRUM_POS_UNKNOWN;
        set_state_locked(POSITION_STATE_FAULT);
        startup_fault = s_ctx.fault;
        has_startup_fault = true;
    } else if (init_pos != DRUM_POS_UNKNOWN && count_set_bits(init_stable) == 1) {
        s_ctx.current = init_pos;
        set_state_locked(POSITION_STATE_IDLE_KNOWN);
    } else {
        s_ctx.current = DRUM_POS_UNKNOWN;
        set_state_locked(POSITION_STATE_IDLE_UNKNOWN);
    }
    xSemaphoreGive(s_ctx.state_mutex);

    if (has_startup_fault) {
        publish_fault_evt(MACHINE_REQUEST_ID_INVALID, &startup_fault);
    }

    /* Signal init complete */
    xSemaphoreGive(s_ctx.task_ready_sem);

    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        /* 1. Emergency notification (non-blocking) */
        uint32_t notify = 0;
        if (xTaskNotifyWait(0, NOTIFY_EMERGENCY_BIT | NOTIFY_STOP_BIT,
                            &notify, 0) == pdTRUE) {
            if (notify & NOTIFY_EMERGENCY_BIT) {
                xSemaphoreTake(s_ctx.state_mutex, portMAX_DELAY);
                position_state_t st = s_ctx.state;
                xSemaphoreGive(s_ctx.state_mutex);
                if (st != POSITION_STATE_FAULT && st != POSITION_STATE_STOPPING) {
                    handle_emergency();
                }
            }
            if (notify & NOTIFY_STOP_BIT) break;
        }

        if (atomic_load(&s_ctx.stop_requested)) break;

        /* 2. Drain command queue */
        pos_cmd_t cmd;
        if (xQueueReceive(s_ctx.cmd_queue, &cmd, 0) == pdTRUE) {
            switch (cmd.type) {
            case CMD_MOVE:   handle_move(&cmd);  break;
            case CMD_CANCEL: handle_cancel(&cmd); break;
            }
        }

        /* 3. Read halls */
        xSemaphoreTake(s_ctx.state_mutex, portMAX_DELAY);
        position_state_t state = s_ctx.state;
        xSemaphoreGive(s_ctx.state_mutex);

        bool mcp_ok = true;
        if (state != POSITION_STATE_FAULT) {
            mcp_ok = update_halls();
            if (!mcp_ok && (state == POSITION_STATE_MOVING ||
                            state == POSITION_STATE_BRAKING)) {
                enter_fault(SERVICE_RESULT_FAULT, FAULT_MCP_IO,
                            FAULT_SEVERITY_RECOVERABLE);
                goto next_tick;
            }
        }

        /* 4. State machine */
        switch (state) {
        case POSITION_STATE_MOVING:
            tick_moving();
            break;

        case POSITION_STATE_BRAKING: {
            xSemaphoreTake(s_ctx.state_mutex, portMAX_DELAY);
            int64_t brake_elapsed = s_ctx.hal->now_ms() - s_ctx.brake_start_ms;
            uint32_t brake_ms = s_ctx.cfg.brake_ms;
            uint8_t stable = s_ctx.debounce.stable_active;
            drum_position_t target = s_ctx.target;
            xSemaphoreGive(s_ctx.state_mutex);

            if (brake_elapsed >= (int64_t)brake_ms) {
                drum_position_t pos = mask_to_position(stable);
                if (count_set_bits(stable) >= 2) {
                    enter_fault(SERVICE_RESULT_FAULT, FAULT_HALL_CONFLICT,
                                FAULT_SEVERITY_LATCHED);
                } else if (pos != target || pos == DRUM_POS_UNKNOWN) {
                    enter_fault(SERVICE_RESULT_FAULT, FAULT_POSITION_DRIVER,
                                FAULT_SEVERITY_RECOVERABLE);
                } else {
                    esp_err_t stop_err = stop_motor();
                    if (stop_err != ESP_OK) {
                        transition_to_fault(SERVICE_RESULT_FAULT,
                                            FAULT_POSITION_DRIVER,
                                            FAULT_SEVERITY_RECOVERABLE,
                                            stop_err);
                        break;
                    }
                    machine_request_id_t rid;
                    xSemaphoreTake(s_ctx.state_mutex, portMAX_DELAY);
                    rid = claim_active_request();
                    s_ctx.current = pos;
                    set_state_locked(POSITION_STATE_IDLE_KNOWN);
                    xSemaphoreGive(s_ctx.state_mutex);
                    publish_done(rid, SERVICE_RESULT_OK, NULL);
                }
            }
            break;
        }

        case POSITION_STATE_STOPPING: {
            xSemaphoreTake(s_ctx.state_mutex, portMAX_DELAY);
            int64_t brake_elapsed = s_ctx.hal->now_ms() - s_ctx.brake_start_ms;
            uint32_t brake_ms = s_ctx.cfg.brake_ms;
            xSemaphoreGive(s_ctx.state_mutex);

            if (brake_elapsed >= (int64_t)brake_ms) {
                xSemaphoreTake(s_ctx.state_mutex, portMAX_DELAY);
                s_ctx.active_request_id = MACHINE_REQUEST_ID_INVALID;
                s_ctx.current = DRUM_POS_UNKNOWN;
                set_state_locked(POSITION_STATE_IDLE_UNKNOWN);
                xSemaphoreGive(s_ctx.state_mutex);
            }
            break;
        }

        case POSITION_STATE_IDLE_UNKNOWN:
        case POSITION_STATE_IDLE_KNOWN: {
            if (mcp_ok && s_ctx.debounce.initialized) {
                xSemaphoreTake(s_ctx.state_mutex, portMAX_DELAY);
                uint8_t stable = s_ctx.debounce.stable_active;
                drum_position_t pos = mask_to_position(stable);
                machine_fault_t idle_fault = {0};
                bool idle_fault_pending = false;
                if (count_set_bits(stable) >= 2) {
                    s_ctx.fault.code = FAULT_HALL_CONFLICT;
                    s_ctx.fault.severity = FAULT_SEVERITY_LATCHED;
                    s_ctx.fault.timestamp_ms = s_ctx.hal->now_ms();
                    s_ctx.current = DRUM_POS_UNKNOWN;
                    set_state_locked(POSITION_STATE_FAULT);
                    idle_fault = s_ctx.fault;
                    idle_fault_pending = true;
                } else if (pos != DRUM_POS_UNKNOWN &&
                           count_set_bits(stable) == 1) {
                    s_ctx.current = pos;
                    if (s_ctx.state == POSITION_STATE_IDLE_UNKNOWN) {
                        set_state_locked(POSITION_STATE_IDLE_KNOWN);
                    }
                } else {
                    s_ctx.current = DRUM_POS_UNKNOWN;
                    if (s_ctx.state == POSITION_STATE_IDLE_KNOWN) {
                        set_state_locked(POSITION_STATE_IDLE_UNKNOWN);
                    }
                }
                xSemaphoreGive(s_ctx.state_mutex);
                if (idle_fault_pending) {
                    publish_fault_evt(MACHINE_REQUEST_ID_INVALID, &idle_fault);
                }
            } else if (!mcp_ok && state != POSITION_STATE_FAULT) {
                enter_fault(SERVICE_RESULT_FAULT, FAULT_MCP_IO,
                            FAULT_SEVERITY_RECOVERABLE);
            }
            break;
        }

        case POSITION_STATE_FAULT:
            break;

        default:
            break;
        }

        /* 5. Feed position → safety_manager (production bridge) */
        {
            xSemaphoreTake(s_ctx.state_mutex, portMAX_DELAY);
            position_state_t feed_state = s_ctx.state;
            drum_position_t feed_pos = s_ctx.current;
            xSemaphoreGive(s_ctx.state_mutex);

            bool feed_valid = (feed_state != POSITION_STATE_UNINITIALIZED &&
                               feed_state != POSITION_STATE_FAULT);
            bool feed_stable = (feed_state == POSITION_STATE_IDLE_KNOWN);
            bool feed_moving = (feed_state == POSITION_STATE_MOVING ||
                                feed_state == POSITION_STATE_BRAKING);
            safety_manager_update_position(feed_pos, feed_stable,
                                           feed_moving, feed_valid,
                                           (uint32_t)s_ctx.hal->now_ms());
        }

next_tick:
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }

    /* Task exiting — stop motor */
    pos_cmd_t drain;
    while (xQueueReceive(s_ctx.cmd_queue, &drain, 0) == pdTRUE) { }

    esp_err_t exit_stop_err = stop_motor();
    if (exit_stop_err != ESP_OK) {
        xSemaphoreTake(s_ctx.state_mutex, portMAX_DELAY);
        machine_request_id_t rid = claim_active_request();
        if (rid != MACHINE_REQUEST_ID_INVALID) {
            s_ctx.fault.code = FAULT_POSITION_DRIVER;
            s_ctx.fault.severity = FAULT_SEVERITY_RECOVERABLE;
            s_ctx.fault.detail = (uint32_t)exit_stop_err;
            s_ctx.fault.timestamp_ms = s_ctx.hal->now_ms();
            set_state_locked(POSITION_STATE_FAULT);
            machine_fault_t fault_snap = s_ctx.fault;
            xSemaphoreGive(s_ctx.state_mutex);
            publish_done(rid, SERVICE_RESULT_FAULT, &fault_snap);
            publish_fault_evt(rid, &fault_snap);
        } else {
            xSemaphoreGive(s_ctx.state_mutex);
        }
        ESP_LOGE(TAG, "task exit stop_motor failed: 0x%x", exit_stop_err);
    }

    /* Write exit result under lc lock, then signal done */
    pos_invoke_hook(POS_HOOK_PRE_EXIT_WRITE);
    lc_lock();
    s_ctx.task_exit_result = exit_stop_err;
    lc_unlock();

    xSemaphoreGive(s_ctx.task_done_sem);
    vTaskDelete(NULL);
}

/* ================================================================
 * Resource cleanup (caller must hold s_lc_mtx)
 * ================================================================ */

static void cleanup_resources(void)
{
    pos_invoke_hook(POS_HOOK_PRE_CLEANUP);

    if (s_ctx.cmd_queue) {
        pos_cmd_t drain;
        while (xQueueReceive(s_ctx.cmd_queue, &drain, 0) == pdTRUE) { }
        vQueueDelete(s_ctx.cmd_queue);
        s_ctx.cmd_queue = NULL;
    }
    if (s_ctx.task_ready_sem) {
        vSemaphoreDelete(s_ctx.task_ready_sem);
        s_ctx.task_ready_sem = NULL;
    }
    if (s_ctx.task_done_sem) {
        vSemaphoreDelete(s_ctx.task_done_sem);
        s_ctx.task_done_sem = NULL;
    }
    if (s_ctx.state_mutex) {
        vSemaphoreDelete(s_ctx.state_mutex);
        s_ctx.state_mutex = NULL;
    }
    s_ctx.task_handle = NULL;
    s_ctx.task_exit_result = ESP_OK;
    s_ctx.hal = NULL;
    s_ctx.event_sink = (machine_event_sink_t){0};
    atomic_store(&s_ctx.stop_requested, false);
    s_ctx.lifecycle = LC_UNINITIALIZED;
    s_ctx.state = POSITION_STATE_UNINITIALIZED;
}

/* ================================================================
 * Public API — Lifecycle
 * ================================================================ */

esp_err_t position_service_init(const position_service_config_t *config,
                                const xiaojing_hal_t *hal,
                                machine_event_sink_t event_sink)
{
    if (!config || !hal) return ESP_ERR_INVALID_ARG;
    if (!validate_config(config)) return ESP_ERR_INVALID_ARG;
    if (!validate_hal(hal)) return ESP_ERR_INVALID_ARG;

    if (wait_for_ready() != ESP_OK) return ESP_ERR_INVALID_STATE;

#ifdef POSITION_SERVICE_TEST_HOOKS
    pos_hooks_init();
#endif

    lc_lock();

    if (s_ctx.lifecycle != LC_UNINITIALIZED) {
        lc_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    /* Reset context — safe because lifecycle_mutex is static */
    s_ctx.cfg = *config;
    s_ctx.hal = hal;
    s_ctx.event_sink = event_sink;
    s_ctx.state = POSITION_STATE_UNINITIALIZED;
    s_ctx.current = DRUM_POS_UNKNOWN;
    s_ctx.target = DRUM_POS_UNKNOWN;
    s_ctx.active_request_id = MACHINE_REQUEST_ID_INVALID;
    s_ctx.task_handle = NULL;
    s_ctx.task_exit_result = ESP_OK;
    memset(&s_ctx.fault, 0, sizeof(s_ctx.fault));
    atomic_store(&s_ctx.stop_requested, false);
    s_ctx.generation++;
    debounce_init(&s_ctx.debounce);
    debounce_init(&s_ctx.alignment_debounce);

    s_ctx.state_mutex = xSemaphoreCreateMutex();
    if (!s_ctx.state_mutex) { lc_unlock(); return ESP_ERR_NO_MEM; }

    s_ctx.task_ready_sem = xSemaphoreCreateBinary();
    if (!s_ctx.task_ready_sem) {
        vSemaphoreDelete(s_ctx.state_mutex); s_ctx.state_mutex = NULL;
        lc_unlock();
        return ESP_ERR_NO_MEM;
    }

    s_ctx.task_done_sem = xSemaphoreCreateBinary();
    if (!s_ctx.task_done_sem) {
        vSemaphoreDelete(s_ctx.task_ready_sem); s_ctx.task_ready_sem = NULL;
        vSemaphoreDelete(s_ctx.state_mutex); s_ctx.state_mutex = NULL;
        lc_unlock();
        return ESP_ERR_NO_MEM;
    }

    s_ctx.cmd_queue = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(pos_cmd_t));
    if (!s_ctx.cmd_queue) {
        vSemaphoreDelete(s_ctx.task_done_sem); s_ctx.task_done_sem = NULL;
        vSemaphoreDelete(s_ctx.task_ready_sem); s_ctx.task_ready_sem = NULL;
        vSemaphoreDelete(s_ctx.state_mutex); s_ctx.state_mutex = NULL;
        lc_unlock();
        return ESP_ERR_NO_MEM;
    }

    s_ctx.lifecycle = LC_INITIALIZED;
    lc_unlock();
    return ESP_OK;
}

esp_err_t position_service_start(void)
{
    if (wait_for_ready() != ESP_OK) return ESP_ERR_INVALID_STATE;

    lc_lock();
    if (s_ctx.lifecycle != LC_INITIALIZED) {
        lc_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx.lifecycle = LC_STARTING;
    atomic_store(&s_ctx.stop_requested, false);
    lc_unlock();

    TaskHandle_t local_handle = NULL;
    BaseType_t ret = xTaskCreatePinnedToCore(
        position_task, "pos_svc", TASK_STACK_SIZE, NULL,
        TASK_PRIORITY, &local_handle, 1);
    if (ret != pdPASS) {
        lc_lock();
        s_ctx.lifecycle = LC_INITIALIZED;
        lc_unlock();
        return ESP_ERR_NO_MEM;
    }

    /* Commit task handle under lc_lock for visibility across cores */
    lc_lock();
    s_ctx.task_handle = local_handle;
    lc_unlock();

    if (xSemaphoreTake(s_ctx.task_ready_sem, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGE(TAG, "start: task init timeout");

        /* Stop the task if it started */
        lc_lock();
        atomic_store(&s_ctx.stop_requested, true);
        lc_unlock();

        lc_lock();
        TaskHandle_t h = s_ctx.task_handle;
        lc_unlock();

        if (h) xTaskNotify(h, NOTIFY_STOP_BIT, eSetBits);

        /* Wait for task exit — WITHOUT holding lc_mtx */
        lc_lock();
        SemaphoreHandle_t done = s_ctx.task_done_sem;
        lc_unlock();

        if (done) {
            if (xSemaphoreTake(done, pdMS_TO_TICKS(3000)) == pdTRUE) {
                lc_lock();
                esp_err_t exit_res = s_ctx.task_exit_result;
                cleanup_resources();
                lc_unlock();
                if (exit_res != ESP_OK) return exit_res;
            } else {
                lc_lock();
                s_ctx.lifecycle = LC_STOPPING;
                lc_unlock();
            }
        } else {
            lc_lock();
            cleanup_resources();
            lc_unlock();
        }
        return ESP_ERR_TIMEOUT;
    }

    lc_lock();
    s_ctx.lifecycle = LC_RUNNING;
    lc_unlock();
    return ESP_OK;
}

/*
 * stop() — R5 deadlock-free design.
 *
 * stop_mutex serializes concurrent stop() callers.
 * lc_mtx is NEVER held while waiting on task_done_sem.
 *
 * Lock ordering: stop_mutex → lc_mtx → state_mutex
 */
esp_err_t position_service_stop(void)
{
    /* Check sync state atomically — never read mutex pointers without
     * first confirming SYNC_READY via acquire barrier. */
    int sync = atomic_load_explicit(&s_sync.state, memory_order_acquire);
    if (sync == PS_SYNC_UNINITIALIZED) return ESP_OK;

    esp_err_t ready_err = wait_for_ready();
    if (ready_err != ESP_OK) return ready_err;

    /* SYNC_READY confirmed — s_lc_mtx/s_stop_mtx are safe to use */

    pos_invoke_hook(POS_HOOK_PRE_LC_LOCK);
    stop_lock();

    lc_lock();
    pos_invoke_hook(POS_HOOK_POST_LC_LOCK);

    lifecycle_t lc = s_ctx.lifecycle;

    switch (lc) {
    case LC_UNINITIALIZED:
        lc_unlock();
        stop_unlock();
        return ESP_OK;

    case LC_INITIALIZED:
        cleanup_resources();
        lc_unlock();
        stop_unlock();
        return ESP_OK;

    case LC_STARTING:
        lc_unlock();
        stop_unlock();
        return ESP_ERR_INVALID_STATE;

    case LC_STOPPING:
    case LC_RUNNING:
        break;
    }

    if (lc == LC_RUNNING) {
        s_ctx.lifecycle = LC_STOPPING;
        atomic_store(&s_ctx.stop_requested, true);
        if (s_ctx.task_handle) {
            xTaskNotify(s_ctx.task_handle, NOTIFY_STOP_BIT, eSetBits);
        }
    }

    /* Read handle and sem under lc, then release lc before waiting */
    SemaphoreHandle_t done = s_ctx.task_done_sem;
    TaskHandle_t      task = s_ctx.task_handle;
    lc_unlock();

    /* Wait for task exit — lc_mtx NOT held */
    bool got_done = false;
    if (done && task) {
        got_done = (xSemaphoreTake(done, pdMS_TO_TICKS(3000)) == pdTRUE);
    } else if (!task) {
        /* No task handle — task already exited or was never started */
        got_done = true;
    }

    /* Re-acquire lc to read result and cleanup */
    lc_lock();
    esp_err_t exit_result = s_ctx.task_exit_result;

    if (got_done || s_ctx.task_handle == NULL) {
        cleanup_resources();
        lc_unlock();
        stop_unlock();
        return exit_result;
    }

    /* Timeout — keep STOPPING state, resources intact, retryable */
    s_ctx.lifecycle = LC_STOPPING;
    lc_unlock();
    stop_unlock();
    return ESP_ERR_TIMEOUT;
}

/* ================================================================
 * Public API — Commands
 *
 * TOCTOU-free: lc lock held during check AND resource use.
 * Lock ordering: lc_mtx → (queue send / task notify) [non-blocking]
 * ================================================================ */

esp_err_t position_service_move_async(const position_move_request_t *request)
{
    if (!validate_move_request(request)) return ESP_ERR_INVALID_ARG;

    if (wait_for_ready() != ESP_OK) return ESP_ERR_INVALID_STATE;

    pos_invoke_hook(POS_HOOK_PRE_LC_LOCK);
    lc_lock();
    pos_invoke_hook(POS_HOOK_POST_LC_LOCK);

    if (s_ctx.lifecycle != LC_RUNNING) {
        lc_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    pos_cmd_t cmd = {0};
    cmd.type = CMD_MOVE;
    cmd.move = *request;

    pos_invoke_hook(POS_HOOK_PRE_QSEND);
    esp_err_t err = (xQueueSend(s_ctx.cmd_queue, &cmd, pdMS_TO_TICKS(10)) == pdTRUE)
                    ? ESP_OK : ESP_ERR_TIMEOUT;
    lc_unlock();
    return err;
}

esp_err_t position_service_cancel(machine_request_id_t request_id)
{
    if (request_id == MACHINE_REQUEST_ID_INVALID) return ESP_ERR_INVALID_ARG;

    if (wait_for_ready() != ESP_OK) return ESP_ERR_INVALID_STATE;

    lc_lock();

    if (s_ctx.lifecycle != LC_RUNNING) {
        lc_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    pos_cmd_t cmd = {0};
    cmd.type = CMD_CANCEL;
    cmd.cancel_id = request_id;

    pos_invoke_hook(POS_HOOK_PRE_QSEND);
    esp_err_t err = (xQueueSend(s_ctx.cmd_queue, &cmd, pdMS_TO_TICKS(10)) == pdTRUE)
                    ? ESP_OK : ESP_ERR_TIMEOUT;
    lc_unlock();
    return err;
}

esp_err_t position_service_emergency_stop(void)
{
    if (wait_for_ready() != ESP_OK) return ESP_ERR_INVALID_STATE;

    pos_invoke_hook(POS_HOOK_PRE_LC_LOCK);
    lc_lock();
    pos_invoke_hook(POS_HOOK_POST_LC_LOCK);

    if (s_ctx.lifecycle != LC_RUNNING) {
        lc_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    TaskHandle_t h = s_ctx.task_handle;
    if (!h) { lc_unlock(); return ESP_ERR_INVALID_STATE; }

    pos_invoke_hook(POS_HOOK_PRE_NOTIFY);
    BaseType_t ret = xTaskNotify(h, NOTIFY_EMERGENCY_BIT, eSetBits);
    lc_unlock();
    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

/* ================================================================
 * Public API — Query
 *
 * TOCTOU-free: lc lock held while using state_mutex.
 * Lock ordering: lc_mtx → state_mutex (never reversed)
 * ================================================================ */

esp_err_t position_service_get_snapshot(position_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    if (wait_for_ready() != ESP_OK) return ESP_ERR_INVALID_STATE;

    pos_invoke_hook(POS_HOOK_PRE_LC_LOCK);
    lc_lock();
    pos_invoke_hook(POS_HOOK_POST_LC_LOCK);

    if (s_ctx.lifecycle == LC_UNINITIALIZED || !s_ctx.state_mutex) {
        lc_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    pos_invoke_hook(POS_HOOK_PRE_SM_LOCK);
    xSemaphoreTake(s_ctx.state_mutex, portMAX_DELAY);
    pos_invoke_hook(POS_HOOK_POST_SM_LOCK);

    out->state             = s_ctx.state;
    out->current           = s_ctx.current;
    out->target            = s_ctx.target;
    out->raw_hall_mask     = s_ctx.debounce.raw_active;
    out->stable_hall_mask  = s_ctx.debounce.stable_active;
    out->bucket_aligned =
        (s_ctx.alignment_debounce.stable_active & HALL_MASK_BUCKET_ALIGN) != 0U;
    out->moving_cw         = s_ctx.moving_cw;
    out->pwm_percent       = s_ctx.pwm_percent;
    out->active_request_id = s_ctx.active_request_id;
    out->fault             = s_ctx.fault;

    xSemaphoreGive(s_ctx.state_mutex);
    lc_unlock();
    return ESP_OK;
}
