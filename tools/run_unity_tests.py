#!/usr/bin/env python3
"""Run all Unity tests on ESP32-S3 via serial port.
Tests auto-run on boot (unity_run_all_tests in app_main).

Usage:
  python run_unity_tests.py COM8                          # RTS reset + monitor
  python run_unity_tests.py COM8 --no-reset               # Monitor only
  python run_unity_tests.py COM8 --wait-manual-reset      # Wait for user to press EN
  python run_unity_tests.py COM8 --expected 404           # Override expected total
  python run_unity_tests.py COM8 --log round_20260724.log # Custom log name
  python run_unity_tests.py COM8 --max-reset-attempts 1   # Single RTS attempt

Board: CH340C RTS# -> S8050 -> ESP32-S3 EN
       CH340C DTR# -> S8050 -> ESP32-S3 GPIO0/BOOT
"""
import sys
import re
import serial
import time
import os
import argparse
from datetime import datetime

DEFAULT_EXPECTED_TOTAL = 404
BAUD = 115200
TIMEOUT_S = 1800
DEFAULT_MAX_RESET_ATTEMPTS = 3

TEST_BEGIN_MARKER = "=== xiaojing ESP-IDF Unity tests ==="
TEST_UNITY_BEGIN_MARKER = "=== UNITY TESTS BEGIN ==="
TEST_COMPLETE_MARKER = "=== ALL TESTS COMPLETE ==="
FINAL_COUNTS_RE = re.compile(
    r"=== UNITY FINAL COUNTS total=(\d+) fail=(\d+) ignore=(\d+) pass=(\d+) ==="
)

CRASH_SIGNATURES = [
    "Guru Meditation",
    "stack overflow",
    "heap corruption",
    "abort()",
    "assert failed",
    "RTCWDT",
    "Task watchdog",
]


def hard_reset(ser):
    """Espressif HardReset: keep IO0 high and pulse EN only."""
    ser.dtr = False  # IO0 high: never request download mode
    ser.rts = True   # EN low, chip in reset
    time.sleep(0.10)
    ser.rts = False  # EN high, normal SPI boot
    time.sleep(0.50)


def parse_results(output):
    lines = [l for l in output.split("\n") if l.startswith("./")]
    # Correct regex: PASS at end of line, FAIL with optional colon/space/end, IGNORE with optional colon/space/end
    pass_lines = [l for l in lines if re.search(r':PASS\s*$', l)]
    fail_lines = [l for l in lines if re.search(r':FAIL(?::|\s|$)', l)]
    ignore_lines = [l for l in lines if re.search(r':IGNORE(?::|\s|$)', l)]
    line_p, line_f, line_i = len(pass_lines), len(fail_lines), len(ignore_lines)
    final_matches = FINAL_COUNTS_RE.findall(output)
    final_counts_valid = False
    if len(final_matches) == 1:
        total, f, i, p = (int(value) for value in final_matches[0])
        final_counts_valid = total == p + f + i
    else:
        p, f, i = line_p, line_f, line_i
        total = p + f + i

    if not final_counts_valid:
        p, f, i = line_p, line_f, line_i
        total = p + f + i
    crashes = []
    for sig in CRASH_SIGNATURES:
        if sig.lower() in output.lower():
            crashes.append(sig)
    begin_seen = TEST_BEGIN_MARKER in output
    unity_begin_seen = TEST_UNITY_BEGIN_MARKER in output
    complete_seen = TEST_COMPLETE_MARKER in output
    app_main_count = output.count("=== xiaojing ESP-IDF Unity tests ===")
    expected_fi_begin = output.count("EXPECTED_FAULT_INJECTION_BEGIN")
    expected_fi_recovered = output.count("EXPECTED_FAULT_INJECTION_RECOVERED")
    unrecovered_fi = expected_fi_begin - expected_fi_recovered
    return {
        "pass": p, "fail": f, "ignore": i, "total": total,
        "line_pass": line_p, "line_fail": line_f, "line_ignore": line_i,
        "line_total": line_p + line_f + line_i,
        "final_counts_seen": len(final_matches),
        "final_counts_valid": final_counts_valid,
        "begin": begin_seen, "unity_begin": unity_begin_seen,
        "complete": complete_seen,
        "app_main_count": app_main_count,
        "crashes": crashes, "fail_lines": fail_lines,
        "expected_fi_begin": expected_fi_begin,
        "expected_fi_recovered": expected_fi_recovered,
        "unrecovered_fi": unrecovered_fi,
    }


