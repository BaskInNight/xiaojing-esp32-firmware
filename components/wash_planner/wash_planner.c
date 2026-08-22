/*
 * wash_planner.c — Pure C wash program compiler
 *
 * Contract:
 *   - Pure function: no static mutable state
 *   - Reentrant / thread-safe
 *   - Deterministic: same inputs → byte-identical output
 *   - Fixed capacity: WASH_PROGRAM_MAX_STEPS = 64
 *   - No FreeRTOS, HAL, NVS, GPIO, or dynamic memory
 */

#include "wash_planner.h"
#include <string.h>

/* ================================================================
 * Named constants — no magic numbers
 * ================================================================ */

/* Formal mode defaults */
#define FORMAL_WASH_ROUNDS       3U
#define FORMAL_WASH_DURATION_MS  30000U   /* per round */
#define FORMAL_WASH_INTENSITY    WASH_INTENSITY_NORMAL
#define FORMAL_SPIN_DURATION_MS  120000U
#define FORMAL_SPIN_INTENSITY    WASH_INTENSITY_STRONG

/* Demo mode short durations */
#define DEMO_WASH_ROUNDS         1U
#define DEMO_WASH_DURATION_MS    10000U
#define DEMO_WASH_INTENSITY      WASH_INTENSITY_LOW
#define DEMO_SPIN_DURATION_MS    15000U
#define DEMO_SPIN_INTENSITY      WASH_INTENSITY_NORMAL
#define DEMO_DRY_DURATION_MS     10000U
#define DEMO_DETERGENT_DURATION_MS 3000U

/* Duration limits */
#define PLANNER_TOTAL_DURATION_MAX_MS   3600000U  /* 1 hour */
#define PLANNER_DRY_CUMULATIVE_MAX_MS   1800000U  /* 30 min */
#define PLANNER_DEMO_AUTO_BUDGET_MS     180000U   /* 3 min */

/* Timeout grace periods — named, no magic */
#define PLANNER_TERMINAL_GRACE_MS       5000U     /* terminal publish grace */
#define PLANNER_DRAIN_CLOSE_GRACE_MS    10000U    /* valve close + settle */
#define PLANNER_UV_OFF_GRACE_MS         5000U     /* UV lamp off confirm */
#define PLANNER_DRY_COOLDOWN_GRACE_MS   10000U    /* beyond cooldown_ms */
#define PLANNER_BL50_RAMP_GRACE_MS      5000U     /* ramp + brake + reversal */

/* HOME timeout */
#define HOME_BL50_TIMEOUT_MS            10000U

/* ================================================================
 * Water state tracking
 * ================================================================ */

typedef enum {
    WATER_PLAN_EMPTY = 0,
    WATER_PLAN_PRESENT
} planner_water_state_t;

/* ================================================================
 * Overflow-safe three-term add: a + b + c → out
 * Returns true on overflow.
 * ================================================================ */

static bool u32_add3_overflow(uint32_t a, uint32_t b, uint32_t c, uint32_t *out)
{
    uint32_t ab;
    if (a > UINT32_MAX - b) return true;
    ab = a + b;
    if (ab > UINT32_MAX - c) return true;
    *out = ab + c;
    return false;
}

/* ================================================================
 * Internal helpers — all static, no mutable state
 * ================================================================ */

static planner_report_t reject(
    planner_error_t reason,
    uint16_t action_index,
    wash_program_t *out)
{
    memset(out, 0, sizeof(*out));
    planner_report_t r;
    memset(&r, 0, sizeof(r));
    r.result = PLAN_RESULT_REJECTED;
    r.reject_reason = reason;
    r.reject_action_index = action_index;
    return r;
}

static bool u32_add_overflow(uint32_t a, uint32_t b, uint32_t *out)
{
    if (a > UINT32_MAX - b) return true;
    *out = a + b;
    return false;
}

static bool u32_mul_overflow(uint32_t a, uint32_t b, uint32_t *out)
{
    if (a == 0 || b == 0) { *out = 0; return false; }
    if (a > UINT32_MAX / b) return true;
    *out = a * b;
    return false;
}

static drum_position_t action_required_position(wash_action_type_t type, drum_position_t action_pos)
{
    switch (type) {
    case WASH_ACTION_MOVE_POSITION: return action_pos;
    case WASH_ACTION_WATER_IN:      return DRUM_POS_0;
    case WASH_ACTION_DETERGENT:     return DRUM_POS_0;
    case WASH_ACTION_PULSATOR_WASH: return DRUM_POS_0;
    case WASH_ACTION_DRUM_WASH:     return DRUM_POS_90;
    case WASH_ACTION_DRAIN:         return DRUM_POS_180;
    case WASH_ACTION_SPIN:          return DRUM_POS_180;
    case WASH_ACTION_DRY:           return DRUM_POS_270;
    case WASH_ACTION_UV:            return DRUM_POS_0;
    default:                        return DRUM_POS_UNKNOWN;
    }
}

static wash_step_type_t action_to_step_type(wash_action_type_t type)
{
    switch (type) {
    case WASH_ACTION_MOVE_POSITION: return STEP_MOVE_POSITION;
    case WASH_ACTION_WATER_IN:      return STEP_WATER_IN;
    case WASH_ACTION_DETERGENT:     return STEP_DETERGENT;
    case WASH_ACTION_PULSATOR_WASH: return STEP_PULSATOR_WASH;
    case WASH_ACTION_DRUM_WASH:     return STEP_DRUM_WASH;
    case WASH_ACTION_DRAIN:         return STEP_DRAIN;
    case WASH_ACTION_SPIN:          return STEP_SPIN;
    case WASH_ACTION_DRY:           return STEP_DRY;
    case WASH_ACTION_UV:            return STEP_UV;
    default:                        return STEP_FINISH;
    }
}

