#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Synthesize voice prompts using MiMo TTS API.

Reads API key from MIMO_API_KEY environment variable or voice_secrets.h.
Generates WAV files for all product prompts.

Usage:
    python tools/synthesize_voice_prompts.py
"""

import base64
import argparse
import hashlib
import json
import os
import re
import struct
import sys
import time
from datetime import datetime
from pathlib import Path

import requests

# Force UTF-8 stdout on Windows
if sys.platform == 'win32':
    sys.stdout.reconfigure(encoding='utf-8')
    sys.stderr.reconfigure(encoding='utf-8')

# Configuration
ENDPOINT = "https://api.xiaomimimo.com/v1/chat/completions"
MODEL = "mimo-v2.5-tts"
VOICE = "mimo_default"
OUTPUT_DIR = Path("components/voice_agent/assets/mimo_generated")
MANIFEST_PATH = Path("components/voice_agent/assets/voice_assets_manifest.json")

# Style prompt - using explicit Unicode escapes to prevent mojibake
STYLE_PROMPT = (
    "请用知性、温柔、自然、清晰的年轻中文女声朗读，"
    "语速稍慢，情绪稳定，带轻微蜜糖般的亲和感；"
    "不要播音腔、客服腔、夸张撒娇，不要读标点。"
)

# Verify style prompt is correct
EXPECTED_STYLE = "请用知性、温柔、自然、清晰的年轻中文女声朗读，语速稍慢，情绪稳定，带轻微蜜糖般的亲和感；不要播音腔、客服腔、夸张撒娇，不要读标点。"
assert STYLE_PROMPT == EXPECTED_STYLE, f"Style prompt mismatch: {STYLE_PROMPT!r}"

# Mojibake patterns to detect
MOJIBAKE_PATTERNS = [
    'Ã¥',  # å from UTF-8 misread as latin1
    'Ã¤',  # ä
    'Ã¶',  # ö
    'å¥',  # Common mojibake fragment
    'è¯',  # Common mojibake fragment
    'ç',  # Common mojibake fragment
    'å¥', 'è¯', 'çš', '濂界', '宸插', '鍞ら',
    '�',  # Replacement character
]

def has_mojibake(text: str) -> bool:
    """Check if text contains mojibake patterns."""
    for pattern in MOJIBAKE_PATTERNS:
        if pattern in text:
            return True
    return False

# Prompts to generate - using Unicode escapes for safety
PROMPTS = [
    {"id": "accepted", "text": "好的，已开始处理。", "description": "Command accepted"},
    {"id": "rejected", "text": "这个请求暂时无法执行。", "description": "Command rejected"},
    {"id": "error", "text": "抱歉，处理失败，请稍后重试。", "description": "Error occurred"},
    {"id": "backend_xiaojing", "text": "已切换到小净唤醒模式。", "description": "Switch to Xiaojing wake word"},
    {"id": "backend_esp_sr", "text": "已切换到小爱同学唤醒模式。", "description": "Switch to ESP-SR wake word"},
    {"id": "ptt_ready", "text": "我在，请说。", "description": "PTT ready for input"},
    {"id": "chat_thinking", "text": "好的，让我想一想。", "description": "Processing chat query"},
    {"id": "need_confirmation", "text": "请确认是否开始执行。", "description": "Need user confirmation"},
    {"id": "network_unavailable", "text": "网络暂不可用，请稍后重试。", "description": "Network unavailable"},
]

# Verify all prompts are correct
EXPECTED_PROMPTS = {
    "accepted": "好的，已开始处理。",
    "rejected": "这个请求暂时无法执行。",
    "error": "抱歉，处理失败，请稍后重试。",
    "backend_xiaojing": "已切换到小净唤醒模式。",
    "backend_esp_sr": "已切换到小爱同学唤醒模式。",
    "ptt_ready": "我在，请说。",
    "chat_thinking": "好的，让我想一想。",
    "need_confirmation": "请确认是否开始执行。",
    "network_unavailable": "网络暂不可用，请稍后重试。",
}

for p in PROMPTS:
    assert p["text"] == EXPECTED_PROMPTS[p["id"]], f"Prompt {p['id']} mismatch: {p['text']!r}"
    assert not has_mojibake(p["text"]), f"Prompt {p['id']} contains mojibake: {p['text']!r}"


def load_api_key() -> str:
    """Load API key from environment or voice_secrets.h."""
    key = os.environ.get("MIMO_API_KEY")
    if key:
        return key

    secrets_path = Path("main/include/voice_secrets.h")
    if secrets_path.exists():
        content = secrets_path.read_text(encoding="utf-8")
        match = re.search(r'#define\s+XIAOJING_VOICE_API_KEY\s+"([^"]+)"', content)
        if match:
            return match.group(1)

    return ""


def validate_wav(data: bytes) -> dict:
    """Validate WAV file and return metadata."""
    info = {
        "valid": False,
        "format": "unknown",
        "channels": 0,
        "sample_rate": 0,
        "bits_per_sample": 0,
        "data_bytes": 0,
        "duration_ms": 0,
        "peak": 0,
        "rms": 0,
        "dc_offset": 0,
        "clipping": False,
        "errors": [],
    }

    if len(data) < 44:
        info["errors"].append(f"Too small: {len(data)} bytes")
        return info

    if data[:4] != b"RIFF":
        info["errors"].append("Missing RIFF header")
        return info

    if data[8:12] != b"WAVE":
        info["errors"].append("Missing WAVE marker")
        return info

    offset = 12
    fmt_found = False
    data_found = False
    data_offset = 0
    data_size = 0

    while offset + 8 <= len(data):
        chunk_id = data[offset:offset+4]
        chunk_size = struct.unpack_from("<I", data, offset+4)[0]
        chunk_data_start = offset + 8

        if chunk_size > len(data) - chunk_data_start:
            info["errors"].append(f"Chunk {chunk_id} extends beyond file")
            return info

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

        offset = chunk_data_start + chunk_size + (chunk_size & 1)

    if not fmt_found:
        info["errors"].append("No fmt chunk found")
        return info
    if not data_found:
        info["errors"].append("No data chunk found")
        return info
    if info["format"] != "PCM":
        info["errors"].append(f"Not PCM: {info['format']}")
    if info["channels"] != 1:
        info["errors"].append(f"Not mono: {info['channels']}")
    if info["bits_per_sample"] != 16:
        info["errors"].append(f"Not 16-bit: {info['bits_per_sample']}")
    if info["sample_rate"] != 24000:
        info["errors"].append(f"Not 24kHz: {info['sample_rate']}")
    if data_size == 0:
        info["errors"].append("Empty data")
    elif data_size & 1:
        info["errors"].append("Odd data size")
    elif data_offset + data_size > len(data):
        info["errors"].append("Data extends beyond file")

    # Audio analysis
    if not info["errors"]:
        num_samples = data_size // 2
        peak = 0
        sum_sq = 0
        dc_sum = 0
        clipping_count = 0

        analyzed_samples = num_samples
        for i in range(analyzed_samples):
            sample = struct.unpack_from("<h", data, data_offset + i*2)[0]
            abs_sample = abs(sample)
            if abs_sample > peak:
                peak = abs_sample
            if abs_sample >= 32767:
                clipping_count += 1
            sum_sq += sample * sample
            dc_sum += sample

        info["peak"] = peak
        info["rms"] = int((sum_sq / analyzed_samples) ** 0.5) if analyzed_samples else 0
        info["dc_offset"] = dc_sum // analyzed_samples if analyzed_samples else 0
        info["clipping"] = clipping_count > analyzed_samples * 0.01
        info["clipped_samples"] = clipping_count
        info["data_bytes"] = data_size
        info["duration_ms"] = int((data_size * 1000) / (info["sample_rate"] * 2))
        info["valid"] = True

    return info


def synthesize_tts(api_key: str, text: str, max_retries: int = 3,
                   style_prompt: str | None = None) -> bytes | None:
    """Call MiMo TTS API and return WAV data."""
    headers = {
        "Content-Type": "application/json",
        "api-key": api_key,
    }

    # Build request with explicit encoding
    payload = {
        "model": MODEL,
        "messages": [
            {"role": "user", "content": style_prompt or STYLE_PROMPT},
            {"role": "assistant", "content": text},
        ],
        "audio": {"format": "wav", "voice": VOICE},
    }

    for attempt in range(max_retries):
        try:
            response = requests.post(
                ENDPOINT,
                headers=headers,
                json=payload,
                timeout=(10, 90),
            )

            if response.status_code == 401:
                print("    ERROR: Authentication failed (401)")
                return None
            elif response.status_code == 403:
                print("    ERROR: Forbidden (403)")
                return None
            elif response.status_code == 429:
                print("    WARN: Rate limited (429), waiting 5s...")
                time.sleep(5)
                continue
            elif response.status_code >= 500:
                print(f"    WARN: Server error ({response.status_code}), retrying...")
                time.sleep(2)
                continue
            elif response.status_code != 200:
                print(f"    ERROR: HTTP {response.status_code}")
                return None

            try:
                data = response.json()
            except json.JSONDecodeError:
                print("    ERROR: Invalid JSON response")
                continue

            try:
                audio_b64 = data["choices"][0]["message"]["audio"]["data"]
            except (KeyError, TypeError, IndexError):
                print("    ERROR: Missing audio.data in response")
                continue

            try:
                wav_data = base64.b64decode(audio_b64)
            except Exception as e:
                print(f"    ERROR: Base64 decode failed: {e}")
                continue

            return wav_data

        except requests.Timeout:
            print("    WARN: Timeout, retrying...")
            time.sleep(2)
        except requests.RequestException as e:
            print(f"    ERROR: Request failed ({type(e).__name__})")
            return None

    return None


def main():
    global OUTPUT_DIR, MANIFEST_PATH, PROMPTS, STYLE_PROMPT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--spec", type=Path, help="Approved UTF-8 prompt/style specification")
    parser.add_argument("--output-dir", type=Path, help="New asset directory; existing WAVs are preserved")
    args = parser.parse_args()
    if args.spec:
        spec = json.loads(args.spec.read_text(encoding="utf-8"))
        PROMPTS = spec["prompts"]
        STYLE_PROMPT = spec["style"]
        ids = [p["id"] for p in PROMPTS]
        if (len(set(ids)) != len(ids) or
                any(not re.fullmatch(r"[a-z][a-z0-9_]*", value) for value in ids)):
            print("ERROR: Invalid or duplicate asset IDs")
            return 1
    if args.output_dir:
        OUTPUT_DIR = args.output_dir
        MANIFEST_PATH = OUTPUT_DIR / "manifest.json"
    if any((OUTPUT_DIR / f"{p['id']}.wav").exists() for p in PROMPTS):
        print("ERROR: Selected WAVs already exist; use a new output directory")
        return 1
    api_key = load_api_key()
    if not api_key:
        print("ERROR: No API key found")
        return 1

    print("API credentials loaded from protected local configuration")

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    # Generate all prompts
    print(f"\nGenerating {len(PROMPTS)} voice prompts...")
    generated = 0
    errors = 0

    for prompt in PROMPTS:
        text = prompt["text"]
        print(f"\n  {prompt['id']}: {text}")

        # Verify text is not mojibake
        if has_mojibake(text):
            print(f"    ERROR: Text contains mojibake patterns")
            errors += 1
            continue

        wav_data = synthesize_tts(api_key, text, style_prompt=prompt.get("style"))
        if not wav_data:
            print("    FAILED: No data")
            errors += 1
            continue

        info = validate_wav(wav_data)
        if not info["valid"]:
            print(f"    FAILED: {info['errors']}")
            errors += 1
            continue

        # Write file
        output_file = OUTPUT_DIR / f"{prompt['id']}.wav"
        output_file.write_bytes(wav_data)

        sha256 = hashlib.sha256(wav_data).hexdigest()

        print(f"    OK: {info['sample_rate']}Hz, {info['duration_ms']}ms, "
              f"{len(wav_data)}B, peak={info['peak']}, rms={info['rms']}, "
              f"dc={info['dc_offset']}, clip={info['clipping']}")
        print(f"    SHA256: {sha256}")

        generated += 1
        time.sleep(1)

    # Generate manifest with ensure_ascii=True for safety
    print(f"\nGenerating manifest...")
    manifest_entries = []
    ts = datetime.now().strftime("%Y-%m-%dT%H:%M:%S")

    for prompt in PROMPTS:
        output_file = OUTPUT_DIR / f"{prompt['id']}.wav"
        if output_file.exists():
            data = output_file.read_bytes()
            info = validate_wav(data)
            manifest_entries.append({
                "logical_id": prompt["id"],
                "source": "mimo_generated",
                "file": f"{OUTPUT_DIR.name}/{prompt['id']}.wav",
                "text": prompt["text"],
                "sample_rate": info["sample_rate"],
                "channels": info["channels"],
                "bits_per_sample": info["bits_per_sample"],
                "duration_ms": info["duration_ms"],
                "byte_size": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
                "generation_model": MODEL,
                "generation_voice": VOICE,
                "generation_style": prompt.get("style", STYLE_PROMPT),
                "generation_timestamp": ts,
                "validation": info,
            })

    manifest = {
        "version": "1.0",
        "generated_by": "tools/synthesize_voice_prompts.py",
        "style": STYLE_PROMPT,
        "assets": manifest_entries,
    }

    MANIFEST_PATH.parent.mkdir(parents=True, exist_ok=True)
    MANIFEST_PATH.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=True),
        encoding="utf-8"
    )

    print(f"\nSummary: {generated} generated, {errors} errors")
    print(f"Output: {OUTPUT_DIR}")
    print(f"Manifest: {MANIFEST_PATH}")

    return 0 if errors == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
