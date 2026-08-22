#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * ESP32 GPIO 映射 — 小净自制主板
 * 来源: 主控接线1.3.html + 01_REQUIREMENTS.md HW-01
 * 所有 GPIO 号只在此文件定义，禁止散落在业务代码中。
 * ================================================================ */

/* ZS-X11B 搅动电机驱动（上层暂保留 BL50 API/命名以兼容协议与服务）。
 * DIR：GPIO1 → 2N7002 开漏 → ZS-X11B 方向接口中间脚。
 * PWM：GPIO21 → 原 STOP 2N7002 开漏 → PWM IN；PWM IN 由 4.7k 上拉到
 * ZS-X11B 自带 5V。LEDC 必须反相，使逻辑 0% 对应开漏持续拉低。
 * GPIO39（原直连 PWM）与 GPIO2（原 BRAKE）不再连接电机驱动。 */
#define XIAOJING_GPIO_PULSATOR_DIR_OD    1
#define XIAOJING_GPIO_PULSATOR_PWM_OD   21

/* 兼容现有 bl50_driver / bl50_service 命名。 */
#define XIAOJING_GPIO_BL50_DIR       XIAOJING_GPIO_PULSATOR_DIR_OD
#define XIAOJING_GPIO_BL50_PWM       XIAOJING_GPIO_PULSATOR_PWM_OD

/* 水位传感器 (XKC-Y25-V, 数字输入, 10k/20k 分压) */
#define XIAOJING_GPIO_WATER_LEVEL    4

/* 浊度传感器 ADC (ADC1_CH4, 10k/20k 分压) */
#define XIAOJING_GPIO_TURBIDITY_ADC  5

/* I2C 总线: MCP23017 + PAJ7620 + SHT30 */
#define XIAOJING_GPIO_I2C_SDA        6
#define XIAOJING_GPIO_I2C_SCL        7

/* ILI9341 SPI LCD (通过 GPIO Matrix 映射) */
#define XIAOJING_GPIO_LCD_DC         9
#define XIAOJING_GPIO_LCD_CS         10
#define XIAOJING_GPIO_LCD_MOSI       11
#define XIAOJING_GPIO_LCD_SCK        12

/* MCP23017 聚合中断 (开漏, 10k 上拉) */
#define XIAOJING_GPIO_MCP_INTB       13

/* I2S 音频: MAX98357 功放 + INMP441 麦克风 */
#define XIAOJING_GPIO_AMP_DIN        14
#define XIAOJING_GPIO_I2S_WS         15
#define XIAOJING_GPIO_I2S_BCLK       16
#define XIAOJING_GPIO_MIC_SD         17

/* 流量计 (NPN 开集电极, PCNT) */
#define XIAOJING_GPIO_FLOW_PCNT      18

/* 双风扇 PWM (S8050 开集电极) */
#define XIAOJING_GPIO_FAN_PWM        38

/* IBT-2 姿态电机 */
#define XIAOJING_GPIO_IBT2_RPWM      40
#define XIAOJING_GPIO_IBT2_LPWM      41
#define XIAOJING_GPIO_IBT2_EN        48

/* 调试串口 UART0 */
#define XIAOJING_GPIO_UART0_TX       43
#define XIAOJING_GPIO_UART0_RX       44

/* 功放使能 (10k 下拉, 上电静音) */
#define XIAOJING_GPIO_AMP_SD         47

/* ================================================================
 * MCP23017 端口映射 — GPA0~7, GPB0~7
 * I2C 地址: 0x20 (A0=A1=A2=GND)
 * ================================================================ */

/* GPA: 输出 (阀门/UV/PTC/泵) + 输入 (按钮) */
#define XIAOJING_MCP_ADDR            0x20

