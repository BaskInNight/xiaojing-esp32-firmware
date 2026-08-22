/*
 * safety_manager.c — 安全授权网关 (Round 1.2)
 * 所有 MCP 安全输出的唯一业务写入口
 *
 * 生命周期模型:
 *   s_ready (_Atomic, acquire/release) 保护 API 入口
 *   s_mutex (静态分配, 永不删除) 保护运行时状态
 *   API 入口: ready_load → mutex → 再次 ready/state 检查 → 工作 → mutex 释放
 *   stop: mutex 内 ready=false + memset runtime → mutex 外 hooks/HAL
 *   start: mutex 内建立 MCP KNOWN_OFF 安全基线
 */

#include "safety_manager.h"
#include "safety_interlocks.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "machine_revision_next.h"
#include <string.h>
#include <stdatomic.h>

static const char *TAG = "safety_mgr";

#define SAFETY_MAX_FAULTS  4

/* ---- Static sync: never deleted, firmware lifetime ---- */

static StaticSemaphore_t s_mutex_static;
static SemaphoreHandle_t s_mutex = NULL;
static _Atomic bool s_ready = false;
static _Atomic safety_mgr_state_t s_state = SAFETY_MGR_UNINITIALIZED;
static bool s_mutex_created = false;

/* ---- Runtime context: cleared on stop/reinit ---- */

typedef struct {
    const xiaojing_hal_t *hal;
    safety_interlock_config_t ilk_config;
    safety_inputs_t inputs;
    safety_stop_hook_entry_t hooks[SAFETY_MAX_STOP_HOOKS];
    uint8_t hook_count;
    machine_fault_t faults[SAFETY_MAX_FAULTS];
    int fault_count;
    _Atomic bool emergency_active;
    uint32_t revision;
} safety_runtime_t;

static safety_runtime_t s_rt;

/* ---- Atomic helpers ---- */

static inline bool ready_load(void)
{
    return atomic_load_explicit(&s_ready, memory_order_acquire);
}

static inline void ready_store(bool desired)
{
    atomic_store_explicit(&s_ready, desired, memory_order_release);
}

static inline safety_mgr_state_t state_load(void)
{
    return atomic_load_explicit(&s_state, memory_order_relaxed);
}

static inline void state_store(safety_mgr_state_t desired)
{
    atomic_store_explicit(&s_state, desired, memory_order_relaxed);
}

#ifdef SAFETY_MANAGER_TEST_HOOKS
typedef struct {
    esp_err_t error;
    safety_stop_hook_t match_fn;
    void *match_ctx;
    bool active;
} test_fail_inject_t;

static test_fail_inject_t s_unregister_inject = {0};
#endif

/* ---- API lock helpers (double-check ready after mutex) ---- */

/* 获取 mutex 并确认 ready=true。ready=false 则直接返回错误。 */
static esp_err_t api_lock(void)
{
    if (!ready_load()) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!ready_load()) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

/* 获取 mutex 并确认 ready=true + state=RUNNING。 */
static esp_err_t api_lock_running(void)
{
    if (!ready_load()) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!ready_load()) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (state_load() != SAFETY_MGR_RUNNING) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static inline void api_unlock(void)
{
    xSemaphoreGive(s_mutex);
}

/* ---- Fault helpers ---- */

static void latch_fault(machine_fault_code_t code, fault_severity_t severity,
                          uint32_t detail)
{
    if (code == FAULT_NONE) return;

    for (int i = 0; i < s_rt.fault_count; i++) {
        if (s_rt.faults[i].code == code) {
            s_rt.faults[i].detail = detail;
            if (s_rt.hal && s_rt.hal->now_ms) {
                s_rt.faults[i].timestamp_ms = s_rt.hal->now_ms();
            }
            return;
        }
    }

    if (s_rt.fault_count < SAFETY_MAX_FAULTS) {
        machine_fault_t *f = &s_rt.faults[s_rt.fault_count++];
        f->code = code;
        f->severity = severity;
        f->detail = detail;
        f->timestamp_ms = (s_rt.hal && s_rt.hal->now_ms) ?
                           s_rt.hal->now_ms() : 0;
    }

    s_rt.revision = machine_revision_next(s_rt.revision);
}

static bool has_active_fault(void)
{
    return s_rt.fault_count > 0;
}

static machine_fault_t get_primary_fault(void)
{
    if (s_rt.fault_count == 0) {
        machine_fault_t empty = {0};
        return empty;
    }
    return s_rt.faults[0];
}

static uint64_t get_active_fault_mask(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < s_rt.fault_count; i++) {
        int bit = (int)s_rt.faults[i].code;
        if (bit >= 0 && bit <= 63) {
            mask |= (1ULL << bit);
        }
    }
    return mask;
}

