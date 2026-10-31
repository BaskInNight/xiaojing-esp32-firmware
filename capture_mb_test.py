# capture_mb_test.py - live serial capture to diagnose motor_bench FAULT (terminal 5)
# Records raw COM8 for a window; user reproduces the bench fault during it.
import serial, time, sys

PORT = "COM8"
BAUD = 115200
DURATION_S = 300
OUT = "build_phase11_c_mbtest.log"
MB_OUT = "build_phase11_c_mbtest_mb.log"

ser = serial.Serial(PORT, BAUD, timeout=1)
print(f"[mbcap] capture on {PORT} for {DURATION_S}s -> {OUT}", flush=True)
t0 = time.time()
buf = b""
with open(OUT, "wb") as f, open(MB_OUT, "wb") as mf:
    while True:
        now = time.time()
        if now - t0 >= DURATION_S:
            break
        data = ser.read(4096)
        if data:
            f.write(data)
            f.flush()
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()
            buf += data
            # flush buffered complete lines
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                if b"[motor_bench]" in line or b"[MB]" in line or b"MB " in line:
                    mf.write(line + b"\n")
                    mf.flush()
ser.close()
print(f"\n[mbcap] done, wrote {OUT}", flush=True)
