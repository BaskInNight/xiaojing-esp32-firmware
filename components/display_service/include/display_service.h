#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DISPLAY_VOICE_IDLE = 0,
    DISPLAY_VOICE_LISTENING,
    DISPLAY_VOICE_THINKING,
    DISPLAY_VOICE_REPLY,
    DISPLAY_VOICE_ERROR,
} display_voice_state_t;

/* Phase 9 three-page layout.  The wash-detail page was folded into the
 * overview page; the old 4th "IO/TELEMETRY" page is now the 2nd page. */
typedef enum {
    DISPLAY_PAGE_OVERVIEW = 0,
    DISPLAY_PAGE_TELEMETRY,
    DISPLAY_PAGE_VOICE,
    DISPLAY_PAGE_COUNT,
} display_page_t;

typedef enum {
    DISPLAY_WAKE_EDGE_IMPULSE = 0,
    DISPLAY_WAKE_ESP_SR,
} display_wake_backend_t;

/* Wi-Fi link state for the header icon (must come from the real runtime). */
typedef enum {
    DISPLAY_WIFI_OFF = 0,       /* 未连接（灰） */
    DISPLAY_WIFI_CONNECTING,    /* 连接中（黄） */
    DISPLAY_WIFI_IP,            /* 已获取 IP（青绿） */
    DISPLAY_WIFI_FAILED,        /* 错误/认证失败（红） */
} display_wifi_state_t;

/* BLE link state for the header icon. */
typedef enum {
    DISPLAY_BLE_OFF = 0,        /* 未启动/关闭（灰） */
    DISPLAY_BLE_ADVERTISING,    /* 广播中（蓝灰） */
    DISPLAY_BLE_CONNECTED,      /* 已连接（亮蓝） */
    DISPLAY_BLE_VOICE_SUSPENDED,/* 语音内存协调临时暂停（黄） */
    DISPLAY_BLE_RECOVER_FAILED, /* 恢复失败（红） */
} display_ble_state_t;

/* Voice backend switching outcome for the voice page status line. */
typedef enum {
    DISPLAY_BACKEND_IDLE = 0,   /* 无切换 */
    DISPLAY_BACKEND_SWITCHING,  /* 切换中（未提前提交目标） */
    DISPLAY_BACKEND_SUCCESS,    /* 切换成功 */
    DISPLAY_BACKEND_FAILURE,    /* 切换失败，保持/恢复原后端 */
    DISPLAY_BACKEND_TIMEOUT,    /* 切换超时，保持原后端 */
} display_backend_switch_t;

/* Output index → output_supported_mask bit (matches the page-2 OUTPUTS order:
 * IN/TR/DR/DT/UV/HT). */
enum {
    DISPLAY_OUTPUT_IN = 0,
    DISPLAY_OUTPUT_TR,
    DISPLAY_OUTPUT_DR,
    DISPLAY_OUTPUT_DT,
    DISPLAY_OUTPUT_UV,
    DISPLAY_OUTPUT_HT,
    DISPLAY_OUTPUT_COUNT,
};

/* Header icon rendering style (shape, not just color, so a color-blind user
 * can still tell link state apart). */
typedef enum {
    DISPLAY_ICON_STYLE_OFF = 0,   /* 轮廓，暗色 */
    DISPLAY_ICON_STYLE_ADV,       /* 轮廓 + 移动标记（连接中/广播中） */
    DISPLAY_ICON_STYLE_ON,        /* 实心填充（已连接/已获取 IP） */
    DISPLAY_ICON_STYLE_SUSPEND,   /* 中柱实心 + 双翼轮廓（语音暂挂） */
    DISPLAY_ICON_STYLE_FAIL,      /* 轮廓 + 红叉 */
} display_icon_style_t;

/* Page-2 output/input LED states (shape + color double-encoded). */
typedef enum {
    DISPLAY_LED_UNKNOWN = 0,      /* 黄色方框 ? */
    DISPLAY_LED_OFF = 1,          /* 灰色空心 ○ */
    DISPLAY_LED_ON = 2,           /* 绿色实心 ● */
    DISPLAY_LED_FAULT = 3,        /* 红色叉 × */
    DISPLAY_LED_UNSUPPORTED = 4,  /* 灰色 --（固件未支持/未初始化） */
    DISPLAY_LED_COOLDOWN = 5,     /* 蓝色锁定/倒计时（冷却中） */
} display_led_state_t;

/* Service-independent, copy-only telemetry contract.  Tri-state outputs use
 * 0=unknown, 1=off, 2=on so the HMI never presents an unknown actuator as
 * safely de-energized. */
