#pragma once

#include "xiaojing_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * fake_hal.h — 可控测试 HAL
 * 支持：虚拟时间、输入脚本、输出历史、故障注入、线程安全快照
 * ================================================================ */

/* ---- 输出历史 ---- */

#define FAKE_HAL_HISTORY_CAPACITY  128

typedef enum {
    FAKE_OUTPUT_SET_SAFE = 0,
    FAKE_OUTPUT_SET_IBT2,
    FAKE_OUTPUT_STOP_IBT2,
    FAKE_OUTPUT_SET_BL50,
    FAKE_OUTPUT_STOP_BL50,
    FAKE_OUTPUT_SET_FAN,
    FAKE_OUTPUT_EMERGENCY_SHUTDOWN,
} fake_output_type_t;

typedef struct {
    fake_output_type_t type;
    int64_t timestamp_ms;
    union {
        struct { safe_output_t output; bool enable; } safe;
        struct { ibt2_command_t cmd; } ibt2;
        struct { bl50_command_t cmd; } bl50;
        struct { uint8_t percent; } fan;
    } data;
} fake_output_record_t;

/* ---- 输入脚本 ---- */

#define FAKE_HAL_SCRIPT_CAPACITY  64

typedef enum {
    FAKE_INPUT_MCP = 0,
    FAKE_INPUT_WATER_LEVEL,
    FAKE_INPUT_FLOW,
    FAKE_INPUT_SHT,
} fake_input_type_t;

typedef struct {
    fake_input_type_t type;
    int64_t trigger_after_ms;  /* 虚拟时间偏移，相对于脚本启动 */
    union {
        mcp_input_snapshot_t mcp;
        struct { bool full; } water_level;
        flow_snapshot_t flow;
        sht_sample_t sht;
    } data;
} fake_script_entry_t;

/* ---- 故障注入 ---- */

typedef struct {
    esp_err_t mcp_read_error;
    esp_err_t mcp_write_error;
    esp_err_t ibt2_error;
    esp_err_t stop_ibt2_error;
    esp_err_t bl50_error;
    esp_err_t stop_bl50_error;
    esp_err_t water_level_error;
    esp_err_t flow_error;
    esp_err_t sht_error;
    esp_err_t btn_read_error;
    bool trigger_emergency_on_next_output;
} fake_fault_config_t;

/* ---- 快照 (线程安全读取) ---- */

typedef struct {
    mcp_input_snapshot_t mcp;
    bool water_full;
    flow_snapshot_t flow;
    sht_sample_t sht;
    int64_t virtual_time_ms;
} fake_hal_state_snapshot_t;

/* ---- Fake HAL Context ---- */

typedef struct fake_hal_ctx fake_hal_ctx_t;

/* 创建 fake HAL 上下文并返回配置好的 xiaojing_hal_t */
fake_hal_ctx_t *fake_hal_create(void);

/* 获取 xiaojing_hal_t 函数表指针 */
const xiaojing_hal_t *fake_hal_get_interface(fake_hal_ctx_t *ctx);

/* 销毁 fake HAL 上下文 */
void fake_hal_destroy(fake_hal_ctx_t *ctx);

/* 获取当前单例指针（用于检测泄漏的单例） */
fake_hal_ctx_t *fake_hal_get_singleton(void);

/* ---- 虚拟时间控制 ---- */

/* 设置虚拟时间（毫秒） */
void fake_hal_set_time(fake_hal_ctx_t *ctx, int64_t time_ms);

/*
 * Enable a monotonic wall-clock for interactive FAKE firmware.
 * Unit tests remain deterministic because the clock is virtual by default.
 */
void fake_hal_enable_realtime_clock(fake_hal_ctx_t *ctx, int64_t start_ms);

/* 推进虚拟时间 */
void fake_hal_advance_time(fake_hal_ctx_t *ctx, int64_t delta_ms);

/* 获取当前虚拟时间 */
int64_t fake_hal_get_time(fake_hal_ctx_t *ctx);

/* ---- 输入控制 ---- */

/* 直接设置 MCP 输入 */
void fake_hal_set_mcp(fake_hal_ctx_t *ctx, const mcp_input_snapshot_t *mcp);

/* 设置水位 */
void fake_hal_set_water_level(fake_hal_ctx_t *ctx, bool full);

/* 设置流量 */
void fake_hal_set_flow(fake_hal_ctx_t *ctx, const flow_snapshot_t *flow);

/* 设置 SHT */
void fake_hal_set_sht(fake_hal_ctx_t *ctx, const sht_sample_t *sht);

/* Set button GPIO state and signal interrupt */
void fake_hal_set_button_state(fake_hal_ctx_t *ctx, uint8_t gpio_a_mask);

/* Get button GPIO read attempt count (for deterministic polling) */
uint32_t fake_hal_get_btn_read_count(fake_hal_ctx_t *ctx);

/* ---- 脚本系统 ---- */

/* 加载输入脚本 */
esp_err_t fake_hal_load_script(fake_hal_ctx_t *ctx,
                                const fake_script_entry_t *entries,
                                size_t count);

/* 推进脚本（基于虚拟时间） */
void fake_hal_tick_script(fake_hal_ctx_t *ctx);

/* 重置脚本索引 */
void fake_hal_reset_script(fake_hal_ctx_t *ctx);

/* ---- 输出历史 ---- */

/* 获取输出历史条目数 */
size_t fake_hal_get_history_count(fake_hal_ctx_t *ctx);

/* 获取输出历史（线程安全复制到 out，最多 max_count 条） */
size_t fake_hal_get_history(fake_hal_ctx_t *ctx,
                             fake_output_record_t *out,
                             size_t max_count);

/* 清空输出历史 */
void fake_hal_clear_history(fake_hal_ctx_t *ctx);

/* 查找特定类型输出记录 */
size_t fake_hal_count_output_type(fake_hal_ctx_t *ctx, fake_output_type_t type);

/* ---- 故障注入 ---- */

/* 设置故障配置 */
void fake_hal_set_faults(fake_hal_ctx_t *ctx, const fake_fault_config_t *faults);

/* 清除所有故障 */
void fake_hal_clear_faults(fake_hal_ctx_t *ctx);

/* ---- 线程安全快照 ---- */

/* 获取当前状态快照（线程安全） */
void fake_hal_get_snapshot(fake_hal_ctx_t *ctx, fake_hal_state_snapshot_t *out);

#ifdef __cplusplus
}
#endif
