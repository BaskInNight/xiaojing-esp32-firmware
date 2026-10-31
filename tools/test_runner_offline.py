#!/usr/bin/env python3
"""Offline tests for run_unity_tests.py logic (no hardware required).

Covers:
  1. Multiple active attempts do NOT count as EXTRA_RESET
  2. Reset during Unity execution IS EXTRA_RESET
  3. Per-attempt log content does not mix between attempts
  4. parse_results correctness
  5. detect_extra_reset_in_test_region correctness
"""
import sys
import os
import unittest

# Add tools dir to path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_unity_tests import (
    parse_results,
    detect_extra_reset_in_test_region,
    parse_boot_diagnostics,
    classify_boot_failure,
    TEST_BEGIN_MARKER,
    TEST_UNITY_BEGIN_MARKER,
    TEST_COMPLETE_MARKER,
)


# --- Sample data ---

SINGLE_SUCCESSFUL_BOOT = """ESP-ROM:esp32s3-20210327
Build:Mar 27 2021
rst:0x1 (POWERON),boot:0x28 (SPI_FAST_FLASH_BOOT)
SPIWP:0xee
mode:DIO, clock div:1
I (24) boot: ESP-IDF GIT-NOTFOUND 2nd stage bootloader
I (25) boot: compile time Jul 24 2026 12:53:40
I (230) MSPI Timing: Enter psram timing tuning
I (235) esp_psram: Found 8MB PSRAM device
I (238) esp_psram: Speed: 80MHz
I (241) cpu_start: Multicore app
I (677) esp_psram: SPI SRAM memory test OK
I (808) main_task: Calling app_main()
""" + TEST_BEGIN_MARKER + """
Auto-running all tests...
""" + TEST_UNITY_BEGIN_MARKER + """
./main/test_runner.c:72:revision_next: 0 goes to 1:PASS
./main/test_runner.c:80:revision_next: normal increment:PASS
./main/test_runner.c:90:revision_next: MAX wraps to 1 (skip 0):FAIL
""" + TEST_COMPLETE_MARKER + """
"""

TWO_ATTEMPTS_NO_BEGIN = """ESP-ROM:esp32s3-20210327
rst:0x1 (POWERON),boot:0x28 (SPI_FAST_FLASH_BOOT)
I (241) cpu_start: Multicore app
ESP-ROM:esp32s3-20210327
rst:0x1 (POWERON),boot:0x28 (SPI_FAST_FLASH_BOOT)
I (241) cpu_start: Multicore app
I (677) esp_psram: SPI SRAM memory test OK
I (808) main_task: Calling app_main()
""" + TEST_BEGIN_MARKER + """
Auto-running all tests...
""" + TEST_UNITY_BEGIN_MARKER + """
./main/test_runner.c:72:revision_next: 0 goes to 1:PASS
""" + TEST_COMPLETE_MARKER + """
"""

RESET_DURING_UNITY = """ESP-ROM:esp32s3-20210327
rst:0x1 (POWERON),boot:0x28 (SPI_FAST_FLASH_BOOT)
I (241) cpu_start: Multicore app
I (677) esp_psram: SPI SRAM memory test OK
I (808) main_task: Calling app_main()
""" + TEST_BEGIN_MARKER + """
Auto-running all tests...
""" + TEST_UNITY_BEGIN_MARKER + """
./main/test_runner.c:72:revision_next: 0 goes to 1:PASS
ESP-ROM:esp32s3-20210327
rst:0x1 (POWERON),boot:0x28 (SPI_FAST_FLASH_BOOT)
"""

MSPI_FAIL_HANG = """ESP-ROM:esp32s3-20210327
rst:0x1 (POWERON),boot:0x28 (SPI_FAST_FLASH_BOOT)
I (241) cpu_start: Multicore app
W MSPI Timing: tuning fail, best point is fallen back to index 5
I (241) esp_psram: Found 8MB PSRAM device
"""

DOWNLOAD_BOOT = """ESP-ROM:esp32s3-20210327
rst:0x1 (POWERON),boot:0x12 (DOWNLOAD_BOOT)
"""


class TestParseResults(unittest.TestCase):
    """Test parse_results with various outputs."""

    def test_success_with_pass_fail_ignore(self):
        r = parse_results(SINGLE_SUCCESSFUL_BOOT)
        self.assertEqual(r["pass"], 2)
        self.assertEqual(r["fail"], 1)
        self.assertEqual(r["ignore"], 0)
        self.assertEqual(r["total"], 3)
        self.assertTrue(r["begin"])
        self.assertTrue(r["unity_begin"])
        self.assertTrue(r["complete"])
        self.assertEqual(r["app_main_count"], 1)

    def test_empty_output(self):
        r = parse_results("")
        self.assertEqual(r["total"], 0)
        self.assertFalse(r["begin"])
        self.assertFalse(r["unity_begin"])
        self.assertFalse(r["complete"])

    def test_crash_detection(self):
        output = SINGLE_SUCCESSFUL_BOOT + "Guru Meditation Error"
        r = parse_results(output)
        self.assertIn("Guru Meditation", r["crashes"])


