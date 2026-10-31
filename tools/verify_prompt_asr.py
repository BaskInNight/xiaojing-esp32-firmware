#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Verify MiMo-generated voice prompts using ASR roundtrip.

Uses MiMo chat completions with audio input to transcribe each WAV
and verify the transcript matches expected text.

Usage:
    python tools/verify_prompt_asr.py
"""

import base64
import hashlib
import json
import os
import re
import sys
import time
from pathlib import Path

import requests

if sys.platform == 'win32':
    sys.stdout.reconfigure(encoding='utf-8')
    sys.stderr.reconfigure(encoding='utf-8')

ENDPOINT = "https://api.xiaomimimo.com/v1/chat/completions"
WAV_DIR = Path("components/voice_agent/assets/mimo_generated")
MANIFEST_PATH = Path("components/voice_agent/assets/voice_assets_manifest.json")

EXPECTED_TEXTS = {
    "accepted": {"text": "好的，已开始处理。", "keywords": ["好", "开始", "处理"]},
    "rejected": {"text": "这个请求暂时无法执行。", "keywords": ["请求", "暂时", "无法", "执行"]},
    "error": {"text": "抱歉，处理失败，请稍后重试。", "keywords": ["处理失败", "稍后", "重试"]},
    "backend_xiaojing": {"text": "已切换到小净唤醒模式。", "keywords": ["小净", "小静"], "match_any": True},
    "backend_esp_sr": {"text": "已切换到小爱同学唤醒模式。", "keywords": ["小爱同学", "唤醒", "模式"]},
    "ptt_ready": {"text": "我在，请说。", "keywords": ["我", "请说"]},
    "chat_thinking": {"text": "好的，让我想一想。", "keywords": ["想一想"]},
    "need_confirmation": {"text": "请确认是否开始执行。", "keywords": ["确认", "开始", "执行"]},
    "network_unavailable": {"text": "网络暂不可用，请稍后重试。", "keywords": ["网络", "暂不可用", "稍后", "重试"]},
}


def load_api_key() -> str:
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


def normalize_text(text: str) -> str:
    """Normalize text for comparison."""
    text = re.sub(r'[，。！？、；：""''（）\[\]【】\s]+', '', text)
    return text.strip()


def asr_wav(api_key: str, wav_path: Path) -> str | None:
    """Transcribe WAV using MiMo chat completions with audio input."""
    with open(wav_path, "rb") as f:
        wav_data = f.read()
    wav_b64 = base64.b64encode(wav_data).decode('utf-8')

    headers = {
        "Content-Type": "application/json",
        "api-key": api_key,
    }
    payload = {
        "model": "mimo-v2.5",
        "messages": [
            {"role": "user", "content": [
                {"type": "input_audio", "input_audio": {"data": f"data:audio/wav;base64,{wav_b64}"}},
                {"type": "text", "text": "请完整转写这段音频中的中文语音，只输出转写文本，不要解释。"}
            ]}
        ],
    }

    try:
        response = requests.post(ENDPOINT, headers=headers, json=payload, timeout=30)
        if response.status_code != 200:
            print(f"    ERROR: HTTP {response.status_code}")
            return None

        result = response.json()
        text = result.get("choices", [{}])[0].get("message", {}).get("content", "")
        return text.strip()

    except Exception as e:
        print(f"    ERROR: {e}")
        return None


def main():
    api_key = load_api_key()
    if not api_key:
        print("ERROR: No API key found")
        return 1

    print(f"API key loaded (length={len(api_key)})")

    results = {}
    all_verified = True
    failed_ids = []

    for wav_id, expected in EXPECTED_TEXTS.items():
        wav_path = WAV_DIR / f"{wav_id}.wav"
        if not wav_path.exists():
            print(f"\n{wav_id}: FILE NOT FOUND")
            all_verified = False
            failed_ids.append(wav_id)
            continue

        print(f"\n{wav_id}: {expected['text']}")

        transcript = asr_wav(api_key, wav_path)
        if transcript is None:
            print("    ASR FAILED")
            all_verified = False
            failed_ids.append(wav_id)
            results[wav_id] = {
                "asr_transcript": "",
                "asr_normalized": "",
                "asr_verified": False,
                "asr_timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
            }
            continue

        normalized = normalize_text(transcript)
        expected_normalized = normalize_text(expected["text"])

        # Check keywords (support match_any for homophones)
        keywords = expected.get("keywords", [])
        match_any = expected.get("match_any", False)

        if match_any:
            # For homophones: at least one keyword must be found
            found = any(kw in transcript for kw in keywords)
            missing = [] if found else keywords
            verified = found
        else:
            # All keywords must be found
            missing = [kw for kw in keywords if kw not in transcript]
            verified = len(missing) == 0

        print(f"    ASR: {transcript}")
        print(f"    Normalized: {normalized}")

        if verified:
            print("    VERIFIED")
        else:
            print(f"    MISSING KEYWORDS: {missing}")
            all_verified = False
            failed_ids.append(wav_id)

        results[wav_id] = {
            "asr_transcript": transcript,
            "asr_normalized": normalized,
            "asr_verified": verified,
            "asr_timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
        }

        time.sleep(1)

    # Update manifest
    if MANIFEST_PATH.exists():
        manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
        for asset in manifest.get("assets", []):
            lid = asset.get("logical_id")
            if lid in results:
                asset.update(results[lid])
        MANIFEST_PATH.write_text(
            json.dumps(manifest, indent=2, ensure_ascii=True),
            encoding="utf-8"
        )
        print(f"\nManifest updated: {MANIFEST_PATH}")

    # Summary
    verified_count = sum(1 for r in results.values() if r.get("asr_verified"))
    total = len(EXPECTED_TEXTS)
    print(f"\nSummary: {verified_count}/{total} verified")

    if failed_ids:
        print(f"Failed: {', '.join(failed_ids)}")

    return 0 if all_verified else 1


if __name__ == "__main__":
    sys.exit(main())
