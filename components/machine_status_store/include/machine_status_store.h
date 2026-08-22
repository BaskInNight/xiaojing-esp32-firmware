#pragma once

#include "esp_err.h"
#include "wash_contract.h"
#include "machine_revision_next.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t machine_status_store_init(void);

#ifdef XIAOJING_TESTING
esp_err_t machine_status_store_reset(void);
#endif

esp_err_t machine_status_store_update(const machine_status_t *status);

/* Executor-owned subset of machine_status_t.  Updating this structure must
 * not overwrite sensor, actuator-shadow, or connectivity fields owned by
 * other producers. */
typedef struct {
    machine_state_t state;
    wash_phase_t phase;
    uint32_t program_id;
    uint16_t current_step;
    uint16_t total_steps;
    uint8_t progress_percent;
    uint32_t elapsed_ms;
    uint32_t remaining_ms;
    drum_position_t target_position;
    machine_fault_t fault;
} machine_execution_status_t;

esp_err_t machine_status_store_update_execution(
    const machine_execution_status_t *execution);
esp_err_t machine_status_store_set_ble_connected(bool connected);
esp_err_t machine_status_store_get(machine_status_t *out_status);
uint32_t machine_status_store_get_revision(void);

#ifdef __cplusplus
}
#endif
