#!/usr/bin/env python3
"""Extract legacy voice assets from old project's LocalAudio.h.

Reads Base64-encoded WAV constants from the old Arduino project,
decodes them, validates format, and writes to assets/legacy/.
Also generates a manifest JSON for tracking.

Usage:
    python tools/extract_legacy_voice_assets.py
"""

import base64
import hashlib
import json
import os
import re
import struct
import sys
from pathlib import Path

# Map old constant names to logical IDs
ASSET_MAP = {
    "AUDIO_WAKE_UP": "wake_up",
    "AUDIO_THINKING": "thinking",
    "AUDIO_HEAR_NOTHING": "hear_nothing",
    "AUDIO_PREF_UPDATED": "pref_updated",
    "AUDIO_NET_ERROR": "net_error",
    "AUDIO_API_TIMEOUT": "api_timeout",
    "AUDIO_JSON_ERROR": "json_error",
    "AUDIO_TASK_DONE": "task_done",
    "AUDIO_HW_FAULT": "hw_fault",
}

OLD_PROJECT = Path("D:/UserData/ESP32Project/ESP32-S3-Wroom1-DevKitC1_XIAOJING")
OUTPUT_DIR = Path("components/voice_agent/assets/legacy")
MANIFEST_PATH = Path("components/voice_agent/assets/voice_assets_manifest.json")


def parse_local_audio_h(filepath: Path) -> dict[str, str]:
    """Parse LocalAudio.h and return {constant_name: base64_string}."""
    content = filepath.read_text(encoding="utf-8", errors="replace")

    # Pattern: const char NAME[] PROGMEM = "BASE64...";
    pattern = r'const\s+char\s+(AUDIO_\w+)\[\]\s+PROGMEM\s*=\s*"([^"]+)"'
    matches = re.findall(pattern, content)

    result = {}
    for name, b64 in matches:
        # Clean up any line breaks or whitespace in Base64
        b64_clean = b64.replace('\n', '').replace('\r', '').replace(' ', '')
        result[name] = b64_clean

    return result


def validate_wav(data: bytes, name: str) -> dict:
    """Validate WAV file and return metadata."""
    info = {
        "valid": False,
        "format": "unknown",
        "channels": 0,
        "sample_rate": 0,
        "bits_per_sample": 0,
        "data_bytes": 0,
        "duration_ms": 0,
        "errors": [],
    }

    if len(data) < 44:
        info["errors"].append(f"Too small: {len(data)} bytes")
        return info

    # Check RIFF header
    if data[:4] != b"RIFF":
        info["errors"].append("Missing RIFF header")
        return info

    if data[8:12] != b"WAVE":
        info["errors"].append("Missing WAVE marker")
        return info

    # Parse chunks
    offset = 12
    fmt_found = False
    data_found = False
    data_offset = 0
    data_size = 0

    while offset + 8 <= len(data):
        chunk_id = data[offset:offset+4]
        chunk_size = struct.unpack_from("<I", data, offset+4)[0]
        chunk_data_start = offset + 8

        if chunk_id == b"fmt ":
            if chunk_size < 16:
                info["errors"].append("fmt chunk too small")
                return info

            fmt_tag = struct.unpack_from("<H", data, chunk_data_start)[0]
            channels = struct.unpack_from("<H", data, chunk_data_start+2)[0]
            sample_rate = struct.unpack_from("<I", data, chunk_data_start+4)[0]
            bits = struct.unpack_from("<H", data, chunk_data_start+14)[0]

            info["format"] = "PCM" if fmt_tag == 1 else f"fmt_{fmt_tag}"
            info["channels"] = channels
            info["sample_rate"] = sample_rate
            info["bits_per_sample"] = bits
            fmt_found = True

        elif chunk_id == b"data":
            data_offset = chunk_data_start
            data_size = chunk_size
            data_found = True

        # Move to next chunk (align to even boundary)
        offset = chunk_data_start + chunk_size + (chunk_size & 1)

    if not fmt_found:
        info["errors"].append("No fmt chunk found")
        return info

    if not data_found:
        info["errors"].append("No data chunk found")
        return info

    # Validate format
    if info["format"] != "PCM":
        info["errors"].append(f"Not PCM: {info['format']}")

    if info["channels"] != 1:
        info["errors"].append(f"Not mono: {info['channels']} channels")

    if info["bits_per_sample"] != 16:
        info["errors"].append(f"Not 16-bit: {info['bits_per_sample']}")

    # Validate data size
    if data_size == 0:
        info["errors"].append("Empty data chunk")
    elif data_size & 1:
        info["errors"].append("Odd data size")
    elif data_offset + data_size > len(data):
        info["errors"].append(f"Data extends beyond file: {data_offset}+{data_size} > {len(data)}")

    # Calculate duration
    if info["sample_rate"] > 0 and info["channels"] > 0:
        bytes_per_sample = info["bits_per_sample"] // 8
        bytes_per_frame = bytes_per_sample * info["channels"]
        if bytes_per_frame > 0:
            num_samples = data_size // bytes_per_frame
            info["duration_ms"] = int((num_samples * 1000) / info["sample_rate"])

    info["data_bytes"] = data_size
    info["valid"] = len(info["errors"]) == 0

    return info


