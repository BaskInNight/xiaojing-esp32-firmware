#!/usr/bin/env python3
"""
Phase 9 Long Soak Monitor
- Auto-detects ESP32-S3 COM port
- Flashes firmware
- Monitors serial output for:
  * Boot success (no Guru/WDT/abort)
  * WiFi connected
  * BLE advertising
  * Edge Impulse classifier running
  * ESP-SR AFE initialized
  * Wake detections (both backends)
  * BTN1 backend switches
  * NVS persistence
  * Heap stability
  * Crashes / WDT resets
- Logs everything to file
- Periodically prints summary stats
"""
import sys
import os
import time
import re
import json
import hashlib
import subprocess
import threading
from datetime import datetime, timedelta

# --- Configuration ---
PROJECT_DIR = r"D:\UserData\ESP32Project\IDF\ESP32-IDF-Test"
BUILD_DIR = os.path.join(PROJECT_DIR, "build")
ELF_PATH = os.path.join(BUILD_DIR, "xiaojing_idf.elf")
BIN_PATH = os.path.join(BUILD_DIR, "xiaojing_idf.bin")
LOG_DIR = os.path.join(PROJECT_DIR, "soak_logs")
BAUD = 115200
SOAK_DURATION_HOURS = 4
SUMMARY_INTERVAL_SEC = 120  # Print stats every 2 minutes

# ESP-IDF activation
IDF_ACTIVATE_PS1 = os.path.join(PROJECT_DIR, ".claude", "esp-idf-activate.ps1")

# Known ESP32-S3 USB VID/PID patterns
ESP32_USB_PATTERNS = [
    "CP210", "CH340", "CH910", "FTDI", "UART", "USB Serial",
    "Silicon Labs", "USB-SERIAL", "ESP32", "ESP", "10C4:EA60",
    "1A86:7523", "0403:6001"
]

# --- Regex patterns for serial log analysis ---
PATTERNS = {
    "boot": re.compile(r"ets Jun  8 2016|ESP-ROM:|idf.py"),
    "wifi_connected": re.compile(r"WIFI_STA.*CONNECTED|WIFI_CONNECTED_BIT|ip_event.*GOT_IP|sta.connected.*true"),
    "wifi_disconnected": re.compile(r"WIFI_STA.*DISCONNECTED reason="),
    "wifi_reason": re.compile(r"reason=(\d+)\s+reason_name=(\w+)"),
    "ble_adv": re.compile(r"BLE.*adv_start.*rc=0|ble_gap_adv_start.*rc=0|GAP.*adv.*start"),
    "ble_connect": re.compile(r"BLE.*connect|GAP.*CONNECT|nimble.*connected"),
    "edge_init": re.compile(r"voice_wake_edge.*initialized|EI.*model.*loaded|classifier.*init"),
    "edge_detect": re.compile(r"WAKE_CALLBACK|DETECTED.*xiao.*jing|edge.*wake.*detect"),
    "edge_frames": re.compile(r"DIAG.*FEED.*calls=(\d+).*samples=(\d+)"),
    "esp_sr_init": re.compile(r"ESP-SR AFE initialized|wake_esp_sr.*AFE|srmodel.*loaded"),
    "esp_sr_detect": re.compile(r"DETECTED.*xiao.*ai.*tong.*xue|wakenet.*detected"),
    "esp_sr_feed": re.compile(r"AFE.*feed.*worker"),
    "backend_switch": re.compile(r"voice_frontend.*started backend|voice_frontend.*switch|BTN.*long.*press|backend.*switch"),
    "nvs_write": re.compile(r"nvs.*commit|wake_backend.*saved|NVS.*wake"),
    "heap_info": re.compile(r"internal.*free.*?(\d+)|minimum_free_heap.*?(\d+)|heap_caps.*free.*?(\d+)"),
    "stack_watermark": re.compile(r"stack.*water.*mark.*?(\d+)|high_water.*?(\d+)"),
    "crash_guru": re.compile(r"Guru Meditation|abort\(\)|assert.*fail|LoadProhibited|StoreProhibited|InstrFetchProhibited"),
    "crash_wdt": re.compile(r"Task watchdog|TWDT|WDT.*timeout|wdt.*reset"),
    "panic": re.compile(r"PANIC|panic_handler|backtrace:"),
    "brownout": re.compile(r"Brownout|brownout|BOD"),
    "voice_state": re.compile(r"voice_service.*state|voice.*idle|voice.*listening|voice.*thinking|voice.*reply"),
    "audio_owner": re.compile(r"audio.*owned by|audio.*owner|SINGLE_AUDIO"),
    "security_leak": re.compile(r"(?:api[_-]?key|password|authorization|token)\s*[=:]\s*\S+", re.IGNORECASE),
    "btn1_event": re.compile(r"BTN.*1|button.*id.*0|BUTTON_ID_1"),
    "wifi_scan": re.compile(r"SCAN.*found.*AP|JINGJIE.*found"),
    "display_update": re.compile(r"display.*running|LVGL.*task|display.*refresh"),
    "paj_gesture": re.compile(r"PAJ.*gesture|gesture.*LEFT|gesture.*RIGHT|gesture_service"),
}