static bool is_water_dependent(wash_action_type_t type)
{
    return type == WASH_ACTION_DETERGENT
        || type == WASH_ACTION_PULSATOR_WASH
        || type == WASH_ACTION_DRUM_WASH;
}

static bool is_valid_intensity(wash_intensity_t intensity)
{
    return intensity >= WASH_INTENSITY_GENTLE
        && intensity <= WASH_INTENSITY_STRONG;
}

/* Only these action types support rounds expansion */
static bool is_rounds_eligible(wash_action_type_t type)
{
    return type == WASH_ACTION_PULSATOR_WASH
        || type == WASH_ACTION_DRUM_WASH
        || type == WASH_ACTION_SPIN;
}

/* ================================================================
 * Timeout computation — centralized, no magic numbers
 * ================================================================ */

/* Compute safe timeout for a service step. Returns true on overflow. */
static bool compute_service_timeout(
    uint32_t duration_ms,
    uint32_t grace_ms,
    uint32_t move_timeout,
    uint32_t *out_timeout)
{
    /* timeout = max(move_timeout, duration + grace) */
    uint32_t with_grace;
    if (u32_add_overflow(duration_ms, grace_ms, &with_grace))
        return true;
    *out_timeout = with_grace > move_timeout ? with_grace : move_timeout;
    return false;
}

/* ================================================================
 * Step builder context
 * ================================================================ */

typedef struct {
    wash_program_t *prog;
    planner_report_t *report;
    uint32_t total_duration_ms;
    uint32_t dry_duration_ms;
    drum_position_t current_pos;
    bool last_move_was_explicit;  /* for steps_merged tracking */
    bool ok;
} step_ctx_t;

static step_ctx_t make_step_ctx(wash_program_t *prog, planner_report_t *report)
{
    step_ctx_t ctx;
    ctx.prog = prog;
    ctx.report = report;
    ctx.total_duration_ms = 0;
    ctx.dry_duration_ms = 0;
    ctx.current_pos = DRUM_POS_UNKNOWN;
    ctx.last_move_was_explicit = false;
    ctx.ok = true;
    return ctx;
}

/* Append a step. steps_inserted only counts SYSTEM_INSERTED steps. */
static bool append_step(step_ctx_t *ctx, const wash_step_t *step)
{
    if (ctx->prog->step_count >= WASH_PROGRAM_MAX_STEPS) {
        ctx->ok = false;
        return false;
    }
    uint16_t id = (uint16_t)ctx->prog->step_count;
    ctx->prog->steps[id] = *step;
    ctx->prog->steps[id].step_id = id;
    ctx->prog->step_count = (size_t)(id + 1);
    ctx->current_pos = step->required_position;
    if (step->flags & WASH_STEP_FLAG_SYSTEM_INSERTED) {
        ctx->report->steps_inserted++;
    }
    if (step->type != STEP_MOVE_POSITION) {
        ctx->last_move_was_explicit = false;
    }
    return true;
}

/* Insert system MOVE only if target differs from current position.
 * Always system-inserted. Does NOT count as steps_merged. */
static bool ensure_move(step_ctx_t *ctx, drum_position_t target,
                         uint32_t move_timeout)
{
    if (ctx->current_pos == target) {
        ctx->last_move_was_explicit = false;
        return true;  /* already there — no-op */
    }
    wash_step_t s;
    memset(&s, 0, sizeof(s));
    s.type = STEP_MOVE_POSITION;
    s.required_position = target;
    s.duration_ms = 0;
    s.timeout_ms = move_timeout;
    s.flags = WASH_STEP_FLAG_SYSTEM_INSERTED;
    ctx->last_move_was_explicit = false;
    return append_step(ctx, &s);
}

/* Insert an explicit MOVE_POSITION action from user intent.
 * If the previous output was also an explicit MOVE to the same target,
 * this is a true merge — steps_merged++. */
static bool ensure_move_explicit(step_ctx_t *ctx, drum_position_t target,
                                  uint32_t move_timeout)
{
    if (ctx->current_pos == target && ctx->last_move_was_explicit) {
        ctx->report->steps_merged++;
        return true;  /* explicit merge */
    }
    wash_step_t s;
    memset(&s, 0, sizeof(s));
    s.type = STEP_MOVE_POSITION;
    s.required_position = target;
    s.duration_ms = 0;
    s.timeout_ms = move_timeout;
    s.flags = 0;  /* not system-inserted */
    ctx->last_move_was_explicit = true;
    return append_step(ctx, &s);
}

static bool add_duration(step_ctx_t *ctx, uint32_t duration_ms, bool is_dry)
{
    if (u32_add_overflow(ctx->total_duration_ms, duration_ms, &ctx->total_duration_ms))
        return true;
    if (is_dry) {
        if (u32_add_overflow(ctx->dry_duration_ms, duration_ms, &ctx->dry_duration_ms))
            return true;
    }
    return false;
}

