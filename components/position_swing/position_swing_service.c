/*
 * position_swing_service.c — 结构电机（IBT-2）连续换向展示台服务
 * （开发/板级诊断，与 motor_bench_service / actuator_self_test 同源
 * 的 fail-closed 门禁）
 *
 * 唯一通电路径：用户显式「开始」→ 本服务直接驱动 IBT-2 开环
 * （CW/CCW），每段 `dwell_ms`（硬上限 8000ms）后停车 → 固定刹车
 * 500ms → 方向翻转 → 下一段；重复 `rounds`（≤4）段后停在最终位置。
 * 每个运行段是一段真实电机转动，霍尔读数经 position_service 快照
 * 观察（组装成 position_swing 子块上报给小程序）。
 *
 * 安全模型（同 motor_bench_service 同源）：
 *   - 绝不无用户请求时通电；每段均有硬上限；
 *   - test_active 门禁：executor 空闲、无 UV/执行器/motor_bench 板测、
 *     无 fault/emergency（任一快照失败 → fail-closed FAULT + 全关）；
 *   - 运行期间 wash_executor_set_external_busy(true)（反向互斥）；
 *   - BLE 断线 / teardown / 急停 → position_swing_service_emergency()。
 *
 * 受理前提：position_service 快照 direction_calibrated==true（否则
 * SWING_NOT_CALIBRATED 拒绝）；IBT-2 直接驱动仅诊断，绝不与生产洗涤并行。
 *
 * 方向：直接 IBT-2 真实方向由 HW_reverse 参数（swing_reverse_dir）决定；
 * 服务默认 CW=物理 CW（direction_calibrated 后再 punch_swing 即真）。
 * 默认不翻转；若需修正接线差异可传 reverse：true。
 */

#include "position_swing.h"

#include <stdatomic.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "safety_manager.h"
#include "position_service.h"
#include "uv_self_test.h"
#include "actuator_self_test.h"
#include "motor_bench_service.h"
#include "wash_executor.h"
#include "machine_config.h"
#include "machine_types.h"
#include "xiaojing_hal.h"

#define PS_TAG            "pos_swing"
#define PS_TICK_MS        60U
#define PS_TASK_STACK     3072U
#define PS_TASK_PRIORITY  13U
#define PS_TASK_CORE      1U
#define PS_BRAKE_MS       500U            /* 每段间隔（含刹停） */
#define PS_PWM            40U             /* 低档，与台架脉冲同 */

#define PS_NOTIFY_RUN      (1U << 0)
#define PS_NOTIFY_EMERGENCY (1U << 1)

typedef struct {
    position_swing_service_config_t cfg;
    bool initialized;
    bool started;

    TaskHandle_t task;
    SemaphoreHandle_t task_done_sem;
    _Atomic bool stop_requested;
    _Atomic bool emergency_latch;
    _Atomic bool test_active;

    position_swing_state_t state;
    position_swing_terminal_t terminal;
    uint32_t request_id;

    uint32_t total_rounds;
    uint32_t dwell_ms;
    uint32_t segment_done;    /* 已完成段数 */
    bool dir_cw;              /* 当前段方向 */
    bool output_confirmed_off;
    uint32_t last_elapsed_ms;

    uint32_t seg_start_ms;    /* 当前段/刹车起始 tick 时钟 */
    uint32_t start_tick_ms;   /* 受理 tick 时钟（elapsed 基准） */
    bool pending_request;

    uint32_t next_request_id;
    char last_code_buf[40];
    const char *last_code;    /* 指向 last_code_buf 或常量 */
} position_swing_rt_t;

static position_swing_rt_t s_rt;
static SemaphoreHandle_t s_mtx = NULL;
static StaticTask_t s_task_tcb;
static EXT_RAM_BSS_ATTR StackType_t s_task_stack[PS_TASK_STACK];
static const xiaojing_hal_t *s_hal = NULL;