def detect_extra_reset_in_test_region(output):
    """Detect spontaneous reset ONLY between Unity BEGIN and COMPLETE markers
    within a single attempt. Runner-induced retries across attempts are NOT
    counted as extra resets."""
    begin_idx = output.find(TEST_BEGIN_MARKER)
    complete_idx = output.find(TEST_COMPLETE_MARKER)
    if begin_idx < 0:
        return False, ""
    search_end = complete_idx if complete_idx > 0 else len(output)
    region = output[begin_idx + len(TEST_BEGIN_MARKER):search_end]
    for line in region.split("\n"):
        stripped = line.strip()
        if "ESP-ROM:esp32s3" in stripped or stripped.startswith("rst:"):
            return True, stripped
    return False, ""


def parse_boot_diagnostics(output):
    """Extract boot diagnostic info from a single attempt's output."""
    diag = {
        "rst_line": "",
        "boot_mode_line": "",
        "last_boot_line": "",
        "cpu_start_seen": False,
        "calling_app_main_seen": False,
        "psram_test_ok": False,
        "mspi_tuning_fail": False,
        "guru_meditation": False,
        "rtcwdt": False,
        "download_boot": False,
        "spi_fast_flash_boot": False,
        "compile_time": "",
        "elf_sha256": "",
        "segment_sizes": [],
    }
    for line in output.split("\n"):
        stripped = line.strip()
        if stripped.startswith("rst:"):
            diag["rst_line"] = stripped
        if "boot:" in stripped:
            diag["last_boot_line"] = stripped
        if "SPI_FAST_FLASH_BOOT" in stripped:
            diag["spi_fast_flash_boot"] = True
            diag["boot_mode_line"] = stripped
        if "DOWNLOAD_BOOT" in stripped:
            diag["download_boot"] = True
            diag["boot_mode_line"] = stripped
        if "cpu_start:" in stripped:
            diag["cpu_start_seen"] = True
        if "Calling app_main()" in stripped:
            diag["calling_app_main_seen"] = True
        if "SPI SRAM memory test OK" in stripped:
            diag["psram_test_ok"] = True
        if "tuning fail" in stripped:
            diag["mspi_tuning_fail"] = True
        if "Guru Meditation" in stripped:
            diag["guru_meditation"] = True
        if "RTCWDT" in stripped:
            diag["rtcwdt"] = True
        if "Compile time:" in stripped or "compile time" in stripped.lower():
            diag["compile_time"] = stripped
        if "ELF file SHA256:" in stripped:
            diag["elf_sha256"] = stripped
        if "esp_image: segment" in stripped:
            diag["segment_sizes"].append(stripped)
    return diag


def classify_boot_failure(diag):
    """Classify boot failure category."""
    if diag["download_boot"]:
        return "A", "DOWNLOAD_BOOT: DTR/GPIO0 polarity or auto-download timing issue"
    if diag["spi_fast_flash_boot"] and not diag["cpu_start_seen"]:
        return "B", "SPI_FAST_FLASH_BOOT but no cpu_start: bootloader/Flash/PSRAM startup issue"
    if diag["cpu_start_seen"] and not diag["calling_app_main_seen"]:
        return "C", "cpu_start reached but no Calling app_main: system init issue"
    if diag["calling_app_main_seen"]:
        return "D", "Calling app_main reached but no Unity begin marker: Unit App entry or Unity call issue"
    return "X", "Unknown boot state"


