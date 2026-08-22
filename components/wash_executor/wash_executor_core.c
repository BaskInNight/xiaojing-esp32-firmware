/*
 * wash_executor_core.c — Pure C executor state machine (Round 1.1)
 *
 * Phase model: IDLE → DISPATCHING → ACTIVE → (CANCELING →) RESETTING → IDLE
 *   or → WAIT_LOAD / WAIT_UNLOAD / FAULT
 *
 * Terminal token: program_id + generation + step_id + request_id + service_kind
 * Command pump: chained, capacity-safe, max 16 iterations.
 */

#include "wash_executor_core.h"
#include <string.h>

#define CMD_PUMP_MAX_ITERATIONS 16

/* ================================================================
 * Request ID: 1-based, monotonic, wrap skips 0
 * ================================================================ */

uint16_t core_next_request_id(wash_executor_core_t *core)
{
    uint16_t id = core->next_request_id;
    core->next_request_id++;
    if (core->next_request_id == 0) core->next_request_id = 1;
    return id;
}

/* ================================================================
 * Token helpers
 * ================================================================ */

/* Exact 6-field comparison */
bool terminal_token_equal_exact(const terminal_token_t *a, const terminal_token_t *b)
{
    if (!a || !b) return false;
    return a->program_id == b->program_id
        && a->generation == b->generation
        && a->step_id == b->step_id
        && a->request_id == b->request_id
        && a->service_kind == b->service_kind
        && a->terminal_kind == b->terminal_kind;
}

terminal_token_t core_build_token(const wash_executor_core_t *core, uint16_t request_id)
{
    terminal_token_t t;
    memset(&t, 0, sizeof(t));
    t.program_id = core->program_id;
    t.generation = core->generation;
    if (core->program && core->current_step_index < core->total_steps) {
        t.step_id = core->program->steps[core->current_step_index].step_id;
        t.service_kind = (uint8_t)core->program->steps[core->current_step_index].type;
    }
    t.request_id = request_id;
    t.terminal_kind = TERMINAL_KIND_NORMAL;
    return t;
}

bool core_token_matches_active(
    const wash_executor_core_t *core,
    const terminal_token_t *token)
{
    if (!core->token_valid || !token) return false;
    /* Active terminal must be NORMAL kind */
    if (token->terminal_kind != TERMINAL_KIND_NORMAL) return false;
    return terminal_token_equal_exact(token, &core->active_token);
}

bool core_token_matches_cancel(
    const wash_executor_core_t *core,
    const terminal_token_t *token)
{
    if (!core->cancel_token_valid || !token) return false;
    /* Cancel ACK must be CANCEL_ACK kind */
    if (token->terminal_kind != TERMINAL_KIND_CANCEL_ACK) return false;
    return terminal_token_equal_exact(token, &core->cancel_token);
}

bool core_token_matches_reset(
    const wash_executor_core_t *core,
    const terminal_token_t *token)
{
    if (!core->reset_token_valid || !token) return false;
    /* Reset ACK must be RESET_ACK kind */
    if (token->terminal_kind != TERMINAL_KIND_RESET_ACK) return false;
    return terminal_token_equal_exact(token, &core->reset_token);
}

/* ================================================================
 * Query helpers
 * ================================================================ */

bool core_can_accept_program(const wash_executor_core_t *core)
{
    return core->phase == CORE_PHASE_IDLE
        && !core->program_active
        && !core->fault_active
        && !core->quiesced;
}

bool core_can_skip_current_step(const wash_executor_core_t *core)
{
    if (!core || !core->program_active || core->phase != CORE_PHASE_ACTIVE ||
        !core->token_valid) {
        return false;
    }
    const wash_step_t *step = core_get_current_step(core);
    return step && step->type == STEP_UV;
}

const wash_step_t *core_get_current_step(const wash_executor_core_t *core)
{
    if (!core->program || !core->program_active) return NULL;
    if (core->current_step_index >= core->total_steps) return NULL;
    return &core->program->steps[core->current_step_index];
}

