# capture_mb_test2.py - live serial capture (10 min) for motor_bench live run evidence
# Broader filter: motor_bench MB lines + safety/mcp/real_hal context.
import serial, time, sys

PORT = "COM8"
BAUD = 115200
DURATION_S = 600
OUT = "build_phase11_c_mbtest2.log"
MB_OUT = "build_phase11_c_mbtest2_mb.log"

ser = serial.Serial(PORT, BAUD, timeout=1)
print(f"[mbcap2] capture on {PORT} for {DURATION_S}s -> {OUT}", flush=True)
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
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                if (b"motor_bench" in line or b"[MB]" in line or b"MB " in line
                        or b"safety_mgr" in line or b"mcp23017" in line
                        or b"real_hal" in line or b"bl50" in line
                        or b"position" in line):
                    mf.write(line + b"\n")
                    mf.flush()
ser.close()
print(f"\n[mbcap2] done, wrote {OUT}", flush=True)
