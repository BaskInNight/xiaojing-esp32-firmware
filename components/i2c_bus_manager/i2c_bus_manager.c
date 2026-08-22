/*
 * i2c_bus_manager.c — I2C master bus lifecycle and bounded retry wrapper
 * Uses ESP-IDF 5.5.4 new i2c_master API.
 *
 * Round 6.1.1: Bus-owned device linked list, address-aware mock,
 * no legacy fault_fn, alloc/free counters for leak detection.
 */

#include <stdlib.h>
#include <string.h>
#include "i2c_bus_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef XIAOJING_TESTING_MOCK_ONLY
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#endif

static const char *TAG = "i2c_bus";

typedef enum {
    BUS_BACKEND_REAL = 0,
    BUS_BACKEND_MOCK,
} bus_backend_type_t;

struct i2c_bus_ctx {
    bus_backend_type_t backend_type;
    uint8_t max_retries;
    uint32_t retry_delay_ms;
    uint32_t xfer_timeout_ms;
    i2c_bus_device_t *device_head;

#ifndef XIAOJING_TESTING_MOCK_ONLY
    i2c_master_bus_handle_t bus_handle;
#endif

#ifdef XIAOJING_TESTING
    i2c_bus_mock_xfer_fn mock_xfer_fn;
    void *mock_user_data;
    int devices_allocated;
    int devices_released;
#endif
};

/* Global counters for post-destroy leak verification (test-only) */
#ifdef XIAOJING_TESTING
static int s_total_allocated = 0;
static int s_total_freed = 0;
#endif

struct i2c_bus_device {
    i2c_bus_ctx_t *owner;
    i2c_bus_device_t *next;
    uint16_t address;
    bool removed;

#ifndef XIAOJING_TESTING_MOCK_ONLY
    i2c_master_dev_handle_t real_handle;
#endif
};

#ifndef XIAOJING_TESTING_MOCK_ONLY

static esp_err_t real_transmit_with_retry(i2c_bus_ctx_t *ctx,
                                          i2c_bus_device_t *dev,
                                          const uint8_t *data,
                                          size_t len)
{
    esp_err_t err = ESP_FAIL;
    for (uint8_t attempt = 0; attempt <= ctx->max_retries; attempt++) {
        err = i2c_master_transmit(dev->real_handle, data, len,
                                  ctx->xfer_timeout_ms);
        if (err == ESP_OK) return ESP_OK;
        if (attempt < ctx->max_retries) {
            ESP_LOGD(TAG, "transmit retry %u/%u, err=0x%x",
                     attempt + 1, ctx->max_retries, err);
            vTaskDelay(pdMS_TO_TICKS(ctx->retry_delay_ms));
        }
    }
    ESP_LOGW(TAG, "transmit failed after %u retries, err=0x%x",
             ctx->max_retries, err);
    return err;
}

static esp_err_t real_receive_with_retry(i2c_bus_ctx_t *ctx,
                                         i2c_bus_device_t *dev,
                                         uint8_t *data,
                                         size_t len)
{
    esp_err_t err = ESP_FAIL;
    for (uint8_t attempt = 0; attempt <= ctx->max_retries; attempt++) {
        err = i2c_master_receive(dev->real_handle, data, len,
                                 ctx->xfer_timeout_ms);
        if (err == ESP_OK) return ESP_OK;
        if (attempt < ctx->max_retries) {
            ESP_LOGD(TAG, "receive retry %u/%u, err=0x%x",
                     attempt + 1, ctx->max_retries, err);
            vTaskDelay(pdMS_TO_TICKS(ctx->retry_delay_ms));
        }
    }
    ESP_LOGW(TAG, "receive failed after %u retries, err=0x%x",
             ctx->max_retries, err);
    return err;
}

static esp_err_t real_transmit_receive_with_retry(i2c_bus_ctx_t *ctx,
                                                  i2c_bus_device_t *dev,
                                                  const uint8_t *tx_data,
                                                  size_t tx_len,
                                                  uint8_t *rx_data,
                                                  size_t rx_len)
{
    esp_err_t err = ESP_FAIL;
    for (uint8_t attempt = 0; attempt <= ctx->max_retries; attempt++) {
        err = i2c_master_transmit_receive(dev->real_handle, tx_data, tx_len,
                                          rx_data, rx_len,
                                          ctx->xfer_timeout_ms);
        if (err == ESP_OK) return ESP_OK;
        if (attempt < ctx->max_retries) {
            ESP_LOGD(TAG, "transmit_receive retry %u/%u, err=0x%x",
                     attempt + 1, ctx->max_retries, err);
            vTaskDelay(pdMS_TO_TICKS(ctx->retry_delay_ms));
        }
    }
    ESP_LOGW(TAG, "transmit_receive failed after %u retries, err=0x%x",
             ctx->max_retries, err);
    return err;
}

#endif

esp_err_t i2c_bus_manager_init(const i2c_bus_config_t *config,
                               i2c_bus_ctx_t **out_ctx)
{
    if (!config || !out_ctx) return ESP_ERR_INVALID_ARG;

    i2c_bus_ctx_t *ctx = calloc(1, sizeof(i2c_bus_ctx_t));
    if (!ctx) return ESP_ERR_NO_MEM;

    ctx->backend_type = BUS_BACKEND_REAL;
    ctx->max_retries = config->max_retries ? config->max_retries
                                           : I2C_BUS_DEFAULT_MAX_RETRIES;
    ctx->retry_delay_ms = config->retry_delay_ms ? config->retry_delay_ms
                                                  : I2C_BUS_DEFAULT_RETRY_MS;
    ctx->xfer_timeout_ms = config->xfer_timeout_ms ? config->xfer_timeout_ms
                                                    : I2C_BUS_DEFAULT_TIMEOUT_MS;

#ifndef XIAOJING_TESTING_MOCK_ONLY
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = config->sda_pin,
        .scl_io_num = config->scl_pin,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = config->internal_pullup,
    };

    esp_err_t err = i2c_new_master_bus(&bus_config, &ctx->bus_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C bus: 0x%x", err);
        free(ctx);
        return err;
    }
    ESP_LOGI(TAG, "I2C bus initialized (SDA=%d, SCL=%d, retries=%u)",
             config->sda_pin, config->scl_pin, ctx->max_retries);
    ESP_LOGI(TAG, "I2C idle levels: SDA=%d SCL=%d",
             gpio_get_level((gpio_num_t)config->sda_pin),
             gpio_get_level((gpio_num_t)config->scl_pin));
#else
    ESP_LOGI(TAG, "I2C bus initialized (mock-only, SDA=%d, SCL=%d)",
             config->sda_pin, config->scl_pin);
#endif

    *out_ctx = ctx;
    return ESP_OK;
}

void i2c_bus_manager_destroy(i2c_bus_ctx_t *ctx)
{
    if (!ctx) return;

    i2c_bus_device_t *dev = ctx->device_head;
    while (dev) {
        i2c_bus_device_t *next = dev->next;

#ifndef XIAOJING_TESTING_MOCK_ONLY
        if (!dev->removed && ctx->backend_type == BUS_BACKEND_REAL
            && dev->real_handle) {
            esp_err_t err = i2c_master_bus_rm_device(dev->real_handle);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "destroy: rm_device 0x%02x failed: 0x%x",
                         dev->address, err);
            }
            dev->real_handle = NULL;
        }
#endif

        dev->removed = true;
#ifdef XIAOJING_TESTING
        ctx->devices_released++;
        s_total_freed++;
#endif
        free(dev);
        dev = next;
    }

#ifndef XIAOJING_TESTING_MOCK_ONLY
    if (ctx->backend_type == BUS_BACKEND_REAL && ctx->bus_handle) {
        esp_err_t err = i2c_del_master_bus(ctx->bus_handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to delete I2C bus: 0x%x", err);
        }
    }