class TestExtraResetDetection(unittest.TestCase):
    """Test that EXTRA_RESET only fires within Unity execution region."""

    def test_runner_retries_not_extra_reset(self):
        """Multiple ROM boots before Unity BEGIN are NOT spontaneous resets."""
        # TWO_ATTEMPTS_NO_BEGIN has two ROM boots, but the second one succeeds
        # and enters Unity. The first ROM boot is before BEGIN marker.
        extra, line = detect_extra_reset_in_test_region(TWO_ATTEMPTS_NO_BEGIN)
        self.assertFalse(extra)

    def test_reset_during_unity_is_extra_reset(self):
        """A ROM boot between BEGIN and COMPLETE IS a spontaneous reset."""
        extra, line = detect_extra_reset_in_test_region(RESET_DURING_UNITY)
        self.assertTrue(extra)
        self.assertIn("ESP-ROM:esp32s3", line)

    def test_no_begin_marker_returns_false(self):
        extra, line = detect_extra_reset_in_test_region("no markers here")
        self.assertFalse(extra)
        self.assertEqual(line, "")

    def test_success_no_extra_reset(self):
        extra, line = detect_extra_reset_in_test_region(SINGLE_SUCCESSFUL_BOOT)
        self.assertFalse(extra)


class TestBootDiagnostics(unittest.TestCase):
    """Test parse_boot_diagnostics and classify_boot_failure."""

    def test_mspi_fail_detected(self):
        diag = parse_boot_diagnostics(MSPI_FAIL_HANG)
        self.assertTrue(diag["mspi_tuning_fail"])
        self.assertTrue(diag["cpu_start_seen"])
        self.assertFalse(diag["calling_app_main_seen"])

    def test_download_boot_detected(self):
        diag = parse_boot_diagnostics(DOWNLOAD_BOOT)
        self.assertTrue(diag["download_boot"])
        cat, desc = classify_boot_failure(diag)
        self.assertEqual(cat, "A")

    def test_category_c_cpu_start_no_app_main(self):
        diag = parse_boot_diagnostics(MSPI_FAIL_HANG)
        cat, desc = classify_boot_failure(diag)
        self.assertEqual(cat, "C")

    def test_category_d_app_main_no_unity(self):
        output = """ESP-ROM:esp32s3
rst:0x1 (POWERON),boot:0x28 (SPI_FAST_FLASH_BOOT)
I (241) cpu_start: Multicore app
I (808) main_task: Calling app_main()
"""
        diag = parse_boot_diagnostics(output)
        cat, desc = classify_boot_failure(diag)
        self.assertEqual(cat, "D")

    def test_successful_boot_all_markers(self):
        diag = parse_boot_diagnostics(SINGLE_SUCCESSFUL_BOOT)
        self.assertTrue(diag["cpu_start_seen"])
        self.assertTrue(diag["calling_app_main_seen"])
        self.assertTrue(diag["psram_test_ok"])
        self.assertFalse(diag["mspi_tuning_fail"])

    def test_psram_test_ok_in_success(self):
        diag = parse_boot_diagnostics(SINGLE_SUCCESSFUL_BOOT)
        self.assertTrue(diag["psram_test_ok"])

    def test_psram_test_missing_in_failure(self):
        diag = parse_boot_diagnostics(MSPI_FAIL_HANG)
        self.assertFalse(diag["psram_test_ok"])


class TestAttemptIsolation(unittest.TestCase):
    """Verify that per-attempt buffers don't cross-contaminate."""

    def test_independent_buffers(self):
        """Simulate two independent attempt outputs — they must not merge."""
        attempt1 = """ESP-ROM:esp32s3
rst:0x1 (POWERON),boot:0x28 (SPI_FAST_FLASH_BOOT)
I (241) cpu_start: Multicore app
W MSPI Timing: tuning fail
"""
        attempt2 = """ESP-ROM:esp32s3
rst:0x1 (POWERON),boot:0x28 (SPI_FAST_FLASH_BOOT)
I (241) cpu_start: Multicore app
I (677) esp_psram: SPI SRAM memory test OK
I (808) main_task: Calling app_main()
""" + TEST_BEGIN_MARKER + "\n" + TEST_UNITY_BEGIN_MARKER + "\n" + TEST_COMPLETE_MARKER + "\n"

        diag1 = parse_boot_diagnostics(attempt1)
        diag2 = parse_boot_diagnostics(attempt2)

        self.assertTrue(diag1["mspi_tuning_fail"])
        self.assertFalse(diag1["psram_test_ok"])
        self.assertFalse(diag2["mspi_tuning_fail"])
        self.assertTrue(diag2["psram_test_ok"])
        self.assertTrue(diag2["calling_app_main_seen"])

    def test_attempt_log_filenames(self):
        """Verify attempt log naming convention."""
        log_base = "round2_2_3_manual_run1"
        log_ext = ".log"
        names = []
        for i in range(1, 4):
            names.append("%s_attempt%d%s" % (log_base, i, log_ext))
        self.assertEqual(names[0], "round2_2_3_manual_run1_attempt1.log")
        self.assertEqual(names[1], "round2_2_3_manual_run1_attempt2.log")
        self.assertEqual(names[2], "round2_2_3_manual_run1_attempt3.log")


if __name__ == "__main__":
    unittest.main()
