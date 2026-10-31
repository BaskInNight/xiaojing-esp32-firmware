#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * i2c_bus_manager.h — I2C master bus lifecycle and retry wrapper
 * Uses ESP-IDF 5.5.4 new i2c_master API.
 *
 * Bus owns all devices via linked list. remove_device marks removed
 * but does not free — destroy walks the list and frees everything.
 * Mock backend: explicit creation via i2c_bus_manager_init_mock().
 *
 * THREAD SAFETY: All bus/device operations are single-threaded.
 * The caller must not share a bus instance across FreeRTOS tasks
 * without external synchronization.
 * ================================================================ */

#define I2C_BUS_DEFAULT_MAX_RETRIES  3
#define I2C_BUS_DEFAULT_RETRY_MS     10
#define I2C_BUS_DEFAULT_TIMEOUT_MS   100

typedef struct i2c_bus_ctx i2c_bus_ctx_t;
typedef struct i2c_bus_device i2c_bus_device_t;

typedef struct {
    int sda_pin;
    int scl_pin;
    uint32_t clk_speed_hz;
    uint8_t max_retries;
    uint32_t retry_delay_ms;
    uint32_t xfer_timeout_ms;
    bool internal_pullup;
} i2c_bus_config_t;

/* Create real I2C master bus. ALWAYS creates real hardware. */
esp_err_t i2c_bus_manager_init(const i2c_bus_config_t *config,
                               i2c_bus_ctx_t **out_ctx);

/* Destroy bus: best-effort removes all devices, frees everything.
 * After this call the pointer is invalid. NULL-safe. */
void i2c_bus_manager_destroy(i2c_bus_ctx_t *ctx);

/* Add device to bus. On success the bus owns the device. */
esp_err_t i2c_bus_add_device(i2c_bus_ctx_t *ctx,
                             uint16_t dev_addr,
                             uint32_t scl_speed_hz,
                             i2c_bus_device_t **out_device);

/* Remove device from bus (marks removed, releases real handle).
 * The device wrapper is NOT freed — it stays in the bus list
 * and is freed by i2c_bus_manager_destroy().
 * Returns ESP_ERR_INVALID_STATE if already removed. */
esp_err_t i2c_bus_remove_device(i2c_bus_ctx_t *ctx,
                                i2c_bus_device_t *device);

/* Probe: returns ESP_OK if device ACKs, ESP_ERR_NOT_FOUND otherwise */
esp_err_t i2c_bus_probe(i2c_bus_ctx_t *ctx, uint16_t addr);

esp_err_t i2c_bus_transmit(i2c_bus_ctx_t *ctx,
                           i2c_bus_device_t *device,
                           const uint8_t *data, size_t len);

esp_err_t i2c_bus_receive(i2c_bus_ctx_t *ctx,
                          i2c_bus_device_t *device,
                          uint8_t *data, size_t len);

esp_err_t i2c_bus_transmit_receive(i2c_bus_ctx_t *ctx,
                                   i2c_bus_device_t *device,
                                   const uint8_t *tx_data, size_t tx_len,
                                   uint8_t *rx_data, size_t rx_len);

/* ---- Mock backend (test-only, XIAOJING_TESTING) ---- */

#ifdef XIAOJING_TESTING

/* I2C operation type for mock callback */
typedef enum {
    I2C_OP_PROBE = 0,
    I2C_OP_TRANSMIT,
    I2C_OP_RECEIVE,
    I2C_OP_TRANSMIT_RECEIVE,
} i2c_mock_op_t;

/* Address-aware mock callback.
 * op:           operation type
 * device_addr:  7-bit I2C address of the target device
 * tx/tx_len:    transmit buffer (NULL for receive/probe)
 * rx/rx_len:    receive buffer (NULL for transmit/probe)
 * user_data:    from i2c_bus_mock_backend_t
 *
 * For I2C_OP_PROBE: return ESP_OK if present, ESP_ERR_NOT_FOUND otherwise.
 * For I2C_OP_RECEIVE / I2C_OP_TRANSMIT_RECEIVE: fill rx buffer.
 * Returns ESP_OK for success, or error code for simulated failure. */
typedef esp_err_t (*i2c_bus_mock_xfer_fn)(i2c_mock_op_t op,
                                          uint16_t device_addr,
                                          const uint8_t *tx, size_t tx_len,
                                          uint8_t *rx, size_t rx_len,
                                          void *user_data);

typedef struct {
    i2c_bus_mock_xfer_fn xfer_fn;
    void *user_data;
} i2c_bus_mock_backend_t;

/* Create mock I2C bus. Never touches real GPIO/I2C hardware. */
esp_err_t i2c_bus_manager_init_mock(const i2c_bus_mock_backend_t *backend,
                                    i2c_bus_ctx_t **out_ctx);

/* Test-only: get allocated/released device counts for leak detection. */
void i2c_bus_get_device_counts(i2c_bus_ctx_t *ctx,
                               int *allocated, int *released);

/* Test-only: get global totals (survives bus destroy). */
void i2c_bus_get_total_counts(int *total_allocated, int *total_freed);

/* Test-only: reset global counters (call before each test). */
void i2c_bus_reset_total_counts(void);

#endif /* XIAOJING_TESTING */

#ifdef __cplusplus
}
#endif
