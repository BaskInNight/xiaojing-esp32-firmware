"""Capture selected audio diagnostics; optionally reset the board once."""
import argparse
from pathlib import Path
import re
import time
import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM8")
    parser.add_argument("--seconds", type=float, default=120)
    parser.add_argument("--reset", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    secret_path = Path(__file__).resolve().parents[1] / "main/include/voice_secrets.h"
    secrets = re.findall(r'#define\s+\w+\s+"([^"]+)"',
                         secret_path.read_text(encoding="utf-8")) if secret_path.exists() else []
    selected = re.compile(
        r"boot_anim:|display_svc:|Display/LVGL|local voice prompt|embedded voice prompt|PCM16 playback|bootstrap_complete|"
        r"Audio output|Audio input|I2S RX resync|wake detected|WAKE_DETECTED|"
        r"Dialog recording|Begin session|capture rejected|cloud job|TTS HTTP|"
        r"finish_reason|stack overflow|panic|Guru Meditation|^E \(|^W \(")
    output = None
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        output = args.output.open("x", encoding="utf-8")

    def emit(line):
        for secret in secrets:
            if len(secret) > 4:
                line = line.replace(secret, "[REDACTED]")
        print(line, flush=True)
        if output:
            output.write(line + "\n")
            output.flush()

    port = serial.Serial()
    port.port = args.port
    port.baudrate = 115200
    port.timeout = 0.25
    port.dtr = False
    port.rts = False
    try:
        port.open()
        emit(f"Audio monitor opened: {args.port} 115200, reset={args.reset}")
        if args.reset:
            port.rts = True
            time.sleep(0.1)
            port.rts = False
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            line = port.readline().decode("utf-8", errors="replace").strip()
            if selected.search(line):
                emit(line)
    finally:
        port.close()
        emit("Audio monitor closed")
        if output:
            output.close()


if __name__ == "__main__":
    main()
