/*
 * position_swing.h — 结构电机（IBT-2）连续换向展示台公共 API
 *
 * 换向/振荡解歧义展示：连续正向转动一段时间 → 刹车 → 反向 → 刹车 →
 * 正向……重复 `rounds` 段后自动停在最后一段的最终位置（安全 OFF）。
 *
 * 安全模型（与 motor_bench_service / actuator_self_test 同源）：
 *   - 绝不自动通电：每次由用户显式「开始」，每段有硬上限；
 *   - test_active 门禁：executor 空闲、无 UV/执行器/motor 板测、无故障；
 *   - fail-closed：任一关键快照失败 → FAULT + 全输出关；
 *   - 终态输出确认关闭（output_confirmed_off）才报告可继续；
 *   - 运行期间 wash_executor_set_external_busy(true)（反向互斥）。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "board_config.h"
#include "machine_types.h"
#include "xiaojing_hal.h"
#include "position_swing_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 单段时长/段数硬上限（比 position move 130s 更严，限制能耗与观众暴露） */
#define POS_SWING_DWELL_MIN_MS        500U
#define POS_SWING_DWELL_DEFAULT_MS    1500U
#define POS_SWING_DWELL_MAX_MS        8000U
#define POS_SWING_ROUNDS_DEFAULT      4U
#define POS_SWING_ROUNDS_MAX          4U
#define POS_SWING_PERIOD_MAX_MS       (POS_SWING_DWELL_MAX_MS + 500U)

/* 状态机（诊断展示台） */
typedef enum {
    POS_SWING_STATE_IDLE = 0,
    POS_SWING_STATE_PRECHECK,
    POS_SWING_STATE_RUNNING_CW,
    POS_SWING_STATE_BRAKING,
    POS_SWING_STATE_RUNNING_CCW,
    POS_SWING_STATE_FAULT,
    POS_SWING_STATE_COMPLETE,
    POS_SWING_STATE_REJECTED,
    POS_SWING_STATE_INTERRUPTED,
    POS_SWING_STATE_TIMEOUT,
} position_swing_state_t;

/* ---- 终态 ---- */
typedef enum {
    POS_SWING_TERMINAL_NONE = 0,
    POS_SWING_TERMINAL_COMPLETE,
    POS_SWING_TERMINAL_REJECTED,
    POS_SWING_TERMINAL_INTERRUPTED,
    POS_SWING_TERMINAL_TIMEOUT,
    POS_SWING_TERMINAL_FAULT,
} position_swing_terminal_t;

/* ---- 请求参数（命令 payload 解析后 clamp） ---- */
typedef struct {
    uint32_t rounds;          /* 0→默认；clamp 到 [1,MAX] */
    uint32_t duration_ms;     /* 0→默认；clamp 到 [MIN,MAX] */
} position_swing_request_t;

/* ---- 快照（status 上报 + 页面回显） ---- */
typedef struct {
    position_swing_state_t state;
    position_swing_terminal_t terminal;     /* 非终态=NONE */
    uint32_t request_id;
    uint32_t segment_index;   /* 0=第1段（CW）；已完成段计数（0-based），间隔 */
    bool dir_cw;              /* 当前段方向 */
    bool dir_flipped;         /* 方向已标定/允许：真机 CW 允许 */
    bool output_confirmed_off;
    uint32_t elapsed_ms;
    bool emergency;           /* 处于闩锁（未复位时对外显示） */
    uint32_t total_rounds;    /* 实际受理段数 */
    uint32_t dwell_ms;        /* 实际受理每段时长 */
} position_swing_snapshot_t;

/* ---- 服务配置（app_bootstrap 填充） ---- */
struct wash_executor;
typedef struct wash_executor wash_executor_t;
typedef struct {
    wash_executor_t *executor;   /* 反向互斥 + 门禁 */
} position_swing_service_config_t;

/* ---- 生命周期（仅 app_bootstrap 调用） ---- */
esp_err_t position_swing_global_init(void);
esp_err_t position_swing_service_init(const position_swing_service_config_t *config);
esp_err_t position_swing_service_start(void);
esp_err_t position_swing_service_stop(void);

/* 注入 HAL（app_bootstrap 创建 hal 成功后调用一次） */
esp_err_t position_swing_set_hal(const xiaojing_hal_t *hal);

/* ---- 协议接入 ---- */
esp_err_t position_swing_service_request(const position_swing_request_t *req,
                                         const char **out_code);
esp_err_t position_swing_service_cancel(const char **out_code);
esp_err_t position_swing_service_emergency(void);
esp_err_t position_swing_service_get_snapshot(position_swing_snapshot_t *out);

/* ---- 纯 C（host-testable）---- */
uint32_t position_swing_clamp_rounds(uint32_t rounds);
uint32_t position_swing_clamp_dwell(uint32_t dwell_ms);

#ifdef __cplusplus
}
#endif