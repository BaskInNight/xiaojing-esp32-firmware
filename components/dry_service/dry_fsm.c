#include "dry_fsm.h"
#include <string.h>

/* ================================================================
 * dry_fsm.c — 烘干状态机（纯 C，无 FreeRTOS/HAL 依赖）
 *
 * 硬规则：
 * - 只有 safety_manager_apply 返回 ESP_OK 才能提交 KNOWN_OFF
 * - UNKNOWN 永远不等于 OFF
 * - terminal 只在 PTC+FAN 均 KNOWN_OFF 时发出
 * - 温度 cutoff 是恒温周期，不是故障
 * ================================================================ */

static inline uint32_t elapsed_ms_int(int64_t now, int64_t start) {
    if (now <= start) return 0;
    return (uint32_t)(now - start);
}

static void no_action(dry_fsm_output_t *out, dry_fsm_state_t state) {
    out->next_state = state;
    out->action = DRY_ACTION_NONE;
    out->terminal = DRY_TERMINAL_NONE;
    out->fault_code = FAULT_NONE;
    out->fault_severity = FAULT_SEVERITY_WARNING;
    out->emit_event = false;
}

static void set_action(dry_fsm_ctx_t *ctx, dry_fsm_output_t *out,
                       dry_fsm_state_t state, dry_fsm_action_t action) {
    ctx->pending_action = action;
    ctx->action_seq++;
    out->next_state = state;
    out->action = action;
    out->terminal = DRY_TERMINAL_NONE;
    out->fault_code = FAULT_NONE;
    out->fault_severity = FAULT_SEVERITY_WARNING;
    out->emit_event = false;
}

static void emit_terminal(dry_fsm_output_t *out, dry_fsm_state_t state,
                          dry_terminal_t terminal,
                          machine_fault_code_t fc, fault_severity_t sev) {
    out->next_state = state;
    out->action = DRY_ACTION_NONE;
    out->terminal = terminal;
    out->fault_code = fc;
    out->fault_severity = sev;
    out->emit_event = true;
}

static void enter_state(dry_fsm_ctx_t *ctx, dry_fsm_state_t state) {
    ctx->state = state;
    ctx->state_enter_ms = ctx->tick_ms;
}

static void set_fault(dry_fsm_ctx_t *ctx, dry_fault_code_t dfc,
                      machine_fault_code_t mfc, fault_severity_t sev) {
    ctx->fault_code = dfc;
    ctx->m_fault_code = mfc;
    ctx->fault_severity = sev;
}

static bool position_ok(const dry_fsm_ctx_t *ctx) {
    return ctx->position_valid && ctx->position_stable &&
           ctx->position_fresh && !ctx->position_motor_moving &&
           ctx->position == DRUM_POS_270;
}

static bool sht_ok(const dry_fsm_ctx_t *ctx) {
    return ctx->sht_valid && ctx->sht_fresh;
}

/* ---- Internal commit ---- */

static esp_err_t commit_result(dry_fsm_ctx_t *ctx, dry_fsm_action_t action,
                               bool success) {
    if (ctx->pending_action == DRY_ACTION_NONE)
        return ESP_ERR_INVALID_STATE;
    if (action != ctx->pending_action)
        return ESP_ERR_INVALID_ARG;
    if (action == DRY_ACTION_NONE || action >= DRY_ACTION_COUNT) {
        ctx->pending_action = DRY_ACTION_NONE;
        return ESP_ERR_INVALID_ARG;
    }

    switch (action) {
    case DRY_ACTION_FAN_ON:
        ctx->fan_state = success ? DRY_OUTPUT_KNOWN_ON : DRY_OUTPUT_UNKNOWN;
        break;
    case DRY_ACTION_FAN_OFF:
        ctx->fan_state = success ? DRY_OUTPUT_KNOWN_OFF : DRY_OUTPUT_UNKNOWN;
        break;
    case DRY_ACTION_HEATER_ON:
        if (success) {
            ctx->heater_state = DRY_OUTPUT_KNOWN_ON;
            ctx->heater_ever_confirmed_on = true;
            ctx->ptc_on_since_ms = ctx->tick_ms;
        } else {
            ctx->heater_state = DRY_OUTPUT_UNKNOWN;
        }
        break;
    case DRY_ACTION_HEATER_OFF:
        if (success) {
            ctx->heater_state = DRY_OUTPUT_KNOWN_OFF;
            ctx->ptc_off_since_ms = ctx->tick_ms;
        } else {
            ctx->heater_state = DRY_OUTPUT_UNKNOWN;
        }
        break;
    default:
        break;
    }
    ctx->pending_action = DRY_ACTION_NONE;
    return ESP_OK;
}

