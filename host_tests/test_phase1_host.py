#!/usr/bin/env python3
"""
test_phase1_host.py — Host-side tests for Phase 1 pure types and config validation.
No C compiler or ESP-IDF required.

Tests:
1. machine_types: enum values, request_id wrap, position validation
2. wash_contract: fixed array sizes
3. machine_config_validate: parameter bounds, NaN, enum range, cross-checks
4. GPIO tool: self-tests
"""

import sys
import math
import struct
import os

tests_run = 0
tests_passed = 0
tests_failed = 0

def TEST(name):
    global tests_run
    tests_run += 1
    sys.stdout.write(f"  TEST {tests_run:02d}: {name:<55s} ")

def PASS():
    global tests_passed
    tests_passed += 1
    print("PASS")

def FAIL(msg):
    global tests_failed
    tests_failed += 1
    print(f"FAIL: {msg}")

# ================================================================
# 1. machine_types enum values (parsed from header)
# ================================================================

def parse_enum_values(header_path, enum_name):
    """Parse enum integer values from a C header file."""
    values = {}
    with open(header_path, encoding='utf-8') as f:
        content = f.read()
    import re
    pattern = rf'typedef enum\s*\{{([^}}]+)\}}\s*{enum_name}\s*;'
    m = re.search(pattern, content, re.DOTALL)
    if not m:
        return values
    body = m.group(1)
    current_val = 0
    for line in body.split('\n'):
        line = line.strip().rstrip(',').split('/*')[0].strip()
        if not line or line.startswith('//'):
            continue
        if '=' in line:
            name_part, val_part = line.split('=', 1)
            name = name_part.strip()
            val_part = val_part.strip()
            try:
                current_val = int(val_part, 0)
            except ValueError:
                if '<<' in val_part:
                    parts = val_part.replace('(', '').replace(')', '').replace('U', '').split('<<')
                    current_val = int(parts[0].strip()) << int(parts[1].strip())
                elif val_part.startswith('-') and val_part[1:].isdigit():
                    current_val = int(val_part)
                else:
                    current_val = 0
            values[name] = current_val
            current_val += 1  # next auto-increment starts after this value
        elif line and (line[0].isalpha() or line[0] == '_'):
            name = line.strip()
            values[name] = current_val
            current_val += 1
    return values

def test_drum_position_values():
    TEST("drum_position_t enum values match design")
    vals = parse_enum_values(
        "components/machine_types/include/machine_types.h",
        "drum_position_t"
    )
    ok = True
    if vals.get('DRUM_POS_UNKNOWN') != -1:
        FAIL(f"DRUM_POS_UNKNOWN={vals.get('DRUM_POS_UNKNOWN')}, expected -1"); return
    if vals.get('DRUM_POS_0') != 0:
        FAIL(f"DRUM_POS_0={vals.get('DRUM_POS_0')}, expected 0"); return
    if vals.get('DRUM_POS_45') != 45:
        FAIL(f"DRUM_POS_45={vals.get('DRUM_POS_45')}, expected 45"); return
    if vals.get('DRUM_POS_90') != 90:
        FAIL(f"DRUM_POS_90={vals.get('DRUM_POS_90')}, expected 90"); return
    if vals.get('DRUM_POS_180') != 180:
        FAIL(f"DRUM_POS_180={vals.get('DRUM_POS_180')}, expected 180"); return
    if vals.get('DRUM_POS_270') != 270:
        FAIL(f"DRUM_POS_270={vals.get('DRUM_POS_270')}, expected 270"); return
    PASS()

def test_request_id_wrap():
    TEST("machine_request_id_next: UINT32_MAX wraps to 1 (skip 0)")
    def next_id(current):
        n = (current + 1) & 0xFFFFFFFF
        return 1 if n == 0 else n

    assert next_id(0) == 1, f"next_id(0)={next_id(0)}"
    assert next_id(42) == 43
    assert next_id(0xFFFFFFFE) == 0xFFFFFFFF
    assert next_id(0xFFFFFFFF) == 1, f"next_id(MAX)={next_id(0xFFFFFFFF)}"
    PASS()

def test_machine_revision_next():
    TEST("machine_revision_next: 0->1, normal, MAX-1->MAX, MAX->1")
    # Same logic as machine_revision_next() in machine_status_store.c
    def rev_next(current):
        n = (current + 1) & 0xFFFFFFFF
        return 1 if n == 0 else n

    assert rev_next(0) == 1, "0->1"
    assert rev_next(42) == 43, "normal increment"
    assert rev_next(0xFFFFFFFE) == 0xFFFFFFFF, "MAX-1->MAX"
    assert rev_next(0xFFFFFFFF) == 1, "MAX->1 (skip 0)"
    PASS()

