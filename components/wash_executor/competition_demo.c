/* Competition demo coordinator. Only service APIs, never HAL/GPIO writes.
 * Executor owns one composite token; service completions keep their own IDs.
 * All waits yield, and every error/cancel path closes the owned outputs. */
#include "competition_demo.h"
#include "wash_planner.h"
#include "wash_executor_core.h"
#include "position_service.h"
#include "bl50_service.h"
#include "coupled_hot_air_service.h"
#include "uv_service.h"
#include "water_service.h"
#include "detergent_service.h"
#include "safety_manager.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <stdatomic.h>
#include <inttypes.h>
#include <string.h>

static const char *TAG = "competition";
static StaticSemaphore_t lock_storage, done_storage;
static SemaphoreHandle_t lock, done;
static StaticQueue_t event_storage;
static uint8_t event_bytes[16 * sizeof(machine_event_t)];
static QueueHandle_t events;
static TaskHandle_t task;
static bool task_join_pending;
static atomic_bool cancel_requested, shutdown_requested;
static atomic_bool job_reserved, event_failure;
static competition_demo_snapshot_t snapshot;
static terminal_token_t owner_token, cancel_token;
static competition_demo_terminal_fn callback;
static void *callback_context;
static uint32_t next_id = 0xd0000000U;
/* Owned by worker, while its job is reserved. */
static uint32_t position_id, motor_id, uv_id, heat_id;
static uint32_t valve_id, detergent_id;
static water_actuator_valve_t owned_valve;
static bool detergent_done;
static service_result_t detergent_result;
static bool position_done, motor_done, uv_done, heat_done;
static int64_t started_ms;
static bool lease;
static uint32_t failure_code, failure_detail;
static wash_recipe_t active_recipe;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static void pause_tick(void) { vTaskDelay(pdMS_TO_TICKS(20) ? pdMS_TO_TICKS(20) : 1); }
static uint32_t child_id(void) { if (++next_id == 0) next_id = 0xd0000001U; return next_id; }
static bool same_owner(const terminal_token_t *a, const terminal_token_t *b)
{
    return a->program_id == b->program_id && a->generation == b->generation &&
           a->request_id == b->request_id && a->step_id == b->step_id;
}

static bool outputs_stopped(void)
{
    safety_snapshot_t s; position_snapshot_t p; bl50_snapshot_t b;
    if (safety_manager_get_snapshot(&s) != ESP_OK ||
        position_service_get_snapshot(&p) != ESP_OK || bl50_service_get_snapshot(&b) != ESP_OK)
        return false;
    const safety_mcp_output_state_t *o = &s.mcp_outputs;
    uint32_t all = (1U << SAFE_OUTPUT_COUNT) - 1U;
    return (s.mcp_known_mask & all) == all && !o->uv && !o->ptc_heater &&
        !o->source_inlet_valve && !o->transfer_valve && !o->drain_valve &&
        !o->detergent_pump && !s.hot_air_on && s.bl50_state_known && !s.bl50_running &&
        b.current_pwm_percent == 0 && b.state == BL50_SVC_STATE_IDLE &&
        p.pwm_percent == 0 && (p.state == POSITION_STATE_IDLE_KNOWN ||
                             p.state == POSITION_STATE_IDLE_UNKNOWN || p.state == POSITION_STATE_FAULT);
}

static esp_err_t healthy(void)
{
    if (atomic_load(&cancel_requested) || atomic_load(&shutdown_requested))
        return ESP_ERR_INVALID_STATE;
    if (now_ms() - started_ms >= 180000) return ESP_ERR_TIMEOUT;
    if (atomic_load(&event_failure)) return ESP_FAIL;
    safety_snapshot_t s;
    if (safety_manager_get_snapshot(&s) != ESP_OK) return ESP_FAIL;
    if (s.fault_active || s.emergency_stop_active) {
        if (s.primary_fault.code != FAULT_NONE && !failure_code) {
            failure_code = s.primary_fault.code;
            failure_detail = s.primary_fault.detail;
        }
        return ESP_FAIL;
    }
    uv_snapshot_t u;
    if (uv_id && (uv_service_get_snapshot(&u) != ESP_OK ||
                  u.request_id != uv_id || u.output_state != 2))
        return ESP_ERR_INVALID_RESPONSE;
    return ESP_OK;
}

