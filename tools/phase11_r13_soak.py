#!/usr/bin/env python3
"""Phase 11 R1.3 — 4-hour passive stability soak monitor (no actuator commands).

Opens COM8, timestamp-tags every serial line into the log, and every 60 seconds
writes a structured summary of:
  - elapsed / uptime heartbeat ticks
  - reset reason, WDT / Guru / Panic / Assert counters
  - internal free heap + largest block (h=/L=) and I2S bytes (iB=)
  - Wi-Fi / BLE link state
  - water / detergent / UV / actuator self-test activity (must stay idle)
  - proof that no actuator was commanded ON (ACT_TEST open count == 0)

Safety: does NOT send any command, does NOT reboot/retry/mask a failure.  If an
unsafe signature appears it is logged as an ALERT line and the run continues so
the evidence is preserved for the report.

Run:  python tools/phase11_r13_soak.py <duration_s> <log_path>
"""

import re
import sys
import time
import serial

PORT = 'COM8'
BAUD = 115200
SUMMARY_EVERY_S = 60

RESET_REASONS = {
    0x1: 'POWERON', 0x3: 'SW_RESET', 0x5: 'SW_CPU_RST', 0x6: 'PANIC',
    0x7: 'BROWNOUT', 0x9: 'DEEPSLEEP', 0xC: 'WDT_RTC', 0xD: 'WDT_TG',
    0xF: 'TG_WDT', 0x10: 'EXT_CPU', 0x11: 'PWR_GLITCH', 0x14: 'SW_SYS_RST',
    0x15: 'USR_WDT', 0x16: 'CORE_WDT', 0x17: 'CACHE_ERR', 0x18: 'STACK_OVF',
}


def now_tag():
    t = time.localtime()
    ms = int((time.time() % 1.0) * 1000)
    return '%02d:%02d:%02d.%03d' % (t.tm_hour, t.tm_min, t.tm_sec, ms)


def token(text, key):
    m = re.search(r'\b' + key + r'=(\d+)', text)
    return int(m.group(1)) if m else None