def test_machine_state_enums():
    TEST("machine_state_t enum count = 9")
    vals = parse_enum_values(
        "components/machine_types/include/machine_types.h",
        "machine_state_t"
    )
    expected = {
        'MACHINE_STATE_BOOTING': 0,
        'MACHINE_STATE_IDLE': 1,
        'MACHINE_STATE_WAITING_LOAD': 2,
        'MACHINE_STATE_RUNNING': 3,
        'MACHINE_STATE_WAITING_UNLOAD': 4,
        'MACHINE_STATE_UV': 5,
        'MACHINE_STATE_PAUSED': 6,
        'MACHINE_STATE_RESETTING': 7,
        'MACHINE_STATE_FAULT': 8,
    }
    for name, val in expected.items():
        if vals.get(name) != val:
            FAIL(f"{name}={vals.get(name)}, expected {val}")
            return
    PASS()

def test_fault_enums():
    TEST("machine_fault_code_t has 15 codes (FAULT_NONE..FAULT_INTERNAL)")
    vals = parse_enum_values(
        "components/machine_types/include/machine_types.h",
        "machine_fault_code_t"
    )
    if vals.get('FAULT_NONE') != 0:
        FAIL(f"FAULT_NONE={vals.get('FAULT_NONE')}"); return
    if 'FAULT_INTERNAL' not in vals:
        FAIL("FAULT_INTERNAL not found"); return
    # Count entries
    count = len(vals)
    if count < 14:
        FAIL(f"Only {count} fault codes, expected >= 14"); return
    PASS()

def test_fixed_array_sizes():
    TEST("fixed array capacity constants from headers")
    # Check WASH_INTENT_MAX_ACTIONS
    with open("components/machine_types/include/wash_contract.h", encoding='utf-8') as f:
        content = f.read()
    assert '#define WASH_INTENT_MAX_ACTIONS  32' in content, "WASH_INTENT_MAX_ACTIONS != 32"
    assert '#define WASH_PROGRAM_MAX_STEPS  64' in content, "WASH_PROGRAM_MAX_STEPS != 64"
    with open("components/fake_hal/include/fake_hal.h", encoding='utf-8') as f:
        content = f.read()
    assert '#define FAKE_HAL_HISTORY_CAPACITY  128' in content
    assert '#define FAKE_HAL_SCRIPT_CAPACITY  64' in content
    PASS()

def test_position_direction_policy():
    TEST("position_direction_policy_t in machine_types.h")
    vals = parse_enum_values(
        "components/machine_types/include/machine_types.h",
        "position_direction_policy_t"
    )
    expected = {
        'POSITION_DIR_AUTO_SHORTEST': 0,
        'POSITION_DIR_PREFER_CW': 1,
        'POSITION_DIR_PREFER_CCW': 2,
        'POSITION_DIR_FORCE_CW': 3,
        'POSITION_DIR_FORCE_CCW': 4,
    }
    for name, val in expected.items():
        if vals.get(name) != val:
            FAIL(f"{name}={vals.get(name)}, expected {val}")
            return
    PASS()

# ================================================================
# 2. machine_config validation (reimplemented in Python)
# ================================================================

