"""
监视 COM8 串口日志，发现 pos_swing / 启动 / 故障关键行时输出。
用法: python mon8.py [--duration N] [--nonewline]
"""
import sys, time, serial

PORT = 'COM8'
BAUD = 115200
DURATION = int(sys.argv[sys.argv.index('--duration')+1]) if '--duration' in sys.argv else 600
KEY = ['pos_swing', 'PS_', 'pos_swing', 'fault', 'Fault', 'FAULT',
       'boot', 'Boot', 'abort', 'assert', 'error', 'panic', 'Rebooting',
       'motor_bench', 'capabilities', 'capabilities published',
       'Guru Meditation', 'Error', 'invalid', 'exception']
PROGRESS = ['Connecting', 'Hash of data verified', 'Chip is ESP32']
last_key = 0

print(f"[mon] COM8 @ {BAUD}  duration={DURATION}s  (Ctrl+C 结束)")
print("[mon] 匹配关键词: " + ", ".join(sorted(set(KEY))), flush=True)

start = time.time()
first_boot_seen = False
while time.time() - start < DURATION:
    try:
        import serial
        break
    except Exception:
        pass

import serial
try:
    ser = serial.Serial(PORT, BAUD, timeout=0.2)
except Exception as e:
    print("open failed:", e); sys.exit(1)

while time.time() - start < DURATION:
    try:
        raw = ser.read(4096)
        if raw:
            txt = raw.decode('utf-8', errors='replace')
            lines = txt.splitlines()
            for ln in lines:
                lnl = ln.lower()
                if any(k.lower() in lnl for k in KEY):
                    print(f"[{time.time()-start:7.1f}s] {ln}", flush=True)
                elif 'capabilities' in lnl or 'pos_swing' in lnl:
                    print(f"[{time.time()-start:7.1f}s] {ln}", flush=True)
    except Exception as e:
        print("read err:", e); break
    time.sleep(0.05)
print("\n[monitor] done.", flush=True)
ser.close()