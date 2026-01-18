#!/usr/bin/env python3
import sys
import traceback
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / 'test' / 'Harness' / 'tests'

TEST_FILES = [
    'test_provisioning.py',
    'test_peers.py',
    'test_light_status_flow.py'
]

# Ensure harness folder is on sys.path so tests can import harness.utils
import sys
sys.path.insert(0, str(ROOT / 'test' / 'Harness'))


def run_test(path):
    print('\n=== RUNNING:', path.name)
    try:
        import subprocess, sys
        res = subprocess.run([sys.executable, str(path)], capture_output=True, text=True)
        print(res.stdout)
        if res.stderr:
            print(res.stderr)
        ok = (res.returncode == 0)
        if ok:
            print('=== PASSED:', path.name)
        else:
            print('=== FAILED:', path.name)
        return ok
    except Exception:
        print('=== FAILED:', path.name)
        traceback.print_exc()
        return False


if __name__ == '__main__':
    all_ok = True
    for t in TEST_FILES:
        p = TEST_DIR / t
        ok = run_test(p)
        all_ok = all_ok and ok

    if not all_ok:
        print('\nSome tests failed')
        sys.exit(2)
    print('\nALL TESTS PASSED')
    sys.exit(0)