static bool is_auto_complete_step(wash_step_type_t type)
{
    return type == STEP_WAIT_LOAD_CONFIRM
        || type == STEP_WAIT_UNLOAD_CONFIRM
        || type == STEP_FINISH;
}

static bool has_more_steps(const wash_executor_core_t *core)
{
    return core->current_step_index < core->total_steps;
}

/* ================================================================
 * Command emission
 * ================================================================ */

static void emit_cmd(core_cmd_t *out, uint32_t *written, uint32_t capacity,
                     const core_cmd_t *cmd)
{
    if (*written < capacity) {
        out[*written] = *cmd;
    }
    (*written)++;
}

/* ================================================================
 * Core init
 * ================================================================ */

void core_init(wash_executor_core_t *core)
{
    memset(core, 0, sizeof(*core));
    core->phase = CORE_PHASE_IDLE;
    core->next_request_id = 1;
}

/* ================================================================
 * Step advancement
 * ================================================================ */

static void advance_to_next_step(wash_executor_core_t *core)
{
    core->completed_steps++;
    core->current_step_index++;
    core->token_valid = false;
    memset(&core->active_token, 0, sizeof(core->active_token));
}

static uint32_t dispatch_current_step(
    wash_executor_core_t *core,
    core_cmd_t *cmds,
    uint32_t capacity);

static uint32_t handle_reset_terminal(
    wash_executor_core_t *core,
    const core_event_t *event,
    core_cmd_t *cmds,
    uint32_t capacity);

/* ================================================================
 * Event handlers
 * ================================================================ */

static uint32_t handle_program_accepted(
    wash_executor_core_t *core,
    const core_event_t *event,
    core_cmd_t *cmds,
    uint32_t capacity)
{
    uint32_t count = 0;

    if (!core_can_accept_program(core)) {
        core_cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = CORE_CMD_PUBLISH_PROGRAM_TERMINAL;
        cmd.terminal_success = false;
        cmd.terminal_error = 1;
        cmd.terminal_result = TERM_RESULT_FAULT;
        cmd.final_position_confirmed = false;  /* program rejected */
        emit_cmd(cmds, &count, capacity, &cmd);
        return count;
    }

    /* program_id passed via token.program_id */
    core->program_id = event->token.program_id;
    core->skip_in_progress = false;
    core->program_active = true;
    core->current_step_index = 0;
    core->completed_steps = 0;
    core->generation++;
    if (core->generation == 0) core->generation = 1;  /* wrap-skip-0 */
    core->terminal_pending = false;
    memset(&core->pending_terminal, 0, sizeof(core->pending_terminal));

    core->last_event = WASH_EXEC_EVENT_PROGRAM_ACCEPTED;

    if (core->total_steps == 0) {
        core->program_active = false;
        core_cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = CORE_CMD_PUBLISH_PROGRAM_TERMINAL;
        cmd.terminal_success = true;
        cmd.program_id = core->program_id;
        cmd.terminal_result = TERM_RESULT_COMPLETE;
        cmd.final_position_confirmed = true;  /* zero-step: no movement needed */
        emit_cmd(cmds, &count, capacity, &cmd);
        core->last_event = WASH_EXEC_EVENT_PROGRAM_COMPLETE;
        core->phase = CORE_PHASE_IDLE;
        return count;
    }

    /* Dispatch first step */
    count += dispatch_current_step(core, cmds + count, capacity > count ? capacity - count : 0);
    return count;
}