typedef struct {
    uint8_t machine_state;
    uint8_t wash_phase;
    int16_t position_degrees;
    int16_t target_position_degrees;
    uint16_t current_step;
    uint16_t total_steps;
    uint32_t elapsed_ms;
    uint32_t remaining_ms;
    uint32_t water_ml;
    uint32_t flow_pulses;
    float turbidity_ntu;
    uint16_t turbidity_raw;
    bool turbidity_valid;
    float temperature_c;
    float humidity_rh;
    bool environment_valid;
    bool water_full;
    bool water_level_valid;
    uint8_t hall_raw_mask;
    uint8_t hall_stable_mask;
    bool hall_valid;              /* 位置快照可读；没有磁铁≠UNKNOWN */
    bool hall_fault;              /* 位置 FSM 故障（红叉） */
    bool bucket_aligned;
    bool gesture_available;
    bool safety_fault;
    bool emergency_stop;
    uint16_t fault_code;
    uint8_t position_pwm;
    uint8_t bl50_pwm;
    uint8_t fan_percent;
    uint8_t fan_state;              /* 0=UNKNOWN 1=OFF 2=ON（dry_service fan_output_state） */
    uint8_t source_valve_state;
    uint8_t transfer_valve_state;
    uint8_t drain_state;
    uint8_t detergent_state;
    uint8_t uv_state;
    uint8_t heater_state;
    /* 耦合热风模块（单继电器，风扇+加热丝并联同步启停）：
     * hot_air_state 0=UNKNOWN 1=OFF 2=ON（命令态，非物理风量证明）；
     * hot_air_cooldown_ms = 剩余冷却锁定（0=无锁）。 */
    uint8_t hot_air_state;
    uint32_t hot_air_cooldown_ms;
    display_wake_backend_t wake_backend;
    bool backend_switching;
    uint8_t wifi_state;         /* display_wifi_state_t */
    uint8_t ble_state;          /* display_ble_state_t */
    uint8_t backend_switch_result;  /* display_backend_switch_t */
    uint8_t backend_switch_target;  /* display_wake_backend_t 切换目标 */
    int32_t wifi_rssi;          /* 0x7fffffff = 未知 */
    uint32_t internal_free_bytes;
    uint32_t mcp_unknown_mask;  /* MCP 未知输出掩码（0=全部已知） */
    uint8_t output_supported_mask;  /* 位 DISPLAY_OUTPUT_*：1=固件支持 */
    uint8_t voice_cloud_state;  /* 0=未知 1=离线 2=网络就绪（非云端确认） */
} display_telemetry_t;

typedef struct {
    display_voice_state_t voice_state;
    bool wifi_connected;
    bool ble_connected;
    bool washing;
    uint8_t wash_progress;
    char wash_step[48];
    display_telemetry_t telemetry;
    /* Match the cloud route's UTF-8 transcript capacity. */
    char transcript[1025];
    char reply[512];
    char error[96];
    uint32_t revision;
} display_model_t;

esp_err_t display_service_init(void);
/* Reserve LCD/LVGL transport resources without constructing UI objects. */
esp_err_t display_service_prepare(void);
esp_err_t display_service_start(void);
esp_err_t display_service_stop(void);
esp_err_t display_service_set_voice_state(display_voice_state_t state);
esp_err_t display_service_set_voice_text(const char *transcript,
                                         const char *reply);
esp_err_t display_service_set_error(const char *message);
esp_err_t display_service_set_connectivity(bool wifi_connected,
                                           bool ble_connected);
/* Read only the two connectivity fields without copying the full model. */
bool display_service_connectivity_matches(bool wifi_connected,
                                          bool ble_connected);
esp_err_t display_service_set_wash(bool washing, uint8_t progress,
                                   const char *step);
esp_err_t display_service_set_telemetry(const display_telemetry_t *telemetry);
/* Thread-safe page requests; LVGL objects are changed by the display task. */
esp_err_t display_service_show_page(display_page_t page);
esp_err_t display_service_step_page(int8_t direction);
esp_err_t display_service_get_model(display_model_t *out);

/* Pure-C display helpers (host-testable, no LVGL/FreeRTOS). */
size_t display_utf8_truncate(char *dst, size_t capacity, const char *src,
                             size_t max_bytes);
size_t display_utf8_sanitize(char *dst, size_t capacity, const char *src,
                             bool (*is_supported)(uint32_t codepoint));
/* Skip a leading speaker prefix (UI-P2-1): 若有前导空白 + prefix（如 "你："），
 * 返回指向 prefix 之后的指针；否则返回原串。用于避免 "你：你：…" 重复前缀。 */
const char *display_skip_speaker_prefix(const char *text, const char *prefix);
void display_backend_label(display_wake_backend_t committed, bool switching,
                           display_wake_backend_t target, uint8_t result,
                           char *buf, size_t capacity);
const char *display_wifi_state_name(uint8_t state);
const char *display_ble_state_name(uint8_t state);
const char *display_voice_state_name(display_voice_state_t state);
/* Same-frame voice-cloud state from the current Wi-Fi link state: 2=网络就绪
 * (IP reachable, cloud NOT confirmed), 1=离线.  Never a fabricated READY. */
uint8_t display_cloud_from_wifi(uint8_t wifi_state);

/* Header icon link colors (uint32_t RGB565-ready hex).  Unknown states map to
 * the dim "off" color so the HMI never renders an unknown link as connected. */
uint32_t display_wifi_color(uint8_t wifi_state);
uint32_t display_ble_color(uint8_t ble_state);

/* Header icon shape semantics: outline vs filled vs fault, so Wi-Fi/BLE state
 * is distinguishable by shape alone (not only by color). */
uint8_t display_wifi_icon_style(uint8_t wifi_state);
uint8_t display_ble_icon_style(uint8_t ble_state);

/* Page-2 I/O truth mapping.  output_state: 0=UNKNOWN, 1=OFF, 2=ON.  A fault
 * always wins; an unsupported/uninitialized output renders "--", never a
 * fabricated UNKNOWN. */
uint8_t display_output_led_state(uint8_t output_state, bool supported,
                                 bool fault);
/* 耦合热风模块 LED：冷却锁定优先显示 COOLDOWN（蓝色），否则按三态显示。 */
uint8_t display_hot_air_led_state(uint8_t output_state, bool supported,
                                  bool fault, bool cooldown_active);
/* position inputs: a healthy snapshot with an inactive hall bit is OFF, not
 * UNKNOWN; only a missing/invalid snapshot is UNKNOWN. */
uint8_t display_position_led_state(bool active, bool snapshot_valid,
                                   bool fault);

#ifdef XIAOJING_TESTING
void display_service_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif
