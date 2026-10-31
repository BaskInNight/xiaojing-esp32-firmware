#!/usr/bin/env python3
"""Short structured stability probe for Phase 11 R1.1 (non-action).

Opens COM8, watches for DURATION_S seconds, and prints a flushed summary of
reset / WDT / Guru / panic / assert, heartbeat and telemetry liveness, and the
real internal-heap tokens (h=internal free, L=largest internal block) plus
iB=I2S bytes.  Exits cleanly (NOT_COMPLETE=YES if it did not run the full
window).
"""
import re
import serial
import time

PORT = 'COM8'
DURATION_S = 90

RESET_REASONS = {
    0x1: 'POWERON', 0x3: 'SW_RESET', 0x5: 'SW_CPU_RST', 0x6: 'PANIC',
    0x7: 'BROWNOUT', 0x9: 'DEEPSLEEP', 0xC: 'WDT_RTC', 0xD: 'WDT_TG',
    0xF: 'TG_WDT', 0x10: 'EXT_CPU', 0x11: 'PWR_GLITCH', 0x14: 'SW_SYS_RST',
    0x15: 'USR_WDT', 0x16: 'CORE_WDT', 0x17: 'CACHE_ERR', 0x18: 'STACK_OVF',
}

def classify_reset(text):
    m = re.search(r'rst:0x([0-9a-fA-F]+)\s*\(([^)]+)\)', text)
    if not m:
        return None
    return (int(m.group(1), 16), m.group(2).strip())

def token(text, key):
    m = re.search(r'\b' + key + r'=(\d+)', text)
    return int(m.group(1)) if m else None

def main():
    ser = None
    try:
        ser = serial.Serial(PORT, 115200, timeout=0.5)
    except Exception as exc:
        print('OPEN_FAILED:', exc)
        return 2

    start = time.time()
    reset_by_reason = {}
    mid_resets = 0
    initial_poweron = False
    wdt = guru = panic = aborts = asserts = 0
    heartbeat_ticks = []
    telemetry = 0
    h_samples, l_samples, ib_samples = [], [], []

    buf = b''
    try:
        while time.time() - start < DURATION_S:
            chunk = ser.read(4096)
            if not chunk:
                continue
            buf += chunk
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                text = line.decode('utf-8', errors='replace')
                rst = classify_reset(text)
                if rst:
                    name = rst[1]
                    reset_by_reason[name] = reset_by_reason.get(name, 0) + 1
                    if not initial_poweron:
                        initial_poweron = True
                    else:
                        mid_resets += 1
                    if 'wdt' in name.lower():
                        wdt += 1
                if 'rst:' not in text and 'watchdog' in text.lower():
                    wdt += 1
                if 'Guru Meditation' in text:
                    guru += 1
                if 'panic' in text.lower() or 'abort()' in text:
                    panic += 1
                if 'assert' in text.lower() and 'assert' in text:
                    asserts += 1
                if 'heartbeat tick=' in text:
                    t = token(text, 'tick')
                    if t is not None:
                        heartbeat_ticks.append(t)
                if 'UI_SENSOR' in text or 'TELEMETRY' in text:
                    telemetry += 1
                h = token(text, 'h')
                if h is not None:
                    h_samples.append(h)
                L = token(text, 'L')
                if L is not None:
                    l_samples.append(L)
                ib = token(text, 'iB')
                if ib is not None:
                    ib_samples.append(ib)
    except KeyboardInterrupt:
        print('interrupted by user', flush=True)
    finally:
        try:
            ser.close()
        except Exception:
            pass

    elapsed = int(time.time() - start)
    complete = elapsed >= DURATION_S

    def rng(s):
        return None if not s else (s[0], s[-1], min(s))

    hr = rng(h_samples)
    lr = rng(l_samples)
    ir = rng(ib_samples)
    print('=== PROBE RESULT ===')
    print(f'PROBE_DURATION_S={elapsed} (target {DURATION_S})')
    print(f'NOT_COMPLETE={ "NO" if complete else "YES" }')
    print(f'RESET_BY_REASON={reset_by_reason}')
    print(f'MID_MONITOR_RESETS={mid_resets}')
    print(f'INITIAL_POWERON_SEEN={initial_poweron}')
    print(f'WDT_COUNT={wdt}')
    print(f'GURU_COUNT={guru}')
    print(f'PANIC_ABORT_COUNT={panic}')
    print(f'ASSERT_COUNT={asserts}')
    print(f'HEARTBEAT_TICKS={heartbeat_ticks[:6]}...count={len(heartbeat_ticks)}')
    print(f'TELEMETRY_COUNT={telemetry}')
    print(f'INTERNAL_FREE_START_END_MIN={hr}')
    print(f'INTERNAL_LARGEST_START_END_MIN={lr}')
    print(f'I2S_BYTES_START_END={ (ir[0], ir[-1]) if ir else None }')
    print('=== PROBE END ===', flush=True)
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
