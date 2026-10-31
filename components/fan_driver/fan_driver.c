/*
 * fan_driver.c — Fan PWM driver (4-wire, 25kHz standard)
 * LEDC timer 0, channel 0, GPIO38.
 */

#include <stdlib.h>
#include "fan_driver.h"
#include "board_config.h"
#include "esp_log.h"
#include "driver/ledc.h"
#include "driver/gpio.h"

static const char *TAG = "fan";

struct fan_ctx {
    uint8_t percent;
};

static uint32_t percent_to_duty(uint8_t percent)
{
    /* 11-bit resolution: 0-2047. 25kHz cannot fit in 12 bits on the 80MHz APB clock. */
    return (uint32_t)percent * 2047 / 100;
}

static esp_err_t set_fan_duty(uint32_t duty)
{
    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE,
                                  XIAOJING_LEDC_CH_FAN, duty);
    return err == ESP_OK
               ? ledc_update_duty(LEDC_LOW_SPEED_MODE,
                                  XIAOJING_LEDC_CH_FAN)
               : err;
}

fan_handle_t *fan_driver_init(void)
{
    /* GPIO38 (FAN_PWM) requires board identity confirmation */
    if (!board_config_is_board_identity_confirmed()) {
        ESP_LOGW(TAG, "GPIO38 (FAN_PWM) blocked — board identity not confirmed");
        return NULL;
    }

    fan_handle_t *handle = calloc(1, sizeof(fan_handle_t));
    if (!handle) return NULL;

    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_11_BIT,
        .timer_num = XIAOJING_LEDC_TIMER_FAN,
        .freq_hz = XIAOJING_PWM_FREQ_FAN_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC timer config failed: 0x%x", err);
        free(handle);
        return NULL;
    }

    /* 安全：配置 LEDC 前先强制 GPIO38 拉高（S8050 通 → FAN_PWM_OUT 低 → 风扇停），
     * 消除上电/配置间隙 GPIO 浮空导致风扇误转的窗口。 */
    gpio_set_direction(XIAOJING_GPIO_FAN_PWM, GPIO_MODE_OUTPUT);
    gpio_set_level(XIAOJING_GPIO_FAN_PWM, 1);

    ledc_channel_config_t ch_conf = {
        .gpio_num = XIAOJING_GPIO_FAN_PWM,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = XIAOJING_LEDC_CH_FAN,
        .timer_sel = XIAOJING_LEDC_TIMER_FAN,
        .duty = 0,
        .hpoint = 0,
        /* 风扇低电平有效（S8050 开集电极下拉 FAN_PWM_OUT，4 线风扇 PWM 高=转）：
         * 反转输出使 duty=0 → GPIO 高 → S8050 通 → FAN_PWM_OUT 低 → 风扇停。 */
        .flags.output_invert = true,
    };
    err = ledc_channel_config(&ch_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC channel config failed: 0x%x", err);
        free(handle);
        return NULL;
    }

    handle->percent = 0;
    ESP_LOGI(TAG, "Fan driver initialized (25kHz)");
    return handle;
}

void fan_driver_destroy(fan_handle_t *handle)
{
    if (!handle) return;
    (void)set_fan_duty(0);
    free(handle);
}

esp_err_t fan_driver_set_percent(fan_handle_t *handle, uint8_t percent)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (percent > 100) return ESP_ERR_INVALID_ARG;

    uint32_t duty = percent_to_duty(percent);
    esp_err_t err = set_fan_duty(duty);
    if (err != ESP_OK) return err;
    handle->percent = percent;
    return ESP_OK;
}

esp_err_t fan_driver_stop(fan_handle_t *handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    esp_err_t err = set_fan_duty(0);
    if (err != ESP_OK) return err;
    handle->percent = 0;
    return ESP_OK;
}

uint8_t fan_driver_get_percent(fan_handle_t *handle)
{
    return handle ? handle->percent : 0;
}
