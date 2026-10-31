/*
 * actuator_self_test_core.c — 执行器板测安全门禁纯实现（无 FreeRTOS/服务依赖）
 */

#include "actuator_self_test_core.h"

#include <string.h>

uint32_t actuator_self_test_duration_ms(actuator_self_test_target_t target)
{
    switch (target) {
    case ACT_TARGET_SOURCE_VALVE:
        return ACT_SELF_TEST_SOURCE_VALVE_DURATION_MS;
    case ACT_TARGET_TRANSFER_VALVE:
        return ACT_SELF_TEST_TRANSFER_VALVE_DURATION_MS;
    case ACT_TARGET_DETERGENT_PUMP:
        return ACT_SELF_TEST_DETERGENT_PUMP_DURATION_MS;
    case ACT_TARGET_DRAIN_PUMP:
        return ACT_SELF_TEST_DRAIN_PUMP_DURATION_MS;
    case ACT_TARGET_FAN:
        return ACT_SELF_TEST_FAN_DURATION_MS;
    case ACT_TARGET_HOT_AIR_COUPLED:
        return ACT_SELF_TEST_HOT_AIR_DURATION_MS;
    default:
        return 0U;
    }
}

const char *actuator_self_test_target_name(actuator_self_test_target_t target)
{
    switch (target) {
    case ACT_TARGET_SOURCE_VALVE:
        return "source_valve";
    case ACT_TARGET_TRANSFER_VALVE:
        return "transfer_valve";
    case ACT_TARGET_DETERGENT_PUMP:
        return "detergent_pump";
    case ACT_TARGET_DRAIN_PUMP:
        return "drain_pump";
    case ACT_TARGET_FAN:
        return "fan";
    case ACT_TARGET_HOT_AIR_COUPLED:
        return "hot_air_coupled";
    default:
        return "";
    }
}

bool actuator_self_test_target_from_name(const char *name,
                                         actuator_self_test_target_t *out)
{
    if (!name || !out) return false;
    if (strcmp(name, "source_valve") == 0) {
        *out = ACT_TARGET_SOURCE_VALVE;
        return true;
    }
    if (strcmp(name, "transfer_valve") == 0) {
        *out = ACT_TARGET_TRANSFER_VALVE;
        return true;
    }
    if (strcmp(name, "detergent_pump") == 0) {
        *out = ACT_TARGET_DETERGENT_PUMP;
        return true;
    }
    if (strcmp(name, "drain_pump") == 0) {
        *out = ACT_TARGET_DRAIN_PUMP;
        return true;
    }
    if (strcmp(name, "fan") == 0) {
        *out = ACT_TARGET_FAN;
        return true;
    }
    if (strcmp(name, "hot_air_coupled") == 0) {
        *out = ACT_TARGET_HOT_AIR_COUPLED;
        return true;
    }
    return false;
}