/* ---- Shutdown sequencer ---- */

#define PTC_OFF_FAST_RETRY_MS  500
#define PTC_OFF_SLOW_RETRY_MS  2000
#define PTC_OFF_FAST_RETRIES   10

static void begin_shutdown(dry_fsm_ctx_t *ctx, dry_fsm_output_t *out,
                           dry_shutdown_reason_t reason) {
    ctx->shutdown_reason = reason;
    ctx->ptc_off_retry_count = 0;

    if (reason == DRY_SHUTDOWN_FAULT) {
        enter_state(ctx, DRY_FSM_FAULT);
    } else if (reason == DRY_SHUTDOWN_CANCELED || reason == DRY_SHUTDOWN_EMERGENCY) {
        ctx->shutdown_reason = DRY_SHUTDOWN_CANCELED;
        enter_state(ctx, DRY_FSM_CANCELING);
    } else {
        enter_state(ctx, DRY_FSM_FINISHING);
    }

    /* Step 1: request HEATER_OFF if ON */
    if (ctx->heater_state == DRY_OUTPUT_KNOWN_ON) {
        set_action(ctx, out, ctx->state, DRY_ACTION_HEATER_OFF);
    } else if (ctx->heater_state == DRY_OUTPUT_UNKNOWN &&
               ctx->heater_ever_confirmed_on) {
        /* Was on, now unknown — must retry OFF */
        set_action(ctx, out, ctx->state, DRY_ACTION_HEATER_OFF);
    } else {
        /* Heater never confirmed on or already OFF — skip to cooldown check */
        no_action(out, ctx->state);
    }
}

/* Unified shutdown handler for FINISHING/CANCELING/FAULT states.
 * Sequence: ENSURE_HEATER_OFF → COOLDOWN → ENSURE_FAN_OFF → COMPLETE */
static void handle_shutdown(dry_fsm_ctx_t *ctx, dry_fsm_output_t *out,
                            dry_fsm_state_t my_state) {
    /* Phase 1: Ensure PTC OFF */
    if (ctx->heater_state == DRY_OUTPUT_KNOWN_ON) {
        ctx->ptc_off_retry_count = 0;
        set_action(ctx, out, my_state, DRY_ACTION_HEATER_OFF);
        return;
    }
    if (ctx->heater_state == DRY_OUTPUT_UNKNOWN &&
        ctx->heater_ever_confirmed_on) {
        /* Retry with backoff — NEVER force to KNOWN_OFF */
        uint32_t retry_interval = (ctx->ptc_off_retry_count < PTC_OFF_FAST_RETRIES)
                                  ? PTC_OFF_FAST_RETRY_MS : PTC_OFF_SLOW_RETRY_MS;
        uint32_t since_enter = elapsed_ms_int(ctx->tick_ms, ctx->state_enter_ms);
        if (since_enter >= retry_interval) {
            ctx->ptc_off_retry_count++;
            ctx->state_enter_ms = ctx->tick_ms;
            set_action(ctx, out, my_state, DRY_ACTION_HEATER_OFF);
        } else {
            no_action(out, my_state);
        }
        return;
        /* PTC stays UNKNOWN — terminal will never be emitted,
         * fan keeps running, stop() will timeout and preserve resources */
    }

    /* Phase 2: Cooldown — only if heater was ever confirmed on */
    if (ctx->heater_ever_confirmed_on) {
        if (ctx->cooldown_start_ms <= 0) {
            ctx->cooldown_start_ms = ctx->tick_ms;
        }
        uint32_t cool_elapsed = elapsed_ms_int(ctx->tick_ms, ctx->cooldown_start_ms);
        if (cool_elapsed < ctx->cooldown_ms) {
            ctx->cooldown_remaining_ms = ctx->cooldown_ms - cool_elapsed;
            no_action(out, my_state);
            return;
        }
    }

    /* Phase 3: Ensure FAN OFF */
    if (ctx->fan_state == DRY_OUTPUT_KNOWN_ON) {
        enter_state(ctx, DRY_FSM_FAN_OFF_WAIT);
        set_action(ctx, out, DRY_FSM_FAN_OFF_WAIT, DRY_ACTION_FAN_OFF);
        return;
    }
    if (ctx->fan_state == DRY_OUTPUT_UNKNOWN) {
        /* Fan OFF failed — retry */
        enter_state(ctx, DRY_FSM_FAN_OFF_WAIT);
        set_action(ctx, out, DRY_FSM_FAN_OFF_WAIT, DRY_ACTION_FAN_OFF);
        return;
    }

    /* Phase 4: Both KNOWN_OFF — emit terminal */
    if (ctx->fan_state == DRY_OUTPUT_KNOWN_OFF &&
        ctx->heater_state == DRY_OUTPUT_KNOWN_OFF) {
        enter_state(ctx, DRY_FSM_COMPLETE);
        no_action(out, DRY_FSM_COMPLETE);
    } else {
        /* Should not reach here — stay in current state */
        no_action(out, my_state);
    }
}