static uint32_t dispatch_current_step(
    wash_executor_core_t *core,
    core_cmd_t *cmds,
    uint32_t capacity)
{
    uint32_t count = 0;
    const wash_step_t *step = core_get_current_step(core);
    if (!step) return count;

    if (is_auto_complete_step(step->type)) {
        if (step->type == STEP_WAIT_LOAD_CONFIRM) {
            core->phase = CORE_PHASE_WAIT_LOAD;
            core->last_event = WASH_EXEC_EVENT_STEP_DISPATCHED;
        } else if (step->type == STEP_WAIT_UNLOAD_CONFIRM) {
            core->phase = CORE_PHASE_WAIT_UNLOAD;
            core->last_event = WASH_EXEC_EVENT_STEP_DISPATCHED;
        } else if (step->type == STEP_FINISH) {
            advance_to_next_step(core);
            core->program_active = false;
            core_cmd_t cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.type = CORE_CMD_PUBLISH_PROGRAM_TERMINAL;
            cmd.terminal_success = true;
            cmd.program_id = core->program_id;
            cmd.terminal_result = TERM_RESULT_COMPLETE;
            cmd.final_position_confirmed = true;  /* program completed */
            emit_cmd(cmds, &count, capacity, &cmd);
            core->last_event = WASH_EXEC_EVENT_PROGRAM_COMPLETE;
            core->phase = CORE_PHASE_IDLE;
        }
        return count;
    }

    /* Regular step — build token and dispatch */
    uint16_t req_id = core_next_request_id(core);
    core->active_token = core_build_token(core, req_id);
    core->token_valid = true;
    core->phase = CORE_PHASE_DISPATCHING;

    core_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CORE_CMD_DISPATCH_STEP;
    cmd.token = core->active_token;
    cmd.program_id = core->program_id;
    emit_cmd(cmds, &count, capacity, &cmd);
    core->last_event = WASH_EXEC_EVENT_STEP_DISPATCHED;

    return count;
}

static uint32_t handle_dispatch_ok(
    wash_executor_core_t *core,
    core_cmd_t *cmds,
    uint32_t capacity)
{
    if (core->phase != CORE_PHASE_DISPATCHING) return 0;
    core->phase = CORE_PHASE_ACTIVE;

    const wash_step_t *step = core_get_current_step(core);
    if (step && step->type == STEP_UV) {
        core->last_event = WASH_EXEC_EVENT_STEP_DISPATCHED;
    }
    return 0;
}

static uint32_t handle_dispatch_fail(
    wash_executor_core_t *core,
    core_cmd_t *cmds,
    uint32_t capacity)
{
    uint32_t count = 0;
    if (core->phase != CORE_PHASE_DISPATCHING) return 0;

    core->fault_active = true;
    core->fault_code = 100;
    core->program_active = false;

    core_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CORE_CMD_ENTER_FAULT;
    cmd.terminal_error = core->fault_code;
    cmd.terminal_result = TERM_RESULT_FAULT;
    emit_cmd(cmds, &count, capacity, &cmd);

    /* Also publish program terminal with fault result */
    core_cmd_t term;
    memset(&term, 0, sizeof(term));
    term.type = CORE_CMD_PUBLISH_PROGRAM_TERMINAL;
    term.terminal_success = false;
    term.program_id = core->program_id;
    term.terminal_result = TERM_RESULT_FAULT;
    term.terminal_error = core->fault_code;
    term.final_position_confirmed = false;  /* dispatch fail */
    emit_cmd(cmds, &count, capacity, &term);

    core->last_event = WASH_EXEC_EVENT_FAULT_ENTERED;
    core->phase = CORE_PHASE_FAULT;

    return count;
}

