#!/usr/bin/env python3
"""Capture the flow-meter proof points from an ESP-IDF serial console.

The firmware prints one FLOW_DIAG line whenever GPIO18's PCNT count grows.
This small, bounded tool deliberately records the raw lines to a file so a
manual water-pour test can be reviewed after the serial port is released.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

import serial


KEYWORDS = ("FLOW_DIAG", "Flow meter initialized", "flow reset failed", "Guru Meditation")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM8")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--duration", type=float, default=180.0)
    parser.add_argument("--log", default="flow_monitor.log")
    args = parser.parse_args()

    deadline = time.monotonic() + args.duration
    log_path = Path(args.log)
    hits = 0
    with serial.Serial(args.port, args.baud, timeout=0.25) as uart, \
            log_path.open("w", encoding="utf-8", newline="\n") as log:
        start = f"FLOW_MONITOR_START port={args.port} duration_s={args.duration:.0f}"
        print(start, flush=True)
        log.write(start + "\n")
        while time.monotonic() < deadline:
            text = uart.readline().decode("utf-8", errors="replace").rstrip()
            if not text:
                continue
            if any(word in text for word in KEYWORDS):
                line = f"{time.strftime('%H:%M:%S')} {text}"
                print(line, flush=True)
                log.write(line + "\n")
                log.flush()
                if "FLOW_DIAG" in text:
                    hits += 1
        end = f"FLOW_MONITOR_DONE flow_events={hits}"
        print(end, flush=True)
        log.write(end + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
