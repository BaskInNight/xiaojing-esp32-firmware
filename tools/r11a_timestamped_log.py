#!/usr/bin/env python3
"""Phase 11 R1.2 — timestamped COM8 serial evidence logger.

Reads every line from COM8 and appends "[HH:MM:SS.mmm] <line>" to the given log
file (flush per line).  Runs until terminated.  Used to supervise the real
actuator board tests: each user action's evidence (ACT_SELFTEST submit / reject /
terminal, ACT_TEST open/close with OFF-confirmed, resets, WDT, Guru, panic,
heartbeat, D-tokens h/L/iB) is captured with a wall-clock timestamp.
"""
import sys
import time
import serial

PORT = 'COM8'
BAUD = 115200

def now_tag():
    t = time.localtime()
    ms = int((time.time() % 1.0) * 1000)
    return '%02d:%02d:%02d.%03d' % (t.tm_hour, t.tm_min, t.tm_sec, ms)

def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else 'build_phase11_r1_2_hw.log'
    ser = None
    try:
        ser = serial.Serial(PORT, BAUD, timeout=0.5)
    except Exception as exc:
        print('OPEN_FAILED:', exc)
        return 2

    out = open(out_path, 'a', encoding='utf-8', buffering=1)
    print('timestamped logger on %s -> %s' % (PORT, out_path), flush=True)
    buf = b''
    try:
        while True:
            chunk = ser.read(4096)
            if not chunk:
                continue
            buf += chunk
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                text = line.decode('utf-8', errors='replace').rstrip('\r')
                if text:
                    out.write('[%s] %s\n' % (now_tag(), text))
    except KeyboardInterrupt:
        pass
    finally:
        try:
            ser.close()
        except Exception:
            pass
        try:
            out.close()
        except Exception:
            pass
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