actuator_self_test_gate_result_t actuator_self_test_gate_check(
    const actuator_self_test_gate_input_t *in)
{
    if (!in) return ACT_GATE_FAULT_ACTIVE;
    /* P1-2 fail-closed：目标所需服务快照不可用 → 明确机器可读拒绝码，绝不
     * 读取失败快照字段、绝不把 UNKNOWN 当 OFF。 */
    if (in->snapshot_fail != ACT_GATE_OK) return in->snapshot_fail;
    /* executor 空闲且无程序/无保留 terminal */
    if (!in->exec_idle) return ACT_GATE_EXECUTOR_BUSY;
    /* 无其他自检运行 */
    if (in->self_test_busy) return ACT_GATE_SELF_TEST_BUSY;
    /* safety fault 未锁存 */
    if (in->fault_active) return ACT_GATE_FAULT_ACTIVE;
    /* emergency 未激活 */
    if (in->emergency_active) return ACT_GATE_EMERGENCY_ACTIVE;
    /* 位置样本存在（pos_valid 与 pos_fresh 分离，stale 可拒绝） */
    if (!in->pos_valid) return ACT_GATE_POSITION_UNKNOWN;
    if (!in->pos_fresh) return ACT_GATE_POSITION_STALE;
    if (!in->pos_stable) return ACT_GATE_POSITION_UNSTABLE;
    if (in->pos_moving) return ACT_GATE_MOTOR_MOVING;
    /* 桶位须等于目标所需位置（source/transfer/det=0°, drain=180°, fan=270°,
     * hot_air_coupled=270°）。线码 ACT_GATE_POSITION_NOT_ZERO 与 UV 自检同名
     * （UV 真要求 0°）；对执行器自检其含义是"未对齐到目标角度"，非字面"非 0°"。
     * 保持线码稳定以兼容已部署小程序，文案在小程序侧按目标动态显示角度。 */
    if (in->pos != in->required_pos) return ACT_GATE_POSITION_NOT_ZERO;
    /* 目标输出状态已知 */
    if (!in->mcp_target_known) return ACT_GATE_OUTPUT_STATE_UNKNOWN;
    /* water 初始安全 OFF 同步未到终态：输出状态未建立，不得受理任何板测
     * （HW-FIX-1）。每阀成败由 target_confirmed_off（UNKNOWN）进一步表达。 */
    if (!in->water_safe_off_synced) return ACT_GATE_OUTPUT_STATE_UNKNOWN;
    /* 目标输出当前确认 OFF */
    if (!in->target_confirmed_off) return ACT_GATE_OUTPUT_ALREADY_ON;
    /* 互斥输出确认 OFF */
    if (!in->conflict_confirmed_off) return ACT_GATE_CONFLICT_OUTPUT_ON;
    /* water_service / detergent_service / drain_service / dry_service 空闲 */
    if (!in->water_service_idle) return ACT_GATE_SERVICE_BUSY;
    if (!in->detergent_service_idle) return ACT_GATE_SERVICE_BUSY;
    if (!in->drain_service_idle) return ACT_GATE_SERVICE_BUSY;
    if (!in->dry_service_idle) return ACT_GATE_SERVICE_BUSY;
    /* 耦合热风模块（GPA7 继电器，风扇+加热丝并联同步启停）：独立门禁项，
     * 冷却锁与温度上限返回专用机器码。 */
    if (in->target == ACT_TARGET_HOT_AIR_COUPLED) {
        if (!in->hot_air_service_idle) return ACT_GATE_SERVICE_BUSY;
        if (!in->hot_air_confirmed_off) return ACT_GATE_OUTPUT_ALREADY_ON;
        if (in->hot_air_cooldown_active) return ACT_GATE_HOT_AIR_COOLDOWN_ACTIVE;
        if (!in->sht_valid_fresh) return ACT_GATE_SERVICE_BUSY;
        if (!in->sht_below_cutoff) return ACT_GATE_HOT_AIR_TEMP_TOO_HIGH;
    }
    /* fan/heater 路径：风扇与加热器须确认 OFF 且温度样本有效新鲜、低于阈值 */
    if (!in->fan_confirmed_off) return ACT_GATE_SERVICE_BUSY;
    if (!in->heater_confirmed_off) return ACT_GATE_SERVICE_BUSY;
    if (!in->sht_valid_fresh) return ACT_GATE_SERVICE_BUSY;
    if (!in->sht_below_cutoff) return ACT_GATE_SERVICE_BUSY;
    /* request_id 非 0 且单调 */
    if (!in->request_id_ok) return ACT_GATE_REQUEST_ID_INVALID;
    return ACT_GATE_OK;
}