def main():
    # Find LocalAudio.h
    local_audio_h = OLD_PROJECT / "src" / "LocalAudio.h"
    if not local_audio_h.exists():
        print(f"ERROR: {local_audio_h} not found", file=sys.stderr)
        return 1

    # Parse Base64 constants
    print(f"Parsing {local_audio_h}...")
    constants = parse_local_audio_h(local_audio_h)
    print(f"Found {len(constants)} audio constants")

    # Create output directory
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    # Process each asset
    manifest_entries = []
    extracted = 0
    errors = 0

    for const_name, logical_id in ASSET_MAP.items():
        if const_name not in constants:
            print(f"  WARN: {const_name} not found in LocalAudio.h")
            errors += 1
            continue

        b64_data = constants[const_name]

        # Decode Base64
        try:
            wav_data = base64.b64decode(b64_data)
        except Exception as e:
            print(f"  ERROR: {const_name}: Base64 decode failed: {e}")
            errors += 1
            continue

        # Validate WAV
        info = validate_wav(wav_data, const_name)

        # Write output file
        output_file = OUTPUT_DIR / f"{logical_id}.wav"
        output_file.write_bytes(wav_data)

        # Calculate SHA256
        sha256 = hashlib.sha256(wav_data).hexdigest()

        # Add to manifest
        manifest_entries.append({
            "logical_id": logical_id,
            "source": "legacy",
            "source_constant": const_name,
            "file": f"legacy/{logical_id}.wav",
            "text": "",  # Legacy assets don't have text metadata
            "sample_rate": info["sample_rate"],
            "channels": info["channels"],
            "bits_per_sample": info["bits_per_sample"],
            "duration_ms": info["duration_ms"],
            "byte_size": len(wav_data),
            "sha256": sha256,
            "generation_model": "legacy_hardware_recording",
            "generation_voice": "unknown",
            "generation_timestamp": "legacy",
            "valid": info["valid"],
            "errors": info["errors"],
        })

        status = "OK" if info["valid"] else "INVALID"
        print(f"  {const_name} -> {logical_id}.wav: {status} "
              f"({info['sample_rate']}Hz, {info['channels']}ch, "
              f"{info['bits_per_sample']}bit, {info['duration_ms']}ms, "
              f"{len(wav_data)} bytes)")

        if info["valid"]:
            extracted += 1
        else:
            for err in info["errors"]:
                print(f"    ERROR: {err}")
            errors += 1

    # Write manifest
    manifest = {
        "version": "1.0",
        "generated_by": "tools/extract_legacy_voice_assets.py",
        "assets": manifest_entries,
    }
    MANIFEST_PATH.parent.mkdir(parents=True, exist_ok=True)
    MANIFEST_PATH.write_text(json.dumps(manifest, indent=2, ensure_ascii=False),
                             encoding="utf-8")

    print(f"\nSummary: {extracted} extracted, {errors} errors")
    print(f"Output: {OUTPUT_DIR}")
    print(f"Manifest: {MANIFEST_PATH}")

    return 0 if errors == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