def find_esp32_port():
    """Try to find ESP32-S3 COM port using esptool or Windows enumeration."""
    # Method 1: esptool chip_id
    try:
        result = subprocess.run(
            ["python", "-m", "esptool", "chip_id"],
            capture_output=True, text=True, timeout=10
        )
        port_match = re.search(r"(COM\d+)", result.stdout + result.stderr)
        if port_match:
            return port_match.group(1)
    except Exception:
        pass

    # Method 2: PowerShell USB serial
    try:
        ps_cmd = (
            "Get-CimInstance Win32_PnPEntity | "
            "Where-Object { $_.Name -match 'COM\\d+' -and "
            "($_.Name -match 'CP210|CH340|CH910|FTDI|UART|USB.*Serial|Silicon.*Labs|ESP') } | "
            "ForEach-Object { if ($_.Name -match '(COM\\d+)') { $matches[1] } }"
        )
        result = subprocess.run(
            ["powershell", "-Command", ps_cmd],
            capture_output=True, text=True, timeout=15
        )
        for line in result.stdout.strip().split("\n"):
            port = line.strip()
            if port.startswith("COM"):
                return port
    except Exception:
        pass

    # Method 3: enumerate all COM ports
    try:
        result = subprocess.run(
            ["powershell", "-Command",
             "Get-CimInstance Win32_SerialPort | Select-Object -ExpandProperty DeviceID"],
            capture_output=True, text=True, timeout=10
        )
        for line in result.stdout.strip().split("\n"):
            port = line.strip()
            if port.startswith("COM"):
                return port
    except Exception:
        pass

    return None


def flash_firmware(port):
    """Flash firmware to ESP32-S3."""
    print(f"[FLASH] Flashing to {port}...")
    # Use esptool directly for reliability
    cmd = [
        "python", "-m", "esptool",
        "--chip", "esp32s3",
        "-p", port,
        "-b", "460800",
        "--before", "default_reset",
        "--after", "hard_reset",
        "write_flash",
        "--flash_mode", "dio",
        "--flash_size", "16MB",
        "--flash_freq", "80m",
        "0x0", os.path.join(BUILD_DIR, "bootloader", "bootloader.bin"),
        "0x8000", os.path.join(BUILD_DIR, "partition_table", "partition-table.bin"),
        "0x11000", os.path.join(BUILD_DIR, "ota_data_initial.bin"),
        "0x20000", BIN_PATH,
        "0x9a0000", os.path.join(BUILD_DIR, "srmodels", "srmodels.bin"),
    ]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        if result.returncode == 0:
            print(f"[FLASH] Success!")
            return True
        else:
            print(f"[FLASH] Failed: {result.stderr[-500:]}")
            return False
    except Exception as e:
        print(f"[FLASH] Error: {e}")
        return False