/* Append auto-inserted DRAIN step (system-inserted). */
static bool auto_drain(step_ctx_t *ctx, const machine_config_t *config,
                        uint32_t move_timeout, planner_water_state_t *water)
{
    if (!ensure_move(ctx, DRUM_POS_180, move_timeout)) return false;
    uint32_t drain_dur = config->drain.max_duration_ms > 0
                          ? config->drain.max_duration_ms : 120000U;
    uint32_t drain_timeout;
    if (compute_service_timeout(drain_dur, PLANNER_DRAIN_CLOSE_GRACE_MS,
                                move_timeout, &drain_timeout))
        return false;
    wash_step_t s;
    memset(&s, 0, sizeof(s));
    s.type = STEP_DRAIN;
    s.required_position = DRUM_POS_180;
    s.duration_ms = drain_dur;
    s.timeout_ms = drain_timeout;
    s.flags = WASH_STEP_FLAG_SYSTEM_INSERTED;
    if (!append_step(ctx, &s)) return false;
    *water = WATER_PLAN_EMPTY;
    return true;
}

/* ================================================================
 * Golden sequence builders
 * ================================================================ */

static planner_report_t build_formal(
    const machine_config_t *config,
    uint32_t program_id,
    bool allow_uv,
    bool allow_dry,
    wash_program_t *prog)
{
    planner_report_t report;
    memset(&report, 0, sizeof(report));
    step_ctx_t ctx = make_step_ctx(prog, &report);

    prog->program_id = program_id;
    prog->kind = WASH_PROGRAM_FORMAL;

    uint32_t wash_dur = FORMAL_WASH_ROUNDS * FORMAL_WASH_DURATION_MS;
    uint32_t spin_dur = FORMAL_SPIN_DURATION_MS;
    uint32_t det_dur = config->detergent.formal_duration_ms > 0
                        ? config->detergent.formal_duration_ms : 6000U;
    uint32_t dry_dur = config->dry.max_total_ms > 0
                        ? config->dry.max_total_ms : 1800000U;
    uint32_t drain_dur = config->drain.max_duration_ms > 0
                          ? config->drain.max_duration_ms : 120000U;
    uint32_t uv_dur = config->uv.default_duration_ms > 0
                       ? config->uv.default_duration_ms : 600000U;
    uint32_t move_timeout = config->position.move_timeout_ms > 0
                             ? config->position.move_timeout_ms : 30000U;

    /* Step 0: WAIT_LOAD_CONFIRM */
    { wash_step_t s; memset(&s, 0, sizeof(s));
      s.type = STEP_WAIT_LOAD_CONFIRM;
      s.required_position = DRUM_POS_UNKNOWN;
      s.duration_ms = 0; s.timeout_ms = 0;
      s.flags = WASH_STEP_FLAG_REQUIRED | WASH_STEP_FLAG_SYSTEM_INSERTED;
      if (!append_step(&ctx, &s)) goto fail; }

    /* Step 1: HOME_BL50 @45 */
    { wash_step_t s; memset(&s, 0, sizeof(s));
      s.type = STEP_HOME_BL50;
      s.required_position = DRUM_POS_45;
      s.duration_ms = 0;
      s.timeout_ms = HOME_BL50_TIMEOUT_MS + PLANNER_TERMINAL_GRACE_MS;
      s.flags = WASH_STEP_FLAG_REQUIRED | WASH_STEP_FLAG_SYSTEM_INSERTED;
      if (!append_step(&ctx, &s)) goto fail; }

    /* Step 2: MOVE @0 */
    if (!ensure_move(&ctx, DRUM_POS_0, move_timeout)) goto fail;

    /* Step 3: WATER_IN @0 */
    { uint32_t water_timeout;
      if (compute_service_timeout(0, config->water.total_inlet_timeout_ms > 0
                                    ? config->water.total_inlet_timeout_ms : 600000U,
                                  move_timeout, &water_timeout))
          goto overflow;
      wash_step_t s; memset(&s, 0, sizeof(s));
      s.type = STEP_WATER_IN;
      s.required_position = DRUM_POS_0;
      s.duration_ms = 0; s.timeout_ms = water_timeout;
      s.flags = WASH_STEP_FLAG_SYSTEM_INSERTED;
      if (!append_step(&ctx, &s)) goto fail; }

    /* Step 4: DETERGENT @0 */
    { uint32_t det_timeout;
      if (compute_service_timeout(det_dur, PLANNER_TERMINAL_GRACE_MS,
                                  move_timeout, &det_timeout))
          goto overflow;
      wash_step_t s; memset(&s, 0, sizeof(s));
      s.type = STEP_DETERGENT;
      s.required_position = DRUM_POS_0;
      s.duration_ms = det_dur; s.timeout_ms = det_timeout;
      s.intensity = FORMAL_WASH_INTENSITY;
      s.flags = WASH_STEP_FLAG_SYSTEM_INSERTED;
      if (!append_step(&ctx, &s)) goto fail;
      if (add_duration(&ctx, det_dur, false)) goto overflow; }

    /* Step 5: PULSATOR_WASH @0 */
    { uint32_t wash_timeout;
      if (compute_service_timeout(wash_dur, PLANNER_BL50_RAMP_GRACE_MS,
                                  move_timeout, &wash_timeout))
          goto overflow;
      wash_step_t s; memset(&s, 0, sizeof(s));
      s.type = STEP_PULSATOR_WASH;
      s.required_position = DRUM_POS_0;
      s.duration_ms = wash_dur; s.timeout_ms = wash_timeout;
      s.intensity = FORMAL_WASH_INTENSITY;
      s.flags = WASH_STEP_FLAG_SYSTEM_INSERTED;
      if (!append_step(&ctx, &s)) goto fail;
      if (add_duration(&ctx, wash_dur, false)) goto overflow; }

    /* Step 6: MOVE @180 */
    if (!ensure_move(&ctx, DRUM_POS_180, move_timeout)) goto fail;

    /* Step 7: DRAIN @180 */
    { uint32_t drain_timeout;
      if (compute_service_timeout(drain_dur, PLANNER_DRAIN_CLOSE_GRACE_MS,
                                  move_timeout, &drain_timeout))
          goto overflow;
      wash_step_t s; memset(&s, 0, sizeof(s));
      s.type = STEP_DRAIN;
      s.required_position = DRUM_POS_180;
      s.duration_ms = drain_dur; s.timeout_ms = drain_timeout;
      s.flags = WASH_STEP_FLAG_SYSTEM_INSERTED;
      if (!append_step(&ctx, &s)) goto fail;
      if (add_duration(&ctx, drain_dur, false)) goto overflow; }

    /* Step 8: SPIN @180 */
    { uint32_t spin_timeout;
      if (compute_service_timeout(spin_dur, PLANNER_BL50_RAMP_GRACE_MS,
                                  move_timeout, &spin_timeout))
          goto overflow;
      wash_step_t s; memset(&s, 0, sizeof(s));
      s.type = STEP_SPIN;
      s.required_position = DRUM_POS_180;
      s.duration_ms = spin_dur; s.timeout_ms = spin_timeout;
      s.intensity = FORMAL_SPIN_INTENSITY;
      s.flags = WASH_STEP_FLAG_SYSTEM_INSERTED;
      if (!append_step(&ctx, &s)) goto fail;
      if (add_duration(&ctx, spin_dur, false)) goto overflow; }

    /* DRY block (allow_dry=true only) */
    if (allow_dry) {
        /* Step 9: MOVE @270 */
        if (!ensure_move(&ctx, DRUM_POS_270, move_timeout)) goto fail;

        /* Step 10: DRY @270 — overflow-safe three-term timeout */
        { uint32_t cd = config->dry.cooldown_ms > 0
                        ? config->dry.cooldown_ms : 60000U;
          uint32_t dry_timeout_dur;
          if (u32_add3_overflow(dry_dur, cd, PLANNER_DRY_COOLDOWN_GRACE_MS,
                                &dry_timeout_dur))
              goto overflow;
          uint32_t dry_timeout = dry_timeout_dur > move_timeout
                                 ? dry_timeout_dur : move_timeout;
          wash_step_t s; memset(&s, 0, sizeof(s));
          s.type = STEP_DRY;
          s.required_position = DRUM_POS_270;
          s.duration_ms = dry_dur; s.timeout_ms = dry_timeout;
          s.flags = WASH_STEP_FLAG_SYSTEM_INSERTED;
          if (!append_step(&ctx, &s)) goto fail;
          if (add_duration(&ctx, dry_dur, true)) goto overflow; }

        /* Step 11: MOVE @45 */
        if (!ensure_move(&ctx, DRUM_POS_45, move_timeout)) goto fail;
    } else {
        report.fixup_flags |= PLAN_FIXUP_SKIP_DRY;
        if (!ensure_move(&ctx, DRUM_POS_45, move_timeout)) goto fail;
    }

    /* WAIT_UNLOAD_CONFIRM */
    { wash_step_t s; memset(&s, 0, sizeof(s));
      s.type = STEP_WAIT_UNLOAD_CONFIRM;
      s.required_position = DRUM_POS_45;
      s.duration_ms = 0; s.timeout_ms = 0;
      s.flags = WASH_STEP_FLAG_REQUIRED | WASH_STEP_FLAG_SYSTEM_INSERTED;
      if (!append_step(&ctx, &s)) goto fail; }

    /* UV block (only if allowed) */
    if (allow_uv) {
        if (!ensure_move(&ctx, DRUM_POS_0, move_timeout)) goto fail;
        { uint32_t uv_timeout;
          if (compute_service_timeout(uv_dur, PLANNER_UV_OFF_GRACE_MS,
                                      move_timeout, &uv_timeout))
              goto overflow;
          wash_step_t s; memset(&s, 0, sizeof(s));
          s.type = STEP_UV;
          s.required_position = DRUM_POS_0;
          s.duration_ms = uv_dur; s.timeout_ms = uv_timeout;
          s.flags = WASH_STEP_FLAG_SYSTEM_INSERTED;
          if (!append_step(&ctx, &s)) goto fail;
          if (add_duration(&ctx, uv_dur, false)) goto overflow; }
        if (!ensure_move(&ctx, DRUM_POS_45, move_timeout)) goto fail;
    } else {
        report.fixup_flags |= PLAN_FIXUP_SKIP_UV;
    }

    /* FINISH @45 */
    { wash_step_t s; memset(&s, 0, sizeof(s));
      s.type = STEP_FINISH;
      s.required_position = DRUM_POS_45;
      s.duration_ms = 0; s.timeout_ms = 0;
      s.flags = WASH_STEP_FLAG_SYSTEM_INSERTED;
      if (!append_step(&ctx, &s)) goto fail; }

    /* Total limits */
    if (ctx.total_duration_ms > PLANNER_TOTAL_DURATION_MAX_MS) {
        report.result = PLAN_RESULT_REJECTED;
        report.reject_reason = PLAN_ERROR_TOTAL_LIMIT;
        memset(prog, 0, sizeof(*prog));
        return report;
    }
    if (ctx.dry_duration_ms > PLANNER_DRY_CUMULATIVE_MAX_MS) {
        report.result = PLAN_RESULT_REJECTED;
        report.reject_reason = PLAN_ERROR_TOTAL_LIMIT;
        memset(prog, 0, sizeof(*prog));
        return report;
    }

    prog->estimated_total_ms = ctx.total_duration_ms;
    report.result = PLAN_RESULT_OK;
    report.original_total_ms = ctx.total_duration_ms;
    report.final_total_ms = ctx.total_duration_ms;
    report.original_dry_ms = ctx.dry_duration_ms;
    report.final_dry_ms = ctx.dry_duration_ms;
    report.reject_action_index = UINT16_MAX;
    return report;

fail:
    report.result = PLAN_RESULT_REJECTED;
    report.reject_reason = PLAN_ERROR_STEP_OVERFLOW;
    memset(prog, 0, sizeof(*prog));
    return report;

overflow:
    report.result = PLAN_RESULT_REJECTED;
    report.reject_reason = PLAN_ERROR_DURATION_OVERFLOW;
    memset(prog, 0, sizeof(*prog));
    return report;
}

