# capture_coldboot.py - hard-reset ESP32-S3 and capture serial boot log
import serial, time, sys

PORT = "COM8"
BAUD = 115200
DURATION_S = 45
OUT = "build_phase11_c_coldboot.log"

ser = serial.Serial(PORT, BAUD, timeout=1)
# CH340 auto-reset: RTS -> EN. Pull EN low, release to hard-reset.
ser.setDTR(False)
ser.setRTS(True)   # EN low (assert)
time.sleep(0.2)
ser.setRTS(False)  # EN high (release) -> cold boot
print(f"[capture] reset asserted, capturing {DURATION_S}s to {OUT}", flush=True)

t0 = time.time()
with open(OUT, "wb") as f:
    while time.time() - t0 < DURATION_S:
        data = ser.read(4096)
        if data:
            f.write(data)
            f.flush()
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()

ser.close()
print(f"\n[capture] done, wrote {OUT}", flush=True)
