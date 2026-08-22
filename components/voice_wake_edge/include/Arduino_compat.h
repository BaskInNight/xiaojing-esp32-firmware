/* Arduino compatibility stubs for Edge Impulse SDK on ESP-IDF */
#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/* millis() - returns milliseconds since boot */
static inline unsigned long millis(void) {
    return (unsigned long)(esp_timer_get_time() / 1000);
}

/* micros() - returns microseconds since boot */
static inline unsigned long micros(void) {
    return (unsigned long)esp_timer_get_time();
}

/* delay() */
static inline void delay(unsigned long ms) {
    vTaskDelay(ms / portTICK_PERIOD_MS);
}

/* yield() */
static inline void yield(void) {
    taskYIELD();
}

#ifdef __cplusplus
}
#endif

/* Note: min/max are NOT defined here to avoid conflicts with C++ std::min/max.
 * The Edge Impulse SDK handles this internally with its own ei_min/ei_max. */
