#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "i2c_bus_manager.h"
#include "xiaojing_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * sht_sensor.h — SHT30 temperature/humidity sensor driver
 * CRC-8 validation, freshness tracking, bounded I2C retries.
 * ================================================================ */

#define SHT30_ADDR_PRIMARY    0x44
#define SHT30_ADDR_SECONDARY  0x45

/* Default stale timeout: 5 seconds */
#define SHT30_DEFAULT_STALE_MS  5000

typedef struct {
    uint16_t i2c_addr;           /* 0x44 or 0x45, 0 = auto-probe */
    uint32_t i2c_speed_hz;       /* default 100kHz */
    uint32_t stale_timeout_ms;   /* sample considered stale after this */
} sht_sensor_config_t;

#define SHT30_DEFAULT_CONFIG() { \
    .i2c_addr = 0, \
    .i2c_speed_hz = 100000, \
    .stale_timeout_ms = SHT30_DEFAULT_STALE_MS, \
}

typedef struct sht_ctx sht_handle_t;

/* Probe bus for SHT30 and create handle.
 * If config->i2c_addr is 0, probes 0x44 then 0x45.
 * Returns NULL if no SHT30 found or init fails. */
sht_handle_t *sht_sensor_init(i2c_bus_ctx_t *bus,
                               const sht_sensor_config_t *config);

/* Destroy handle. NULL-safe. */
void sht_sensor_destroy(sht_handle_t *handle);

/* Read temperature and humidity with CRC validation.
 * Sets out->valid=false if CRC fails, data is stale, or I2C error.
 * Always returns ESP_OK unless handle/out is NULL. */
esp_err_t sht_sensor_read(sht_handle_t *handle, sht_sample_t *out);

/* Get the detected I2C address (0 if not initialized). */
uint16_t sht_sensor_get_addr(sht_handle_t *handle);

/* Get error counter (CRC failures + I2C errors). */
uint32_t sht_sensor_get_error_count(sht_handle_t *handle);

#ifdef __cplusplus
}
#endif
