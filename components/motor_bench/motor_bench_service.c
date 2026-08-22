/*
 * motor_bench_service.c — 电机空载台架 运行时组件（诊断模式专用）
 *
 * 唯一通电路径：用户确认脉冲 → bl50_service_run_async()（真实生产动作适配器）
 * → safety_manager → HAL。绝不自动让电机通电：每步仅在用户手动摆放桶位到生产
 * 契约位置（bl50_service_action_to_position，禁止硬编码猜测）且稳定后，经用户
 * 显式 confirm_pulse() 才提交一次低能量短脉冲（低档 ≤300ms，PWM=15%）。
 * IBT-2 位置电机绝不由此组件驱动（位置由用户手动摆放，五路霍尔读取）。
 *
 * 门禁 + 多步 FSM 为纯逻辑（motor_bench_core.c，host-testable）；本文件只做：
 *   1. 快照采集（fail-closed，任一关键快照失败 → SNAPSHOT_UNAVAILABLE）
 *   2. tick 驱动纯 FSM（终态 exactly-once 由 core 保证）
 *   3. submit_pulse → bl50_service_run_async（错误 → SUBMIT_FAILED）
 *   4. request_all_off → 全输出紧急关断（BL50/IBT-2/水阀/排水/蠕动/UV/热风）
 *   5. confirm/cancel/emergency 边缘 + 生命周期（init/start/stop）
 *
 * 安全模型（与 actuator_self_test 同源）：
 *   - test_active 门禁：executor 空闲、无 UV/执行器板测、无 fault/emergency；
 *   - 每 tick 复核禁止输出全 OFF，违反即 fail-closed FAULT + 全关；
 *   - BLE 断线/teardown/急停 → motor_bench_service_emergency()（闩锁 + 全关）；
 *   - 非 COMPLETE 终态一律不声明"输出已确认关闭"（fail-closed 报告）。
 *
 * 任务栈放 PSRAM（coupled_hot_air 同型先例）：内部 RAM 预算紧张（BLE 控制器
 * init 需大块连续内部内存），避免重蹈 Group B BLE OOM 重启循环。
 */

#include "motor_bench_service.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "safety_manager.h"
#include "water_service.h"
#include "detergent_service.h"
#include "drain_service.h"
#include "dry_service.h"
#include "coupled_hot_air_service.h"
#include "uv_self_test.h"
#include "actuator_self_test.h"
#include "machine_config.h"
#include "wash_executor.h"
#include "position_service.h"
#include "bl50_service.h"

#define MB_TICK_MS               50U
#define MB_TASK_STACK            4096U
#define MB_TASK_PRIORITY         15U
#define MB_TASK_CORE             1U
#define MB_STOP_OFF_TIMEOUT_MS   2500U

/* 输出三态（与 dry/detergent/drain/uv fsm 公开约定一致：0=UNKNOWN 1=OFF 2=ON） */
#define MB_OUT_UNKNOWN 0
#define MB_OUT_OFF     1
#define MB_OUT_ON      2

#define MB_NOTIFY_RUN       (1U << 0)
#define MB_NOTIFY_EMERGENCY (1U << 1)

static const char *TAG = "motor_bench";

typedef struct {
    motor_bench_service_config_t cfg;
    bool initialized;
    bool started;

    TaskHandle_t task;
    SemaphoreHandle_t task_done_sem;
    _Atomic bool stop_requested;
    /* 锁竞争安全 emergency 闩锁：任何 emergency 入口先置位；任务每 tick 将其
     * 并入 FSM emergency 输入（core prelude → INTERRUPTED + 全关）。 */
    _Atomic bool emergency_latch;
    _Atomic bool test_active;

    machine_request_id_t next_request_id;
    uint32_t start_tick_ms;

    /* 纯核心 FSM（host-testable）：状态/终态/码的唯一权威。 */
    motor_bench_fsm_ctx_t fsm;

    /* 一次性确认/取消边缘（任务每 tick 消费即清，防陈旧自动提交）。 */
    _Atomic bool confirm_edge;
    _Atomic bool cancel_edge;

    /* 脉冲提交结果：本 tick 提交失败 → 下一 tick 作为 submit_failed 消费。 */
    bool submit_failed;
} motor_bench_rt_t;

static motor_bench_rt_t s_rt;

/* Mutex 在 s_rt 之外：init() memset 不得抹掉句柄。 */
static SemaphoreHandle_t s_mtx = NULL;
static StaticTask_t s_task_tcb;
static EXT_RAM_BSS_ATTR StackType_t s_task_stack[MB_TASK_STACK];

static bool lock(void)
{
    return s_mtx && xSemaphoreTake(s_mtx, pdMS_TO_TICKS(500)) == pdTRUE;
}

