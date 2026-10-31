# passive_monitor.py - passive serial capture for soak (no reset, no injection)
import serial, time, sys

PORT = "COM8"
BAUD = 115200
DURATION_S = 32 * 60   # 32 minutes
OUT = "build_phase11_c_soak.log"
HEARTBEAT_S = 30

ser = serial.Serial(PORT, BAUD, timeout=1)
print(f"[soak] passive capture on {PORT} for {DURATION_S}s -> {OUT}", flush=True)
t0 = time.time()
last_hb = t0
with open(OUT, "wb") as f:
    while True:
        now = time.time()
        if now - t0 >= DURATION_S:
            break
        if now - last_hb >= HEARTBEAT_S:
            line = f"\n[soak] t={int(now - t0)}s elapsed\n"
            f.write(line.encode())
            f.flush()
            print(f"[soak] t={int(now - t0)}s", flush=True)
            last_hb = now
        data = ser.read(4096)
        if data:
            f.write(data)
            f.flush()
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()
ser.close()
print(f"\n[soak] done, wrote {OUT}", flush=True)
