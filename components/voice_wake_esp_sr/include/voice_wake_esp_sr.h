#pragma once

#include "voice_frontend.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Get the ESP-SR wake backend operations.
 * Returns a pointer to the static ops structure.
 */
const voice_wake_backend_ops_t *voice_wake_esp_sr_get_ops(void);

/**
 * Initialize the ESP-SR backend.
 * Must be called before registering with voice_frontend.
 */
esp_err_t voice_wake_esp_sr_init(void);

#ifdef __cplusplus
}
#endif
