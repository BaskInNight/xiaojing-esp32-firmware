/*
 * program_control_core.c — 程序控制 纯 C 模型实现
 *
 * 纯函数/可重入：无 FreeRTOS/HAL/NVS/GPIO，无静态可变状态，无动态内存。
 * 与 host 测试共用权威逻辑（recipe/仲裁/BTN1/手势/审计）。
 */

#include "program_control_core.h"
#include <string.h>

/* ================================================================
 * DEFAULT_DEMO_V1 规范阶段表（26 阶段）
 *
 * 位置契约与生产服务同源（权威：bl50_service_action_to_position 及
 * wash_planner 的 action_required_position）：
 *   UV→0 / BL50 HOME→45 / PULSATOR→0 / DRUM→90 / SPIN→180
 *   WATER+DETERGENT→0 / DRAIN→180 / DRY→270
 * ================================================================ */

#define DEMO_MOVE_TIMEOUT_MS       30000U   /* 换位超时兜底（config 优先） */
#define DEMO_HOME_TIMEOUT_MS       15000U
#define DEMO_WATER_TIMEOUT_MS      120000U  /* 源箱批上限 + 沉降 + 裕量 */
#define DEMO_SERVICE_GRACE_MS      60000U
#define DEMO_DRY_GRACE_MS          120000U
#define DEMO_TERMINAL_GRACE_MS     5000U
#define DEMO_INTERNAL_TIMEOUT_MS   5000U

