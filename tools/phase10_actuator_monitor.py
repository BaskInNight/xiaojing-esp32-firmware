#!/usr/bin/env python3
"""Phase 10/11 monitor: watch COM8 for 15 minutes after the actuator-selftest /
3-page display firmware.

Correctly classifies the firmware's diagnostic log tokens:
  D: h=<internal free> L=<largest internal block> i2s=<ok> iB=<I2S bytes> ...
  HEAP[tag] int_free=<n> int_largest=<n> int_min=<n> psram=<n>

`iB` is I2S BYTES, never internal heap.  Reset stats classify every `rst:0x`
reason and separate the initial power-on from resets that appear during
monitoring.  Exits with NOT_COMPLETE=YES when it did not run the full
duration (Ctrl-C / user termination / serial drop).
"""
import re
import serial
import sys
import time

PORT = 'COM8'
DURATION_S = 10 * 60

RESET_REASON_NAMES = {
    0x1: 'POWERON',
    0x3: 'SW_RESET',        # TG0WDT_SYS_RST / software system reset
    0x5: 'SW_CPU_RST',      # SW_CPU_RESET
    0x6: 'PANIC',           # PANIC / deep sleep wake is 0x9, brownout 0x7...
    0x7: 'BROWNOUT',
    0x9: 'DEEPSLEEP',
    0xC: 'WDT_RTC',
    0xD: 'WDT_TG',          # TG1WDT_SYS_RST
    0xF: 'TG_WDT',          # TG0WDT_SYS_RST
    0x10: 'EXT_CPU',        # EXTERNAL_CPU_RESET
    0x11: 'PWR_GLITCH',
    0x14: 'SW_SYS_RST',
    0x15: 'USR_WDT',
    0x16: 'CORE_WDT',
    0x17: 'CACHE_ERR',
    0x18: 'STACK_OVF',
}

def classify_reset(text):
    """Return (code, name) for an `rst:0xNN (NAME)` line, or None."""
    m = re.search(r'rst:0x([0-9a-fA-F]+)\s*\(([^)]+)\)', text)
    if not m:
        return None
    code = int(m.group(1), 16)
    raw = m.group(2).strip()
    return (code, raw)

def parse_u64_token(text, token):
    """Parse `token=NNN` in a line, returning int or None."""
    m = re.search(r'\b' + token + r'=(\d+)', text)
    if not m:
        return None
    try:
        return int(m.group(1))
    except ValueError:
        return None

