import sys
import time

import serial

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def main() -> int:
    port = sys.argv[1] if len(sys.argv) > 1 else "COM8"
    duration = int(sys.argv[2]) if len(sys.argv) > 2 else 600
    with serial.Serial(port, 115200, timeout=0.2) as stream:
        end = time.time() + duration
        while time.time() < end:
            raw = stream.readline()
            if raw:
                print(raw.decode("utf-8", "replace").rstrip(), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