class SoakStats:
    def __init__(self):
        self.lock = threading.Lock()
        self.start_time = datetime.now()
        self.counts = {k: 0 for k in [
            "wifi_connected", "wifi_disconnected", "ble_adv", "ble_connect",
            "edge_detect", "esp_sr_detect", "backend_switch", "crash",
            "wdt", "panic", "brownout", "heap_samples",
            "btn1_events", "display_updates", "security_leaks",
        ]}
        self.heap_values = []
        self.last_heap = 0
        self.errors = []
        self.wifi_rssi_samples = []
        self.backend_current = "unknown"
        self.boot_count = 0
        self.last_boot_time = None

    def record(self, category, value=None):
        with self.lock:
            if category in self.counts:
                self.counts[category] += 1
            if category == "crash" or category == "wdt" or category == "panic":
                self.errors.append(f"{datetime.now().isoformat()}: {category} {value or ''}")
            if category == "wifi_connected":
                self.last_wifi_time = datetime.now()
            if category == "heap_samples" and value:
                self.heap_values.append(value)
                self.last_heap = value

    def set_backend(self, name):
        with self.lock:
            self.backend_current = name

    def record_boot(self):
        with self.lock:
            self.boot_count += 1
            self.last_boot_time = datetime.now()

    def summary(self):
        with self.lock:
            elapsed = datetime.now() - self.start_time
            lines = [
                f"\n{'='*60}",
                f"SOAK TEST SUMMARY @ {datetime.now().strftime('%H:%M:%S')}",
                f"Elapsed: {elapsed}",
                f"{'='*60}",
                f"Boots: {self.boot_count}",
                f"Backend: {self.backend_current}",
            ]
            for k, v in sorted(self.counts.items()):
                if v > 0:
                    lines.append(f"  {k}: {v}")
            if self.heap_values:
                lines.append(f"Heap: min={min(self.heap_values)} max={max(self.heap_values)} "
                             f"last={self.heap_values[-1]} samples={len(self.heap_values)}")
            if self.errors:
                lines.append(f"ERRORS ({len(self.errors)}):")
                for e in self.errors[-5:]:
                    lines.append(f"  {e}")
            else:
                lines.append("ERRORS: NONE (PASS)")
            # Gate checks
            gates = []
            gates.append(f"  WIFI_CONNECTED: {'PASS' if self.counts['wifi_connected'] > 0 else 'FAIL'}")
            gates.append(f"  BLE_ADVERTISING: {'PASS' if self.counts['ble_adv'] > 0 else 'FAIL'}")
            gates.append(f"  CRASH_FREE: {'PASS' if self.counts['crash'] == 0 else 'FAIL'}")
            gates.append(f"  WDT_FREE: {'PASS' if self.counts['wdt'] == 0 else 'FAIL'}")
            gates.append(f"  PANIC_FREE: {'PASS' if self.counts['panic'] == 0 else 'FAIL'}")
            gates.append(f"  SECURITY_LEAK: {'PASS' if self.counts['security_leaks'] == 0 else 'FAIL'}")
            lines.append("GATES:")
            lines.extend(gates)
            lines.append(f"{'='*60}\n")
            return "\n".join(lines)


def monitor_serial(port, stats, duration_hours=SOAK_DURATION_HOURS):
    """Monitor serial port and analyze output."""
    import serial

    end_time = datetime.now() + timedelta(hours=duration_hours)
    last_summary = datetime.now()
    log_file = os.path.join(LOG_DIR, f"soak_{datetime.now().strftime('%Y%m%d_%H%M%S')}.log")
    boot_detected = False

    print(f"[MONITOR] Logging to {log_file}")
    print(f"[MONITOR] Will run until {end_time.strftime('%H:%M:%S')}")

    with open(log_file, "w", encoding="utf-8") as flog:
        try:
            ser = serial.Serial(port, BAUD, timeout=1)
            ser.reset_input_buffer()
            print(f"[MONITOR] Connected to {port} @ {BAUD}")

            while datetime.now() < end_time:
                try:
                    line = ser.readline()
                    if not line:
                        continue
                    try:
                        text = line.decode("utf-8", errors="replace").strip()
                    except:
                        continue

                    timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
                    flog.write(f"{timestamp} {text}\n")
                    flog.flush()

                    # Analyze line
                    for name, pattern in PATTERNS.items():
                        m = pattern.search(text)
                        if m:
                            if name == "crash_guru":
                                stats.record("crash", text[:100])
                                print(f"\n!!! CRASH DETECTED: {text[:100]}")
                            elif name == "crash_wdt":
                                stats.record("wdt", text[:100])
                                print(f"\n!!! WDT DETECTED: {text[:100]}")
                            elif name == "panic":
                                stats.record("panic", text[:100])
                                print(f"\n!!! PANIC DETECTED: {text[:100]}")
                            elif name == "brownout":
                                stats.record("brownout", text[:100])
                                print(f"\n!!! BROWNOUT: {text[:100]}")
                            elif name == "wifi_connected":
                                stats.record("wifi_connected")
                                print(f"[WIFI] Connected")
                            elif name == "wifi_disconnected":
                                stats.record("wifi_disconnected")
                                reason_m = PATTERNS["wifi_reason"].search(text)
                                if reason_m:
                                    print(f"[WIFI] Disconnected reason={reason_m.group(1)} {reason_m.group(2)}")
                            elif name == "ble_adv":
                                stats.record("ble_adv")
                            elif name == "ble_connect":
                                stats.record("ble_connect")
                                print(f"[BLE] Connected")
                            elif name == "edge_detect":
                                stats.record("edge_detect")
                                print(f"[EDGE] Wake detected! total={stats.counts['edge_detect']}")
                            elif name == "esp_sr_detect":
                                stats.record("esp_sr_detect")
                                print(f"[ESP-SR] Wake detected! total={stats.counts['esp_sr_detect']}")
                            elif name == "backend_switch":
                                stats.record("backend_switch")
                                if "esp_sr" in text.lower():
                                    stats.set_backend("ESP-SR")
                                elif "edge" in text.lower():
                                    stats.set_backend("Edge")
                                print(f"[SWITCH] Backend changed: {stats.backend_current}")
                            elif name == "btn1_event":
                                stats.record("btn1_events")
                            elif name == "heap_info":
                                val = int(m.group(1) or m.group(2) or m.group(3) or 0)
                                if val > 0:
                                    stats.record("heap_samples", val)
                            elif name == "security_leak":
                                stats.record("security_leaks")
                                print(f"\n!!! SECURITY LEAK: {text[:80]}")
                            elif name == "boot":
                                if not boot_detected:
                                    boot_detected = True
                                    stats.record_boot()
                                    print(f"[BOOT] Firmware starting...")
                            elif name == "display_update":
                                stats.record("display_updates")
                            break

                except serial.SerialException as e:
                    print(f"[MONITOR] Serial error: {e}")
                    stats.record("crash", f"serial_error: {e}")
                    break
                except Exception as e:
                    continue

                # Periodic summary
                if (datetime.now() - last_summary).total_seconds() > SUMMARY_INTERVAL_SEC:
                    print(stats.summary())
                    last_summary = datetime.now()

        except serial.SerialException as e:
            print(f"[MONITOR] Cannot open {port}: {e}")
            return False
        except Exception as e:
            print(f"[MONITOR] Error: {e}")
            return False
        finally:
            try:
                ser.close()
            except:
                pass

    print(stats.summary())
    return len(stats.errors) == 0


