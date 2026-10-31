/*
 * program_control_service.c — 程序控制 运行时接线
 *
 * 依赖：wash_executor / machine_config / program_control_core（纯 C）。
 * 所有洗涤/急停输入统一经本服务仲裁后访问 executor。
 *
 * 线程模型：控制平面（button/gesture/protocol/status 各任务入口）由
 * s_mutex 串行化。急停/后端/审计 provider 由 app_bootstrap 注入。
 */

#include "program_control.h"
#include "program_control_core.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "program_control";

#define PC_GESTURE_COOLDOWN_DEFAULT_MS 1000U

typedef struct {
    pc_arbiter_t arbiter;
    pc_gesture_state_t gesture;
    wash_executor_t *executor;
    const machine_config_t *(*get_config)(void);
    bool (*is_voice_active)(void);
    uint32_t gesture_cooldown_ms;
    program_control_emergency_fn emergency;
    void *emergency_ctx;
    program_control_backend_fn backend;
    void *backend_ctx;
    program_control_audit_provider_fn audit_provider;
    void *audit_ctx;
    SemaphoreHandle_t mutex;
    bool initialized;
    uint32_t program_id;
    uint32_t last_program_id;
} program_control_inst_t;

static program_control_inst_t s_inst;

static uint32_t pc_next_program_id(void)
{
    uint32_t id = s_inst.program_id;
    s_inst.program_id = (id == 0xFFFFFFFFU) ? 1U : id + 1U;
    s_inst.last_program_id = s_inst.program_id;
    return s_inst.program_id;
}

uint32_t program_control_last_program_id(void)
{
    return s_inst.last_program_id;
}

static esp_err_t execute_action(pc_action_t action)
{
    switch (action) {
    case PC_ACTION_START_DEMO: {
        if (!s_inst.executor) return ESP_ERR_INVALID_STATE;
        const machine_config_t *cfg = s_inst.get_config
            ? s_inst.get_config() : NULL;
        uint32_t program_id = pc_next_program_id();
        wash_program_t program;
        planner_report_t rep = default_demo_build_program(
            cfg, program_id, DEFAULT_DEMO_START_POSITION, &program);
        if (rep.result != PLAN_RESULT_OK) {
            ESP_LOGE(TAG, "demo build rejected: %d", (int)rep.reject_reason);
            return ESP_ERR_INVALID_ARG;
        }
        esp_err_t err = wash_executor_submit_program(
            s_inst.executor, &program, program_id);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "demo submit rejected: 0x%x", (unsigned)err);
        }
        return err;
    }
    case PC_ACTION_STOP_PROGRAM:
        if (!s_inst.executor) return ESP_ERR_INVALID_STATE;
        return wash_executor_submit_urgent(s_inst.executor, false);
    case PC_ACTION_ABORT_PROGRAM:
        if (!s_inst.executor) return ESP_ERR_INVALID_STATE;
        return wash_executor_submit_urgent(s_inst.executor, true);
    case PC_ACTION_EMERGENCY_STOP: {
        esp_err_t result = ESP_OK;
        if (s_inst.emergency) {
            result = s_inst.emergency(s_inst.emergency_ctx);
        } else {
            result = ESP_ERR_NOT_SUPPORTED;
        }
        if (s_inst.executor) {
            esp_err_t abort_err = wash_executor_submit_urgent(
                s_inst.executor, true);
            if (result == ESP_OK) result = abort_err;
        }
        return result;
    }
    case PC_ACTION_BACKEND_SWITCH:
        return s_inst.backend
            ? s_inst.backend(s_inst.backend_ctx)
            : ESP_ERR_NOT_SUPPORTED;
    case PC_ACTION_NONE:
    default:
        return ESP_OK;
    }
}

esp_err_t program_control_global_init(void)
{
    if (s_inst.mutex) return ESP_OK;   /* 幂等 */
    s_inst.mutex = xSemaphoreCreateMutex();
    return s_inst.mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t program_control_service_init(const program_control_service_config_t *config)
{
    if (!config || !config->executor) return ESP_ERR_INVALID_ARG;
    s_inst.executor = config->executor;
    s_inst.get_config = config->get_config;
    s_inst.is_voice_active = config->is_voice_active;
    s_inst.gesture_cooldown_ms = config->gesture_cooldown_ms > 0
        ? config->gesture_cooldown_ms : PC_GESTURE_COOLDOWN_DEFAULT_MS;
    s_inst.emergency = config->emergency;
    s_inst.emergency_ctx = config->emergency_ctx;
    s_inst.backend = config->backend;
    s_inst.backend_ctx = config->backend_ctx;
    s_inst.audit_provider = config->audit_provider;
    s_inst.audit_ctx = config->audit_ctx;
    s_inst.initialized = true;
    /* 首手势即可触发（否则开机后第一次手势被冷却抑制掉） */
    s_inst.gesture.armed = true;
    s_inst.gesture.cooldown_until_ms = 0;
    return ESP_OK;
}

esp_err_t program_control_service_start(void)
{
    if (!s_inst.initialized || !s_inst.executor) return ESP_ERR_INVALID_STATE;
    /* 注册 executor 审计 sink：程序内部 STEP_AUDIT_* 步骤据此校验。 */
    wash_executor_set_audit_sink(
        s_inst.executor, program_control_executor_audit, &s_inst);
    return ESP_OK;
}

esp_err_t program_control_service_stop(void)
{
    s_inst.initialized = false;
    s_inst.executor = NULL;
    return ESP_OK;
}

static void lock(void)
{
    if (s_inst.mutex) xSemaphoreTake(s_inst.mutex, portMAX_DELAY);
}

static void unlock(void)
{
    if (s_inst.mutex) xSemaphoreGive(s_inst.mutex);
}

/* 从 executor 快照回填外部真实（无锁读取快照，锁内刷新闩锁）。 */
esp_err_t program_control_refresh(void)
{
    lock();
    bool was_active = s_inst.arbiter.program_active;
    if (s_inst.executor) {
        wash_exec_snapshot_t snap;
        memset(&snap, 0, sizeof(snap));
        if (wash_executor_get_snapshot(s_inst.executor, &snap) == ESP_OK) {
            s_inst.arbiter.program_active = snap.program_active;
            s_inst.arbiter.fault_active =
                (snap.state == WASH_EXEC_STATE_FAULT);
            if (was_active && !snap.program_active) {
                ESP_LOGI(TAG, "program terminal observed; arbiter -> idle");
            }
        }
    }
    pc_arbiter_refresh(&s_inst.arbiter);
    unlock();
    return ESP_OK;
}

esp_err_t program_control_request(pc_request_t req, pc_action_t *out_action)
{
    if (out_action) *out_action = PC_ACTION_NONE;
    if (!s_inst.initialized) return ESP_ERR_INVALID_STATE;
    lock();
    pc_action_t action = pc_arbitrate(&s_inst.arbiter, req);
    esp_err_t result = ESP_OK;
    if (action != PC_ACTION_NONE) {
        result = execute_action(action);
        if (out_action) *out_action = action;
    }
    /* START 提交被 executor 拒绝（busy 等）时，闩锁经下次 refresh 回 IDLE。 */
    unlock();
    return result;
}

esp_err_t program_control_gesture(hal_gesture_t gesture,
                                  pc_gesture_action_t *out_action)
{
    if (out_action) *out_action = PC_GESTURE_ACTION_NONE;
    if (!s_inst.initialized) return ESP_ERR_INVALID_STATE;
    /* 手势冷却与触发由纯状态机驱动；控制平面需要单调时钟，这里用
     * esp_timer（毫秒）。 */
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    lock();
    pc_gesture_action_t ga = pc_gesture_step(
        &s_inst.gesture, gesture, now_ms, s_inst.gesture_cooldown_ms);
    if (ga == PC_GESTURE_ACTION_START) {
        pc_action_t action = pc_arbitrate(&s_inst.arbiter, PC_REQ_START_DEMO);
        if (action == PC_ACTION_START_DEMO) {
            esp_err_t err = execute_action(action);
            if (out_action) *out_action = ga;
            unlock();
            return err;
        }
    } else if (ga == PC_GESTURE_ACTION_STOP) {
        pc_action_t action = pc_arbitrate(&s_inst.arbiter, PC_REQ_STOP);
        if (action == PC_ACTION_STOP_PROGRAM) {
            esp_err_t err = execute_action(action);
            if (out_action) *out_action = ga;
            unlock();
            return err;
        }
    } else {
        if (out_action) *out_action = ga;   /* 翻页动作交给调用方 */
    }
    unlock();
    return ESP_OK;
}

esp_err_t program_control_btn1(pc_btn1_event_t ev, pc_btn1_action_t *out_action)
{
    if (out_action) *out_action = PC_BTN1_ACTION_NONE;
    if (!s_inst.initialized) return ESP_ERR_INVALID_STATE;
    lock();
    bool voice_active = s_inst.is_voice_active
        ? s_inst.is_voice_active() : false;
    pc_btn1_action_t ba = pc_btn1_decide(ev, voice_active);
    esp_err_t result = ESP_OK;
    if (ba == PC_BTN1_ACTION_START) {
        pc_action_t action = pc_arbitrate(&s_inst.arbiter, PC_REQ_START_DEMO);
        if (action == PC_ACTION_START_DEMO) {
            result = execute_action(action);
            if (out_action) *out_action = ba;
        }
    } else if (ba == PC_BTN1_ACTION_BACKEND) {
        pc_action_t action = pc_arbitrate(&s_inst.arbiter, PC_REQ_BACKEND_SWITCH);
        if (action == PC_ACTION_BACKEND_SWITCH) {
            result = execute_action(action);
            if (out_action) *out_action = ba;
        }
    }
    unlock();
    return result;
}

pc_state_t program_control_get_state(void)
{
    lock();
    pc_state_t s = s_inst.arbiter.state;
    unlock();
    return s;
}

bool program_control_program_active(void)
{
    lock();
    bool active = s_inst.arbiter.program_active;
    unlock();
    return active;
}

void program_control_set_external_busy(bool busy)
{
    lock();
    s_inst.arbiter.external_busy = busy;
    pc_arbiter_refresh(&s_inst.arbiter);
    unlock();
}

bool program_control_executor_audit(wash_step_type_t step_type, void *ctx)
{
    (void)ctx;
    if (!s_inst.initialized) return false;   /* fail-closed */
    if (!s_inst.audit_provider) return false; /* 无 provider → 审计无法验证 */
    pc_output_audit_t a;
    memset(&a, 0, sizeof(a));
    if (s_inst.audit_provider(&a, s_inst.audit_ctx) != ESP_OK) return false;
    switch (step_type) {
    case STEP_AUDIT_UV_OFF:   return pc_audit_uv_off(&a);
    case STEP_AUDIT_SAFE_OFF: return pc_audit_safe_off(&a);
    default:                  return false;
    }
}