static planner_report_t build_demo(
    const machine_config_t *config,
    uint32_t program_id,
    bool allow_dry,
    wash_program_t *prog)
{
    planner_report_t report;
    memset(&report, 0, sizeof(report));
    step_ctx_t ctx = make_step_ctx(prog, &report);

    prog->program_id = program_id;
    prog->kind = WASH_PROGRAM_DEMO;

    uint32_t wash_dur = DEMO_WASH_ROUNDS * DEMO_WASH_DURATION_MS;
    uint32_t spin_dur = DEMO_SPIN_DURATION_MS;
    uint32_t det_dur = DEMO_DETERGENT_DURATION_MS;
    uint32_t dry_dur = DEMO_DRY_DURATION_MS;
    uint32_t drain_dur = config->drain.max_duration_ms > 0
                          ? config->drain.max_duration_ms : 120000U;
    uint32_t move_timeout = config->position.move_timeout_ms > 0
                             ? config->position.move_timeout_ms : 30000U;

    /* HOME_BL50 @45 */
    { wash_step_t s; memset(&s, 0, sizeof(s));
      s.type = STEP_HOME_BL50;
      s.required_position = DRUM_POS_45;
      s.duration_ms = 0;
      s.timeout_ms = HOME_BL50_TIMEOUT_MS + PLANNER_TERMINAL_GRACE_MS;
      s.flags = WASH_STEP_FLAG_REQUIRED | WASH_STEP_FLAG_SYSTEM_INSERTED;
      if (!append_step(&ctx, &s)) goto fail; }

    /* MOVE @0 */
    if (!ensure_move(&ctx, DRUM_POS_0, move_timeout)) goto fail;

    /* WATER_IN @0 */
    { uint32_t water_timeout;
      if (compute_service_timeout(0, config->water.total_inlet_timeout_ms > 0
                                    ? config->water.total_inlet_timeout_ms : 600000U,
                                  move_timeout, &water_timeout))
          goto overflow;
      wash_step_t s; memset(&s, 0, sizeof(s));
      s.type = STEP_WATER_IN;
      s.required_position = DRUM_POS_0;
      s.duration_ms = 0; s.timeout_ms = water_timeout;
      s.flags = WASH_STEP_FLAG_SYSTEM_INSERTED;
      if (!append_step(&ctx, &s)) goto fail; }

    /* DETERGENT @0 */
    { uint32_t det_timeout;
      if (compute_service_timeout(det_dur, PLANNER_TERMINAL_GRACE_MS,
                                  move_timeout, &det_timeout))
          goto overflow;
      wash_step_t s; memset(&s, 0, sizeof(s));
      s.type = STEP_DETERGENT;
      s.required_position = DRUM_POS_0;
      s.duration_ms = det_dur; s.timeout_ms = det_timeout;
      s.intensity = DEMO_WASH_INTENSITY;
      s.flags = WASH_STEP_FLAG_SYSTEM_INSERTED;
      if (!append_step(&ctx, &s)) goto fail;
      if (add_duration(&ctx, det_dur, false)) goto overflow; }

    /* PULSATOR_WASH @0 */
    { uint32_t wash_timeout;
      if (compute_service_timeout(wash_dur, PLANNER_BL50_RAMP_GRACE_MS,
                                  move_timeout, &wash_timeout))
          goto overflow;
      wash_step_t s; memset(&s, 0, sizeof(s));
      s.type = STEP_PULSATOR_WASH;
      s.required_position = DRUM_POS_0;
      s.duration_ms = wash_dur; s.timeout_ms = wash_timeout;
      s.intensity = DEMO_WASH_INTENSITY;
      s.flags = WASH_STEP_FLAG_SYSTEM_INSERTED;
      if (!append_step(&ctx, &s)) goto fail;
      if (add_duration(&ctx, wash_dur, false)) goto overflow; }

    /* MOVE @180 */
    if (!ensure_move(&ctx, DRUM_POS_180, move_timeout)) goto fail;

    /* DRAIN @180 */
    { uint32_t drain_timeout;
      if (compute_service_timeout(drain_dur, PLANNER_DRAIN_CLOSE_GRACE_MS,
                                  move_timeout, &drain_timeout))
          goto overflow;
      wash_step_t s; memset(&s, 0, sizeof(s));
      s.type = STEP_DRAIN;
      s.required_position = DRUM_POS_180;
      s.duration_ms = drain_dur; s.timeout_ms = drain_timeout;
      s.flags = WASH_STEP_FLAG_SYSTEM_INSERTED;
      if (!append_step(&ctx, &s)) goto fail;
      if (add_duration(&ctx, drain_dur, false)) goto overflow; }

    /* SPIN @180 */
    { uint32_t spin_timeout;
      if (compute_service_timeout(spin_dur, PLANNER_BL50_RAMP_GRACE_MS,
                                  move_timeout, &spin_timeout))
          goto overflow;
      wash_step_t s; memset(&s, 0, sizeof(s));
      s.type = STEP_SPIN;
      s.required_position = DRUM_POS_180;
      s.duration_ms = spin_dur; s.timeout_ms = spin_timeout;
      s.intensity = DEMO_SPIN_INTENSITY;
      s.flags = WASH_STEP_FLAG_SYSTEM_INSERTED;
      if (!append_step(&ctx, &s)) goto fail;
      if (add_duration(&ctx, spin_dur, false)) goto overflow; }

    if (allow_dry) {
        /* MOVE @270 */
        if (!ensure_move(&ctx, DRUM_POS_270, move_timeout)) goto fail;

        /* DRY @270 — overflow-safe three-term timeout */
        { uint32_t cd = config->dry.cooldown_ms > 0
                        ? config->dry.cooldown_ms : 60000U;
          uint32_t dry_timeout_dur;
          if (u32_add3_overflow(dry_dur, cd, PLANNER_DRY_COOLDOWN_GRACE_MS,
                                &dry_timeout_dur))
              goto overflow;
          uint32_t dry_timeout = dry_timeout_dur > move_timeout
                                 ? dry_timeout_dur : move_timeout;
          wash_step_t s; memset(&s, 0, sizeof(s));
          s.type = STEP_DRY;
          s.required_position = DRUM_POS_270;
          s.duration_ms = dry_dur; s.timeout_ms = dry_timeout;
          s.flags = WASH_STEP_FLAG_SYSTEM_INSERTED;
          if (!append_step(&ctx, &s)) goto fail;
          if (add_duration(&ctx, dry_dur, true)) goto overflow; }

        /* MOVE @45 */
        if (!ensure_move(&ctx, DRUM_POS_45, move_timeout)) goto fail;
    } else {
        report.fixup_flags |= PLAN_FIXUP_SKIP_DRY;
        if (!ensure_move(&ctx, DRUM_POS_45, move_timeout)) goto fail;
    }

    /* FINISH @45 */
    { wash_step_t s; memset(&s, 0, sizeof(s));
      s.type = STEP_FINISH;
      s.required_position = DRUM_POS_45;
      s.duration_ms = 0; s.timeout_ms = 0;
      s.flags = WASH_STEP_FLAG_SYSTEM_INSERTED;
      if (!append_step(&ctx, &s)) goto fail; }

    /* Check demo budget */
    if (ctx.total_duration_ms > PLANNER_DEMO_AUTO_BUDGET_MS) {
        report.result = PLAN_RESULT_REJECTED;
        report.reject_reason = PLAN_ERROR_TOTAL_LIMIT;
        memset(prog, 0, sizeof(*prog));
        return report;
    }

    prog->estimated_total_ms = ctx.total_duration_ms;
    report.result = PLAN_RESULT_OK;
    report.original_total_ms = ctx.total_duration_ms;
    report.final_total_ms = ctx.total_duration_ms;
    report.original_dry_ms = ctx.dry_duration_ms;
    report.final_dry_ms = ctx.dry_duration_ms;
    report.reject_action_index = UINT16_MAX;
    return report;

fail:
    report.result = PLAN_RESULT_REJECTED;
    report.reject_reason = PLAN_ERROR_STEP_OVERFLOW;
    memset(prog, 0, sizeof(*prog));
    return report;

overflow:
    report.result = PLAN_RESULT_REJECTED;
    report.reject_reason = PLAN_ERROR_DURATION_OVERFLOW;
    memset(prog, 0, sizeof(*prog));
    return report;
}