def main():
    reset_by_reason = {}     # reason name -> count
    reset_order = []         # names in order seen
    initial_poweron_seen = False
    mid_monitor_resets = 0   # resets observed after the first (power-on)
    wdt_count = 0
    guru_count = 0
    panic_count = 0
    assert_count = 0
    heartbeat_ts = []
    telemetry_ts = []
    int_free_samples = []
    int_largest_samples = []
    psram_samples = []
    i2s_bytes_samples = []
    i2s_ok_count = 0
    wifi_updates = 0
    ble_updates = 0

    try:
        ser = serial.Serial(PORT, 115200, timeout=0.5)
    except Exception as exc:
        print('=== MONITOR ERROR ===')
        print(f'OPEN_FAILED: {exc}')
        print('=== END ===')
        sys.exit(2)

    start = time.time()
    buf = b''
    try:
        while time.time() - start < DURATION_S:
            chunk = ser.read(4096)
            if not chunk:
                continue
            buf += chunk
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                try:
                    text = line.decode('utf-8', errors='replace')
                except Exception:
                    continue
                now = time.time()

                rst = classify_reset(text)
                if rst is not None:
                    code, name = rst
                    if name in reset_by_reason:
                        reset_by_reason[name] += 1
                    else:
                        reset_by_reason[name] = 1
                    reset_order.append(name)
                    if not initial_poweron_seen:
                        initial_poweron_seen = True
                    else:
                        mid_monitor_resets += 1
                    if 'wdt' in name.lower():
                        wdt_count += 1

                # Explicit watchdog notices that are not already a reset line
                # (e.g. "Task watchdog got triggered").  Count once per line.
                if 'rst:' not in text and (
                        'watchdog' in text.lower()
                        or 'task_wdt' in text.lower()
                        or 'wdt timer' in text.lower()):
                    wdt_count += 1
                if 'Guru Meditation' in text:
                    guru_count += 1
                if 'panic' in text.lower() or 'abort()' in text:
                    panic_count += 1
                if 'assert' in text.lower() and 'assert' in text:
                    assert_count += 1

                if 'display_svc' in text and 'heartbeat' in text:
                    heartbeat_ts.append(now)
                if 'UI_SENSOR' in text:
                    telemetry_ts.append(now)

                # D-line diagnostic tokens (h=internal free, L=largest
                # internal block, iB=I2S bytes — NOT heap).
                h = parse_u64_token(text, 'h')
                if h is not None:
                    int_free_samples.append(h)
                L = parse_u64_token(text, 'L')
                if L is not None:
                    int_largest_samples.append(L)
                iB = parse_u64_token(text, 'iB')
                if iB is not None:
                    i2s_bytes_samples.append(iB)
                i2s = parse_u64_token(text, 'i2s')
                if i2s is not None and i2s:
                    i2s_ok_count += 1

                # HEAP[tag] lines.
                hf = parse_u64_token(text, 'int_free')
                if hf is not None:
                    int_free_samples.append(hf)
                hl = parse_u64_token(text, 'int_largest')
                if hl is not None:
                    int_largest_samples.append(hl)
                ps = parse_u64_token(text, 'psram')
                if ps is not None:
                    psram_samples.append(ps)

                if 'GOT_IP' in text or ('WIFI' in text and 'state' in text):
                    wifi_updates += 1
                if 'BLE' in text and ('connected' in text or 'advertis' in text):
                    ble_updates += 1

            elapsed = int(time.time() - start)
            if elapsed % 300 == 0 and elapsed > 0:
                print(f'[monitor] t={elapsed}s resets={len(reset_order)} '
                      f'wdt={wdt_count} guru={guru_count} '
                      f'heartbeat={len(heartbeat_ts)} telemetry={len(telemetry_ts)} '
                      f'h_samples={len(int_free_samples)}', flush=True)
    except KeyboardInterrupt:
        print('\n[monitor] interrupted by user', flush=True)
    finally:
        try:
            ser.close()
        except Exception:
            pass

    elapsed = int(time.time() - start)
    complete = elapsed >= DURATION_S

    def sample_range(samples):
        if not samples:
            return None
        return (samples[0], samples[-1], min(samples))

    h_range = sample_range(int_free_samples)
    L_range = sample_range(int_largest_samples)
    ps_range = sample_range(psram_samples)
    i2s_range = (i2s_bytes_samples[0], i2s_bytes_samples[-1]) if i2s_bytes_samples else None

    def max_gap(ts_list):
        if len(ts_list) < 2:
            return None
        gaps = [ts_list[i + 1] - ts_list[i] for i in range(len(ts_list) - 1)]
        return int(max(gaps)) if gaps else None

    hb_gap = max_gap(heartbeat_ts)
    te_gap = max_gap(telemetry_ts)

    def fmt_range(prefix, r, suffix=''):
        if r is None:
            print(f'{prefix}_START=-- {prefix}_END=-- {prefix}_MIN=--')
        else:
            print(f'{prefix}_START={r[0]}{suffix} {prefix}_END={r[1]}{suffix} '
                  f'{prefix}_MIN={r[2]}{suffix}')

    print('=== MONITOR SUMMARY ===')
    print(f'DURATION_S={elapsed}')
    print(f'NOT_COMPLETE={"NO" if complete else "YES"}'
          f' (target {DURATION_S}s)')
    print(f'INITIAL_POWERON_SEEN={"YES" if initial_poweron_seen else "NO"}')
    print(f'RESET_BY_REASON={reset_by_reason}')
    print(f'MID_MONITOR_RESETS={mid_monitor_resets}')
    print(f'WDT_COUNT={wdt_count}')
    print(f'GURU_COUNT={guru_count}')
    print(f'PANIC_ABORT_COUNT={panic_count}')
    print(f'ASSERT_COUNT={assert_count}')
    print(f'DISPLAY_HEARTBEAT_LOG_COUNT={len(heartbeat_ts)}')
    print(f'TELEMETRY_UI_SENSOR_COUNT={len(telemetry_ts)}')
    print(f'WIFI_LINK_UPDATES={wifi_updates}')
    print(f'BLE_LINK_UPDATES={ble_updates}')
    print(f'DISPLAY_HEARTBEAT_MAX_GAP_S={hb_gap if hb_gap is not None else -1}')
    print(f'TELEMETRY_MAX_GAP_S={te_gap if te_gap is not None else -1}')
    fmt_range('INTERNAL_FREE', h_range)
    fmt_range('INTERNAL_LARGEST', L_range)
    fmt_range('PSRAM_FREE', ps_range)
    if i2s_range is not None:
        print(f'I2S_BYTES_START={i2s_range[0]} I2S_BYTES_END={i2s_range[1]}')
    else:
        print('I2S_BYTES_START=-- I2S_BYTES_END=--')
    print(f'I2S_READ_OK_SAMPLES={i2s_ok_count}')
    print('=== END ===')

if __name__ == '__main__':
    main()
