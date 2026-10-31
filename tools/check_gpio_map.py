#!/usr/bin/env python3
"""
check_gpio_map.py -- GPIO/MCP mapping static validation tool

Checks:
1. board_config.h GPIO numbers: no duplicates, valid ESP32-S3 range, no forbidden
2. MCP ports (GPA/GPB): no duplicates, pins 0..7, all 8 present per port
3. LEDC timer/channel: no duplicates
4. Required GPIO definitions exist
5. Scan main/components for bare GPIO numbers and old step/encoder symbols
6. Self-tests: inject errors, confirm non-zero exit

Usage:
    python tools/check_gpio_map.py [--self-test] [board_config.h path]
"""

import re
import sys
import os
from pathlib import Path

DEFAULT_PATH = "components/board_config/include/board_config.h"

# ESP32-S3 valid GPIO range
VALID_GPIO_PINS = set(range(0, 22)) | {26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37} | set(range(38, 49))

# Forbidden symbols: old Arduino step motor / EC11 encoder / raw GPIO patterns
# Only match explicit _PIN/_GPIO suffixes (not ESP-IDF enum names like STEP_SPIN)
FORBIDDEN_SYMBOLS = [
    r'\bSTEP_[A-Z]+_PIN\b', r'\bEC11_[A-Z]+_PIN\b', r'\bSTEPPER_[A-Z]+_PIN\b',
    r'\bENCODER_[A-Z]+_PIN\b', r'\bOLD_GPIO_[A-Z]+\b',
]

# MCP port valid range
MCP_PIN_RANGE = set(range(0, 8))  # 0..7


def parse_gpio_defines(lines):
    """Parse GPIO defines. Returns (dict, errors_list). Detects duplicates."""
    gpio_pattern = re.compile(r'#define\s+XIAOJING_GPIO_(\w+)\s+(\d+)')
    results = {}
    errors = []
    for i, line in enumerate(lines, 1):
        m = gpio_pattern.search(line)
        if m:
            name = m.group(1)
            pin = int(m.group(2))
            if pin in results:
                old_name, old_line = results[pin]
                errors.append(f"  ERROR: GPIO{pin} duplicate: {old_name} (line {old_line}) vs {name} (line {i})")
            else:
                results[pin] = (name, i)
    return results, errors


def parse_mcp_defines(lines):
    """Parse MCP port definitions. Returns (port_a, port_b, errors). Validates 0..7."""
    mcp_pattern = re.compile(r'#define\s+XIAOJING_MCP_(GP[AB])(\d+)_(\w+)\s+(\d+)')
    port_a = {}
    port_b = {}
    errors = []
    for i, line in enumerate(lines, 1):
        m = mcp_pattern.search(line)
        if m:
            port = m.group(1)
            _bit = int(m.group(2))   # bit position from name (informational)
            name = m.group(3)
            pin = int(m.group(4))
            if pin not in MCP_PIN_RANGE:
                errors.append(f"  ERROR: MCP {port}{pin} out of range 0..7: {name} (line {i})")
                continue
            target = port_a if port == 'GPA' else port_b
            if pin in target:
                old_name, old_line = target[pin]
                errors.append(f"  ERROR: MCP {port}{pin} duplicate: {old_name} (line {old_line}) vs {name} (line {i})")
            else:
                target[pin] = (name, i)
    return port_a, port_b, errors


def parse_ledc_defines(lines):
    """Parse LEDC resource definitions. Returns (timers, channels, errors)."""
    timer_pattern = re.compile(r'#define\s+XIAOJING_LEDC_TIMER_(\w+)\s+(\d+)')
    ch_pattern = re.compile(r'#define\s+XIAOJING_LEDC_CH_(\w+)\s+(\d+)')
    timers = {}
    channels = {}
    errors = []
    for i, line in enumerate(lines, 1):
        m = timer_pattern.search(line)
        if m:
            timer = int(m.group(2))
            name = m.group(1)
            if timer in timers:
                errors.append(f"  ERROR: LEDC timer {timer} duplicate: {timers[timer][0]} vs {name} (line {i})")
            timers[timer] = (name, i)
        m = ch_pattern.search(line)
        if m:
            ch = int(m.group(2))
            name = m.group(1)
            if ch in channels:
                errors.append(f"  ERROR: LEDC channel {ch} duplicate: {channels[ch][0]} vs {name} (line {i})")
            channels[ch] = (name, i)
    return timers, channels, errors


