#!/usr/bin/env python3
"""Run bounded, fail-fast Unity board-test soak rounds."""

import argparse
import json
import re
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path


def save_checkpoint(path, data):
    path.write_text(json.dumps(data, indent=2), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("port")
    parser.add_argument("output_dir")
    parser.add_argument("--expected", type=int, required=True)
    parser.add_argument("--minutes", type=float, default=30.0)
    args = parser.parse_args()

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    checkpoint_path = output_dir / "checkpoint.json"
    runner = Path(__file__).with_name("run_unity_tests.py")
    start = time.monotonic()
    deadline = start + args.minutes * 60.0
    completed = 0
    total_pass = 0
    total_ignore = 0
    checkpoint = {
        "start_time": datetime.now().isoformat(),
        "target_minutes": args.minutes,
        "expected": args.expected,
        "completed_rounds": 0,
        "valid_rounds": 0,
        "failed_round": 0,
        "last_result": "STARTING",
    }
    save_checkpoint(checkpoint_path, checkpoint)

    while time.monotonic() < deadline or completed == 0:
        round_no = completed + 1
        log_path = output_dir / f"round_{round_no:03d}.log"
        cmd = [sys.executable, str(runner), args.port, "--expected",
               str(args.expected), "--log", str(log_path)]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=360)
        combined = result.stdout + result.stderr
        (output_dir / f"round_{round_no:03d}.runner.txt").write_text(
            combined, encoding="utf-8")
        completed += 1
        match = re.search(r"PASS=(\d+) FAIL=(\d+) IGNORE=(\d+) TOTAL=(\d+)",
                          combined)
        valid = result.returncode == 0 and match is not None
        if valid:
            passed, failed, ignored, total = map(int, match.groups())
            valid = failed == 0 and total == args.expected
        else:
            passed = failed = ignored = total = 0
        if valid:
            total_pass += passed
            total_ignore += ignored
        checkpoint.update({
            "last_update": datetime.now().isoformat(),
            "completed_rounds": completed,
            "valid_rounds": completed if valid else completed - 1,
            "failed_round": 0 if valid else round_no,
            "last_result": (f"PASS={passed} FAIL={failed} IGNORE={ignored} "
                            f"TOTAL={total}" if match else
                            f"RUNNER_EXIT={result.returncode}"),
            "total_pass": total_pass,
            "total_ignore": total_ignore,
            "elapsed_seconds": round(time.monotonic() - start, 1),
        })
        save_checkpoint(checkpoint_path, checkpoint)
        print(f"ROUND {round_no}: {checkpoint['last_result']}", flush=True)
        if not valid:
            return 1
        if time.monotonic() < deadline:
            time.sleep(2.0)

    checkpoint["complete"] = True
    checkpoint["end_time"] = datetime.now().isoformat()
    save_checkpoint(checkpoint_path, checkpoint)
    print(f"SOAK COMPLETE: {completed} valid rounds", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
