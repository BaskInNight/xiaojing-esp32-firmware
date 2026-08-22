#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "machine_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * xiaojing_hal.h — 硬件抽象层函数表
 * 业务组件只通过此表访问硬件，不直接调用驱动
 * ================================================================ */

/* ---- MCP 输入快照 ---- */

typedef struct {
    uint8_t gpio_a;       /* GPA 输入 (按钮等) */
    uint8_t gpio_b;       /* GPB 输入 (霍尔/手势/寻零) */
    uint8_t intcap_a;     /* INTCAP A (中断捕获) */
    uint8_t intcap_b;     /* INTCAP B (中断捕获) */
    int64_t timestamp_ms; /* 采样时间 */
} mcp_input_snapshot_t;

/* ---- Safe Output (MCP 安全输出) ---- */

typedef enum {
    SAFE_OUTPUT_TAP_VALVE = 0,    /* GPA0: 自来水进水阀 */
    SAFE_OUTPUT_TRANSFER_VALVE,   /* GPA1: 水箱→洗涤桶阀 */
    SAFE_OUTPUT_UV,               /* GPA2: UV 杀菌灯 */
    SAFE_OUTPUT_DRAIN_VALVE,      /* GPA3: 排水阀 */
    SAFE_OUTPUT_PTC_HEATER,       /* GPA7: PTC 继电器 */
    SAFE_OUTPUT_DETERGENT_PUMP,   /* GPB7: 洗衣液泵 */
} safe_output_t;

#define SAFE_OUTPUT_COUNT  6

/* ---- IBT-2 命令 ---- */

typedef enum {
    IBT2_CMD_STOP = 0,
    IBT2_CMD_CW,
    IBT2_CMD_CCW,
} ibt2_command_type_t;

typedef struct {
    ibt2_command_type_t command;
    uint8_t pwm_percent;  /* 0-100 */
} ibt2_command_t;

/* ---- BL50 命令 ---- */

typedef enum {
    BL50_CMD_STOP = 0,
    BL50_CMD_RUN_CW,
    BL50_CMD_RUN_CCW,
    BL50_CMD_BRAKE,
} bl50_command_type_t;

typedef struct {
    bl50_command_type_t command;
    uint8_t pwm_percent;  /* 0-100 */
} bl50_command_t;

/* ---- Flow Snapshot ---- */

typedef struct {
    uint32_t pulses;        /* 累计脉冲 */
    uint32_t delta_pulses;  /* 自上次读取后的增量 */
    int64_t timestamp_ms;
} flow_snapshot_t;

/* ---- SHT Sample ---- */

typedef struct {
    float temperature_c;
    float humidity_rh;
    bool valid;
    int64_t timestamp_ms;
    uint32_t age_ms;     /* 距采样时间 */
} sht_sample_t;

/* ---- Button GPIO (MCP GPA4/5/6) ---- */

typedef struct {
    uint8_t gpio_a;         /* Raw GPA byte (buttons at bits 4/5/6) */
    uint8_t intcap_a;       /* Interrupt capture GPA */
    int64_t timestamp_ms;
} button_gpio_snapshot_t;

/* ---- PAJ7620 gesture input ---- */

typedef enum {
    HAL_GESTURE_NONE = 0,
    HAL_GESTURE_RIGHT,
    HAL_GESTURE_LEFT,
    HAL_GESTURE_UP,
    HAL_GESTURE_DOWN,
    HAL_GESTURE_FORWARD,
    HAL_GESTURE_BACKWARD,
    HAL_GESTURE_CLOCKWISE,
    HAL_GESTURE_COUNTER_CW,
} hal_gesture_t;

#define BUTTON_GPIO_BIT(btn_id)  (1U << ((btn_id) + 4))  /* BTN1=GPA4, BTN2=GPA5, BTN3=GPA6 */

/* ---- HAL Function Table ---- */

typedef struct {
    /* MCP 输入 */
    esp_err_t (*read_mcp_inputs)(mcp_input_snapshot_t *out);
    /* MCP 安全输出（只通过 safety_manager 授权） */
    esp_err_t (*set_safe_output)(safe_output_t output, bool enable);
    /* IBT-2 姿态电机 */
    esp_err_t (*set_ibt2)(ibt2_command_t command);
    esp_err_t (*stop_ibt2)(void);
    /* BL50 内筒 */
    esp_err_t (*set_bl50)(bl50_command_t command);
    esp_err_t (*stop_bl50)(void);
    /* 风扇 */
    esp_err_t (*set_fan_percent)(uint8_t percent);
    /* 水位 */
    esp_err_t (*read_water_level)(bool *full);
    /* SHT 温湿度 */
    esp_err_t (*read_sht)(sht_sample_t *out);
    /* 流量计 */
    esp_err_t (*read_flow)(flow_snapshot_t *out);
    esp_err_t (*reset_flow_counter)(void);
    /* 单调时间 */
    int64_t (*now_ms)(void);
    /* 安全停机（所有输出归安全态） */
    esp_err_t (*emergency_shutdown)(void);
    /* 按钮 GPIO (MCP GPA4/5/6) */
    esp_err_t (*configure_button_interrupts)(void);
    esp_err_t (*disable_button_interrupts)(void);
    esp_err_t (*wait_button_interrupt)(uint32_t timeout_ms);
    esp_err_t (*read_button_gpio)(button_gpio_snapshot_t *out);
    /* Gesture sensor; ESP_ERR_NOT_SUPPORTED when not fitted. */
    esp_err_t (*read_gesture)(hal_gesture_t *out);
    /* I2S microphone capture. Voice owns sessions; HAL owns the driver. */
    esp_err_t (*audio_start_capture)(void);
    esp_err_t (*audio_stop_capture)(void);
    esp_err_t (*audio_read_pcm)(void *buffer, size_t size,
                                size_t *bytes_read, uint32_t timeout_ms);
} xiaojing_hal_t;

#ifdef __cplusplus
}
#endif
