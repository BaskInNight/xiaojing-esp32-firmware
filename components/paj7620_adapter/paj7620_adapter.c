/*
 * paj7620_adapter.c — PAJ7620 gesture sensor interrupt adapter
 *
 * Interrupt path: PAJ7620 INT → MCP GPB6 → MCP INTB → GPIO13
 * The GPIO13 ISR (in mcp23017_driver) handles the notification.
 * This adapter reads gesture data via I2C when told GPB6 is active.
 * No I2C in ISR — all I2C work happens in task context.
 */

#include <stdlib.h>
#include "paj7620_adapter.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "paj7620";

/* PAJ7620 gesture register */
#define PAJ7620_REG_GESTURE  0x43
#define PAJ7620_REG_BANK     0xEF

#include "paj7620_init_table.inc"

struct paj7620_ctx {
    i2c_bus_ctx_t *bus;
    i2c_bus_device_t *i2c_dev;
    uint16_t i2c_addr;
    int mcp_gpb_pin;
    paj7620_gesture_callback_t callback;
    void *user_data;
};

static esp_err_t paj_write_reg(paj7620_handle_t *handle,
                               uint8_t reg, uint8_t value)
{
    uint8_t payload[2] = {reg, value};
    return i2c_bus_transmit(handle->bus, handle->i2c_dev,
                            payload, sizeof(payload));
}

static esp_err_t paj_read_reg(paj7620_handle_t *handle,
                              uint8_t reg, uint8_t *value)
{
    return i2c_bus_transmit_receive(handle->bus, handle->i2c_dev,
                                    &reg, 1, value, 1);
}

static esp_err_t paj_configure_gesture_mode(paj7620_handle_t *handle)
{
    /* The device needs at least 700 us after power-up/wake. */
    vTaskDelay(pdMS_TO_TICKS(1));
    esp_err_t err = paj_write_reg(handle, PAJ7620_REG_BANK, 0);
    if (err != ESP_OK) return err;
    err = paj_write_reg(handle, PAJ7620_REG_BANK, 0);
    if (err != ESP_OK) return err;

    uint8_t id0 = 0, id1 = 0;
    if ((err = paj_read_reg(handle, 0x00, &id0)) != ESP_OK ||
        (err = paj_read_reg(handle, 0x01, &id1)) != ESP_OK)
        return err;
    if (id0 != 0x20 || id1 != 0x76) {
        ESP_LOGE(TAG, "unexpected PAJ7620 ID: %02x %02x", id0, id1);
        return ESP_ERR_INVALID_RESPONSE;
    }

    for (size_t i = 0; i < sizeof(s_paj_init) / sizeof(s_paj_init[0]); ++i) {
        err = paj_write_reg(handle, s_paj_init[i][0], s_paj_init[i][1]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "gesture init failed at %u reg=0x%02x err=0x%x",
                     (unsigned)i, s_paj_init[i][0], err);
            return err;
        }
    }
    if ((err = paj_write_reg(handle, PAJ7620_REG_BANK, 0)) != ESP_OK)
        return err;
    uint8_t discard = 0;
    (void)paj_read_reg(handle, 0x43, &discard);
    (void)paj_read_reg(handle, 0x44, &discard);
    ESP_LOGI(TAG, "gesture mode ready (id=%02x%02x regs=%u)",
             id0, id1, (unsigned)(sizeof(s_paj_init) / sizeof(s_paj_init[0])));
    return ESP_OK;
}

paj7620_handle_t *paj7620_init(i2c_bus_ctx_t *bus,
                               const paj7620_config_t *config)
{
    if (!bus || !config) {
        return NULL;
    }

    paj7620_handle_t *handle = calloc(1, sizeof(paj7620_handle_t));
    if (!handle) {
        return NULL;
    }

    handle->bus = bus;
    handle->i2c_addr = config->i2c_addr;
    handle->mcp_gpb_pin = config->mcp_gpb_pin;

    /* Probe PAJ7620 with a bounded power-up/line-settling retry. */
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (unsigned attempt = 0; attempt < 5U; ++attempt) {
        err = i2c_bus_probe(bus, config->i2c_addr);
        if (err == ESP_OK) break;
        if (attempt < 4U) vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PAJ7620 not found at 0x%02x", config->i2c_addr);
        free(handle);
        return NULL;
    }

    err = i2c_bus_add_device(bus, config->i2c_addr,
                             config->i2c_speed_hz,
                             &handle->i2c_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add PAJ7620 device: 0x%x", err);
        free(handle);
        return NULL;
    }

    err = paj_configure_gesture_mode(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PAJ7620 gesture configuration failed: 0x%x", err);
        (void)i2c_bus_remove_device(bus, handle->i2c_dev);
        free(handle);
        return NULL;
    }

    ESP_LOGI(TAG, "PAJ7620 adapter initialized (addr=0x%02x, GPB%d)",
             config->i2c_addr, config->mcp_gpb_pin);
    return handle;
}

