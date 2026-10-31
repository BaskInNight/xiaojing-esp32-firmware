/*
 * mcp23017_driver.c — MCP23017 I2C GPIO expander driver
 *
 * Safe init: OLAT=0 → IODIR → IOCON(MIRROR=1,ODR=1,INTPOL=0) → interrupts.
 * Shadow registers: update-then-write, rollback on I2C failure.
 * Emergency latch: once set, all ON rejected; OFF always allowed.
 * All mutex operations use bounded timeout (MCP_MUTEX_TIMEOUT_MS).
 * INTB: open-drain, active-low (ODR=1), GPIO13 NEGEDGE.
 */

#include <stdlib.h>
#include <string.h>
#include "mcp23017_driver.h"
#include "board_config.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "mcp23017";

/* ---- MCP23017 register addresses (BANK=0 mode) ---- */

#define MCP_REG_IODIRA     0x00
#define MCP_REG_IODIRB     0x01
#define MCP_REG_GPINTENA   0x04
#define MCP_REG_GPINTENB   0x05
#define MCP_REG_DEFVALA    0x06
#define MCP_REG_DEFVALB    0x07
#define MCP_REG_INTCONA    0x08
#define MCP_REG_INTCONB    0x09
#define MCP_REG_IOCONA     0x0A
#define MCP_REG_IOCONB     0x0B
#define MCP_REG_GPPUA      0x0C
#define MCP_REG_GPPUB      0x0D
#define MCP_REG_INTFA      0x0E
#define MCP_REG_INTFB      0x0F
#define MCP_REG_INTCAPA    0x10
#define MCP_REG_INTCAPB    0x11
#define MCP_REG_GPIOA      0x12
#define MCP_REG_GPIOB      0x13
#define MCP_REG_OLATA      0x14
#define MCP_REG_OLATB      0x15

/* ---- IOCON bits ----
 * ODR=1 (bit 2): INTB is open-drain output (requires external pull-up).
 * INTPOL=0 (bit 1): INTB active-low.
 * MIRROR=1 (bit 6): OR INTA+INTB onto INTB output.
 * Result: 0x44 */
#define IOCON_ODR          (1 << 2)
#define IOCON_MIRROR       (1 << 6)
#define IOCON_CONFIG       (IOCON_MIRROR | IOCON_ODR)   /* 0x44 */

/* Bounded timeout for xSemaphoreTake */
#define MUTEX_TICKS  pdMS_TO_TICKS(MCP_MUTEX_TIMEOUT_MS)

/* ---- Context ---- */

struct mcp23017_ctx {
    i2c_bus_ctx_t *bus;
    i2c_bus_device_t *i2c_dev;
    uint16_t i2c_addr;

    /* Shadow registers */
    uint8_t shadow_olata;
    uint8_t shadow_olatb;

    /* Emergency latch: once set, all ON commands rejected */
    bool emergency_latched;

    /* Interrupt */
    int intb_gpio;
    SemaphoreHandle_t int_sem;
    mcp23017_int_callback_t int_callback;
    void *int_user_data;

    /* Mutex for shadow + latch + snapshot */
    SemaphoreHandle_t mutex;

#ifdef XIAOJING_TESTING
    /* Mock mode: skip real I2C, use fault callback only */
    bool is_mock;
    mcp_i2c_fault_fn_t fault_fn;
    void *fault_user_data;
#endif
};

/* ---- ISR handler ---- */

static void IRAM_ATTR mcp_intb_isr(void *arg)
{
    mcp23017_handle_t *handle = (mcp23017_handle_t *)arg;
    BaseType_t higher_prio_woken = pdFALSE;
    if (handle->int_sem) {
        xSemaphoreGiveFromISR(handle->int_sem, &higher_prio_woken);
    }
    if (higher_prio_woken) {
        portYIELD_FROM_ISR();
    }
}

/* ---- Internal I2C helpers ---- */

static esp_err_t write_reg(mcp23017_handle_t *handle,
                           uint8_t reg, uint8_t value)
{
#ifdef XIAOJING_TESTING
    if (handle->fault_fn) {
        uint8_t data = value;
        esp_err_t f_err = handle->fault_fn(reg, true, &data, handle->fault_user_data);
        if (f_err != ESP_OK) return f_err;
    }
    if (handle->is_mock) return ESP_OK;  /* Mock: skip real I2C */
#endif
    uint8_t buf[2] = {reg, value};
    return i2c_bus_transmit(handle->bus, handle->i2c_dev, buf, 2);
}