static void stage(uint8_t number, wash_phase_t phase, drum_position_t target)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    snapshot.stage = number; snapshot.phase = phase; snapshot.target = target;
    xSemaphoreGive(lock);
    ESP_LOGI(TAG,"stage=%u target=%d elapsed=%ums",number,(int)target,
             (unsigned)(now_ms() - started_ms));
}

static esp_err_t settle(uint32_t duration)
{
    int64_t until = now_ms() + duration;
    while (now_ms() < until) { esp_err_t e = healthy(); if (e != ESP_OK) return e; pause_tick(); }
    return ESP_OK;
}

static void remember_terminal(const machine_event_t *e)
{
    if (e->type == MACHINE_EVENT_POSITION_DONE && e->request_id == position_id) position_done = true;
    if (e->type == MACHINE_EVENT_BL50_DONE && e->request_id == motor_id) motor_done = true;
    if (e->type == MACHINE_EVENT_UV_DONE && e->request_id == uv_id) uv_done = true;
    if (e->type == MACHINE_EVENT_HOT_AIR_DONE && e->request_id == heat_id) heat_done = true;
    if (e->type == MACHINE_EVENT_DETERGENT_DONE && e->request_id == detergent_id) {
        detergent_done = true;
        detergent_result = e->result;
        if (e->result != SERVICE_RESULT_OK && e->result != SERVICE_RESULT_CANCELED) {
            failure_code = competition_demo_event_fault(e);
            failure_detail = e->fault.detail;
        }
    }
}

static bool valve_closed(void)
{
    if (!valve_id) return true;
    water_snapshot_t w;
    return water_service_get_snapshot(&w) == ESP_OK && w.act_test_request_id == valve_id &&
        !w.act_test_active && !w.source_valve_unknown && !w.transfer_valve_unknown &&
        !w.source_valve_on && !w.transfer_valve_on &&
        (w.act_test_state == WATER_ACT_TEST_COMPLETE || w.act_test_state == WATER_ACT_TEST_FAULT);
}