static const default_demo_phase_t s_default_demo_phases[DEFAULT_DEMO_PHASE_COUNT] = {
    /* 1  order acceptance（等待用户确认） */
    { DEMO_PHASE_ORDER_ACCEPT,  STEP_WAIT_LOAD_CONFIRM, DRUM_POS_UNKNOWN,
      0, WASH_INTENSITY_NORMAL, 0 },
    /* 2  UV（空桶杀菌 @0） */
    { DEMO_PHASE_UV,            STEP_UV,            DRUM_POS_0,
      DEFAULT_DEMO_UV_MS, WASH_INTENSITY_NORMAL, 0 },
    /* 3  precheck（BL50 HOME @45） */
    { DEMO_PHASE_PRECHECK,      STEP_HOME_BL50,     DRUM_POS_45,
      DEFAULT_DEMO_PRECHECK_MS, WASH_INTENSITY_NORMAL, 0 },
    /* 4  position 0° */
    { DEMO_PHASE_POS_0,         STEP_MOVE_POSITION, DRUM_POS_0,
      0, WASH_INTENSITY_NORMAL, 0 },
    /* 5  water fill */
    { DEMO_PHASE_WATER,         STEP_WATER_IN,      DRUM_POS_0,
      DEFAULT_DEMO_WATER_MS, WASH_INTENSITY_NORMAL, 0 },
    /* 6  detergent */
    { DEMO_PHASE_DETERGENT,     STEP_DETERGENT,     DRUM_POS_0,
      DEFAULT_DEMO_DETERGENT_MS, WASH_INTENSITY_NORMAL, 0 },
    /* 7  pulsator prewash */
    { DEMO_PHASE_PREWASH,       STEP_PULSATOR_WASH, DRUM_POS_0,
      DEFAULT_DEMO_PREWASH_MS, WASH_INTENSITY_GENTLE, 0 },
    /* 8  settle */
    { DEMO_PHASE_SETTLE_1,      STEP_SETTLE,        DRUM_POS_0,
      DEFAULT_DEMO_SETTLE_MS, WASH_INTENSITY_NORMAL, 0 },
    /* 9  position 90° */
    { DEMO_PHASE_POS_90,        STEP_MOVE_POSITION, DRUM_POS_90,
      0, WASH_INTENSITY_NORMAL, 0 },
    /* 10 drum main wash */
    { DEMO_PHASE_DRUM,          STEP_DRUM_WASH,     DRUM_POS_90,
      DEFAULT_DEMO_DRUM_MS, WASH_INTENSITY_NORMAL, 0 },
    /* 11 settle */
    { DEMO_PHASE_SETTLE_2,      STEP_SETTLE,        DRUM_POS_90,
      DEFAULT_DEMO_SETTLE_MS, WASH_INTENSITY_NORMAL, 0 },
    /* 12 position 180° */
    { DEMO_PHASE_POS_180,       STEP_MOVE_POSITION, DRUM_POS_180,
      0, WASH_INTENSITY_NORMAL, 0 },
    /* 13 drain */
    { DEMO_PHASE_DRAIN,         STEP_DRAIN,         DRUM_POS_180,
      DEFAULT_DEMO_DRAIN_MS, WASH_INTENSITY_NORMAL, 0 },
    /* 14 spin ramp */
    { DEMO_PHASE_SPIN_RAMP,     STEP_SPIN,          DRUM_POS_180,
      DEFAULT_DEMO_SPIN_RAMP_MS, WASH_INTENSITY_GENTLE, 0 },
    /* 15 spin hold */
    { DEMO_PHASE_SPIN_HOLD,     STEP_SPIN,          DRUM_POS_180,
      DEFAULT_DEMO_SPIN_HOLD_MS, WASH_INTENSITY_NORMAL, 0 },
    /* 16 spin ramp_down */
    { DEMO_PHASE_SPIN_RAMP_DOWN, STEP_SPIN,         DRUM_POS_180,
      DEFAULT_DEMO_SPIN_RAMP_DOWN_MS, WASH_INTENSITY_GENTLE, 0 },
    /* 17 settle */
    { DEMO_PHASE_SETTLE_3,      STEP_SETTLE,        DRUM_POS_180,
      DEFAULT_DEMO_SETTLE_MS, WASH_INTENSITY_NORMAL, 0 },
    /* 18 position 270° */
    { DEMO_PHASE_POS_270,       STEP_MOVE_POSITION, DRUM_POS_270,
      0, WASH_INTENSITY_NORMAL, 0 },
    /* 19 hot air tumble（PTC 加热） */
    { DEMO_PHASE_HOT_AIR,       STEP_DRY,           DRUM_POS_270,
      DEFAULT_DEMO_HOT_AIR_MS, WASH_INTENSITY_NORMAL, WASH_STEP_FLAG_HEATER_ON },
    /* 20 cooling tumble（仅风机） */
    { DEMO_PHASE_COOL_TUMBLE,   STEP_DRY,           DRUM_POS_270,
      DEFAULT_DEMO_COOL_TUMBLE_MS, WASH_INTENSITY_NORMAL, 0 },
    /* 21 settle */
    { DEMO_PHASE_SETTLE_4,      STEP_SETTLE,        DRUM_POS_270,
      DEFAULT_DEMO_SETTLE_MS, WASH_INTENSITY_NORMAL, 0 },
    /* 22 position 45° */
    { DEMO_PHASE_POS_45,        STEP_MOVE_POSITION, DRUM_POS_45,
      0, WASH_INTENSITY_NORMAL, 0 },
    /* 23 UV off audit */
    { DEMO_PHASE_UV_OFF_AUDIT,  STEP_AUDIT_UV_OFF,  DRUM_POS_45,
      0, WASH_INTENSITY_NORMAL, 0 },
    /* 24 safe-off audit */
    { DEMO_PHASE_SAFE_OFF_AUDIT, STEP_AUDIT_SAFE_OFF, DRUM_POS_45,
      0, WASH_INTENSITY_NORMAL, 0 },
    /* 25 wait for unload */
    { DEMO_PHASE_WAIT_UNLOAD,   STEP_WAIT_UNLOAD_CONFIRM, DRUM_POS_45,
      0, WASH_INTENSITY_NORMAL, 0 },
    /* 26 complete */
    { DEMO_PHASE_COMPLETE,      STEP_FINISH,        DRUM_POS_45,
      0, WASH_INTENSITY_NORMAL, 0 },
};

const default_demo_phase_t *default_demo_phase_table(void)
{
    return s_default_demo_phases;
}

/* ---- 步骤是否受桶位门禁约束（与 wash_planner action_required_position 同源） ---- */

