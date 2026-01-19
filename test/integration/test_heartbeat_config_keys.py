#!/usr/bin/env python3
"""Integration-style test: verify hub and node config files include heartbeat keys

This test does NOT require hardware; it asserts that the configuration files
have the required keys and that the numeric values are reasonable.

Run locally:
    python3 test/integration/test_heartbeat_config_keys.py
"""

import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
HUB_CONFIG = ROOT / 'src' / 'hub' / 'data' / 'config' / 'hub_config.txt'
NODE_CONFIGS = list((ROOT / 'src' / 'nodes').glob('**/data/node_config.txt'))


def read_kv_file(path):
    result = {}
    with open(path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            if '=' not in line:
                continue
            k, v = line.split('=', 1)
            result[k.strip()] = v.strip()
    return result


EXPECTED_HUB_KEY = 'HUB_HEARTBEAT_INTERVAL_SEC'
EXPECTED_NODE_KEY = 'HUB_HEARTBEAT_TIMEOUT_MS'


def test_hub_key():
    assert HUB_CONFIG.exists(), f"Hub config not found at {HUB_CONFIG}"

    kv = read_kv_file(HUB_CONFIG)
    assert EXPECTED_HUB_KEY in kv, f"{EXPECTED_HUB_KEY} missing in {HUB_CONFIG}"

    try:
        v = int(kv[EXPECTED_HUB_KEY])
    except Exception:
        assert False, f"{EXPECTED_HUB_KEY} value invalid: {kv.get(EXPECTED_HUB_KEY)}"

    assert v > 0, f"{EXPECTED_HUB_KEY} must be > 0"

    print(f"OK: {EXPECTED_HUB_KEY}={v} in {HUB_CONFIG}")


def test_node_keys():
    assert NODE_CONFIGS, "No node config files found under src/nodes/*/data/node_config.txt"

    failures = 0
    for p in NODE_CONFIGS:
        kv = read_kv_file(p)
        assert EXPECTED_NODE_KEY in kv, f"{EXPECTED_NODE_KEY} missing in {p}"
        try:
            v = int(kv[EXPECTED_NODE_KEY])
        except Exception:
            assert False, f"{EXPECTED_NODE_KEY} value invalid: {kv.get(EXPECTED_NODE_KEY)} in {p}"
        assert v > 0, f"{EXPECTED_NODE_KEY} must be > 0 in {p}"
        print(f"OK: {p.name}: {EXPECTED_NODE_KEY}={v}")

    # All good
    return None


if __name__ == '__main__':
    rc = 0
    rc |= test_hub_key()
    rc |= test_node_keys()
    sys.exit(rc)