static bool can_clear_faults(void)
{
    const safety_inputs_t *in = &s_rt.inputs;

    /* Temperature at or above cutoff — definitely cannot clear */
    if (in->sht_valid && in->sht_fresh &&
        in->sht_sample.temperature_c >= s_rt.ilk_config.heater_cutoff_c) {
        return false;
    }

    /* Overtemp fault latched — must wait until temp drops below resume */
    if (in->sht_valid && in->sht_fresh) {
        for (int i = 0; i < s_rt.fault_count; i++) {
            if (s_rt.faults[i].code == FAULT_DRY_OVERTEMP &&
                in->sht_sample.temperature_c >= s_rt.ilk_config.heater_resume_c) {
                return false;
            }
        }
    }

    if (!in->sht_valid) {
        for (int i = 0; i < s_rt.fault_count; i++) {
            if (s_rt.faults[i].code == FAULT_SHT_INVALID ||
                s_rt.faults[i].code == FAULT_DRY_OVERTEMP) {
                return false;
            }
        }
    }

    /* All MCP outputs must be KNOWN_OFF */
    if (!safety_all_mcp_known_off(&in->mcp_outputs, in->mcp_known_mask)) {
        return false;
    }
    if (in->mcp_outputs.ptc_heater || in->mcp_outputs.uv) {
        return false;
    }

    if (in->position_motor_moving) {
        return false;
    }

    return true;
}

/* ---- MCP helpers ---- */

static safe_output_t op_to_safe_output(safety_operation_t op)
{
    switch (op) {
    case SAFETY_OP_SOURCE_INLET_VALVE: return SAFE_OUTPUT_TAP_VALVE;
    case SAFETY_OP_TRANSFER_VALVE:     return SAFE_OUTPUT_TRANSFER_VALVE;
    case SAFETY_OP_UV:                 return SAFE_OUTPUT_UV;
    case SAFETY_OP_DRAIN_VALVE:        return SAFE_OUTPUT_DRAIN_VALVE;
    case SAFETY_OP_PTC_HEATER:         return SAFE_OUTPUT_PTC_HEATER;
    case SAFETY_OP_HOT_AIR_MODULE:     return SAFE_OUTPUT_PTC_HEATER;
    case SAFETY_OP_DETERGENT_PUMP:     return SAFE_OUTPUT_DETERGENT_PUMP;
    default:                           return SAFE_OUTPUT_TAP_VALVE;
    }
}

static void mark_mcp_known(safe_output_t output)
{
    s_rt.inputs.mcp_known_mask |= (1U << (int)output);
}

static void mark_mcp_unknown(safe_output_t output)
{
    s_rt.inputs.mcp_known_mask &= ~(1U << (int)output);
}

static void update_mcp_shadow(safety_operation_t op, bool enable)
{
    switch (op) {
    case SAFETY_OP_SOURCE_INLET_VALVE:
        s_rt.inputs.mcp_outputs.source_inlet_valve = enable;
        mark_mcp_known(SAFE_OUTPUT_TAP_VALVE);
        break;
    case SAFETY_OP_TRANSFER_VALVE:
        s_rt.inputs.mcp_outputs.transfer_valve = enable;
        mark_mcp_known(SAFE_OUTPUT_TRANSFER_VALVE);
        break;
    case SAFETY_OP_UV:
        s_rt.inputs.mcp_outputs.uv = enable;
        mark_mcp_known(SAFE_OUTPUT_UV);
        break;
    case SAFETY_OP_DRAIN_VALVE:
        s_rt.inputs.mcp_outputs.drain_valve = enable;
        mark_mcp_known(SAFE_OUTPUT_DRAIN_VALVE);
        break;
    case SAFETY_OP_PTC_HEATER:
        s_rt.inputs.mcp_outputs.ptc_heater = enable;
        mark_mcp_known(SAFE_OUTPUT_PTC_HEATER);
        if (enable) {
            s_rt.inputs.ptc_on = true;
            s_rt.inputs.ptc_on_since_ms = s_rt.hal->now_ms();
        } else {
            s_rt.inputs.ptc_on = false;
            s_rt.inputs.ptc_on_since_ms = 0;
        }
        break;
    case SAFETY_OP_HOT_AIR_MODULE:
        s_rt.inputs.mcp_outputs.ptc_heater = enable;
        mark_mcp_known(SAFE_OUTPUT_PTC_HEATER);
        s_rt.inputs.hot_air_on = enable;
        if (enable) {
            s_rt.inputs.hot_air_on_since_ms = s_rt.hal->now_ms();
        } else {
            s_rt.inputs.hot_air_on_since_ms = 0;
            s_rt.inputs.ptc_on = false;   /* 与 PTC 共享同一继电器：互斥 */
            /* 每次 OFF 启动冷却锁（自动/取消/紧急/故障关断统一路径）。 */
            s_rt.inputs.hot_air_cooldown_until_ms =
                s_rt.hal->now_ms() + (int64_t)s_rt.ilk_config.hot_air_cooldown_ms;
        }
        break;
    case SAFETY_OP_DETERGENT_PUMP:
        s_rt.inputs.mcp_outputs.detergent_pump = enable;
        mark_mcp_known(SAFE_OUTPUT_DETERGENT_PUMP);
        break;
    default: break;
    }
}

