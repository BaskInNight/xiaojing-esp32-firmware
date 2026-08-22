#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "machine_config.h"
#include "wash_executor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*app_protocol_tx_fn)(const char *json, size_t length,
                                        uint32_t timeout_ms, void *context);
typedef esp_err_t (*app_protocol_wifi_provision_fn)(
    const char *ssid, const char *password, void *context);

/* UV 灯安全自检（开发/板级诊断）。out_code 成功时为
 * "UV_SELF_TEST_ACCEPTED" + out_duration_ms=3000，失败时为 machine-readable
 * 拒绝码。非阻塞，异步提交。 */
typedef esp_err_t (*app_protocol_uv_self_test_fn)(
    const char **out_code, uint32_t *out_duration_ms);
/* 当前自检状态码（uv_self_test_state_t），用于 status 消息。 */
typedef uint8_t (*app_protocol_uv_self_test_state_fn)(void);

/* 执行器板测（开发/板级诊断）。target: "source_valve" / "transfer_valve" /
 * "detergent_pump"。out_code 成功时为 "ACTUATOR_SELF_TEST_ACCEPTED" +
 * out_duration_ms=固定时长，失败时为 machine-readable 拒绝码。非阻塞。 */
typedef esp_err_t (*app_protocol_actuator_self_test_fn)(
    const char *target, const char **out_code, uint32_t *out_duration_ms);

/* 执行器板测快照（status 消息用）。字段与 actuator_self_test_snapshot_t
 * 对应；last_code 为静态字符串指针（生命周期稳定）。 */
typedef struct {
    uint8_t state;               /* actuator_self_test_state_t */
    uint8_t target;              /* actuator_self_test_target_t */
    uint32_t request_id;
    uint32_t duration_ms;
    bool output_confirmed_off;
    const char *last_code;
    uint32_t hot_air_cooldown_ms; /* 热风模块冷却剩余 ms（权威，来自服务快照） */
} app_protocol_actuator_snapshot_t;

typedef bool (*app_protocol_actuator_snapshot_fn)(
    app_protocol_actuator_snapshot_t *out);

/* 电机空载台架（开发/板级诊断，绝不自动让电机通电）。type_name:
 * "pulsator"（主洗涤/波轮）或 "drum"（滚筒/BLDC）。out_code 成功时为
 * "MOTOR_BENCH_ACCEPTED"，失败时为 machine-readable 拒绝码（含
 * NOT_SUPPORTED / EXECUTOR_BUSY / INVALID_STATE / ALREADY_RUNNING /
 * BAD_TYPE 等）。非阻塞，异步执行 —— 每步仅在用户手动摆放稳定并经
 * confirm 确认后才提交一次低能量短脉冲（≤300ms），随后再次确认 OFF
 * 才推进。 */
typedef esp_err_t (*app_protocol_motor_bench_start_fn)(
    const char *type_name, const char **out_code);
/* 当前步位置就位后，用户显式确认提交一次脉冲（仅 WAIT_CONFIRM 消费）。 */
typedef esp_err_t (*app_protocol_motor_bench_confirm_fn)(
    const char **out_code);
/* 取消当前台架。全部输出 OFF。 */
typedef esp_err_t (*app_protocol_motor_bench_cancel_fn)(
    const char **out_code);

/* 电机台架快照（status 消息用）。字段与 motor_bench_snapshot_t 对应；
 * last_code 为静态字符串指针（生命周期稳定）。 */
typedef struct {
    uint8_t state;               /* motor_bench_state_t */
    uint8_t type;                /* motor_bench_type_t */
    uint32_t request_id;
    uint8_t current_step;        /* 0-based */
    uint8_t step_count;
    int32_t required_position;   /* drum_position_t，当前步所需位置 */
    uint8_t current_action;      /* bl50_action_t */
    bool position_ready;         /* 当前步位置已就位，等待确认脉冲 */
    uint8_t terminal;            /* motor_bench_terminal_t */
    bool output_confirmed_off;
    uint32_t elapsed_ms;
    const char *last_code;       /* 静态字符串：ACCEPTED / 拒绝码 / 等待原因 */
} app_protocol_motor_bench_snapshot_t;