/* ================================================================
 * Custom sequence builder
 * ================================================================ */

static planner_report_t build_custom(
    const wash_intent_t *intent,
    const machine_config_t *config,
    uint32_t program_id,
    wash_program_t *prog)
{
    planner_report_t report;
    memset(&report, 0, sizeof(report));
    step_ctx_t ctx = make_step_ctx(prog, &report);

    prog->program_id = program_id;
    prog->kind = WASH_PROGRAM_CUSTOM;

    planner_water_state_t water_state = WATER_PLAN_EMPTY;
    uint32_t move_timeout = config->position.move_timeout_ms > 0
                             ? config->position.move_timeout_ms : 30000U;

    for (size_t i = 0; i < intent->action_count; i++) {
        const wash_action_t *action = &intent->actions[i];

        /* Validate action type enum */
        if (action->type < WASH_ACTION_MOVE_POSITION || action->type > WASH_ACTION_UV) {
            return reject(PLAN_ERROR_INVALID_ACTION, (uint16_t)i, prog);
        }

        /* Skip disabled actions */
        if (!action->enabled) {
            report.steps_dropped++;
            continue;
        }

        wash_action_type_t atype = action->type;

        /* Validate MOVE position */
        if (atype == WASH_ACTION_MOVE_POSITION) {
            if (!drum_position_is_valid(action->position)) {
                return reject(PLAN_ERROR_INVALID_POSITION, (uint16_t)i, prog);
            }
        }

        /* Validate intensity for wash/spin/detergent actions */
        if ((atype == WASH_ACTION_PULSATOR_WASH || atype == WASH_ACTION_DRUM_WASH
             || atype == WASH_ACTION_SPIN || atype == WASH_ACTION_DETERGENT)
            && !is_valid_intensity(action->intensity)) {
            return reject(PLAN_ERROR_INVALID_ACTION, (uint16_t)i, prog);
        }

        /* Skip UV if not allowed */
        if (atype == WASH_ACTION_UV && !intent->allow_uv) {
            report.steps_dropped++;
            report.fixup_flags |= PLAN_FIXUP_SKIP_UV;
            continue;
        }

        /* Skip DRY if not allowed */
        if (atype == WASH_ACTION_DRY && !intent->allow_dry) {
            report.steps_dropped++;
            report.fixup_flags |= PLAN_FIXUP_SKIP_DRY;
            continue;
        }

        /* Track water state */
        if (atype == WASH_ACTION_WATER_IN) {
            water_state = WATER_PLAN_PRESENT;
        }

        /* Auto-insert WATER_IN before water-dependent steps */
        if (is_water_dependent(atype) && water_state == WATER_PLAN_EMPTY) {
            if (!ensure_move(&ctx, DRUM_POS_0, move_timeout)) goto fail;
            wash_step_t s; memset(&s, 0, sizeof(s));
            s.type = STEP_WATER_IN;
            s.required_position = DRUM_POS_0;
            s.duration_ms = 0;
            uint32_t wt;
            if (compute_service_timeout(0, config->water.total_inlet_timeout_ms > 0
                                          ? config->water.total_inlet_timeout_ms : 600000U,
                                        move_timeout, &wt))
                goto overflow;
            s.timeout_ms = wt;
            s.flags = WASH_STEP_FLAG_SYSTEM_INSERTED;
            if (!append_step(&ctx, &s)) goto fail;
            water_state = WATER_PLAN_PRESENT;
        }

        /* Auto-insert DRAIN before DRY/SPIN/UV if water present */
        if ((atype == WASH_ACTION_DRY || atype == WASH_ACTION_SPIN
             || atype == WASH_ACTION_UV)
            && water_state == WATER_PLAN_PRESENT) {
            if (!auto_drain(&ctx, config, move_timeout, &water_state)) goto fail;
            if (add_duration(&ctx, config->drain.max_duration_ms > 0
                                    ? config->drain.max_duration_ms : 120000U,
                             false))
                goto overflow;
        }

        /* DRAIN action clears water state */
        if (atype == WASH_ACTION_DRAIN) {
            water_state = WATER_PLAN_EMPTY;
        }

        /* MOVE_POSITION: use ensure_move_explicit for merge tracking */
        if (atype == WASH_ACTION_MOVE_POSITION) {
            if (!ensure_move_explicit(&ctx, action->position, move_timeout))
                goto fail;
            continue;
        }

        /* Calculate duration */
        uint32_t dur = action->duration_ms;
        uint16_t rounds = action->rounds;

        /* DRAIN duration from config */
        if (atype == WASH_ACTION_DRAIN) {
            dur = config->drain.max_duration_ms > 0
                  ? config->drain.max_duration_ms : 120000U;
        }

        /* Rounds expansion only for eligible actions */
        if (is_rounds_eligible(atype)) {
            if (rounds == 0) {
                return reject(PLAN_ERROR_INVALID_DURATION, (uint16_t)i, prog);
            }
            if (rounds > 1) {
                uint32_t expanded;
                if (u32_mul_overflow(rounds, dur, &expanded)) {
                    return reject(PLAN_ERROR_DURATION_OVERFLOW, (uint16_t)i, prog);
                }
                dur = expanded;
            }
        }
        /* Non-rounds-eligible actions: rounds field ignored */

        /* Track duration */
        if (dur > 0) {
            bool is_dry = (atype == WASH_ACTION_DRY);
            if (add_duration(&ctx, dur, is_dry)) goto overflow;
        }

        /* Insert MOVE if needed */
        drum_position_t req_pos = action_required_position(atype, action->position);
        if (req_pos != DRUM_POS_UNKNOWN) {
            if (!ensure_move(&ctx, req_pos, move_timeout)) goto fail;
        }

        /* Compute timeout for this step */
        uint32_t step_timeout = move_timeout;
        {
            uint32_t grace = PLANNER_TERMINAL_GRACE_MS;
            if (atype == WASH_ACTION_DRAIN) grace = PLANNER_DRAIN_CLOSE_GRACE_MS;
            else if (atype == WASH_ACTION_DRY) {
                uint32_t cd = config->dry.cooldown_ms > 0
                              ? config->dry.cooldown_ms : 60000U;
                uint32_t dry_grace;
                if (u32_add_overflow(cd, PLANNER_DRY_COOLDOWN_GRACE_MS, &dry_grace))
                    goto overflow;
                grace = dry_grace;
            }
            else if (atype == WASH_ACTION_UV) grace = PLANNER_UV_OFF_GRACE_MS;
            else if (atype == WASH_ACTION_PULSATOR_WASH || atype == WASH_ACTION_DRUM_WASH
                     || atype == WASH_ACTION_SPIN) grace = PLANNER_BL50_RAMP_GRACE_MS;
            uint32_t computed;
            uint32_t timeout_dur = dur;
            if (atype == WASH_ACTION_WATER_IN) {
                timeout_dur = 0;
                grace = config->water.total_inlet_timeout_ms > 0
                        ? config->water.total_inlet_timeout_ms : 600000U;
            }
            if (compute_service_timeout(timeout_dur, grace, move_timeout, &computed))
                goto overflow;
            step_timeout = computed;
        }

        /* Build and append step (user action, NOT system-inserted) */
        {
            wash_step_t s;
            memset(&s, 0, sizeof(s));
            s.type = action_to_step_type(atype);
            s.required_position = req_pos;
            s.duration_ms = dur;
            s.timeout_ms = step_timeout;
            s.rounds = 0;
            s.intensity = action->intensity;
            s.flags = 0;  /* user action */
            if (!append_step(&ctx, &s)) goto fail;
        }
    }

    /* Final drain if water still present */
    if (water_state == WATER_PLAN_PRESENT) {
        if (!auto_drain(&ctx, config, move_timeout, &water_state)) goto fail;
        if (add_duration(&ctx, config->drain.max_duration_ms > 0
                                ? config->drain.max_duration_ms : 120000U,
                         false))
            goto overflow;
    }

    /* Final MOVE @45 + FINISH */
    if (!ensure_move(&ctx, DRUM_POS_45, move_timeout)) goto fail;
    {
        wash_step_t s; memset(&s, 0, sizeof(s));
        s.type = STEP_FINISH;
        s.required_position = DRUM_POS_45;
        s.duration_ms = 0; s.timeout_ms = 0;
        s.flags = WASH_STEP_FLAG_SYSTEM_INSERTED;
        if (!append_step(&ctx, &s)) goto fail;
    }

    /* Check total limits */
    if (ctx.total_duration_ms > PLANNER_TOTAL_DURATION_MAX_MS) {
        return reject(PLAN_ERROR_TOTAL_LIMIT, UINT16_MAX, prog);
    }
    if (ctx.dry_duration_ms > PLANNER_DRY_CUMULATIVE_MAX_MS) {
        return reject(PLAN_ERROR_TOTAL_LIMIT, UINT16_MAX, prog);
    }

    prog->estimated_total_ms = ctx.total_duration_ms;
    report.result = PLAN_RESULT_OK;
    report.original_total_ms = ctx.total_duration_ms;
    report.final_total_ms = ctx.total_duration_ms;
    report.original_dry_ms = ctx.dry_duration_ms;
    report.final_dry_ms = ctx.dry_duration_ms;
    report.reject_action_index = UINT16_MAX;
    return report;

fail:
    report.result = PLAN_RESULT_REJECTED;
    report.reject_reason = PLAN_ERROR_STEP_OVERFLOW;
    memset(prog, 0, sizeof(*prog));
    return report;

overflow:
    report.result = PLAN_RESULT_REJECTED;
    report.reject_reason = PLAN_ERROR_DURATION_OVERFLOW;
    memset(prog, 0, sizeof(*prog));
    return report;
}

