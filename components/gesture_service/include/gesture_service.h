#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "xiaojing_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*gesture_event_sink_t)(hal_gesture_t gesture,
                                          void *context);

typedef struct {
    const xiaojing_hal_t *hal;
    gesture_event_sink_t sink;
    void *sink_context;
    uint32_t poll_interval_ms;
    uint32_t cooldown_ms;
} gesture_service_config_t;

esp_err_t gesture_service_global_init(void);
esp_err_t gesture_service_init(const gesture_service_config_t *config);
esp_err_t gesture_service_start(void);
esp_err_t gesture_service_stop(void);

#ifdef __cplusplus
}
#endif