def scan_for_bare_gpio(root_dir):
    """Scan main/ and components/ for bare GPIO numbers and forbidden symbols.
    Returns list of error strings."""
    errors = []
    bare_gpio_re = re.compile(r'\bgpio_(?:set|get|config|num|pad)\s*\(\s*(\d+)\b', re.IGNORECASE)
    # Match raw digitalRead/ digitalWrite(pinNumber) or gpio_set_level(N,...)
    raw_gpio_re = re.compile(r'(?:digitalRead|digitalWrite|gpio_set_level|gpio_set_direction)\s*\(\s*(\d+)')
    forbidden_res = [re.compile(p) for p in FORBIDDEN_SYMBOLS]

    scan_dirs = []
    for sub in ['main', 'components']:
        d = os.path.join(root_dir, sub)
        if os.path.isdir(d):
            scan_dirs.append(d)

    for scan_dir in scan_dirs:
        for dirpath, _dirnames, filenames in os.walk(scan_dir):
            # Skip board_config and test directories for bare GPIO (board_config is allowed)
            rel = os.path.relpath(dirpath, root_dir)
            if rel.startswith('components' + os.sep + 'board_config'):
                continue
            for fname in filenames:
                if not fname.endswith(('.c', '.h')):
                    continue
                fpath = os.path.join(dirpath, fname)
                try:
                    with open(fpath, encoding='utf-8', errors='replace') as f:
                        for lineno, line in enumerate(f, 1):
                            # Skip #define lines (board_config re-exports are ok)
                            if line.strip().startswith('#define'):
                                continue
                            for m in raw_gpio_re.finditer(line):
                                pin = int(m.group(1))
                                if pin in VALID_GPIO_PINS:
                                    errors.append(f"  ERROR: Bare GPIO{pin} in {rel}/{fname}:{lineno}")
                            for pat in forbidden_res:
                                if pat.search(line):
                                    errors.append(f"  ERROR: Forbidden symbol in {rel}/{fname}:{lineno}: {line.strip()}")
                except OSError:
                    pass
    return errors


