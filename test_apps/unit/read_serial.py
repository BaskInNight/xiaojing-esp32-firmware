import serial
import sys
import time

port = sys.argv[1] if len(sys.argv) > 1 else 'COM7'
baud = 115200
timeout = 90  # seconds

ser = serial.Serial(port, baud, timeout=1)
start = time.time()
output = []

while time.time() - start < timeout:
    line = ser.readline().decode('utf-8', errors='replace').strip()
    if line:
        print(line)
        output.append(line)
        # Check for completion markers
        if 'Tests finished' in line or 'UNITY_END' in line or 'COMPLETE' in line:
            break
        if 'Guru Meditation' in line or 'abort()' in line:
            print("CRASH DETECTED")
            break

ser.close()

# Summary
pass_count = sum(1 for l in output if 'PASS' in l and 'FAIL' not in l)
fail_count = sum(1 for l in output if 'FAIL' in l)
ignore_count = sum(1 for l in output if 'IGNORE' in l)

print(f"\n=== SUMMARY: PASS={pass_count} FAIL={fail_count} IGNORE={ignore_count} ===")
