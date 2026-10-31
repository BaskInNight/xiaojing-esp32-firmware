"""Unattended Phase 9 serial soak capture for COM8.

The firmware's autonomous switch/RX-recovery test has already completed; this
process only observes the running device for long-term resets or assertions.
"""
import json
import re
import time
from pathlib import Path

import serial


DURATION_S = 4 * 60 * 60
PORT = "COM8"
OUT = Path("phase9_r269_soak.log")
CHECKPOINT = Path("phase9_r269_soak_checkpoint.json")


def main() -> None:
    ser = serial.Serial(PORT, 115200, timeout=0.25)
    start = time.time()
    line_count = 0
    resets = 0
    panics = 0
    last_checkpoint = start
    reset_re = re.compile(r"rst:|Guru Meditation|assert failed|abort\(\)|WDT")
    try:
        with OUT.open("w", encoding="utf-8", errors="replace") as log:
            while time.time() - start < DURATION_S:
                data = ser.read(4096)
                if data:
                    text = data.decode("utf-8", errors="replace")
                    log.write(text)
                    log.flush()
                    line_count += text.count("\n")
                    hits = reset_re.findall(text)
                    if hits:
                        resets += sum(1 for h in hits if h == "rst:")
                        panics += len(hits) - sum(1 for h in hits if h == "rst:")
                now = time.time()
                if now - last_checkpoint >= 60:
                    payload = {
                        "elapsed_s": int(now - start),
                        "line_count": line_count,
                        "reset_markers": resets,
                        "panic_markers": panics,
                        "complete": False,
                    }
                    CHECKPOINT.write_text(
                        json.dumps(payload, ensure_ascii=True, indent=2),
                        encoding="utf-8",
                    )
                    last_checkpoint = now
    finally:
        ser.close()
    payload = {
        "elapsed_s": int(time.time() - start),
        "line_count": line_count,
        "reset_markers": resets,
        "panic_markers": panics,
        "complete": True,
    }
    CHECKPOINT.write_text(
        json.dumps(payload, ensure_ascii=True, indent=2), encoding="utf-8"
    )
    print(json.dumps(payload, ensure_ascii=True))


if __name__ == "__main__":
    main()