static bool lock(void)
{
    return s_mtx && xSemaphoreTake(s_mtx, pdMS_TO_TICKS(400)) == pdTRUE;
}
static void unlock(void)
{
    xSemaphoreGive(s_mtx);
}
static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void set_last_code(const char *code)
{
    if (!code) { s_rt.last_code = ""; return; }
    snprintf(s_rt.last_code_buf, sizeof(s_rt.last_code_buf), "%s", code);
    s_rt.last_code = s_rt.last_code_buf;
}

static const char *ps_state_name(position_swing_state_t st)
{
    switch (st) {
#define E(n) case n: return #n;
    E(POS_SWING_STATE_IDLE) E(POS_SWING_STATE_PRECHECK)
    E(POS_SWING_STATE_RUNNING_CW) E(POS_SWING_STATE_BRAKING)
    E(POS_SWING_STATE_RUNNING_CCW) E(POS_SWING_STATE_FAULT)
    E(POS_SWING_STATE_COMPLETE) E(POS_SWING_STATE_REJECTED)
    E(POS_SWING_STATE_INTERRUPTED) E(POS_SWING_STATE_TIMEOUT)
#undef E
    }
    return "?";
}

/* ================================================================
 * 门禁采集（fail-closed）
 * ================================================================ */

typedef struct {
    bool snapshot_fail;
    bool exec_idle;
    bool uv_busy;
    bool act_busy;
    bool mb_busy;
    bool fault_active;
    bool emergency_active;
    bool hall_conflict;
    bool direction_calibrated;
} ps_gate_input_t;

static void collect_gate_input(ps_gate_input_t *in)
{
    memset(in, 0, sizeof(*in));
    const machine_config_t *cfg = machine_config_get();
    in->direction_calibrated = cfg && cfg->position.direction_calibrated;

    if (s_rt.cfg.executor) {
        wash_exec_snapshot_t snap;
        if (wash_executor_get_snapshot(s_rt.cfg.executor, &snap) == ESP_OK) {
            bool reserved = wash_executor_program_reserved(s_rt.cfg.executor);
            in->exec_idle = snap.state == WASH_EXEC_STATE_IDLE &&
                            !snap.program_active && !snap.terminal_pending &&
                            !reserved;
        } else {
            in->snapshot_fail = true;
        }
    }

    uint8_t uv = uv_self_test_state_code();
    in->uv_busy = (uv == (uint8_t)UV_STATE_SELFTEST_VALIDATING ||
                   uv == (uint8_t)UV_STATE_SELFTEST_RUNNING ||
                   uv == (uint8_t)UV_STATE_SELFTEST_STOPPING);
    uint8_t act = actuator_self_test_state_code();
    in->act_busy = (act == (uint8_t)ACT_STATE_ACCEPTED ||
                    act == (uint8_t)ACT_STATE_RUNNING);

    motor_bench_snapshot_t mb;
    if (motor_bench_service_get_snapshot(&mb) == ESP_OK) {
        in->mb_busy = mb.state != MOTOR_BENCH_STATE_INACTIVE &&
                      mb.terminal == MOTOR_BENCH_TERMINAL_NONE;
    } else {
        in->snapshot_fail = true;
    }

    safety_snapshot_t safe;
    if (safety_manager_get_snapshot(&safe) == ESP_OK) {
        in->fault_active = safe.fault_active;
        in->emergency_active = safe.emergency_stop_active;
    } else {
        in->snapshot_fail = true;
    }

    position_snapshot_t ps;
    if (position_service_get_snapshot(&ps) == ESP_OK) {
        /* 方向已标定：由 machine_config live 读取（direction_calibrated
         * 仅存于 config / 持久 NVS，position_snapshot 不含该字段）。 */
        unsigned m = ps.stable_hall_mask;
        in->hall_conflict = (m & (m - 1U)) != 0U ||
                            (ps.state == POSITION_STATE_FAULT &&
                             ps.fault.code == FAULT_HALL_CONFLICT);
    } else {
        in->snapshot_fail = true;
    }
}

