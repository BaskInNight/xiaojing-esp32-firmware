#!/usr/bin/env python3
"""
Parse user wake test log and extract results.
"""

import re
import sys

def parse_log(log_file):
    """Parse the log file and extract wake detection events."""

    results = []
    current_round = 0

    # Patterns to match
    candidate_pattern = re.compile(
        r'CANDIDATE: xj=([\d.]+) noise=([\d.]+) unknown=([\d.]+) '
        r'margin_n=([\d.]+) margin_u=([\d.]+) consecutive=(\d+)/(\d+) (\w+)'
    )
    detected_pattern = re.compile(
        r'DETECTED xiao_jing score=([\d.]+) noise=([\d.]+) unknown=([\d.]+) '
        r'\(count=\d+\) ACCEPTED'
    )
    rejected_pattern = re.compile(
        r'REJECTED xj=([\d.]+) noise=([\d.]+) unknown=([\d.]+) margin_fail'
    )
    wake_detected_pattern = re.compile(r'WAKE_DETECTED: gen=(\d+)')
    dialog_begin_pattern = re.compile(r'voice_recorder: Begin session=(\d+)')
    dialog_end_pattern = re.compile(
        r'voice_recorder: End session=(\d+) reason=(\w+) pcm=(\d+) dur=(\d+) ms'
    )
    wake_resume_pattern = re.compile(r'State:.*-> WAKE')

    with open(log_file, 'r', encoding='utf-8', errors='ignore') as f:
        lines = f.readlines()

    for line in lines:
        # Check for CANDIDATE event
        m = candidate_pattern.search(line)
        if m:
            xj, noise, unknown, margin_n, margin_u, cons, cons_max, status = m.groups()
            print(f"CANDIDATE: xj={xj} noise={noise} unknown={unknown} "
                  f"margin_n={margin_n} margin_u={margin_u} "
                  f"consecutive={cons}/{cons_max} {status}")

        # Check for DETECTED (accepted)
        m = detected_pattern.search(line)
        if m:
            xj, noise, unknown = m.groups()
            print(f"DETECTED: xj={xj} noise={noise} unknown={unknown} ACCEPTED")

        # Check for REJECTED
        m = rejected_pattern.search(line)
        if m:
            xj, noise, unknown = m.groups()
            print(f"REJECTED: xj={xj} noise={noise} unknown={unknown}")

        # Check for WAKE_DETECTED
        m = wake_detected_pattern.search(line)
        if m:
            gen = m.group(1)
            print(f"WAKE_DETECTED: gen={gen}")

        # Check for dialog begin
        m = dialog_begin_pattern.search(line)
        if m:
            session = m.group(1)
            print(f"DIALOG_BEGIN: session={session}")

        # Check for dialog end
        m = dialog_end_pattern.search(line)
        if m:
            session, reason, pcm, dur = m.groups()
            print(f"DIALOG_END: session={session} reason={reason} pcm={pcm} dur={dur}")

        # Check for wake resume
        m = wake_resume_pattern.search(line)
        if m:
            print(f"WAKE_RESUME: {line.strip()}")

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python parse_wake_test.py <log_file>")
        sys.exit(1)

    parse_log(sys.argv[1])
