# capture_coldboot_calib.py - cold-boot safe-OFF verification capture (60s)
# Reset via EN (RTS) toggle ONLY. File write only (no stdout decode crash).
import serial, time
PORT = "COM8"; BAUD = 115200; DURATION_S = 60
OUT = "D:/UserData/ESP32Project/IDF/ESP32-IDF-Test/build_phase11_c_calib_coldboot.log"
ser = serial.Serial(PORT, BAUD, timeout=1)
ser.setDTR(False); ser.setRTS(False); time.sleep(0.05)
ser.setRTS(True); time.sleep(0.15)
ser.setRTS(False); time.sleep(0.05)
print(f"[coldboot] EN reset issued, capturing {DURATION_S}s -> {OUT}", flush=True)
t0 = time.time()
with open(OUT, "wb") as f:
    while True:
        if time.time() - t0 >= DURATION_S: break
        data = ser.read(4096)
        if data:
            f.write(data); f.flush()
ser.close()
print(f"\n[coldboot] done, wrote {OUT}", flush=True)
