# capture_calib5.py - live serial capture (300s) for calibration command + ESP32 restart evidence
import serial, time
PORT = "COM8"; BAUD = 115200; DURATION_S = 300
OUT = "D:/UserData/ESP32Project/IDF/ESP32-IDF-Test/build_phase11_c_calib5.log"
ser = serial.Serial(PORT, BAUD, timeout=1)
print(f"[calib5] capturing {DURATION_S}s -> {OUT}", flush=True)
t0 = time.time()
with open(OUT, "wb") as f:
    while True:
        if time.time() - t0 >= DURATION_S: break
        data = ser.read(4096)
        if data:
            f.write(data); f.flush()
ser.close()
print(f"\n[calib5] done, wrote {OUT}", flush=True)