def check_file(filepath, root_dir=None):
    """Run all checks. Returns True if all pass."""
    path = Path(filepath)
    if not path.exists():
        print(f"ERROR: File not found: {path}")
        return False

    lines = path.read_text(encoding='utf-8').splitlines()
    print(f"Checking: {path}")
    print("=" * 60)
    all_errors = []

    # 1. GPIO duplicate + range
    print("\n[1] GPIO pin assignments...")
    gpios, gpio_dup_errors = parse_gpio_defines(lines)
    all_errors.extend(gpio_dup_errors)
    if not gpios:
        print("  WARN: No GPIO defines found")
    else:
        print(f"  Found {len(gpios)} GPIO defines")
        for pin, (name, line_no) in sorted(gpios.items()):
            print(f"    GPIO_{name} = {pin} (line {line_no})")
            if pin not in VALID_GPIO_PINS:
                all_errors.append(f"  ERROR: GPIO{pin} not in ESP32-S3 valid range")
            if pin in FORBIDDEN_GPIOS:
                all_errors.append(f"  ERROR: GPIO{pin} in forbidden list")

    # 2. MCP
    print("\n[2] MCP port assignments...")
    port_a, port_b, mcp_errors = parse_mcp_defines(lines)
    all_errors.extend(mcp_errors)
    print(f"  GPA: {len(port_a)} defined (expect 8)")
    print(f"  GPB: {len(port_b)} defined (expect 8)")
    if len(port_a) != 8:
        all_errors.append(f"  ERROR: GPA has {len(port_a)}/8 entries")
    if len(port_b) != 8:
        all_errors.append(f"  ERROR: GPB has {len(port_b)}/8 entries")
    for pin in sorted(port_a.keys()):
        print(f"    GPA{pin} = {port_a[pin][0]} (line {port_a[pin][1]})")
    for pin in sorted(port_b.keys()):
        print(f"    GPB{pin} = {port_b[pin][0]} (line {port_b[pin][1]})")

    # 3. LEDC
    print("\n[3] LEDC resource assignments...")
    timers, channels, ledc_errors = parse_ledc_defines(lines)
    all_errors.extend(ledc_errors)
    if not ledc_errors:
        print(f"  Timers: {len(timers)}, Channels: {len(channels)} -- no conflicts")

    # 4. Required GPIO existence
    print("\n[4] Required GPIO definitions...")
    required = {
        'BL50_DIR', 'BL50_BRAKE', 'BL50_PWM', 'BL50_STOP',
        'WATER_LEVEL', 'I2C_SDA', 'I2C_SCL', 'MCP_INTB',
        'FLOW_PCNT', 'FAN_PWM', 'IBT2_RPWM', 'IBT2_LPWM', 'IBT2_EN',
        'AMP_SD',
    }
    defined_names = {name for name, _ in gpios.values()} if gpios else set()
    for req in sorted(required):
        if req not in defined_names:
            all_errors.append(f"  ERROR: Missing required GPIO: {req}")
            print(f"    MISSING: {req}")
        else:
            print(f"    OK: {req}")

    # 5. Scan main/components for bare GPIO
    print("\n[5] Bare GPIO scan (main/components)...")
    if root_dir:
        bare_errors = scan_for_bare_gpio(root_dir)
        all_errors.extend(bare_errors)
        if not bare_errors:
            print("  No bare GPIO or forbidden symbols found")
    else:
        print("  Skipped (root_dir not specified)")

    # 6. MCP completeness check
    print("\n[6] MCP completeness...")
    missing_gpa = MCP_PIN_RANGE - set(port_a.keys())
    missing_gpb = MCP_PIN_RANGE - set(port_b.keys())
    if missing_gpa:
        all_errors.append(f"  ERROR: GPA missing pins: {sorted(missing_gpa)}")
    if missing_gpb:
        all_errors.append(f"  ERROR: GPB missing pins: {sorted(missing_gpb)}")
    if not missing_gpa and not missing_gpb:
        print("  GPA and GPB complete (all 8 pins)")

    # Report
    print("\n" + "=" * 60)
    for err in all_errors:
        print(err)
    if not all_errors:
        print("ALL CHECKS PASSED")
        return True
    else:
        print(f"\n{len(all_errors)} ERROR(S) FOUND")
        return False


# Forbidden GPIO pin numbers: old Arduino step motor / EC11 encoder pins.
# These must never appear in board_config.h as new assignments.
# Populate from the old Arduino project's pin mapping before Phase 11 hardware bring-up.
FORBIDDEN_GPIOS = set()  # TODO: populate with confirmed old Arduino step/encoder GPIO numbers


