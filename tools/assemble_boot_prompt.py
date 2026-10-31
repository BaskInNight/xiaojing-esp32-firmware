"""Join independently synthesized boot phrases with a controlled pause."""

import argparse
from array import array
import hashlib
import json
from pathlib import Path
import sys
import wave

from synthesize_voice_prompts import validate_wav


def trimmed_pcm(path: Path) -> bytes:
    with wave.open(str(path), "rb") as source:
        if (source.getnchannels(), source.getsampwidth(), source.getframerate()) != (1, 2, 24000):
            raise ValueError("Expected mono 16-bit 24kHz PCM")
        pcm = source.readframes(source.getnframes())
    samples = array("h", pcm)
    if sys.byteorder != "little":
        samples.byteswap()
    # Remove only quiet boundary windows, retaining 50ms around detected speech.
    window = 240
    active = [i for i in range(0, len(samples), window)
              if sum(s * s for s in samples[i:i + window]) /
              len(samples[i:i + window]) > 100 * 100]
    if not active:
        raise ValueError("No audible speech detected")
    start = max(0, active[0] - 1200)
    end = min(len(samples), active[-1] + window + 1200)
    return pcm[start * 2:end * 2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    target = args.directory / "boot_ready.wav"
    if target.exists():
        raise FileExistsError("Preserving existing boot_ready.wav")
    sources = [args.directory / "boot_greeting.wav", args.directory / "boot_statement.wav"]
    # Each phrase retains 50ms silence; 120ms inserted gives about 220ms total.
    pcm = trimmed_pcm(sources[0]) + bytes(2880 * 2) + trimmed_pcm(sources[1])
    with wave.open(str(target), "wb") as output:
        output.setparams((1, 2, 24000, 0, "NONE", "not compressed"))
        output.writeframes(pcm)
    data = target.read_bytes()
    info = validate_wav(data)
    if not info["valid"] or info["clipped_samples"]:
        raise ValueError("Assembled audio failed validation")
    manifest_path = args.directory / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["assets"].append({
        "logical_id": "boot_ready",
        "file": f"{args.directory.name}/boot_ready.wav",
        "text": "你好呀，小净准备好啦。",
        "source": "assembled_from_synthesized_phrases",
        "sources": [{"file": p.name, "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
                    for p in sources],
        "inserted_silence_ms": 120,
        "boundary_padding_ms": 50,
        "sha256": hashlib.sha256(data).hexdigest(),
        "byte_size": len(data),
        "validation": info,
    })
    manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=True), encoding="utf-8")
    print(json.dumps({"file": str(target), "validation": info}))


if __name__ == "__main__":
    main()
