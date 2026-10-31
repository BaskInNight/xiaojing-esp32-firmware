/*
 * machine_config_validate.c — 纯 C 参数校验
 * 无 NVS/FreeRTOS/ESP-IDF 依赖，固件和 host test 共同编译
 */

#include <math.h>
#include "machine_config.h"

bool machine_config_validate(const machine_config_t *cfg)
{
    if (!cfg) return false;
    if (cfg->schema_version != MACHINE_CONFIG_SCHEMA_VERSION) return false;

    /* 姿态 — 枚举范围 */
    int pos_policy = (int)cfg->position.default_policy;
    if (pos_policy < 0 || pos_policy > (int)POSITION_DIR_MAX_VALID_FOR_CONFIG) return false;
    if (cfg->position.move_pwm_percent < 5 || cfg->position.move_pwm_percent > 100) return false;
    if (cfg->position.approach_pwm_percent < 5 || cfg->position.approach_pwm_percent > 100) return false;
    if (cfg->position.approach_pwm_percent > cfg->position.move_pwm_percent) return false;
    if (cfg->position.debounce_ms < 20 || cfg->position.debounce_ms > 200) return false;
    if (cfg->position.move_timeout_ms < 1000 || cfg->position.move_timeout_ms > 120000) return false;
    if (cfg->position.brake_ms < 100 || cfg->position.brake_ms > 5000) return false;

    /* 进水 — float 必须 isfinite 且 >= 0 */
    if (!isfinite(cfg->water.pulses_per_liter) || cfg->water.pulses_per_liter < 0.0f) return false;
    if (cfg->water.no_flow_timeout_ms < 1000 || cfg->water.no_flow_timeout_ms > 30000) return false;
    if (cfg->water.low_flow_window_ms < 500 || cfg->water.low_flow_window_ms > 30000) return false;
    if (cfg->water.source_batch_target_pulses < 1 || cfg->water.source_batch_target_pulses > 100000) return false;
    if (cfg->water.source_batch_max_ms < 1000 || cfg->water.source_batch_max_ms > 120000) return false;
    if (cfg->water.source_settle_ms < 500 || cfg->water.source_settle_ms > 30000) return false;
    if (cfg->water.transfer_timeout_ms < 1000 || cfg->water.transfer_timeout_ms > 120000) return false;
    if (cfg->water.default_transfer_ms < 1000 || cfg->water.default_transfer_ms > cfg->water.transfer_timeout_ms) return false;
    if (cfg->water.max_fill_cycles < 1 || cfg->water.max_fill_cycles > 100) return false;
    if (cfg->water.total_inlet_timeout_ms < 60000 || cfg->water.total_inlet_timeout_ms > 7200000) return false;

    /* 烘干 — float 必须 isfinite */
    if (cfg->dry.max_total_ms < 60000 || cfg->dry.max_total_ms > 3600000) return false;
    if (cfg->dry.pre_fan_ms < 1000 || cfg->dry.pre_fan_ms > 60000) return false;
    if (cfg->dry.heat_on_max_ms < 5000 || cfg->dry.heat_on_max_ms > 300000) return false;
    if (cfg->dry.heat_off_min_ms < 5000 || cfg->dry.heat_off_min_ms > 300000) return false;
    if (cfg->dry.cooldown_ms < 5000 || cfg->dry.cooldown_ms > 300000) return false;
    if (!isfinite(cfg->dry.heater_cutoff_c) || cfg->dry.heater_cutoff_c < 40.0f || cfg->dry.heater_cutoff_c > 80.0f) return false;
    if (!isfinite(cfg->dry.heater_resume_c) || cfg->dry.heater_resume_c < 30.0f || cfg->dry.heater_resume_c >= cfg->dry.heater_cutoff_c) return false;
    if (cfg->dry.fan_percent > 100) return false;
    if (cfg->dry.sht_stale_timeout_ms < 1000 || cfg->dry.sht_stale_timeout_ms > 60000) return false;

    /* 洗衣液 — 交叉校验: demo/formal 不超过 max_single_ms */
    if (cfg->detergent.demo_duration_ms < 500 || cfg->detergent.demo_duration_ms > 30000) return false;
    if (cfg->detergent.formal_duration_ms < 1000 || cfg->detergent.formal_duration_ms > 60000) return false;
    if (cfg->detergent.max_single_ms < 1000 || cfg->detergent.max_single_ms > 60000) return false;
    if (cfg->detergent.demo_duration_ms > cfg->detergent.max_single_ms) return false;
    if (cfg->detergent.formal_duration_ms > cfg->detergent.max_single_ms) return false;
    if (!isfinite(cfg->detergent.ml_per_second) || cfg->detergent.ml_per_second < 0.0f) return false;

    /* UV — default <= max */
    if (cfg->uv.default_duration_ms < 1000 || cfg->uv.default_duration_ms > 1800000) return false;
    if (cfg->uv.max_duration_ms < 1000 || cfg->uv.max_duration_ms > 1800000) return false;
    if (cfg->uv.default_duration_ms > cfg->uv.max_duration_ms) return false;

    /* 排水 */
    if (cfg->drain.max_duration_ms < 1000 || cfg->drain.max_duration_ms > 300000) return false;

    /* 板测诊断参数（V4） */
    if (cfg->benchtst.bl50_pwm > 100) return false;
    if (cfg->benchtst.ibt2_pwm > 100) return false;
    if (cfg->benchtst.act_duration_ms == 0 ||
        cfg->benchtst.act_duration_ms > 10000U) return false;

    return true;
}

void machine_config_apply_direction_calibration(machine_config_t *cfg,
                                                bool rpwm_is_cw)
{
    if (!cfg) return;
    cfg->position.direction_calibrated = true;
    cfg->position.rpwm_is_cw = rpwm_is_cw;
}

void machine_config_apply_bl50_direction_reverse(machine_config_t *cfg,
                                                 bool reverse)
{
    if (!cfg) return;
    cfg->position.bl50_reverse_dir = reverse;
}

void machine_config_apply_benchtst_defaults(machine_config_t *cfg)
{
    if (!cfg) return;
    cfg->benchtst.bl50_pwm = 40U;
    cfg->benchtst.ibt2_pwm = 80U;
    cfg->benchtst.act_duration_ms = 1500U;
}
