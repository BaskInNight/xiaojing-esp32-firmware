#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "machine_types.h"
#include "xiaojing_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * coupled_hot_air_service.h — 单继电器耦合热风模块服务
 *
 * 硬件拓扑（厂家规定）：220V 固定转速风扇 + 220V 加热丝并联，由同一路
 * 继电器（MCP GPA7 = SAFE_OUTPUT_PTC_HEATER）同步启停。无独立 FAN_ON /
 * HEATER_ON 控制，无第二个继电器。
 *
 * 语义诚实声明：
 * - command_on=true            = 仅代表已请求继电器闭合（命令态）。
 * - output_confirmed_on=true   = 继电器/MCP 输出命令得到现有反馈确认
 *                                （OLAT 闩锁命令成功，非触点电气回读）。
 * - 以上任何字段都绝不命名为 airflow_proven，也不代表固件知道风扇物理
 *   转速。模块风扇与加热丝按厂家并联同步通电。
 * ================================================================ */

/* 冷却锁：每次停止后拒绝再次启动的时间（默认 30s）。 */
#define COUPLED_HOT_AIR_COOLDOWN_MS         30000U
/* 单次硬上限（秒级），第一轮实物自检默认只运行 1s。 */
#define COUPLED_HOT_AIR_HARD_CAP_MS         3000U
#define COUPLED_HOT_AIR_DEFAULT_DURATION_MS 1000U

/* ---- 状态（向订阅端暴露） ---- */

typedef enum {
    CHA_STATE_UNINITIALIZED = 0,
    CHA_STATE_IDLE_OFF,       /* 空闲，继电器 OFF */
    CHA_STATE_STARTING,       /* 正在请求继电器闭合 */
    CHA_STATE_RUNNING,        /* 继电器已闭合，模块通电（命令态） */
    CHA_STATE_STOPPING,       /* 正在请求继电器断开 */
    CHA_STATE_COMPLETE,       /* 正常完成，继电器 OFF 已确认 */
    CHA_STATE_INTERRUPTED,    /* 被中断（紧急/取消），继电器 OFF 已确认 */
    CHA_STATE_TIMEOUT,        /* 超时终态 */
    CHA_STATE_FAULT,          /* 故障（含 OFF 确认失败） */
} coupled_hot_air_state_t;

/* ---- 终态 ---- */

typedef enum {
    CHA_TERMINAL_NONE = 0,
    CHA_TERMINAL_COMPLETE,
    CHA_TERMINAL_INTERRUPTED,
    CHA_TERMINAL_TIMEOUT,
    CHA_TERMINAL_FAULT,
} coupled_hot_air_terminal_t;

/* ---- 请求 ---- */

typedef struct {
    machine_request_id_t request_id;
    uint32_t duration_ms;   /* 固件固定；<= COUPLED_HOT_AIR_HARD_CAP_MS */
} coupled_hot_air_request_t;

/* ---- 快照 ---- */

typedef struct {
    coupled_hot_air_state_t state;
    machine_request_id_t request_id;
    uint32_t elapsed_ms;
    uint32_t requested_duration_ms;

    /* 诚实语义：命令态，非物理风量证明 */
    bool command_on;
    bool output_known;           /* 继电器输出 known */
    bool output_confirmed_on;    /* 继电器命令确认 ON（OLAT 闩锁） */
    bool output_confirmed_off;   /* 继电器命令确认 OFF（OLAT 闩锁） */

    int64_t started_at_ms;
    int64_t deadline_ms;

    coupled_hot_air_terminal_t terminal;
    const char *terminal_reason; /* 机器可读原因（静态字符串） */
    uint32_t generation;
    uint32_t revision;

    bool temperature_valid;
    bool temperature_fresh;
    float temperature_c;
    uint32_t cooldown_remaining_ms;
} coupled_hot_air_snapshot_t;

/* ---- 生命周期 ---- */

esp_err_t coupled_hot_air_service_global_init(void);
esp_err_t coupled_hot_air_service_init(const xiaojing_hal_t *hal,
                                       machine_event_sink_t event_sink);
esp_err_t coupled_hot_air_service_start(void);
esp_err_t coupled_hot_air_service_stop(void);

/* ---- 命令 ---- */

/* 提交热风模块请求。同一时间只接受一个。request_id 必须有效且非 0，
 * duration_ms 必须 >0 且 <= COUPLED_HOT_AIR_HARD_CAP_MS。
 * 门禁（位置/SHT/温度/冷却锁/输出 OFF）在 safety_manager 与
 * actuator_self_test 两侧校验；此处拒绝 busy 与非法参数。 */
esp_err_t coupled_hot_air_service_run_async(const coupled_hot_air_request_t *request);

/* 取消活动请求（按 ID；晚到/错误 ID 忽略）。 */
esp_err_t coupled_hot_air_service_cancel(machine_request_id_t request_id);

/* 紧急：立即请求继电器 OFF（不经 FSM 序列），并通知任务走关断序列。
 * 与生命周期 stop 分离，绝不设置 stop_requested。 */
esp_err_t coupled_hot_air_service_emergency_stop(void);

/* ---- 查询 ---- */

esp_err_t coupled_hot_air_service_get_snapshot(coupled_hot_air_snapshot_t *out);

#ifdef __cplusplus
}
#endif
