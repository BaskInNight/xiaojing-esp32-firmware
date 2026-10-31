#!/usr/bin/env python3
"""
Phase 7 Group B Round 2.3.2 — Overnight Soak Runner
Unattended test runner with checkpoint/resume support.
"""
import serial
import time
import json
import os
import sys
import hashlib
from datetime import datetime, timezone
from pathlib import Path

PORT = "COM7"
BAUD = 115200
SOAK_DIR = Path(__file__).parent.parent / "phase7_group_b_round2_3_2_overnight"
SOAK_SUBDIR = SOAK_DIR / "soak"
CHECKPOINT_FILE = SOAK_SUBDIR / "checkpoint.json"
MANIFEST_FILE = SOAK_DIR / "manifest.json"
PID_FILE = SOAK_SUBDIR / ".runner.pid"

SOAK_TARGET_HOURS = 5
SOAK_MIN_HOURS = 4
SOAK_MAX_HOURS = 6
TEST_TIMEOUT = 900  # 15 minutes per round

def get_binary_hash(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(8192), b''):
            h.update(chunk)
    return h.hexdigest()

def read_serial_until_tests_done(ser, timeout=TEST_TIMEOUT):
    start = time.time()
    buffer = ""
    while time.time() - start < timeout:
        if ser.in_waiting:
            data = ser.read(ser.in_waiting).decode('utf-8', errors='replace')
            buffer += data
            if 'Tests run:' in buffer:
                return buffer
        time.sleep(0.01)
    return buffer

def parse_results(output):
    results = {'pass': 0, 'fail': 0, 'ignore': 0, 'total': 0,
               'begin': 0, 'complete': 0, 'reset': 0, 'crash': False,
               'non_hw_ignore': 0, 'failures': []}
    for line in output.split('\n'):
        line = line.strip()
        if ':PASS' in line: results['pass'] += 1
        elif ':FAIL' in line and ':PASS' not in line:
            results['fail'] += 1
            results['failures'].append(line[:200])
        elif ':IGNORE' in line:
            results['ignore'] += 1
            if 'Board not confirmed' not in line and 'No PCNT hardware' not in line:
                results['non_hw_ignore'] += 1
        if 'Guru Meditation' in line or 'abort()' in line:
            results['crash'] = True
    results['total'] = results['pass'] + results['fail'] + results['ignore']
    return results

def save_checkpoint(data):
    SOAK_SUBDIR.mkdir(parents=True, exist_ok=True)
    tmp = CHECKPOINT_FILE.with_suffix('.tmp')
    with open(tmp, 'w') as f:
        json.dump(data, f, indent=2)
    # Windows: rename doesn't overwrite, so remove target first
    if CHECKPOINT_FILE.exists():
        CHECKPOINT_FILE.unlink()
    tmp.rename(CHECKPOINT_FILE)

def load_checkpoint():
    if CHECKPOINT_FILE.exists():
        with open(CHECKPOINT_FILE) as f:
            return json.load(f)
    return None

def check_duplicate():
    if PID_FILE.exists():
        try:
            with open(PID_FILE) as f:
                old_pid = int(f.read().strip())
            import subprocess
            result = subprocess.run(['tasklist', '/FI', f'PID eq {old_pid}'],
                                  capture_output=True, text=True)
            if str(old_pid) in result.stdout:
                return True
        except: pass
    with open(PID_FILE, 'w') as f:
        f.write(str(os.getpid()))
    return False

def reset_board(ser):
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.1)
    ser.setRTS(False)
    time.sleep(1.0)

def run_round(ser, round_num, binary_hash):
    print(f"\n{'='*60}")
    print(f"Round {round_num} at {datetime.now()}")
    print(f"{'='*60}", flush=True)

    reset_board(ser)
    output = read_serial_until_tests_done(ser)
    results = parse_results(output)

    log_file = SOAK_SUBDIR / f"soak_{round_num:03d}.log"
    SOAK_SUBDIR.mkdir(parents=True, exist_ok=True)
    with open(log_file, 'w', encoding='utf-8') as f:
        f.write(f"Round {round_num}\nTimestamp: {datetime.now().isoformat()}\n")
        f.write(f"Binary hash: {binary_hash}\n{'='*60}\n")
        f.write(output)

    return results