static esp_err_t apply_mcp_output(safety_operation_t op, bool enable)
{
    if (!s_rt.hal || !s_rt.hal->set_safe_output) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = s_rt.hal->set_safe_output(op_to_safe_output(op), enable);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MCP write failed op=%d err=0x%x", op, err);
        mark_mcp_unknown(op_to_safe_output(op));
        latch_fault(FAULT_MCP_IO, FAULT_SEVERITY_LATCHED, (uint32_t)err);
        return err;
    }

    update_mcp_shadow(op, enable);
    return ESP_OK;
}

static esp_err_t apply_fan(uint8_t percent)
{
    if (!s_rt.hal || !s_rt.hal->set_fan_percent) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = s_rt.hal->set_fan_percent(percent);
    if (err == ESP_OK) {
        s_rt.inputs.fan_percent = percent;
        if (percent > 0) {
            if (!s_rt.inputs.fan_running) {
                s_rt.inputs.fan_running = true;
                s_rt.inputs.fan_running_since_ms = s_rt.hal->now_ms();
            }
        } else {
            s_rt.inputs.fan_running = false;
            s_rt.inputs.fan_running_since_ms = 0;
        }
    }
    return err;
}

static esp_err_t apply_bl50(const safety_request_t *req)
{
    if (!s_rt.hal) return ESP_ERR_INVALID_STATE;

    if (!req->enable) {
        if (s_rt.hal->stop_bl50) {
            esp_err_t err = s_rt.hal->stop_bl50();
            if (err == ESP_OK) {
                /* STOP 成功: BL50 状态 = STOPPED */
                s_rt.inputs.bl50_state_known = true;
                s_rt.inputs.bl50_running = false;
            } else {
                /* STOP 失败: BL50 状态 = UNKNOWN */
                s_rt.inputs.bl50_state_known = false;
            }
            return err;
        }
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (s_rt.hal->set_bl50) {
        bl50_command_t cmd = {
            .command = req->params.motor.clockwise ? BL50_CMD_RUN_CW : BL50_CMD_RUN_CCW,
            .pwm_percent = req->params.motor.pwm_percent,
        };
        esp_err_t err = s_rt.hal->set_bl50(cmd);
        if (err == ESP_OK) {
            /* RUN 成功: BL50 状态 = RUNNING */
            s_rt.inputs.bl50_state_known = true;
            s_rt.inputs.bl50_running = true;
        } else {
            /* RUN 失败: BL50 状态 = UNKNOWN */
            s_rt.inputs.bl50_state_known = false;
        }
        return err;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

/* ---- MCP Safe Baseline (called from start, under mutex) ---- */

static esp_err_t establish_mcp_safe_baseline_locked(void)
{
    static const safe_output_t all_outputs[] = {
        SAFE_OUTPUT_TAP_VALVE,
        SAFE_OUTPUT_TRANSFER_VALVE,
        SAFE_OUTPUT_UV,
        SAFE_OUTPUT_DRAIN_VALVE,
        SAFE_OUTPUT_PTC_HEATER,
        SAFE_OUTPUT_DETERGENT_PUMP,
    };

    esp_err_t first_err = ESP_OK;

    for (size_t i = 0; i < sizeof(all_outputs) / sizeof(all_outputs[0]); i++) {
        if (!s_rt.hal || !s_rt.hal->set_safe_output) {
            mark_mcp_unknown(all_outputs[i]);
            if (first_err == ESP_OK) first_err = ESP_ERR_INVALID_STATE;
            continue;
        }
        esp_err_t err = s_rt.hal->set_safe_output(all_outputs[i], false);
        if (err == ESP_OK) {
            mark_mcp_known(all_outputs[i]);
        } else {
            mark_mcp_unknown(all_outputs[i]);
            latch_fault(FAULT_MCP_IO, FAULT_SEVERITY_LATCHED, (uint32_t)err);
            if (first_err == ESP_OK) first_err = err;
        }
    }

    return first_err;
}

/* ================================================================
 * Lifecycle (init/start/stop 要求调用者串行)
 * ================================================================ */

esp_err_t safety_manager_init(const safety_manager_config_t *config)
{
    if (!config || !config->hal) {
        return ESP_ERR_INVALID_ARG;
    }

    if (ready_load()) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Create static mutex once (firmware lifetime, never deleted) */
    if (!s_mutex_created) {
        s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_static);
        if (!s_mutex) {
            ESP_LOGE(TAG, "Failed to create static mutex");
            return ESP_ERR_NO_MEM;
        }
        s_mutex_created = true;
    }

    /* Clear runtime context */
    memset(&s_rt, 0, sizeof(s_rt));
    s_rt.revision = 1;

    s_rt.hal = config->hal;

    if (config->config) {
        s_rt.ilk_config.heater_cutoff_c = config->config->dry.heater_cutoff_c;
        s_rt.ilk_config.heater_resume_c = config->config->dry.heater_resume_c;
        s_rt.ilk_config.heat_on_max_ms = config->config->dry.heat_on_max_ms;
        s_rt.ilk_config.pre_fan_ms = config->config->dry.pre_fan_ms;
        s_rt.ilk_config.sht_stale_timeout_ms = config->config->dry.sht_stale_timeout_ms;
    } else {
        s_rt.ilk_config.heater_cutoff_c = 55.0f;
        s_rt.ilk_config.heater_resume_c = 45.0f;
        s_rt.ilk_config.heat_on_max_ms = 60000;
        s_rt.ilk_config.pre_fan_ms = 5000;
        s_rt.ilk_config.sht_stale_timeout_ms = 10000;
    }
    s_rt.ilk_config.position_stale_timeout_ms = 500;

    /* PTC config from board_config (explicit, not inferred from config!=NULL) */
    s_rt.ilk_config.output_mode = config->output_mode;
    s_rt.ilk_config.ptc_enabled = config->ptc_enabled;
    s_rt.ilk_config.hot_air_module_enabled = config->hot_air_module_enabled;
    /* 冷却锁：默认 30s，可经 config 覆盖。0 = 不锁定（测试用）。 */
    s_rt.ilk_config.hot_air_cooldown_ms = config->hot_air_cooldown_ms;
    s_rt.ilk_config.board_identity_confirmed = config->board_identity_confirmed;

    /* Default safe inputs — all UNKNOWN until baseline established */
    s_rt.inputs.position = DRUM_POS_UNKNOWN;
    s_rt.inputs.position_sample_valid = false;
    s_rt.inputs.sht_valid = false;
    s_rt.inputs.sht_fresh = false;
    s_rt.inputs.ptc_enabled_in_config = config->ptc_enabled;
    s_rt.inputs.output_mode = config->output_mode;
    s_rt.inputs.mcp_known_mask = 0;

    state_store(SAFETY_MGR_INITIALIZED);
    ready_store(true);

    ESP_LOGI(TAG, "Initialized (mode=%d ptc=%d board=%d)",
             config->output_mode, config->ptc_enabled,
             config->board_identity_confirmed);
    return ESP_OK;
}

esp_err_t safety_manager_start(void)
{
    esp_err_t lock_err = api_lock();
    if (lock_err != ESP_OK) return lock_err;

    if (state_load() != SAFETY_MGR_INITIALIZED) {
        api_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    /* Establish MCP KNOWN_OFF baseline */
    esp_err_t baseline_err = establish_mcp_safe_baseline_locked();
    if (baseline_err != ESP_OK) {
        ESP_LOGE(TAG, "MCP baseline failed, entering FAILED state");
        state_store(SAFETY_MGR_FAILED);
        latch_fault(FAULT_MCP_IO, FAULT_SEVERITY_LATCHED, 0);
        s_rt.inputs.fault_active = true;
        api_unlock();
        return baseline_err;
    }

    state_store(SAFETY_MGR_RUNNING);
    api_unlock();

    ESP_LOGI(TAG, "Running (MCP baseline OK)");
    return ESP_OK;
}

esp_err_t safety_manager_stop(void)
{
    if (!ready_load()) return ESP_OK;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (state_load() == SAFETY_MGR_STOPPED) {
        api_unlock();
        return ESP_OK;
    }

    /* STOPPING visible during hooks/HAL execution */
    state_store(SAFETY_MGR_STOPPING);

    /* Snapshot hooks/HAL under mutex */
    uint8_t hook_count = s_rt.hook_count;
    safety_stop_hook_entry_t hooks_copy[SAFETY_MAX_STOP_HOOKS];
    memcpy(hooks_copy, s_rt.hooks, hook_count * sizeof(hooks_copy[0]));
    const xiaojing_hal_t *hal = s_rt.hal;

    /* Clear ready + runtime INSIDE mutex */
    ready_store(false);
    memset(&s_rt, 0, sizeof(s_rt));

    api_unlock();

    /* Call hooks outside mutex — get_state returns STOPPING during this */
    for (uint8_t i = 0; i < hook_count; i++) {
        if (hooks_copy[i].fn) {
            ESP_LOGI(TAG, "Stop hook '%s'", hooks_copy[i].name ? hooks_copy[i].name : "?");
            hooks_copy[i].fn(hooks_copy[i].context);
        }
    }

    if (hal && hal->emergency_shutdown) {
        hal->emergency_shutdown();
    }

    /* STOPPED after hooks/HAL complete */
    state_store(SAFETY_MGR_STOPPED);

    ESP_LOGI(TAG, "Stopped");
    return ESP_OK;
}

safety_mgr_state_t safety_manager_get_state(void)
{
    safety_mgr_state_t s = state_load();
    if (s == SAFETY_MGR_STOPPING || s == SAFETY_MGR_STOPPED) {
        return s;
    }
    if (!ready_load()) return SAFETY_MGR_UNINITIALIZED;
    return s;
}

/* ================================================================
 * Stop Hook
 * ================================================================ */

esp_err_t safety_manager_register_stop_hook(safety_stop_hook_t fn,
                                              void *context,
                                              const char *name)
{
    if (!fn) return ESP_ERR_INVALID_ARG;
    esp_err_t lock_err = api_lock();
    if (lock_err != ESP_OK) return lock_err;

    if (s_rt.hook_count >= SAFETY_MAX_STOP_HOOKS) {
        api_unlock();
        return ESP_ERR_NO_MEM;
    }

    s_rt.hooks[s_rt.hook_count].fn = fn;
    s_rt.hooks[s_rt.hook_count].context = context;
    s_rt.hooks[s_rt.hook_count].name = name;
    s_rt.hook_count++;

    api_unlock();
    return ESP_OK;
}

esp_err_t safety_manager_unregister_stop_hook(safety_stop_hook_t fn,
                                               void *context)
{
    if (!fn) return ESP_ERR_INVALID_ARG;
    esp_err_t lock_err = api_lock();
    /* Manager not ready (not initialized/stopped/reset): no hooks exist.
     * Treat unregister as success so callers can proceed with cleanup. */
    if (lock_err != ESP_OK) return ESP_OK;

    for (uint8_t i = 0; i < s_rt.hook_count; i++) {
        if (s_rt.hooks[i].fn == fn && s_rt.hooks[i].context == context) {
#ifdef SAFETY_MANAGER_TEST_HOOKS
            /* Check for injected unregister failure */
            if (s_unregister_inject.active &&
                (s_unregister_inject.match_fn == NULL || s_unregister_inject.match_fn == fn) &&
                (s_unregister_inject.match_ctx == NULL || s_unregister_inject.match_ctx == context)) {
                esp_err_t injected = s_unregister_inject.error;
                s_unregister_inject.active = false; /* one-shot */
                api_unlock();
                return injected;
            }
#endif
            /* Swap with last entry and decrement count */
            if (i < s_rt.hook_count - 1) {
                s_rt.hooks[i] = s_rt.hooks[s_rt.hook_count - 1];
            }
            s_rt.hook_count--;
            api_unlock();
            return ESP_OK;
        }
    }

    api_unlock();
    return ESP_ERR_NOT_FOUND;
}

/* ================================================================
 * Typed Input Updates
 * ================================================================ */

esp_err_t safety_manager_update_position(drum_position_t position,
                                          bool stable,
                                          bool motor_moving,
                                          bool sample_valid,
                                          uint32_t sample_ms)
{
    esp_err_t lock_err = api_lock();
    if (lock_err != ESP_OK) return lock_err;
    s_rt.inputs.position = position;
    s_rt.inputs.position_stable = stable;
    s_rt.inputs.position_motor_moving = motor_moving;
    s_rt.inputs.position_sample_valid = sample_valid;
    s_rt.inputs.position_sample_ms = sample_ms;
    api_unlock();
    return ESP_OK;
}

esp_err_t safety_manager_update_sht(const sht_sample_t *sample,
                                     bool valid, bool fresh)
{
    if (!sample) return ESP_ERR_INVALID_ARG;
    esp_err_t lock_err = api_lock();
    if (lock_err != ESP_OK) return lock_err;
    s_rt.inputs.sht_sample = *sample;
    s_rt.inputs.sht_valid = valid;
    s_rt.inputs.sht_fresh = fresh;
    api_unlock();
    return ESP_OK;
}

esp_err_t safety_manager_update_water(bool full, bool valid)
{
    esp_err_t lock_err = api_lock();
    if (lock_err != ESP_OK) return lock_err;
    s_rt.inputs.water_full = full;
    s_rt.inputs.water_level_valid = valid;
    api_unlock();
    return ESP_OK;
}

esp_err_t safety_manager_update_fan(uint8_t percent, bool running,
                                     int64_t running_since_ms)
{
    esp_err_t lock_err = api_lock();
    if (lock_err != ESP_OK) return lock_err;
    s_rt.inputs.fan_percent = percent;
    s_rt.inputs.fan_running = running;
    s_rt.inputs.fan_running_since_ms = running_since_ms;
    api_unlock();
    return ESP_OK;
}

esp_err_t safety_manager_update_time(int64_t now_ms)
{
    esp_err_t lock_err = api_lock();
    if (lock_err != ESP_OK) return lock_err;
    s_rt.inputs.now_ms = now_ms;
    api_unlock();
    return ESP_OK;
}

/* ================================================================
 * Check (query only, no side effects)
 * ================================================================ */

esp_err_t safety_manager_check(const safety_request_t *request,
                                safety_decision_t *out_decision)
{
    if (!request || !out_decision) return ESP_ERR_INVALID_ARG;
    esp_err_t lock_err = api_lock_running();
    if (lock_err != ESP_OK) {
        if (lock_err == ESP_ERR_INVALID_STATE && ready_load()) {
            out_decision->allowed = false;
            out_decision->reason = SAFETY_REJECT_NOT_INITIALIZED;
            out_decision->fault_code = FAULT_NONE;
            out_decision->interlock_mask = SAFETY_ILK_NOT_INITIALIZED;
        }
        return lock_err;
    }

    /* Snapshot inputs AND config under lock */
    safety_inputs_t snap = s_rt.inputs;
    safety_interlock_config_t config_snap = s_rt.ilk_config;
    if (s_rt.hal && s_rt.hal->now_ms) {
        snap.now_ms = s_rt.hal->now_ms();
    }
    snap.fault_active = has_active_fault();
    snap.emergency_stop_active = s_rt.emergency_active;

    api_unlock();

    /* Pure interlock check — only local copies */
    safety_interlock_check(&snap, &config_snap, request, out_decision);
    return ESP_OK;
}

esp_err_t safety_manager_validate_active(safety_operation_t operation,
                                          drum_position_t required_position,
                                          bool *allowed,
                                          safety_reject_reason_t *reason)
{
    if (!allowed || !reason) return ESP_ERR_INVALID_ARG;
    esp_err_t lock_err = api_lock_running();
    if (lock_err != ESP_OK) {
        *allowed = false;
        *reason = SAFETY_REJECT_NOT_INITIALIZED;
        return lock_err;
    }

    safety_inputs_t snap = s_rt.inputs;
    safety_interlock_config_t config_snap = s_rt.ilk_config;
    if (s_rt.hal && s_rt.hal->now_ms) {
        snap.now_ms = s_rt.hal->now_ms();
    }
    snap.fault_active = has_active_fault();
    snap.emergency_stop_active = s_rt.emergency_active;

    api_unlock();

    *allowed = safety_validate_active(&snap, &config_snap,
                                       operation, required_position, reason);
    return ESP_OK;
}

/* ================================================================
 * Apply (check + execute, atomic under mutex)
 * ================================================================ */

esp_err_t safety_manager_apply(const safety_request_t *request)
{
    if (!request) return ESP_ERR_INVALID_ARG;
    esp_err_t lock_err = api_lock_running();
    if (lock_err != ESP_OK) return lock_err;

    /* OFF requests: always attempt */
    if (!request->enable) {
        esp_err_t err = ESP_OK;
        safety_operation_t op = request->operation;

        if (op >= SAFETY_OP_COUNT) {
            api_unlock();
            return ESP_ERR_INVALID_ARG;
        }

        if (safety_is_mcp_output_op(op)) {
            err = apply_mcp_output(op, false);
        } else if (op == SAFETY_OP_FAN) {
            err = apply_fan(0);
        } else if (op == SAFETY_OP_BL50) {
            err = apply_bl50(request);
        } else if (op == SAFETY_OP_POSITION_MOTOR) {
            /* Motor stop delegated to position_service */
        }

        api_unlock();
        return err;
    }

    /* ON requests: interlock check under lock */
    safety_inputs_t snap = s_rt.inputs;
    safety_interlock_config_t config_snap = s_rt.ilk_config;
    if (s_rt.hal && s_rt.hal->now_ms) {
        snap.now_ms = s_rt.hal->now_ms();
    }
    snap.fault_active = has_active_fault();
    snap.emergency_stop_active = s_rt.emergency_active;

    safety_decision_t decision;
    safety_interlock_check(&snap, &config_snap, request, &decision);

    if (!decision.allowed) {
        if (decision.fault_code != FAULT_NONE) {
            latch_fault(decision.fault_code, FAULT_SEVERITY_LATCHED, 0);
        }
        api_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    /* Execute under lock */
    esp_err_t err = ESP_OK;
    safety_operation_t op = request->operation;

    if (safety_is_mcp_output_op(op)) {
        err = apply_mcp_output(op, true);
    } else if (op == SAFETY_OP_FAN) {
        err = apply_fan(request->params.fan.percent);
    } else if (op == SAFETY_OP_BL50) {
        err = apply_bl50(request);
    } else if (op == SAFETY_OP_POSITION_MOTOR) {
        /* Motor control delegated to position_service */
    }

    if (err != ESP_OK) {
        api_unlock();
        return err;
    }

    s_rt.revision = machine_revision_next(s_rt.revision);
    api_unlock();
    return ESP_OK;
}

/* ================================================================
 * Fault Management
 * ================================================================ */

esp_err_t safety_manager_report_fault(machine_fault_code_t code,
                                       fault_severity_t severity,
                                       uint32_t detail)
{
    if (code == FAULT_NONE) return ESP_ERR_INVALID_ARG;
    esp_err_t lock_err = api_lock();
    if (lock_err != ESP_OK) return lock_err;
    latch_fault(code, severity, detail);
    s_rt.inputs.fault_active = true;
    api_unlock();
    return ESP_OK;
}

esp_err_t safety_manager_request_fault_clear(void)
{
    esp_err_t lock_err = api_lock();
    if (lock_err != ESP_OK) return lock_err;

    if (!has_active_fault()) {
        api_unlock();
        return ESP_OK;
    }

    if (s_rt.hal && s_rt.hal->now_ms) {
        s_rt.inputs.now_ms = s_rt.hal->now_ms();
    }

    if (!can_clear_faults()) {
        api_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    s_rt.fault_count = 0;
    memset(s_rt.faults, 0, sizeof(s_rt.faults));
    s_rt.inputs.fault_active = false;
    s_rt.revision = machine_revision_next(s_rt.revision);

    api_unlock();
    return ESP_OK;
}

/* ================================================================
 * Emergency Stop
 * ================================================================ */

esp_err_t safety_manager_emergency_stop(void)
{
    esp_err_t lock_err = api_lock();
    if (lock_err != ESP_OK) return lock_err;

    if (s_rt.emergency_active) {
        api_unlock();
        return ESP_OK;
    }

    s_rt.emergency_active = true;
    s_rt.inputs.emergency_stop_active = true;

    esp_err_t first_err = ESP_OK;

    /* 1. PTC OFF — update shadow only on success */
    if (s_rt.hal && s_rt.hal->set_safe_output) {
        esp_err_t err = s_rt.hal->set_safe_output(SAFE_OUTPUT_PTC_HEATER, false);
        if (err != ESP_OK) {
            if (first_err == ESP_OK) first_err = err;
            mark_mcp_unknown(SAFE_OUTPUT_PTC_HEATER);
        } else {
            s_rt.inputs.mcp_outputs.ptc_heater = false;
            s_rt.inputs.ptc_on = false;
            s_rt.inputs.ptc_on_since_ms = 0;
            /* 紧急关断后热风模块同样进入冷却锁（若使能）。 */
            s_rt.inputs.hot_air_on = false;
            s_rt.inputs.hot_air_on_since_ms = 0;
            s_rt.inputs.hot_air_cooldown_until_ms =
                s_rt.hal->now_ms() + (int64_t)s_rt.ilk_config.hot_air_cooldown_ms;
            mark_mcp_known(SAFE_OUTPUT_PTC_HEATER);
        }
    }

    /* 2. BL50 STOP — track state based on result */
    if (s_rt.hal && s_rt.hal->stop_bl50) {
        esp_err_t err = s_rt.hal->stop_bl50();
        if (err != ESP_OK) {
            if (first_err == ESP_OK) first_err = err;
            /* STOP 失败: 状态 = UNKNOWN */
            s_rt.inputs.bl50_state_known = false;
        } else {
            /* STOP 成功: 状态 = STOPPED */
            s_rt.inputs.bl50_state_known = true;
            s_rt.inputs.bl50_running = false;
        }
    }

    /* 3. Close all MCP outputs — per-output tracking */
    if (s_rt.hal && s_rt.hal->set_safe_output) {
        static const struct { safe_output_t output; safety_operation_t op; } mcp_list[] = {
            { SAFE_OUTPUT_TAP_VALVE,     SAFETY_OP_SOURCE_INLET_VALVE },
            { SAFE_OUTPUT_TRANSFER_VALVE, SAFETY_OP_TRANSFER_VALVE },
            { SAFE_OUTPUT_UV,            SAFETY_OP_UV },
            { SAFE_OUTPUT_DRAIN_VALVE,   SAFETY_OP_DRAIN_VALVE },
            { SAFE_OUTPUT_DETERGENT_PUMP, SAFETY_OP_DETERGENT_PUMP },
        };
        for (size_t i = 0; i < sizeof(mcp_list) / sizeof(mcp_list[0]); i++) {
            esp_err_t err = s_rt.hal->set_safe_output(mcp_list[i].output, false);
            if (err != ESP_OK) {
                if (first_err == ESP_OK) first_err = err;
                mark_mcp_unknown(mcp_list[i].output);
            } else {
                update_mcp_shadow(mcp_list[i].op, false);
            }
        }
    }

    /* 4. Latch FAULT_MCP_IO if any output is unknown */
    uint32_t all_mcp_mask = ((1U << SAFE_OUTPUT_COUNT) - 1);
    if ((s_rt.inputs.mcp_known_mask & all_mcp_mask) != all_mcp_mask) {
        latch_fault(FAULT_MCP_IO, FAULT_SEVERITY_LATCHED, 0);
    }

    /* 5. Latch emergency fault */
    latch_fault(FAULT_INTERNAL, FAULT_SEVERITY_LATCHED, 0);
    s_rt.inputs.fault_active = true;
    s_rt.revision = machine_revision_next(s_rt.revision);

    /* Snapshot hooks for iteration outside mutex */
    uint8_t hook_count = s_rt.hook_count;
    safety_stop_hook_entry_t hooks_copy[SAFETY_MAX_STOP_HOOKS];
    memcpy(hooks_copy, s_rt.hooks, hook_count * sizeof(hooks_copy[0]));

    const xiaojing_hal_t *hal = s_rt.hal;
    api_unlock();

    /* 6. Call hooks outside mutex */
    for (uint8_t i = 0; i < hook_count; i++) {
        if (hooks_copy[i].fn) {
            esp_err_t hook_err = hooks_copy[i].fn(hooks_copy[i].context);
            if (hook_err != ESP_OK && first_err == ESP_OK) first_err = hook_err;
        }
    }

    /* 7. HAL emergency_shutdown as final fallback */
    if (hal && hal->emergency_shutdown) {
        esp_err_t err = hal->emergency_shutdown();
        if (err != ESP_OK && first_err == ESP_OK) first_err = err;
    }

    return first_err;
}

/* ================================================================
 * Snapshot
 * ================================================================ */

esp_err_t safety_manager_get_snapshot(safety_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    esp_err_t lock_err = api_lock();
    if (lock_err != ESP_OK) {
        memset(out, 0, sizeof(*out));
        return lock_err;
    }

    out->fault_active = has_active_fault();
    out->primary_fault = get_primary_fault();
    out->active_fault_mask = get_active_fault_mask();
    out->emergency_stop_active = s_rt.emergency_active;
    out->revision = s_rt.revision;
    out->mcp_known_mask = s_rt.inputs.mcp_known_mask;
    out->mcp_unknown_mask = ((1U << SAFE_OUTPUT_COUNT) - 1) & ~s_rt.inputs.mcp_known_mask;
    out->mcp_outputs = s_rt.inputs.mcp_outputs;
    out->bl50_state_known = s_rt.inputs.bl50_state_known;
    out->bl50_running = s_rt.inputs.bl50_running;
    out->hot_air_on = s_rt.inputs.hot_air_on;
    out->hot_air_cooldown_until_ms = s_rt.inputs.hot_air_cooldown_until_ms;

    safety_inputs_t snap = s_rt.inputs;
    safety_interlock_config_t config_snap = s_rt.ilk_config;
    if (s_rt.hal && s_rt.hal->now_ms) {
        snap.now_ms = s_rt.hal->now_ms();
    }
    snap.fault_active = out->fault_active;
    snap.emergency_stop_active = out->emergency_stop_active;

    out->allowed_operation_mask = safety_compute_allowed_mask(&snap, &config_snap);
    out->water_outputs_safe = safety_water_outputs_safe(&snap.mcp_outputs,
                                                         snap.mcp_known_mask);
    out->heater_safe = safety_heater_safe(&snap.mcp_outputs,
                                           snap.mcp_known_mask,
                                           &snap, &config_snap);

    api_unlock();
    return ESP_OK;
}

esp_err_t safety_manager_get_position_view(safety_position_view_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    esp_err_t lock_err = api_lock();
    if (lock_err != ESP_OK) {
        out->position = DRUM_POS_UNKNOWN;
        out->sample_valid = false;
        return lock_err;
    }

    int64_t now = 0;
    if (s_rt.hal && s_rt.hal->now_ms) {
        now = s_rt.hal->now_ms();
    }

    out->position = s_rt.inputs.position;
    out->sample_valid = s_rt.inputs.position_sample_valid;
    out->stable = s_rt.inputs.position_stable;
    out->motor_moving = s_rt.inputs.position_motor_moving;
    out->sample_ms = s_rt.inputs.position_sample_ms;
    out->now_ms = now;

    /* Compute freshness — consistent with safety_interlocks.c rules */
    if (!s_rt.inputs.position_sample_valid) {
        out->fresh = false;
    } else if (s_rt.ilk_config.position_stale_timeout_ms > 0 && now > 0 &&
               s_rt.inputs.position_sample_ms > 0) {
        uint32_t elapsed = (uint32_t)(now - s_rt.inputs.position_sample_ms);
        out->fresh = elapsed < s_rt.ilk_config.position_stale_timeout_ms;
    } else {
        /* No timeout configured or no timestamps — treat valid as fresh */
        out->fresh = s_rt.inputs.position_sample_valid;
    }

    api_unlock();
    return ESP_OK;
}

/* ================================================================
 * Test Helpers
 * ================================================================ */

#ifdef SAFETY_MANAGER_TEST_HOOKS

/* ---- Unregister failure injection ---- */

void safety_manager_test_fail_next_unregister(esp_err_t error,
                                               safety_stop_hook_t match_fn,
                                               void *match_ctx)
{
    s_unregister_inject.error = error;
    s_unregister_inject.match_fn = match_fn;
    s_unregister_inject.match_ctx = match_ctx;
    s_unregister_inject.active = true;
}

void safety_manager_test_clear_unregister_injection(void)
{
    memset(&s_unregister_inject, 0, sizeof(s_unregister_inject));
}

void safety_manager_test_set_inputs(const safety_inputs_t *inputs)
{
    if (!inputs) return;
    esp_err_t lock_err = api_lock();
    if (lock_err != ESP_OK) return;
    s_rt.inputs = *inputs;
    api_unlock();
}

void safety_manager_test_get_inputs(safety_inputs_t *out)
{
    if (!out) return;
    esp_err_t lock_err = api_lock();
    if (lock_err != ESP_OK) return;
    *out = s_rt.inputs;
    api_unlock();
}

void safety_manager_test_set_bl50_state(bool state_known, bool running)
{
    esp_err_t lock_err = api_lock();
    if (lock_err != ESP_OK) return;
    s_rt.inputs.bl50_state_known = state_known;
    s_rt.inputs.bl50_running = running;
    api_unlock();
}

void safety_manager_test_reset(void)
{
    memset(&s_unregister_inject, 0, sizeof(s_unregister_inject));
    ready_store(false);
    if (s_mutex_created && s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        memset(&s_rt, 0, sizeof(s_rt));
        state_store(SAFETY_MGR_UNINITIALIZED);
        api_unlock();
    }
}

uint8_t safety_manager_test_get_hook_count(void)
{
    esp_err_t lock_err = api_lock();
    if (lock_err != ESP_OK) return 0;
    uint8_t count = s_rt.hook_count;
    api_unlock();
    return count;
}

#endif /* SAFETY_MANAGER_TEST_HOOKS */
