/*
 * uv_self_test_core.c — UV 自检安全门禁纯实现（无 FreeRTOS/服务依赖）
 */

#include "uv_self_test_core.h"

uv_self_test_gate_result_t uv_self_test_gate_check(
    const uv_self_test_gate_input_t *in)
{
    if (!in) return UV_ST_GATE_FAULT_ACTIVE;
    /* 1-2: executor IDLE 且无保留 terminal */
    if (!in->exec_idle) return UV_ST_GATE_MACHINE_NOT_IDLE;
    /* 3: uv_service IDLE */
    if (!in->uv_idle) return UV_ST_GATE_UV_BUSY;
    /* 4: safety fault */
    if (in->fault_active) return UV_ST_GATE_FAULT_ACTIVE;
    /* 5: emergency */
    if (in->emergency_active) return UV_ST_GATE_EMERGENCY_ACTIVE;
    /* 6: position valid */
    if (!in->pos_valid) return UV_ST_GATE_POSITION_UNKNOWN;
    /* 7: position fresh */
    if (!in->pos_fresh) return UV_ST_GATE_POSITION_STALE;
    /* 8: position stable */
    if (!in->pos_stable) return UV_ST_GATE_POSITION_UNSTABLE;
    /* 9: motor stopped */
    if (in->pos_moving) return UV_ST_GATE_MOTOR_MOVING;
    /* 10: position == DRUM_POS_0（桶对齐测试位置） */
    if (in->pos != DRUM_POS_0) return UV_ST_GATE_POSITION_NOT_ZERO;
    /* 11: MCP UV output known */
    if (!in->mcp_uv_known) return UV_ST_GATE_MCP_UNKNOWN;
    /* 12: UV confirmed OFF */
    if (!in->uv_confirmed_off) return UV_ST_GATE_UV_NOT_CONFIRMED_OFF;
    return UV_ST_GATE_OK;
}

const char *uv_self_test_gate_code(uv_self_test_gate_result_t result)
{
    switch (result) {
    case UV_ST_GATE_OK:                  return "UV_SELF_TEST_ACCEPTED";
    case UV_ST_GATE_MACHINE_NOT_IDLE:    return "MACHINE_NOT_IDLE";
    case UV_ST_GATE_UV_BUSY:             return "UV_BUSY";
    case UV_ST_GATE_FAULT_ACTIVE:        return "FAULT_ACTIVE";
    case UV_ST_GATE_EMERGENCY_ACTIVE:    return "EMERGENCY_ACTIVE";
    case UV_ST_GATE_POSITION_UNKNOWN:    return "POSITION_UNKNOWN";
    case UV_ST_GATE_POSITION_STALE:      return "POSITION_STALE";
    case UV_ST_GATE_POSITION_UNSTABLE:   return "POSITION_UNSTABLE";
    case UV_ST_GATE_MOTOR_MOVING:        return "MOTOR_MOVING";
    case UV_ST_GATE_POSITION_NOT_ZERO:   return "POSITION_NOT_ZERO";
    case UV_ST_GATE_MCP_UNKNOWN:         return "MCP_UNKNOWN";
    case UV_ST_GATE_UV_NOT_CONFIRMED_OFF:return "UV_NOT_CONFIRMED_OFF";
    default:                             return "INTERNAL";
    }
}

machine_request_id_t uv_self_test_next_request_id(machine_request_id_t current)
{
    machine_request_id_t next = current + 1;
    return (next == MACHINE_REQUEST_ID_INVALID) ? 1 : next;
}

uv_self_test_state_t uv_self_test_map_terminal(uv_terminal_t terminal)
{
    switch (terminal) {
    case UV_TERMINAL_COMPLETE:
        return UV_STATE_SELFTEST_COMPLETE;
    case UV_TERMINAL_REJECTED:
        return UV_STATE_SELFTEST_REJECTED;
    case UV_TERMINAL_FAULT:
        return UV_STATE_SELFTEST_FAULT;
    case UV_TERMINAL_CANCELED:
    case UV_TERMINAL_SKIPPED:
        return UV_STATE_SELFTEST_INTERRUPTED;
    case UV_TERMINAL_TIMEOUT:
        return UV_STATE_SELFTEST_TIMEOUT;
    case UV_TERMINAL_NONE:
    default:
        return UV_STATE_SELFTEST_UNKNOWN;
    }
}
