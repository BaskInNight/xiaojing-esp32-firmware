#!/usr/bin/env python3
"""Capture diagnostic output from serial port, filtering for r2integ and dispatch-related lines."""
import serial
import time

PORT = "COM7"
BAUD = 115200
TIMEOUT = 300

ser = serial.Serial(PORT, BAUD, timeout=1)
start = time.time()

try:
    while time.time() - start < TIMEOUT:
        line = ser.readline()
        if line:
            text = line.decode('utf-8', errors='replace').rstrip()
            if text and ('r2integ' in text or 'dispatch' in text or 'FAULT' in text or
                        'submit_program' in text or 'state=' in text or 'Tests run:' in text):
                print(text, flush=True)
            if 'Tests run:' in text:
                break
except KeyboardInterrupt:
    pass
finally:
    ser.close()
