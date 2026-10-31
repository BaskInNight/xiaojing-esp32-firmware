#!/usr/bin/env python3
"""Overnight soak runner for ESP32-S3 unit tests.
Runs the same binary repeatedly via RTS reset, validating each round.
"""
import sys
import re
import serial
import time
import os
import json
import hashlib
from datetime import datetime

BAUD = 115200
TIMEOUT_S = 300
ROUND_INTERVAL_S = 10

TEST_BEGIN_MARKER = "=== xiaojing ESP-IDF Unity tests ==="
TEST_UNITY_BEGIN_MARKER = "=== UNITY TESTS BEGIN ==="
TEST_COMPLETE_MARKER = "=== ALL TESTS COMPLETE ==="

CRASH_SIGNATURES = [
    "Guru Meditation", "stack overflow", "heap corruption",
    "abort()", "assert failed", "RTCWDT", "Task watchdog",
]

STOP_CONDITIONS = [
    "Guru Meditation", "stack overflow", "heap corruption",
    "brownout", "unexpected reset",
]


def hard_reset(ser):
    ser.dtr = False
    ser.rts = False
    time.sleep(0.10)
    ser.rts = True
    time.sleep(0.10)
    ser.rts = False
    time.sleep(0.50)


def parse_results(output):
    lines = [l for l in output.split("\n") if l.startswith("./")]
    pass_lines = [l for l in lines if re.search(r':PASS\s*$', l)]
    fail_lines = [l for l in lines if re.search(r':FAIL(?::|\s|$)', l)]
    ignore_lines = [l for l in lines if re.search(r':IGNORE(?::|\s|$)', l)]
    p, f, i = len(pass_lines), len(fail_lines), len(ignore_lines)
    crashes = [s for s in CRASH_SIGNATURES if s.lower() in output.lower()]
    begin_seen = TEST_BEGIN_MARKER in output
    unity_begin_seen = TEST_UNITY_BEGIN_MARKER in output
    complete_seen = TEST_COMPLETE_MARKER in output
    app_main_count = output.count(TEST_BEGIN_MARKER)
    unity_begin_count = output.count(TEST_UNITY_BEGIN_MARKER)
    complete_count = output.count(TEST_COMPLETE_MARKER)
    fi_begin = output.count("EXPECTED_FAULT_INJECTION_BEGIN")
    fi_recovered = output.count("EXPECTED_FAULT_INJECTION_RECOVERED")
    return {
        "pass": p, "fail": f, "ignore": i, "total": p + f + i,
        "begin": begin_seen, "unity_begin": unity_begin_seen,
        "complete": complete_seen,
        "app_main_count": app_main_count,
        "unity_begin_count": unity_begin_count,
        "complete_count": complete_count,
        "crashes": crashes, "fail_lines": fail_lines,
        "fi_begin": fi_begin, "fi_recovered": fi_recovered,
        "unrecovered_fi": fi_begin - fi_recovered,
    }


def run_one_round(ser, round_num):
    """Run one round of tests via RTS reset. Returns (output, result_dict, error)."""
    ser.reset_input_buffer()
    hard_reset(ser)

    output = ""
    boot_found = False
    start = time.time()

    # Phase 1: wait for ROM boot
    boot_start = time.time()
    while time.time() - boot_start < 15:
        data = ser.read(ser.in_waiting or 1)
        if data:
            text = data.decode("utf-8", errors="replace")
            output += text
            if "ESP-ROM:esp32s3" in text:
                boot_found = True
                break
        else:
            time.sleep(0.05)

    if not boot_found:
        return output, None, "NO_BOOT"

    # Phase 2: wait for begin marker
    check_start = time.time()
    begin_found = False
    while time.time() - check_start < 30:
        data = ser.read(ser.in_waiting or 1)
        if data:
            text = data.decode("utf-8", errors="replace")
            output += text
            if TEST_BEGIN_MARKER in output:
                begin_found = True
                break
        else:
            time.time() - check_start < 30
            time.sleep(0.05)

    if not begin_found:
        return output, None, "NO_BEGIN"

    # Phase 3: collect test output
    while time.time() - start < TIMEOUT_S:
        data = ser.read(ser.in_waiting or 1)
        if data:
            text = data.decode("utf-8", errors="replace")
            output += text
            if TEST_COMPLETE_MARKER in output:
                time.sleep(2)
                while True:
                    d = ser.read(ser.in_waiting or 0)
                    if d:
                        output += d.decode("utf-8", errors="replace")
                    else:
                        break
                break
        else:
            time.sleep(0.05)

    if TEST_COMPLETE_MARKER not in output:
        return output, None, "NO_COMPLETE"

    # Check for crashes between BEGIN and COMPLETE
    begin_idx = output.find(TEST_BEGIN_MARKER)
    complete_idx = output.find(TEST_COMPLETE_MARKER)
    region = output[begin_idx:complete_idx] if begin_idx >= 0 and complete_idx > 0 else ""
    for sig in CRASH_SIGNATURES:
        if sig.lower() in region.lower():
            return output, None, f"CRASH:{sig}"

    r = parse_results(output)
    return output, r, None


def save_log(output, directory, filename):
    path = os.path.join(directory, filename)
    with open(path, "w", encoding="utf-8") as f:
        f.write(output)
    return path


def save_checkpoint(checkpoint_path, data):
    with open(checkpoint_path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)