static uint32_t handle_service_terminal(
    wash_executor_core_t *core,
    const core_event_t *event,
    core_cmd_t *cmds,
    uint32_t capacity)
{
    uint32_t count = 0;

    /* In CANCELING phase, check for cancel terminal */
    if (core->phase == CORE_PHASE_CANCELING) {
        if (core->cancel_token_valid && core_token_matches_cancel(core, &event->token)) {
            core->cancel_token_valid = false;

            if (event->success && core->skip_in_progress) {
                core->skip_in_progress = false;
                core->last_event = WASH_EXEC_EVENT_STEP_SKIPPED;
                advance_to_next_step(core);
                if (has_more_steps(core)) {
                    count += dispatch_current_step(
                        core, cmds + count,
                        capacity > count ? capacity - count : 0);
                } else {
                    core->program_active = false;
                    core_cmd_t term;
                    memset(&term, 0, sizeof(term));
                    term.type = CORE_CMD_PUBLISH_PROGRAM_TERMINAL;
                    term.terminal_success = true;
                    term.program_id = core->program_id;
                    term.terminal_result = TERM_RESULT_COMPLETE;
                    term.final_position_confirmed = true;
                    emit_cmd(cmds, &count, capacity, &term);
                    core->phase = CORE_PHASE_IDLE;
                }
            } else if (event->success) {
                /* Cancel success — transition to RESETTING */
                core->phase = CORE_PHASE_RESETTING;
                uint16_t reset_req = core_next_request_id(core);
                core->reset_token = core_build_token(core, reset_req);
                core->reset_token.terminal_kind = TERMINAL_KIND_RESET_ACK;
                core->reset_token_valid = true;

                core_cmd_t cmd;
                memset(&cmd, 0, sizeof(cmd));
                cmd.type = CORE_CMD_REQUEST_FINAL_RESET;
                cmd.token = core->reset_token;
                emit_cmd(cmds, &count, capacity, &cmd);
                core->last_event = WASH_EXEC_EVENT_STEP_TERMINAL;
            } else {
                core->skip_in_progress = false;
                /* Cancel failure — FAULT, no reset, final_position_confirmed=false */
                core->fault_active = true;
                core->fault_code = event->error_code > 0 ? event->error_code : 300;
                core->program_active = false;

                core_cmd_t cmd;
                memset(&cmd, 0, sizeof(cmd));
                cmd.type = CORE_CMD_ENTER_FAULT;
                cmd.terminal_error = core->fault_code;
                cmd.terminal_result = TERM_RESULT_FAULT;
                emit_cmd(cmds, &count, capacity, &cmd);

                core_cmd_t term;
                memset(&term, 0, sizeof(term));
                term.type = CORE_CMD_PUBLISH_PROGRAM_TERMINAL;
                term.terminal_success = false;
                term.program_id = core->program_id;
                term.terminal_result = TERM_RESULT_FAULT;
                term.terminal_error = core->fault_code;
                term.final_position_confirmed = false;  /* cancel failure */
                emit_cmd(cmds, &count, capacity, &term);

                core->last_event = WASH_EXEC_EVENT_FAULT_ENTERED;
                core->phase = CORE_PHASE_FAULT;
            }
        }
        /* Wrong token in CANCELING — reject */
        return count;
    }

    /* In ACTIVE phase, check for active terminal */
    if (core->phase == CORE_PHASE_ACTIVE) {
        if (!core->token_valid) return 0;
        if (!core_token_matches_active(core, &event->token)) {
            return 0;  /* stale/wrong token */
        }

        core->token_valid = false;

        if (event->success) {
            core->last_event = WASH_EXEC_EVENT_STEP_TERMINAL;
            advance_to_next_step(core);

            if (has_more_steps(core)) {
                count += dispatch_current_step(core, cmds + count,
                                               capacity > count ? capacity - count : 0);
            } else {
                core->program_active = false;
                core_cmd_t cmd;
                memset(&cmd, 0, sizeof(cmd));
                cmd.type = CORE_CMD_PUBLISH_PROGRAM_TERMINAL;
                cmd.terminal_success = true;
                cmd.program_id = core->program_id;
                cmd.terminal_result = TERM_RESULT_COMPLETE;
                cmd.final_position_confirmed = true;  /* all steps completed */
                emit_cmd(cmds, &count, capacity, &cmd);
                core->last_event = WASH_EXEC_EVENT_PROGRAM_COMPLETE;
                core->phase = CORE_PHASE_IDLE;
            }
        } else {
            /* Service failure */
            core->fault_active = true;
            core->fault_code = event->error_code > 0 ? event->error_code : 200;
            core->program_active = false;

            core_cmd_t cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.type = CORE_CMD_ENTER_FAULT;
            cmd.terminal_error = core->fault_code;
            cmd.terminal_result = TERM_RESULT_FAULT;
            emit_cmd(cmds, &count, capacity, &cmd);

            core_cmd_t term;
            memset(&term, 0, sizeof(term));
            term.type = CORE_CMD_PUBLISH_PROGRAM_TERMINAL;
            term.terminal_success = false;
            term.program_id = core->program_id;
            term.terminal_result = TERM_RESULT_FAULT;
            term.terminal_error = core->fault_code;
            term.final_position_confirmed = false;  /* service failure */
            emit_cmd(cmds, &count, capacity, &term);

            core->last_event = WASH_EXEC_EVENT_FAULT_ENTERED;
            core->phase = CORE_PHASE_FAULT;
        }
        return count;
    }

    /* In RESETTING phase, check for reset terminal */
    if (core->phase == CORE_PHASE_RESETTING) {
        if (core->reset_token_valid && core_token_matches_reset(core, &event->token)) {
            core->reset_token_valid = false;
            return handle_reset_terminal(core, event, cmds, capacity);
        }
        return count;
    }

    /* Not in a phase that accepts service terminals */
    return 0;
}