static bool step_is_position_gated(wash_step_type_t type)
{
    switch (type) {
    case STEP_MOVE_POSITION:
    case STEP_HOME_BL50:
    case STEP_WATER_IN:
    case STEP_DETERGENT:
    case STEP_PULSATOR_WASH:
    case STEP_DRUM_WASH:
    case STEP_DRAIN:
    case STEP_SPIN:
    case STEP_DRY:
    case STEP_UV:
        return true;
    default:
        return false;
    }
}

static bool append_step(wash_program_t *prog, const wash_step_t *step)
{
    if (prog->step_count >= WASH_PROGRAM_MAX_STEPS) return false;
    uint16_t id = (uint16_t)prog->step_count;
    prog->steps[id] = *step;
    prog->steps[id].step_id = id;
    prog->step_count = (size_t)(id + 1);
    return true;
}

planner_report_t default_demo_build_program(
    const machine_config_t *config,
    uint32_t program_id,
    drum_position_t initial_position,
    wash_program_t *out_program)
{
    planner_report_t report;
    memset(&report, 0, sizeof(report));
    if (!out_program) {
        report.result = PLAN_RESULT_REJECTED;
        report.reject_reason = PLAN_ERROR_INVALID_ARG;
        return report;
    }
    memset(out_program, 0, sizeof(*out_program));
    out_program->program_id = program_id;
    out_program->kind = WASH_PROGRAM_DEMO;

    drum_position_t current = initial_position;
    uint32_t move_timeout = (config && config->position.move_timeout_ms > 0)
        ? config->position.move_timeout_ms : DEMO_MOVE_TIMEOUT_MS;
    uint32_t total_ms = 0;

    for (size_t i = 0; i < DEFAULT_DEMO_PHASE_COUNT; ++i) {
        const default_demo_phase_t *ph = &s_default_demo_phases[i];

        /* 位置门禁：非 MOVE 的服务步骤若当前位置不符，先插入系统 MOVE。 */
        if (ph->step_type != STEP_MOVE_POSITION &&
            step_is_position_gated(ph->step_type) &&
            ph->pos != DRUM_POS_UNKNOWN && ph->pos != current) {
            wash_step_t mv;
            memset(&mv, 0, sizeof(mv));
            mv.type = STEP_MOVE_POSITION;
            mv.required_position = ph->pos;
            mv.timeout_ms = move_timeout;
            mv.flags = WASH_STEP_FLAG_SYSTEM_INSERTED;
            if (!append_step(out_program, &mv)) {
                report.result = PLAN_RESULT_REJECTED;
                report.reject_reason = PLAN_ERROR_STEP_OVERFLOW;
                return report;
            }
            report.steps_inserted++;
            current = ph->pos;
        }

        wash_step_t s;
        memset(&s, 0, sizeof(s));
        s.type = ph->step_type;
        s.required_position = ph->step_type == STEP_MOVE_POSITION
            ? ph->pos
            : (step_is_position_gated(ph->step_type) ? ph->pos : current);
        s.duration_ms = ph->duration_ms;
        s.intensity = ph->intensity;
        s.flags = ph->flags;

        switch (ph->step_type) {
        case STEP_MOVE_POSITION:
            s.timeout_ms = move_timeout;
            current = ph->pos;
            break;
        case STEP_SETTLE: {
            uint32_t dur = ph->duration_ms > 0
                ? ph->duration_ms : DEFAULT_DEMO_SETTLE_FALLBACK_MS;
            s.duration_ms = dur;
            s.timeout_ms = dur + DEMO_TERMINAL_GRACE_MS;
            break;
        }
        case STEP_AUDIT_UV_OFF:
        case STEP_AUDIT_SAFE_OFF:
            s.timeout_ms = DEMO_INTERNAL_TIMEOUT_MS;
            break;
        case STEP_HOME_BL50:
            s.timeout_ms = DEMO_HOME_TIMEOUT_MS;
            break;
        case STEP_WATER_IN:
            s.timeout_ms = DEMO_WATER_TIMEOUT_MS;
            break;
        case STEP_DRY:
            s.timeout_ms = ph->duration_ms + DEMO_DRY_GRACE_MS;
            break;
        case STEP_DETERGENT:
        case STEP_PULSATOR_WASH:
        case STEP_DRUM_WASH:
        case STEP_DRAIN:
        case STEP_SPIN:
        case STEP_UV:
            s.timeout_ms = ph->duration_ms + DEMO_SERVICE_GRACE_MS;
            break;
        default:
            /* WAIT_LOAD / WAIT_UNLOAD / FINISH：等待用户确认，无超时 */
            s.timeout_ms = 0;
            break;
        }

        if (!append_step(out_program, &s)) {
            report.result = PLAN_RESULT_REJECTED;
            report.reject_reason = PLAN_ERROR_STEP_OVERFLOW;
            return report;
        }
        total_ms += ph->duration_ms;
    }

    out_program->estimated_total_ms = total_ms;
    report.result = PLAN_RESULT_OK;
    report.reject_reason = PLAN_ERROR_NONE;
    report.original_total_ms = total_ms;
    report.final_total_ms = total_ms;
    report.original_dry_ms = DEFAULT_DEMO_HOT_AIR_MS + DEFAULT_DEMO_COOL_TUMBLE_MS;
    report.final_dry_ms = report.original_dry_ms;
    return report;
}

