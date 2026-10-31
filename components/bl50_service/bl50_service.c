#include "bl50_service.h"
#include "bl50_fsm.h"
#include "safety_manager.h"
#include "board_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <stdatomic.h>
#include <string.h>

static const char *TAG = "bl50_svc";

/* 生产契约：动作→所需桶位（薄包装，复用 bl50_fsm 的权威映射）。
 * 供 motor_bench 等诊断组件派生位置语义，杜绝硬编码猜测霍尔顺序。 */
drum_position_t bl50_service_action_to_position(bl50_action_t action)
{
    return bl50_action_to_position(action);
}

/* ================================================================
 * bl50_service.c — BL50 内筒业务服务层
 * 单 task 驱动 FSM，通过 safety_manager_apply 控制 HAL
 * ================================================================ */

/* ---- 内部状态 ---- */

typedef enum {
    LC_UNINITIALIZED = 0,
    LC_INITIALIZING,
    LC_RUNNING,
    LC_STOPPING,
    LC_STOPPED,
} lifecycle_state_t;

typedef struct {
    bl50_fsm_ctx_t fsm;
    const xiaojing_hal_t *hal;
    const machine_config_t *config;
    machine_event_sink_t event_sink;
    /* GPB5 HOME sensor debounce state */
    bool home_raw_low;                  /* raw decoded: true=LOW (triggered) */
    bool home_debounced;                /* debounced result */
    bool home_low_tracking;             /* actively tracking a LOW period */
    uint32_t home_low_since_ms;         /* time raw first went LOW (valid only when tracking) */
} bl50_runtime_t;

static SemaphoreHandle_t s_lc_mtx;
static StaticSemaphore_t s_lc_mtx_storage;
static _Atomic bool s_ready = false;
static _Atomic bool s_stop_requested = false;
static lifecycle_state_t s_lifecycle = LC_UNINITIALIZED;
static bl50_runtime_t s_rt;
static TaskHandle_t s_task_handle = NULL;
static QueueHandle_t s_cmd_queue = NULL;
static SemaphoreHandle_t s_stop_sem = NULL;
static StaticSemaphore_t s_stop_sem_storage;
/* 生命周期状态（受 s_lc_mtx 保护） */
static bool s_task_exited = false;    /* task 已完成 cleanup STOP */
static bool s_task_joined = false;    /* stop caller 已消费 done semaphore */
static esp_err_t s_task_exit_result = ESP_OK;

/* Test hooks — inject raw inputs, production decode/debounce always runs */
#ifdef CONFIG_BL50_SERVICE_TEST_HOOKS
static _Atomic uint8_t s_test_mcp_gpio_b = 0xFF;  /* default: all HIGH (not triggered) */
static _Atomic esp_err_t s_test_mcp_read_error = ESP_OK;
static _Atomic bool s_test_running = false;
/* Exit barrier: blocks task from giving done semaphore (deterministic timeout test) */
static _Atomic bool s_test_exit_barrier = false;
static _Atomic bool s_test_stop_halt = false;
#endif

/* ---- 命令队列消息 ---- */

typedef enum {
    CMD_RUN = 0,
    CMD_CANCEL,
    CMD_EMERGENCY,
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    bl50_request_t request;  /* 仅 CMD_RUN 使用 */
} cmd_msg_t;

/* ---- Safety 应用辅助 ---- */

static esp_err_t apply_bl50_stop(void)
{
    safety_request_t req = {
        .operation = SAFETY_OP_BL50,
        .enable = false,
        .request_id = bl50_fsm_get_request_id(&s_rt.fsm),
        .required_position = DRUM_POS_UNKNOWN,
        .source = APP_SOURCE_SYSTEM,
    };
    return safety_manager_apply(&req);
}

static esp_err_t apply_bl50_run(bool cw, uint8_t pwm)
{
    safety_request_t req = {
        .operation = SAFETY_OP_BL50,
        .enable = true,
        .request_id = bl50_fsm_get_request_id(&s_rt.fsm),
        .required_position = bl50_action_to_position((bl50_action_t)s_rt.fsm.action),
        .source = APP_SOURCE_SYSTEM,
        .params.motor = {
            .pwm_percent = pwm,
            .clockwise = cw,
        },
    };
    return safety_manager_apply(&req);
}