/* ---- State Handlers ---- */

static void handle_idle(dry_fsm_ctx_t *ctx, const dry_fsm_event_t *evt,
                        dry_fsm_output_t *out) {
    no_action(out, DRY_FSM_IDLE);
}

static void handle_validating(dry_fsm_ctx_t *ctx, const dry_fsm_event_t *evt,
                              dry_fsm_output_t *out) {
    if (!ctx->position_valid) {
        begin_shutdown(ctx, out, DRY_SHUTDOWN_FAULT);
        set_fault(ctx, DRY_FAULT_POSITION_LOST,
                  FAULT_POSITION_TIMEOUT, FAULT_SEVERITY_RECOVERABLE);
        return;
    }
    if (!ctx->position_stable || !ctx->position_fresh ||
        ctx->position_motor_moving || ctx->position != DRUM_POS_270) {
        begin_shutdown(ctx, out, DRY_SHUTDOWN_FAULT);
        set_fault(ctx, DRY_FAULT_POSITION_LOST,
                  FAULT_POSITION_TIMEOUT, FAULT_SEVERITY_RECOVERABLE);
        return;
    }
    /* Position OK → establish output baseline: ensure PTC off */
    enter_state(ctx, DRY_FSM_ENSURE_PTC_OFF);
    no_action(out, DRY_FSM_ENSURE_PTC_OFF);
}

/* Baseline: confirm PTC KNOWN_OFF before starting */
static void handle_ensure_ptc_off(dry_fsm_ctx_t *ctx,
                                  const dry_fsm_event_t *evt,
                                  dry_fsm_output_t *out) {
    if (ctx->heater_state == DRY_OUTPUT_KNOWN_OFF) {
        /* PTC confirmed off → next: ensure fan off */
        enter_state(ctx, DRY_FSM_ENSURE_FAN_OFF);
        no_action(out, DRY_FSM_ENSURE_FAN_OFF);
        return;
    }
    if (ctx->heater_state == DRY_OUTPUT_UNKNOWN) {
        /* Need to send HEATER_OFF to establish baseline */
        set_action(ctx, out, DRY_FSM_ENSURE_PTC_OFF, DRY_ACTION_HEATER_OFF);
        return;
    }
    /* KNOWN_ON shouldn't happen at this point, but handle it */
    set_action(ctx, out, DRY_FSM_ENSURE_PTC_OFF, DRY_ACTION_HEATER_OFF);
}