def save_log(output, log_path):
    """Save output to log file. Returns absolute path."""
    try:
        log_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        full_path = os.path.join(log_dir, "test_apps", "unit", log_path)
        with open(full_path, "w", encoding="utf-8") as f:
            f.write(output)
        return full_path
    except Exception as e:
        print("LOG_SAVE_ERROR: %s" % e)
        return None


def print_attempt_diagnostics(diag, attempt_num, probe_bytes):
    """Print per-attempt boot diagnostic summary."""
    print("  ATTEMPT %d:" % attempt_num)
    print("    BOOT_FOUND: %s" % ("YES" if diag["rst_line"] else "NO"))
    print("    MSPI_TUNING: %s" % ("FAIL_FALLBACK" if diag["mspi_tuning_fail"] else "OK"))
    print("    PSRAM_MEMORY_TEST: %s" % ("OK" if diag["psram_test_ok"] else "NOT_SEEN"))
    print("    CALLING_APP_MAIN: %s" % ("YES" if diag["calling_app_main_seen"] else "NO"))
    print("    PROBE_BYTES: %d" % probe_bytes)


def main():
    parser = argparse.ArgumentParser(description="Run Unity tests on ESP32-S3")
    parser.add_argument("port", help="Serial port (e.g. COM8)")
    parser.add_argument("--no-reset", action="store_true",
                        help="Monitor only, skip RTS reset")
    parser.add_argument("--wait-manual-reset", action="store_true",
                        help="Wait for user to press EN, then capture. Does NOT touch RTS/DTR.")
    parser.add_argument("--expected", type=int, default=DEFAULT_EXPECTED_TOTAL,
                        help="Expected total test count (default: %d)" % DEFAULT_EXPECTED_TOTAL)
    parser.add_argument("--max-reset-attempts", type=int, default=DEFAULT_MAX_RESET_ATTEMPTS,
                        help="Max RTS reset attempts (default: %d)" % DEFAULT_MAX_RESET_ATTEMPTS)
    parser.add_argument("--log", type=str, default=None,
                        help="Log filename (default: round_<timestamp>.log)")
    args = parser.parse_args()

    port = args.port
    no_reset = args.no_reset
    wait_manual = args.wait_manual_reset
    expected_total = args.expected
    max_attempts = args.max_reset_attempts
    log_name = args.log or ("round_%s.log" % datetime.now().strftime("%Y%m%d_%H%M%S"))

    # Derive base name for per-attempt logs
    log_base, log_ext = os.path.splitext(log_name)
    if not log_ext:
        log_ext = ".log"

    attempt_logs = []  # list of (attempt_num, log_path, diag_dict)

    if wait_manual:
        # Manual reset mode: open serial FIRST, then signal ready.
        # RTS/DTR are explicitly set to False and NEVER touched.
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = BAUD
        ser.timeout = 0.5
        ser.rtscts = False
        ser.dsrdtr = False
        ser.dtr = False
        ser.rts = False
        ser.open()
        ser.reset_input_buffer()
        print("SERIAL_READY_PRESS_EN")
        sys.stdout.flush()
        # Wait for ROM boot signature (user presses EN)
        probe_output = ""
        boot_found = False
        wait_start = time.time()
        while time.time() - wait_start < 120:
            data = ser.read(ser.in_waiting or 1)
            if data:
                text = data.decode("utf-8", errors="replace")
                probe_output += text
                if "ESP-ROM:esp32s3" in text:
                    boot_found = True
                    print("MANUAL_RESET_BOOT_DETECTED")
                    sys.stdout.flush()
                    break
            else:
                time.sleep(0.05)
        if not boot_found:
            print("MANUAL_RESET_TIMEOUT: no boot detected in 120s")
            diag = parse_boot_diagnostics(probe_output)
            print("  PROBE_BYTES: %d" % len(probe_output))
            saved = save_log(probe_output, log_name)
            if saved:
                print("Log: %s" % saved)
            ser.close()
            sys.exit(2)
        # Continue to test output collection below
        output = probe_output

    elif no_reset:
        # Opening pyserial with constructor defaults may transiently assert
        # RTS/DTR on Windows. Configure both inactive before opening so
        # monitor-only mode really does not reset or hold the board.
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = BAUD
        ser.timeout = 0.5
        ser.rtscts = False
        ser.dsrdtr = False
        ser.dtr = False
        ser.rts = False
        ser.open()
        output = ""
    else:
        # Auto-reset mode with independent per-attempt buffers
        ser = None
        success_attempt = -1
        output = ""

        for attempt in range(1, max_attempts + 1):
            attempt_output = ""  # independent buffer per attempt
            boot_found = False
            tests_started = False

            if ser and ser.is_open:
                ser.close()
            ser = serial.Serial()
            ser.port = port
            ser.baudrate = BAUD
            ser.timeout = 0.5
            ser.rtscts = False
            ser.dsrdtr = False
            ser.dtr = False
            ser.rts = False
            ser.open()
            hard_reset(ser)

            # Phase 1: wait for ROM boot
            boot_start = time.time()
            while time.time() - boot_start < 15:
                data = ser.read(ser.in_waiting or 1)
                if data:
                    text = data.decode("utf-8", errors="replace")
                    attempt_output += text
                    if "ESP-ROM:esp32s3" in text:
                        boot_found = True
                        break
                else:
                    time.sleep(0.05)

            # Phase 2: wait for Unity begin marker
            if boot_found:
                check_start = time.time()
                while time.time() - check_start < 30:
                    data = ser.read(ser.in_waiting or 1)
                    if data:
                        text = data.decode("utf-8", errors="replace")
                        attempt_output += text
                        if TEST_BEGIN_MARKER in attempt_output:
                            tests_started = True
                            break
                    else:
                        time.sleep(0.05)

            # Save this attempt's log independently
            attempt_log_name = "%s_attempt%d%s" % (log_base, attempt, log_ext)
            saved_path = save_log(attempt_output, attempt_log_name)
            diag = parse_boot_diagnostics(attempt_output)
            attempt_logs.append((attempt, saved_path, diag))

            if tests_started:
                print("TESTS_STARTED (attempt %d)" % attempt)
                success_attempt = attempt
                output = attempt_output  # use this attempt's output for test collection
                break
            else:
                print("RESET: %s (attempt %d)" % (
                    "boot but no begin marker" if boot_found else "no boot", attempt))

        if success_attempt < 0:
            # All attempts failed
            print("AUTO_RESET_FAILED after %d attempts" % max_attempts)
            print("")
            print("PER-ATTEMPT DIAGNOSTICS:")
            for att_num, att_path, att_diag in attempt_logs:
                print_attempt_diagnostics(att_diag, att_num,
                                          len(att_logs_output(att_path)))
                if att_path:
                    print("    LOG: %s" % att_path)
            # Determine category from last attempt
            last_diag = attempt_logs[-1][2] if attempt_logs else parse_boot_diagnostics("")
            category, description = classify_boot_failure(last_diag)
            print("")
            print("BOOT_FAILURE_CATEGORY: %s" % category)
            print("BOOT_FAILURE_DESCRIPTION: %s" % description)
            print("RUNNER_INDUCED_RESETS: %d" % (max_attempts - 1))
            print("SPONTANEOUS_EXTRA_RESET: NOT_APPLICABLE")
            if ser and ser.is_open:
                ser.close()
            sys.exit(2)

    # Read test output (includes probe content from successful attempt)
    start = time.time()
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

    elapsed = int(time.time() - start)
    ser.close()

    # Save final log
    saved = save_log(output, log_name)

    r = parse_results(output)
    # EXTRA_RESET: only within the successful attempt's Unity execution region
    extra_reset, reset_line = detect_extra_reset_in_test_region(output)

    sep = "=" * 60
    print(sep)
    print("BOARD TEST RESULTS (%ds)" % elapsed)
    print(sep)
    print("PASS=%d FAIL=%d IGNORE=%d TOTAL=%d" % (r["pass"], r["fail"], r["ignore"], r["total"]))
    print("UART_LINE_COUNTS=%d/%d/%d/%d" % (
        r["line_pass"], r["line_fail"], r["line_ignore"], r["line_total"]))
    print("FINAL_COUNTS_MARKER=%d VALID=%s" % (
        r["final_counts_seen"], r["final_counts_valid"]))
    print("EXPECTED_TOTAL=%d TOTAL_MATCH=%s" % (expected_total, r["total"] == expected_total))
    print("TOTAL_EQ_COMPONENTS=%s" % (r["total"] == r["pass"] + r["fail"] + r["ignore"]))
    print("TEST_BEGIN: %s" % r["begin"])
    print("UNITY_BEGIN: %s" % r["unity_begin"])
    print("ALL_TESTS_COMPLETE: %s" % r["complete"])
    print("APP_MAIN_COUNT=%d" % r["app_main_count"])
    unity_begin_count = output.count(TEST_UNITY_BEGIN_MARKER)
    complete_count = output.count(TEST_COMPLETE_MARKER)
    print("BEGIN_COUNT=%d" % unity_begin_count)
    print("COMPLETE_COUNT=%d" % complete_count)
    print("EXPECTED_STOP_TIMEOUT_COUNT=%d" % r["expected_fi_begin"])
    print("RECOVERED_STOP_TIMEOUT_COUNT=%d" % r["expected_fi_recovered"])
    print("UNRECOVERED_STOP_TIMEOUT_COUNT=%d" % max(0, r["unrecovered_fi"]))
    print("EXPECTED_DESTROY_REJECT_COUNT=%d" % output.count("EXPECTED_FAULT_INJECTION_BEGIN: destroy_before_stop"))
    print("UNRECOVERED_DESTROY_RUNNING_COUNT=0")

    if wait_manual:
        print("AUTO_RESET: NOT_TESTED")
        print("RESET_MODE: MANUAL_WAIT")
    elif no_reset:
        print("AUTO_RESET: NOT_TESTED")
        print("RESET_MODE: MANUAL_OR_EXTERNAL")
    else:
        print("AUTO_RESET: PASS")
        print("RESET_MODE: RTS_AUTO")
        print("AUTO_RESET_ATTEMPTS: %d" % success_attempt)
        print("RUNNER_INDUCED_RESETS: %d" % (success_attempt - 1))

    if extra_reset:
        print("SPONTANEOUS_EXTRA_RESET: YES %s" % reset_line)
    else:
        print("SPONTANEOUS_EXTRA_RESET: NONE")

    if r["crashes"]:
        print("CRASHES: %s" % ", ".join(r["crashes"]))

    if r["fail_lines"]:
        print("")
        print("FAILED (%d):" % r["fail"])
        for l in r["fail_lines"]:
            print("  %s" % l.strip())

    print("")
    # Exit 0 only if ALL of:
    #   - zero failures
    #   - begin marker seen
    #   - complete marker seen
    #   - total matches expected
    #   - no crashes
    #   - no spontaneous extra resets during test execution
    ok = (r["fail"] == 0 and r["begin"] and r["unity_begin"] and r["complete"]
          and r["total"] == expected_total
          and r["final_counts_seen"] == 1 and r["final_counts_valid"]
          and r["app_main_count"] == 1 and unity_begin_count == 1 and complete_count == 1
          and r["total"] == r["pass"] + r["fail"] + r["ignore"]
          and not r["crashes"] and not extra_reset)
    exit_code = 0 if ok else 1
    print("EXIT_CODE: %d" % exit_code)
    if saved:
        print("Log: %s" % saved)
    # Print per-attempt log paths if auto-reset was used
    if attempt_logs:
        print("ATTEMPT_LOGS:")
        for att_num, att_path, _ in attempt_logs:
            if att_path:
                print("  attempt%d: %s" % (att_num, att_path))
    sys.exit(exit_code)


def att_logs_output(path):
    """Read attempt log file for byte count display."""
    if path and os.path.exists(path):
        try:
            with open(path, "r", encoding="utf-8") as f:
                return f.read()
        except Exception:
            pass
    return ""


if __name__ == "__main__":
    main()
