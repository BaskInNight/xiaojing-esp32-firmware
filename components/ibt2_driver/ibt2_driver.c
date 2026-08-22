/*
 * ibt2_driver.c — IBT-2 H-bridge motor driver
 * HARD CONSTRAINT: RPWM/LPWM never both non-zero.
 * Always zero both sides before setting the active side.
 * GPIO48 (EN) NOT driven — hardware voltage domain unconfirmed.
 */

#include <stdlib.h>
#include "ibt2_driver.h"
#include "board_config.h"
#include "esp_log.h"
#include "driver/ledc.h"
#include "driver/gpio.h"

static const char *TAG = "ibt2";

/* IBT-2 H-bridge EN 使能脚（GPIO48）。此前因 GPIO48 电压域（1.8V/3.3V）未确认
 * 而故意不驱动；用户已 CONFIG_XIAOJING_BOARD_IDENTITY_CONFIRMED=y（板卡身份
 * 已确认、电压域已验证），故在此拉高使能 IBT-2 输出。若未确认则保持不驱动
 * （fail-safe：EN 悬空 → 模块不输出）。 */
static esp_err_t ibt2_en_apply(bool enable)
{
    if (!board_config_is_board_identity_confirmed()) {
        return ESP_OK;   /* 未确认板卡身份：不驱动 EN，保持 fail-safe */
    }
    esp_err_t err = gpio_set_level((gpio_num_t)XIAOJING_GPIO_IBT2_EN, enable ? 1 : 0);
    return err;
}

static esp_err_t configure_en_pin(void)
{
    if (!board_config_is_board_identity_confirmed()) {
        ESP_LOGW(TAG, "GPIO48 (IBT2_EN) NOT driven — board identity unconfirmed");
        return ESP_OK;
    }
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << XIAOJING_GPIO_IBT2_EN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) return err;
    err = gpio_set_level((gpio_num_t)XIAOJING_GPIO_IBT2_EN, 0);  /* 默认禁用 */
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "GPIO48 (IBT2_EN) configured as output (drive when running)");
    return ESP_OK;
}

struct ibt2_ctx {
    ibt2_state_t state;
    uint8_t pwm_percent;
    bool direction_calibrated;
    bool rpwm_is_cw;
};

/* Convert 0-100% to LEDC duty value */
static uint32_t percent_to_duty(uint8_t percent)
{
    /* 11-bit resolution: 0-2047. 20kHz cannot fit in 12 bits on the 80MHz APB clock. */
    return (uint32_t)percent * 2047 / 100;
}

static esp_err_t set_channel_duty(ledc_channel_t channel, uint32_t duty)
{
    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, duty);
    return err == ESP_OK ? ledc_update_duty(LEDC_LOW_SPEED_MODE, channel) : err;
}

/* Configure LEDC timer 2 for IBT-2 */
static esp_err_t configure_ledc_timer(void)
{
    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_11_BIT,
        .timer_num = XIAOJING_LEDC_TIMER_IBT2,
        .freq_hz = XIAOJING_PWM_FREQ_IBT2_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    return ledc_timer_config(&timer_conf);
}

/* Configure LEDC channels for RPWM and LPWM */
static esp_err_t configure_ledc_channels(void)
{
    ledc_channel_config_t rpwm_conf = {
        .gpio_num = XIAOJING_GPIO_IBT2_RPWM,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = XIAOJING_LEDC_CH_IBT2_RPWM,
        .timer_sel = XIAOJING_LEDC_TIMER_IBT2,
        .duty = 0,
        .hpoint = 0,
    };
    esp_err_t err = ledc_channel_config(&rpwm_conf);
    if (err != ESP_OK) {
        return err;
    }

    ledc_channel_config_t lpwm_conf = {
        .gpio_num = XIAOJING_GPIO_IBT2_LPWM,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = XIAOJING_LEDC_CH_IBT2_LPWM,
        .timer_sel = XIAOJING_LEDC_TIMER_IBT2,
        .duty = 0,
        .hpoint = 0,
    };
    return ledc_channel_config(&lpwm_conf);
}

/* Set both channels to zero (atomic stop) */
static void zero_both_channels(void)
{
    (void)set_channel_duty(XIAOJING_LEDC_CH_IBT2_RPWM, 0);
    (void)set_channel_duty(XIAOJING_LEDC_CH_IBT2_LPWM, 0);
}

