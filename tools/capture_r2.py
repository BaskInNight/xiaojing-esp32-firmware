#!/usr/bin/env python3
"""Capture diagnostic output filtering for r2integ and setup failures."""
import serial
import time

PORT = "COM7"
BAUD = 115200
TIMEOUT = 600

ser = serial.Serial(PORT, BAUD, timeout=1)
start = time.time()
found_r2 = False

try:
    while time.time() - start < TIMEOUT:
        line = ser.readline()
        if line:
            text = line.decode('utf-8', errors='replace').rstrip()
            # Show r2integ lines, FAIL lines, and safety_mgr lines near tests
            if ('r2integ' in text or 'R2:' in text or
                ('FAIL' in text and 'wash_executor_round2' in text) or
                'Tests run:' in text):
                print(text, flush=True)
            if 'Tests run:' in text:
                break
except KeyboardInterrupt:
    pass
finally:
    ser.close()
