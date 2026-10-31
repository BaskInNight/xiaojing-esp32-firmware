"""Package approved prompts at the hardware's 16kHz rate, preserving sources."""
from array import array
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import wave

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "components/voice_agent/assets"
SELECTION = {
    "boot_ready": "honey_v4/boot_ready_sunny_b.wav",
    "wake_ready": "honey_v3/wake_ready_a_sunny.wav",
    "ptt_ready": "honey_v1/ptt_ready.wav",
    "not_heard": "honey_v1/not_heard.wav",
    "chat_thinking": "honey_v8/chat_thinking_wait.wav",
    "network_unavailable": "honey_v1/network_unavailable.wav",
    "wash_started": "honey_v4/wash_started_reassuring_a.wav",
    "wash_complete": "honey_v1/wash_complete.wav",
    "device_fault": "honey_v1/device_fault.wav",
    "error": "honey_v1/error.wav",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=ASSETS / "approved_honey")
    parser.add_argument("--only", choices=SELECTION)
    args = parser.parse_args()
    # Use exactly the current firmware's Q15 FIR coefficients and rounding.
    code = (ROOT / "components/audio_driver/audio_pcm_resample.c").read_text(encoding="utf-8")
    body = code.split("taps[2][24] = {", 1)[1].split("};", 1)[0]
    taps = [[int(x) for x in re.findall(r"-?\d+", phase)]
            for phase in re.findall(r"\{([^}]+)\}", body)]
    assert len(taps) == 2 and all(len(p) == 24 for p in taps)
    output = args.output_dir
    if output.exists():
        raise FileExistsError("Preserving existing approved package")
    output.mkdir()
    entries = []
    for name, relative in SELECTION.items():
        if args.only and args.only != name:
            continue
        source = ASSETS / relative
        with wave.open(str(source), "rb") as reader:
            assert (reader.getnchannels(), reader.getsampwidth(), reader.getframerate()) == (1, 2, 24000)
            samples = array("h", reader.readframes(reader.getnframes()))
        if sys.byteorder != "little":
            samples.byteswap()
        result = array("h")
        for i in range(len(samples) * 2 // 3):
            center = (i // 2) * 3 + (i & 1)
            total = sum(samples[max(0, min(len(samples) - 1, center + k - 11))] * tap
                        for k, tap in enumerate(taps[i & 1]))
            rounded = (abs(total) + 16384) // 32768
            result.append(max(-32768, min(32767, rounded if total >= 0 else -rounded)))
        assert max(abs(v) for v in result) < 32767
        if sys.byteorder != "little":
            result.byteswap()
        target = output / f"{name}.wav"
        with wave.open(str(target), "wb") as writer:
            writer.setparams((1, 2, 16000, 0, "NONE", "not compressed"))
            writer.writeframes(result.tobytes())
        entries.append({"id": name, "source": relative,
                        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                        "file": target.name, "sample_rate": 16000,
                        "duration_ms": len(result) * 1000 // 16000,
                        "sha256": hashlib.sha256(target.read_bytes()).hexdigest()})
    (output / "manifest.json").write_text(json.dumps({
        "conversion": "Same 24-tap Q15 FIR as audio_pcm_resample.c; no speed/pitch change",
        "assets": entries}, indent=2), encoding="utf-8")
    print(f"Packaged {len(entries)} approved prompts; {sum(p.stat().st_size for p in output.glob('*.wav'))} WAV bytes")


if __name__ == "__main__":
    main()