static bool gate_passes(const ps_gate_input_t *in)
{
    return !in->snapshot_fail && in->exec_idle &&
           !in->uv_busy && !in->act_busy && !in->mb_busy &&
           !in->fault_active && !in->emergency_active &&
           !in->hall_conflict && in->direction_calibrated;
}

/* 拒绝码：按优先级 */
static const char *gate_reject_code(const ps_gate_input_t *in)
{
    if (in->snapshot_fail) return "GATE_UNAVAILABLE";
    if (in->emergency_active) return "EMERGENCY_ACTIVE";
    if (in->fault_active) return "FAULT_ACTIVE";
    if (!in->exec_idle) return "EXECUTOR_BUSY";
    if (in->mb_busy || in->uv_busy || in->act_busy) return "SELF_TEST_BUSY";
    if (in->hall_conflict) return "HALL_CONFLICT";
    if (!in->direction_calibrated) return "SWING_NOT_CALIBRATED";
    return "REJECTED";
}

/* ================================================================
 * 输出：IBT-2 直接驱动（经 hal）
 * ================================================================ */

static esp_err_t ps_start_locked(void)
{
    if (!s_hal || !s_hal->set_ibt2) return ESP_ERR_INVALID_STATE;
    uint8_t pwm = PS_PWM;
    const machine_config_t *mcfg = machine_config_get();
    if (mcfg && mcfg->benchtst.ibt2_pwm > 0U &&
        mcfg->benchtst.ibt2_pwm <= 100U) {
        pwm = mcfg->benchtst.ibt2_pwm;   /* 换向展示台 PWM 跟随滑杆/NVS */
    }
    ibt2_command_t cmd = {
        .command = s_rt.dir_cw ? IBT2_CMD_CW : IBT2_CMD_CCW,
        .pwm_percent = pwm,
    };
    return s_hal->set_ibt2(cmd);
}

static esp_err_t ps_stop_locked(void)
{
    if (!s_hal || !s_hal->stop_ibt2) return ESP_ERR_INVALID_STATE;
    return s_hal->stop_ibt2();
}

static bool output_confirmed_off_locked(void)
{
    /* 本服务只拥有 IBT-2。以 position_service 快照 IDLE 为证据；
     * 快照保留（非 MOVING/BRAKING/FAULT）即证明电机未运转。 */
    if (!s_hal) return false;
    position_snapshot_t ps;
    if (position_service_get_snapshot(&ps) != ESP_OK) return false;
    return (ps.state == POSITION_STATE_IDLE_KNOWN ||
            ps.state == POSITION_STATE_IDLE_UNKNOWN);
}

/* ================================================================
 * 状态机（锁内调用）
 * ================================================================ */

static void fsm_reset_locked(void)
{
    s_rt.state = POS_SWING_STATE_IDLE;
    s_rt.terminal = POS_SWING_TERMINAL_NONE;
    s_rt.segment_done = 0;
    s_rt.dir_cw = true;
    s_rt.output_confirmed_off = false;
    s_rt.last_elapsed_ms = 0;
    s_rt.seg_start_ms = 0;
    s_rt.start_tick_ms = now_ms();
    set_last_code("");
}

static void fsm_term_locked(position_swing_state_t st,
                            position_swing_terminal_t term,
                            const char *code)
{
    (void)ps_stop_locked();
    s_rt.state = st;
    s_rt.terminal = term;
    if (code) set_last_code(code);
    /* 终态 OFF 确认 */
    s_rt.output_confirmed_off = output_confirmed_off_locked();
}

static void fsm_interrupt_locked(void)
{
    (void)ps_stop_locked();
    s_rt.state = POS_SWING_STATE_INTERRUPTED;
    s_rt.terminal = POS_SWING_TERMINAL_INTERRUPTED;
    s_rt.output_confirmed_off = output_confirmed_off_locked();
    set_last_code("INTERRUPTED");
}

