#!/usr/bin/env python3
"""Capture full unit test output from ESP32 serial port."""
import serial
import time
import sys

port = sys.argv[1] if len(sys.argv) > 1 else 'COM7'
baud = 115200
timeout = 600  # 10 minutes max

ser = serial.Serial(port, baud, timeout=1)
start = time.time()
found_summary = False

try:
    while time.time() - start < timeout:
        line = ser.readline()
        if line:
            try:
                text = line.decode('utf-8', errors='replace').rstrip()
                if text:
                    print(text, flush=True)
                    # Check for test summary
                    if 'Tests run:' in text or 'FAILURES' in text:
                        found_summary = True
                    if found_summary and time.time() - start > timeout - 10:
                        break
            except:
                pass
except KeyboardInterrupt:
    pass
finally:
    ser.close()
