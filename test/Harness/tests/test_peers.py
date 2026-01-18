#!/usr/bin/env python3
import json
import time
from pathlib import Path
import sys
# Ensure harness package is importable when running as subprocess
# Add parent of Harness (so package 'Harness' is importable)
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from Harness.utils import http_get, http_post_form, assert_ok, load_config

cfg = load_config()
NODE_MAC = cfg['node_mac']


def test_peers_list_contains(mac):
    print('[TEST] GET /api/peers')
    code, body = http_get('/api/peers')
    assert_ok(code == 200, f'/api/peers returned {code} -> {body}')
    doc = json.loads(body)
    peers = doc.get('peers', [])
    found = any(p.get('mac', '').upper() == mac.upper() for p in peers)
    assert_ok(found, f'MAC {mac} not found in peers')
    print('  - Peer found')


def test_remove_peer(mac):
    print('[TEST] POST /api/peer/remove')
    code, body = http_post_form('/api/peer/remove', {'mac': mac})
    assert_ok(code == 200, f'/api/peer/remove returned {code} ({body})')
    # Allow some time for hub to update
    time.sleep(1)
    code, body = http_get('/api/peers')
    assert_ok(code == 200, f'/api/peers returned {code} -> {body}')
    doc = json.loads(body)
    peers = doc.get('peers', [])
    found = any(p.get('mac','').upper() == mac.upper() for p in peers)
    assert_ok(not found, f'MAC {mac} still present after remove')
    print('  - Peer removed')


if __name__ == '__main__':
    test_peers_list_contains(NODE_MAC)
    test_remove_peer(NODE_MAC)
    print('PEERS TESTS PASSED')
