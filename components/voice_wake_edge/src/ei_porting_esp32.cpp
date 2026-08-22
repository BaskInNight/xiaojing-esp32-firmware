/* Custom Edge Impulse porting for ESP-IDF
 * Allocates from PSRAM when possible to preserve internal RAM for BLE/WiFi. */

#include "ei_classifier_porting.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *EI_TAG = "ei_porting";

/* All Edge Impulse allocations go to PSRAM to preserve internal RAM for BLE/WiFi */
#define EI_PSRAM_THRESHOLD  0

void *ei_malloc(size_t size)
{
    void *p = heap_caps_aligned_alloc(16, size, MALLOC_CAP_SPIRAM);
    if (p) {
        if (!esp_ptr_external_ram(p)) {
            ESP_LOGE(EI_TAG, "ei_malloc(%u) returned non-PSRAM ptr=%p",
                     (unsigned)size, p);
        }
        return p;
    }
    ESP_LOGW(EI_TAG, "ei_malloc(%u) PSRAM failed, falling back to internal",
             (unsigned)size);
    p = heap_caps_aligned_alloc(16, size, MALLOC_CAP_DEFAULT);
    if (p && esp_ptr_external_ram(p)) {
        ESP_LOGW(EI_TAG, "ei_malloc(%u) fallback returned PSRAM ptr=%p",
                 (unsigned)size, p);
    }
    return p;
}

void *ei_calloc(size_t nitems, size_t size)
{
    size_t total = nitems * size;
    void *p = heap_caps_aligned_calloc(16, nitems, size, MALLOC_CAP_SPIRAM);
    if (p) {
        if (!esp_ptr_external_ram(p)) {
            ESP_LOGE(EI_TAG, "ei_calloc(%u) returned non-PSRAM ptr=%p",
                     (unsigned)total, p);
        }
        return p;
    }
    ESP_LOGW(EI_TAG, "ei_calloc(%u) PSRAM failed, falling back to internal",
             (unsigned)total);
    p = heap_caps_aligned_calloc(16, nitems, size, MALLOC_CAP_DEFAULT);
    if (p && esp_ptr_external_ram(p)) {
        ESP_LOGW(EI_TAG, "ei_calloc(%u) fallback returned PSRAM ptr=%p",
                 (unsigned)total, p);
    }
    return p;
}

void *ei_realloc(void *ptr, size_t size)
{
    /* heap_caps_realloc doesn't have aligned variant; use standard realloc */
    return realloc(ptr, size);
}

void ei_free(void *ptr)
{
    heap_caps_free(ptr);
}

EI_IMPULSE_ERROR ei_sleep(int32_t time_ms)
{
    vTaskDelay(time_ms / portTICK_PERIOD_MS);
    return EI_IMPULSE_OK;
}

uint64_t ei_read_timer_ms()
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

uint64_t ei_read_timer_us()
{
    return (uint64_t)esp_timer_get_time();
}

/* ei_printf - debug output for Edge Impulse SDK */
void ei_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}

void ei_printf_float(float f)
{
    printf("%f", f);
}

EI_IMPULSE_ERROR ei_run_impulse_check_canceled()
{
    return EI_IMPULSE_OK;
}
