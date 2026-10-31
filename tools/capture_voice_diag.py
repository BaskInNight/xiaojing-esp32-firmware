import sys
import time

import serial


def main() -> int:
    port = sys.argv[1] if len(sys.argv) > 1 else "COM8"
    duration = int(sys.argv[2]) if len(sys.argv) > 2 else 180
    keys = (
        "button raw", "button event", "PTT ", "Audio input",
        "I2S capture", "I2S raw level", "capture rejected", "capture ready",
        "record finish", "cloud job", "HTTP", "response",
        "voice prompt", "Guru", "abort",
    )
    with serial.Serial(port, 115200, timeout=0.2) as stream:
        end = time.time() + duration
        while time.time() < end:
            raw = stream.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", "replace").rstrip()
            if any(key.lower() in line.lower() for key in keys):
                print(line, flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
