#!/usr/bin/env python3
"""Single-boot test runner with strict BEGIN/COMPLETE validation."""
import serial
import time
import sys
import json
from datetime import datetime
from pathlib import Path

PORT = "COM7"
BAUD = 115200
TIMEOUT = 900  # 15 minutes

def run_single_boot(output_file):
    ser = serial.Serial(PORT, BAUD, timeout=1)

    # Reset board for clean boot
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.1)
    ser.setRTS(False)

    start = time.time()
    buffer = ""
    begin_count = 0
    complete_count = 0

    try:
        while time.time() - start < TIMEOUT:
            if ser.in_waiting:
                data = ser.read(ser.in_waiting).decode('utf-8', errors='replace')
                buffer += data

                # Count markers
                begin_count = buffer.count("UNITY TESTS BEGIN")
                complete_count = buffer.count("ALL TESTS COMPLETE")

                # Check for completion
                if complete_count >= 1:
                    # Read a bit more to capture final stats
                    time.sleep(2)
                    if ser.in_waiting:
                        buffer += ser.read(ser.in_waiting).decode('utf-8', errors='replace')
                    break

                # Check for double boot (invalid)
                if begin_count > 1:
                    print(f"ERROR: Multiple boots detected ({begin_count} BEGIN markers)")
                    break

            time.sleep(0.01)
    finally:
        ser.close()

    # Write output
    with open(output_file, 'w', encoding='utf-8') as f:
        f.write(buffer)

    # Parse results
    pass_count = buffer.count(':PASS')
    fail_lines = [l for l in buffer.split('\n') if ':FAIL' in l and ':PASS' not in l]
    ignore_count = buffer.count(':IGNORE')
    total = pass_count + len(fail_lines) + ignore_count

    results = {
        'begin_count': begin_count,
        'complete_count': complete_count,
        'pass': pass_count,
        'fail': len(fail_lines),
        'failures': [l.strip()[:200] for l in fail_lines],
        'ignore': ignore_count,
        'total': total,
        'valid': begin_count == 1 and complete_count >= 1,
        'elapsed_seconds': round(time.time() - start, 1),
    }

    return results

if __name__ == '__main__':
    output_file = sys.argv[1] if len(sys.argv) > 1 else 'diagnostic.log'
    results = run_single_boot(output_file)

    print(f"\n{'='*60}")
    print(f"Single Boot Diagnostic Results")
    print(f"{'='*60}")
    print(f"BEGIN: {results['begin_count']}")
    print(f"COMPLETE: {results['complete_count']}")
    print(f"PASS: {results['pass']}")
    print(f"FAIL: {results['fail']}")
    print(f"IGNORE: {results['ignore']}")
    print(f"TOTAL: {results['total']}")
    print(f"VALID: {results['valid']}")
    print(f"Elapsed: {results['elapsed_seconds']}s")

    if results['failures']:
        print(f"\nFailures:")
        for f in results['failures']:
            print(f"  {f}")

    print(f"{'='*60}")

    # Write JSON
    json_file = output_file.replace('.log', '.json')
    with open(json_file, 'w') as jf:
        json.dump(results, jf, indent=2)