static esp_err_t read_reg(mcp23017_handle_t *handle,
                          uint8_t reg, uint8_t *value)
{
#ifdef XIAOJING_TESTING
    if (handle->fault_fn) {
        /* Fault callback controls value for mocks (writes to *value) */
        esp_err_t f_err = handle->fault_fn(reg, false, value, handle->fault_user_data);
        if (f_err != ESP_OK) return f_err;
        if (handle->is_mock) return ESP_OK;  /* Callback set value, done */
    } else if (handle->is_mock) {
        *value = 0;
        return ESP_OK;
    }
#endif
    return i2c_bus_transmit_receive(handle->bus, handle->i2c_dev,
                                   &reg, 1, value, 1);
}

/* ---- Public API ---- */

mcp23017_handle_t *mcp23017_init(i2c_bus_ctx_t *bus,
                                 const mcp23017_config_t *config)
{
    if (!bus || !config) return NULL;

    mcp23017_handle_t *handle = calloc(1, sizeof(mcp23017_handle_t));
    if (!handle) return NULL;

    handle->bus = bus;
    handle->i2c_addr = config->i2c_addr;
    handle->intb_gpio = config->intb_gpio;
    handle->emergency_latched = false;

    handle->mutex = xSemaphoreCreateMutex();
    if (!handle->mutex) { free(handle); return NULL; }

    /* Add device to I2C bus */
    esp_err_t err = i2c_bus_add_device(bus, config->i2c_addr,
                                       config->i2c_speed_hz,
                                       &handle->i2c_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device: 0x%x", err);
        vSemaphoreDelete(handle->mutex);
        free(handle);
        return NULL;
    }

    /*
     * Safe power-up sequence: OLAT-first
     * Write OLAT=0 BEFORE configuring IODIR so outputs start LOW.
     */
    err = write_reg(handle, MCP_REG_OLATA, 0x00);
    if (err != ESP_OK) { ESP_LOGE(TAG, "OLATA write failed: 0x%x", err); goto fail; }
    handle->shadow_olata = 0x00;

    err = write_reg(handle, MCP_REG_OLATB, 0x00);
    if (err != ESP_OK) { ESP_LOGE(TAG, "OLATB write failed: 0x%x", err); goto fail; }
    handle->shadow_olatb = 0x00;

    /* Configure IODIR.
     * PTC/hot-air disabled: IODIRA=0xF0 (GPA7=INPUT, safe default).
     * Enabled (either PTC or coupled hot-air module): IODIRA=0x70
     * (GPA7=OUTPUT, for relay control). */
    uint8_t iodira = (board_config_is_ptc_enabled() ||
                      board_config_is_hot_air_module_enabled()) ? 0x70 : 0xF0;
    err = write_reg(handle, MCP_REG_IODIRA, iodira);
    if (err != ESP_OK) { ESP_LOGE(TAG, "IODIRA write failed: 0x%x", err); goto fail; }

    err = write_reg(handle, MCP_REG_IODIRB, 0x7F);
    if (err != ESP_OK) { ESP_LOGE(TAG, "IODIRB write failed: 0x%x", err); goto fail; }

    /* GPB0..GPB6 are active-low Hall/gesture inputs.  Enable the MCP's
     * internal pull-ups so open-collector Hall switches have a defined idle
     * HIGH level; GPB7 is an output and ignores this bit. */
    err = write_reg(handle, MCP_REG_GPPUB, 0x7F);
    if (err != ESP_OK) { ESP_LOGE(TAG, "GPPUB write failed: 0x%x", err); goto fail; }

    /* Configure IOCON: MIRROR=1 (OR INTA+INTB), ODR=1 (open-drain), INTPOL=0 */
    err = write_reg(handle, MCP_REG_IOCONA, IOCON_CONFIG);
    if (err != ESP_OK) { ESP_LOGE(TAG, "IOCON write failed: 0x%x", err); goto fail; }

    /* Configure interrupts on GPB (hall + gesture) */
    err = write_reg(handle, MCP_REG_GPINTENA, 0x00);
    if (err != ESP_OK) goto fail;
    err = write_reg(handle, MCP_REG_GPINTENB, 0x7F);
    if (err != ESP_OK) goto fail;
    err = write_reg(handle, MCP_REG_DEFVALB, 0x7F);
    if (err != ESP_OK) goto fail;
    err = write_reg(handle, MCP_REG_INTCONB, 0x7F);
    if (err != ESP_OK) goto fail;

    /* Read initial GPIO state (clears pending interrupts) */
    uint8_t dummy;
    read_reg(handle, MCP_REG_GPIOA, &dummy);
    read_reg(handle, MCP_REG_GPIOB, &dummy);
    read_reg(handle, MCP_REG_INTCAPA, &dummy);
    read_reg(handle, MCP_REG_INTCAPB, &dummy);

    /* Verify IODIR readback */
    uint8_t verify;
    err = read_reg(handle, MCP_REG_IODIRA, &verify);
    if (err != ESP_OK || verify != iodira) {
        ESP_LOGE(TAG, "IODIRA verify: read 0x%02x expected 0x%02x", verify, iodira);
        err = ESP_ERR_INVALID_RESPONSE;
        goto fail;
    }
    err = read_reg(handle, MCP_REG_IODIRB, &verify);
    if (err != ESP_OK || verify != 0x7F) {
        ESP_LOGE(TAG, "IODIRB verify: read 0x%02x expected 0x7F", verify);
        err = ESP_ERR_INVALID_RESPONSE;
        goto fail;
    }
    err = read_reg(handle, MCP_REG_GPPUB, &verify);
    if (err != ESP_OK || verify != 0x7F) {
        ESP_LOGE(TAG, "GPPUB verify: read 0x%02x expected 0x7F", verify);
        err = ESP_ERR_INVALID_RESPONSE;
        goto fail;
    }

    /* Setup GPIO13 ISR */
    if (handle->intb_gpio >= 0) {
        handle->int_sem = xSemaphoreCreateBinary();
        if (!handle->int_sem) { err = ESP_ERR_NO_MEM; goto fail; }

        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << handle->intb_gpio),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_NEGEDGE,
        };
        err = gpio_config(&io_conf);
        if (err != ESP_OK) goto fail;

        err = gpio_install_isr_service(0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) goto fail;

        err = gpio_isr_handler_add(handle->intb_gpio, mcp_intb_isr, handle);
        if (err != ESP_OK) goto fail;
    }

    ESP_LOGI(TAG, "MCP23017 initialized (addr=0x%02x, IOCON=0x%02x)",
             config->i2c_addr, IOCON_CONFIG);
    return handle;