bool default_demo_validate_position_trace(
    const wash_program_t *program,
    drum_position_t initial_position)
{
    if (!program) return false;
    drum_position_t current = initial_position;
    for (size_t i = 0; i < program->step_count; ++i) {
        const wash_step_t *s = &program->steps[i];
        if (s->type == STEP_MOVE_POSITION) {
            current = s->required_position;
        } else if (step_is_position_gated(s->type)) {
            if (s->required_position != DRUM_POS_UNKNOWN &&
                s->required_position != current) {
                return false;
            }
        }
    }
    return true;
}

/* ================================================================
 * 输出审计纯谓词（fail-closed）
 * ================================================================ */

static bool output_confirmed_off(int v)
{
    return v == 1;   /* 0=UNKNOWN, 1=OFF(已确认), 2=ON */
}

bool pc_audit_uv_off(const pc_output_audit_t *a)
{
    if (!a) return false;
    return output_confirmed_off(a->uv);
}

bool pc_audit_safe_off(const pc_output_audit_t *a)
{
    if (!a) return false;
    return output_confirmed_off(a->uv) &&
           output_confirmed_off(a->heater) &&
           output_confirmed_off(a->fan) &&
           output_confirmed_off(a->tap_valve) &&
           output_confirmed_off(a->transfer_valve) &&
           output_confirmed_off(a->detergent_pump) &&
           output_confirmed_off(a->drain_valve) &&
           !a->bl50_busy && !a->position_moving;
}

/* ================================================================
 * 多输入仲裁
 * ================================================================ */

void pc_arbiter_refresh(pc_arbiter_t *a)
{
    if (!a) return;
    if (a->emergency_latched) { a->state = PC_STATE_EMERGENCY; return; }
    if (a->fault_active)      { a->state = PC_STATE_FAULT;     return; }
    if (a->program_active)    { a->state = PC_STATE_RUNNING;   return; }
    /* 程序非激活：STARTING/STOPPING 闩锁塌缩回 IDLE */
    if (a->state == PC_STATE_STARTING || a->state == PC_STATE_STOPPING) {
        a->state = PC_STATE_IDLE;
    }
}