static uint32_t handle_step_timeout(
    wash_executor_core_t *core,
    core_cmd_t *cmds,
    uint32_t capacity)
{
    uint32_t count = 0;
    if (core->phase != CORE_PHASE_ACTIVE) return 0;

    core->skip_in_progress = false;

    /* Cancel the active step */
    core->cancel_token = core->active_token;
    core->cancel_token.terminal_kind = TERMINAL_KIND_CANCEL_ACK;
    core->cancel_token_valid = true;
    core->token_valid = false;

    core_cmd_t cancel_cmd;
    memset(&cancel_cmd, 0, sizeof(cancel_cmd));
    cancel_cmd.type = CORE_CMD_CANCEL_ACTIVE_STEP;
    cancel_cmd.token = core->cancel_token;
    emit_cmd(cmds, &count, capacity, &cancel_cmd);

    core->phase = CORE_PHASE_CANCELING;
    core->last_event = WASH_EXEC_EVENT_STEP_TIMEOUT;

    return count;
}

static uint32_t handle_cancel(
    wash_executor_core_t *core,
    core_cmd_t *cmds,
    uint32_t capacity)
{
    uint32_t count = 0;
    if (core->phase != CORE_PHASE_ACTIVE && core->phase != CORE_PHASE_DISPATCHING)
        return 0;
    /* Cancel is idempotent */
    if (core->phase == CORE_PHASE_CANCELING) return 0;

    core->skip_in_progress = false;

    if (core->token_valid) {
        core->cancel_token = core->active_token;
        core->cancel_token.terminal_kind = TERMINAL_KIND_CANCEL_ACK;
        core->cancel_token_valid = true;

        core_cmd_t cancel_cmd;
        memset(&cancel_cmd, 0, sizeof(cancel_cmd));
        cancel_cmd.type = CORE_CMD_CANCEL_ACTIVE_STEP;
        cancel_cmd.token = core->cancel_token;
        emit_cmd(cmds, &count, capacity, &cancel_cmd);
    }

    core->token_valid = false;
    core->phase = CORE_PHASE_CANCELING;
    core->last_event = WASH_EXEC_EVENT_CANCELLED;

    return count;
}