static esp_err_t mb_emergency_outputs(void);

/* P1-2 反向互斥：台架运行期间锁死生产洗涤程序提交（wash_executor 侧门禁）。
 * 洗涤执行器为 NULL 时无洗涤可提交，no-op。由受理置位、终态/拒绝/stop 复位。 */
static void mb_set_external_busy(bool busy)
{
    if (s_rt.cfg.executor)
        wash_executor_set_external_busy(s_rt.cfg.executor, busy);
}

/* ================================================================
 * 门禁输入采集（fail-closed）
 * ================================================================ */

static void collect_gate_input(motor_bench_gate_input_t *in,
                               drum_position_t required_pos)
{
    memset(in, 0, sizeof(*in));
    in->required_pos = required_pos;
    in->pos = DRUM_POS_UNKNOWN;

    /* 与 BL50 REAL 门禁使用同一配置真值，必须在用户确认脉冲前拒绝。
     * 这样不会出现 queue submit 成功、BL50 异步拒绝后被误报 SUBMIT_FAILED。 */
    const machine_config_t *mcfg = machine_config_get();
    if (mcfg) {
        in->direction_calibrated = mcfg->position.direction_calibrated;
    } else {
        in->snapshot_fail = true;
    }

    /* 无其他自检运行：UV 自检、执行器板测 任一运行 → busy。
     * 注意：台架自身 test_active 刻意不参与（经纯函数聚合）——受理后
     * test_active 置 true，若计入则门禁每 tick 自锁 SELF_TEST_BUSY，
     * 台架"一受理即 FAULT"（P0，板测 4 连发复现；host 纯 FSM 测不到此
     * 服务层聚合缺陷）。重入保护由 request() 顶部 test_active 检查独占。 */
    uint8_t uv_code = uv_self_test_state_code();
    bool uv_busy = uv_code == (uint8_t)UV_STATE_SELFTEST_VALIDATING ||
                   uv_code == (uint8_t)UV_STATE_SELFTEST_RUNNING ||
                   uv_code == (uint8_t)UV_STATE_SELFTEST_STOPPING;
    uint8_t act_code = actuator_self_test_state_code();
    bool act_busy = act_code == (uint8_t)ACT_STATE_CHECKING ||
                    act_code == (uint8_t)ACT_STATE_ACCEPTED ||
                    act_code == (uint8_t)ACT_STATE_RUNNING;
    in->self_test_busy = motor_bench_external_self_test_busy(uv_busy, act_busy);

    if (s_rt.cfg.executor) {
        wash_exec_snapshot_t snap;
        if (wash_executor_get_snapshot(s_rt.cfg.executor, &snap) == ESP_OK) {
            /* exec_idle = 生产洗涤完全空闲（无程序运行/待交付/已预约）。
             * program_reserved 须 live 读取：submit_program 在 wrapper 互斥量内
             * 置位，published 快照由任务发布、存在滞后；若不实时确认，台架可能在
             * 洗涤"已预约未激活"窗口内被受理，随后 EXECUTOR_BUSY 致命门禁会
             * 强关共享输出、中断一次合法洗涤（P1-2 反向互斥）。 */
            bool reserved = wash_executor_program_reserved(s_rt.cfg.executor);
            in->exec_idle = snap.state == WASH_EXEC_STATE_IDLE &&
                            !snap.program_active && !snap.terminal_pending &&
                            !reserved;
        } else {
            in->snapshot_fail = true;   /* executor 快照不可用 → fail-closed */
        }
    }

    safety_snapshot_t safety = {0};
    if (safety_manager_get_snapshot(&safety) == ESP_OK) {
        in->fault_active = safety.fault_active;
        in->emergency_active = safety.emergency_stop_active;
    } else {
        /* 安全快照不可用：fault/emergency 未确认 → fail-closed（规则 6）。
         * 必须置 snapshot_fail，否则 fault/emergency 会被静默当作 false，
         * 门禁可能错误放行（曾作为 P1 缺陷真实发生）。 */
        in->snapshot_fail = true;
    }

    safety_position_view_t view;
    if (safety_manager_get_position_view(&view) == ESP_OK) {
        in->pos_valid = view.sample_valid && drum_position_is_valid(view.position);
        in->pos_fresh = view.fresh;
        in->pos_stable = view.stable;
        in->pos_moving = view.motor_moving;
        in->pos = view.position;
    }

    position_snapshot_t ps = {0};
    bool pos_ok = (position_service_get_snapshot(&ps) == ESP_OK);
    if (pos_ok) {
        /* 位置服务空闲：IDLE_KNOWN/IDLE_UNKNOWN 均算空闲；MOVING/BRAKING/
         * STOPPING 视为运动，FAULT 由 snapshot_fail/hall_conflict 表达。 */
        in->position_service_idle =
            (ps.state == POSITION_STATE_IDLE_KNOWN ||
             ps.state == POSITION_STATE_IDLE_UNKNOWN);
        in->position_moving =
            (ps.state == POSITION_STATE_MOVING ||
             ps.state == POSITION_STATE_BRAKING ||
             ps.state == POSITION_STATE_STOPPING);
        in->raw_hall_mask = ps.raw_hall_mask;
        in->stable_hall_mask = ps.stable_hall_mask;
        /* 多霍尔矛盾：稳定掩码多位置同时有效，或位置服务已锁存 HALL_CONFLICT
         * 故障。fail-closed：绝不把矛盾位置当作可确认位置。 */
        uint8_t m = ps.stable_hall_mask;
        bool multi = (m & (m - 1U)) != 0U;   /* >1 bit set */
        in->hall_conflict = multi ||
            (ps.state == POSITION_STATE_FAULT && ps.fault.code == FAULT_HALL_CONFLICT);
    } else {
        in->snapshot_fail = true;
    }

    bl50_snapshot_t bs = {0};
    bool bl50_ok = (bl50_service_get_snapshot(&bs) == ESP_OK);
    if (bl50_ok) {
        in->bl50_idle = (bs.state == BL50_SVC_STATE_IDLE);
        in->bl50_pwm = bs.current_pwm_percent;
        in->bl50_fault = (bs.state == BL50_SVC_STATE_FAULT);
    } else {
        in->snapshot_fail = true;
    }

    /* forbidden_outputs_off 聚合子句（fail-closed：基值 true，任一未确认 → false）。
     * 经纯函数 motor_bench_forbidden_outputs_off 计算（host-testable）。
     * 注意：不得以"从 false 起 AND"的写法聚合——那会令结果恒 false（P0 缺陷）。 */
    motor_bench_forbidden_clauses_t fb = {0};

    water_snapshot_t ws = {0};
    bool water_ok = (water_service_get_snapshot(&ws) == ESP_OK);
    if (water_ok) {
        in->water_service_idle =
            ws.state == WATER_STATE_IDLE && !ws.act_test_active &&
            !ws.terminal_pending;
        in->water_safe_off_synced = ws.safe_off_synced;
        fb.water_valves_off =
            (!ws.source_valve_on && !ws.source_valve_unknown) &&
            (!ws.transfer_valve_on && !ws.transfer_valve_unknown);
    } else {
        in->snapshot_fail = true;
    }

    detergent_snapshot_t ds = {0};
    bool det_ok = (detergent_service_get_snapshot(&ds) == ESP_OK);
    if (det_ok) {
        in->detergent_service_idle =
            ds.state == DETERGENT_STATE_IDLE && !ds.terminal_pending;
        fb.detergent_off = (ds.output_state == MB_OUT_OFF);
    } else {
        in->snapshot_fail = true;
    }

    drain_snapshot_t dr = {0};
    bool drain_ok = (drain_service_get_snapshot(&dr) == ESP_OK);
    if (drain_ok) {
        in->drain_service_idle =
            dr.state == DRAIN_STATE_IDLE && !dr.terminal_pending;
        fb.drain_off = (dr.output_state == MB_OUT_OFF);
    } else {
        in->snapshot_fail = true;
    }

    dry_snapshot_t dy = {0};
    bool dry_ok = (dry_service_get_snapshot(&dy) == ESP_OK);
    if (dry_ok) {
        in->dry_service_idle =
            dy.state == DRY_STATE_IDLE && !dy.terminal_pending;
        fb.dry_off = (dy.fan_output_state == MB_OUT_OFF) &&
                     (dy.heater_output_state == MB_OUT_OFF);
    } else {
        in->snapshot_fail = true;
    }

    coupled_hot_air_snapshot_t cha = {0};
    bool cha_ok = (coupled_hot_air_service_get_snapshot(&cha) == ESP_OK);
    if (cha_ok) {
        in->hot_air_service_idle = (cha.state == CHA_STATE_IDLE_OFF);
        fb.hot_air_off = cha.output_known && cha.output_confirmed_off;
    } else {
        in->snapshot_fail = true;
    }

    /* UV：MCP 输出，须 KNOWN_OFF（safety 快照位）。fault/emergency 激活时不应用。 */
    fb.uv_clause_applicable = (in->emergency_active == false &&
                               in->fault_active == false);
    if (fb.uv_clause_applicable) {
        fb.uv_off =
            (safety.mcp_known_mask & (1U << (unsigned)SAFE_OUTPUT_UV)) != 0U &&
            !safety.mcp_outputs.uv;
    }
    in->forbidden_outputs_off = motor_bench_forbidden_outputs_off(&fb);

    in->request_id_ok = (s_rt.next_request_id != MACHINE_REQUEST_ID_INVALID);
}

