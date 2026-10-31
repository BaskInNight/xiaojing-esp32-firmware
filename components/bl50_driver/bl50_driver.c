/*
 * ZS-X11B Hall BLDC pulsator driver.
 *
 * The legacy bl50_* API is retained for compatibility with bl50_service.
 * Physical wiring:
 *   GPIO1  -> 2N7002 open-drain -> ZS-X11B direction middle pin
 *   GPIO21 -> 2N7002 open-drain -> ZS-X11B PWM IN
 *   PWM IN -> 4.7k pull-up -> ZS-X11B +5V
 *
 * GPIO21 drives the MOS gate, so a GPIO high pulls PWM IN low. LEDC output is
 * inverted in hardware: logical duty N% therefore appears at PWM IN as N%.
 * Logical 0% leaves GPIO21 high and continuously holds PWM IN at 0V.
 * ZS-X11B has no STOP or BRAKE input; both semantics map to PWM=0/coast.
 */

#include <stdlib.h>
#include "bl50_driver.h"
#include "board_config.h"
#include "esp_log.h"
#include "driver/ledc.h"
#include "driver/gpio.h"

static const char *TAG = "zs_x11b";

struct bl50_ctx {
    bl50_state_t state;
    uint8_t pwm_percent;
};

/* Convert 0-100% to LEDC duty (13-bit: 0-8191) */
static uint32_t percent_to_duty(uint8_t percent)
{
    return (uint32_t)percent * 8191 / 100;
}

static esp_err_t set_pwm_duty(uint32_t duty)
{
    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE,
                                  XIAOJING_LEDC_CH_BL50_PWM, duty);
    return err == ESP_OK
               ? ledc_update_duty(LEDC_LOW_SPEED_MODE,
                                  XIAOJING_LEDC_CH_BL50_PWM)
               : err;
}

static esp_err_t configure_ledc(void)
{
    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num = XIAOJING_LEDC_TIMER_BL50,
        .freq_hz = XIAOJING_PWM_FREQ_BL50_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_conf);
    if (err != ESP_OK) return err;

    ledc_channel_config_t ch_conf = {
        .gpio_num = XIAOJING_GPIO_BL50_PWM,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = XIAOJING_LEDC_CH_BL50_PWM,
        .timer_sel = XIAOJING_LEDC_TIMER_BL50,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = 1,
    };
    return ledc_channel_config(&ch_conf);
}

static esp_err_t configure_gpio(void)
{
    /* Set output latches before enabling the pins. The PWM gate high is the
     * fail-safe state: Q_STOP conducts and clamps ZS-X11B PWM IN to 0V. */
    esp_err_t err = gpio_set_level(XIAOJING_GPIO_BL50_PWM, 1);
    if (err != ESP_OK) return err;
    err = gpio_set_level(XIAOJING_GPIO_BL50_DIR, 0);
    if (err != ESP_OK) return err;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << XIAOJING_GPIO_BL50_DIR) |
                        (1ULL << XIAOJING_GPIO_BL50_PWM),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&io_conf);
    if (err != ESP_OK) return err;

    err = gpio_set_level(XIAOJING_GPIO_BL50_PWM, 1);
    if (err != ESP_OK) return err;
    return gpio_set_level(XIAOJING_GPIO_BL50_DIR, 0);
}

static bool direction_change_while_running(const bl50_handle_t *handle,
                                           bl50_state_t requested)
{
    if (handle->pwm_percent == 0) return false;
    return handle->state != requested;
}