def main():
    os.makedirs(LOG_DIR, exist_ok=True)

    print(f"Phase 9 Long Soak Test")
    print(f"Duration: {SOAK_DURATION_HOURS} hours")
    print(f"ELF SHA256: {hashlib.sha256(open(ELF_PATH, 'rb').read()).hexdigest()[:16]}...")
    print()

    # Find port
    print("[INIT] Searching for ESP32-S3...")
    port = find_esp32_port()
    if not port:
        print("[INIT] No ESP32-S3 found. Waiting for board connection...")
        # Poll every 30 seconds for up to 10 minutes
        for i in range(20):
            time.sleep(30)
            port = find_esp32_port()
            if port:
                break
            print(f"[INIT] Still searching... ({(i+1)*30}s)")
        if not port:
            print("[INIT] Board not found after 10 minutes. Exiting.")
            sys.exit(1)

    print(f"[INIT] Found board on {port}")

    # Flash
    if not flash_firmware(port):
        print("[FLASH] Flash failed. Retrying once...")
        time.sleep(5)
        if not flash_firmware(port):
            print("[FLASH] Flash failed again. Exiting.")
            sys.exit(1)

    # Wait for boot
    print("[INIT] Waiting for boot...")
    time.sleep(3)

    # Monitor
    stats = SoakStats()
    passed = monitor_serial(port, stats, SOAK_DURATION_HOURS)

    # Final report
    report_path = os.path.join(LOG_DIR, f"report_{datetime.now().strftime('%Y%m%d_%H%M%S')}.json")
    report = {
        "start_time": stats.start_time.isoformat(),
        "end_time": datetime.now().isoformat(),
        "duration_hours": SOAK_DURATION_HOURS,
        "elf_sha256": hashlib.sha256(open(ELF_PATH, 'rb').read()).hexdigest(),
        "bin_sha256": hashlib.sha256(open(BIN_PATH, 'rb').read()).hexdigest(),
        "srmodels_sha256": hashlib.sha256(
            open(os.path.join(BUILD_DIR, "srmodels", "srmodels.bin"), 'rb').read()
        ).hexdigest(),
        "counts": stats.counts,
        "boot_count": stats.boot_count,
        "errors": stats.errors,
        "passed": passed,
        "heap_min": min(stats.heap_values) if stats.heap_values else 0,
        "heap_max": max(stats.heap_values) if stats.heap_values else 0,
    }
    with open(report_path, "w") as f:
        json.dump(report, f, indent=2)

    print(f"\nFinal report: {report_path}")
    print(f"PASS" if passed else "FAIL")
    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
