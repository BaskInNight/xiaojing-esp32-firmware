#!/usr/bin/env python3
"""Capture unit test output from ESP32 serial port."""
import serial
import time
import sys

port = sys.argv[1] if len(sys.argv) > 1 else 'COM7'
baud = 115200
timeout = 300  # 5 minutes max

ser = serial.Serial(port, baud, timeout=1)
start = time.time()

try:
    while time.time() - start < timeout:
        line = ser.readline()
        if line:
            try:
                text = line.decode('utf-8', errors='replace').rstrip()
                print(text, flush=True)
                # Check for completion
                if 'Tests run:' in text or 'FAIL' in text or 'COMPLETE' in text:
                    # Keep reading for a bit more
                    time.sleep(2)
                    while ser.readline():
                        text = ser.readline().decode('utf-8', errors='replace').rstrip()
                        if text:
                            print(text, flush=True)
                    break
            except:
                pass
except KeyboardInterrupt:
    pass
finally:
    ser.close()