bl50_handle_t *bl50_driver_init(void)
{
    bl50_handle_t *handle = calloc(1, sizeof(bl50_handle_t));
    if (!handle) return NULL;

    /* Establish the hardware clamp before routing LEDC to GPIO21. */
    esp_err_t err = configure_gpio();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO safe-state config failed: 0x%x", err);
        free(handle);
        return NULL;
    }

    err = configure_ledc();
    if (err != ESP_OK) {
        /* Keep the open-drain transistor ON if LEDC setup fails. */
        (void)gpio_set_level(XIAOJING_GPIO_BL50_PWM, 1);
        ESP_LOGE(TAG, "LEDC config failed: 0x%x", err);
        free(handle);
        return NULL;
    }

    err = set_pwm_duty(0);
    if (err != ESP_OK) {
        (void)ledc_stop(LEDC_LOW_SPEED_MODE, XIAOJING_LEDC_CH_BL50_PWM, 1);
        ESP_LOGE(TAG, "PWM safe-state failed: 0x%x", err);
        free(handle);
        return NULL;
    }

    handle->state = BL50_STATE_IDLE;
    handle->pwm_percent = 0;

    ESP_LOGI(TAG, "ZS-X11B driver initialized: pwm_gpio=%d dir_gpio=%d freq=%dHz",
             XIAOJING_GPIO_BL50_PWM, XIAOJING_GPIO_BL50_DIR,
             XIAOJING_PWM_FREQ_BL50_HZ);
    return handle;
}

void bl50_driver_destroy(bl50_handle_t *handle)
{
    if (!handle) return;
    /* Stop safely. idle_level=1 keeps Q_STOP ON after LEDC is detached. */
    (void)set_pwm_duty(0);
    (void)ledc_stop(LEDC_LOW_SPEED_MODE, XIAOJING_LEDC_CH_BL50_PWM, 1);
    (void)gpio_set_level(XIAOJING_GPIO_BL50_PWM, 1);
    free(handle);
}

esp_err_t bl50_driver_run_cw(bl50_handle_t *handle, uint8_t pwm_percent)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (pwm_percent > 100) return ESP_ERR_INVALID_ARG;
    if (direction_change_while_running(handle, BL50_STATE_RUNNING_CW)) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = gpio_set_level(XIAOJING_GPIO_BL50_DIR, 1);
    if (err != ESP_OK) return err;

    uint32_t duty = percent_to_duty(pwm_percent);
    err = set_pwm_duty(duty);
    if (err != ESP_OK) return err;

    handle->state = (pwm_percent == 0) ? BL50_STATE_IDLE : BL50_STATE_RUNNING_CW;
    handle->pwm_percent = pwm_percent;
    return ESP_OK;
}

esp_err_t bl50_driver_run_ccw(bl50_handle_t *handle, uint8_t pwm_percent)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (pwm_percent > 100) return ESP_ERR_INVALID_ARG;
    if (direction_change_while_running(handle, BL50_STATE_RUNNING_CCW)) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = gpio_set_level(XIAOJING_GPIO_BL50_DIR, 0);
    if (err != ESP_OK) return err;

    uint32_t duty = percent_to_duty(pwm_percent);
    err = set_pwm_duty(duty);
    if (err != ESP_OK) return err;

    handle->state = (pwm_percent == 0) ? BL50_STATE_IDLE : BL50_STATE_RUNNING_CCW;
    handle->pwm_percent = pwm_percent;
    return ESP_OK;
}

esp_err_t bl50_driver_brake(bl50_handle_t *handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    /* No active brake on ZS-X11B: PWM=0 requests a coast stop. */
    esp_err_t err = set_pwm_duty(0);
    if (err != ESP_OK) return err;

    handle->state = BL50_STATE_BRAKING;
    handle->pwm_percent = 0;
    return ESP_OK;
}

esp_err_t bl50_driver_stop(bl50_handle_t *handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    /* Hardware-inverted LEDC duty 0 keeps GPIO21 high and PWM IN at 0V. */
    esp_err_t err = set_pwm_duty(0);
    if (err != ESP_OK) return err;

    handle->state = BL50_STATE_IDLE;
    handle->pwm_percent = 0;
    return ESP_OK;
}

esp_err_t bl50_driver_emergency_stop(bl50_handle_t *handle)
{
    return bl50_driver_stop(handle);
}

bl50_state_t bl50_driver_get_state(bl50_handle_t *handle)
{
    return handle ? handle->state : BL50_STATE_IDLE;
}

uint8_t bl50_driver_get_pwm(bl50_handle_t *handle)
{
    return handle ? handle->pwm_percent : 0;
}
