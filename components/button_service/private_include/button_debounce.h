#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * button_debounce.h — Pure C button debounce engine
 *
 * No FreeRTOS, no HAL, no I2C dependencies.
 * Low-active buttons on MCP23017 GPA4/5/6.
 * Tick-wrap safe (uint32_t unsigned subtraction).
 * ================================================================ */

typedef enum {
    BUTTON_EVENT_CLICK = 0,
    BUTTON_EVENT_LONG_PRESS,
    BUTTON_EVENT_LONG_PRESS_RELEASE,
    BUTTON_EVENT_DOUBLE_CLICK,  /* Reserved V2 */
    BUTTON_EVENT_COMBO,         /* Reserved V2 */
} button_event_type_t;

typedef enum {
    BUTTON_ID_1 = 0,
    BUTTON_ID_2,
    BUTTON_ID_3,
    BUTTON_ID_COUNT,
} button_id_t;

#define BUTTON_RAW_BIT_BTN1  4
#define BUTTON_RAW_BIT_BTN2  5
#define BUTTON_RAW_BIT_BTN3  6
#define BUTTON_RAW_MASK      (0x70U)

typedef struct {
    button_id_t         button_id;
    button_event_type_t event_type;
    uint32_t            timestamp_ms;  /* stable press confirmed time */
    uint32_t            sequence;      /* global monotonic */
} button_event_t;

typedef enum {
    BTN_STATE_IDLE = 0,
    BTN_STATE_WAIT_PRESS_DEBOUNCE,
    BTN_STATE_PRESSED,
    BTN_STATE_WAIT_RELEASE_DEBOUNCE,
    BTN_STATE_SEED_RELEASE,
} btn_state_t;

typedef struct {
    btn_state_t state;
    bool        raw_pressed;
    bool        long_press_emitted;
    uint32_t    edge_timestamp;
    uint32_t    press_timestamp;
    uint32_t    sequence;
} button_debounce_state_t;

typedef struct {
    button_debounce_state_t buttons[BUTTON_ID_COUNT];
    uint32_t debounce_ms;
    uint32_t long_press_ms[BUTTON_ID_COUNT];
    uint8_t long_press_mask;
    uint32_t global_sequence;
} button_debounce_ctx_t;

/* Deadline query result */
typedef struct {
    bool     active;        /* true = at least one button is debounce-waiting */
    bool     due_now;       /* true = earliest deadline is overdue, process immediately */
    uint32_t remaining_ms;  /* ms until deadline; 0 if due_now or !active */
} button_deadline_t;

void button_debounce_init(button_debounce_ctx_t *ctx, uint32_t debounce_ms);

/* Enable one-shot long-press events for selected raw button bits.
 * A long press suppresses the release CLICK for that button. */
void button_debounce_set_long_press(button_debounce_ctx_t *ctx,
                                    uint8_t button_mask,
                                    uint32_t long_press_ms);
void button_debounce_set_long_press_for_button(
    button_debounce_ctx_t *ctx,
    button_id_t button_id,
    uint32_t long_press_ms);

void button_debounce_seed(button_debounce_ctx_t *ctx,
                           uint8_t raw_mask,
                           uint32_t now_ms);

int button_debounce_process(button_debounce_ctx_t *ctx,
                             uint8_t raw_mask,
                             uint32_t now_ms,
                             button_event_t *events_out,
                             int max_events);

/* Query the next debounce deadline.
 * - active=false: no button waiting, use poll_fallback.
 * - active=true, due_now=true, remaining_ms=0: overdue, process immediately.
 * - active=true, due_now=false, remaining_ms>0: wait this many ms.
 * Tick-wrap safe. SEED_RELEASE release transitions also provide deadlines. */
button_deadline_t button_debounce_next_deadline(const button_debounce_ctx_t *ctx,
                                                 uint32_t now_ms);

#ifdef __cplusplus
}
#endif
