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
 * mcp23017_driver.h — MCP23017 I2C GPIO expander driver
 *
 * Safe OLAT-first power-up, shadow registers, atomic refresh,
 * GPIO/INTCAP snapshot. GPIO13 ISR only notifies — no I2C in ISR.
 * INTB: open-drain, active-low (MIRROR=1, ODR=1, INTPOL=0).
 * ================================================================ */

#define MCP23017_ADDR_DEFAULT  0x20

/* Bounded mutex timeout for all MCP operations (ms) */
#define MCP_MUTEX_TIMEOUT_MS   100

/* ---- Port identifiers ---- */

typedef enum {
    MCP_PORT_A = 0,
    MCP_PORT_B = 1,
} mcp_port_t;

#define MCP_PORT_IS_VALID(p) ((p) == MCP_PORT_A || (p) == MCP_PORT_B)

/* ---- Configuration ---- */

typedef struct {
    uint16_t i2c_addr;         /* I2C address (default 0x20) */
    uint32_t i2c_speed_hz;     /* I2C clock (board default 100kHz) */
    int intb_gpio;             /* GPIO for INTB (-1 = no interrupt) */
} mcp23017_config_t;

#define MCP23017_DEFAULT_CONFIG() { \
    .i2c_addr = MCP23017_ADDR_DEFAULT, \
    .i2c_speed_hz = 100000, \
    .intb_gpio = XIAOJING_GPIO_MCP_INTB, \
}

/* ---- Interrupt callback ---- */

typedef void (*mcp23017_int_callback_t)(uint8_t port_changed, void *user_data);

/* ---- Opaque handle ---- */

typedef struct mcp23017_ctx mcp23017_handle_t;

/* ---- Lifecycle ---- */

mcp23017_handle_t *mcp23017_init(i2c_bus_ctx_t *bus,
                                 const mcp23017_config_t *config);

void mcp23017_destroy(mcp23017_handle_t *handle);

/* ---- Output pin whitelist ---- */

/* Only these pins are allowed as outputs.
 * GPA0-3: valves/UV/drain  GPA7: PTC (gated)  GPB7: pump */
#define MCP_OUTPUT_WHITELIST_A  ((1U<<0)|(1U<<1)|(1U<<2)|(1U<<3)|(1U<<7))
#define MCP_OUTPUT_WHITELIST_B  (1U<<7)
#define MCP_PTC_PIN_BIT         (1U<<7)

/* ---- Output control ---- */

/* Set single output pin. Shadow-then-write with rollback on I2C failure.
 * Whitelist enforced. Port enum validated.
 * Emergency-latched: ON rejected, OFF always allowed.
 * PTC ON requires board_config_is_ptc_enabled(). */
esp_err_t mcp23017_set_output(mcp23017_handle_t *handle,
                              mcp_port_t port,
                              uint8_t pin,
                              bool value);

/* Force-close ALL outputs on both ports to LOW (safe state).
 * Both ports processed in single mutex hold.
 * On I2C write failure: shadow is NOT cleared (retry possible).
 * Emergency_latched is set.
 * Best-effort: continues to close port B even if port A fails.
 * Returns first error, or ESP_OK. */
esp_err_t mcp23017_close_all_outputs(mcp23017_handle_t *handle);

/* ---- Emergency latch ---- */

/* Check if emergency has been latched. */
bool mcp23017_is_emergency_latched(mcp23017_handle_t *handle);

/* Clear emergency latch (for recovery/testing). */
esp_err_t mcp23017_clear_emergency_latched(mcp23017_handle_t *handle);

/* ---- Output state snapshot (for test verification) ---- */

typedef struct {
    uint8_t shadow_a;
    uint8_t shadow_b;
    bool emergency_latched;
} mcp_output_state_t;

/* Get output state snapshot. Thread-safe. */
esp_err_t mcp23017_get_output_state(mcp23017_handle_t *handle,
                                    mcp_output_state_t *out);

/* ---- Input snapshot ---- */

esp_err_t mcp23017_get_input_snapshot(mcp23017_handle_t *handle,
                                      mcp_input_snapshot_t *out);

esp_err_t mcp23017_read_gpio(mcp23017_handle_t *handle,
                             uint8_t *gpio_a,
                             uint8_t *gpio_b);

/* ---- Interrupt handling ---- */

esp_err_t mcp23017_register_int_callback(mcp23017_handle_t *handle,
                                         mcp23017_int_callback_t callback,
                                         void *user_data);

esp_err_t mcp23017_wait_interrupt(mcp23017_handle_t *handle,
                                  uint32_t timeout_ms);

/* ---- Button interrupt configuration (Port A inputs) ---- */

/* Configure Port A button interrupts.
 * pin_mask: bit per pin (e.g. 0x70 for GPA4-6).
 * Sets GPINTENA, clears INTCONA (any-change mode), optionally enables GPPUA.
 * Reads GPIOA/INTCAPA/INTFA to establish baseline and clear pending.
 * Single mutex hold. Port B config untouched. */
esp_err_t mcp23017_configure_button_interrupts(mcp23017_handle_t *handle,
                                                uint8_t pin_mask,
                                                bool enable_pullup);

/* Disable Port A button interrupts (clears GPINTENA bits).
 * Read-modify-write. Only specified bits touched. */
esp_err_t mcp23017_disable_button_interrupts(mcp23017_handle_t *handle,
                                              uint8_t pin_mask);

/* Read interrupt flags and captured state.
 * Read order: INTFA → INTFB → INTCAPA → INTCAPB (flags before clear).
 * All four output pointers are required (non-NULL). */
esp_err_t mcp23017_read_interrupt_state(mcp23017_handle_t *handle,
                                         uint8_t *port_flags_a,
                                         uint8_t *port_flags_b,
                                         uint8_t *intcap_a,
                                         uint8_t *intcap_b);

/* ---- Shadow register access ---- */

esp_err_t mcp23017_get_shadow(mcp23017_handle_t *handle,
                              uint8_t *shadow_a,
                              uint8_t *shadow_b);

/* ---- Raw I2C read (for PAJ7620 adapter and diagnostics) ---- */

esp_err_t mcp23017_read_reg(mcp23017_handle_t *handle,
                            uint8_t reg_addr,
                            uint8_t *value);

/* ---- I2C fault injection (test-only, XIAOJING_TESTING) ---- */

#ifdef XIAOJING_TESTING
typedef esp_err_t (*mcp_i2c_fault_fn_t)(uint8_t reg, bool is_write,
                                        uint8_t *data, void *user_data);

void mcp23017_test_set_i2c_fault(mcp23017_handle_t *handle,
                                  mcp_i2c_fault_fn_t fn,
                                  void *user_data);

mcp23017_handle_t *mcp23017_test_create_mock(bool init_success);

/* Get the IOCON register value used during init (for test verification). */
uint8_t mcp23017_test_get_iocon_config(void);
#endif

#ifdef __cplusplus
}
#endif