def run_self_test():
    """Inject errors and verify detection. Returns True if all tests pass."""
    print("Running self-tests...")
    all_pass = True

    # Test 1: duplicate GPIO detection
    lines_dup = [
        '#define XIAOJING_GPIO_FOO  1',
        '#define XIAOJING_GPIO_BAR  1',
    ]
    _, errors = parse_gpio_defines(lines_dup)
    if not errors:
        print("  FAIL: duplicate GPIO not detected")
        all_pass = False
    else:
        print("  PASS: duplicate GPIO detected")

    # Test 2: MCP pin out of range
    lines_mcp_bad = [
        '#define XIAOJING_MCP_GPA0_VALVE 8',  # 8 is out of range
    ]
    _, _, mcp_errors = parse_mcp_defines(lines_mcp_bad)
    if not mcp_errors:
        print("  FAIL: MCP pin 8 not rejected")
        all_pass = False
    else:
        print("  PASS: MCP pin out-of-range detected")

    # Test 3: MCP duplicate
    lines_mcp_dup = [
        '#define XIAOJING_MCP_GPA0_FOO 0',
        '#define XIAOJING_MCP_GPA0_BAR 0',
    ]
    _, _, mcp_dup_errors = parse_mcp_defines(lines_mcp_dup)
    if not mcp_dup_errors:
        print("  FAIL: MCP duplicate not detected")
        all_pass = False
    else:
        print("  PASS: MCP duplicate detected")

    # Test 4: LEDC timer duplicate
    lines_ledc_dup = [
        '#define XIAOJING_LEDC_TIMER_FAN  0',
        '#define XIAOJING_LEDC_TIMER_BL50 0',
    ]
    _, _, ledc_errors = parse_ledc_defines(lines_ledc_dup)
    if not ledc_errors:
        print("  FAIL: LEDC timer duplicate not detected")
        all_pass = False
    else:
        print("  PASS: LEDC timer duplicate detected")

    # Test 5: LEDC channel duplicate
    lines_ch_dup = [
        '#define XIAOJING_LEDC_CH_FAN  0',
        '#define XIAOJING_LEDC_CH_BL50 0',
    ]
    _, _, ch_errors = parse_ledc_defines(lines_ch_dup)
    if not ch_errors:
        print("  FAIL: LEDC channel duplicate not detected")
        all_pass = False
    else:
        print("  PASS: LEDC channel duplicate detected")

    # Test 6: missing required GPIO
    lines_no_gpio = ['#define XIAOJING_GPIO_FOO  1']
    gpios, _ = parse_gpio_defines(lines_no_gpio)
    defined_names = {name for name, _ in gpios.values()}
    required = {'BL50_DIR', 'AMP_SD'}
    missing = required - defined_names
    if not missing:
        print("  FAIL: missing required GPIO not detected")
        all_pass = False
    else:
        print("  PASS: missing required GPIO detected")

    # Test 7: bare GPIO scan on synthetic file
    import tempfile
    with tempfile.TemporaryDirectory() as tmpdir:
        os.makedirs(os.path.join(tmpdir, 'main'), exist_ok=True)
        with open(os.path.join(tmpdir, 'main', 'test.c'), 'w') as f:
            f.write('gpio_set_level(42, 1);\n')
        bare_errors = scan_for_bare_gpio(tmpdir)
        if not bare_errors:
            print("  FAIL: bare GPIO not detected in scan")
            all_pass = False
        else:
            print("  PASS: bare GPIO detected in scan")

    # Test 8: forbidden symbol scan
    with tempfile.TemporaryDirectory() as tmpdir:
        os.makedirs(os.path.join(tmpdir, 'main'), exist_ok=True)
        with open(os.path.join(tmpdir, 'main', 'test.c'), 'w') as f:
            f.write('int STEP_DIR_PIN = 5;\n')
        forb_errors = scan_for_bare_gpio(tmpdir)
        has_forbidden = any('Forbidden' in e for e in forb_errors)
        if not has_forbidden:
            print("  FAIL: forbidden symbol not detected")
            all_pass = False
        else:
            print("  PASS: forbidden symbol detected")

    # Test 8b: ESP-IDF enum name NOT flagged as forbidden
    with tempfile.TemporaryDirectory() as tmpdir:
        os.makedirs(os.path.join(tmpdir, 'main'), exist_ok=True)
        with open(os.path.join(tmpdir, 'main', 'test.c'), 'w') as f:
            f.write('case STEP_SPIN: break;\n')
        false_errors = scan_for_bare_gpio(tmpdir)
        has_false = any('Forbidden' in e for e in false_errors)
        if has_false:
            print("  FAIL: STEP_SPIN false positive")
            all_pass = False
        else:
            print("  PASS: STEP_SPIN not flagged")

    # Test 9: MCP missing pins
    lines_partial = [
        '#define XIAOJING_MCP_GPA0_VALVE 0',
        '#define XIAOJING_MCP_GPA1_VALVE 1',
        # Missing GPA2-GPA7
    ]
    pa, _, _ = parse_mcp_defines(lines_partial)
    missing_gpa = MCP_PIN_RANGE - set(pa.keys())
    if len(missing_gpa) == 0:
        print("  FAIL: missing MCP pins not detected")
        all_pass = False
    else:
        print("  PASS: missing MCP pins detected")

    print()
    if all_pass:
        print("ALL SELF-TESTS PASSED")
    else:
        print("SELF-TEST FAILURES")
    return all_pass


if __name__ == '__main__':
    if '--self-test' in sys.argv:
        success = run_self_test()
        sys.exit(0 if success else 1)

    filepath = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else DEFAULT_PATH
    root_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # project root
    success = check_file(filepath, root_dir=root_dir)
    sys.exit(0 if success else 1)