static esp_err_t apply_bl50_brake(void)
{
    /* BRAKE = run at 0% (STOP) — 实际上 BL50 BRAKE 是引脚控制
     * 通过 safety_manager 只能 STOP，BRAKE 通过 driver 内部处理 */
    return apply_bl50_stop();
}

/* ---- 执行 FSM 输出命令 ---- */

static esp_err_t execute_fsm_action(bl50_fsm_action_t action, uint8_t pwm)
{
    /* 软件方向翻转（实时读配置，无启动时滞）：bl50_reverse_dir=true 时交换
     * CW/CCW，使软件 CW 命令=物理 CW（修正 UVW 相序/机械安装导致的
     * 物理方向与软件方向不一致）。运行时标定命令改配置后下一脉冲即生效。 */
    bool rev = false;
    if (s_rt.config) {
        rev = s_rt.config->position.bl50_reverse_dir;
    }
    switch (action) {
    case BL50_FSM_ACTION_STOP:
        return apply_bl50_stop();
    case BL50_FSM_ACTION_BRAKE:
        return apply_bl50_brake();
    case BL50_FSM_ACTION_RUN_CW:
        return apply_bl50_run(!rev, pwm);
    case BL50_FSM_ACTION_RUN_CCW:
        return apply_bl50_run(rev, pwm);
    default:
        return ESP_OK;
    }
}

/* ---- 发布事件 ---- */

static void publish_event(const bl50_fsm_output_t *out)
{
    if (!s_rt.event_sink.publish || !out->emit_event) return;

    machine_event_t event = {
        .type = MACHINE_EVENT_BL50_DONE,
        .request_id = bl50_fsm_get_request_id(&s_rt.fsm),
        .result = out->event_result,
        .fault = {
            .code = out->fault_code,
            .severity = FAULT_SEVERITY_RECOVERABLE,
            .detail = out->fault_detail,
        },
        .value = 0,
    };

    esp_err_t err = s_rt.event_sink.publish(&event, 100, s_rt.event_sink.context);
    if (err == ESP_OK) {
        bl50_fsm_mark_terminal_emitted(&s_rt.fsm);
    }
}

/* ---- 获取当前时间 ---- */

static uint32_t get_now_ms(void)
{
    if (!s_rt.hal) return 0;
    return (uint32_t)s_rt.hal->now_ms();
}

/* ---- GPB5 HOME 传感器 ---- */

#define BL50_HOME_DEBOUNCE_MS  30

/* 解码 GPB5 LOW-active: bit5==0 表示触发 (non-static for test access) */
bool bl50_home_decode_gpb5(uint8_t gpio_b)
{
    return (gpio_b & (1U << 5)) == 0;  /* LOW = triggered */
}

/* 读取 raw GPB5 状态 (不含防抖) */
static esp_err_t read_home_raw(bool *triggered)
{
    if (!triggered) return ESP_ERR_INVALID_ARG;
#ifdef CONFIG_BL50_SERVICE_TEST_HOOKS
    esp_err_t err = atomic_load(&s_test_mcp_read_error);
    if (err != ESP_OK) return err;
    *triggered = bl50_home_decode_gpb5(atomic_load(&s_test_mcp_gpio_b));
    return ESP_OK;
#else
    if (!s_rt.hal) return ESP_ERR_INVALID_STATE;
    mcp_input_snapshot_t mcp;
    esp_err_t err = s_rt.hal->read_mcp_inputs(&mcp);
    if (err != ESP_OK) return err;
    *triggered = bl50_home_decode_gpb5(mcp.gpio_b);
    return ESP_OK;
#endif
}

/* 非阻塞防抖: 只有 LOW 连续稳定 >= debounce_ms 才算触发
 * 返回 ESP_OK 并设置 *triggered；或返回 I/O 错误。 */