/* ================================================================
 * FSM 输入构建
 * ================================================================ */

static void build_fsm_input(motor_bench_fsm_input_t *fin,
                            const motor_bench_gate_input_t *gin,
                            motor_bench_gate_result_t gate)
{
    memset(fin, 0, sizeof(*fin));
    fin->now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    fin->gate = gate;
    fin->bl50_idle = gin->bl50_idle;
    fin->bl50_pwm = gin->bl50_pwm;
    fin->bl50_fault = gin->bl50_fault;
    fin->forbidden_outputs_off = gin->forbidden_outputs_off;
    fin->submit_failed = s_rt.submit_failed;
    s_rt.submit_failed = false;

    /* emergency：闩锁（BLE 断线/急停）或 safety 层急停闩锁。 */
    fin->emergency = gin->emergency_active ||
                     atomic_load_explicit(&s_rt.emergency_latch,
                                          memory_order_acquire);

    /* 取消边缘：消费即清。 */
    fin->cancel_requested = atomic_exchange(&s_rt.cancel_edge, false);

    /* 确认边缘：仅当位置此刻 OK 且处于 WAIT_CONFIRM 才生效，防陈旧自动提交。 */
    fin->user_confirm_pulse =
        atomic_exchange(&s_rt.confirm_edge, false) &&
        gate == MOTOR_BENCH_GATE_OK &&
        s_rt.fsm.state == MOTOR_BENCH_STATE_WAIT_CONFIRM;
}