static uint32_t handle_abort(
    wash_executor_core_t *core,
    core_cmd_t *cmds,
    uint32_t capacity)
{
    uint32_t count = 0;
    if (!core->program_active) return 0;
    /* Abort is idempotent — if already canceling/resetting, ignore */
    if (core->phase == CORE_PHASE_CANCELING || core->phase == CORE_PHASE_RESETTING)
        return 0;

    core->skip_in_progress = false;

    if (core->phase == CORE_PHASE_ACTIVE || core->phase == CORE_PHASE_DISPATCHING) {
        if (core->token_valid) {
            core->cancel_token = core->active_token;
            core->cancel_token.terminal_kind = TERMINAL_KIND_CANCEL_ACK;
            core->cancel_token_valid = true;

            core_cmd_t cancel_cmd;
            memset(&cancel_cmd, 0, sizeof(cancel_cmd));
            cancel_cmd.type = CORE_CMD_CANCEL_ACTIVE_STEP;
            cancel_cmd.token = core->cancel_token;
            emit_cmd(cmds, &count, capacity, &cancel_cmd);
        }
        core->token_valid = false;
    }

    /* Abort goes through CANCELING — cancel ACK will trigger RESETTING */
    core->phase = CORE_PHASE_CANCELING;
    core->last_event = WASH_EXEC_EVENT_ABORTED;

    return count;
}

static uint32_t handle_reset_terminal(
    wash_executor_core_t *core,
    const core_event_t *event,
    core_cmd_t *cmds,
    uint32_t capacity)
{
    uint32_t count = 0;

    if (event->success) {
        core->program_active = false;
        core_cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = CORE_CMD_PUBLISH_PROGRAM_TERMINAL;
        cmd.terminal_success = false;
        cmd.program_id = core->program_id;
        /* Determine result based on what triggered the reset */
        cmd.terminal_result = core->last_event == WASH_EXEC_EVENT_ABORTED
                              ? TERM_RESULT_ABORTED : TERM_RESULT_CANCELED;
        cmd.terminal_error = 0;
        cmd.final_position_confirmed = true;  /* reset success confirmed position */
        emit_cmd(cmds, &count, capacity, &cmd);
        core->last_event = WASH_EXEC_EVENT_RESET_COMPLETE;
        core->phase = CORE_PHASE_IDLE;
    } else {
        core->fault_active = true;
        core->fault_code = 400;
        core->program_active = false;

        core_cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = CORE_CMD_ENTER_FAULT;
        cmd.terminal_error = core->fault_code;
        cmd.terminal_result = TERM_RESULT_FAULT;
        emit_cmd(cmds, &count, capacity, &cmd);

        core_cmd_t term;
        memset(&term, 0, sizeof(term));
        term.type = CORE_CMD_PUBLISH_PROGRAM_TERMINAL;
        term.terminal_success = false;
        term.program_id = core->program_id;
        term.terminal_result = TERM_RESULT_FAULT;
        term.terminal_error = core->fault_code;
        term.final_position_confirmed = false;  /* reset failure */
        emit_cmd(cmds, &count, capacity, &term);

        core->last_event = WASH_EXEC_EVENT_FAULT_ENTERED;
        core->phase = CORE_PHASE_FAULT;
    }

    return count;
}

static uint32_t handle_fault_ack(
    wash_executor_core_t *core,
    core_cmd_t *cmds,
    uint32_t capacity)
{
    uint32_t count = 0;
    if (core->phase != CORE_PHASE_FAULT) return 0;

    core->fault_active = false;
    core->fault_code = 0;
    core->program_active = false;
    core->token_valid = false;
    core->cancel_token_valid = false;
    core->reset_token_valid = false;

    core_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CORE_CMD_RETURN_IDLE;
    emit_cmd(cmds, &count, capacity, &cmd);
    core->phase = CORE_PHASE_IDLE;

    return count;
}

static uint32_t handle_load_confirmed(
    wash_executor_core_t *core,
    core_cmd_t *cmds,
    uint32_t capacity)
{
    uint32_t count = 0;
    if (core->phase != CORE_PHASE_WAIT_LOAD) return 0;

    advance_to_next_step(core);
    if (has_more_steps(core)) {
        count += dispatch_current_step(core, cmds + count,
                                       capacity > count ? capacity - count : 0);
    } else {
        core->program_active = false;
        core_cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = CORE_CMD_PUBLISH_PROGRAM_TERMINAL;
        cmd.terminal_success = true;
        cmd.program_id = core->program_id;
        cmd.terminal_result = TERM_RESULT_COMPLETE;
        cmd.final_position_confirmed = true;  /* load confirmed complete */
        emit_cmd(cmds, &count, capacity, &cmd);
        core->phase = CORE_PHASE_IDLE;
    }
    return count;
}