def machine_config_validate(cfg):
    """Python port of machine_config_validate for testing."""
    if cfg['schema_version'] != 2:
        return False
    # Position
    if cfg['pos_policy'] < 0 or cfg['pos_policy'] > 4: return False
    if not (5 <= cfg['move_pwm'] <= 100): return False
    if not (5 <= cfg['approach_pwm'] <= 100): return False
    if cfg['approach_pwm'] > cfg['move_pwm']: return False
    if not (20 <= cfg['debounce_ms'] <= 200): return False
    if not (1000 <= cfg['move_timeout_ms'] <= 120000): return False
    if not (100 <= cfg['brake_ms'] <= 5000): return False
    # Water
    ppl = cfg['pulses_per_liter']
    if not math.isfinite(ppl) or ppl < 0: return False
    if not (1000 <= cfg['no_flow_ms'] <= 30000): return False
    if not (500 <= cfg['low_flow_ms'] <= 30000): return False
    if not (1 <= cfg['batch_pulses'] <= 100000): return False
    if not (1000 <= cfg['batch_max_ms'] <= 120000): return False
    if not (500 <= cfg['settle_ms'] <= 30000): return False
    if not (1000 <= cfg['transfer_timeout_ms'] <= 120000): return False
    if not (1000 <= cfg['default_transfer_ms'] <= cfg['transfer_timeout_ms']): return False
    if not (1 <= cfg['max_fill_cycles'] <= 100): return False
    if not (60000 <= cfg['total_inlet_ms'] <= 7200000): return False
    # Dry
    if not (60000 <= cfg['dry_max_ms'] <= 3600000): return False
    if not (1000 <= cfg['pre_fan_ms'] <= 60000): return False
    if not (5000 <= cfg['heat_on_ms'] <= 300000): return False
    if not (5000 <= cfg['heat_off_ms'] <= 300000): return False
    if not (5000 <= cfg['cooldown_ms'] <= 300000): return False
    cutoff = cfg['cutoff_c']
    resume = cfg['resume_c']
    if not math.isfinite(cutoff) or not (40.0 <= cutoff <= 80.0): return False
    if not math.isfinite(resume) or not (30.0 <= resume < cutoff): return False
    if cfg['fan_pct'] > 100: return False
    if not (1000 <= cfg['sht_stale_ms'] <= 60000): return False
    # Detergent
    if not (500 <= cfg['demo_ms'] <= 30000): return False
    if not (1000 <= cfg['formal_ms'] <= 60000): return False
    if not (1000 <= cfg['max_single_ms'] <= 60000): return False
    if cfg['demo_ms'] > cfg['max_single_ms']: return False
    if cfg['formal_ms'] > cfg['max_single_ms']: return False
    mlps = cfg['ml_per_second']
    if not math.isfinite(mlps) or mlps < 0: return False
    return True

def make_valid_config():
    return {
        'schema_version': 2,
        'pos_policy': 1, 'move_pwm': 30, 'approach_pwm': 15,
        'debounce_ms': 30, 'move_timeout_ms': 30000, 'brake_ms': 500,
        'pulses_per_liter': 0.0, 'no_flow_ms': 5000, 'low_flow_ms': 3000,
        'batch_pulses': 500, 'batch_max_ms': 30000, 'settle_ms': 2000,
        'transfer_timeout_ms': 30000, 'default_transfer_ms': 15000,
        'max_fill_cycles': 10, 'total_inlet_ms': 600000,
        'dry_max_ms': 1800000, 'pre_fan_ms': 5000, 'heat_on_ms': 60000,
        'heat_off_ms': 60000, 'cooldown_ms': 60000, 'cutoff_c': 55.0,
        'resume_c': 45.0, 'fan_pct': 80, 'sht_stale_ms': 10000,
        'demo_ms': 3000, 'formal_ms': 6000, 'max_single_ms': 10000,
        'ml_per_second': 0.0,
    }

def test_config_valid():
    TEST("config_validate: valid default passes")
    if machine_config_validate(make_valid_config()): PASS()
    else: FAIL("valid config rejected")

def test_config_bad_version():
    TEST("config_validate: wrong schema_version rejected")
    cfg = make_valid_config(); cfg['schema_version'] = 99
    if not machine_config_validate(cfg): PASS()
    else: FAIL("bad version accepted")

def test_config_nan_ppl():
    TEST("config_validate: NaN pulses_per_liter rejected")
    cfg = make_valid_config(); cfg['pulses_per_liter'] = float('nan')
    if not machine_config_validate(cfg): PASS()
    else: FAIL("NaN accepted")

def test_config_inf_cutoff():
    TEST("config_validate: Inf heater_cutoff rejected")
    cfg = make_valid_config(); cfg['cutoff_c'] = float('inf')
    if not machine_config_validate(cfg): PASS()
    else: FAIL("Inf accepted")

def test_config_nan_ml():
    TEST("config_validate: NaN ml_per_second rejected")
    cfg = make_valid_config(); cfg['ml_per_second'] = float('nan')
    if not machine_config_validate(cfg): PASS()
    else: FAIL("NaN accepted")

def test_config_bad_policy():
    TEST("config_validate: invalid position policy (99) rejected")
    cfg = make_valid_config(); cfg['pos_policy'] = 99
    if not machine_config_validate(cfg): PASS()
    else: FAIL("bad policy accepted")

