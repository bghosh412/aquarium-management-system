#!/usr/bin/env python3
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / 'test' / 'integration' / 'unmapped_provision_unmap.py'


def run():
    print('[TEST] Running provisioning/unmap script')
    result = subprocess.run([sys.executable, str(SCRIPT)], capture_output=True, text=True)
    print(result.stdout)
    print(result.stderr)
    return result.returncode == 0


if __name__ == '__main__':
    ok = run()
    print('PROVISIONING TEST', 'PASSED' if ok else 'FAILED')
    sys.exit(0 if ok else 2)
