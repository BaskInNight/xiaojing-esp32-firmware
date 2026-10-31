#include <stdio.h>
#include <string.h>
#include "wash_planner.h"
#include "machine_config.h"

static machine_config_t make_valid_config(void) {
    machine_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.schema_version = MACHINE_CONFIG_SCHEMA_VERSION;
    cfg.position.default_policy = POSITION_DIR_PREFER_CW;
    cfg.position.move_pwm_percent = 30;
    cfg.position.approach_pwm_percent = 15;
    cfg.position.debounce_ms = 30;
    cfg.position.move_timeout_ms = 30000;
    cfg.position.brake_ms = 500;
    cfg.water.pulses_per_liter = 0.0f;
    cfg.water.no_flow_timeout_ms = 5000;
    cfg.water.low_flow_window_ms = 3000;
    cfg.water.source_batch_target_pulses = 500;
    cfg.water.source_batch_max_ms = 30000;
    cfg.water.source_settle_ms = 2000;
    cfg.water.transfer_timeout_ms = 30000;
    cfg.water.default_transfer_ms = 15000;
    cfg.water.max_fill_cycles = 10;
    cfg.water.total_inlet_timeout_ms = 600000;
    cfg.dry.max_total_ms = 1800000;
    cfg.dry.pre_fan_ms = 5000;
    cfg.dry.heat_on_max_ms = 60000;
    cfg.dry.heat_off_min_ms = 60000;
    cfg.dry.cooldown_ms = 60000;
    cfg.dry.heater_cutoff_c = 55.0f;
    cfg.dry.heater_resume_c = 45.0f;
    cfg.dry.fan_percent = 80;
    cfg.dry.sht_stale_timeout_ms = 10000;
    cfg.drain.max_duration_ms = 50000;
    cfg.detergent.formal_duration_ms = 6000;
    cfg.uv.default_duration_ms = 600000;
    return cfg;
}

int main(void) {
    machine_config_t cfg = make_valid_config();
    
    /* Test WATER */
    { wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.action_count = 1;
      intent.actions[0].type = WASH_ACTION_WATER_IN;
      intent.actions[0].enabled = true;
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      printf("WATER: result=%d steps=%zu\n", r.result, prog.step_count);
      for (size_t i = 0; i < prog.step_count; i++) {
          printf("  [%zu] type=%d timeout=%u\n", i, prog.steps[i].type, prog.steps[i].timeout_ms);
      }
    }
    
    /* Test DRAIN */
    { wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.action_count = 1;
      intent.actions[0].type = WASH_ACTION_DRAIN;
      intent.actions[0].enabled = true;
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      printf("DRAIN: result=%d steps=%zu\n", r.result, prog.step_count);
      for (size_t i = 0; i < prog.step_count; i++) {
          printf("  [%zu] type=%d timeout=%u dur=%u\n", i, prog.steps[i].type, prog.steps[i].timeout_ms, prog.steps[i].duration_ms);
      }
    }
    
    /* Test DRY */
    { wash_intent_t intent; memset(&intent, 0, sizeof(intent));
      intent.kind = WASH_PROGRAM_CUSTOM;
      intent.allow_dry = true;
      intent.action_count = 1;
      intent.actions[0].type = WASH_ACTION_DRY;
      intent.actions[0].enabled = true;
      intent.actions[0].duration_ms = 60000;
      wash_program_t prog;
      planner_report_t r = wash_planner_compile(&intent, &cfg, 1, &prog);
      printf("DRY: result=%d steps=%zu\n", r.result, prog.step_count);
      for (size_t i = 0; i < prog.step_count; i++) {
          printf("  [%zu] type=%d timeout=%u dur=%u\n", i, prog.steps[i].type, prog.steps[i].timeout_ms, prog.steps[i].duration_ms);
      }
    }
    
    return 0;
}
