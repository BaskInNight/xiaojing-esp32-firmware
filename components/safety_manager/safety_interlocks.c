/*
 * safety_interlocks.c — 纯 C 确定性策略层
 * 不依赖 FreeRTOS、ESP Timer、I2C、MCP 或具体 service
 * 所有时间使用显式传入的 now_ms
 * 输入相同必须得到相同结果
 */

#include "safety_interlocks.h"
#include <string.h>

/* ---- MCP 输出对应的安全操作映射 ---- */

bool safety_is_mcp_output_op(safety_operation_t op)
{
    switch (op) {
    case SAFETY_OP_SOURCE_INLET_VALVE:
    case SAFETY_OP_TRANSFER_VALVE:
    case SAFETY_OP_DRAIN_VALVE:
    case SAFETY_OP_DETERGENT_PUMP:
    case SAFETY_OP_UV:
    case SAFETY_OP_PTC_HEATER:
    case SAFETY_OP_HOT_AIR_MODULE:
        return true;
    default:
        return false;
    }
}

/* ---- 位置互锁规则 ---- */

/* 姿态 freshness + known + stable 检查 (用于依赖固定姿态的操作) */
static bool check_position_fresh_and_known(const safety_inputs_t *inputs,
                                             const safety_interlock_config_t *config,
                                             safety_decision_t *decision)
{
    if (!inputs->position_sample_valid) {
        decision->allowed = false;
        decision->reason = SAFETY_REJECT_POSITION_UNKNOWN;
        decision->fault_code = FAULT_NONE;
        decision->interlock_mask = SAFETY_ILK_POSITION_UNKNOWN;
        return false;
    }

    /* Stale check: unsigned subtraction, wrap-safe.
     * valid=true 时无论 sample_ms 是否为 0 都计算 elapsed */
    if (config->position_stale_timeout_ms > 0) {
        uint32_t elapsed = (uint32_t)(inputs->now_ms - inputs->position_sample_ms);
        if (elapsed >= config->position_stale_timeout_ms) {
            decision->allowed = false;
            decision->reason = SAFETY_REJECT_POSITION_STALE;
            decision->fault_code = FAULT_NONE;
            decision->interlock_mask = SAFETY_ILK_POSITION_STALE;
            return false;
        }
    }

    if (inputs->position == DRUM_POS_UNKNOWN) {
        decision->allowed = false;
        decision->reason = SAFETY_REJECT_POSITION_UNKNOWN;
        decision->fault_code = FAULT_NONE;
        decision->interlock_mask = SAFETY_ILK_POSITION_UNKNOWN;
        return false;
    }

    if (!inputs->position_stable) {
        decision->allowed = false;
        decision->reason = SAFETY_REJECT_POSITION_MISMATCH;
        decision->fault_code = FAULT_NONE;
        decision->interlock_mask = SAFETY_ILK_POSITION_MISMATCH;
        return false;
    }

    return true; /* All checks passed */
}

/* ---- 阀门互斥检查 ---- */

bool safety_mcp_output_conflict(const safety_mcp_output_state_t *current,
                                 safety_operation_t requested_op,
                                 bool requested_enable)
{
    if (!requested_enable || !current) {
        return false;
    }

    /* GPA0(source) 和 GPA1(transfer) 不得同时开启 */
    if (requested_op == SAFETY_OP_SOURCE_INLET_VALVE && current->transfer_valve) {
        return true;
    }
    if (requested_op == SAFETY_OP_TRANSFER_VALVE && current->source_inlet_valve) {
        return true;
    }

    /* GPA0/GPA1 和 GPA3(drain) 不得同时开启 */
    if (requested_op == SAFETY_OP_SOURCE_INLET_VALVE && current->drain_valve) {
        return true;
    }
    if (requested_op == SAFETY_OP_TRANSFER_VALVE && current->drain_valve) {
        return true;
    }
    if (requested_op == SAFETY_OP_DRAIN_VALVE &&
        (current->source_inlet_valve || current->transfer_valve)) {
        return true;
    }

    return false;
}

/* ---- MCP Output Known Helpers ---- */

bool safety_mcp_output_is_known(uint32_t known_mask, safe_output_t output)
{
    return (known_mask & (1U << (int)output)) != 0;
}

bool safety_all_mcp_known_off(const safety_mcp_output_state_t *mcp,
                               uint32_t known_mask)
{
    if (!mcp) return false;
    uint32_t all_mask = ((1U << SAFE_OUTPUT_COUNT) - 1);
    if ((known_mask & all_mask) != all_mask) return false; /* some UNKNOWN */
    return !mcp->source_inlet_valve && !mcp->transfer_valve &&
           !mcp->uv && !mcp->drain_valve &&
           !mcp->ptc_heater && !mcp->detergent_pump;
}

/* ---- PTC 强制关断检查 ---- */

bool safety_ptc_force_shutdown(const safety_inputs_t *inputs,
                                const safety_interlock_config_t *config)
{
    if (!inputs || !config) {
        return true;
    }

    /* 过温 */
    if (inputs->sht_valid && inputs->sht_fresh &&
        inputs->sht_sample.temperature_c >= config->heater_cutoff_c) {
        return true;
    }

    /* SHT 失效或过期 */
    if (!inputs->sht_valid || !inputs->sht_fresh) {
        return true;
    }

    /* PTC 超时 */
    if (inputs->ptc_on && inputs->ptc_on_since_ms > 0 &&
        config->heat_on_max_ms > 0 &&
        (uint64_t)(inputs->now_ms - inputs->ptc_on_since_ms) >= config->heat_on_max_ms) {
        return true;
    }

    return false;
}

/* ---- 水路输出安全检查 ---- */

bool safety_water_outputs_safe(const safety_mcp_output_state_t *mcp,
                                uint32_t known_mask)
{
    if (!mcp) return false;
    /* 水路输出: GPA0, GPA1, GPA3, GPB7 */
    static const safe_output_t water_outputs[] = {
        SAFE_OUTPUT_TAP_VALVE, SAFE_OUTPUT_TRANSFER_VALVE,
        SAFE_OUTPUT_DRAIN_VALVE, SAFE_OUTPUT_DETERGENT_PUMP,
    };
    for (size_t i = 0; i < sizeof(water_outputs)/sizeof(water_outputs[0]); i++) {
        if (!safety_mcp_output_is_known(known_mask, water_outputs[i])) return false;
    }
    return !mcp->source_inlet_valve && !mcp->transfer_valve &&
           !mcp->drain_valve && !mcp->detergent_pump;
}

/* ---- 加热器安全检查 ---- */

bool safety_heater_safe(const safety_mcp_output_state_t *mcp,
                         uint32_t known_mask,
                         const safety_inputs_t *inputs,
                         const safety_interlock_config_t *config)
{
    if (!mcp || !inputs || !config) return false;
    /* PTC 输出必须已知 */
    if (!safety_mcp_output_is_known(known_mask, SAFE_OUTPUT_PTC_HEATER)) return false;
    if (mcp->ptc_heater) return false;
    if (inputs->sht_valid && inputs->sht_fresh &&
        inputs->sht_sample.temperature_c >= config->heater_resume_c) {
        return false;
    }
    return true;
}

/* ---- 允许操作掩码计算（通过 interlock_check 保证一致性） ---- */

uint64_t safety_compute_allowed_mask(const safety_inputs_t *inputs,
                                      const safety_interlock_config_t *config)
{
    if (!inputs || !config) return 0;

    uint64_t mask = 0;

    /* 代表性操作：逐个用 interlock_check 判定 */
    static const struct {
        safety_operation_t op;
        bool enable;
        safety_motor_params_t motor;
        safety_fan_params_t fan;
        bool use_fan_params;
    } probe_ops[] = {
        { SAFETY_OP_SOURCE_INLET_VALVE, true, {0}, {0}, false },
        { SAFETY_OP_TRANSFER_VALVE,     true, {0}, {0}, false },
        { SAFETY_OP_DRAIN_VALVE,        true, {0}, {0}, false },
        { SAFETY_OP_DETERGENT_PUMP,     true, {0}, {0}, false },
        { SAFETY_OP_UV,                 true, {0}, {0}, false },
        { SAFETY_OP_PTC_HEATER,         true, {0}, {0}, false },
        { SAFETY_OP_HOT_AIR_MODULE,     true, {0}, {0}, false },
        { SAFETY_OP_FAN,                true, {0}, {.percent=80}, true },
        { SAFETY_OP_POSITION_MOTOR,     true, {.pwm_percent=30, .clockwise=true}, {0}, false },
        { SAFETY_OP_BL50,               true, {0}, {0}, false },
    };

    for (size_t i = 0; i < sizeof(probe_ops)/sizeof(probe_ops[0]); i++) {
        safety_request_t req;
        memset(&req, 0, sizeof(req));
        req.operation = probe_ops[i].op;
        req.enable = probe_ops[i].enable;
        if (probe_ops[i].use_fan_params) {
            req.params.fan = probe_ops[i].fan;
        } else {
            req.params.motor = probe_ops[i].motor;
        }
        /* BL50: use current position as required_position when stable+valid */
        if (probe_ops[i].op == SAFETY_OP_BL50 &&
            inputs->position_sample_valid && inputs->position_stable &&
            drum_position_is_valid(inputs->position)) {
            req.required_position = inputs->position;
        }
        safety_decision_t dec;
        safety_interlock_check(inputs, config, &req, &dec);
        if (dec.allowed) {
            mask |= (1ULL << probe_ops[i].op);
        }
    }

    return mask;
}