pc_action_t pc_arbitrate(pc_arbiter_t *a, pc_request_t req)
{
    if (!a) return PC_ACTION_NONE;
    switch (req) {
    case PC_REQ_EMERGENCY:
        a->emergency_latched = true;
        a->state = PC_STATE_EMERGENCY;
        return PC_ACTION_EMERGENCY_STOP;

    case PC_REQ_ABORT:
        if (a->emergency_latched || a->fault_active) return PC_ACTION_NONE;
        if (a->state == PC_STATE_STOPPING) return PC_ACTION_NONE;   /* dedupe */
        if (a->state == PC_STATE_STARTING || a->state == PC_STATE_RUNNING ||
            a->program_active) {
            a->state = PC_STATE_STOPPING;
            return PC_ACTION_ABORT_PROGRAM;
        }
        return PC_ACTION_NONE;   /* 无程序可中止 */

    case PC_REQ_STOP:
        if (a->emergency_latched || a->fault_active) return PC_ACTION_NONE;
        if (a->state == PC_STATE_STOPPING) return PC_ACTION_NONE;   /* dedupe */
        if (a->state == PC_STATE_STARTING || a->state == PC_STATE_RUNNING ||
            a->program_active) {
            a->state = PC_STATE_STOPPING;
            return PC_ACTION_STOP_PROGRAM;
        }
        return PC_ACTION_NONE;   /* 无程序可停 */

    case PC_REQ_START_DEMO:
        if (a->emergency_latched || a->fault_active) return PC_ACTION_NONE;
        if (a->external_busy) return PC_ACTION_NONE;               /* 诊断持有 executor */
        if (a->program_active) return PC_ACTION_NONE;              /* 已在运行 */
        if (a->state == PC_STATE_STARTING || a->state == PC_STATE_STOPPING ||
            a->state == PC_STATE_RUNNING) return PC_ACTION_NONE;   /* stop/start 竞态 */
        a->state = PC_STATE_STARTING;
        return PC_ACTION_START_DEMO;

    case PC_REQ_BACKEND_SWITCH:
        if (a->emergency_latched || a->fault_active) return PC_ACTION_NONE;
        return PC_ACTION_BACKEND_SWITCH;
    }
    return PC_ACTION_NONE;
}

/* ================================================================
 * BTN1 短按 / 长按（互斥）
 * ================================================================ */

pc_btn1_action_t pc_btn1_decide(pc_btn1_event_t ev, bool voice_active)
{
    switch (ev) {
    case PC_BTN1_EV_LONG_PRESS:
        return voice_active ? PC_BTN1_ACTION_BACKEND : PC_BTN1_ACTION_NONE;
    case PC_BTN1_EV_CLICK:
        return PC_BTN1_ACTION_START;
    default:
        return PC_BTN1_ACTION_NONE;
    }
}

/* ================================================================
 * 手势 UP→START / DOWN→STOP / 左右→翻页（冷却 + 重武装 + 尾串抑制）
 * ================================================================ */

pc_gesture_action_t pc_gesture_step(pc_gesture_state_t *s,
                                    hal_gesture_t gesture,
                                    uint32_t now_ms,
                                    uint32_t cooldown_ms)
{
    if (!s) return PC_GESTURE_ACTION_NONE;

    if (gesture == HAL_GESTURE_NONE) {
        /* 传感器空闲：冷却过后重武装 */
        if (!s->armed && now_ms >= s->cooldown_until_ms) s->armed = true;
        return PC_GESTURE_ACTION_NONE;
    }

    /* 可动作手势：未武装（尾串/冷却中）→ 抑制 */
    if (!s->armed) return PC_GESTURE_ACTION_NONE;
    if (now_ms < s->cooldown_until_ms) return PC_GESTURE_ACTION_NONE;

    /* 触发并进入冷却 */
    s->armed = false;
    s->cooldown_until_ms = now_ms + cooldown_ms;

    switch (gesture) {
    case HAL_GESTURE_UP:        return PC_GESTURE_ACTION_START;
    case HAL_GESTURE_DOWN:      return PC_GESTURE_ACTION_STOP;
    case HAL_GESTURE_RIGHT:
    case HAL_GESTURE_FORWARD:   return PC_GESTURE_ACTION_PAGE_NEXT;
    case HAL_GESTURE_LEFT:
    case HAL_GESTURE_BACKWARD:  return PC_GESTURE_ACTION_PAGE_PREV;
    default:                    return PC_GESTURE_ACTION_NONE;  /* CLOCKWISE/COUNTER_CW */
    }
}