/* ================================================================
 * Public API
 * ================================================================ */

planner_report_t wash_planner_compile(
    const wash_intent_t *intent,
    const machine_config_t *config,
    uint32_t program_id,
    wash_program_t *out_program)
{
    if (!intent || !config || !out_program) {
        planner_report_t r;
        memset(&r, 0, sizeof(r));
        r.result = PLAN_RESULT_REJECTED;
        r.reject_reason = PLAN_ERROR_INVALID_ARG;
        r.reject_action_index = UINT16_MAX;
        if (out_program) memset(out_program, 0, sizeof(*out_program));
        return r;
    }

    if (program_id == 0) {
        return reject(PLAN_ERROR_INVALID_PROGRAM_ID, UINT16_MAX, out_program);
    }

    memset(out_program, 0, sizeof(*out_program));

    if (intent->action_count > WASH_INTENT_MAX_ACTIONS) {
        return reject(PLAN_ERROR_INVALID_ARG, UINT16_MAX, out_program);
    }

    /* Validate kind */
    if (intent->kind < WASH_PROGRAM_FORMAL || intent->kind > WASH_PROGRAM_CUSTOM) {
        return reject(PLAN_ERROR_INVALID_ACTION, UINT16_MAX, out_program);
    }

    /* Empty custom intent */
    if (intent->kind == WASH_PROGRAM_CUSTOM && intent->action_count == 0) {
        return reject(PLAN_ERROR_INVALID_ARG, UINT16_MAX, out_program);
    }

    /* Validate config */
    if (!machine_config_validate(config)) {
        return reject(PLAN_ERROR_INVALID_CONFIG, UINT16_MAX, out_program);
    }

    switch (intent->kind) {
    case WASH_PROGRAM_FORMAL:
        return build_formal(config, program_id, intent->allow_uv,
                            intent->allow_dry, out_program);
    case WASH_PROGRAM_DEMO:
        return build_demo(config, program_id, intent->allow_dry, out_program);
    case WASH_PROGRAM_CUSTOM:
        return build_custom(intent, config, program_id, out_program);
    default:
        return reject(PLAN_ERROR_INVALID_ACTION, UINT16_MAX, out_program);
    }
}
