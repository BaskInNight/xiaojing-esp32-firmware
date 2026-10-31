/*
 * button_debounce.c — Pure C button debounce engine (Round 1.1)
 *
 * Per-button state machine:
 *   IDLE → WAIT_PRESS_DEBOUNCE → PRESSED → WAIT_RELEASE_DEBOUNCE → IDLE (+ CLICK)
 *   SEED_RELEASE → IDLE (no click, startup guard)
 * Tick-wrap safe: uint32_t unsigned subtraction.
 * Low-active: raw bit=0 means pressed.
 * timestamp_ms = stable press confirmed time (when debounce elapsed).
 */

#include "button_debounce.h"
#include <string.h>

void button_debounce_init(button_debounce_ctx_t *ctx, uint32_t debounce_ms)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->debounce_ms = (debounce_ms > 0) ? debounce_ms : 30;
}

void button_debounce_set_long_press(button_debounce_ctx_t *ctx,
                                    uint8_t button_mask,
                                    uint32_t long_press_ms)
{
    if (!ctx) return;
    ctx->long_press_mask = button_mask & BUTTON_RAW_MASK;
    for (int i = 0; i < BUTTON_ID_COUNT; i++)
        ctx->long_press_ms[i] = long_press_ms;
}

void button_debounce_set_long_press_for_button(
    button_debounce_ctx_t *ctx,
    button_id_t button_id,
    uint32_t long_press_ms)
{
    if (!ctx || button_id < BUTTON_ID_1 ||
        button_id >= BUTTON_ID_COUNT) return;
    ctx->long_press_ms[button_id] = long_press_ms;
}

void button_debounce_seed(button_debounce_ctx_t *ctx,
                           uint8_t raw_mask,
                           uint32_t now_ms)
{
    if (!ctx) return;
    for (int i = 0; i < BUTTON_ID_COUNT; i++) {
        button_debounce_state_t *s = &ctx->buttons[i];
        int bit = BUTTON_RAW_BIT_BTN1 + i;
        bool pressed = ((raw_mask >> bit) & 1) == 0;
        s->raw_pressed = pressed;
        s->long_press_emitted = false;
        if (pressed) {
            s->state = BTN_STATE_SEED_RELEASE;
            s->edge_timestamp = now_ms;
        } else {
            s->state = BTN_STATE_IDLE;
        }
    }
}

static bool raw_pressed(uint8_t mask, int bit)
{
    return ((mask >> bit) & 1) == 0;
}

static bool elapsed(uint32_t now, uint32_t start, uint32_t dur)
{
    return (now - start) >= dur;
}

static int process_button(button_debounce_state_t *s, uint32_t debounce_ms,
                           bool long_press_enabled, uint32_t long_press_ms,
                           bool pressed, uint32_t now_ms,
                           button_id_t id, uint32_t *global_seq,
                           button_event_t *out)
{
    switch (s->state) {

    case BTN_STATE_IDLE:
        if (pressed) {
            s->raw_pressed = true;
            s->edge_timestamp = now_ms;
            s->long_press_emitted = false;
            s->state = BTN_STATE_WAIT_PRESS_DEBOUNCE;
        }
        break;

    case BTN_STATE_WAIT_PRESS_DEBOUNCE:
        if (!pressed) {
            s->raw_pressed = false;
            s->state = BTN_STATE_IDLE;
        } else if (elapsed(now_ms, s->edge_timestamp, debounce_ms)) {
            s->raw_pressed = true;
            s->state = BTN_STATE_PRESSED;
            s->press_timestamp = now_ms;  /* confirmed time */
        }
        break;

    case BTN_STATE_PRESSED:
        if (!pressed) {
            s->raw_pressed = false;
            s->edge_timestamp = now_ms;
            s->state = BTN_STATE_WAIT_RELEASE_DEBOUNCE;
        } else if (long_press_enabled && long_press_ms > 0 &&
                   !s->long_press_emitted &&
                   elapsed(now_ms, s->press_timestamp, long_press_ms)) {
            s->long_press_emitted = true;
            s->sequence++;
            (*global_seq)++;
            out->button_id = id;
            out->event_type = BUTTON_EVENT_LONG_PRESS;
            out->timestamp_ms = now_ms;
            out->sequence = *global_seq;
            return 1;
        }
        break;

    case BTN_STATE_WAIT_RELEASE_DEBOUNCE:
        if (pressed) {
            s->raw_pressed = true;
            s->state = BTN_STATE_PRESSED;
        } else if (elapsed(now_ms, s->edge_timestamp, debounce_ms)) {
            s->raw_pressed = false;
            s->state = BTN_STATE_IDLE;
            if (s->long_press_emitted) {
                s->long_press_emitted = false;
                s->sequence++;
                (*global_seq)++;
                out->button_id = id;
                out->event_type = BUTTON_EVENT_LONG_PRESS_RELEASE;
                out->timestamp_ms = now_ms;
                out->sequence = *global_seq;
                return 1;
            }
            s->sequence++;
            (*global_seq)++;
            out->button_id = id;
            out->event_type = BUTTON_EVENT_CLICK;
            out->timestamp_ms = s->press_timestamp;
            out->sequence = *global_seq;
            return 1;
        }
        break;

    case BTN_STATE_SEED_RELEASE:
        if (pressed) {
            s->raw_pressed = true;
        } else {
            if (!s->raw_pressed) {
                if (elapsed(now_ms, s->edge_timestamp, debounce_ms)) {
                    s->state = BTN_STATE_IDLE;
                }
            } else {
                s->raw_pressed = false;
                s->edge_timestamp = now_ms;
            }
        }
        break;
    }
    return 0;
}