/* ---- 完整联锁判断 ---- */

void safety_interlock_check(const safety_inputs_t *inputs,
                             const safety_interlock_config_t *config,
                             const safety_request_t *request,
                             safety_decision_t *decision)
{
    /* 清零输出 */
    decision->allowed = false;
    decision->reason = SAFETY_REJECT_NONE;
    decision->fault_code = FAULT_NONE;
    decision->interlock_mask = SAFETY_ILK_NONE;

    /* ---- 前置校验 ---- */

    if (!inputs || !config || !request) {
        decision->reason = SAFETY_REJECT_INVALID_PARAM;
        decision->interlock_mask = SAFETY_ILK_INVALID_PARAM;
        return;
    }

    safety_operation_t op = request->operation;
    bool enable = request->enable;

    /* 操作码范围检查 */
    if (op >= SAFETY_OP_COUNT) {
        decision->reason = SAFETY_REJECT_INVALID_PARAM;
        decision->interlock_mask = SAFETY_ILK_INVALID_PARAM;
        return;
    }

    /* ---- OFF 请求：安全方向，允许执行 ---- */

    if (!enable) {
        decision->allowed = true;
        decision->reason = SAFETY_REJECT_NONE;
        return;
    }

    /* ---- 从这里开始是 ON 请求的联锁检查 ---- */

    /* 未初始化 */
    if (inputs->output_mode == XIAOJING_MODE_FAKE &&
        inputs->position == DRUM_POS_UNKNOWN &&
        !inputs->position_stable &&
        !inputs->ptc_enabled_in_config) {
        /* 宽松判定：FAKE 模式下不强制要求 INITIALIZED 标志 */
    }

    /* 故障锁存 */
    if (inputs->fault_active) {
        decision->reason = SAFETY_REJECT_FAULT_ACTIVE;
        decision->interlock_mask = SAFETY_ILK_FAULT_ACTIVE;
        return;
    }

    /* 急停 */
    if (inputs->emergency_stop_active) {
        decision->reason = SAFETY_REJECT_EMERGENCY;
        decision->interlock_mask = SAFETY_ILK_EMERGENCY;
        return;
    }

    /* ---- 按操作类型检查（姿态门控移入各操作内） ---- */

    switch (op) {
    case SAFETY_OP_POSITION_MOTOR: {
        /* 寻位: 不要求当前姿态 known/fresh/stable
         * 但必须检查所有 MCP 输出为 KNOWN_OFF */
        if (!safety_all_mcp_known_off(&inputs->mcp_outputs, inputs->mcp_known_mask)) {
            decision->reason = SAFETY_REJECT_OUTPUT_UNKNOWN;
            decision->interlock_mask = SAFETY_ILK_OUTPUT_UNKNOWN;
            return;
        }
        /* BL50 必须 STOPPED (不是 RUNNING 也不是 UNKNOWN) */
        if (!inputs->bl50_state_known || inputs->bl50_running) {
            decision->reason = SAFETY_REJECT_MOTOR_RUNNING;
            decision->interlock_mask = SAFETY_ILK_BL50_RUNNING;
            return;
        }
        if (request->params.motor.pwm_percent < 5 ||
            request->params.motor.pwm_percent > 100) {
            decision->reason = SAFETY_REJECT_INVALID_PARAM;
            decision->interlock_mask = SAFETY_ILK_INVALID_PARAM;
            return;
        }
        decision->allowed = true;
        break;
    }

    case SAFETY_OP_BL50: {
        /* BL50 ON: 需要 fresh 稳定姿态 + required_position + MCP 全部 KNOWN_OFF */
        if (!safety_all_mcp_known_off(&inputs->mcp_outputs, inputs->mcp_known_mask)) {
            decision->reason = SAFETY_REJECT_OUTPUT_UNKNOWN;
            decision->interlock_mask = SAFETY_ILK_OUTPUT_UNKNOWN;
            return;
        }
        if (!check_position_fresh_and_known(inputs, config, decision)) return;
        if (!drum_position_is_valid(request->required_position)) {
            decision->reason = SAFETY_REJECT_INVALID_PARAM;
            decision->interlock_mask = SAFETY_ILK_INVALID_PARAM;
            return;
        }
        if (inputs->position != request->required_position) {
            decision->reason = SAFETY_REJECT_POSITION_MISMATCH;
            decision->interlock_mask = SAFETY_ILK_POSITION_MISMATCH;
            return;
        }
        if (inputs->position_motor_moving) {
            decision->reason = SAFETY_REJECT_POSITION_MOVING;
            decision->interlock_mask = SAFETY_ILK_POSITION_MOVING;
            return;
        }
        decision->allowed = true;
        break;
    }

    case SAFETY_OP_SOURCE_INLET_VALVE: {
        /* 进水: 需要 fresh 稳定 0°, 所有相关输出已知 */
        if (!check_position_fresh_and_known(inputs, config, decision)) return;
        if (inputs->position != DRUM_POS_0) {
            decision->reason = SAFETY_REJECT_POSITION_MISMATCH;
            decision->interlock_mask = SAFETY_ILK_POSITION_MISMATCH;
            return;
        }
        if (inputs->position_motor_moving) {
            decision->reason = SAFETY_REJECT_POSITION_MOVING;
            decision->interlock_mask = SAFETY_ILK_POSITION_MOVING;
            return;
        }
        /* Check valve conflict BEFORE unknown — conflict is more specific */
        if (safety_mcp_output_conflict(&inputs->mcp_outputs, op, true)) {
            decision->reason = SAFETY_REJECT_VALVE_CONFLICT;
            decision->interlock_mask = SAFETY_ILK_VALVE_CONFLICT;
            return;
        }
        if (!safety_all_mcp_known_off(&inputs->mcp_outputs, inputs->mcp_known_mask)) {
            decision->reason = SAFETY_REJECT_OUTPUT_UNKNOWN;
            decision->interlock_mask = SAFETY_ILK_OUTPUT_UNKNOWN;
            return;
        }
        decision->allowed = true;
        break;
    }

    case SAFETY_OP_TRANSFER_VALVE: {
        /* 转移: 需要 fresh 稳定 0°, 所有相关输出已知 */
        if (!check_position_fresh_and_known(inputs, config, decision)) return;
        if (inputs->position != DRUM_POS_0) {
            decision->reason = SAFETY_REJECT_POSITION_MISMATCH;
            decision->interlock_mask = SAFETY_ILK_POSITION_MISMATCH;
            return;
        }
        if (inputs->position_motor_moving) {
            decision->reason = SAFETY_REJECT_POSITION_MOVING;
            decision->interlock_mask = SAFETY_ILK_POSITION_MOVING;
            return;
        }
        /* Check valve conflict BEFORE unknown — conflict is more specific */
        if (safety_mcp_output_conflict(&inputs->mcp_outputs, op, true)) {
            decision->reason = SAFETY_REJECT_VALVE_CONFLICT;
            decision->interlock_mask = SAFETY_ILK_VALVE_CONFLICT;
            return;
        }
        if (!safety_all_mcp_known_off(&inputs->mcp_outputs, inputs->mcp_known_mask)) {
            decision->reason = SAFETY_REJECT_OUTPUT_UNKNOWN;
            decision->interlock_mask = SAFETY_ILK_OUTPUT_UNKNOWN;
            return;
        }
        decision->allowed = true;
        break;
    }

    case SAFETY_OP_DRAIN_VALVE: {
        /* 排水: 需要 fresh 稳定 180°, 所有相关输出已知 */
        if (!check_position_fresh_and_known(inputs, config, decision)) return;
        if (inputs->position != DRUM_POS_180) {
            decision->reason = SAFETY_REJECT_POSITION_MISMATCH;
            decision->interlock_mask = SAFETY_ILK_POSITION_MISMATCH;
            return;
        }
        if (inputs->position_motor_moving) {
            decision->reason = SAFETY_REJECT_POSITION_MOVING;
            decision->interlock_mask = SAFETY_ILK_POSITION_MOVING;
            return;
        }
        /* Check valve conflict BEFORE unknown — conflict is more specific */
        if (safety_mcp_output_conflict(&inputs->mcp_outputs, op, true)) {
            decision->reason = SAFETY_REJECT_VALVE_CONFLICT;
            decision->interlock_mask = SAFETY_ILK_VALVE_CONFLICT;
            return;
        }
        if (!safety_all_mcp_known_off(&inputs->mcp_outputs, inputs->mcp_known_mask)) {
            decision->reason = SAFETY_REJECT_OUTPUT_UNKNOWN;
            decision->interlock_mask = SAFETY_ILK_OUTPUT_UNKNOWN;
            return;
        }
        decision->allowed = true;
        break;
    }

    case SAFETY_OP_DETERGENT_PUMP: {
        /* 洗衣液: 需要 fresh 稳定 0°, 输出已知 */
        if (!check_position_fresh_and_known(inputs, config, decision)) return;
        if (inputs->position != DRUM_POS_0) {
            decision->reason = SAFETY_REJECT_POSITION_MISMATCH;
            decision->interlock_mask = SAFETY_ILK_POSITION_MISMATCH;
            return;
        }
        if (inputs->position_motor_moving) {
            decision->reason = SAFETY_REJECT_POSITION_MOVING;
            decision->interlock_mask = SAFETY_ILK_POSITION_MOVING;
            return;
        }
        if (!safety_all_mcp_known_off(&inputs->mcp_outputs, inputs->mcp_known_mask)) {
            decision->reason = SAFETY_REJECT_OUTPUT_UNKNOWN;
            decision->interlock_mask = SAFETY_ILK_OUTPUT_UNKNOWN;
            return;
        }
        decision->allowed = true;
        break;
    }

    case SAFETY_OP_UV: {
        /* UV: 需要 fresh 稳定 0°, 输出已知 */
        if (!check_position_fresh_and_known(inputs, config, decision)) return;
        if (inputs->position != DRUM_POS_0) {
            decision->reason = SAFETY_REJECT_POSITION_MISMATCH;
            decision->interlock_mask = SAFETY_ILK_POSITION_MISMATCH;
            return;
        }
        if (inputs->position_motor_moving) {
            decision->reason = SAFETY_REJECT_POSITION_MOVING;
            decision->interlock_mask = SAFETY_ILK_POSITION_MOVING;
            return;
        }
        if (!safety_all_mcp_known_off(&inputs->mcp_outputs, inputs->mcp_known_mask)) {
            decision->reason = SAFETY_REJECT_OUTPUT_UNKNOWN;
            decision->interlock_mask = SAFETY_ILK_OUTPUT_UNKNOWN;
            return;
        }
        decision->allowed = true;
        break;
    }

    case SAFETY_OP_FAN: {
        /* 风扇: 参数检查 + fault/emergency 拦截 + 可选姿态检查 */
        if (request->params.fan.percent > 100) {
            decision->reason = SAFETY_REJECT_INVALID_PARAM;
            decision->interlock_mask = SAFETY_ILK_INVALID_PARAM;
            return;
        }
        if (inputs->fault_active) {
            decision->reason = SAFETY_REJECT_FAULT_ACTIVE;
            decision->interlock_mask = SAFETY_ILK_FAULT_ACTIVE;
            return;
        }
        if (inputs->emergency_stop_active) {
            decision->reason = SAFETY_REJECT_EMERGENCY;
            decision->interlock_mask = SAFETY_ILK_EMERGENCY;
            return;
        }
        /* 可选姿态检查: 当 required_position != UNKNOWN 时启用 */
        if (request->required_position != DRUM_POS_UNKNOWN) {
            if (!check_position_fresh_and_known(inputs, config, decision)) return;
            if (inputs->position != request->required_position ||
                !inputs->position_stable) {
                decision->reason = SAFETY_REJECT_POSITION_MISMATCH;
                decision->interlock_mask = SAFETY_ILK_POSITION_MISMATCH;
                return;
            }
        }
        decision->allowed = true;
        break;
    }

    case SAFETY_OP_PTC_HEATER: {
        /* PTC: 需要 REAL mode + enabled + confirmed + fresh 270° + 完整联锁 */
        if (!config->ptc_enabled) {
            decision->reason = SAFETY_REJECT_CONFIG_DISABLED;
            decision->interlock_mask = SAFETY_ILK_CONFIG_DISABLED;
            return;
        }
        if (config->output_mode != XIAOJING_MODE_REAL) {
            decision->reason = SAFETY_REJECT_CONFIG_DISABLED;
            decision->interlock_mask = SAFETY_ILK_CONFIG_DISABLED;
            return;
        }
        if (!config->board_identity_confirmed) {
            decision->reason = SAFETY_REJECT_CONFIG_DISABLED;
            decision->interlock_mask = SAFETY_ILK_CONFIG_DISABLED;
            return;
        }
        if (!check_position_fresh_and_known(inputs, config, decision)) return;
        if (inputs->position != DRUM_POS_270 || !inputs->position_stable) {
            decision->reason = SAFETY_REJECT_POSITION_MISMATCH;
            decision->interlock_mask = SAFETY_ILK_POSITION_MISMATCH;
            return;
        }
        if (inputs->position_motor_moving) {
            decision->reason = SAFETY_REJECT_POSITION_MOVING;
            decision->interlock_mask = SAFETY_ILK_POSITION_MOVING;
            return;
        }
        /* PTC: 所有 MCP 输出必须 KNOWN_OFF (含 PTC 自身) */
        if (!safety_all_mcp_known_off(&inputs->mcp_outputs, inputs->mcp_known_mask)) {
            decision->reason = SAFETY_REJECT_OUTPUT_UNKNOWN;
            decision->interlock_mask = SAFETY_ILK_OUTPUT_UNKNOWN;
            return;
        }
        if (!inputs->sht_valid) {
            decision->reason = SAFETY_REJECT_SHT_INVALID;
            decision->interlock_mask = SAFETY_ILK_SHT_INVALID;
            return;
        }
        if (!inputs->sht_fresh) {
            decision->reason = SAFETY_REJECT_SHT_STALE;
            decision->interlock_mask = SAFETY_ILK_SHT_STALE;
            return;
        }
        if (!inputs->fan_running) {
            decision->reason = SAFETY_REJECT_FAN_NOT_RUNNING;
            decision->interlock_mask = SAFETY_ILK_FAN_NOT_RUNNING;
            return;
        }
        if (inputs->fan_running_since_ms <= 0 ||
            (uint32_t)(inputs->now_ms - inputs->fan_running_since_ms) < config->pre_fan_ms) {
            decision->reason = SAFETY_REJECT_FAN_PREBLOW_SHORT;
            decision->interlock_mask = SAFETY_ILK_FAN_PREBLOW_SHORT;
            return;
        }
        if (inputs->sht_sample.temperature_c >= config->heater_cutoff_c) {
            decision->reason = SAFETY_REJECT_OVERTEMP;
            decision->interlock_mask = SAFETY_ILK_OVERTEMP;
            decision->fault_code = FAULT_DRY_OVERTEMP;
            return;
        }
        if (inputs->ptc_on && inputs->ptc_on_since_ms > 0 &&
            config->heat_on_max_ms > 0 &&
            (uint32_t)(inputs->now_ms - inputs->ptc_on_since_ms) >= config->heat_on_max_ms) {
            decision->reason = SAFETY_REJECT_PTC_DURATION_EXCEEDED;
            decision->interlock_mask = SAFETY_ILK_OVERTEMP;
            return;
        }
        decision->allowed = true;
        break;
    }

    case SAFETY_OP_HOT_AIR_MODULE: {
        /* 单继电器耦合热风模块（风扇+加热丝按厂家规定并联、同步启停）。
         * 无独立风扇，因此不要求 fan_running / pre_fan —— 模块自带同步风扇。
         * fan_running 语义已改为命令态，绝不作为"真实风量已证明"。 */
        if (!config->hot_air_module_enabled) {
            decision->reason = SAFETY_REJECT_CONFIG_DISABLED;
            decision->interlock_mask = SAFETY_ILK_CONFIG_DISABLED;
            return;
        }
        if (config->output_mode != XIAOJING_MODE_REAL) {
            decision->reason = SAFETY_REJECT_CONFIG_DISABLED;
            decision->interlock_mask = SAFETY_ILK_CONFIG_DISABLED;
            return;
        }
        if (!config->board_identity_confirmed) {
            decision->reason = SAFETY_REJECT_CONFIG_DISABLED;
            decision->interlock_mask = SAFETY_ILK_CONFIG_DISABLED;
            return;
        }
        if (!check_position_fresh_and_known(inputs, config, decision)) return;
        if (inputs->position != DRUM_POS_270 || !inputs->position_stable) {
            decision->reason = SAFETY_REJECT_POSITION_MISMATCH;
            decision->interlock_mask = SAFETY_ILK_POSITION_MISMATCH;
            return;
        }
        if (inputs->position_motor_moving) {
            decision->reason = SAFETY_REJECT_POSITION_MOVING;
            decision->interlock_mask = SAFETY_ILK_POSITION_MOVING;
            return;
        }
        /* 热风模块：所有 MCP 输出必须 KNOWN_OFF（含继电器自身） */
        if (!safety_all_mcp_known_off(&inputs->mcp_outputs, inputs->mcp_known_mask)) {
            decision->reason = SAFETY_REJECT_OUTPUT_UNKNOWN;
            decision->interlock_mask = SAFETY_ILK_OUTPUT_UNKNOWN;
            return;
        }
        if (!inputs->sht_valid) {
            decision->reason = SAFETY_REJECT_SHT_INVALID;
            decision->interlock_mask = SAFETY_ILK_SHT_INVALID;
            return;
        }
        if (!inputs->sht_fresh) {
            decision->reason = SAFETY_REJECT_SHT_STALE;
            decision->interlock_mask = SAFETY_ILK_SHT_STALE;
            return;
        }
        if (inputs->sht_sample.temperature_c >= config->heater_cutoff_c) {
            decision->reason = SAFETY_REJECT_OVERTEMP;
            decision->interlock_mask = SAFETY_ILK_OVERTEMP;
            decision->fault_code = FAULT_DRY_OVERTEMP;
            return;
        }
        /* 已处于 ON：拒绝重复开启（防止双请求交错）。 */
        if (inputs->hot_air_on) {
            decision->reason = SAFETY_REJECT_ALREADY_ACTIVE;
            decision->interlock_mask = SAFETY_ILK_HOT_AIR_ALREADY_ON;
            return;
        }
        /* 冷却锁定：停止后必须等待 hot_air_cooldown_ms 才能再开。 */
        if (config->hot_air_cooldown_ms > 0 &&
            inputs->hot_air_cooldown_until_ms > 0 &&
            inputs->now_ms < inputs->hot_air_cooldown_until_ms) {
            decision->reason = SAFETY_REJECT_HOT_AIR_COOLDOWN_ACTIVE;
            decision->interlock_mask = SAFETY_ILK_HOT_AIR_COOLDOWN;
            return;
        }
        decision->allowed = true;
        break;
    }

    default:
        decision->reason = SAFETY_REJECT_INVALID_PARAM;
        decision->interlock_mask = SAFETY_ILK_INVALID_PARAM;
        break;
    }
}

