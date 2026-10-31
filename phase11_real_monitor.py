"""Phase 11 Group B REAL-mode cold-boot + passive monitor capture.

Opens COM8, issues a best-effort classic auto-reset pulse (RTS toggle -> EN),
then captures timestamped serial output for REAL_MONITOR_S seconds.  Writes the
full stream to phase11_real_monitor.log, prints crash tokens and key boot
markers live, and prints REAL_MONITOR_DONE at completion.

Use: REAL_MONITOR_S=1980 python phase11_real_monitor.py   (33 min default)
"""
import os
import re
import serial
import sys
import time

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

PORT = "COM8"
BAUD = 115200
DURATION_S = int(os.environ.get("REAL_MONITOR_S", "1980"))
OUT = "phase11_real_monitor.log"

# Boot markers relevant to the cold-start safe-OFF proof and stability.
KEYS = (
    "MCP23017 initialized",
    "Initialized (mode=",
    "Running (MCP baseline OK)",
    "hot_air",
    "HOT AIR",
    "PTC",
    "relay",
    "safe",
    "Guru Meditation",
    "Backtrace",
    "rst:",
    "abort",
    "panic",
    "CORRUPT",
    "LoadProhibited",
    "StoreProhibited",
    "stack overflow",
    "WDT",
)
CRASH = re.compile(
    r"Guru|Backtrace|rst:0x|abort|CORRUPT|LoadProhibited|StoreProhibited|"
    r"stack overflow|panic|assert failed|task_wdt|watchdog",
    re.IGNORECASE,
)

ser = serial.Serial(PORT, BAUD, timeout=0.25, dsrdtr=False, rtscts=False)
# Best-effort classic auto-reset pulse: RTS asserted pulls EN low (reset),
# de-asserted lets EN float high (run).  DTR left high so GPIO0 stays in
# normal boot.  If the board's auto-reset circuit differs, this is a no-op
# and the flash-triggered boot is still captured.
try:
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.12)
    ser.setRTS(False)
    time.sleep(0.05)
except Exception:
    pass

start = time.monotonic()
with open(OUT, "w", encoding="utf-8", errors="replace") as f:
    f.write(f"REAL_MONITOR_START duration_s={DURATION_S}\n")
    f.flush()
    while time.monotonic() - start < DURATION_S:
        b = ser.readline()
        if not b:
            continue
        s = b.decode("utf-8", "replace").rstrip()
        if not s:
            continue
        el = time.monotonic() - start
        line = f"[+{el:8.1f}s] {s}"
        f.write(line + "\n")
        f.flush()
        if CRASH.search(s):
            print(f"CRASH_TOKEN: {s}", flush=True)
        elif any(k.lower() in s.lower() for k in KEYS):
            print(line, flush=True)
ser.close()
print(f"REAL_MONITOR_DONE {OUT}", flush=True)