static esp_err_t home_debounce_update(uint32_t now_ms, bool *triggered)
{
    if (!triggered) return ESP_ERR_INVALID_ARG;

    bool raw = false;
    esp_err_t err = read_home_raw(&raw);
    if (err != ESP_OK) {
        /* 读取失败: 重置防抖状态，返回 false，传播真实错误 */
        s_rt.home_low_tracking = false;
        s_rt.home_debounced = false;
        *triggered = false;
        return err;
    }

    s_rt.home_raw_low = raw;

    if (!raw) {
        /* HIGH: 重置防抖 */
        s_rt.home_low_tracking = false;
        s_rt.home_debounced = false;
        *triggered = false;
        return ESP_OK;
    }

    /* raw LOW */
    if (!s_rt.home_low_tracking) {
        /* 首次 LOW，开始跟踪 */
        s_rt.home_low_tracking = true;
        s_rt.home_low_since_ms = now_ms;
        s_rt.home_debounced = false;
        *triggered = false;
        return ESP_OK;
    }

    /* 持续 LOW，检查是否达到 debounce */
    uint32_t elapsed = now_ms - s_rt.home_low_since_ms;  /* wrap-safe */
    if (elapsed >= BL50_HOME_DEBOUNCE_MS) {
        s_rt.home_debounced = true;
    }
    *triggered = s_rt.home_debounced;
    return ESP_OK;
}

/* ---- 获取 home 传感器状态 (带防抖, 传播错误) ---- */

static esp_err_t read_home_sensor(bool *triggered)
{
    if (!triggered) return ESP_ERR_INVALID_ARG;
    uint32_t now = get_now_ms();
    return home_debounce_update(now, triggered);
}

/* ---- 获取位置 ---- */

static void get_position_info(bool *valid, bool *stable, drum_position_t *pos)
{
    safety_position_view_t view;
    if (safety_manager_get_position_view(&view) == ESP_OK) {
        *valid = view.sample_valid && view.fresh;
        *stable = view.stable;
        *pos = view.position;
    } else {
        *valid = false;
        *stable = false;
        *pos = DRUM_POS_UNKNOWN;
    }
}

/* ---- 服务任务 ---- */

