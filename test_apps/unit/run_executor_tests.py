import serial, time, sys

def run_with_timeout(port, timeout_s):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    ser = serial.Serial(port, 115200, timeout=1)
    ser.dtr = False; ser.rts = True; time.sleep(0.1)
    ser.rts = False; time.sleep(0.1)
    ser.dtr = True

    start = time.time()
    lines = []
    in_executor = False
    executor_done = False

    while time.time() - start < timeout_s:
        line = ser.readline().decode('utf-8', errors='replace').strip()
        if not line:
            continue
        lines.append(line)

        # Track executor tests
        if 'wash_executor_tests.c' in line:
            in_executor = True

        # After executor tests, look for next test file
        if in_executor and ('group_d_tests.c' in line or 'test_runner.c' in line):
            if ':PASS' in line or ':FAIL' in line:
                executor_done = True
                break

        if 'ALL TESTS COMPLETE' in line:
            break

        if 'guru' in line.lower():
            break

    ser.close()

    # Count results
    pass_c = sum(1 for l in lines if l.endswith(':PASS'))
    fail_c = sum(1 for l in lines if l.endswith(':FAIL'))
    ign_c = sum(1 for l in lines if l.endswith(':IGNORE'))
    crashes = sum(1 for l in lines if 'guru' in l.lower())
    stop_to = sum(1 for l in lines if 'stop timeout' in l.lower())
    destroy_run = sum(1 for l in lines if 'destroy: task still running' in l.lower())

    # Executor results
    exec_results = []
    for l in lines:
        if 'wash_executor_tests.c' in l and (':PASS' in l or ':FAIL' in l):
            name = l.split('wash_executor_tests.c:')[1] if 'wash_executor_tests.c:' in l else l
            if name not in exec_results:
                exec_results.append(name)

    return {
        'lines': len(lines),
        'pass': pass_c,
        'fail': fail_c,
        'ignore': ign_c,
        'crashes': crashes,
        'stop_timeouts': stop_to,
        'destroy_running': destroy_run,
        'executor_results': exec_results,
        'elapsed': int(time.time() - start)
    }

if __name__ == '__main__':
    port = sys.argv[1] if len(sys.argv) > 1 else 'COM7'
    timeout = int(sys.argv[2]) if len(sys.argv) > 2 else 300

    r = run_with_timeout(port, timeout)
    print(f"LINES={r['lines']} ELAPSED={r['elapsed']}s")
    print(f"PASS={r['pass']} FAIL={r['fail']} IGNORE={r['ignore']}")
    print(f"CRASHES={r['crashes']} STOP_TIMEOUTS={r['stop_timeouts']} DESTROY_RUNNING={r['destroy_running']}")
    print(f"EXECUTOR_TESTS={len(r['executor_results'])}")
    for t in r['executor_results']:
        print(f"  {t}")