int button_debounce_process(button_debounce_ctx_t *ctx,
                             uint8_t raw_mask,
                             uint32_t now_ms,
                             button_event_t *events_out,
                             int max_events)
{
    if (!ctx || !events_out || max_events <= 0) return 0;
    int count = 0;
    static const struct { int id; int bit; } B[BUTTON_ID_COUNT] = {
        { BUTTON_ID_1, BUTTON_RAW_BIT_BTN1 },
        { BUTTON_ID_2, BUTTON_RAW_BIT_BTN2 },
        { BUTTON_ID_3, BUTTON_RAW_BIT_BTN3 },
    };
    for (int i = 0; i < BUTTON_ID_COUNT && count < max_events; i++) {
        bool p = raw_pressed(raw_mask, B[i].bit);
        bool long_enabled =
            (ctx->long_press_mask & (uint8_t)(1U << B[i].bit)) != 0;
        count += process_button(&ctx->buttons[B[i].id], ctx->debounce_ms,
                                 long_enabled, ctx->long_press_ms[B[i].id],
                                 p, now_ms, (button_id_t)B[i].id,
                                 &ctx->global_sequence, &events_out[count]);
    }
    return count;
}

button_deadline_t button_debounce_next_deadline(const button_debounce_ctx_t *ctx,
                                                 uint32_t now_ms)
{
    button_deadline_t result = {false, false, 0};
    if (!ctx) return result;

    for (int i = 0; i < BUTTON_ID_COUNT; i++) {
        const button_debounce_state_t *s = &ctx->buttons[i];

        /* SEED_RELEASE has a deadline for the release debounce too */
        bool long_press_wait =
            s->state == BTN_STATE_PRESSED &&
            !s->long_press_emitted &&
            ctx->long_press_ms[i] > 0 &&
            (ctx->long_press_mask &
             (uint8_t)(1U << (BUTTON_RAW_BIT_BTN1 + i))) != 0;

        if (s->state != BTN_STATE_WAIT_PRESS_DEBOUNCE &&
            s->state != BTN_STATE_WAIT_RELEASE_DEBOUNCE &&
            s->state != BTN_STATE_SEED_RELEASE &&
            !long_press_wait) {
            continue;
        }

        /* SEED_RELEASE: only has a deadline when transitioning to released
         * (raw_pressed == false means we're timing the release) */
        if (s->state == BTN_STATE_SEED_RELEASE && s->raw_pressed) {
            continue;  /* still held, no release deadline */
        }

        result.active = true;

        uint32_t start = long_press_wait ?
                         s->press_timestamp : s->edge_timestamp;
        uint32_t duration = long_press_wait ?
                            ctx->long_press_ms[i] : ctx->debounce_ms;

        if (elapsed(now_ms, start, duration)) {
            result.due_now = true;
            result.remaining_ms = 0;
            return result;  /* overdue: highest priority, return immediately */
        }

        uint32_t remaining = (start + duration) - now_ms;
        if (!result.due_now && (result.remaining_ms == 0 || remaining < result.remaining_ms)) {
            result.remaining_ms = remaining;
        }
    }

    return result;
}