/* Baseline: confirm FAN KNOWN_OFF before starting */
static void handle_ensure_fan_off(dry_fsm_ctx_t *ctx,
                                  const dry_fsm_event_t *evt,
                                  dry_fsm_output_t *out) {
    if (ctx->fan_state == DRY_OUTPUT_KNOWN_OFF) {
        /* Both confirmed off → start fan */
        enter_state(ctx, DRY_FSM_STARTING_FAN);
        no_action(out, DRY_FSM_STARTING_FAN);
        return;
    }
    if (ctx->fan_state == DRY_OUTPUT_UNKNOWN) {
        set_action(ctx, out, DRY_FSM_ENSURE_FAN_OFF, DRY_ACTION_FAN_OFF);
        return;
    }
    /* KNOWN_ON — shouldn't happen, but send OFF */
    set_action(ctx, out, DRY_FSM_ENSURE_FAN_OFF, DRY_ACTION_FAN_OFF);
}

static void handle_starting_fan(dry_fsm_ctx_t *ctx,
                                const dry_fsm_event_t *evt,
                                dry_fsm_output_t *out) {
    if (ctx->fan_state != DRY_OUTPUT_KNOWN_ON) {
        set_action(ctx, out, DRY_FSM_STARTING_FAN, DRY_ACTION_FAN_ON);
        return;
    }
    ctx->fan_on_since_ms = ctx->tick_ms;
    enter_state(ctx, DRY_FSM_PRE_FAN);
    no_action(out, DRY_FSM_PRE_FAN);
}

static void handle_pre_fan(dry_fsm_ctx_t *ctx, const dry_fsm_event_t *evt,
                           dry_fsm_output_t *out) {
    if (!position_ok(ctx)) {
        begin_shutdown(ctx, out, DRY_SHUTDOWN_CANCELED);
        return;
    }
    uint32_t fan_elapsed = elapsed_ms_int(ctx->tick_ms, ctx->fan_on_since_ms);
    uint32_t total_elapsed = elapsed_ms_int(ctx->tick_ms, ctx->heating_start_ms);

    if (total_elapsed >= ctx->duration_ms) {
        begin_shutdown(ctx, out, DRY_SHUTDOWN_NORMAL);
        return;
    }
    if (fan_elapsed >= ctx->pre_fan_ms) {
        if (ctx->heater_requested) {
            enter_state(ctx, DRY_FSM_HEATING);
            no_action(out, DRY_FSM_HEATING);
        }
        /* fan-only: stay in PRE_FAN */
    } else {
        no_action(out, DRY_FSM_PRE_FAN);
    }
}

