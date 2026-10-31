#!/usr/bin/env python3
"""Capture r2integ diagnostic lines."""
import serial, time
ser = serial.Serial("COM7", 115200, timeout=1)
start = time.time()
try:
    while time.time() - start < 600:
        line = ser.readline()
        if line:
            text = line.decode('utf-8', errors='replace').rstrip()
            if 'r2integ' in text or ('R2:' in text and 'FAIL' in text):
                print(text, flush=True)
            if 'Tests run:' in text:
                print(text, flush=True)
                break
except: pass
finally: ser.close()