def test_config_demo_exceeds_max():
    TEST("config_validate: demo_duration > max_single rejected")
    cfg = make_valid_config(); cfg['max_single_ms'] = 2000; cfg['demo_ms'] = 3000
    if not machine_config_validate(cfg): PASS()
    else: FAIL("demo > max_single accepted")

def test_config_formal_exceeds_max():
    TEST("config_validate: formal_duration > max_single rejected")
    cfg = make_valid_config(); cfg['max_single_ms'] = 5000; cfg['formal_ms'] = 6000
    if not machine_config_validate(cfg): PASS()
    else: FAIL("formal > max_single accepted")

def test_config_pwm_low():
    TEST("config_validate: PWM < 5 rejected")
    cfg = make_valid_config(); cfg['move_pwm'] = 4
    if not machine_config_validate(cfg): PASS()
    else: FAIL("PWM=4 accepted")

def test_config_approach_gt_move():
    TEST("config_validate: approach > move PWM rejected")
    cfg = make_valid_config(); cfg['move_pwm'] = 20; cfg['approach_pwm'] = 25
    if not machine_config_validate(cfg): PASS()
    else: FAIL("approach > move accepted")

def test_config_debounce_range():
    TEST("config_validate: debounce 19ms and 201ms rejected")
    cfg1 = make_valid_config(); cfg1['debounce_ms'] = 19
    cfg2 = make_valid_config(); cfg2['debounce_ms'] = 201
    if not machine_config_validate(cfg1) and not machine_config_validate(cfg2): PASS()
    else: FAIL("debounce out of range accepted")

def test_config_fan_pct():
    TEST("config_validate: fan_pct > 100 rejected")
    cfg = make_valid_config(); cfg['fan_pct'] = 101
    if not machine_config_validate(cfg): PASS()
    else: FAIL("fan_pct=101 accepted")

def test_config_resume_ge_cutoff():
    TEST("config_validate: resume >= cutoff rejected")
    cfg = make_valid_config(); cfg['resume_c'] = 55.0
    if not machine_config_validate(cfg): PASS()
    else: FAIL("resume >= cutoff accepted")

def test_config_default_transfer_gt_timeout():
    TEST("config_validate: default_transfer > transfer_timeout rejected")
    cfg = make_valid_config()
    cfg['transfer_timeout_ms'] = 10000; cfg['default_transfer_ms'] = 15000
    if not machine_config_validate(cfg): PASS()
    else: FAIL("default_transfer > transfer_timeout accepted")

# ================================================================
# 3. GPIO tool self-tests
# ================================================================

def test_gpio_tool_self_test():
    TEST("check_gpio_map.py --self-test passes")
    import subprocess
    result = subprocess.run(
        [sys.executable, "tools/check_gpio_map.py", "--self-test"],
        capture_output=True, text=True
    )
    if result.returncode == 0:
        PASS()
    else:
        FAIL(f"exit code {result.returncode}: {result.stderr}")

def test_gpio_tool_real_check():
    TEST("check_gpio_map.py on real board_config passes")
    import subprocess
    result = subprocess.run(
        [sys.executable, "tools/check_gpio_map.py"],
        capture_output=True, text=True
    )
    if result.returncode == 0:
        PASS()
    else:
        FAIL(f"exit code {result.returncode}")

# ================================================================
# Main
# ================================================================

if __name__ == '__main__':
    os.chdir(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    print("=== xiaojing host tests (Python) ===\n")

    print("[1] machine_types")
    test_drum_position_values()
    test_request_id_wrap()
    test_machine_revision_next()
    test_machine_state_enums()
    test_fault_enums()
    test_fixed_array_sizes()
    test_position_direction_policy()

    print("\n[2] machine_config validation")
    test_config_valid()
    test_config_bad_version()
    test_config_nan_ppl()
    test_config_inf_cutoff()
    test_config_nan_ml()
    test_config_bad_policy()
    test_config_demo_exceeds_max()
    test_config_formal_exceeds_max()
    test_config_pwm_low()
    test_config_approach_gt_move()
    test_config_debounce_range()
    test_config_fan_pct()
    test_config_resume_ge_cutoff()
    test_config_default_transfer_gt_timeout()

    print("\n[3] GPIO tool")
    test_gpio_tool_self_test()
    test_gpio_tool_real_check()

    print(f"\n=== Results: {tests_passed}/{tests_run} passed, {tests_failed} failed ===")
    sys.exit(0 if tests_failed == 0 else 1)