void paj7620_destroy(paj7620_handle_t *handle)
{
    if (!handle) {
        return;
    }
    if (handle->i2c_dev) {
        i2c_bus_remove_device(handle->bus, handle->i2c_dev);
    }
    free(handle);
}

esp_err_t paj7620_register_callback(paj7620_handle_t *handle,
                                    paj7620_gesture_callback_t callback,
                                    void *user_data)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    handle->callback = callback;
    handle->user_data = user_data;
    return ESP_OK;
}

esp_err_t paj7620_is_gesture_pending(paj7620_handle_t *handle,
                                     bool *pending)
{
    if (!handle || !pending) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Read GPB6 state via MCP GPIOB register */
    /* This requires the MCP handle, which we don't have directly.
     * The caller (real_hal or task) should read MCP GPIOB and check bit 6.
     * For now, we provide a direct I2C read to the PAJ7620 itself. */

    /* Read PAJ7620 gesture register — non-zero means gesture pending */
    uint8_t gesture = 0;
    esp_err_t err = paj_read_reg(handle, PAJ7620_REG_GESTURE, &gesture);
    if (err != ESP_OK) {
        *pending = false;
        return err;
    }
    *pending = (gesture != 0);
    return ESP_OK;
}

esp_err_t paj7620_read_gesture(paj7620_handle_t *handle,
                               paj_gesture_event_t *out)
{
    if (!handle || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    out->gesture = PAJ_GESTURE_NONE;
    out->timestamp_ms = 0;
    out->valid = false;

    uint8_t gesture_byte = 0;
    esp_err_t err = paj_read_reg(handle, PAJ7620_REG_GESTURE, &gesture_byte);
    if (err != ESP_OK) {
        return err;
    }

    /* PAJ7620 exposes the primary gesture flags at 0x43 and the secondary
     * wave/flag bank at 0x44.  Older modules report only 0x43, while some
     * revisions place wave/extended flags in 0x44.  Poll both registers so
     * page navigation does not depend on the module revision. */
    uint8_t extended_byte = 0;
    err = paj_read_reg(handle, PAJ7620_REG_GESTURE + 1U, &extended_byte);
    if (err != ESP_OK) {
        return err;
    }
    gesture_byte |= extended_byte;

    if (gesture_byte != 0) {
        /* A register read can contain more than one flag. Use the same
         * deterministic priority as the previously validated firmware. */
        if (gesture_byte & PAJ_GESTURE_UP)
            out->gesture = PAJ_GESTURE_UP;
        else if (gesture_byte & PAJ_GESTURE_DOWN)
            out->gesture = PAJ_GESTURE_DOWN;
        else if (gesture_byte & PAJ_GESTURE_LEFT)
            out->gesture = PAJ_GESTURE_LEFT;
        else if (gesture_byte & PAJ_GESTURE_RIGHT)
            out->gesture = PAJ_GESTURE_RIGHT;
        else if (gesture_byte & PAJ_GESTURE_CLOCKWISE)
            out->gesture = PAJ_GESTURE_CLOCKWISE;
        else if (gesture_byte & PAJ_GESTURE_COUNTER_CW)
            out->gesture = PAJ_GESTURE_COUNTER_CW;
        else if (gesture_byte & PAJ_GESTURE_FORWARD)
            out->gesture = PAJ_GESTURE_FORWARD;
        else if (gesture_byte & PAJ_GESTURE_BACKWARD)
            out->gesture = PAJ_GESTURE_BACKWARD;
        out->timestamp_ms = (int64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
        out->valid = true;

        if (handle->callback) {
            handle->callback(out, handle->user_data);
        }
    }

    return ESP_OK;
}
