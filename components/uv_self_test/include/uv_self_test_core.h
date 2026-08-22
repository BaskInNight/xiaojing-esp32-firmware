#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "machine_types.h"
#include "uv_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * uv_self_test_core.h — UV 灯安全自检核心（纯 C，无 FreeRTOS/服务依赖）
 *
 * 开发/板级诊断命令 uv_self_test 的安全门禁与 request_id 分配。
 * 固定点亮 3000 ms；不创建订单；不经过 wash_planner 完整洗涤序列。
 * 运行时接线见 uv_self_test.h。
 * ================================================================ */

#define UV_SELF_TEST_DURATION_MS 3000U

/* 自检状态（向状态订阅端暴露） */
typedef enum {
    UV_STATE_SELFTEST_INACTIVE = 0,   /* IDLE：未运行或已终态 */
    UV_STATE_SELFTEST_VALIDATING,     /* 已受理，等待 uv_service 校验 */
    UV_STATE_SELFTEST_RUNNING,        /* 灯已点亮 */
    UV_STATE_SELFTEST_STOPPING,       /* 正在关灯 */
    UV_STATE_SELFTEST_COMPLETE,       /* 正常完成，灯已关 */
    UV_STATE_SELFTEST_REJECTED,       /* 门禁拒绝 */
    UV_STATE_SELFTEST_FAULT,          /* 自检故障 */
    UV_STATE_SELFTEST_INTERRUPTED,    /* 被取消/跳过（CANCELED/SKIPPED） */
    UV_STATE_SELFTEST_TIMEOUT,        /* 超时终态 */
    UV_STATE_SELFTEST_UNKNOWN,        /* 终态未知/无法确定 */
} uv_self_test_state_t;

/* 门禁结果 */
typedef enum {
    UV_ST_GATE_OK = 0,
    UV_ST_GATE_MACHINE_NOT_IDLE,      /* executor 忙/有保留 terminal */
    UV_ST_GATE_UV_BUSY,               /* uv_service 非 IDLE */
    UV_ST_GATE_FAULT_ACTIVE,          /* safety fault 激活 */
    UV_ST_GATE_EMERGENCY_ACTIVE,      /* 急停激活 */
    UV_ST_GATE_POSITION_UNKNOWN,      /* 位置快照无效 */
    UV_ST_GATE_POSITION_STALE,        /* 位置快照过期 */
    UV_ST_GATE_POSITION_UNSTABLE,     /* 位置不稳定 */
    UV_ST_GATE_MOTOR_MOVING,          /* 电机运动中 */
    UV_ST_GATE_POSITION_NOT_ZERO,     /* 位置不是 0°（桶对齐） */
    UV_ST_GATE_MCP_UNKNOWN,           /* MCP 输出状态未知 */
    UV_ST_GATE_UV_NOT_CONFIRMED_OFF,  /* UV 未确认 OFF */
} uv_self_test_gate_result_t;

/* 门禁输入（由运行时从各服务快照采集） */
typedef struct {
    bool exec_idle;             /* executor IDLE 且无程序/无保留 terminal */
    bool uv_idle;               /* uv_service IDLE */
    bool uv_confirmed_off;      /* uv snapshot output_state == KNOWN_OFF */
    bool fault_active;          /* safety fault 激活 */
    bool emergency_active;      /* emergency 激活 */
    bool mcp_uv_known;          /* MCP UV 输出 known */
    bool pos_valid;             /* 位置快照有效 */
    bool pos_fresh;             /* 位置新鲜 */
    bool pos_stable;            /* 位置稳定 */
    bool pos_moving;            /* 电机运动 */
    drum_position_t pos;        /* 当前位置 */
} uv_self_test_gate_input_t;

/* 纯门禁判定：返回 UV_ST_GATE_OK 或具体拒绝码（无副作用） */
uv_self_test_gate_result_t uv_self_test_gate_check(
    const uv_self_test_gate_input_t *in);

/* 门禁拒绝码 → machine-readable 字符串（供 BLE ack code 使用） */
const char *uv_self_test_gate_code(uv_self_test_gate_result_t result);

/* 单调 request_id 分配：非 0、wrap 后跳过 0（与 executor 独立） */
machine_request_id_t uv_self_test_next_request_id(machine_request_id_t current);

/* uv_service terminal → 自检终态映射。CANCELED/SKIPPED 映射 INTERRUPTED，
 * TIMEOUT 映射 TIMEOUT，绝不折叠成 COMPLETE；未知映射 UNKNOWN。纯函数。 */
uv_self_test_state_t uv_self_test_map_terminal(uv_terminal_t terminal);

#ifdef __cplusplus
}
#endif