def validate(results):
    issues = []
    if results['fail'] != 0:
        issues.append(f"FAIL={results['fail']}")
    if results['crash']:
        issues.append("CRASH detected")
    if results['non_hw_ignore'] != 0:
        issues.append(f"NON_HW_IGNORE={results['non_hw_ignore']}")
    return issues

def main():
    if check_duplicate():
        print("ERROR: Another runner instance is active")
        sys.exit(1)

    binary_path = Path(__file__).parent.parent / "test_apps" / "unit" / "build" / "xiaojing_unit_test.bin"
    if not binary_path.exists():
        print(f"ERROR: Binary not found: {binary_path}")
        sys.exit(1)

    binary_hash = get_binary_hash(binary_path)
    print(f"Binary: {binary_path}")
    print(f"Hash: {binary_hash}")

    checkpoint = load_checkpoint()
    if checkpoint and checkpoint.get('binary_hash') == binary_hash:
        print(f"Resuming: {checkpoint['rounds_completed']} rounds done")
    else:
        print("Starting fresh soak")

    checkpoint = {
        'binary_hash': binary_hash,
        'start_time': datetime.now(timezone.utc).isoformat(),
        'rounds_completed': 0,
        'rounds_passed': 0,
        'consecutive_pass_start': None,
        'status': 'running',
    }
    save_checkpoint(checkpoint)

    ser = serial.Serial(PORT, BAUD, timeout=1)
    start_time = time.time()
    consecutive_pass_start = None
    round_num = checkpoint.get('rounds_completed', 0)

    try:
        while True:
            elapsed_hours = (time.time() - start_time) / 3600
            if elapsed_hours >= SOAK_MAX_HOURS:
                print(f"\nHard limit {SOAK_MAX_HOURS}h reached")
                break

            round_num += 1
            results = run_round(ser, round_num, binary_hash)
            issues = validate(results)

            checkpoint['rounds_completed'] = round_num
            checkpoint['last_update'] = datetime.now(timezone.utc).isoformat()

            if not issues:
                checkpoint['rounds_passed'] += 1
                print(f"Round {round_num}: PASS ({results['pass']}P/{results['fail']}F/{results['ignore']}I)", flush=True)

                if consecutive_pass_start is None:
                    consecutive_pass_start = time.time()
                cp_hours = (time.time() - consecutive_pass_start) / 3600
                checkpoint['consecutive_pass_hours'] = round(cp_hours, 2)

                if cp_hours >= SOAK_TARGET_HOURS:
                    print(f"\nTarget soak: {cp_hours:.1f}h")
                    checkpoint['status'] = 'target_reached'
                    break
            else:
                print(f"Round {round_num}: FAIL - {', '.join(issues)}")
                checkpoint['status'] = 'failed'
                checkpoint['failure_reason'] = issues
                consecutive_pass_start = None
                break

            save_checkpoint(checkpoint)

    except KeyboardInterrupt:
        checkpoint['status'] = 'interrupted'
    except Exception as e:
        checkpoint['status'] = 'error'
        checkpoint['error'] = str(e)
    finally:
        ser.close()
        checkpoint['end_time'] = datetime.now(timezone.utc).isoformat()
        save_checkpoint(checkpoint)

        manifest = {
            'binary_hash': binary_hash,
            'port': PORT,
            'start_time': checkpoint['start_time'],
            'end_time': checkpoint['end_time'],
            'rounds_completed': checkpoint['rounds_completed'],
            'rounds_passed': checkpoint['rounds_passed'],
            'status': checkpoint['status'],
            'consecutive_pass_hours': checkpoint.get('consecutive_pass_hours', 0),
        }
        with open(MANIFEST_FILE, 'w') as f:
            json.dump(manifest, f, indent=2)

        if PID_FILE.exists():
            PID_FILE.unlink()

    print(f"\n{'='*60}")
    print(f"Status: {checkpoint['status']}")
    print(f"Rounds: {checkpoint['rounds_completed']} total, {checkpoint['rounds_passed']} passed")
    print(f"Consecutive: {checkpoint.get('consecutive_pass_hours', 0)}h")
    print(f"{'='*60}")

if __name__ == '__main__':
    main()
