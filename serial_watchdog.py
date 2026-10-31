"""Non-resetting COM8 watchdog for long-running ESP32 diagnosis."""
import re
import serial
import time
import os
import sys

# Windows consoles commonly use GBK; ESP-SR may emit UTF-8/garbled bytes.
# Never let console encoding terminate the watchdog before a real device
# failure is observed.
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

PORT = "COM8"
BAUD = 115200
DURATION_S = int(os.environ.get("WATCHDOG_DURATION_S", "1800"))
IDLE_ALERT_S = 5
OUT = "phase9_serial_watchdog.log"
CRASH = re.compile(
    r"Guru|Backtrace|rst:|WDT|abort|CORRUPT|assert|LoadProhibited|"
    r"StoreProhibited|stack overflow|panic",
    re.IGNORECASE,
)

ser = serial.Serial(PORT, BAUD, timeout=0.25, dsrdtr=False, rtscts=False)
ser.setDTR(False)
ser.setRTS(False)
start = time.monotonic()
last_rx = start
with open(OUT, "w", encoding="utf-8", errors="replace") as log:
    log.write("WATCHDOG_START\n")
    log.flush()
    while time.monotonic() - start < DURATION_S:
        raw = ser.readline()
        now = time.monotonic()
        if raw:
            last_rx = now
            text = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            line = f"{text}\n"
            print(text, flush=True)
            log.write(line)
            if CRASH.search(text):
                print(f"WATCHDOG_CRASH_TOKEN: {text}", flush=True)
            log.flush()
        elif now - last_rx >= IDLE_ALERT_S:
            alert = f"SERIAL_IDLE seconds={now - last_rx:.1f}\n"
            print(alert.rstrip(), flush=True)
            log.write(alert)
            log.flush()
            last_rx = now
ser.close()
print(f"WATCHDOG_DONE {OUT}", flush=True)