static void fsm_complete_locked(void)
{
    (void)ps_stop_locked();
    s_rt.state = POS_SWING_STATE_COMPLETE;
    s_rt.terminal = POS_SWING_TERMINAL_COMPLETE;
    s_rt.output_confirmed_off = output_confirmed_off_locked();
    set_last_code(s_rt.output_confirmed_off ? "COMPLETE" : "COMPLETE_PENDING");
}

static bool fsm_is_terminal_locked(void)
{
    return s_rt.terminal != POS_SWING_TERMINAL_NONE;
}

/* ---- 每 tick 状态机推进（返回 1 表示到达终态） ---- */

static bool fsm_tick(void)
{
    if (fsm_is_terminal_locked()) return true;

    uint32_t now = now_ms();
    s_rt.last_elapsed_ms = now - s_rt.start_tick_ms;

    switch (s_rt.state) {
    case POS_SWING_STATE_PRECHECK: {
        ps_gate_input_t in;
        collect_gate_input(&in);
        if (!gate_passes(&in)) {
            fsm_term_locked(POS_SWING_STATE_REJECTED,
                            POS_SWING_TERMINAL_REJECTED,
                            gate_reject_code(&in));
            return true;
        }
        s_rt.state = POS_SWING_STATE_RUNNING_CW;
        s_rt.dir_cw = true;
        s_rt.seg_start_ms = now;
        if (ps_start_locked() != ESP_OK) {
            fsm_term_locked(POS_SWING_STATE_FAULT,
                            POS_SWING_TERMINAL_FAULT, "DRIVER_START");
            return true;
        }
        ESP_LOGI(PS_TAG, "seg=0 dir=CW start");
        return false;
    }
    case POS_SWING_STATE_RUNNING_CW:
    case POS_SWING_STATE_RUNNING_CCW: {
        if ((now - s_rt.seg_start_ms) < s_rt.dwell_ms) return false;
        (void)ps_stop_locked();          /* 进入刹车间隔 */
        s_rt.seg_start_ms = now;
        s_rt.state = POS_SWING_STATE_BRAKING;
        return false;
    }
    case POS_SWING_STATE_BRAKING: {
        if ((now - s_rt.seg_start_ms) < PS_BRAKE_MS) return false;
        s_rt.segment_done++;
        if (s_rt.segment_done >= s_rt.total_rounds) {
            fsm_complete_locked();
            return true;
        }
        s_rt.dir_cw = !s_rt.dir_cw;
        s_rt.state = (s_rt.dir_cw ? POS_SWING_STATE_RUNNING_CW
                                  : POS_SWING_STATE_RUNNING_CCW);
        s_rt.seg_start_ms = now;
        if (ps_start_locked() != ESP_OK) {
            fsm_term_locked(POS_SWING_STATE_FAULT,
                            POS_SWING_TERMINAL_FAULT, "DRIVER_START");
            return true;
        }
        return false;
    }
    case POS_SWING_STATE_IDLE:
    case POS_SWING_STATE_FAULT:
    case POS_SWING_STATE_COMPLETE:
    case POS_SWING_STATE_REJECTED:
    case POS_SWING_STATE_INTERRUPTED:
    case POS_SWING_STATE_TIMEOUT:
    default:
        return true;
    }
}

/* ================================================================
 * service tick（锁内）
 * ================================================================ */

static void clear_test_locked(void)
{
    if (s_rt.cfg.executor) wash_executor_set_external_busy(s_rt.cfg.executor, false);
    atomic_store(&s_rt.test_active, false);
}

static void service_tick_locked(void)
{
    if (!atomic_load(&s_rt.test_active)) return;

    if (atomic_load(&s_rt.emergency_latch)) {
        fsm_interrupt_locked();
        clear_test_locked();
        return;
    }

    /* 运行中持续复核门禁（fault/emergency/其他自检抢占） */
    {
        ps_gate_input_t in;
        collect_gate_input(&in);
        if (in.snapshot_fail || in.fault_active || in.emergency_active ||
            in.uv_busy || in.act_busy || in.mb_busy) {
            fsm_interrupt_locked();
            clear_test_locked();
            return;
        }
    }

    if (fsm_tick()) {
        ESP_LOGI(PS_TAG, "terminal=%d state=%s off=%u",
                 (int)s_rt.terminal, ps_state_name(s_rt.state),
                 (unsigned)s_rt.output_confirmed_off);
        clear_test_locked();
    }
}

