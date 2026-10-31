# capture_bench6.py - live serial capture (600s) for bench run after calibration
import serial, time
PORT = "COM8"; BAUD = 115200; DURATION_S = 600
OUT = "D:/UserData/ESP32Project/IDF/ESP32-IDF-Test/build_phase11_c_bench6.log"
MB_OUT = "D:/UserData/ESP32Project/IDF/ESP32-IDF-Test/build_phase11_c_bench6_mb.log"
ser = serial.Serial(PORT, BAUD, timeout=1)
print(f"[bench6] capturing {DURATION_S}s -> {OUT}", flush=True)
t0 = time.time(); buf = b""
with open(OUT, "wb") as f, open(MB_OUT, "wb") as mf:
    while True:
        if time.time() - t0 >= DURATION_S: break
        data = ser.read(4096)
        if data:
            f.write(data); f.flush(); buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                if (b"motor_bench" in line or b"[MB]" in line or b"MB " in line
                        or b"machine_config" in line or b"Direction calibration"
                        or b"bl50" in line or b"position" in line
                        or b"real_hal" in line or b"safety_mgr" in line
                        or b"ibt2" in line):
                    mf.write(line + b"\n"); mf.flush()
ser.close()
print(f"\n[bench6] done, wrote {OUT}", flush=True)
