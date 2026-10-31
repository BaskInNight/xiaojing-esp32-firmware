#!/usr/bin/env python3
"""
Phase 9 Auto-Flash + Soak launcher.
Polls for ESP32-S3 COM port, then flashes and starts soak monitor.
"""
import subprocess
import time
import re
import sys
import os
from datetime import datetime

PROJECT_DIR = r"D:\UserData\ESP32Project\IDF\ESP32-IDF-Test"
POLL_INTERVAL = 30  # seconds
MAX_WAIT = 14400    # 4 hours
SOAK_SCRIPT = os.path.join(PROJECT_DIR, "tools", "phase9_soak_monitor.py")
BUILD_DIR = os.path.join(PROJECT_DIR, "build")

def find_port():
    """Try esptool to find ESP32 port."""
    try:
        r = subprocess.run(
            ["python", "-m", "esptool", "chip_id"],
            capture_output=True, text=True, timeout=15, cwd=PROJECT_DIR
        )
        combined = r.stdout + r.stderr
        m = re.search(r"(COM\d+)", combined)
        if m:
            return m.group(1)
    except:
        pass

    # Fallback: scan all COM ports for USB serial
    try:
        r = subprocess.run(
            ["powershell", "-Command",
             "Get-CimInstance Win32_SerialPort | Select-Object -ExpandProperty DeviceID"],
            capture_output=True, text=True, timeout=10
        )
        ports = [l.strip() for l in r.stdout.split() if l.strip().startswith("COM")]
        for p in ports:
            # Try opening with esptool chip_id on each port
            try:
                r2 = subprocess.run(
                    ["python", "-m", "esptool", "-p", p, "chip_id"],
                    capture_output=True, text=True, timeout=8, cwd=PROJECT_DIR
                )
                if "ESP32" in (r2.stdout + r2.stderr):
                    return p
            except:
                continue
    except:
        pass

    return None


def flash(port):
    """Flash using idf.py."""
    print(f"[{datetime.now().strftime('%H:%M:%S')}] Flashing to {port}...")
    # Activate ESP-IDF and flash
    ps_script = f'''
. "{os.path.join(PROJECT_DIR, '.claude', 'esp-idf-activate.ps1')}"
Set-Location "{PROJECT_DIR}"
idf.py -p {port} flash 2>&1
'''
    r = subprocess.run(
        ["powershell", "-Command", ps_script],
        capture_output=True, text=True, timeout=300, cwd=PROJECT_DIR
    )
    output = r.stdout + r.stderr
    print(output[-2000:] if len(output) > 2000 else output)
    return r.returncode == 0


def main():
    print(f"Phase 9 Auto-Flash+Soak")
    print(f"Polling every {POLL_INTERVAL}s for ESP32-S3 (max {MAX_WAIT}s)")
    start = time.time()

    while (time.time() - start) < MAX_WAIT:
        port = find_port()
        if port:
            print(f"\n[{datetime.now().strftime('%H:%M:%S')}] Found ESP32-S3 on {port}")

            # Flash
            if not flash(port):
                print("[FLASH] Failed. Retrying in 30s...")
                time.sleep(30)
                if not flash(port):
                    print("[FLASH] Failed twice. Waiting for next detection...")
                    time.sleep(POLL_INTERVAL)
                    continue

            # Wait for boot
            time.sleep(5)

            # Run soak monitor
            print(f"[SOAK] Starting soak monitor...")
            r = subprocess.run(
                [sys.executable, SOAK_SCRIPT],
                cwd=PROJECT_DIR,
                timeout=SOAK_DURATION_SEC + 300
            )
            print(f"[SOAK] Monitor exited with code {r.returncode}")
            return r.returncode

        elapsed = int(time.time() - start)
        remaining = MAX_WAIT - elapsed
        print(f"[{datetime.now().strftime('%H:%M:%S')}] No board found. "
              f"Waiting... ({elapsed}s elapsed, {remaining}s remaining)")
        time.sleep(POLL_INTERVAL)

    print("[TIMEOUT] Board never connected.")
    return 1


SOAK_DURATION_SEC = 4 * 3600  # 4 hours

if __name__ == "__main__":
    sys.exit(main())
