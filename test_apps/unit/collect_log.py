import serial, time, sys, re

def collect_run(port, log_path, timeout_s=600):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    ser = serial.Serial(port, 115200, timeout=1)
    # Reset board
    ser.dtr = False; ser.rts = True; time.sleep(0.1)
    ser.rts = False; time.sleep(0.1)
    ser.dtr = True

    start = time.time()
    lines = []
    app_main_count = 0
    begin_count = 0
    complete_count = 0
    first_complete_idx = -1
    registered = 0

    while time.time() - start < timeout_s:
        line = ser.readline().decode('utf-8', errors='replace').strip()
        if not line:
            continue
        lines.append(line)

        # Count markers
        if 'xiaojing ESP-IDF Unity tests' in line:
            app_main_count += 1
        if 'Auto-running all tests' in line:
            begin_count += 1
        if 'ALL TESTS COMPLETE' in line:
            complete_count += 1
            if first_complete_idx < 0:
                first_complete_idx = len(lines) - 1
            break  # Stop at first COMPLETE

        # Try to read registered count from Unity output
        m = re.search(r'(\d+) Tests (\d+) Failures (\d+) Ignored', line)
        if m:
            registered = int(m.group(1))

    ser.close()

    # Save full log up to first COMPLETE
    with open(log_path, 'w', encoding='utf-8') as f:
        for l in lines:
            f.write(l + '\n')

    # Count results from saved lines
    pass_c = sum(1 for l in lines if re.search(r':PASS$', l))
    # ESP-IDF Unity appends the assertion explanation after :FAIL, for example
    # ``name:FAIL: Expected ...``.  endswith(':FAIL') silently missed every
    # assertion failure.
    fail_c = sum(1 for l in lines if re.search(r':FAIL(?::|$)', l))
    ign_c = sum(1 for l in lines if re.search(r':IGNORE(?::|$)', l))
    total = pass_c + fail_c + ign_c

    # Check for crashes
    crashes = sum(1 for l in lines if 'guru' in l.lower() or 'stack overflow' in l.lower())
    stop_timeouts = sum(1 for l in lines if 'stop timeout' in l.lower())
    destroy_running = sum(1 for l in lines if 'destroy: task still running' in l.lower())

    return {
        'lines': len(lines),
        'app_main': app_main_count,
        'begin': begin_count,
        'complete': complete_count,
        'registered': registered,
        'pass': pass_c,
        'fail': fail_c,
        'ignore': ign_c,
        'total': total,
        'crashes': crashes,
        'stop_timeouts': stop_timeouts,
        'destroy_running': destroy_running,
        # Negative-path tests intentionally emit stop-timeout/destroy-running
        # diagnostics and then assert successful recovery.  They remain
        # observable counters, but validity is determined by Unity and reset/
        # crash evidence rather than matching those expected log strings.
        'valid': (app_main_count == 1 and begin_count == 1 and complete_count == 1
                  and fail_c == 0 and crashes == 0)
    }

if __name__ == '__main__':
    port = sys.argv[1] if len(sys.argv) > 1 else 'COM7'
    log_path = sys.argv[2] if len(sys.argv) > 2 else 'run.log'
    timeout = int(sys.argv[3]) if len(sys.argv) > 3 else 600

    r = collect_run(port, log_path, timeout)
    print(f"APP_MAIN={r['app_main']} BEGIN={r['begin']} COMPLETE={r['complete']}")
    print(f"PASS={r['pass']} FAIL={r['fail']} IGNORE={r['ignore']} TOTAL={r['total']}")
    print(f"REGISTERED={r['registered']}")
    print(f"CRASHES={r['crashes']} STOP_TIMEOUTS={r['stop_timeouts']} DESTROY_RUNNING={r['destroy_running']}")
    print(f"VALID={'YES' if r['valid'] else 'NO'}")
