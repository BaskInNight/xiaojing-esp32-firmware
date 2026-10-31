#include "wash_planner.h"
#include <stdio.h>
#include <string.h>

bool wash_recipe_validate(const wash_recipe_t *r, uint32_t *estimated_ms)
{
    if (!r || !r->count || r->count > WASH_RECIPE_MAX_ACTIONS) return false;
    bool washed = false, spun = false, dried = false, water = false;
    uint32_t estimate = 30000; /* Verified loading->0, fixed two-valve dose, final 45. */
    for (unsigned i = 0; i < r->count; ++i) {
        const wash_recipe_action_t *a = &r->actions[i];
        switch (a->op) {
        case RECIPE_MOVE:
            if (!drum_position_is_valid((drum_position_t)a->position) || a->seconds || a->level) return false;
            estimate += 10000; break;
        case RECIPE_WATER:
            if (i != 0 || water || a->level || a->seconds != 5 || a->position) return false;
            water = true; break; /* Local two-stage water/detergent is mandatory and runs once. */
        case RECIPE_PULSATOR:
        case RECIPE_DRUM:
            if (spun || dried || a->level < 1 || a->level > 3 || a->seconds < 5 || a->seconds > 30 || a->position)
                return false;
            washed = true; estimate += 13000 + a->seconds * 1000U; break;
        case RECIPE_SPIN:
            if (!washed || spun || dried || a->level < 1 || a->level > 3 ||
                a->seconds < 5 || a->seconds > 12 || a->position) return false;
            spun = true; estimate += 13000 + a->seconds * 1000U; break;
        case RECIPE_DRY:
            if (!washed || dried || !r->allow_dry || a->level != 1 ||
                a->seconds < 5 || a->seconds > 15 || a->position) return false;
            dried = true; estimate += 13000 + a->seconds * 1000U; break;
        case RECIPE_UV:
            if (!washed || !r->allow_uv || a->level != 1 ||
                a->seconds < 1 || a->seconds > 15 || a->position) return false;
            estimate += a->seconds * 1000U; break;
        default: return false;
        }
        if (estimate > 176000U) return false;
    }
    if (!washed) return false;
    if (estimated_ms) *estimated_ms = estimate;
    return true;
}

planner_report_t wash_recipe_compile(const wash_recipe_t *r, uint32_t id, wash_program_t *p)
{
    planner_report_t result = {.result = PLAN_RESULT_REJECTED, .reject_reason = PLAN_ERROR_INVALID_ACTION};
    if (!p) return result;
    memset(p, 0, sizeof(*p));
    uint32_t estimate;
    if (!id || !wash_recipe_validate(r, &estimate)) return result;
    p->program_id = id; p->kind = WASH_PROGRAM_CUSTOM; p->estimated_total_ms = estimate;
    p->step_count = 3; p->recipe = *r;
    p->steps[0] = (wash_step_t){.step_id = 0, .type = STEP_MOVE_POSITION,
        .required_position = DRUM_POS_45, .timeout_ms = 31000, .flags = WASH_STEP_FLAG_REQUIRED};
    p->steps[1] = (wash_step_t){.step_id = 1, .type = STEP_WAIT_LOAD_CONFIRM,
        .required_position = DRUM_POS_45, .flags = WASH_STEP_FLAG_REQUIRED};
    p->steps[2] = (wash_step_t){.step_id = 2, .type = STEP_COMPETITION_DEMO,
        .required_position = DRUM_POS_UNKNOWN, .timeout_ms = 190000, .flags = WASH_STEP_FLAG_REQUIRED};
    result.result = PLAN_RESULT_OK; result.final_total_ms = estimate;
    return result;
}

bool wash_recipe_summary(const wash_recipe_t *r, bool pending, char *text, size_t capacity)
{
    if (!text || !capacity || !wash_recipe_validate(r, NULL)) return false;
    size_t used = 0;
    int n = snprintf(text, capacity, "未上水演示方案：");
    if (n < 0 || (size_t)n >= capacity) return false;
    used = (size_t)n;
    for (unsigned i = 0; i < r->count; ++i) {
        const wash_recipe_action_t *a = &r->actions[i];
        const char *name = a->op == RECIPE_PULSATOR ? "波轮" : a->op == RECIPE_DRUM ? "滚筒" :
            a->op == RECIPE_SPIN ? "脱水" : a->op == RECIPE_DRY ? "烘干展示" :
            a->op == RECIPE_UV ? "示意灯" : NULL;
        if (!name) continue;
        n = snprintf(text + used, capacity - used, "%s%u秒，", name, a->seconds);
        if (n < 0 || (size_t)n >= capacity - used) return false;
        used += (size_t)n;
    }
    n = snprintf(text + used, capacity - used, "%s", pending ?
        "按按钮1确认方案并前往装衣位。" : "正在前往45度装衣位，放好后按按钮1确认。");
    return n >= 0 && (size_t)n < capacity - used;
}