/* ================================================================
 * 输出动作
 * ================================================================ */

/* 请求全部禁止输出 OFF + BL50/IBT-2 急停。返回 ESP_OK 表示全部请求成功。 */
static esp_err_t mb_emergency_outputs(void)
{
    esp_err_t r = ESP_OK;
    esp_err_t e;
    e = bl50_service_emergency_stop();
    if (e != ESP_OK) r = e;
    e = position_service_emergency_stop();
    if (e != ESP_OK) r = e;
    e = uv_self_test_emergency();
    if (e != ESP_OK) r = e;
    e = water_service_emergency_close();
    if (e != ESP_OK) r = e;
    e = detergent_service_emergency_stop();
    if (e != ESP_OK) r = e;
    e = drain_service_emergency_stop();
    if (e != ESP_OK) r = e;
    e = dry_service_emergency_stop();
    if (e != ESP_OK) r = e;
    e = coupled_hot_air_service_emergency_stop();
    if (e != ESP_OK) r = e;
    return r;
}

/* 提交一次低能量短脉冲到真实 bl50_service（生产动作适配器）。 */
static void mb_submit_pulse_locked(void)
{
    const motor_bench_step_t *step = &s_rt.fsm.steps[s_rt.fsm.current_step];
    bool behavior =
        (s_rt.fsm.type == MOTOR_BENCH_TYPE_PULSATOR_BEHAVIOR);
    uint32_t pulse_pwm_u32 = 0U;
    const machine_config_t *mcfg = machine_config_get();
    if (mcfg) pulse_pwm_u32 = mcfg->benchtst.bl50_pwm;
    /* 台架脉冲 PWM：由 machine_config.benchtst.bl50_pwm 决定（滑杆/NVS 可调，
     * 默认 40）。0 或超范围 → 回退编译默认。行为模式同档。 */
    uint8_t pulse_pwm = (pulse_pwm_u32 >= 1U && pulse_pwm_u32 <= 100U)
        ? (uint8_t)pulse_pwm_u32 : MOTOR_BENCH_PULSE_PWM_PERCENT;
    bl50_request_t req = {
        .request_id = s_rt.fsm.request_id,
        .action = step->action,
        .intensity = WASH_INTENSITY_LOW,
        .duration_ms = behavior ? MOTOR_BENCH_BEHAVIOR_DURATION_MS
                                : MOTOR_BENCH_PULSE_DURATION_MS,
        .target_pwm_percent = pulse_pwm,
        /* 行为模式：按生产波轮逻辑每 2s 换向（转-刹-反向交替）；
         * 普通短脉冲不反转（direction_interval=0 → 默认 5s 间隔 > 脉冲时长）。 */
        .direction_interval_ms = behavior ? MOTOR_BENCH_BEHAVIOR_DIR_INTERVAL_MS
                                          : 0U,
    };
    esp_err_t err = bl50_service_run_async(&req);
    s_rt.submit_failed = (err != ESP_OK);
    ESP_LOGI(TAG, "MB submit_pulse action=%d rid=%" PRIu32
             " pwm=%u dur=%u dir_int=%u err=0x%x",
             (int)step->action, s_rt.fsm.request_id,
             (unsigned)req.target_pwm_percent, (unsigned)req.duration_ms,
             (unsigned)req.direction_interval_ms,
             (unsigned)err);
}

