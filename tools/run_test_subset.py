#!/usr/bin/env python3
"""Run specific test subsets and capture results."""
import serial
import time
import sys
import re
from datetime import datetime

PORT = "COM7"
BAUD = 115200
TIMEOUT = 600  # 10 minutes

def read_until_pattern(ser, pattern, output_stream, timeout=TIMEOUT):
    """Read serial until pattern is found."""
    start = time.time()
    buffer = ""
    while time.time() - start < timeout:
        if ser.in_waiting:
            data = ser.read(ser.in_waiting).decode('utf-8', errors='replace')
            buffer += data
            output_stream.write(data)
            output_stream.flush()
            print(data, end="", flush=True)
            if pattern in buffer:
                return buffer, True
        time.sleep(0.01)
    return buffer, False

def count_results(output):
    """Count PASS/FAIL/IGNORE in output."""
    pass_count = output.count(':PASS')
    fail_count = len([l for l in output.split('\n') if ':FAIL' in l and ':PASS' not in l])
    ignore_count = output.count(':IGNORE')
    return pass_count, fail_count, ignore_count

def main():
    if len(sys.argv) < 2:
        print("Usage: run_test_subset.py <output_file>")
        sys.exit(1)

    output_file = sys.argv[1]

    ser = serial.Serial(PORT, BAUD, timeout=1)

    # Match the proven run_unity_tests.py CP210x reset sequence exactly.
    ser.setDTR(False)
    ser.setRTS(False)
    time.sleep(0.1)
    ser.setRTS(True)
    time.sleep(0.1)
    ser.setRTS(False)
    time.sleep(0.5)

    # Read all output, persisting each chunk so a hung test can be diagnosed
    # from the last complete Unity line.
    print(f"Reading test output from {PORT}...")
    with open(output_file, 'w', encoding='utf-8') as output_stream:
        output, found = read_until_pattern(
            ser, "=== ALL TESTS COMPLETE ===", output_stream, timeout=TIMEOUT)

    ser.close()

    # Parse results
    pass_count, fail_count, ignore_count = count_results(output)
    total = pass_count + fail_count + ignore_count

    print(f"\nResults:")
    print(f"  PASS: {pass_count}")
    print(f"  FAIL: {fail_count}")
    print(f"  IGNORE: {ignore_count}")
    print(f"  TOTAL: {total}")

    # Check for specific failures
    if fail_count > 0:
        print(f"\nFailures:")
        for line in output.split('\n'):
            if ':FAIL' in line and ':PASS' not in line:
                print(f"  {line.strip()}")

    return fail_count == 0

if __name__ == '__main__':
    success = main()
    sys.exit(0 if success else 1)