static esp_err_t timed_water_dose(water_actuator_valve_t valve, bool pump)
{
    safety_operation_t op = valve == WATER_ACTUATOR_VALVE_SOURCE ?
        SAFETY_OP_SOURCE_INLET_VALVE : SAFETY_OP_TRANSFER_VALVE;
    esp_err_t e = healthy(); if (e != ESP_OK) return e;
    e = safety_manager_demo_dose(owner_token.program_id, owner_token.generation, op, true);
    if (e != ESP_OK) return e;
    ESP_LOGW(TAG, "DRY DEMO: valve=%u 5000ms pump=%u; water/flow volume checks bypassed (not calibrated ml)",
        (unsigned)valve, (unsigned)pump);
    owned_valve = valve; valve_id = child_id();
    e = water_service_demo_valve_run(valve, valve_id);
    if (e != ESP_OK) { valve_id = 0; return e; }
    int64_t deadline = now_ms() + 9000;
    int64_t start_deadline = now_ms() + 2000;
    bool opened = false;
    while (now_ms() < start_deadline) {
        e = healthy(); if (e != ESP_OK) return e;
        water_snapshot_t w;
        if (water_service_get_snapshot(&w) == ESP_OK && w.act_test_request_id == valve_id) {
            if (w.act_test_state == WATER_ACT_TEST_FAULT) { failure_code = FAULT_MCP_IO; return ESP_FAIL; }
            if (w.act_test_active && w.act_test_state == WATER_ACT_TEST_RUNNING) {
                opened = true; break;
            }
        }
        pause_tick();
    }
    if (!opened) return ESP_ERR_TIMEOUT;
    if (pump) {
        detergent_request_t request = {.request_id = child_id(), .duration_ms = 5000, .competition_demo = true};
        detergent_id = request.request_id; detergent_done = false;
        e = detergent_service_run_async(&request);
        if (e != ESP_OK) { detergent_id = 0; return e; }
    }
    while (now_ms() < deadline) {
        e = healthy(); if (e != ESP_OK) return e;
        if (!safety_manager_demo_dose_allowed(op)) return ESP_ERR_INVALID_STATE;
        machine_event_t event;
        while (xQueueReceive(events, &event, 0) == pdTRUE) remember_terminal(&event);
        water_snapshot_t w;
        if (water_service_get_snapshot(&w) != ESP_OK) return ESP_FAIL;
        if (w.act_test_request_id == valve_id && w.act_test_state == WATER_ACT_TEST_FAULT) {
            failure_code = FAULT_MCP_IO; return ESP_FAIL;
        }
        if (pump && detergent_done && detergent_result != SERVICE_RESULT_OK) return ESP_FAIL;
        detergent_snapshot_t d;
        bool pump_off = !pump || (detergent_done && detergent_service_get_snapshot(&d) == ESP_OK &&
            d.request_id == detergent_id && d.output_state == 1 && d.terminal == DETERGENT_TERMINAL_COMPLETE);
        if (valve_closed() && pump_off) {
            ESP_LOGI(TAG, "DRY DEMO dose complete: both valves and pump OFF confirmed");
            return safety_manager_demo_dose(owner_token.program_id, owner_token.generation, op, false);
        }
        pause_tick();
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t wait_event(machine_event_type_t type, uint32_t id, uint32_t timeout)
{
    int64_t until = now_ms() + timeout;
    while (now_ms() < until) {
        esp_err_t e = healthy(); if (e != ESP_OK) return e;
        machine_event_t event;
        while (xQueueReceive(events, &event, 0) == pdTRUE) {
            remember_terminal(&event);
            if (event.type == type && event.request_id == id) {
                if (event.result == SERVICE_RESULT_OK) return ESP_OK;
                failure_code = competition_demo_event_fault(&event);
                failure_detail = event.fault.detail;
                ESP_LOGE(TAG, "child failed: service=%u request=%" PRIu32
                    " result=%u fault=%" PRIu32 " detail=%" PRIu32,
                    (unsigned)type, id, (unsigned)event.result, failure_code, failure_detail);
                return ESP_FAIL;
            }
        }
        pause_tick();
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t move(drum_position_t target)
{
    esp_err_t e = healthy(); if (e != ESP_OK) return e;
    position_move_request_t request = { .request_id = child_id(), .target = target,
        .direction_policy = POSITION_DIR_AUTO_SHORTEST, .pwm_percent = 0, .timeout_ms = 0 };
    position_id = request.request_id;
    position_done = false;
    e = position_service_move_async(&request);
    if (e != ESP_OK) position_id = 0;
    if (e == ESP_OK) e = wait_event(MACHINE_EVENT_POSITION_DONE, position_id, 31000);
    if (e == ESP_OK) {
        safety_position_view_t view;
        if (safety_manager_get_position_view(&view) != ESP_OK || !view.sample_valid ||
            !view.fresh || !view.stable || view.motor_moving || view.position != target)
            e = ESP_ERR_INVALID_RESPONSE;
    }
    return e;
}

static esp_err_t wait_motor_idle(void)
{
    int64_t until = now_ms() + 2000;
    while (now_ms() < until) {
        bl50_snapshot_t b;
        esp_err_t e = healthy(); if (e != ESP_OK) return e;
        if (bl50_service_get_snapshot(&b) == ESP_OK && b.state == BL50_SVC_STATE_IDLE &&
            b.current_pwm_percent == 0) return ESP_OK;
        pause_tick();
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t motor(bl50_action_t action, uint8_t pwm, uint32_t duration)
{
    esp_err_t e = healthy(); if (e != ESP_OK) return e;
    bl50_request_t request = { .request_id = child_id(), .action = action,
        .intensity = WASH_INTENSITY_GENTLE, .duration_ms = duration,
        .target_pwm_percent = pwm, .direction_interval_ms = 4000 };
    motor_id = request.request_id;
    motor_done = false;
    e = bl50_service_run_async(&request);
    if (e != ESP_OK) motor_id = 0;
    if (e == ESP_OK) e = wait_event(MACHINE_EVENT_BL50_DONE, motor_id,
                                   action == BL50_ACTION_HOME ? 17000 : duration + 7000);
    if (e == ESP_OK) e = wait_motor_idle();
    return e;
}

static esp_err_t dry(uint32_t duration_ms)
{
    esp_err_t e = safety_manager_demo_dry(owner_token.program_id, owner_token.generation, true);
    if (e != ESP_OK) return e;
    coupled_hot_air_request_t request = { .request_id = child_id(),
        .duration_ms = duration_ms, .competition_demo = true };
    heat_id = request.request_id;
    heat_done = false;
    e = coupled_hot_air_service_run_async(&request);
    if (e != ESP_OK) heat_id = 0;
    if (e != ESP_OK) return e;
    int64_t wait_until = now_ms() + 3000, until = 0;
    while (now_ms() < wait_until) {
        coupled_hot_air_snapshot_t h;
        e = healthy(); if (e != ESP_OK) return e;
        if (coupled_hot_air_service_get_snapshot(&h) == ESP_OK && h.request_id == heat_id) {
            if (h.output_confirmed_on) { until = h.deadline_ms; break; }
            if (h.terminal != CHA_TERMINAL_NONE) return ESP_FAIL;
        }
        pause_tick();
    }
    if (!until) return ESP_ERR_TIMEOUT;
    while (now_ms() < until) {
        /* Each 2s action includes ramps; service completion is required before OFF pause. */
        uint32_t remaining = (uint32_t)(until - now_ms());
        if (remaining < 500) break;
        e = motor(BL50_ACTION_TUMBLE, 40, remaining < 2000 ? remaining : 2000);
        if (e != ESP_OK) return e;
        coupled_hot_air_snapshot_t h;
        if (coupled_hot_air_service_get_snapshot(&h) != ESP_OK || h.request_id != heat_id ||
            (h.terminal != CHA_TERMINAL_NONE && h.terminal != CHA_TERMINAL_COMPLETE)) return ESP_FAIL;
        remaining = until > now_ms() ? (uint32_t)(until - now_ms()) : 0;
        if (remaining) { e = settle(remaining < 1000 ? remaining : 1000); if (e != ESP_OK) return e; }
    }
    wait_until = now_ms() + 3000;
    while (now_ms() < wait_until) {
        coupled_hot_air_snapshot_t h;
        e = healthy(); if (e != ESP_OK) return e;
        if (coupled_hot_air_service_get_snapshot(&h) == ESP_OK && h.request_id == heat_id &&
            h.output_confirmed_off && h.terminal == CHA_TERMINAL_COMPLETE) {
            return safety_manager_demo_dry(owner_token.program_id, owner_token.generation, false);
        }
        pause_tick();
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t execute_recipe(void)
{
    uint8_t index = 5;
    esp_err_t e;
    static const uint8_t pulsator_pwm[] = {20, 30, 40};
    static const uint8_t drum_pwm[] = {15, 20, 30};
    static const uint8_t spin_pwm[] = {40, 60, 80};
    for (unsigned i = 0; i < active_recipe.count; ++i) {
        const wash_recipe_action_t *a = &active_recipe.actions[i];
        if (a->op == RECIPE_WATER) continue;
        if (a->op == RECIPE_MOVE) {
            stage(++index, WASH_PHASE_MOVE_POSITION, (drum_position_t)a->position);
            e = move((drum_position_t)a->position);
        } else if (a->op == RECIPE_UV) {
            stage(++index, WASH_PHASE_UV, DRUM_POS_UNKNOWN);
            e = settle(a->seconds * 1000U);
        } else {
            drum_position_t target = a->op == RECIPE_PULSATOR ? DRUM_POS_0 :
                a->op == RECIPE_DRUM ? DRUM_POS_90 : a->op == RECIPE_SPIN ? DRUM_POS_180 : DRUM_POS_270;
            wash_phase_t phase = a->op == RECIPE_PULSATOR ? WASH_PHASE_PULSATOR_WASH :
                a->op == RECIPE_DRUM ? WASH_PHASE_DRUM_WASH : a->op == RECIPE_SPIN ? WASH_PHASE_SPIN : WASH_PHASE_DRY;
            stage(++index, WASH_PHASE_MOVE_POSITION, target);
            e = move(target); if (e != ESP_OK) return e;
            stage(++index, phase, target);
            if (a->op == RECIPE_DRY) e = dry(a->seconds * 1000U);
            else e = motor(a->op == RECIPE_PULSATOR ? BL50_ACTION_PULSATOR :
                a->op == RECIPE_DRUM ? BL50_ACTION_DRUM : BL50_ACTION_SPIN,
                a->op == RECIPE_PULSATOR ? pulsator_pwm[a->level - 1] :
                a->op == RECIPE_DRUM ? drum_pwm[a->level - 1] : spin_pwm[a->level - 1], a->seconds * 1000U);
            if (e != ESP_OK) return e;
            stage(++index, phase, target);
            e = settle(3000);
        }
        if (e != ESP_OK) return e;
    }
    stage(++index, WASH_PHASE_MOVE_POSITION, DRUM_POS_45);
    e = move(DRUM_POS_45); if (e != ESP_OK) return e;
    stage(++index, WASH_PHASE_MOVE_POSITION, DRUM_POS_45);
    e = COMPETITION_DEMO_SHAFT_HOME_REQUIRED ? motor(BL50_ACTION_HOME, 40, 0) : wait_motor_idle();
    if (e != ESP_OK) return e;
    stage(++index, WASH_PHASE_DONE, DRUM_POS_45);
    return ESP_OK;
}

static esp_err_t execute(void)
{
    esp_err_t e = safety_manager_demo_begin(owner_token.program_id, owner_token.generation);
    if (e != ESP_OK) return e;
    lease = true;
    if (!active_recipe.count || active_recipe.allow_uv) {
    uv_request_t uv = { .request_id = child_id(), .duration_ms = 180000, .competition_demo = true };
    e = uv_service_run_async(&uv); if (e != ESP_OK) return e;
    uv_id = uv.request_id;
    bool confirmed = false;
    int64_t until = now_ms() + 3000;
    while (now_ms() < until) {
        uv_snapshot_t u;
        if (atomic_load(&cancel_requested) || atomic_load(&shutdown_requested)) return ESP_ERR_INVALID_STATE;
        if (uv_service_get_snapshot(&u) == ESP_OK && u.request_id == uv.request_id && u.output_state == 2) {
            confirmed = true; break;
        }
        pause_tick();
    }
    /* Keep the ID even if startup failed, so cleanup cancels a late start. */
    if (!confirmed) return ESP_ERR_TIMEOUT;
    }
#define RUN(number, phase, target, call) do { stage(number, phase, target); e = (call); if (e != ESP_OK) return e; } while (0)
    RUN(1, WASH_PHASE_MOVE_POSITION, DRUM_POS_45, move(DRUM_POS_45));
    ESP_LOGI(TAG, "shaft HOME sensor required=%u; bucket position checks remain enabled",
             (unsigned)COMPETITION_DEMO_SHAFT_HOME_REQUIRED);
    if (COMPETITION_DEMO_SHAFT_HOME_REQUIRED) {
        RUN(2, WASH_PHASE_HOME_BL50, DRUM_POS_45, motor(BL50_ACTION_HOME, 40, 0));
    } else {
        RUN(2, WASH_PHASE_MOVE_POSITION, DRUM_POS_45, wait_motor_idle());
    }
    RUN(3, WASH_PHASE_MOVE_POSITION, DRUM_POS_0, move(DRUM_POS_0));
    RUN(4, WASH_PHASE_WATER_SOURCE_FILL, DRUM_POS_0, timed_water_dose(WATER_ACTUATOR_VALVE_SOURCE, false));
    RUN(5, WASH_PHASE_WATER_TRANSFER, DRUM_POS_0, timed_water_dose(WATER_ACTUATOR_VALVE_TRANSFER, true));
    if (active_recipe.count) return execute_recipe();
    RUN(6, WASH_PHASE_PULSATOR_WASH, DRUM_POS_0, motor(BL50_ACTION_PULSATOR, 40, 20000));
    RUN(7, WASH_PHASE_PULSATOR_WASH, DRUM_POS_0, settle(3000));
    RUN(8, WASH_PHASE_MOVE_POSITION, DRUM_POS_90, move(DRUM_POS_90));
    RUN(9, WASH_PHASE_DRUM_WASH, DRUM_POS_90, motor(BL50_ACTION_DRUM, 30, 24000));
    RUN(10, WASH_PHASE_DRUM_WASH, DRUM_POS_90, settle(3000));
    RUN(11, WASH_PHASE_MOVE_POSITION, DRUM_POS_180, move(DRUM_POS_180));
    RUN(12, WASH_PHASE_SPIN, DRUM_POS_180, motor(BL50_ACTION_SPIN, 80, 12000));
    RUN(13, WASH_PHASE_SPIN, DRUM_POS_180, settle(3000));
    RUN(14, WASH_PHASE_MOVE_POSITION, DRUM_POS_270, move(DRUM_POS_270));
    RUN(15, WASH_PHASE_DRY, DRUM_POS_270, dry(15000));
    RUN(16, WASH_PHASE_DRY, DRUM_POS_270, settle(3000));
    RUN(17, WASH_PHASE_MOVE_POSITION, DRUM_POS_45, move(DRUM_POS_45));
    if (COMPETITION_DEMO_SHAFT_HOME_REQUIRED) {
        RUN(18, WASH_PHASE_HOME_BL50, DRUM_POS_45, motor(BL50_ACTION_HOME, 40, 0));
    } else {
        RUN(18, WASH_PHASE_MOVE_POSITION, DRUM_POS_45, wait_motor_idle());
    }
    stage(19, WASH_PHASE_DONE, DRUM_POS_45);
#undef RUN
    return ESP_OK;
}

static bool cleanup(void)
{
    /* Normal cancels preserve restartability and never start a return move. */
    if (heat_id) (void)coupled_hot_air_service_cancel(heat_id);
    if (motor_id) (void)bl50_service_cancel(motor_id);
    if (position_id) (void)position_service_cancel(position_id);
    if (uv_id) (void)uv_service_cancel(uv_id);
    if (detergent_id) (void)detergent_service_cancel(detergent_id);
    if (valve_id) (void)water_service_actuator_valve_close(owned_valve);
    int64_t until = now_ms() + 3500;
    bool acknowledged = false;
    while (now_ms() < until) {
        machine_event_t event;
        while (xQueueReceive(events, &event, 0) == pdTRUE) remember_terminal(&event);
        acknowledged = (!position_id || position_done) && (!motor_id || motor_done) &&
            (!uv_id || uv_done) && (!heat_id || heat_done) &&
            (!detergent_id || detergent_done) && valve_closed();
        if (acknowledged && outputs_stopped()) break;
        pause_tick();
    }
    bool off = acknowledged && outputs_stopped();
    if (!off) {
        ESP_LOGE(TAG,"Output OFF confirmation failed; emergency shutdown required");
        (void)safety_manager_emergency_stop();
    }
    if (lease) (void)safety_manager_demo_end(owner_token.program_id, owner_token.generation);
    lease = false;
    return off;
}

static void worker(void *unused)
{
    (void)unused;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(lock, portMAX_DELAY);
        bool active = snapshot.active;
        xSemaphoreGive(lock);
        if (atomic_load(&shutdown_requested) && !active) break;
        position_id = motor_id = uv_id = heat_id = 0; lease = false;
        valve_id = detergent_id = 0; detergent_done = false;
        position_done = motor_done = uv_done = heat_done = false;
        failure_code = failure_detail = 0;
        esp_err_t result = atomic_load(&cancel_requested) ? ESP_ERR_INVALID_STATE : execute();
        bool off = cleanup();
        if (result == ESP_OK) {
            safety_snapshot_t safe; safety_position_view_t pos;
            if (safety_manager_get_snapshot(&safe) != ESP_OK || safe.fault_active ||
                safe.emergency_stop_active || safety_manager_get_position_view(&pos) != ESP_OK ||
                !pos.sample_valid || !pos.fresh || !pos.stable || pos.motor_moving ||
                pos.position != DRUM_POS_45) result = ESP_ERR_INVALID_RESPONSE;
        }
        xSemaphoreTake(lock, portMAX_DELAY);
        bool canceled = atomic_load(&cancel_requested) || atomic_load(&shutdown_requested);
        terminal_token_t terminal = canceled ? cancel_token : owner_token;
        snapshot.active = false;
        snapshot.completed = !canceled && result == ESP_OK && off;
        snapshot.error_code = snapshot.completed || (canceled && off) ? 0 :
            (failure_code ? failure_code : result == ESP_ERR_TIMEOUT ? FAULT_PROGRAM_TIMEOUT : FAULT_INTERNAL);
        snapshot.fault_detail = failure_detail;
        if (snapshot.error_code)
            ESP_LOGE(TAG, "demo failed: stage=%u phase=%u fault=%" PRIu32
                " detail=%" PRIu32 " esp_err=%s outputs_off=%u",
                snapshot.stage, (unsigned)snapshot.phase, snapshot.error_code,
                snapshot.fault_detail, esp_err_to_name(result), (unsigned)off);
        snapshot.elapsed_ms = (uint32_t)(now_ms() - started_ms);
        atomic_store(&job_reserved, false);
        uint32_t error = snapshot.error_code;
        competition_demo_terminal_fn notify = callback;
        void *context = callback_context;
        xSemaphoreGive(lock);
        notify(&terminal, off && (canceled || result == ESP_OK), error, context);
        if (atomic_load(&shutdown_requested)) break;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    task = NULL;
    xSemaphoreGive(lock);
    xSemaphoreGive(done);
    vTaskDeleteWithCaps(NULL);
}

esp_err_t competition_demo_submit_plan(const void *token, size_t bytes,
    const wash_recipe_t *recipe, competition_demo_terminal_fn terminal, void *context)
{
    if (!token || bytes != sizeof(terminal_token_t) || !terminal) return ESP_ERR_INVALID_ARG;
    uint32_t estimated = 176000;
    if (recipe && !wash_recipe_validate(recipe, &estimated)) return ESP_ERR_INVALID_ARG;
    if (!lock) {
        lock = xSemaphoreCreateMutexStatic(&lock_storage);
        done = xSemaphoreCreateBinaryStatic(&done_storage);
        events = xQueueCreateStatic(16, sizeof(machine_event_t), event_bytes, &event_storage);
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    if (snapshot.active) { xSemaphoreGive(lock); return ESP_ERR_INVALID_STATE; }
    if (!task && task_join_pending) {
        if (xSemaphoreTake(done, 0) != pdTRUE) { xSemaphoreGive(lock); return ESP_ERR_INVALID_STATE; }
        task_join_pending = false;
    }
    atomic_store(&shutdown_requested, false);
    atomic_store(&cancel_requested, false);
    atomic_store(&event_failure, false);
    owner_token = *(const terminal_token_t *)token; cancel_token = owner_token;
    cancel_token.terminal_kind = TERMINAL_KIND_CANCEL_ACK;
    callback = terminal; callback_context = context;
    started_ms = now_ms();
    memset(&active_recipe, 0, sizeof(active_recipe));
    if (recipe) active_recipe = *recipe;
    uint8_t stages = 19;
    if (recipe) {
        stages = 8;
        for (unsigned i = 0; i < recipe->count; ++i) {
            uint8_t op = recipe->actions[i].op;
            stages += op == RECIPE_WATER ? 0 : op == RECIPE_MOVE || op == RECIPE_UV ? 1 : 3;
        }
    }
    snapshot = (competition_demo_snapshot_t){ .active = true, .program_id = owner_token.program_id,
        .generation = owner_token.generation, .total_stages = stages, .estimated_total_ms = estimated,
        .target = DRUM_POS_UNKNOWN };
    xQueueReset(events);
    if (!task && xTaskCreateWithCaps(worker, "competition", 6144, NULL, 3, &task,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        snapshot.active = false; xSemaphoreGive(lock); return ESP_ERR_NO_MEM;
    }
    task_join_pending = true;
    atomic_store(&job_reserved, true);
    xTaskNotifyGive(task);
    xSemaphoreGive(lock);
    return ESP_OK;
}

esp_err_t competition_demo_submit(const void *token, size_t bytes,
    competition_demo_terminal_fn terminal, void *context)
{
    return competition_demo_submit_plan(token, bytes, NULL, terminal, context);
}

esp_err_t competition_demo_cancel(const void *token, size_t bytes)
{
    if (!lock || !token || bytes != sizeof(terminal_token_t)) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!same_owner(&owner_token, token)) { xSemaphoreGive(lock); return ESP_ERR_INVALID_STATE; }
    cancel_token = *(const terminal_token_t *)token;
    atomic_store(&cancel_requested, true);
    bool active = snapshot.active;
    xSemaphoreGive(lock);
    if (!active) callback(token, outputs_stopped(), 0, callback_context);
    return ESP_OK;
}

void competition_demo_abort(void) { atomic_store(&cancel_requested, true); }

esp_err_t competition_demo_shutdown(void)
{
    if (!lock) return ESP_OK;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!task_join_pending) { xSemaphoreGive(lock); return ESP_OK; }
    atomic_store(&shutdown_requested, true); atomic_store(&cancel_requested, true);
    if (task) xTaskNotifyGive(task);
    xSemaphoreGive(lock);
    if (xSemaphoreTake(done, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    xSemaphoreTake(lock, portMAX_DELAY);
    task_join_pending = false;
    xSemaphoreGive(lock);
    return ESP_OK;
}

void competition_demo_observe_event(const machine_event_t *event)
{
    if (events && event && atomic_load(&job_reserved) &&
        (event->type == MACHINE_EVENT_POSITION_DONE || event->type == MACHINE_EVENT_BL50_DONE ||
         event->type == MACHINE_EVENT_UV_DONE || event->type == MACHINE_EVENT_HOT_AIR_DONE ||
         event->type == MACHINE_EVENT_DETERGENT_DONE)) {
        if (xQueueSend(events, event, 0) != pdTRUE) {
            ESP_LOGE(TAG, "terminal queue full; canceling demonstration");
            atomic_store(&event_failure, true);
        }
    }
}

bool competition_demo_get_snapshot(competition_demo_snapshot_t *out)
{
    if (!lock || !out || xSemaphoreTake(lock, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    *out = snapshot;
    if (out->active) out->elapsed_ms = (uint32_t)(now_ms() - started_ms);
    xSemaphoreGive(lock);
    return out->program_id != 0;
}
