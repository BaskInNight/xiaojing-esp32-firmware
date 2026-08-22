#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "xiaojing_hal.h"
#include "button_debounce.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    button_id_t         button_id;
    button_event_type_t event_type;
    uint32_t            timestamp_ms;
    uint32_t            sequence;
    uint8_t             stable_mask;
} button_sink_event_t;

typedef esp_err_t (*button_event_sink_fn)(const button_sink_event_t *event,
                                          uint32_t timeout_ms, void *context);
typedef esp_err_t (*button_urgent_sink_fn)(const button_sink_event_t *event,
                                           uint32_t timeout_ms, void *context);

typedef struct { button_event_sink_fn  publish; void *context; } button_event_sink_t;
typedef struct { button_urgent_sink_fn publish; void *context; } button_urgent_sink_t;

#define BTN_SINK_TIMEOUT_MS         50
#define BTN_POLL_FALLBACK_MS        200
#define BTN_DEBOUNCE_DEFAULT_MS     30
#define BTN_URGENT_MIN_RETRY_MS     50
#define BTN_LONG_PRESS_DEFAULT_MS   2000
#define BTN2_PTT_LONG_PRESS_MS       600

typedef struct {
    uint32_t debounce_ms;
    uint32_t poll_interval_ms;
    uint32_t long_press_ms;
    /* Optional per-button override; zero uses long_press_ms. */
    uint32_t long_press_ms_by_button[BUTTON_ID_COUNT];
    uint8_t long_press_mask;
    const xiaojing_hal_t *hal;
    bool enable_internal_pullup;
    button_event_sink_t  normal_sink;
    button_urgent_sink_t urgent_sink;
} button_service_config_t;

esp_err_t button_service_global_init(void);
esp_err_t button_service_init(const button_service_config_t *config);
esp_err_t button_service_start(void);
esp_err_t button_service_stop(void);

typedef struct {
    uint8_t   stable_mask;
    uint32_t  last_event_sequence;
    uint32_t  total_events;
    bool      task_running;
    uint32_t  normal_sink_drops;
    esp_err_t last_normal_error;
    uint32_t  urgent_delivered;
    uint32_t  urgent_coalesced;
    uint32_t  urgent_retry_count;
    bool      urgent_pending;
    uint32_t  urgent_pending_sequence;
    esp_err_t urgent_last_error;
    esp_err_t interrupt_disable_error;
    esp_err_t urgent_flush_error;
    uint32_t  i2c_read_errors;
    uint32_t  interrupt_wait_errors;
    /* R1.5: observability */
    uint32_t  join_commit_count;
    uint32_t  irq_disable_count;
    uint32_t  urgent_publish_attempts;
} button_service_snapshot_t;

esp_err_t button_service_get_snapshot(button_service_snapshot_t *out);

#ifdef XIAOJING_TESTING
typedef enum {
    BTN_HOOK_PRE_CONFIGURE = 0,
    BTN_HOOK_PRE_READ_GPIO,
    BTN_HOOK_PRE_SINK,
    BTN_HOOK_BEFORE_READY_SIGNAL,
    BTN_HOOK_URGENT_ATTEMPT_STARTED,
    BTN_HOOK_URGENT_PENDING_COMMITTED,
    BTN_HOOK_COUNT,
} button_hook_point_t;

esp_err_t button_service_test_abort_cleanup(void);
void button_service_test_set_hook(button_hook_point_t hp);
void button_service_test_clear_hooks(void);
SemaphoreHandle_t button_service_test_get_barrier(button_hook_point_t hp);
esp_err_t button_service_test_global_cleanup(void);
bool button_service_test_is_poisoned(void);
void button_service_test_set_poisoned(bool poisoned);

/* R1.7: Read-only test APIs — under s_lc_mtx only, no state_mtx dependency */
esp_err_t button_service_test_get_lifecycle(int *out_lifecycle);
uint32_t button_service_test_get_current_attempt(void);
void button_service_test_inject_completion(uint32_t attempt_id, esp_err_t result);
bool button_service_test_is_task_done(void);

/* R1.8: Post-stop counter snapshot — readable even after state_mtx destroyed */
typedef struct {
    uint32_t join_commit_count;
    uint32_t irq_disable_count;
    uint32_t urgent_publish_attempts;
} button_service_test_counters_t;

esp_err_t button_service_test_get_counters(button_service_test_counters_t *out);
#endif

#ifdef __cplusplus
}
#endif
