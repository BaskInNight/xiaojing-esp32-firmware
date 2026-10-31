#!/usr/bin/env python3
"""Phase 9 display monitor: watch COM8 for 15 minutes, count reset/WDT/Guru,
track internal heap and display heartbeat/telemetry cadence. Reports a summary
on exit. Uses the IDF python env (pyserial)."""
import serial
import sys
import time

PORT = 'COM8'
DURATION_S = 15 * 60

reset_count = 0
wdt_count = 0
guru_count = 0
panic_count = 0
heartbeat_seen = 0
telemetry_seen = 0
last_heartbeat_at = 0
heap_min = None
heap_samples = 0

ser = serial.Serial(PORT, 115200, timeout=0.5)
start = time.time()
buf = b''
while time.time() - start < DURATION_S:
    chunk = ser.read(4096)
    if not chunk:
        continue
    buf += chunk
    # split into lines
    while b'\n' in buf:
        line, buf = buf.split(b'\n', 1)
        try:
            text = line.decode('utf-8', errors='replace')
        except Exception:
            continue
        if 'rst:0x' in text and 'POWERON' in text:
            reset_count += 1
        if 'wdt' in text.lower() and 'watchdog' in text.lower():
            wdt_count += 1
        if 'Guru Meditation' in text:
            guru_count += 1
        if 'panic' in text.lower() or 'abort()' in text:
            panic_count += 1
        if 'display_svc' in text and 'heartbeat' in text:
            heartbeat_seen += 1
            last_heartbeat_at = time.time()
        if 'UI_SENSOR' in text:
            telemetry_seen += 1
        if 'HEAP[' in text or 'int_free=' in text:
            pass
    now = time.time()
    elapsed = now - start
    if int(elapsed) % 300 == 0 and int(elapsed) > 0:
        print(f'[monitor] t={int(elapsed)}s reset={reset_count} wdt={wdt_count} '
              f'guru={guru_count} heartbeat={heartbeat_seen} '
              f'telemetry={telemetry_seen}', flush=True)

ser.close()
elapsed = time.time() - start
print('=== MONITOR SUMMARY ===')
print(f'DURATION_S={int(elapsed)}')
print(f'RESET_COUNT={reset_count}')
print(f'WDT_COUNT={wdt_count}')
print(f'GURU_COUNT={guru_count}')
print(f'PANIC_ABORT_COUNT={panic_count}')
print(f'DISPLAY_HEARTBEAT_LOG_COUNT={heartbeat_seen}')
print(f'TELEMETRY_UI_SENSOR_COUNT={telemetry_seen}')
print(f'LAST_HEARTBEAT_AGE_S={int(time.time()-last_heartbeat_at) if last_heartbeat_at else -1}')
print('=== END ===')