static void handle_fsm_out(const motor_bench_fsm_output_t *out)
{
    if (out->submit_pulse) {
        mb_submit_pulse_locked();
    }
    if (out->request_all_off) {
        esp_err_t e = mb_emergency_outputs();
        if (e != ESP_OK) {
            /* 输出未确认关闭 → fail-closed：终态保持 FSM 原值（非 COMPLETE
             * 一律不声明 output_confirmed_off），此处仅醒目记录。 */
            ESP_LOGE(TAG, "MB request_all_off failed err=0x%x", (unsigned)e);
        }
    }
}

/* ================================================================
 * tick 驱动
 * ================================================================ */

static void mb_drive_locked(void)
{
    if (motor_bench_fsm_is_terminal(&s_rt.fsm)) {
        atomic_store(&s_rt.test_active, false);
        mb_set_external_busy(false);   /* P1-2：终态 → 恢复洗涤可提交 */
        return;
    }

    uint8_t step = s_rt.fsm.current_step;
    drum_position_t req_pos = (step < s_rt.fsm.step_count)
        ? s_rt.fsm.steps[step].required_position : DRUM_POS_UNKNOWN;

    motor_bench_gate_input_t gin;
    collect_gate_input(&gin, req_pos);
    motor_bench_gate_result_t gate = motor_bench_gate_check(&gin);

    motor_bench_fsm_input_t fin;
    build_fsm_input(&fin, &gin, gate);
    motor_bench_fsm_output_t out = {0};
    motor_bench_fsm_tick(&s_rt.fsm, &fin, &out);
    handle_fsm_out(&out);

    if (motor_bench_fsm_is_terminal(&s_rt.fsm)) {
        atomic_store(&s_rt.test_active, false);
        mb_set_external_busy(false);   /* P1-2：终态 → 恢复洗涤可提交 */
        ESP_LOGI(TAG, "MB terminal type=%s state=%s code=%s",
                 motor_bench_type_name(s_rt.fsm.type),
                 motor_bench_state_name(s_rt.fsm.state),
                 s_rt.fsm.last_code ? s_rt.fsm.last_code : "?");
    }
}

/* ---- 任务 ---- */

static void mb_task(void *arg)
{
    (void)arg;
    while (!atomic_load(&s_rt.stop_requested)) {
        uint32_t notify = 0;
        TickType_t wait = atomic_load(&s_rt.test_active)
            ? pdMS_TO_TICKS(MB_TICK_MS)
            : pdMS_TO_TICKS(250);
        xTaskNotifyWait(0, MB_NOTIFY_RUN | MB_NOTIFY_EMERGENCY, &notify, wait);

        if (notify & MB_NOTIFY_EMERGENCY) {
            if (lock()) {
                if (atomic_load(&s_rt.test_active)) {
                    esp_err_t off_err = mb_emergency_outputs();
                    atomic_store(&s_rt.emergency_latch, true);
                    ESP_LOGW(TAG, "MB emergency notify off_err=0x%x",
                             (unsigned)off_err);
                }
                xSemaphoreGive(s_mtx);
            }
        }

        if (lock()) {
            if (atomic_load(&s_rt.test_active)) mb_drive_locked();
            xSemaphoreGive(s_mtx);
        }
    }
    xSemaphoreGive(s_rt.task_done_sem);
    vTaskDelete(NULL);
}

/* ---- 生命周期 ---- */