static void handle_heating(dry_fsm_ctx_t *ctx, const dry_fsm_event_t *evt,
                           dry_fsm_output_t *out) {
    /* Request PTC ON if not already on */
    if (ctx->heater_state != DRY_OUTPUT_KNOWN_ON) {
        if (!sht_ok(ctx)) {
            begin_shutdown(ctx, out, DRY_SHUTDOWN_FAULT);
            set_fault(ctx, DRY_FAULT_SHT_IO,
                      FAULT_SHT_INVALID, FAULT_SEVERITY_RECOVERABLE);
            return;
        }
        if (ctx->temperature_c >= ctx->heater_cutoff_c) {
            begin_shutdown(ctx, out, DRY_SHUTDOWN_FAULT);
            set_fault(ctx, DRY_FAULT_OVERTEMP,
                      FAULT_DRY_OVERTEMP, FAULT_SEVERITY_LATCHED);
            return;
        }
        set_action(ctx, out, DRY_FSM_HEATING, DRY_ACTION_HEATER_ON);
        return;
    }

    /* Temperature cutoff → HEAT_REST (thermostat cycle, not fault) */
    if (sht_ok(ctx) && ctx->temperature_c >= ctx->heater_cutoff_c) {
        ctx->current_heat_cycle_ms = elapsed_ms_int(ctx->tick_ms, ctx->ptc_on_since_ms);
        ctx->heating_total_ms += ctx->current_heat_cycle_ms;
        enter_state(ctx, DRY_FSM_HEAT_REST);
        set_action(ctx, out, DRY_FSM_HEAT_REST, DRY_ACTION_HEATER_OFF);
        return;
    }

    /* heat_on_max_ms per cycle → HEAT_REST */
    uint32_t cycle_elapsed = elapsed_ms_int(ctx->tick_ms, ctx->ptc_on_since_ms);
    if (cycle_elapsed >= ctx->heat_on_max_ms) {
        ctx->heating_total_ms += cycle_elapsed;
        enter_state(ctx, DRY_FSM_HEAT_REST);
        set_action(ctx, out, DRY_FSM_HEAT_REST, DRY_ACTION_HEATER_OFF);
        return;
    }

    /* Position check */
    if (!position_ok(ctx)) {
        ctx->current_heat_cycle_ms = cycle_elapsed;
        ctx->heating_total_ms += cycle_elapsed;
        begin_shutdown(ctx, out, DRY_SHUTDOWN_CANCELED);
        return;
    }

    /* SHT check */
    if (!sht_ok(ctx)) {
        ctx->current_heat_cycle_ms = cycle_elapsed;
        ctx->heating_total_ms += cycle_elapsed;
        begin_shutdown(ctx, out, DRY_SHUTDOWN_FAULT);
        set_fault(ctx, DRY_FAULT_SHT_IO,
                  FAULT_SHT_INVALID, FAULT_SEVERITY_RECOVERABLE);
        return;
    }

    /* Emergency/fault from safety layer */
    if (evt->emergency_active || evt->fault_active) {
        ctx->current_heat_cycle_ms = cycle_elapsed;
        ctx->heating_total_ms += cycle_elapsed;
        begin_shutdown(ctx, out, DRY_SHUTDOWN_CANCELED);
        return;
    }

    /* Duration check → normal finish */
    uint32_t total_elapsed = elapsed_ms_int(ctx->tick_ms, ctx->heating_start_ms);
    if (total_elapsed >= ctx->duration_ms) {
        ctx->current_heat_cycle_ms = cycle_elapsed;
        ctx->heating_total_ms += cycle_elapsed;
        begin_shutdown(ctx, out, DRY_SHUTDOWN_NORMAL);
        return;
    }

    ctx->current_heat_cycle_ms = cycle_elapsed;
    no_action(out, DRY_FSM_HEATING);
}

static void handle_heat_rest(dry_fsm_ctx_t *ctx, const dry_fsm_event_t *evt,
                             dry_fsm_output_t *out) {
    if (!position_ok(ctx)) {
        begin_shutdown(ctx, out, DRY_SHUTDOWN_CANCELED);
        return;
    }

    uint32_t rest_elapsed = elapsed_ms_int(ctx->tick_ms, ctx->ptc_off_since_ms);
    bool rest_time_ok = rest_elapsed >= ctx->heat_off_min_ms;
    bool temp_ok = sht_ok(ctx) && ctx->temperature_c <= ctx->heater_resume_c;

    uint32_t total_elapsed = elapsed_ms_int(ctx->tick_ms, ctx->heating_start_ms);
    if (total_elapsed >= ctx->duration_ms) {
        begin_shutdown(ctx, out, DRY_SHUTDOWN_NORMAL);
        return;
    }

    if (rest_time_ok && temp_ok && sht_ok(ctx) && position_ok(ctx)) {
        enter_state(ctx, DRY_FSM_HEATING);
        no_action(out, DRY_FSM_HEATING);
    } else {
        no_action(out, DRY_FSM_HEAT_REST);
    }
}

static void handle_fan_off_wait(dry_fsm_ctx_t *ctx,
                                const dry_fsm_event_t *evt,
                                dry_fsm_output_t *out) {
    if (ctx->fan_state == DRY_OUTPUT_KNOWN_ON || ctx->fan_state == DRY_OUTPUT_UNKNOWN) {
        set_action(ctx, out, DRY_FSM_FAN_OFF_WAIT, DRY_ACTION_FAN_OFF);
        return;
    }
    /* Fan confirmed OFF */
    if (ctx->fan_state == DRY_OUTPUT_KNOWN_OFF &&
        ctx->heater_state == DRY_OUTPUT_KNOWN_OFF) {
        enter_state(ctx, DRY_FSM_COMPLETE);
        no_action(out, DRY_FSM_COMPLETE);
    } else {
        /* Heater became unknown during fan-off — shouldn't happen */
        no_action(out, DRY_FSM_FAN_OFF_WAIT);
    }
}