#define XIAOJING_MCP_GPA0_TAP_VALVE     0   /* 自来水进水阀 */
#define XIAOJING_MCP_GPA1_TRANSFER_VALVE 1   /* 水箱→洗涤桶阀 */
#define XIAOJING_MCP_GPA2_UV            2   /* UV 杀菌灯 */
#define XIAOJING_MCP_GPA3_DRAIN_VALVE   3   /* 洗涤桶排水阀 */
#define XIAOJING_MCP_GPA4_BTN1          4   /* 按钮 1 (输入, 低有效) */
#define XIAOJING_MCP_GPA5_BTN2          5   /* 按钮 2 (输入, 低有效) */
#define XIAOJING_MCP_GPA6_BTN3          6   /* 按钮 3 (输入, 低有效) */
#define XIAOJING_MCP_GPA7_PTC           7   /* PTC 继电器 (仅输出) */

/* GPB: 输入 (霍尔/手势) + 输出 (泵) */
#define XIAOJING_MCP_GPB0_HALL_0        0   /* 外筒 0° 霍尔 */
#define XIAOJING_MCP_GPB1_HALL_45       1   /* 外筒 45° 霍尔 */
#define XIAOJING_MCP_GPB2_HALL_90       2   /* 外筒 90° 霍尔 */
#define XIAOJING_MCP_GPB3_HALL_180      3   /* 外筒 180° 霍尔 */
#define XIAOJING_MCP_GPB4_HALL_270      4   /* 外筒 270° 霍尔 */
#define XIAOJING_MCP_GPB5_HALL_BLDC     5   /* BL50 寻零霍尔 */
#define XIAOJING_MCP_GPB6_GESTURE_INT   6   /* PAJ7620 手势中断 */
#define XIAOJING_MCP_GPB7_PUMP          7   /* 洗衣液蠕动泵 (仅输出) */

/* MCP 中断位掩码 (用于 GPIO/INTCAP 读取后解析) */
#define XIAOJING_MCP_INT_HALL_0         (1U << 0)
#define XIAOJING_MCP_INT_HALL_45        (1U << 1)
#define XIAOJING_MCP_INT_HALL_90        (1U << 2)
#define XIAOJING_MCP_INT_HALL_180       (1U << 3)
#define XIAOJING_MCP_INT_HALL_270       (1U << 4)
#define XIAOJING_MCP_INT_HALL_BLDC      (1U << 5)
#define XIAOJING_MCP_INT_GESTURE        (1U << 6)
#define XIAOJING_MCP_INT_ALL_HALL       (0x3FU)  /* GPB0~5 */
#define XIAOJING_MCP_INT_BTN_MASK_PORTA (0x70U)  /* GPA4~6 */

/* ================================================================
 * LEDC PWM 资源分配
 * ESP32-S3: 8 通道, 4 timer
 * ================================================================ */
#define XIAOJING_LEDC_TIMER_FAN         0
#define XIAOJING_LEDC_TIMER_BL50        1
#define XIAOJING_LEDC_TIMER_IBT2        2
#define XIAOJING_LEDC_CH_FAN            0
#define XIAOJING_LEDC_CH_BL50_PWM       1
#define XIAOJING_LEDC_CH_IBT2_RPWM      2
#define XIAOJING_LEDC_CH_IBT2_LPWM      3

#define XIAOJING_PWM_FREQ_FAN_HZ        25000   /* 25kHz 四线风扇标准 */
#define XIAOJING_PWM_FREQ_BL50_HZ       5000    /* ZS-X11B 初始板测频率 */
#define XIAOJING_PWM_FREQ_IBT2_HZ       20000   /* IBT-2 20kHz */

/* ================================================================
 * PCNT 资源
 * ================================================================ */
#define XIAOJING_PCNT_UNIT_FLOW         0

/* ================================================================
 * 输出模式运行时查询
 * ================================================================ */
typedef enum {
    XIAOJING_MODE_FAKE = 0,
    XIAOJING_MODE_LOW_VOLTAGE,
    XIAOJING_MODE_REAL,
} xiaojing_output_mode_t;

xiaojing_output_mode_t board_config_get_output_mode(void);
bool board_config_is_ptc_enabled(void);
bool board_config_is_hot_air_module_enabled(void);
bool board_config_is_voice_enabled(void);
bool board_config_is_display_enabled(void);
bool board_config_is_legacy_ble_enabled(void);

const char *board_config_get_output_mode_str(void);

bool board_config_is_board_identity_confirmed(void);

#ifdef __cplusplus
}
#endif