/* ================================================================
 * Active Output Validation
 *
 * 检查已开启的输出是否应保持开启。
 * 不要求所有 MCP KNOWN_OFF（区别于首次 ON 检查）。
 * ================================================================ */

bool safety_validate_active(const safety_inputs_t *inputs,
                             const safety_interlock_config_t *config,
                             safety_operation_t operation,
                             drum_position_t required_position,
                             safety_reject_reason_t *reason)
{
    if (!inputs || !config || !reason) {
        if (reason) *reason = SAFETY_REJECT_INVALID_PARAM;
        return false;
    }

    *reason = SAFETY_REJECT_NONE;

    /* 1. Fault/emergency check */
    if (inputs->fault_active) {
        *reason = SAFETY_REJECT_FAULT_ACTIVE;
        return false;
    }
    if (inputs->emergency_stop_active) {
        *reason = SAFETY_REJECT_EMERGENCY;
        return false;
    }

    /* 2. Position check (fresh + known + stable + correct) */
    if (!inputs->position_sample_valid) {
        *reason = SAFETY_REJECT_POSITION_UNKNOWN;
        return false;
    }
    if (config->position_stale_timeout_ms > 0) {
        uint32_t elapsed = (uint32_t)(inputs->now_ms - inputs->position_sample_ms);
        if (elapsed >= config->position_stale_timeout_ms) {
            *reason = SAFETY_REJECT_POSITION_STALE;
            return false;
        }
    }
    if (inputs->position == DRUM_POS_UNKNOWN) {
        *reason = SAFETY_REJECT_POSITION_UNKNOWN;
        return false;
    }
    if (!inputs->position_stable) {
        *reason = SAFETY_REJECT_POSITION_MISMATCH;
        return false;
    }
    if (inputs->position != required_position) {
        *reason = SAFETY_REJECT_POSITION_MISMATCH;
        return false;
    }

    /* 3. Motor not moving */
    if (inputs->position_motor_moving) {
        *reason = SAFETY_REJECT_POSITION_MOVING;
        return false;
    }

    /* 4. Target output must be KNOWN_ON */
    if (operation == SAFETY_OP_SOURCE_INLET_VALVE) {
        if (!safety_mcp_output_is_known(inputs->mcp_known_mask, SAFE_OUTPUT_TAP_VALVE)) {
            *reason = SAFETY_REJECT_OUTPUT_UNKNOWN;
            return false;
        }
        if (!inputs->mcp_outputs.source_inlet_valve) {
            *reason = SAFETY_REJECT_OUTPUT_UNKNOWN;
            return false;
        }
        /* 5. Conflicting outputs must be KNOWN_OFF */
        if (!safety_mcp_output_is_known(inputs->mcp_known_mask, SAFE_OUTPUT_TRANSFER_VALVE) ||
            inputs->mcp_outputs.transfer_valve) {
            *reason = SAFETY_REJECT_VALVE_CONFLICT;
            return false;
        }
        if (!safety_mcp_output_is_known(inputs->mcp_known_mask, SAFE_OUTPUT_DRAIN_VALVE) ||
            inputs->mcp_outputs.drain_valve) {
            *reason = SAFETY_REJECT_VALVE_CONFLICT;
            return false;
        }
    } else if (operation == SAFETY_OP_TRANSFER_VALVE) {
        if (!safety_mcp_output_is_known(inputs->mcp_known_mask, SAFE_OUTPUT_TRANSFER_VALVE)) {
            *reason = SAFETY_REJECT_OUTPUT_UNKNOWN;
            return false;
        }
        if (!inputs->mcp_outputs.transfer_valve) {
            *reason = SAFETY_REJECT_OUTPUT_UNKNOWN;
            return false;
        }
        if (!safety_mcp_output_is_known(inputs->mcp_known_mask, SAFE_OUTPUT_TAP_VALVE) ||
            inputs->mcp_outputs.source_inlet_valve) {
            *reason = SAFETY_REJECT_VALVE_CONFLICT;
            return false;
        }
        if (!safety_mcp_output_is_known(inputs->mcp_known_mask, SAFE_OUTPUT_DRAIN_VALVE) ||
            inputs->mcp_outputs.drain_valve) {
            *reason = SAFETY_REJECT_VALVE_CONFLICT;
            return false;
        }
    } else if (operation == SAFETY_OP_DRAIN_VALVE) {
        /* Drain: self KNOWN_ON, GPA0/GPA1 must be KNOWN_OFF */
        if (!safety_mcp_output_is_known(inputs->mcp_known_mask, SAFE_OUTPUT_DRAIN_VALVE)) {
            *reason = SAFETY_REJECT_OUTPUT_UNKNOWN;
            return false;
        }
        if (!inputs->mcp_outputs.drain_valve) {
            *reason = SAFETY_REJECT_OUTPUT_UNKNOWN;
            return false;
        }
        if (!safety_mcp_output_is_known(inputs->mcp_known_mask, SAFE_OUTPUT_TAP_VALVE) ||
            inputs->mcp_outputs.source_inlet_valve) {
            *reason = SAFETY_REJECT_VALVE_CONFLICT;
            return false;
        }
        if (!safety_mcp_output_is_known(inputs->mcp_known_mask, SAFE_OUTPUT_TRANSFER_VALVE) ||
            inputs->mcp_outputs.transfer_valve) {
            *reason = SAFETY_REJECT_VALVE_CONFLICT;
            return false;
        }
    } else if (operation == SAFETY_OP_DETERGENT_PUMP) {
        /* Detergent: self KNOWN_ON, other dangerous outputs must be KNOWN_OFF */
        if (!safety_mcp_output_is_known(inputs->mcp_known_mask, SAFE_OUTPUT_DETERGENT_PUMP)) {
            *reason = SAFETY_REJECT_OUTPUT_UNKNOWN;
            return false;
        }
        if (!inputs->mcp_outputs.detergent_pump) {
            *reason = SAFETY_REJECT_OUTPUT_UNKNOWN;
            return false;
        }
    } else if (operation == SAFETY_OP_UV) {
        /* UV: self KNOWN_ON, other dangerous outputs must be KNOWN_OFF */
        if (!safety_mcp_output_is_known(inputs->mcp_known_mask, SAFE_OUTPUT_UV)) {
            *reason = SAFETY_REJECT_OUTPUT_UNKNOWN;
            return false;
        }
        if (!inputs->mcp_outputs.uv) {
            *reason = SAFETY_REJECT_OUTPUT_UNKNOWN;
            return false;
        }
    } else {
        *reason = SAFETY_REJECT_INVALID_PARAM;
        return false;
    }

    return true;
}