static void handle_complete(dry_fsm_ctx_t *ctx, const dry_fsm_event_t *evt,
                            dry_fsm_output_t *out) {
    if (!ctx->terminal_event_emitted) {
        dry_terminal_t term;
        if (ctx->fault_code != DRY_FAULT_NONE ||
            ctx->m_fault_code != FAULT_NONE) {
            term = DRY_TERMINAL_FAULT;
        } else if (ctx->shutdown_reason == DRY_SHUTDOWN_CANCELED) {
            term = DRY_TERMINAL_CANCELED;
        } else {
            term = DRY_TERMINAL_COMPLETE;
        }
        ctx->terminal = term;
        emit_terminal(out, DRY_FSM_COMPLETE, term,
                      ctx->m_fault_code, ctx->fault_severity);
    } else {
        enter_state(ctx, DRY_FSM_IDLE);
        no_action(out, DRY_FSM_IDLE);
    }
}

static void handle_canceling(dry_fsm_ctx_t *ctx, const dry_fsm_event_t *evt,
                             dry_fsm_output_t *out) {
    handle_shutdown(ctx, out, DRY_FSM_CANCELING);
}

static void handle_fault(dry_fsm_ctx_t *ctx, const dry_fsm_event_t *evt,
                         dry_fsm_output_t *out) {
    handle_shutdown(ctx, out, DRY_FSM_FAULT);
}

static void handle_finishing(dry_fsm_ctx_t *ctx, const dry_fsm_event_t *evt,
                             dry_fsm_output_t *out) {
    handle_shutdown(ctx, out, DRY_FSM_FINISHING);
}

/* ---- Action result handlers ---- */

static void handle_action_result(dry_fsm_ctx_t *ctx,
                                 dry_fsm_action_t action, bool success,
                                 dry_fsm_output_t *out) {
    switch (ctx->state) {
    case DRY_FSM_ENSURE_PTC_OFF:
        if (ctx->heater_state == DRY_OUTPUT_KNOWN_OFF) {
            enter_state(ctx, DRY_FSM_ENSURE_FAN_OFF);
            no_action(out, DRY_FSM_ENSURE_FAN_OFF);
        } else {
            /* Still UNKNOWN — retry */
            no_action(out, DRY_FSM_ENSURE_PTC_OFF);
        }
        return;

    case DRY_FSM_ENSURE_FAN_OFF:
        if (ctx->fan_state == DRY_OUTPUT_KNOWN_OFF) {
            enter_state(ctx, DRY_FSM_STARTING_FAN);
            no_action(out, DRY_FSM_STARTING_FAN);
        } else {
            no_action(out, DRY_FSM_ENSURE_FAN_OFF);
        }
        return;

    case DRY_FSM_STARTING_FAN:
        if (action == DRY_ACTION_FAN_ON) {
            if (success) {
                ctx->fan_on_since_ms = ctx->tick_ms;
                enter_state(ctx, DRY_FSM_PRE_FAN);
                no_action(out, DRY_FSM_PRE_FAN);
            } else {
                begin_shutdown(ctx, out, DRY_SHUTDOWN_FAULT);
                set_fault(ctx, DRY_FAULT_FAN_APPLY,
                          FAULT_INTERNAL, FAULT_SEVERITY_RECOVERABLE);
            }
        }
        return;

    case DRY_FSM_HEATING:
        if (action == DRY_ACTION_HEATER_ON) {
            if (success) {
                no_action(out, DRY_FSM_HEATING);
            } else {
                begin_shutdown(ctx, out, DRY_SHUTDOWN_FAULT);
                set_fault(ctx, DRY_FAULT_PTC_APPLY,
                          FAULT_INTERNAL, FAULT_SEVERITY_RECOVERABLE);
            }
        }
        return;

    case DRY_FSM_HEAT_REST:
        if (action == DRY_ACTION_HEATER_OFF) {
            /* HEATER_OFF confirmed in HEAT_REST */
            no_action(out, DRY_FSM_HEAT_REST);
        }
        return;

    case DRY_FSM_FINISHING:
    case DRY_FSM_CANCELING:
    case DRY_FSM_FAULT:
        /* Shutdown states: re-evaluate via handle_shutdown */
        handle_shutdown(ctx, out, ctx->state);
        return;

    case DRY_FSM_FAN_OFF_WAIT:
        handle_fan_off_wait(ctx, NULL, out);
        return;

    default:
        break;
    }
    no_action(out, ctx->state);
}