typedef bool (*app_protocol_motor_bench_snapshot_fn)(
    app_protocol_motor_bench_snapshot_t *out);

/* 换位电机（IBT-2 有刷位置电机）板测诊断：target_name 为角度名
 * "0"/"45"/"90"/"180"/"270"。out_code 成功时为 "POSITION_MOVE_ACCEPTED"，
 * 失败为 machine-readable 拒绝码。非阻塞，驱动 position_service 移动滚筒。 */
typedef esp_err_t (*app_protocol_position_move_fn)(
    const char *target_name, const char **out_code);

/* 连续换向（position swing）展示台：rounds/duration_ms 由调用方解析后
 * clamp，非阻塞。成功 out_code="POSITION_SWING_ACCEPTED"。 */
typedef esp_err_t (*app_protocol_position_swing_fn)(
    uint32_t rounds, uint32_t duration_ms, const char **out_code);

/* 取消当前连续换向展示台。全部输出 OFF（IBT-2）。 */
typedef esp_err_t (*app_protocol_position_swing_cancel_fn)(
    const char **out_code);

/* position_swing 快照（status 消息用）。字段与 position_swing_snapshot_t
 * 对应；last_code 静态字符串生命周期稳定。 */
typedef struct {
    uint32_t state;               /* position_swing_state_t */
    uint32_t terminal;            /* position_swing_terminal_t，非终态=0 */
    uint32_t request_id;
    uint32_t segment_index;       /* 已完成段数（0-based） */
    uint32_t dir_cw;              /* 当前段方向 1=CW 0=CCW */
    uint32_t off;                 /* output_confirmed_off ? 1 : 0 */
    uint32_t fl;                  /* direction_flipped（诊断） */
    uint32_t elapsed_ms;
    uint32_t emergency;           /* 处于闩锁 ? 1 : 0 */
    uint32_t rounds;              /* 实际受理段数 */
    uint32_t dwell_ms;            /* 实际受理每段时长 */
    const char *last_code;        /* 静态字符串 */
} app_protocol_position_swing_snapshot_t;

typedef bool (*app_protocol_position_swing_snapshot_fn)(
    app_protocol_position_swing_snapshot_t *out);

typedef struct {
    wash_executor_t *executor;
    const machine_config_t *machine_config;
    bool legacy_enabled;
    app_protocol_tx_fn critical_tx;
    app_protocol_tx_fn status_tx;
    app_protocol_wifi_provision_fn provision_wifi;
    app_protocol_uv_self_test_fn uv_self_test;
    app_protocol_uv_self_test_state_fn uv_self_test_state;
    app_protocol_actuator_self_test_fn actuator_self_test;
    app_protocol_actuator_snapshot_fn actuator_self_test_snapshot;
    app_protocol_motor_bench_start_fn motor_bench_start;
    app_protocol_motor_bench_confirm_fn motor_bench_confirm;
    app_protocol_motor_bench_cancel_fn motor_bench_cancel;
    app_protocol_motor_bench_snapshot_fn motor_bench_snapshot;
    app_protocol_position_move_fn position_move_test;
    app_protocol_position_swing_fn position_swing_test;
    app_protocol_position_swing_cancel_fn position_swing_cancel;
    app_protocol_position_swing_snapshot_fn position_swing_snapshot;
    void *provision_context;
    void *tx_context;
} app_protocol_config_t;

typedef struct {
    bool initialized;
    bool running;
    bool connected;
    uint32_t next_program_id;
    uint32_t frames_received;
    uint32_t commands_executed;
    uint32_t duplicates_replayed;
    uint32_t sequence_conflicts;
    uint32_t parse_errors;
    uint32_t planner_rejections;
    uint32_t tx_errors;
    esp_err_t last_error;
} app_protocol_snapshot_t;

esp_err_t app_protocol_init(const app_protocol_config_t *config);
esp_err_t app_protocol_start(void);
esp_err_t app_protocol_stop(void);
esp_err_t app_protocol_process_frame(const char *frame, size_t length);
void app_protocol_set_connected(bool connected);
esp_err_t app_protocol_publish_status(void);
esp_err_t app_protocol_get_snapshot(app_protocol_snapshot_t *out);

#ifdef __cplusplus
}
#endif