static void bl50_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "task started");

    /* 等待 start 信号 */
    while (!atomic_load_explicit(&s_ready, memory_order_acquire)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

#ifdef CONFIG_BL50_SERVICE_TEST_HOOKS
    atomic_store(&s_test_running, true);
#endif

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(20); /* 50Hz tick */

    while (atomic_load_explicit(&s_ready, memory_order_acquire) &&
           !atomic_load_explicit(&s_stop_requested, memory_order_acquire)) {
        vTaskDelayUntil(&last_wake, period);

        /* 处理命令队列 */
        cmd_msg_t cmd;
        while (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
            switch (cmd.type) {
            case CMD_RUN:
                if (bl50_fsm_get_state(&s_rt.fsm) == BL50_SVC_STATE_IDLE) {
                    /* 从配置获取参数 */
                    uint8_t home_pwm = BL50_DEFAULT_HOME_PWM;
                    uint32_t home_timeout = BL50_DEFAULT_HOME_TIMEOUT_MS;
                    uint32_t max_action_ms = BL50_DEFAULT_MAX_ACTION_MS;
                    bool calibrated = false;
                    bool is_fake = (board_config_get_output_mode() == XIAOJING_MODE_FAKE);

                    if (s_rt.config) {
                        calibrated = s_rt.config->position.direction_calibrated;
                    }

                    esp_err_t err = bl50_fsm_begin_request(&s_rt.fsm,
                        &cmd.request, home_pwm, home_timeout,
                        max_action_ms, calibrated, is_fake);
                    if (err != ESP_OK) {
                        /* 拒绝请求，发布拒绝事件 */
                        machine_event_t event = {
                            .type = MACHINE_EVENT_BL50_DONE,
                            .request_id = cmd.request.request_id,
                            .result = SERVICE_RESULT_REJECTED,
                            .fault = {FAULT_NONE, 0, 0, 0},
                            .value = 0,
                        };
                        if (s_rt.event_sink.publish) {
                            s_rt.event_sink.publish(&event, 100, s_rt.event_sink.context);
                        }
                    }
                }
                break;

            case CMD_CANCEL:
                bl50_fsm_cancel(&s_rt.fsm, cmd.request.request_id);
                break;

            case CMD_EMERGENCY:
                bl50_fsm_emergency_stop(&s_rt.fsm);
                break;
            }
        }

        /* 仅在 HOMING 状态读取 GPB5 */
        bool home = false;
        bl50_service_state_t fsm_st = bl50_fsm_get_state(&s_rt.fsm);
        if (fsm_st == BL50_SVC_STATE_HOMING) {
            esp_err_t home_err = read_home_sensor(&home);
            if (home_err != ESP_OK) {
                /* GPB5 I/O error during HOMING: 明确故障身份 */
                bl50_fsm_fail(&s_rt.fsm, FAULT_BL50_HOME_IO, (uint32_t)home_err);
            }
        }

        bool pos_valid = false, pos_stable = false;
        drum_position_t pos = DRUM_POS_UNKNOWN;
        get_position_info(&pos_valid, &pos_stable, &pos);

        bl50_fsm_output_t out;
        bl50_fsm_tick(&s_rt.fsm, get_now_ms(), home,
                      pos_valid, pos_stable, pos, &out);

        /* 执行输出命令 */
        if (out.action != BL50_FSM_ACTION_NONE) {
            esp_err_t err = execute_fsm_action(out.action, out.pwm_percent);
            bl50_fsm_commit_result(&s_rt.fsm,
                err == ESP_OK ? BL50_FSM_ACTION_RESULT_OK : BL50_FSM_ACTION_RESULT_FAILED);
        }

        /* 发布终态事件 */
        if (out.emit_event) {
            publish_event(&out);
        }
    }

#ifdef CONFIG_BL50_SERVICE_TEST_HOOKS
    /* Exit barrier: 阻止 give done semaphore（确定性 timeout 测试用） */
    while (atomic_load_explicit(&s_test_exit_barrier, memory_order_acquire)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
#endif

    /* cleanup: stop BL50 */
    esp_err_t stop_err = apply_bl50_stop();

    /* 在 give semaphore 前，将结果和退出标志一起提交（受 mutex 保护） */
    xSemaphoreTake(s_lc_mtx, portMAX_DELAY);
    s_task_exit_result = stop_err;
    s_task_exited = true;
    xSemaphoreGive(s_lc_mtx);

#ifdef CONFIG_BL50_SERVICE_TEST_HOOKS
    /* Stop halt: 在 stop 之后、give sem 之前暂停（可选测试用） */
    while (atomic_load_explicit(&s_test_stop_halt, memory_order_acquire)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    atomic_store(&s_test_running, false);
#endif

    ESP_LOGI(TAG, "task exiting (stop_err=0x%x)", stop_err);
    /* 不在此处写 task_handle=NULL — stop() join 后清理 */
    xSemaphoreGive(s_stop_sem);
    vTaskDelete(NULL);
}

/* ---- Lifecycle API ---- */

esp_err_t bl50_service_init(const bl50_service_config_t *config)
{
    if (!config || !config->hal || !config->config) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lc_mtx, portMAX_DELAY);

    if (s_lifecycle != LC_UNINITIALIZED) {
        xSemaphoreGive(s_lc_mtx);
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_rt, 0, sizeof(s_rt));
    s_rt.hal = config->hal;
    s_rt.config = config->config;
    s_rt.event_sink = config->event_sink;
    bl50_fsm_init(&s_rt.fsm);

    s_cmd_queue = xQueueCreate(8, sizeof(cmd_msg_t));
    if (!s_cmd_queue) {
        xSemaphoreGive(s_lc_mtx);
        return ESP_ERR_NO_MEM;
    }

    s_lifecycle = LC_INITIALIZING;
    xSemaphoreGive(s_lc_mtx);
    return ESP_OK;
}

esp_err_t bl50_service_start(void)
{
    xSemaphoreTake(s_lc_mtx, portMAX_DELAY);

    if (s_lifecycle != LC_INITIALIZING) {
        xSemaphoreGive(s_lc_mtx);
        return ESP_ERR_INVALID_STATE;
    }

    s_stop_sem = xSemaphoreCreateBinaryStatic(&s_stop_sem_storage);
    if (!s_stop_sem) {
        xSemaphoreGive(s_lc_mtx);
        return ESP_ERR_NO_MEM;
    }

    /* 安全基线: 通过 safety_manager 执行 STOP 确认 BL50 已停止。
     * STOP 成功后 apply_bl50 自动提交 KNOWN_STOPPED。
     * STOP 失败则 start 返回错误，BL50 保持 UNKNOWN。 */
    safety_request_t baseline_stop = {
        .operation = SAFETY_OP_BL50,
        .enable = false,
        .request_id = MACHINE_REQUEST_ID_INVALID,
        .source = APP_SOURCE_SYSTEM,
    };
    esp_err_t baseline_err = safety_manager_apply(&baseline_stop);
    if (baseline_err != ESP_OK) {
        ESP_LOGE(TAG, "baseline STOP failed (0x%x), BL50 UNKNOWN", baseline_err);
        xSemaphoreGive(s_lc_mtx);
        return baseline_err;
    }

    /* 创建 task */
    BaseType_t ret = xTaskCreate(bl50_task, "bl50_svc", 4096, NULL, 5, &s_task_handle);
    if (ret != pdPASS) {
        xSemaphoreGive(s_lc_mtx);
        return ESP_ERR_NO_MEM;
    }

    s_lifecycle = LC_RUNNING;
    s_task_exit_result = ESP_OK;
    s_task_exited = false;
    s_task_joined = false;
    atomic_store_explicit(&s_stop_requested, false, memory_order_release);
    atomic_store_explicit(&s_ready, true, memory_order_release);
    xSemaphoreGive(s_lc_mtx);

    ESP_LOGI(TAG, "started");
    return ESP_OK;
}

esp_err_t bl50_service_stop(void)
{
    xSemaphoreTake(s_lc_mtx, portMAX_DELAY);

    if (s_lifecycle == LC_UNINITIALIZED || s_lifecycle == LC_STOPPED) {
        xSemaphoreGive(s_lc_mtx);
        return ESP_OK;
    }

    if (s_lifecycle == LC_STOPPING) {
        if (s_task_joined) {
            /* task 已 join：直接重试 STOP（不等待 semaphore） */
            if (s_task_exit_result != ESP_OK) {
                esp_err_t retry_err = apply_bl50_stop();
                s_task_exit_result = retry_err;
            }
            goto check_result;
        }
        /* task 尚未 join：等待 done semaphore */
        if (s_stop_sem) {
            if (!s_task_exited) {
                /* task 尚未退出，需要等待 */
                xSemaphoreGive(s_lc_mtx);
                if (xSemaphoreTake(s_stop_sem, pdMS_TO_TICKS(3000)) != pdTRUE) {
                    ESP_LOGW(TAG, "stop retry timeout");
                    return ESP_ERR_TIMEOUT;
                }
                xSemaphoreTake(s_lc_mtx, portMAX_DELAY);
            } else {
                /* task 已退出，sem 有 token，立即消费 */
                xSemaphoreGive(s_lc_mtx);
                if (xSemaphoreTake(s_stop_sem, pdMS_TO_TICKS(100)) != pdTRUE) {
                    ESP_LOGW(TAG, "stop sem unexpected empty");
                    return ESP_ERR_TIMEOUT;
                }
                xSemaphoreTake(s_lc_mtx, portMAX_DELAY);
            }
            s_task_joined = true;
            goto check_result;
        }
        xSemaphoreGive(s_lc_mtx);
        return ESP_OK;
    }

    if (s_lifecycle == LC_RUNNING) {
        s_lifecycle = LC_STOPPING;
        s_task_exit_result = ESP_OK;
        s_task_exited = false;
        s_task_joined = false;
        atomic_store_explicit(&s_stop_requested, true, memory_order_release);
        atomic_store_explicit(&s_ready, false, memory_order_release);

        /* 等待 task 退出 */
        if (s_stop_sem) {
            xSemaphoreGive(s_lc_mtx);
            if (xSemaphoreTake(s_stop_sem, pdMS_TO_TICKS(3000)) != pdTRUE) {
                ESP_LOGW(TAG, "stop timeout");
                /* task 可能仍在运行，保留所有资源 */
                return ESP_ERR_TIMEOUT;
            }
            xSemaphoreTake(s_lc_mtx, portMAX_DELAY);
            s_task_joined = true;
        }
    }

check_result:
    /* task_exit_result 由 task cleanup 在 mutex 下设置 */
    if (s_task_exit_result != ESP_OK) {
        /* STOP HAL 失败：不得清理资源，不得返回 ESP_OK
         * BL50 状态已由 safety_manager 设为 UNKNOWN
         * caller 必须重试 stop() */
        ESP_LOGE(TAG, "task BL50 STOP failed (0x%x), retry needed", s_task_exit_result);
        xSemaphoreGive(s_lc_mtx);
        return s_task_exit_result;
    }

    /* 清理资源 (只有 STOP 成功 + join 成功后才执行) */
    if (s_cmd_queue) {
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
    }
    s_task_handle = NULL;
    s_lifecycle = LC_STOPPED;
    s_task_exited = false;
    s_task_joined = false;
    atomic_store_explicit(&s_stop_requested, false, memory_order_release);
    atomic_store_explicit(&s_ready, false, memory_order_release);

    xSemaphoreGive(s_lc_mtx);
    ESP_LOGI(TAG, "stopped");
    return ESP_OK;
}

/* ---- Request API ---- */

esp_err_t bl50_service_run_async(const bl50_request_t *request)
{
    if (!request) return ESP_ERR_INVALID_ARG;
    if (!atomic_load_explicit(&s_ready, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Reject if FSM is already busy */
    if (bl50_fsm_get_state(&s_rt.fsm) != BL50_SVC_STATE_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }

    cmd_msg_t cmd = {
        .type = CMD_RUN,
        .request = *request,
    };

    if (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(10)) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;  /* queue full = busy */
    }
    return ESP_OK;
}

esp_err_t bl50_service_cancel(machine_request_id_t request_id)
{
    if (!atomic_load_explicit(&s_ready, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }

    cmd_msg_t cmd = {
        .type = CMD_CANCEL,
        .request.request_id = request_id,
    };

    if (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(10)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t bl50_service_emergency_stop(void)
{
    if (!atomic_load_explicit(&s_ready, memory_order_acquire)) {
        /* 未 ready: 直接尝试 BL50 OFF (不触发整机 emergency) */
        safety_request_t req = {
            .operation = SAFETY_OP_BL50,
            .enable = false,
            .request_id = MACHINE_REQUEST_ID_INVALID,
            .source = APP_SOURCE_SYSTEM,
        };
        return safety_manager_apply(&req);
    }

    cmd_msg_t cmd = {
        .type = CMD_EMERGENCY,
    };

    /* 尝试发送到队列；队列满时丢弃一个旧消息腾出空间再重试 */
    if (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(10)) != pdTRUE) {
        cmd_msg_t discard;
        if (xQueueReceive(s_cmd_queue, &discard, 0) == pdTRUE) {
            /* 丢弃一个旧消息，重试发送 emergency */
            if (xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE) {
                return ESP_OK;
            }
        }
        /* 重试仍失败：直接通过 safety_manager 执行 BL50 STOP
         * FSM 状态可能与 HAL 不同步，但电机已物理停止 */
        ESP_LOGW(TAG, "emergency queue full, direct BL50 STOP");
        safety_request_t req = {
            .operation = SAFETY_OP_BL50,
            .enable = false,
            .request_id = MACHINE_REQUEST_ID_INVALID,
            .source = APP_SOURCE_SYSTEM,
        };
        return safety_manager_apply(&req);
    }
    return ESP_OK;
}

/* ---- Snapshot ---- */

esp_err_t bl50_service_get_snapshot(bl50_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    out->state = bl50_fsm_get_state(&s_rt.fsm);
    out->active_action = (bl50_action_t)s_rt.fsm.action;
    out->active_request_id = bl50_fsm_get_request_id(&s_rt.fsm);
    out->current_pwm_percent = s_rt.fsm.current_pwm_percent;
    out->running_cw = s_rt.fsm.running_cw;
    out->fault = s_rt.fsm.fault;
    return ESP_OK;
}

/* ---- Test Helpers ---- */

#ifdef CONFIG_BL50_SERVICE_TEST_HOOKS

esp_err_t bl50_service_test_reset(void)
{
    xSemaphoreTake(s_lc_mtx, portMAX_DELAY);

    /* P0 UAF fix: If task is still running, wait for it to join before resetting */
    if (s_lifecycle == LC_RUNNING || s_lifecycle == LC_STOPPING) {
        if (!s_task_joined && s_task_handle) {
            /* Set stop requested and wait for task to exit */
            atomic_store_explicit(&s_stop_requested, true, memory_order_release);
            atomic_store_explicit(&s_ready, false, memory_order_release);
            xSemaphoreGive(s_lc_mtx);

            /* Wait for task done semaphore (up to 9s = 3 retries) */
            for (int i = 0; i < 3; i++) {
                if (s_stop_sem && xSemaphoreTake(s_stop_sem, pdMS_TO_TICKS(3000)) == pdTRUE) {
                    xSemaphoreTake(s_lc_mtx, portMAX_DELAY);
                    s_task_joined = true;
                    break;
                }
            }
            if (!s_task_joined) {
                ESP_LOGE(TAG, "test_reset: task did not exit after 9s");
                return ESP_ERR_TIMEOUT;
            }
        } else {
            xSemaphoreGive(s_lc_mtx);
        }
    } else {
        xSemaphoreGive(s_lc_mtx);
    }

    xSemaphoreTake(s_lc_mtx, portMAX_DELAY);
    atomic_store_explicit(&s_ready, false, memory_order_release);
    atomic_store_explicit(&s_stop_requested, false, memory_order_release);
    if (s_cmd_queue) {
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
    }
    s_task_handle = NULL;
    s_lifecycle = LC_UNINITIALIZED;
    s_task_exit_result = ESP_OK;
    s_task_exited = false;
    s_task_joined = false;
    memset(&s_rt, 0, sizeof(s_rt));
    atomic_store(&s_test_mcp_gpio_b, (uint8_t)0xFF);
    atomic_store(&s_test_mcp_read_error, ESP_OK);
    atomic_store(&s_test_running, false);
    atomic_store(&s_test_exit_barrier, false);
    atomic_store(&s_test_stop_halt, false);
    xSemaphoreGive(s_lc_mtx);
    return ESP_OK;
}

/* 注入 GPB5 raw 状态: triggered=true → LOW (bit5=0), triggered=false → HIGH (bit5=1) */
void bl50_service_test_set_home_sensor(bool triggered)
{
    uint8_t old_val = atomic_load(&s_test_mcp_gpio_b);
    uint8_t new_val;
    if (triggered) {
        new_val = old_val & ~(uint8_t)(1U << 5);  /* clear bit 5 = LOW */
    } else {
        new_val = old_val | (uint8_t)(1U << 5);   /* set bit 5 = HIGH */
    }
    atomic_store(&s_test_mcp_gpio_b, new_val);
}

void bl50_service_test_inject_mcp_error(esp_err_t error)
{
    atomic_store(&s_test_mcp_read_error, error);
}

bool bl50_service_test_get_running(void)
{
    return atomic_load(&s_test_running);
}

esp_err_t bl50_service_test_read_home_raw(bool *triggered)
{
    return read_home_raw(triggered);
}

esp_err_t bl50_service_test_read_home_debounced(bool *triggered)
{
    return read_home_sensor(triggered);
}

void bl50_service_test_set_exit_barrier(bool enabled)
{
    atomic_store(&s_test_exit_barrier, enabled);
}

void bl50_service_test_set_stop_halt(bool enabled)
{
    atomic_store(&s_test_stop_halt, enabled);
}

void bl50_service_test_set_hal(const xiaojing_hal_t *hal)
{
    s_rt.hal = hal;
}

#endif /* BL50_SERVICE_TEST_HOOKS */

/* ---- Module init (once) ---- */

static bool s_mtx_inited = false;

void bl50_service_module_init(void)
{
    if (!s_mtx_inited) {
        s_lc_mtx = xSemaphoreCreateMutexStatic(&s_lc_mtx_storage);
        s_mtx_inited = true;
    }
}

/* 静态初始化 constructor (GCC/Clang) */
__attribute__((constructor))
static void bl50_service_auto_init(void)
{
    bl50_service_module_init();
}