def main():
    if len(sys.argv) < 3:
        print("Usage: python run_executor_overnight_soak.py <PORT> <soak_dir>")
        sys.exit(1)

    port = sys.argv[1]
    soak_dir = sys.argv[2]
    os.makedirs(soak_dir, exist_ok=True)

    checkpoint_path = os.path.join(os.path.dirname(soak_dir), "checkpoint.json")
    target_hours = 4.0
    min_hours = 3.0
    start_time = time.time()
    target_end = start_time + target_hours * 3600
    hard_deadline = start_time + 6 * 3600

    ser = serial.Serial(port, BAUD, timeout=0.5, rtscts=False, dsrdtr=False)

    completed = 0
    valid = 0
    invalid_capture = 0
    test_fail = 0
    reset_failures = 0
    total_pass = 0
    total_fail = 0
    total_ignore = 0
    consecutive_reset_failures = 0
    first_fail_log = None

    checkpoint = {
        "start_time": datetime.now().isoformat(),
        "last_update": datetime.now().isoformat(),
        "target_end_time": datetime.fromtimestamp(target_end).isoformat(),
        "completed_rounds": 0,
        "valid_rounds": 0,
        "invalid_capture_rounds": 0,
        "test_fail_rounds": 0,
        "reset_failures": 0,
        "last_binary_sha256": "",
        "last_elf_sha256": "",
        "last_result": "",
    }
    save_checkpoint(checkpoint_path, checkpoint)

    print(f"SOAK START port={port} target={target_hours}h min={min_hours}h")
    sys.stdout.flush()

    round_num = 0
    while True:
        now = time.time()
        if now >= hard_deadline:
            print(f"HARD_DEADLINE_REACHED at round {round_num}")
            break
        if now >= target_end and completed > 0:
            elapsed_h = (now - start_time) / 3600
            if elapsed_h >= min_hours:
                print(f"TARGET_REACHED {elapsed_h:.1f}h after {completed} rounds")
                break

        round_num += 1
        round_start = time.time()

        output, result, error = run_one_round(ser, round_num)
        round_elapsed = time.time() - round_start

        log_filename = f"soak_{round_num:04d}.log"
        log_path = save_log(output, soak_dir, log_filename)

        completed += 1

        if error:
            if "CRASH" in error:
                print(f"ROUND {round_num}: {error} - STOPPING SOAK")
                first_fail_log = log_path
                break
            elif error in ("NO_BOOT", "NO_BEGIN", "NO_COMPLETE"):
                reset_failures += 1
                invalid_capture += 1
                consecutive_reset_failures += 1
                if consecutive_reset_failures >= 3:
                    print(f"ROUND {round_num}: {consecutive_reset_failures} consecutive reset failures - STOPPING")
                    break
                print(f"ROUND {round_num}: {error} (reset_fail={reset_failures})")
            else:
                print(f"ROUND {round_num}: {error}")
        else:
            consecutive_reset_failures = 0
            r = result
            total_pass += r["pass"]
            total_fail += r["fail"]
            total_ignore += r["ignore"]

            # Validate this round
            issues = []
            if r["fail"] > 0:
                issues.append(f"FAIL={r['fail']}")
                test_fail += 1
                if not first_fail_log:
                    first_fail_log = log_path
            if r["app_main_count"] != 1:
                issues.append(f"APP_MAIN={r['app_main_count']}")
            if r["unity_begin_count"] != 1:
                issues.append(f"BEGIN={r['unity_begin_count']}")
            if r["complete_count"] != 1:
                issues.append(f"COMPLETE={r['complete_count']}")
            if r["crashes"]:
                issues.append(f"CRASHES={r['crashes']}")
            if r["unrecovered_fi"] > 0:
                issues.append(f"UNRECOVERED_FI={r['unrecovered_fi']}")
            non_hw_ignore = r["ignore"] - 15 if r["ignore"] >= 15 else r["ignore"]
            if non_hw_ignore > 0:
                issues.append(f"NON_HW_IGNORE={non_hw_ignore}")

            if issues:
                print(f"ROUND {round_num}: ISSUES: {', '.join(issues)}")
                if r["fail"] > 0 or r["crashes"]:
                    first_fail_log = log_path
                    break
            else:
                valid += 1
                if round_num % 10 == 0:
                    print(f"ROUND {round_num}: PASS={r['pass']} IGNORE={r['ignore']} "
                          f"valid={valid}/{completed} elapsed={round_elapsed:.1f}s")

        checkpoint.update({
            "last_update": datetime.now().isoformat(),
            "completed_rounds": completed,
            "valid_rounds": valid,
            "invalid_capture_rounds": invalid_capture,
            "test_fail_rounds": test_fail,
            "reset_failures": reset_failures,
            "last_result": error or f"PASS={result['pass']}" if result else error,
        })
        save_checkpoint(checkpoint_path, checkpoint)

        # Interval between rounds
        time.sleep(ROUND_INTERVAL_S)

    ser.close()

    elapsed_h = (time.time() - start_time) / 3600
    print(f"\nSOAK COMPLETE: {completed} rounds in {elapsed_h:.1f}h")
    print(f"  valid={valid} invalid_capture={invalid_capture} test_fail={test_fail}")
    print(f"  reset_failures={reset_failures}")
    print(f"  total_pass={total_pass} total_fail={total_fail} total_ignore={total_ignore}")

    checkpoint.update({
        "last_update": datetime.now().isoformat(),
        "completed_rounds": completed,
        "valid_rounds": valid,
        "invalid_capture_rounds": invalid_capture,
        "test_fail_rounds": test_fail,
        "reset_failures": reset_failures,
    })
    save_checkpoint(checkpoint_path, checkpoint)

    return 0 if test_fail == 0 and first_fail_log is None else 1


if __name__ == "__main__":
    sys.exit(main())