const char *actuator_self_test_gate_code(actuator_self_test_gate_result_t result)
{
    switch (result) {
    case ACT_GATE_OK:                  return "ACTUATOR_SELF_TEST_ACCEPTED";
    case ACT_GATE_EXECUTOR_BUSY:       return "EXECUTOR_BUSY";
    case ACT_GATE_SELF_TEST_BUSY:      return "SELF_TEST_BUSY";
    case ACT_GATE_FAULT_ACTIVE:        return "FAULT_ACTIVE";
    case ACT_GATE_EMERGENCY_ACTIVE:    return "EMERGENCY_ACTIVE";
    case ACT_GATE_POSITION_UNKNOWN:    return "POSITION_UNKNOWN";
    case ACT_GATE_POSITION_STALE:      return "POSITION_STALE";
    case ACT_GATE_POSITION_UNSTABLE:   return "POSITION_UNSTABLE";
    case ACT_GATE_MOTOR_MOVING:        return "MOTOR_MOVING";
    case ACT_GATE_POSITION_NOT_ZERO:   return "POSITION_NOT_ZERO";
    case ACT_GATE_OUTPUT_STATE_UNKNOWN:return "OUTPUT_STATE_UNKNOWN";
    case ACT_GATE_OUTPUT_ALREADY_ON:   return "OUTPUT_ALREADY_ON";
    case ACT_GATE_CONFLICT_OUTPUT_ON:  return "CONFLICT_OUTPUT_ON";
    case ACT_GATE_SERVICE_BUSY:        return "SERVICE_BUSY";
    case ACT_GATE_REQUEST_ID_INVALID:  return "REQUEST_ID_INVALID";
    case ACT_GATE_WATER_SNAPSHOT_UNAVAILABLE:     return "WATER_SNAPSHOT_UNAVAILABLE";
    case ACT_GATE_DETERGENT_SNAPSHOT_UNAVAILABLE: return "DETERGENT_SNAPSHOT_UNAVAILABLE";
    case ACT_GATE_DRAIN_SNAPSHOT_UNAVAILABLE:     return "DRAIN_SNAPSHOT_UNAVAILABLE";
    case ACT_GATE_DRY_SNAPSHOT_UNAVAILABLE:       return "DRY_SNAPSHOT_UNAVAILABLE";
    case ACT_GATE_HOT_AIR_SNAPSHOT_UNAVAILABLE:   return "HOT_AIR_SNAPSHOT_UNAVAILABLE";
    case ACT_GATE_HOT_AIR_COOLDOWN_ACTIVE:        return "HOT_AIR_COOLDOWN_ACTIVE";
    case ACT_GATE_HOT_AIR_TEMP_TOO_HIGH:          return "HOT_AIR_TEMP_TOO_HIGH";
    default:                           return "INTERNAL";
    }
}

machine_request_id_t actuator_self_test_next_request_id(machine_request_id_t current)
{
    machine_request_id_t next = current + 1;
    return (next == MACHINE_REQUEST_ID_INVALID) ? 1 : next;
}

const char *actuator_self_test_state_name(actuator_self_test_state_t state)
{
    switch (state) {
    case ACT_STATE_INACTIVE:    return "INACTIVE";
    case ACT_STATE_CHECKING:    return "CHECKING";
    case ACT_STATE_ACCEPTED:    return "ACCEPTED";
    case ACT_STATE_RUNNING:     return "RUNNING";
    case ACT_STATE_COMPLETE:    return "COMPLETE";
    case ACT_STATE_REJECTED:    return "REJECTED";
    case ACT_STATE_INTERRUPTED: return "INTERRUPTED";
    case ACT_STATE_TIMEOUT:     return "TIMEOUT";
    case ACT_STATE_FAULT:       return "FAULT";
    case ACT_STATE_UNKNOWN:     return "UNKNOWN";
    default:                    return "UNKNOWN";
    }
}

/* Fail-closed OFF proof (pure, host-testable).  Returns true ONLY when both
 * service snapshots succeeded AND every relevant output is KNOWN_OFF.  A
 * missing/timed-out snapshot, an UNKNOWN output, or an ON output all return
 * false: "cannot prove OFF" must never be treated as "closed". */
bool actuator_outputs_confirmed_off(const actuator_off_proof_t *p)
{
    if (!p) return false;
    if (!p->water_snapshot_ok || !p->detergent_snapshot_ok ||
        !p->drain_snapshot_ok || !p->dry_snapshot_ok ||
        !p->hot_air_snapshot_ok) return false;
    if (p->source_unknown || p->transfer_unknown) return false;
    if (!p->source_off || !p->transfer_off) return false;
    if (!p->detergent_off) return false;
    if (!p->drain_off) return false;
    if (!p->fan_off || !p->heater_off) return false;
    if (!p->hot_air_off) return false;
    return true;
}
