#!/usr/bin/env python3
import json
import time
from pathlib import Path
import sys
# Ensure harness package is importable when running as subprocess
# Add parent of Harness (so package 'Harness' is importable)
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from Harness.utils import http_get, assert_ok, load_config

cfg = load_config()
NODE_MAC = cfg['node_mac']


def test_light_status_flow(mac, timeout_sec=10):
    print('[TEST] LIGHT STATUS flow (send request and wait for cached status)')
    # Trigger status request
    code, body = http_get('/api/light-status', params={'mac': mac})
    assert_ok(code == 200, f'/api/light-status returned {code}')
    # If pending, poll until cached status appears
    deadline = time.time() + timeout_sec
    while time.time() < deadline:
        code, body = http_get('/api/light-status', params={'mac': mac})
        assert_ok(code == 200, f'/api/light-status returned {code}')
        doc = json.loads(body)
        if doc.get('success'):
            status = doc.get('status', {})
            print('  - Received cached status:', status)
            return True
        print('  - pending...')
        time.sleep(0.5)
    assert_ok(False, 'Timed out waiting for cached status')


if __name__ == '__main__':
    test_light_status_flow(NODE_MAC)
    print('LIGHT STATUS FLOW PASSED')