/* ---- 任务 ---- */

static void ps_task(void *arg)
{
    (void)arg;
    while (!atomic_load(&s_rt.stop_requested)) {
        uint32_t notify = 0;
        TickType_t wait = atomic_load(&s_rt.test_active)
            ? pdMS_TO_TICKS(PS_TICK_MS)
            : pdMS_TO_TICKS(200);
        xTaskNotifyWait(0, PS_NOTIFY_RUN | PS_NOTIFY_EMERGENCY, &notify, wait);

        if (notify & PS_NOTIFY_EMERGENCY) {
            if (lock()) {
                if (atomic_load(&s_rt.test_active)) {
                    fsm_interrupt_locked();
                    clear_test_locked();
                }
                atomic_store(&s_rt.emergency_latch, true);
                unlock();
            }
        }

        if (lock()) {
            service_tick_locked();
            unlock();
        }
    }
    if (s_rt.task_done_sem) xSemaphoreGive(s_rt.task_done_sem);
    vTaskDelete(NULL);
}

/* ================================================================
 * 公共 API
 * ================================================================ */

esp_err_t position_swing_global_init(void)
{
    if (!s_mtx) {
        s_mtx = xSemaphoreCreateMutex();
        if (!s_mtx) return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t position_swing_service_init(
    const position_swing_service_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    if (!s_mtx) {
        s_mtx = xSemaphoreCreateMutex();
        if (!s_mtx) return ESP_ERR_NO_MEM;
    }
    if (!lock()) return ESP_ERR_TIMEOUT;
    if (s_rt.initialized) { unlock(); return ESP_ERR_INVALID_STATE; }
    memset(&s_rt, 0, sizeof(s_rt));
    s_rt.cfg = *config;
    s_rt.next_request_id = 1;
    s_rt.start_tick_ms = now_ms();
    s_rt.initialized = true;
    unlock();
    return ESP_OK;
}

esp_err_t position_swing_service_start(void)
{
    if (!lock()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.initialized || s_rt.started) { unlock(); return ESP_ERR_INVALID_STATE; }
    s_rt.started = true;
    if (s_rt.task_done_sem) vSemaphoreDelete(s_rt.task_done_sem);
    s_rt.task_done_sem = xSemaphoreCreateBinary();
    if (!s_rt.task_done_sem) { s_rt.started = false; unlock(); return ESP_ERR_NO_MEM; }
    atomic_store(&s_rt.stop_requested, false);
    TaskHandle_t h = xTaskCreateStaticPinnedToCore(
        ps_task, "ps_task", PS_TASK_STACK, NULL,
        PS_TASK_PRIORITY, s_task_stack, &s_task_tcb, PS_TASK_CORE);
    if (!h) { s_rt.started = false; unlock(); return ESP_ERR_NO_MEM; }
    s_rt.task = h;
    unlock();
    return ESP_OK;
}

/* 注入 HAL（app_bootstrap 建 hal 后调用） */
esp_err_t position_swing_set_hal(const xiaojing_hal_t *hal)
{
    if (!hal) return ESP_ERR_INVALID_ARG;
    if (!lock()) return ESP_ERR_TIMEOUT;
    s_hal = hal;
    unlock();
    return ESP_OK;
}

esp_err_t position_swing_service_request(
    const position_swing_request_t *req, const char **out_code)
{
    if (!out_code) return ESP_ERR_INVALID_ARG;
    *out_code = NULL;
    if (!req) { *out_code = "BAD_PAYLOAD"; return ESP_OK; }
    if (!lock()) return ESP_ERR_TIMEOUT;
    if (!s_rt.initialized || !s_rt.started) { *out_code = "NOT_READY"; unlock(); return ESP_OK; }
    if (atomic_load(&s_rt.test_active) && !fsm_is_terminal_locked()) {
        *out_code = "BUSY"; unlock(); return ESP_OK;
    }

    ps_gate_input_t in;
    collect_gate_input(&in);
    if (!gate_passes(&in)) {
        *out_code = gate_reject_code(&in);
        unlock();
        return ESP_OK;
    }

    atomic_store(&s_rt.test_active, true);
    fsm_reset_locked();
    s_rt.total_rounds = position_swing_clamp_rounds(req->rounds);
    s_rt.dwell_ms = position_swing_clamp_dwell(req->duration_ms);
    s_rt.request_id = s_rt.next_request_id++;
    s_rt.state = POS_SWING_STATE_PRECHECK;
    s_rt.terminal = POS_SWING_TERMINAL_NONE;
    s_rt.start_tick_ms = now_ms();
    set_last_code("ACCEPTED");
    if (s_rt.cfg.executor) wash_executor_set_external_busy(s_rt.cfg.executor, true);

    *out_code = "POSITION_SWING_ACCEPTED";
    ESP_LOGI(PS_TAG, "accepted rounds=%u dwell=%u rid=%"PRIu32,
             s_rt.total_rounds, s_rt.dwell_ms, s_rt.request_id);
    unlock();
    return ESP_OK;
}

esp_err_t position_swing_service_cancel(const char **out_code)
{
    if (out_code) *out_code = NULL;
    if (!lock()) return ESP_ERR_TIMEOUT;
    if (!atomic_load(&s_rt.test_active) || fsm_is_terminal_locked()) {
        if (out_code) *out_code = "NOT_ACTIVE";
        unlock(); return ESP_OK;
    }
    fsm_interrupt_locked();
    clear_test_locked();
    if (out_code) *out_code = "CANCELLED";
    unlock();
    return ESP_OK;
}

esp_err_t position_swing_service_emergency(void)
{
    if (!lock()) return ESP_ERR_TIMEOUT;
    atomic_store(&s_rt.emergency_latch, true);
    if (atomic_load(&s_rt.test_active)) {
        fsm_interrupt_locked();
        clear_test_locked();
    }
    unlock();
    return ESP_OK;
}

esp_err_t position_swing_service_get_snapshot(position_swing_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!lock()) return ESP_ERR_TIMEOUT;
    out->state = s_rt.state;
    out->terminal = s_rt.terminal;
    out->request_id = s_rt.request_id;
    out->segment_index = s_rt.segment_done;
    out->dir_cw = s_rt.dir_cw;
    out->output_confirmed_off = s_rt.output_confirmed_off;
    out->emergency = atomic_load(&s_rt.emergency_latch);
    out->total_rounds = s_rt.total_rounds;
    out->dwell_ms = s_rt.dwell_ms;
    out->elapsed_ms = (atomic_load(&s_rt.test_active)
                       ? (now_ms() - s_rt.start_tick_ms)
                       : s_rt.last_elapsed_ms);
    unlock();
    return ESP_OK;
}

esp_err_t position_swing_service_stop(void)
{
    if (!s_mtx) return ESP_ERR_INVALID_STATE;
    if (!lock()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.initialized) { unlock(); return ESP_ERR_INVALID_STATE; }
    atomic_store(&s_rt.emergency_latch, true);
    if (atomic_load(&s_rt.test_active)) {
        fsm_interrupt_locked();
        clear_test_locked();
    }
    atomic_store(&s_rt.stop_requested, true);
    unlock();
    if (s_rt.task) xTaskNotify(s_rt.task, PS_NOTIFY_EMERGENCY, eSetBits);
    if (s_rt.task_done_sem)
        xSemaphoreTake(s_rt.task_done_sem, pdMS_TO_TICKS(2000));
    if (!lock()) return ESP_ERR_TIMEOUT;
    s_rt.started = false;
    s_rt.initialized = false;
    unlock();
    return ESP_OK;
}
/* clamp 实现见 position_swing_core.c（纯 C，host 测试共用）。 */