ibt2_handle_t *ibt2_driver_init(const ibt2_driver_config_t *config)
{
    if (!config) {
        return NULL;
    }

    ibt2_handle_t *handle = calloc(1, sizeof(ibt2_handle_t));
    if (!handle) {
        return NULL;
    }

    handle->direction_calibrated = config->direction_calibrated;
    handle->rpwm_is_cw = config->rpwm_is_cw;
    handle->state = IBT2_STATE_IDLE;
    handle->pwm_percent = 0;

    esp_err_t err = configure_ledc_timer();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC timer config failed: 0x%x", err);
        free(handle);
        return NULL;
    }

    err = configure_ledc_channels();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC channel config failed: 0x%x", err);
        free(handle);
        return NULL;
    }

    err = configure_en_pin();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IBT2_EN pin config failed: 0x%x", err);
        free(handle);
        return NULL;
    }

    /* Ensure both channels start at zero */
    zero_both_channels();

    ESP_LOGI(TAG, "IBT-2 driver initialized (calibrated=%s)",
             config->direction_calibrated ? "yes" : "no");
    return handle;
}

void ibt2_driver_destroy(ibt2_handle_t *handle)
{
    if (!handle) {
        return;
    }
    /* Stop motor before destroying */
    zero_both_channels();
    (void)ibt2_en_apply(false);
    free(handle);
}

esp_err_t ibt2_driver_run_cw(ibt2_handle_t *handle, uint8_t pwm_percent)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pwm_percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->direction_calibrated) {
        ESP_LOGW(TAG, "CW rejected: direction not calibrated");
        return ESP_ERR_INVALID_STATE;
    }

    /* HARD CONSTRAINT: zero both sides first */
    zero_both_channels();

    /* Determine which channel is CW based on calibration */
    uint32_t duty = percent_to_duty(pwm_percent);
    ledc_channel_t cw_ch = handle->rpwm_is_cw ?
                           XIAOJING_LEDC_CH_IBT2_RPWM :
                           XIAOJING_LEDC_CH_IBT2_LPWM;

    if (duty > 0) {
        esp_err_t err = set_channel_duty(cw_ch, duty);
        if (err != ESP_OK) return err;
        /* 使能 H 桥：PWM 有效输出（板卡身份已确认时）。 */
        esp_err_t en_err = ibt2_en_apply(true);
        if (en_err != ESP_OK) return en_err;
    }

    handle->state = (pwm_percent == 0) ? IBT2_STATE_IDLE : IBT2_STATE_RUNNING_CW;
    handle->pwm_percent = pwm_percent;
    return ESP_OK;
}

esp_err_t ibt2_driver_run_ccw(ibt2_handle_t *handle, uint8_t pwm_percent)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pwm_percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->direction_calibrated) {
        ESP_LOGW(TAG, "CCW rejected: direction not calibrated");
        return ESP_ERR_INVALID_STATE;
    }

    /* HARD CONSTRAINT: zero both sides first */
    zero_both_channels();

    /* CCW is opposite of CW */
    uint32_t duty = percent_to_duty(pwm_percent);
    ledc_channel_t ccw_ch = handle->rpwm_is_cw ?
                            XIAOJING_LEDC_CH_IBT2_LPWM :
                            XIAOJING_LEDC_CH_IBT2_RPWM;

    if (duty > 0) {
        esp_err_t err = set_channel_duty(ccw_ch, duty);
        if (err != ESP_OK) return err;
        esp_err_t en_err = ibt2_en_apply(true);
        if (en_err != ESP_OK) return en_err;
    }

    handle->state = (pwm_percent == 0) ? IBT2_STATE_IDLE : IBT2_STATE_RUNNING_CCW;
    handle->pwm_percent = pwm_percent;
    return ESP_OK;
}

esp_err_t ibt2_driver_stop(ibt2_handle_t *handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    zero_both_channels();
    (void)ibt2_en_apply(false);   /* 停：禁能 H 桥（若板卡身份已确认） */
    handle->state = IBT2_STATE_IDLE;
    handle->pwm_percent = 0;
    return ESP_OK;
}

esp_err_t ibt2_driver_emergency_stop(ibt2_handle_t *handle)
{
    return ibt2_driver_stop(handle);
}

ibt2_state_t ibt2_driver_get_state(ibt2_handle_t *handle)
{
    if (!handle) {
        return IBT2_STATE_IDLE;
    }
    return handle->state;
}

uint8_t ibt2_driver_get_pwm(ibt2_handle_t *handle)
{
    if (!handle) {
        return 0;
    }
    return handle->pwm_percent;
}

esp_err_t ibt2_driver_set_calibration(ibt2_handle_t *handle,
                                      bool calibrated,
                                      bool rpwm_is_cw)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    handle->direction_calibrated = calibrated;
    handle->rpwm_is_cw = rpwm_is_cw;
    ESP_LOGI(TAG, "Calibration updated: calibrated=%s, rpwm_is_cw=%s",
             calibrated ? "yes" : "no", rpwm_is_cw ? "yes" : "no");
    return ESP_OK;
}