fail:
    mcp23017_destroy(handle);
    return NULL;
}

void mcp23017_destroy(mcp23017_handle_t *handle)
{
    if (!handle) return;
    if (handle->intb_gpio >= 0) gpio_isr_handler_remove(handle->intb_gpio);
    if (handle->i2c_dev) {
        write_reg(handle, MCP_REG_OLATA, 0x00);
        write_reg(handle, MCP_REG_OLATB, 0x00);
        i2c_bus_remove_device(handle->bus, handle->i2c_dev);
    }
    if (handle->int_sem) vSemaphoreDelete(handle->int_sem);
    if (handle->mutex) vSemaphoreDelete(handle->mutex);
    free(handle);
}

esp_err_t mcp23017_set_output(mcp23017_handle_t *handle,
                              mcp_port_t port,
                              uint8_t pin,
                              bool value)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (!MCP_PORT_IS_VALID(port)) return ESP_ERR_INVALID_ARG;
    if (pin > 7) return ESP_ERR_INVALID_ARG;

    /* Output whitelist (outside mutex — no shared state) */
    uint8_t whitelist = (port == MCP_PORT_A) ?
                        MCP_OUTPUT_WHITELIST_A : MCP_OUTPUT_WHITELIST_B;
    if (!(whitelist & (1U << pin))) {
        ESP_LOGW(TAG, "Pin %u port %c not in whitelist",
                 pin, (port == MCP_PORT_A) ? 'A' : 'B');
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* ALL remaining checks and operations under mutex */
    if (xSemaphoreTake(handle->mutex, MUTEX_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    /* Emergency latch: reject ON, always allow OFF */
    if (handle->emergency_latched && value) {
        xSemaphoreGive(handle->mutex);
        ESP_LOGW(TAG, "ON rejected: emergency latched");
        return ESP_ERR_INVALID_STATE;
    }

    /* PTC/hot-air relay gate (inside mutex) */
    if (port == MCP_PORT_A && pin == 7) {
        if (!board_config_is_ptc_enabled() &&
            !board_config_is_hot_air_module_enabled() && value) {
            xSemaphoreGive(handle->mutex);
            ESP_LOGW(TAG, "relay ON blocked (PTC/hot-air disabled)");
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (value) {
            /* Switch GPA7 to output in IODIR */
            uint8_t iodir;
            esp_err_t err = read_reg(handle, MCP_REG_IODIRA, &iodir);
            if (err != ESP_OK) { xSemaphoreGive(handle->mutex); return err; }
            if (iodir & MCP_PTC_PIN_BIT) {
                iodir &= ~MCP_PTC_PIN_BIT;
                err = write_reg(handle, MCP_REG_IODIRA, iodir);
                if (err != ESP_OK) { xSemaphoreGive(handle->mutex); return err; }
            }
        }
    }

    uint8_t *shadow = (port == MCP_PORT_A) ?
                      &handle->shadow_olata : &handle->shadow_olatb;
    uint8_t reg = (port == MCP_PORT_A) ? MCP_REG_OLATA : MCP_REG_OLATB;

    uint8_t old_val = *shadow;
    uint8_t new_val = value ? (old_val | (1U << pin)) : (old_val & ~(1U << pin));

    if (new_val != old_val) {
        esp_err_t err = write_reg(handle, reg, new_val);
        if (err != ESP_OK) {
            /* Shadow NOT cleared — retry can attempt again */
            xSemaphoreGive(handle->mutex);
            return err;
        }
        *shadow = new_val;
    }

    xSemaphoreGive(handle->mutex);
    return ESP_OK;
}

esp_err_t mcp23017_close_all_outputs(mcp23017_handle_t *handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    if (xSemaphoreTake(handle->mutex, MUTEX_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t first_err = ESP_OK;

    /* Port A: write 0x00. On failure, shadow NOT cleared. */
    esp_err_t err_a = write_reg(handle, MCP_REG_OLATA, 0x00);
    if (err_a != ESP_OK) {
        first_err = err_a;
        ESP_LOGE(TAG, "close_all OLATA failed: 0x%x", err_a);
    } else {
        handle->shadow_olata = 0x00;
    }

    /* Port B: always attempt even if A failed */
    esp_err_t err_b = write_reg(handle, MCP_REG_OLATB, 0x00);
    if (err_b != ESP_OK) {
        if (first_err == ESP_OK) first_err = err_b;
        ESP_LOGE(TAG, "close_all OLATB failed: 0x%x", err_b);
    } else {
        handle->shadow_olatb = 0x00;
    }

    /* PTC/hot-air IODIR restore: switch GPA7 back to input (inside mutex) */
    if (board_config_is_ptc_enabled() ||
        board_config_is_hot_air_module_enabled()) {
        uint8_t iodir;
        esp_err_t r_err = read_reg(handle, MCP_REG_IODIRA, &iodir);
        if (r_err != ESP_OK) {
            if (first_err == ESP_OK) first_err = r_err;
            ESP_LOGE(TAG, "close_all IODIR read failed: 0x%x", r_err);
        } else if (!(iodir & MCP_PTC_PIN_BIT)) {
            iodir |= MCP_PTC_PIN_BIT;
            esp_err_t w_err = write_reg(handle, MCP_REG_IODIRA, iodir);
            if (w_err != ESP_OK) {
                if (first_err == ESP_OK) first_err = w_err;
                ESP_LOGE(TAG, "close_all IODIR write failed: 0x%x", w_err);
            }
        }
    }

    /* Latch emergency AFTER all I/O, BEFORE releasing mutex */
    handle->emergency_latched = true;

    xSemaphoreGive(handle->mutex);
    return first_err;
}

bool mcp23017_is_emergency_latched(mcp23017_handle_t *handle)
{
    if (!handle) return false;
    if (xSemaphoreTake(handle->mutex, MUTEX_TICKS) != pdTRUE) return false;
    bool latched = handle->emergency_latched;
    xSemaphoreGive(handle->mutex);
    return latched;
}

esp_err_t mcp23017_clear_emergency_latched(mcp23017_handle_t *handle)
{
    /* NOTE: Only callable from safety_manager fault-reset path.
     * Refuses to clear if any output shadow is non-zero (outputs not safe). */
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(handle->mutex, MUTEX_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (handle->shadow_olata != 0x00 || handle->shadow_olatb != 0x00) {
        xSemaphoreGive(handle->mutex);
        ESP_LOGW(TAG, "clear_latch rejected: outputs not safe (A=0x%02x B=0x%02x)",
                 handle->shadow_olata, handle->shadow_olatb);
        return ESP_ERR_INVALID_STATE;
    }
    handle->emergency_latched = false;
    xSemaphoreGive(handle->mutex);
    return ESP_OK;
}

esp_err_t mcp23017_get_output_state(mcp23017_handle_t *handle,
                                    mcp_output_state_t *out)
{
    if (!handle || !out) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(handle->mutex, MUTEX_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    out->shadow_a = handle->shadow_olata;
    out->shadow_b = handle->shadow_olatb;
    out->emergency_latched = handle->emergency_latched;
    xSemaphoreGive(handle->mutex);
    return ESP_OK;
}

esp_err_t mcp23017_get_input_snapshot(mcp23017_handle_t *handle,
                                      mcp_input_snapshot_t *out)
{
    if (!handle || !out) return ESP_ERR_INVALID_ARG;

    if (xSemaphoreTake(handle->mutex, MUTEX_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = read_reg(handle, MCP_REG_GPIOA, &out->gpio_a);
    if (err != ESP_OK) goto done;
    err = read_reg(handle, MCP_REG_GPIOB, &out->gpio_b);
    if (err != ESP_OK) goto done;
    err = read_reg(handle, MCP_REG_INTCAPA, &out->intcap_a);
    if (err != ESP_OK) goto done;
    err = read_reg(handle, MCP_REG_INTCAPB, &out->intcap_b);
    if (err != ESP_OK) goto done;

    out->timestamp_ms = (int64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;

done:
    xSemaphoreGive(handle->mutex);
    return err;
}

esp_err_t mcp23017_read_gpio(mcp23017_handle_t *handle,
                             uint8_t *gpio_a,
                             uint8_t *gpio_b)
{
    if (!handle || !gpio_a || !gpio_b) return ESP_ERR_INVALID_ARG;

    if (xSemaphoreTake(handle->mutex, MUTEX_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = read_reg(handle, MCP_REG_GPIOA, gpio_a);
    if (err == ESP_OK) {
        err = read_reg(handle, MCP_REG_GPIOB, gpio_b);
    }

    xSemaphoreGive(handle->mutex);
    return err;
}

esp_err_t mcp23017_register_int_callback(mcp23017_handle_t *handle,
                                         mcp23017_int_callback_t callback,
                                         void *user_data)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    handle->int_callback = callback;
    handle->int_user_data = user_data;
    return ESP_OK;
}

esp_err_t mcp23017_wait_interrupt(mcp23017_handle_t *handle,
                                  uint32_t timeout_ms)
{
    if (!handle || !handle->int_sem) return ESP_ERR_INVALID_STATE;

    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(handle->int_sem, ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    /* Read INTFA/INTFB (interrupt flags) BEFORE INTCAPA/INTCAPB (which clears flags).
     * This correctly determines which ports triggered the interrupt. */
    uint8_t intfa = 0, intfb = 0, intcap_a = 0, intcap_b = 0;
    if (xSemaphoreTake(handle->mutex, MUTEX_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err_a = read_reg(handle, MCP_REG_INTFA, &intfa);
    esp_err_t err_b = read_reg(handle, MCP_REG_INTFB, &intfb);
    esp_err_t err_c = read_reg(handle, MCP_REG_INTCAPA, &intcap_a);
    esp_err_t err_d = read_reg(handle, MCP_REG_INTCAPB, &intcap_b);
    xSemaphoreGive(handle->mutex);

    if (err_a != ESP_OK || err_b != ESP_OK ||
        err_c != ESP_OK || err_d != ESP_OK) {
        ESP_LOGW(TAG, "wait_interrupt: register read failed "
                 "(INTFA=0x%x INTFB=0x%x INTCAPA=0x%x INTCAPB=0x%x)",
                 err_a, err_b, err_c, err_d);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (handle->int_callback) {
        uint8_t port_changed = 0;
        if (intfa != 0) port_changed |= (1 << MCP_PORT_A);
        if (intfb != 0) port_changed |= (1 << MCP_PORT_B);
        handle->int_callback(port_changed, handle->int_user_data);
    }
    return ESP_OK;
}

esp_err_t mcp23017_get_shadow(mcp23017_handle_t *handle,
                              uint8_t *shadow_a,
                              uint8_t *shadow_b)
{
    if (!handle || !shadow_a || !shadow_b) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(handle->mutex, MUTEX_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *shadow_a = handle->shadow_olata;
    *shadow_b = handle->shadow_olatb;
    xSemaphoreGive(handle->mutex);
    return ESP_OK;
}

esp_err_t mcp23017_read_reg(mcp23017_handle_t *handle,
                            uint8_t reg_addr,
                            uint8_t *value)
{
    if (!handle || !value) return ESP_ERR_INVALID_ARG;
    return read_reg(handle, reg_addr, value);
}

esp_err_t mcp23017_configure_button_interrupts(mcp23017_handle_t *handle,
                                                uint8_t pin_mask,
                                                bool enable_pullup)
{
    /* Reject: null handle, empty mask, or mask includes non-button pins (bits 0-3,7) */
    if (!handle || pin_mask == 0 || (pin_mask & ~0x70) != 0) return ESP_ERR_INVALID_ARG;

    if (xSemaphoreTake(handle->mutex, MUTEX_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err;
    uint8_t reg_val;
    uint8_t orig_gpintena = 0, orig_intcona = 0, orig_gppua = 0;
    bool gpintena_written = false, intcona_written = false, gppua_written = false;
    bool gpintena_enabled = false;

    /* Save originals for rollback */
    err = read_reg(handle, MCP_REG_GPINTENA, &orig_gpintena);
    if (err != ESP_OK) goto done;
    err = read_reg(handle, MCP_REG_INTCONA, &orig_intcona);
    if (err != ESP_OK) goto done;
    if (enable_pullup) {
        err = read_reg(handle, MCP_REG_GPPUA, &orig_gppua);
        if (err != ESP_OK) goto done;
    }

    /* 1. Disable button interrupt bits first */
    reg_val = orig_gpintena & ~pin_mask;
    err = write_reg(handle, MCP_REG_GPINTENA, reg_val);
    if (err != ESP_OK) goto rollback;
    gpintena_written = true;

    /* 2. Clear INTCONA bits → any-change mode */
    reg_val = orig_intcona & ~pin_mask;
    err = write_reg(handle, MCP_REG_INTCONA, reg_val);
    if (err != ESP_OK) goto rollback;
    intcona_written = true;

    /* 3. Optionally enable internal pullups.
     * enable_pullup=false: do NOT modify GPPUA (preserve existing values). */
    if (enable_pullup) {
        reg_val = orig_gppua | pin_mask;
        err = write_reg(handle, MCP_REG_GPPUA, reg_val);
        if (err != ESP_OK) goto rollback;
        gppua_written = true;
    }

    /* 4. Read GPIOA to establish baseline */
    { uint8_t dummy; err = read_reg(handle, MCP_REG_GPIOA, &dummy); }
    if (err != ESP_OK) goto rollback;

    /* 5. Read INTFA + INTCAPA to clear pending */
    { uint8_t dummy; err = read_reg(handle, MCP_REG_INTFA, &dummy); }
    if (err != ESP_OK) goto rollback;
    { uint8_t dummy; err = read_reg(handle, MCP_REG_INTCAPA, &dummy); }
    if (err != ESP_OK) goto rollback;

    /* 6. Enable button interrupt bits */
    err = read_reg(handle, MCP_REG_GPINTENA, &reg_val);
    if (err != ESP_OK) goto rollback;
    reg_val |= pin_mask;
    err = write_reg(handle, MCP_REG_GPINTENA, reg_val);
    if (err != ESP_OK) goto rollback;
    gpintena_enabled = true;

done:
    xSemaphoreGive(handle->mutex);
    return err;

rollback:
    /* Best-effort rollback of already-modified registers */
    if (gpintena_enabled) {
        /* Re-disable: we had enabled, now clear those bits */
        uint8_t rv;
        if (read_reg(handle, MCP_REG_GPINTENA, &rv) == ESP_OK) {
            write_reg(handle, MCP_REG_GPINTENA, rv & ~pin_mask);
        }
    } else if (gpintena_written) {
        write_reg(handle, MCP_REG_GPINTENA, orig_gpintena);
    }
    if (intcona_written) write_reg(handle, MCP_REG_INTCONA, orig_intcona);
    if (gppua_written) write_reg(handle, MCP_REG_GPPUA, orig_gppua);
    xSemaphoreGive(handle->mutex);
    return err;
}

esp_err_t mcp23017_disable_button_interrupts(mcp23017_handle_t *handle,
                                              uint8_t pin_mask)
{
    if (!handle || pin_mask == 0 || (pin_mask & ~0x70) != 0) return ESP_ERR_INVALID_ARG;

    if (xSemaphoreTake(handle->mutex, MUTEX_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t reg_val;
    esp_err_t err = read_reg(handle, MCP_REG_GPINTENA, &reg_val);
    if (err != ESP_OK) { xSemaphoreGive(handle->mutex); return err; }

    reg_val &= ~pin_mask;
    err = write_reg(handle, MCP_REG_GPINTENA, reg_val);

    xSemaphoreGive(handle->mutex);
    return err;
}

esp_err_t mcp23017_read_interrupt_state(mcp23017_handle_t *handle,
                                         uint8_t *port_flags_a,
                                         uint8_t *port_flags_b,
                                         uint8_t *intcap_a,
                                         uint8_t *intcap_b)
{
    if (!handle || !port_flags_a || !port_flags_b ||
        !intcap_a || !intcap_b) return ESP_ERR_INVALID_ARG;

    if (xSemaphoreTake(handle->mutex, MUTEX_TICKS) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    /* Read into local vars; commit to outputs only on full success.
     * This ensures caller's pointers are never partially written. */
    uint8_t fa, fb, ca, cb;
    esp_err_t err;

    err = read_reg(handle, MCP_REG_INTFA, &fa);
    if (err != ESP_OK) goto fail;
    err = read_reg(handle, MCP_REG_INTFB, &fb);
    if (err != ESP_OK) goto fail;
    err = read_reg(handle, MCP_REG_INTCAPA, &ca);
    if (err != ESP_OK) goto fail;
    err = read_reg(handle, MCP_REG_INTCAPB, &cb);
    if (err != ESP_OK) goto fail;

    /* All reads succeeded — atomic commit */
    *port_flags_a = fa;
    *port_flags_b = fb;
    *intcap_a = ca;
    *intcap_b = cb;

    xSemaphoreGive(handle->mutex);
    return ESP_OK;

fail:
    /* On failure: caller's output pointers are NOT modified */
    xSemaphoreGive(handle->mutex);
    return err;
}

#ifdef XIAOJING_TESTING
void mcp23017_test_set_i2c_fault(mcp23017_handle_t *handle,
                                  mcp_i2c_fault_fn_t fn,
                                  void *user_data)
{
    if (!handle) return;
    handle->fault_fn = fn;
    handle->fault_user_data = user_data;
}

mcp23017_handle_t *mcp23017_test_create_mock(bool init_success)
{
    mcp23017_handle_t *handle = calloc(1, sizeof(mcp23017_handle_t));
    if (!handle) return NULL;

    handle->mutex = xSemaphoreCreateMutex();
    if (!handle->mutex) { free(handle); return NULL; }

    handle->i2c_addr = MCP23017_ADDR_DEFAULT;
    handle->intb_gpio = -1;
    handle->emergency_latched = false;
    handle->is_mock = true;

    if (init_success) {
        handle->shadow_olata = 0x00;
        handle->shadow_olatb = 0x00;
    }

    return handle;
}

uint8_t mcp23017_test_get_iocon_config(void)
{
    return IOCON_CONFIG;
}
#endif