def main():
    duration_s = int(sys.argv[1]) if len(sys.argv) > 1 else 14400
    out_path = sys.argv[2] if len(sys.argv) > 2 else 'build_phase11_r13_soak.log'
    try:
        ser = serial.Serial(PORT, BAUD, timeout=0.5)
    except Exception as exc:
        print('OPEN_FAILED:', exc)
        return 2
    out = open(out_path, 'a', encoding='utf-8', buffering=1)

    start = time.time()
    counters = {'reset': {}, 'mid_reset': 0, 'wdt': 0, 'guru': 0, 'panic': 0,
                'assert': 0, 'act_open': 0, 'act_selftest': 0, 'uv_selftest': 0,
                'emergency': 0, 'fault': 0, 'output_on_evidence': 0}
    heartbeat_ticks = []
    h_samples, l_samples, ib_samples = [], [], []
    wifi_state, ble_state = '?', '?'
    alerts = []
    last_summary = 0
    buf = b''

    def tag():
        return '[%s] ' % now_tag()

    out.write(tag() + 'SOAK_START duration_s=%d\n' % duration_s)

    try:
        while time.time() - start < duration_s:
            chunk = ser.read(4096)
            if not chunk:
                continue
            buf += chunk
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                text = line.decode('utf-8', errors='replace').rstrip('\r')
                if not text:
                    continue
                out.write(tag() + text + '\n')

                rst = re.search(r'rst:0x([0-9a-fA-F]+)\s*\(([^)]+)\)', text)
                if rst:
                    code = int(rst.group(1), 16)
                    name = rst.group(2).strip()
                    counters['reset'][name] = counters['reset'].get(name, 0) + 1
                    if len(counters['reset']) > 1 or sum(counters['reset'].values()) > 1:
                        counters['mid_reset'] += 1
                    if 'wdt' in name.lower():
                        counters['wdt'] += 1
                if 'rst:' not in text and 'watchdog' in text.lower():
                    counters['wdt'] += 1
                if 'Guru Meditation' in text:
                    counters['guru'] += 1
                if 'panic' in text.lower() or 'abort()' in text:
                    counters['panic'] += 1
                if 'assert' in text.lower():
                    counters['assert'] += 1
                if 'ACT_TEST open' in text:
                    counters['act_open'] += 1   # actuator energize — unsafe unattended
                if 'ACT_SELFTEST' in text:
                    counters['act_selftest'] += 1
                if 'UV_SELFTEST' in text:
                    counters['uv_selftest'] += 1
                if 'emergency' in text.lower():
                    counters['emergency'] += 1
                if 'FAULT' in text or 'fault' in text:
                    counters['fault'] += 1
                if 'heartbeat tick=' in text:
                    t = token(text, 'tick')
                    if t is not None:
                        heartbeat_ticks.append(t)
                if re.search(r'\bWIFI_STA:', text):
                    wifi_state = text.split('WIFI_STA:')[-1][:40].strip()
                if 'ble' in text.lower() and ('connected' in text.lower() or
                                              'running' in text.lower()):
                    ble_state = text.strip()[:40]
                h = token(text, 'h')
                if h is not None:
                    h_samples.append(h)
                L = token(text, 'L')
                if L is not None:
                    l_samples.append(L)
                ib = token(text, 'iB')
                if ib is not None:
                    ib_samples.append(ib)

            now = time.time()
            if now - last_summary >= SUMMARY_EVERY_S:
                last_summary = now
                elapsed = int(now - start)
                hh, mm = divmod(elapsed, 3600)
                mm, ss = divmod(mm, 60)
                def rng(s):
                    return (s[0], s[-1], min(s)) if s else None
                hr = rng(h_samples); lr = rng(l_samples); ir = rng(ib_samples)
                out.write(tag() + 'SOAK_SUMMARY elapsed=%02d:%02d:%02d '
                          'hb=%d hb_last=%s reset=%s wdt=%d guru=%d panic=%d '
                          'assert=%d act_open=%d act_st=%d uv_st=%d emerg=%d '
                          'fault=%d h_range=%s L_range=%s iB_range=%s '
                          'wifi=%s ble=%s\n' % (
                              hh, mm, ss, len(heartbeat_ticks),
                              heartbeat_ticks[-1] if heartbeat_ticks else '?',
                              counters['reset'], counters['wdt'],
                              counters['guru'], counters['panic'],
                              counters['assert'], counters['act_open'],
                              counters['act_selftest'], counters['uv_selftest'],
                              counters['emergency'], counters['fault'],
                              hr, lr, ir, wifi_state, ble_state))
                unsafe = (counters['reset'] or counters['wdt'] or
                          counters['guru'] or counters['panic'] or
                          counters['assert'] or counters['act_open'] or
                          counters['emergency'])
                if unsafe:
                    msg = 'UNSAFE_SIGNATURE counters=%s' % counters
                    alerts.append(msg)
                    out.write(tag() + 'ALERT ' + msg + '\n')
    except KeyboardInterrupt:
        out.write(tag() + 'SOAK_INTERRUPTED\n')
    finally:
        elapsed = int(time.time() - start)
        out.write(tag() + 'SOAK_END elapsed_s=%d resets=%s wdt=%d guru=%d '
                  'panic=%d assert=%d act_open=%d act_st=%d uv_st=%d emerg=%d '
                  'hb=%d\n' % (elapsed, counters['reset'], counters['wdt'],
                               counters['guru'], counters['panic'],
                               counters['assert'], counters['act_open'],
                               counters['act_selftest'], counters['uv_selftest'],
                               counters['emergency'], len(heartbeat_ticks)))
        try:
            ser.close()
        except Exception:
            pass
        try:
            out.close()
        except Exception:
            pass
    print('SOAK_FINISHED elapsed=%d alerts=%d' % (elapsed, len(alerts)))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