/* ---- Public API ---- */

void dry_fsm_init(dry_fsm_ctx_t *ctx, const dry_params_t *config) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->state = DRY_FSM_IDLE;
    ctx->fan_state = DRY_OUTPUT_UNKNOWN;
    ctx->heater_state = DRY_OUTPUT_UNKNOWN;
    ctx->pre_fan_ms = config->pre_fan_ms;
    ctx->heat_on_max_ms = config->heat_on_max_ms;
    ctx->heat_off_min_ms = config->heat_off_min_ms;
    ctx->cooldown_ms = config->cooldown_ms;
    ctx->max_total_ms = config->max_total_ms;
    ctx->heater_cutoff_c = config->heater_cutoff_c;
    ctx->heater_resume_c = config->heater_resume_c;
    ctx->fan_percent = config->fan_percent;
}

void dry_fsm_begin_request(dry_fsm_ctx_t *ctx, const dry_request_t *req,
                           int64_t now_ms) {
    ctx->request_id = req->request_id;
    ctx->duration_ms = req->duration_ms;
    ctx->heater_requested = req->heater_requested;
    ctx->heating_start_ms = now_ms;
    ctx->tick_ms = now_ms;
    ctx->state = DRY_FSM_VALIDATING;
    ctx->state_enter_ms = now_ms;
    ctx->shutdown_reason = DRY_SHUTDOWN_NONE;
    ctx->pending_action = DRY_ACTION_NONE;
    ctx->action_seq = 0;
    ctx->fan_state = DRY_OUTPUT_UNKNOWN;
    ctx->heater_state = DRY_OUTPUT_UNKNOWN;
    ctx->heater_ever_confirmed_on = false;
    ctx->heating_total_ms = 0;
    ctx->current_heat_cycle_ms = 0;
    ctx->ptc_on_since_ms = 0;
    ctx->ptc_off_since_ms = 0;
    ctx->fan_on_since_ms = 0;
    ctx->cooldown_start_ms = 0;
    ctx->cooldown_remaining_ms = 0;
    ctx->ptc_off_retry_count = 0;
    ctx->fault_code = DRY_FAULT_NONE;
    ctx->m_fault_code = FAULT_NONE;
    ctx->fault_severity = FAULT_SEVERITY_WARNING;
    ctx->terminal_event_emitted = false;
    ctx->terminal = DRY_TERMINAL_NONE;
    memset(&ctx->last_result, 0, sizeof(ctx->last_result));
    ctx->revision++;
}