static uint32_t handle_unload_confirmed(
    wash_executor_core_t *core,
    core_cmd_t *cmds,
    uint32_t capacity)
{
    uint32_t count = 0;
    if (core->phase != CORE_PHASE_WAIT_UNLOAD) return 0;

    advance_to_next_step(core);
    if (has_more_steps(core)) {
        count += dispatch_current_step(core, cmds + count,
                                       capacity > count ? capacity - count : 0);
    } else {
        core->program_active = false;
        core_cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = CORE_CMD_PUBLISH_PROGRAM_TERMINAL;
        cmd.terminal_success = true;
        cmd.program_id = core->program_id;
        cmd.terminal_result = TERM_RESULT_COMPLETE;
        cmd.final_position_confirmed = true;  /* unload confirmed complete */
        emit_cmd(cmds, &count, capacity, &cmd);
        core->phase = CORE_PHASE_IDLE;
    }
    return count;
}

static uint32_t handle_skip_step(
    wash_executor_core_t *core,
    core_cmd_t *cmds,
    uint32_t capacity)
{
    uint32_t count = 0;
    if (!core_can_skip_current_step(core)) return 0;

    core->cancel_token = core->active_token;
    core->cancel_token.terminal_kind = TERMINAL_KIND_CANCEL_ACK;
    core->cancel_token_valid = true;
    core->token_valid = false;
    core->skip_in_progress = true;

    core_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CORE_CMD_CANCEL_ACTIVE_STEP;
    cmd.token = core->cancel_token;
    emit_cmd(cmds, &count, capacity, &cmd);
    core->phase = CORE_PHASE_CANCELING;
    core->last_event = WASH_EXEC_EVENT_STEP_SKIPPED;
    return count;
}

/* ================================================================
 * Main event processor
 * ================================================================ */

uint32_t core_process_event(
    wash_executor_core_t *core,
    const core_event_t *event,
    core_cmd_t *out_cmds,
    uint32_t out_capacity)
{
    if (!event || event->type == CORE_EVENT_NONE) return 0;

    switch (event->type) {
    case CORE_EVENT_PROGRAM_ACCEPTED:
        return handle_program_accepted(core, event, out_cmds, out_capacity);
    case CORE_EVENT_STEP_DISPATCH_OK:
        return handle_dispatch_ok(core, out_cmds, out_capacity);
    case CORE_EVENT_STEP_DISPATCH_FAIL:
        return handle_dispatch_fail(core, out_cmds, out_capacity);
    case CORE_EVENT_SERVICE_TERMINAL:
        return handle_service_terminal(core, event, out_cmds, out_capacity);
    case CORE_EVENT_STEP_TIMEOUT:
        return handle_step_timeout(core, out_cmds, out_capacity);
    case CORE_EVENT_SKIP_STEP:
        return handle_skip_step(core, out_cmds, out_capacity);
    case CORE_EVENT_CANCEL:
        return handle_cancel(core, out_cmds, out_capacity);
    case CORE_EVENT_ABORT:
        return handle_abort(core, out_cmds, out_capacity);
    case CORE_EVENT_FAULT_ACK:
        return handle_fault_ack(core, out_cmds, out_capacity);
    case CORE_EVENT_LOAD_CONFIRMED:
        return handle_load_confirmed(core, out_cmds, out_capacity);
    case CORE_EVENT_UNLOAD_CONFIRMED:
        return handle_unload_confirmed(core, out_cmds, out_capacity);
    default:
        return 0;
    }
}