#endif

    free(ctx);
}

esp_err_t i2c_bus_add_device(i2c_bus_ctx_t *ctx,
                             uint16_t dev_addr,
                             uint32_t scl_speed_hz,
                             i2c_bus_device_t **out_device)
{
    if (!ctx || !out_device) return ESP_ERR_INVALID_ARG;

    i2c_bus_device_t *dev = calloc(1, sizeof(i2c_bus_device_t));
    if (!dev) return ESP_ERR_NO_MEM;

    dev->owner = ctx;
    dev->address = dev_addr;
    dev->removed = false;
    dev->next = NULL;

#ifndef XIAOJING_TESTING_MOCK_ONLY
    if (ctx->backend_type == BUS_BACKEND_REAL) {
        i2c_device_config_t dev_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = dev_addr,
            .scl_speed_hz = scl_speed_hz,
        };

        esp_err_t err = i2c_master_bus_add_device(ctx->bus_handle,
                                                  &dev_config,
                                                  &dev->real_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add device 0x%02x: 0x%x", dev_addr, err);
            free(dev);
            return err;
        }
    }
#endif

    dev->next = ctx->device_head;
    ctx->device_head = dev;

#ifdef XIAOJING_TESTING
    ctx->devices_allocated++;
    s_total_allocated++;
#endif

    *out_device = dev;
    return ESP_OK;
}

esp_err_t i2c_bus_remove_device(i2c_bus_ctx_t *ctx,
                                i2c_bus_device_t *device)
{
    if (!ctx || !device) return ESP_ERR_INVALID_ARG;
    if (device->owner != ctx) return ESP_ERR_INVALID_ARG;
    if (device->removed) return ESP_ERR_INVALID_STATE;

#ifndef XIAOJING_TESTING_MOCK_ONLY
    if (ctx->backend_type == BUS_BACKEND_REAL && device->real_handle) {
        esp_err_t err = i2c_master_bus_rm_device(device->real_handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to remove device 0x%02x: 0x%x",
                     device->address, err);
            return err;
        }
        device->real_handle = NULL;
    }
#endif

    device->removed = true;
    return ESP_OK;
}

esp_err_t i2c_bus_probe(i2c_bus_ctx_t *ctx, uint16_t addr)
{
    if (!ctx) return ESP_ERR_INVALID_ARG;

#ifdef XIAOJING_TESTING
    if (ctx->backend_type == BUS_BACKEND_MOCK) {
        if (ctx->mock_xfer_fn) {
            return ctx->mock_xfer_fn(I2C_OP_PROBE, addr,
                                     NULL, 0, NULL, 0,
                                     ctx->mock_user_data);
        }
        return ESP_ERR_INVALID_STATE;
    }
#endif

#ifndef XIAOJING_TESTING_MOCK_ONLY
    return i2c_master_probe(ctx->bus_handle, addr, ctx->xfer_timeout_ms);
#else
    return ESP_ERR_INVALID_STATE;
#endif
}

esp_err_t i2c_bus_transmit(i2c_bus_ctx_t *ctx,
                           i2c_bus_device_t *device,
                           const uint8_t *data,
                           size_t len)
{
    if (!ctx || !device) return ESP_ERR_INVALID_ARG;
    if (device->removed) return ESP_ERR_INVALID_STATE;
    if (device->owner != ctx) return ESP_ERR_INVALID_ARG;

#ifdef XIAOJING_TESTING
    if (ctx->backend_type == BUS_BACKEND_MOCK) {
        if (!ctx->mock_xfer_fn) return ESP_ERR_INVALID_STATE;
        return ctx->mock_xfer_fn(I2C_OP_TRANSMIT, device->address,
                                 data, len, NULL, 0,
                                 ctx->mock_user_data);
    }
#endif

#ifndef XIAOJING_TESTING_MOCK_ONLY
    return real_transmit_with_retry(ctx, device, data, len);
#else
    return ESP_ERR_INVALID_STATE;
#endif
}

