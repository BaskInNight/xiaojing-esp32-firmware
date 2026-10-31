#!/usr/bin/env python3
"""
check_safety_bypass.py — 静态旁路扫描
扫描生产代码中未经许可直接调用高风险输出的旁路

Exit codes:
  0 = no violations
  1 = violations found
  2 = script error
"""

import os
import re
import sys
from pathlib import Path

# ---- Configuration ----

# High-risk function patterns to scan for
HIGH_RISK_FUNCTIONS = [
    r'\bset_safe_output\b',
    r'\bset_ibt2\b',
    r'\bstop_ibt2\b',
    r'\bset_bl50\b',
    r'\bstop_bl50\b',
    r'\bset_fan_percent\b',
    r'\bemergency_shutdown\b',
]

# MCP raw output patterns
MCP_RAW_OUTPUTS = [
    r'\bmcp23017_set_output\b',
    r'\bGPA0\b.*\bset\b',
    r'\bGPA1\b.*\bset\b',
    r'\bGPA2\b.*\bset\b',
    r'\bGPA3\b.*\bset\b',
    r'\bGPA7\b.*\bset\b',
    r'\bGPB7\b.*\bset\b',
]

# Whitelist: files/directories that are allowed to call high-risk functions
# Uses forward slashes, relative to project root
WHITELIST_PATTERNS = [
    # HAL implementations (specific files that implement the interface)
    r'components/real_hal/real_hal\.c',
    r'components/fake_hal/fake_hal\.c',
    # Safety manager (authorized gateway - specific files)
    r'components/safety_manager/safety_manager\.c',
    r'components/safety_manager/safety_interlocks\.c',
    # Low-level drivers (they implement the hardware interface)
    r'components/ibt2_driver/',
    r'components/bl50_driver/',
    r'components/fan_driver/',
    r'components/mcp23017_driver/',
    # HAL contract (type definitions only)
    r'components/hal_contract/',
    # Test files
    r'test_apps/',
    r'host_tests/',
    r'tools/',
    # Design docs
    r'docs/',
    # Build output
    r'build/',
    r'managed_components/',
]

# Files that are known bypasses (documented, not hidden)
KNOWN_BYPASSES = {
    'components/position_service/position_service.c': {
        'functions': ['set_ibt2', 'stop_ibt2', 'emergency_shutdown'],
        'reason': 'ACTUATOR_MIGRATION_PENDING: position_service retains direct IBT-2 access per design doc. '
                  'Will migrate when water_service/dry_service are built. '
                  'Safety_manager retains emergency stop path via registered stop hook.',
    },
}

# ---- Scanner ----

def normalize_path(path):
    """Normalize path to forward slashes for consistent matching."""
    return path.replace('\\', '/')

def is_whitelisted(rel_path):
    """Check if a file path matches any whitelist pattern."""
    normalized = normalize_path(rel_path)
    for pattern in WHITELIST_PATTERNS:
        if re.search(pattern, normalized):
            return True
    return False

def is_known_bypass(rel_path):
    """Check if a file is a documented known bypass."""
    normalized = normalize_path(rel_path)
    for pattern in KNOWN_BYPASSES:
        if normalized.endswith(pattern) or normalized == pattern:
            return True
    return False

def scan_file(file_path, project_root):
    """Scan a single C/H file for high-risk function calls."""
    rel_path = os.path.relpath(file_path, project_root)
    violations = []

    if is_whitelisted(rel_path):
        return violations

    try:
        with open(file_path, 'r', encoding='utf-8', errors='ignore') as f:
            lines = f.readlines()
    except Exception as e:
        print(f"WARNING: Could not read {file_path}: {e}", file=sys.stderr)
        return violations

    for line_num, line in enumerate(lines, 1):
        # Skip comments
        stripped = line.strip()
        if stripped.startswith('//') or stripped.startswith('/*') or stripped.startswith('*'):
            continue

        for pattern in HIGH_RISK_FUNCTIONS:
            if re.search(pattern, line):
                violations.append({
                    'file': rel_path,
                    'line': line_num,
                    'pattern': pattern.strip('\\b'),
                    'content': stripped[:120],
                    'known_bypass': is_known_bypass(rel_path),
                })

    return violations

def scan_project(project_root):
    """Scan all C/H files in the project."""
    all_violations = []
    scan_dirs = ['components', 'main']

    for scan_dir in scan_dirs:
        dir_path = os.path.join(project_root, scan_dir)
        if not os.path.isdir(dir_path):
            continue

        for root, dirs, files in os.walk(dir_path):
            # Skip build directories
            dirs[:] = [d for d in dirs if d not in ('build', 'managed_components', '.git')]

            for filename in files:
                if not filename.endswith(('.c', '.h')):
                    continue
                file_path = os.path.join(root, filename)
                violations = scan_file(file_path, project_root)
                all_violations.extend(violations)

    return all_violations

# ---- Self-test ----

def run_self_test():
    """Verify the scanner logic works correctly."""
    print("Running self-test...")

    # Test whitelist
    assert is_whitelisted('components/real_hal/real_hal.c') == True
    assert is_whitelisted('components/fake_hal/fake_hal.c') == True
    assert is_whitelisted('components/safety_manager/safety_manager.c') == True
    assert is_whitelisted('test_apps/unit/main/test_runner.c') == True
    assert is_whitelisted('components/ibt2_driver/ibt2_driver.c') == True
    assert is_whitelisted('components/mcp23017_driver/mcp23017_driver.c') == True

    # Test non-whitelist
    assert is_whitelisted('components/position_service/position_service.c') == False
    assert is_whitelisted('main/app_main.c') == False

    # Test known bypass
    assert is_known_bypass('components/position_service/position_service.c') == True
    assert is_known_bypass('main/app_main.c') == False

    print("Self-test PASSED.\n")

# ---- Main ----

def main():
    if len(sys.argv) > 1 and sys.argv[1] == '--self-test':
        run_self_test()
        return 0

    # Find project root
    script_dir = Path(__file__).parent
    project_root = script_dir.parent

    if not (project_root / 'CMakeLists.txt').exists():
        print(f"ERROR: Cannot find project root (CMakeLists.txt) from {script_dir}", file=sys.stderr)
        return 2

    print(f"=== Safety Bypass Scan ===")
    print(f"Project root: {project_root}")
    print()

    violations = scan_project(str(project_root))

    if not violations:
        print("No high-risk output bypasses found in business components.")
        print()
        print("Known documented bypasses:")
        for path, info in KNOWN_BYPASSES.items():
            print(f"  {path}:")
            print(f"    Functions: {', '.join(info['functions'])}")
            print(f"    Status: {info['reason']}")
        print()
        print("SCAN RESULT: PASS")
        return 0

    # Separate known bypasses from new violations
    new_violations = [v for v in violations if not v['known_bypass']]
    known = [v for v in violations if v['known_bypass']]

    if known:
        print("Known documented bypasses:")
        for v in known:
            print(f"  {v['file']}:{v['line']} — {v['pattern']}")
        print()

    if new_violations:
        print(f"NEW VIOLATIONS ({len(new_violations)}):")
        for v in new_violations:
            print(f"  {v['file']}:{v['line']}")
            print(f"    Pattern: {v['pattern']}")
            print(f"    Content: {v['content']}")
            print()
        print("SCAN RESULT: FAIL")
        return 1

    print("No new bypasses. All findings are documented known bypasses.")
    print()
    print("SCAN RESULT: PASS (with documented exceptions)")
    return 0

if __name__ == '__main__':
    sys.exit(main())