esp_err_t motor_bench_service_global_init(void)
{
    if (!s_mtx) {
        s_mtx = xSemaphoreCreateMutex();
        if (!s_mtx) return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t motor_bench_service_init(const motor_bench_service_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    if (!s_mtx) {
        s_mtx = xSemaphoreCreateMutex();
        if (!s_mtx) return ESP_ERR_NO_MEM;
    }
    if (!lock()) return ESP_ERR_TIMEOUT;
    if (s_rt.initialized) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_rt, 0, sizeof(s_rt));
    s_rt.cfg = *config;
    s_rt.next_request_id = 1;
    s_rt.initialized = true;
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

esp_err_t motor_bench_service_start(void)
{
    if (!lock()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.initialized || s_rt.started) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    s_rt.started = true;
    /* 每次启动作废旧 task_done_sem，避免旧任务残留信号被新实例消费。 */
    if (s_rt.task_done_sem) vSemaphoreDelete(s_rt.task_done_sem);
    s_rt.task_done_sem = xSemaphoreCreateBinary();
    if (!s_rt.task_done_sem) {
        s_rt.started = false;
        xSemaphoreGive(s_mtx);
        return ESP_ERR_NO_MEM;
    }
    atomic_store(&s_rt.stop_requested, false);
    TaskHandle_t handle = xTaskCreateStaticPinnedToCore(
        mb_task, "mb_task", MB_TASK_STACK, NULL,
        MB_TASK_PRIORITY, s_task_stack, &s_task_tcb, MB_TASK_CORE);
    if (!handle) {
        s_rt.started = false;
        xSemaphoreGive(s_mtx);
        return ESP_ERR_NO_MEM;
    }
    s_rt.task = handle;
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

/* Fail-closed OFF 确认：任一快照失败或输出 UNKNOWN → "无法证明 OFF"，
 * stop() 视为未关闭。绝不被解读为"一切已关闭"。 */
static bool mb_outputs_confirmed_off(void)
{
    water_snapshot_t ws;
    bool wok = water_service_get_snapshot(&ws) == ESP_OK;
    detergent_snapshot_t ds;
    bool dok = detergent_service_get_snapshot(&ds) == ESP_OK;
    drain_snapshot_t dr;
    bool rok = drain_service_get_snapshot(&dr) == ESP_OK;
    dry_snapshot_t dy;
    bool yok = dry_service_get_snapshot(&dy) == ESP_OK;
    coupled_hot_air_snapshot_t cha;
    bool cok = coupled_hot_air_service_get_snapshot(&cha) == ESP_OK;
    bl50_snapshot_t bs;
    bool bok = bl50_service_get_snapshot(&bs) == ESP_OK;
    position_snapshot_t ps;
    bool pok = position_service_get_snapshot(&ps) == ESP_OK;
    safety_snapshot_t safe;
    bool sok = safety_manager_get_snapshot(&safe) == ESP_OK;

    if (!wok || !dok || !rok || !yok || !cok || !bok || !pok || !sok) {
        return false;
    }
    bool valves_off = (!ws.source_valve_on && !ws.source_valve_unknown) &&
                      (!ws.transfer_valve_on && !ws.transfer_valve_unknown) &&
                      ws.safe_off_synced;
    bool det_off = ds.output_state == MB_OUT_OFF;
    bool drain_off = dr.output_state == MB_OUT_OFF;
    bool dry_off = dy.fan_output_state == MB_OUT_OFF &&
                   dy.heater_output_state == MB_OUT_OFF;
    bool cha_off = cha.output_known && cha.output_confirmed_off;
    bool bl50_off = bs.state == BL50_SVC_STATE_IDLE &&
                    bs.current_pwm_percent == 0U;
    bool pos_off = ps.state == POSITION_STATE_IDLE_KNOWN ||
                   ps.state == POSITION_STATE_IDLE_UNKNOWN;
    bool uv_off = (safe.mcp_known_mask & (1U << (unsigned)SAFE_OUTPUT_UV)) != 0U &&
                  !safe.mcp_outputs.uv;
    return valves_off && det_off && drain_off && dry_off && cha_off &&
           bl50_off && pos_off && uv_off;
}

esp_err_t motor_bench_service_stop(void)
{
    if (!s_mtx) return ESP_ERR_INVALID_STATE;
    if (!lock()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.initialized) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    /* 先锁存 stop_requested 与 started=false：停止窗口内并发 request() 必须被
     * 拒绝，避免 OFF 确认期间重新上电。 */
    atomic_store(&s_rt.stop_requested, true);
    s_rt.started = false;
    mb_set_external_busy(false);   /* P1-2：服务停止 → 恢复洗涤可提交 */
    TaskHandle_t task = s_rt.task;
    /* 上次 stop() 超时后任务可能已自行退出。非阻塞 take task_done_sem 证明
     * 任务已给信号并即将自删；take 失败 ⇒ 任务仍存活，通知安全（UAF 防护）。 */
    if (task && s_rt.task_done_sem &&
        xSemaphoreTake(s_rt.task_done_sem, 0) == pdTRUE) {
        s_rt.task = NULL;
        task = NULL;
    }
    if (task) xTaskNotify(task, MB_NOTIFY_EMERGENCY, eSetBits);
    xSemaphoreGive(s_mtx);

    if (!mb_outputs_confirmed_off()) {
        mb_emergency_outputs();
        TickType_t deadline =
            xTaskGetTickCount() + pdMS_TO_TICKS(MB_STOP_OFF_TIMEOUT_MS);
        bool confirmed = false;
        while (xTaskGetTickCount() < deadline) {
            if (mb_outputs_confirmed_off()) { confirmed = true; break; }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (!confirmed) {
            ESP_LOGE(TAG, "stop: outputs not confirmed OFF; preserving lifecycle");
            return ESP_ERR_TIMEOUT;
        }
    }

    /* 在互斥量之外 join：任务自身需要该互斥量才能退出。 */
    if (task) {
        if (s_rt.task_done_sem &&
            xSemaphoreTake(s_rt.task_done_sem, pdMS_TO_TICKS(2000)) != pdTRUE) {
            ESP_LOGE(TAG, "stop: task did not exit; preserve lifecycle for retry");
            return ESP_ERR_TIMEOUT;
        }
    }
    if (!lock()) return ESP_ERR_INVALID_STATE;
    if (s_rt.task_done_sem) {
        vSemaphoreDelete(s_rt.task_done_sem);
        s_rt.task_done_sem = NULL;
    }
    s_rt.task = NULL;
    s_rt.started = false;
    s_rt.initialized = false;
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

/* ---- 受理 ---- */

esp_err_t motor_bench_service_request(motor_bench_type_t type,
                                      const char **out_code)
{
    if (out_code) *out_code = NULL;
    if (!lock()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.initialized || !s_rt.started) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    if (atomic_load(&s_rt.test_active)) {
        xSemaphoreGive(s_mtx);
        if (out_code) *out_code = "SELF_TEST_BUSY";
        return ESP_OK;
    }

    /* 新一次台架：清陈旧闩锁/边缘/提交结果（不覆盖系统级 fault/emergency）。 */
    atomic_store(&s_rt.emergency_latch, false);
    atomic_store(&s_rt.confirm_edge, false);
    atomic_store(&s_rt.cancel_edge, false);
    s_rt.submit_failed = false;

    /* P1-2：受理尝试一开始即锁死洗涤提交（在门禁检查之前，闭合"洗涤已预约未激活"
     * 的竞态窗口——此后任何新洗涤程序被 wash_executor 拒绝）。任一拒绝路径必须复位，
     * 以免洗涤被永久锁死。 */
    mb_set_external_busy(true);

    motor_bench_step_t steps[MOTOR_BENCH_MAX_STEPS];
    uint8_t step_count = 0;
    if (!motor_bench_build_default_program(type, bl50_service_action_to_position,
                                           steps, &step_count) ||
        step_count == 0U) {
        mb_set_external_busy(false);
        xSemaphoreGive(s_mtx);
        if (out_code) *out_code = "UNKNOWN_TARGET";
        return ESP_OK;
    }

    machine_request_id_t rid = machine_request_id_next(s_rt.next_request_id);
    s_rt.next_request_id = rid;

    /* 重新初始化纯 FSM（清陈旧终态），并同步跑一次 PRECHECK。 */
    motor_bench_fsm_init(&s_rt.fsm, type, steps, step_count, rid);

    drum_position_t req_pos = s_rt.fsm.steps[0].required_position;
    motor_bench_gate_input_t gin;
    collect_gate_input(&gin, req_pos);
    motor_bench_gate_result_t gate = motor_bench_gate_check(&gin);

    motor_bench_fsm_input_t fin;
    build_fsm_input(&fin, &gin, gate);
    motor_bench_fsm_output_t out = {0};
    motor_bench_fsm_tick(&s_rt.fsm, &fin, &out);
    handle_fsm_out(&out);

    if (motor_bench_fsm_is_terminal(&s_rt.fsm)) {
        /* 受理拒绝：无任何输出被本测试触发。返回 FSM 门禁码。 */
        mb_set_external_busy(false);   /* P1-2：拒绝 → 恢复洗涤可提交 */
        const char *code = s_rt.fsm.last_code ? s_rt.fsm.last_code : "REJECTED";
        ESP_LOGI(TAG, "MB request reject type=%s code=%s pos=%d valid=%d "
                 "fresh=%d stable=%d moving=%d fault=%d emerg=%d exec_idle=%d "
                 "self_test=%d bl50_idle=%d pwm=%d pos_svc_idle=%d forb_off=%d",
                 motor_bench_type_name(type), code, (int)gin.pos,
                 gin.pos_valid ? 1 : 0, gin.pos_fresh ? 1 : 0,
                 gin.pos_stable ? 1 : 0, gin.pos_moving ? 1 : 0,
                 gin.fault_active ? 1 : 0, gin.emergency_active ? 1 : 0,
                 gin.exec_idle ? 1 : 0, gin.self_test_busy ? 1 : 0,
                 gin.bl50_idle ? 1 : 0, (int)gin.bl50_pwm,
                 gin.position_service_idle ? 1 : 0,
                 gin.forbidden_outputs_off ? 1 : 0);
        xSemaphoreGive(s_mtx);
        if (out_code) *out_code = code;
        return ESP_OK;
    }

    /* 受理成功 → WAIT_POSITION：等待用户手动摆放。绝不自动驱动电机。 */
    s_rt.start_tick_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    atomic_store(&s_rt.test_active, true);
    if (s_rt.task) xTaskNotify(s_rt.task, MB_NOTIFY_RUN, eSetBits);
    xSemaphoreGive(s_mtx);

    ESP_LOGI(TAG, "MB request ACCEPTED type=%s rid=%" PRIu32
             " steps=%u pos0=%d",
             motor_bench_type_name(type), rid,
             (unsigned)s_rt.fsm.step_count, (int)req_pos);
    if (out_code) *out_code = "MOTOR_BENCH_ACCEPTED";
    return ESP_OK;
}

/* ---- 用户交互 ---- */

esp_err_t motor_bench_service_confirm_pulse(void)
{
    if (!lock()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.initialized || !s_rt.started ||
        !atomic_load(&s_rt.test_active)) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_rt.fsm.state != MOTOR_BENCH_STATE_WAIT_CONFIRM) {
        /* 仅 WAIT_CONFIRM 消费；位置未就位/已提交则忽略。 */
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store(&s_rt.confirm_edge, true);
    if (s_rt.task) xTaskNotify(s_rt.task, MB_NOTIFY_RUN, eSetBits);
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

esp_err_t motor_bench_service_cancel(void)
{
    if (!lock()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.initialized || !s_rt.started ||
        !atomic_load(&s_rt.test_active)) {
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store(&s_rt.cancel_edge, true);
    if (s_rt.task) xTaskNotify(s_rt.task, MB_NOTIFY_RUN, eSetBits);
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

esp_err_t motor_bench_service_emergency(void)
{
    if (!s_mtx) return ESP_ERR_INVALID_STATE;

    /* 先置位闩锁（与互斥量无关）：任务在锁内恢复后经 FSM 给出 INTERRUPTED。 */
    atomic_store_explicit(&s_rt.emergency_latch, true, memory_order_release);

    /* 不依赖互斥量的后备关断：直接请求全部输出 OFF。 */
    esp_err_t off_err = mb_emergency_outputs();
    esp_err_t r = off_err;

    if (lock()) {
        TaskHandle_t t = s_rt.task;
        xSemaphoreGive(s_mtx);
        if (t) xTaskNotify(t, MB_NOTIFY_EMERGENCY, eSetBits);
    } else {
        /* 互斥量被占用：闩锁 + 无锁 OFF 已覆盖关断，但状态机未在锁内更新。
         * 不得返回 ESP_OK（避免调用方误以为状态已落定）。 */
        if (r == ESP_OK) r = ESP_ERR_TIMEOUT;
    }
    return r;
}

esp_err_t motor_bench_service_get_snapshot(motor_bench_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!lock()) return ESP_ERR_INVALID_STATE;
    if (!s_rt.initialized) {
        memset(out, 0, sizeof(*out));
        xSemaphoreGive(s_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    const motor_bench_fsm_ctx_t *fsm = &s_rt.fsm;
    out->state = fsm->state;
    out->type = fsm->type;
    out->request_id = fsm->request_id;
    out->current_step = fsm->current_step;
    out->step_count = fsm->step_count;
    out->terminal = fsm->terminal;
    out->last_code = fsm->last_code ? fsm->last_code : "";
    out->output_confirmed_off = fsm->output_confirmed_off;
    if (fsm->step_count > 0U && fsm->current_step < fsm->step_count) {
        out->required_position = fsm->steps[fsm->current_step].required_position;
        out->current_action = fsm->steps[fsm->current_step].action;
    } else {
        out->required_position = DRUM_POS_UNKNOWN;
        out->current_action = BL50_ACTION_HOME;
    }
    out->position_ready = (fsm->state == MOTOR_BENCH_STATE_WAIT_CONFIRM);
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    out->elapsed_ms = (now_ms > s_rt.start_tick_ms)
        ? (now_ms - s_rt.start_tick_ms) : 0U;
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

uint8_t motor_bench_service_state_code(void)
{
    motor_bench_snapshot_t snap;
    if (motor_bench_service_get_snapshot(&snap) != ESP_OK)
        return (uint8_t)MOTOR_BENCH_STATE_INACTIVE;
    return (uint8_t)snap.state;
}
