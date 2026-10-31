#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DISPLAY_VOICE_IDLE = 0,
    DISPLAY_VOICE_LISTENING,
    DISPLAY_VOICE_THINKING,
    DISPLAY_VOICE_REPLY,
    DISPLAY_VOICE_ERROR,
} display_voice_state_t;

typedef enum {
    DISPLAY_PAGE_STATUS = 0,
    DISPLAY_PAGE_WASH,
    DISPLAY_PAGE_PERIPHERALS,
    DISPLAY_PAGE_VOICE,
    DISPLAY_PAGE_COUNT,
} display_page_t;

typedef enum {
    DISPLAY_WAKE_EDGE_IMPULSE = 0,
    DISPLAY_WAKE_ESP_SR,
} display_wake_backend_t;

/* Service-independent, copy-only telemetry contract.  Tri-state outputs use
 * 0=unknown, 1=off, 2=on so the HMI never presents an unknown actuator as
 * safely de-energized. */
typedef struct {
    uint8_t machine_state;
    uint8_t wash_phase;
    int16_t position_degrees;
    int16_t target_position_degrees;
    uint16_t current_step;
    uint16_t total_steps;
    uint32_t elapsed_ms;
    uint32_t remaining_ms;
    uint32_t water_ml;
    uint32_t flow_pulses;
    float temperature_c;
    float humidity_rh;
    bool environment_valid;
    bool water_full;
    bool water_level_valid;
    bool safety_fault;
    bool emergency_stop;
    uint16_t fault_code;
    uint8_t position_pwm;
    uint8_t bl50_pwm;
    uint8_t fan_percent;
    uint8_t source_valve_state;
    uint8_t transfer_valve_state;
    uint8_t drain_state;
    uint8_t detergent_state;
    uint8_t uv_state;
    uint8_t heater_state;
    display_wake_backend_t wake_backend;
    bool backend_switching;
    uint32_t internal_free_bytes;
} display_telemetry_t;

typedef struct {
    display_voice_state_t voice_state;
    bool wifi_connected;
    bool ble_connected;
    bool washing;
    uint8_t wash_progress;
    char wash_step[48];
    display_telemetry_t telemetry;
    /* Match the cloud route's UTF-8 transcript capacity. */
    char transcript[1025];
    char reply[512];
    char error[96];
    uint32_t revision;
} display_model_t;

esp_err_t display_service_init(void);
/* Reserve LCD/LVGL transport resources without constructing UI objects. */
esp_err_t display_service_prepare(void);
esp_err_t display_service_start(void);
esp_err_t display_service_stop(void);
esp_err_t display_service_set_voice_state(display_voice_state_t state);
esp_err_t display_service_set_voice_text(const char *transcript,
                                         const char *reply);
esp_err_t display_service_set_error(const char *message);
esp_err_t display_service_set_connectivity(bool wifi_connected,
                                           bool ble_connected);
/* Read only the two connectivity fields without copying the full model. */
bool display_service_connectivity_matches(bool wifi_connected,
                                          bool ble_connected);
esp_err_t display_service_set_wash(bool washing, uint8_t progress,
                                   const char *step);
esp_err_t display_service_set_telemetry(const display_telemetry_t *telemetry);
/* Thread-safe page requests; LVGL objects are changed by the display task. */
esp_err_t display_service_show_page(display_page_t page);
esp_err_t display_service_step_page(int8_t direction);
esp_err_t display_service_get_model(display_model_t *out);

#ifdef XIAOJING_TESTING
void display_service_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif
