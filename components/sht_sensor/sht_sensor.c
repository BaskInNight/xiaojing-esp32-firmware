/*
 * sht_sensor.c — SHT30 temperature/humidity sensor driver
 * Single-shot measurement with CRC-8 validation.
 * Freshness tracking with configurable stale timeout.
 */

#include <stdlib.h>
#include <string.h>
#include "sht_sensor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "sht30";

/* SHT30 commands.  Use the high-repeatability, clock-stretching-disabled
 * single-shot command.  The ESP-IDF master driver exposes separate
 * transmit/receive calls, so this form lets the sensor finish while the bus
 * is idle and then performs a clean read transaction. */
#define SHT30_CMD_MEASURE_H   0x24
#define SHT30_CMD_MEASURE_L   0x00  /* High repeatability, no clock stretch */
#define SHT30_CMD_RESET_H     0x30
#define SHT30_CMD_RESET_L     0xA2  /* Soft reset */
#define SHT30_MIN_READ_INTERVAL_MS 500U

/* CRC-8: polynomial 0x31, init 0xFF */
static uint8_t sht_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 0x80) {
                crc = (uint8_t)((crc << 1) ^ 0x31);
            } else {
                crc = (uint8_t)(crc << 1);
            }
        }
    }
    return crc;
}

struct sht_ctx {
    i2c_bus_ctx_t *bus;
    i2c_bus_device_t *i2c_dev;
    uint16_t i2c_addr;
    uint32_t stale_timeout_ms;
    bool has_last_sample;       /* true after first successful read */
    int64_t last_success_ms;
    int64_t last_attempt_ms;
    uint32_t error_count;
    sht_sample_t last_sample;
    SemaphoreHandle_t mutex;
};

sht_handle_t *sht_sensor_init(i2c_bus_ctx_t *bus,
                               const sht_sensor_config_t *config)
{
    if (!bus || !config) {
        return NULL;
    }

    sht_handle_t *handle = calloc(1, sizeof(sht_handle_t));
    if (!handle) {
        return NULL;
    }

    handle->bus = bus;
    handle->stale_timeout_ms = config->stale_timeout_ms;
    handle->last_sample.valid = false;
    handle->mutex = xSemaphoreCreateMutex();
    if (!handle->mutex) {
        free(handle);
        return NULL;
    }

    /* Probe for SHT30 */
    uint16_t addr = config->i2c_addr;
    if (addr == 0) {
        /* Auto-probe: try 0x44 first, then 0x45 */
        if (i2c_bus_probe(bus, SHT30_ADDR_PRIMARY) == ESP_OK) {
            addr = SHT30_ADDR_PRIMARY;
            ESP_LOGI(TAG, "SHT30 found at 0x%02x", addr);
        } else if (i2c_bus_probe(bus, SHT30_ADDR_SECONDARY) == ESP_OK) {
            addr = SHT30_ADDR_SECONDARY;
            ESP_LOGI(TAG, "SHT30 found at 0x%02x (secondary)", addr);
        } else {
            ESP_LOGW(TAG, "SHT30 not found on I2C bus");
            vSemaphoreDelete(handle->mutex);
            free(handle);
            return NULL;
        }
    }

    handle->i2c_addr = addr;

    esp_err_t err = i2c_bus_add_device(bus, addr, config->i2c_speed_hz,
                                       &handle->i2c_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SHT30 device: 0x%x", err);
        vSemaphoreDelete(handle->mutex);
        free(handle);
        return NULL;
    }

    /* Clear any partially received command left by a previous boot or bus
     * interruption before the first measurement.  The command is harmless on
     * SHT30-compatible modules and prevents a stale command parser from
     * returning an all-ones humidity word. */
    uint8_t reset_cmd[2] = {SHT30_CMD_RESET_H, SHT30_CMD_RESET_L};
    if (i2c_bus_transmit(bus, handle->i2c_dev, reset_cmd,
                         sizeof(reset_cmd)) == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(2));
    } else {
        ESP_LOGW(TAG, "SHT30 soft reset command failed");
    }

    ESP_LOGI(TAG, "SHT30 initialized (addr=0x%02x, stale=%"PRIu32"ms)",
             addr, handle->stale_timeout_ms);
    return handle;
}

void sht_sensor_destroy(sht_handle_t *handle)
{
    if (!handle) {
        return;
    }
    if (handle->i2c_dev) {
        i2c_bus_remove_device(handle->bus, handle->i2c_dev);
    }
    if (handle->mutex) vSemaphoreDelete(handle->mutex);
    free(handle);
}

esp_err_t sht_sensor_read(sht_handle_t *handle, sht_sample_t *out)
{
    if (!handle || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(handle->mutex, pdMS_TO_TICKS(250)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    int64_t now_ms = (int64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;

    /* The dry-service tick is 50ms, but an SHT30 conversion plus the shared
     * I2C bus is much slower.  Rate-limit physical conversions and serve the
     * cached sample between attempts so a noisy/disconnected sensor cannot
     * monopolize I2C and starve the display/gesture peripherals. */
    if (handle->last_attempt_ms != 0 &&
        (uint32_t)(now_ms - handle->last_attempt_ms) <
            SHT30_MIN_READ_INTERVAL_MS) {
        if (handle->has_last_sample) {
            *out = handle->last_sample;
            out->age_ms = (uint32_t)(now_ms - handle->last_success_ms);
            out->timestamp_ms = now_ms;
        } else {
            out->valid = false;
            out->age_ms = UINT32_MAX;
        }
        xSemaphoreGive(handle->mutex);
        return ESP_OK;
    }
    handle->last_attempt_ms = now_ms;

    /* Try a fresh I2C measurement.  The explicit STOP after the command is
     * required for the no-clock-stretching single-shot mode. */
    uint8_t cmd[2] = {SHT30_CMD_MEASURE_H, SHT30_CMD_MEASURE_L};
    esp_err_t err = i2c_bus_transmit(handle->bus, handle->i2c_dev, cmd,
                                     sizeof(cmd));
    if (err != ESP_OK) {
        handle->error_count++;
        goto try_last_good;
    }

    /* Conversion completes in <=15 ms at high repeatability. */
    vTaskDelay(pdMS_TO_TICKS(20));

    /* Read 6 bytes: temp_msb, temp_lsb, temp_crc, hum_msb, hum_lsb, hum_crc. */
    uint8_t data[6];
    err = i2c_bus_receive(handle->bus, handle->i2c_dev, data, sizeof(data));
    if (err != ESP_OK) {
        handle->error_count++;
        goto try_last_good;
    }

    /* CRC validation */
    if (sht_crc8(data, 2) != data[2]) {
        if (handle->error_count == 0U ||
            (handle->error_count % 200U) == 0U) {
            ESP_LOGW(TAG,
                     "Temperature CRC failed (errors=%" PRIu32
                     ", raw=%02x %02x %02x %02x %02x %02x)",
                     handle->error_count + 1U, data[0], data[1], data[2],
                     data[3], data[4], data[5]);
        }
        handle->error_count++;
        goto try_last_good;
    }
    if (sht_crc8(data + 3, 2) != data[5]) {
        if (handle->error_count == 0U ||
            (handle->error_count % 200U) == 0U) {
            ESP_LOGW(TAG,
                     "Humidity CRC failed (errors=%" PRIu32
                     ", raw=%02x %02x %02x %02x %02x %02x)",
                     handle->error_count + 1U, data[0], data[1], data[2],
                     data[3], data[4], data[5]);
        }
        handle->error_count++;
        goto try_last_good;
    }

    /* Convert raw values */
    uint16_t raw_temp = ((uint16_t)data[0] << 8) | data[1];
    uint16_t raw_hum  = ((uint16_t)data[3] << 8) | data[4];

    float temp_c = -45.0f + 175.0f * (float)raw_temp / 65535.0f;
    float hum_rh = 100.0f * (float)raw_hum / 65535.0f;

    if (hum_rh < 0.0f) hum_rh = 0.0f;
    if (hum_rh > 100.0f) hum_rh = 100.0f;

    /* Fresh read succeeded */
    out->temperature_c = temp_c;
    out->humidity_rh = hum_rh;
    out->timestamp_ms = now_ms;
    out->age_ms = 0;
    out->valid = true;

    handle->last_success_ms = now_ms;
    handle->last_sample = *out;
    handle->has_last_sample = true;
    xSemaphoreGive(handle->mutex);
    return ESP_OK;

try_last_good:
    /* I2C error or CRC failure: try returning last good sample if fresh */
    if (handle->has_last_sample) {
        uint32_t age = (uint32_t)(now_ms - handle->last_success_ms);
        if (age <= handle->stale_timeout_ms) {
            *out = handle->last_sample;
            out->age_ms = age;
            out->timestamp_ms = now_ms;
            xSemaphoreGive(handle->mutex);
            return ESP_OK;
        }
    }
    /* No good sample available or stale */
    out->valid = false;
    out->age_ms = handle->has_last_sample ?
                  (uint32_t)(now_ms - handle->last_success_ms) : UINT32_MAX;
    xSemaphoreGive(handle->mutex);
    return ESP_OK;
}

uint16_t sht_sensor_get_addr(sht_handle_t *handle)
{
    return handle ? handle->i2c_addr : 0;
}

uint32_t sht_sensor_get_error_count(sht_handle_t *handle)
{
    return handle ? handle->error_count : 0;
}