void dry_fsm_tick(dry_fsm_ctx_t *ctx, const dry_fsm_event_t *evt,
                  dry_fsm_output_t *out) {
    if (evt->now_ms > 0) {
        ctx->tick_ms = evt->now_ms;
    }

    if (evt->type == DRY_EVT_TICK) {
        ctx->position = evt->position;
        ctx->position_valid = evt->position_valid;
        ctx->position_stable = evt->position_stable;
        ctx->position_fresh = evt->position_fresh;
        ctx->position_motor_moving = evt->position_motor_moving;
        ctx->sht_valid = evt->sht_valid;
        ctx->sht_fresh = evt->sht_fresh;
        ctx->temperature_c = evt->temperature_c;
        ctx->humidity_percent = evt->humidity_percent;
    }

    /* ACTION_RESULT */
    if (evt->type == DRY_EVT_ACTION_RESULT) {
        esp_err_t cr = commit_result(ctx, evt->action_result_action,
                                     evt->action_result_ok);
        bool commit_ok = (cr == ESP_OK) && evt->action_result_ok;
        ctx->last_result.valid = true;
        ctx->last_result.action = evt->action_result_action;
        ctx->last_result.success = commit_ok;
        handle_action_result(ctx, evt->action_result_action, commit_ok, out);
        return;
    }

    /* CANCEL */
    if (evt->type == DRY_EVT_CANCEL) {
        if (evt->cancel_request_id != ctx->request_id) {
            no_action(out, ctx->state);
            return;
        }
        if (ctx->state == DRY_FSM_IDLE || ctx->state == DRY_FSM_COMPLETE ||
            ctx->state == DRY_FSM_FAULT || ctx->state == DRY_FSM_CANCELING ||
            ctx->state == DRY_FSM_FINISHING || ctx->state == DRY_FSM_FAN_OFF_WAIT) {
            no_action(out, ctx->state);
            return;
        }
        begin_shutdown(ctx, out, DRY_SHUTDOWN_CANCELED);
        return;
    }

    /* EMERGENCY */
    if (evt->type == DRY_EVT_EMERGENCY) {
        if (ctx->state == DRY_FSM_IDLE || ctx->state == DRY_FSM_COMPLETE ||
            ctx->state == DRY_FSM_CANCELING || ctx->state == DRY_FSM_FAULT ||
            ctx->state == DRY_FSM_FINISHING || ctx->state == DRY_FSM_FAN_OFF_WAIT) {
            no_action(out, ctx->state);
            return;
        }
        begin_shutdown(ctx, out, DRY_SHUTDOWN_EMERGENCY);
        return;
    }

    /* Pending action → wait */
    if (ctx->pending_action != DRY_ACTION_NONE) {
        no_action(out, ctx->state);
        return;
    }

    /* Dispatch */
    switch (ctx->state) {
    case DRY_FSM_IDLE:            handle_idle(ctx, evt, out); break;
    case DRY_FSM_VALIDATING:      handle_validating(ctx, evt, out); break;
    case DRY_FSM_ENSURE_PTC_OFF:  handle_ensure_ptc_off(ctx, evt, out); break;
    case DRY_FSM_ENSURE_FAN_OFF:  handle_ensure_fan_off(ctx, evt, out); break;
    case DRY_FSM_STARTING_FAN:    handle_starting_fan(ctx, evt, out); break;
    case DRY_FSM_PRE_FAN:         handle_pre_fan(ctx, evt, out); break;
    case DRY_FSM_HEATING:         handle_heating(ctx, evt, out); break;
    case DRY_FSM_HEAT_REST:       handle_heat_rest(ctx, evt, out); break;
    case DRY_FSM_FINISHING:       handle_finishing(ctx, evt, out); break;
    case DRY_FSM_FAN_OFF_WAIT:    handle_fan_off_wait(ctx, evt, out); break;
    case DRY_FSM_COMPLETE:        handle_complete(ctx, evt, out); break;
    case DRY_FSM_CANCELING:       handle_canceling(ctx, evt, out); break;
    case DRY_FSM_FAULT:           handle_fault(ctx, evt, out); break;
    default:                      no_action(out, ctx->state); break;
    }
}

void dry_fsm_mark_terminal_emitted(dry_fsm_ctx_t *ctx) {
    ctx->terminal_event_emitted = true;
    ctx->revision++;
}

/* ---- 初始安全 OFF 同步（fan/heater 板测前置） ---- */

esp_err_t dry_fsm_commit_initial_off(dry_fsm_ctx_t *ctx,
                                     bool fan_off_ok, bool heater_off_ok)
{
    if (!ctx) return ESP_ERR_INVALID_ARG;
    if (ctx->state != DRY_FSM_IDLE) return ESP_ERR_INVALID_STATE;
    ctx->fan_state = fan_off_ok ? DRY_OUTPUT_KNOWN_OFF : DRY_OUTPUT_UNKNOWN;
    ctx->heater_state = heater_off_ok ? DRY_OUTPUT_KNOWN_OFF : DRY_OUTPUT_UNKNOWN;
    ctx->revision++;
    return ESP_OK;
}

bool dry_fsm_terminal_pending(const dry_fsm_ctx_t *ctx)
{
    if (!ctx) return false;
    return !ctx->terminal_event_emitted &&
           ctx->terminal != DRY_TERMINAL_NONE;
}