esp_err_t i2c_bus_receive(i2c_bus_ctx_t *ctx,
                          i2c_bus_device_t *device,
                          uint8_t *data,
                          size_t len)
{
    if (!ctx || !device) return ESP_ERR_INVALID_ARG;
    if (device->removed) return ESP_ERR_INVALID_STATE;
    if (device->owner != ctx) return ESP_ERR_INVALID_ARG;

#ifdef XIAOJING_TESTING
    if (ctx->backend_type == BUS_BACKEND_MOCK) {
        if (!ctx->mock_xfer_fn) return ESP_ERR_INVALID_STATE;
        return ctx->mock_xfer_fn(I2C_OP_RECEIVE, device->address,
                                 NULL, 0, data, len,
                                 ctx->mock_user_data);
    }
#endif

#ifndef XIAOJING_TESTING_MOCK_ONLY
    return real_receive_with_retry(ctx, device, data, len);
#else
    return ESP_ERR_INVALID_STATE;
#endif
}

esp_err_t i2c_bus_transmit_receive(i2c_bus_ctx_t *ctx,
                                   i2c_bus_device_t *device,
                                   const uint8_t *tx_data,
                                   size_t tx_len,
                                   uint8_t *rx_data,
                                   size_t rx_len)
{
    if (!ctx || !device) return ESP_ERR_INVALID_ARG;
    if (device->removed) return ESP_ERR_INVALID_STATE;
    if (device->owner != ctx) return ESP_ERR_INVALID_ARG;

#ifdef XIAOJING_TESTING
    if (ctx->backend_type == BUS_BACKEND_MOCK) {
        if (!ctx->mock_xfer_fn) return ESP_ERR_INVALID_STATE;
        return ctx->mock_xfer_fn(I2C_OP_TRANSMIT_RECEIVE, device->address,
                                 tx_data, tx_len, rx_data, rx_len,
                                 ctx->mock_user_data);
    }
#endif

#ifndef XIAOJING_TESTING_MOCK_ONLY
    return real_transmit_receive_with_retry(ctx, device, tx_data, tx_len,
                                            rx_data, rx_len);
#else
    return ESP_ERR_INVALID_STATE;
#endif
}

#ifdef XIAOJING_TESTING

esp_err_t i2c_bus_manager_init_mock(const i2c_bus_mock_backend_t *backend,
                                    i2c_bus_ctx_t **out_ctx)
{
    if (!out_ctx) return ESP_ERR_INVALID_ARG;

    i2c_bus_ctx_t *ctx = calloc(1, sizeof(i2c_bus_ctx_t));
    if (!ctx) return ESP_ERR_NO_MEM;

    ctx->backend_type = BUS_BACKEND_MOCK;
    ctx->max_retries = 0;
    ctx->retry_delay_ms = 0;
    ctx->xfer_timeout_ms = 100;

    if (backend) {
        ctx->mock_xfer_fn = backend->xfer_fn;
        ctx->mock_user_data = backend->user_data;
    }

    ESP_LOGI(TAG, "I2C bus initialized in MOCK mode");
    *out_ctx = ctx;
    return ESP_OK;
}

void i2c_bus_get_device_counts(i2c_bus_ctx_t *ctx,
                               int *allocated, int *released)
{
    if (!ctx) {
        if (allocated) *allocated = 0;
        if (released) *released = 0;
        return;
    }
    if (allocated) *allocated = ctx->devices_allocated;
    if (released) *released = ctx->devices_released;
}

void i2c_bus_get_total_counts(int *total_allocated, int *total_freed)
{
    if (total_allocated) *total_allocated = s_total_allocated;
    if (total_freed) *total_freed = s_total_freed;
}

void i2c_bus_reset_total_counts(void)
{
    s_total_allocated = 0;
    s_total_freed = 0;
}

#endif
