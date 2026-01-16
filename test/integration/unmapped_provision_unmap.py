#!/usr/bin/env python3
import json
import os
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CONFIG_DIR = ROOT / 'src' / 'hub' / 'data' / 'config'
UNMAPPED_FILE = CONFIG_DIR / 'unmapped-devices.json'
DEVICES_FILE = CONFIG_DIR / 'devices.json'

TEST_MAC = 'AA:BB:CC:DD:EE:AA'


def load_json(path):
    with open(path, 'r') as f:
        return json.load(f)


def save_json(path, doc):
    with open(path, 'w') as f:
        json.dump(doc, f, indent=2)


def backup_files():
    b1 = CONFIG_DIR / 'unmapped-devices.json.bak'
    b2 = CONFIG_DIR / 'devices.json.bak'
    shutil.copy2(UNMAPPED_FILE, b1)
    shutil.copy2(DEVICES_FILE, b2)
    return b1, b2


def restore_files(b1, b2):
    shutil.move(b1, UNMAPPED_FILE)
    shutil.move(b2, DEVICES_FILE)


def simulate_discovery(mac, type_str='LIGHT', firmware=1):
    doc = load_json(UNMAPPED_FILE)
    if 'unmappedDevices' not in doc:
        doc['unmappedDevices'] = []
    # Avoid duplicates
    doc['unmappedDevices'] = [d for d in doc['unmappedDevices'] if d.get('mac') != mac]
    entry = {
        'mac': mac,
        'type': type_str,
        'firmwareVersion': firmware,
        'discoveredAt': 0,
        'announceCount': 1
    }
    doc['unmappedDevices'].append(entry)
    save_json(UNMAPPED_FILE, doc)


def simulate_provision(mac, name='Test Device', tankId=1, requested_type=None, firmware=None):
    # Load unmapped
    udoc = load_json(UNMAPPED_FILE)
    unmapped = udoc.get('unmappedDevices', [])
    found = None
    for d in unmapped:
        if d.get('mac') == mac:
            found = d
            break

    if not found:
        print('ERROR: device not found in unmapped list')
        return False

    # Determine type and firmware
    final_type = requested_type if requested_type else found.get('type', 'UNKNOWN')
    final_fw = firmware if firmware is not None else found.get('firmwareVersion', 0)

    # Remove all entries for this mac
    udoc['unmappedDevices'] = [d for d in unmapped if d.get('mac') != mac]
    save_json(UNMAPPED_FILE, udoc)

    # Add to devices.json
    ddoc = load_json(DEVICES_FILE)
    if 'devices' not in ddoc:
        ddoc['devices'] = []
    # avoid duplicate
    ddoc['devices'] = [d for d in ddoc['devices'] if d.get('mac') != mac]

    new_dev = {
        'mac': mac,
        'type': final_type,
        'name': name,
        'tankId': tankId,
        'firmwareVersion': final_fw,
        'enabled': True,
        'status': 'PROVISIONING'
    }
    ddoc['devices'].append(new_dev)
    save_json(DEVICES_FILE, ddoc)

    return True


def simulate_unmap(mac):
    # Load devices
    ddoc = load_json(DEVICES_FILE)
    devices = ddoc.get('devices', [])
    found = None
    for d in devices:
        if d.get('mac') == mac:
            found = d
            break

    if found:
        # Remove from devices
        ddoc['devices'] = [d for d in devices if d.get('mac') != mac]
        save_json(DEVICES_FILE, ddoc)
    else:
        found = {'type': 'UNKNOWN', 'firmwareVersion': 0}

    # Add back to unmapped
    udoc = load_json(UNMAPPED_FILE)
    if 'unmappedDevices' not in udoc:
        udoc['unmappedDevices'] = []

    # remove duplicates
    udoc['unmappedDevices'] = [d for d in udoc['unmappedDevices'] if d.get('mac') != mac]

    entry = {
        'mac': mac,
        'type': found.get('type', 'UNKNOWN') or 'UNKNOWN',
        'firmwareVersion': found.get('firmwareVersion', 0),
        'discoveredAt': 0,
        'announceCount': 0
    }

    udoc['unmappedDevices'].append(entry)
    save_json(UNMAPPED_FILE, udoc)

    return True


def assert_condition(cond, msg):
    if not cond:
        print('ASSERTION FAILED:', msg)
        return False
    return True


def run_test():
    print('Backing up files...')
    b1, b2 = backup_files()

    try:
        print('Simulating discovery...')
        simulate_discovery(TEST_MAC, type_str='LIGHT', firmware=1)
        udoc = load_json(UNMAPPED_FILE)
        assert_condition(any(d.get('mac') == TEST_MAC for d in udoc.get('unmappedDevices', [])), 'Device not added to unmapped on discovery')

        print('Simulating provision (with type provided)...')
        ok = simulate_provision(TEST_MAC, name='My Light', tankId=2, requested_type='LIGHT')
        assert_condition(ok, 'Provision failed')

        ddoc = load_json(DEVICES_FILE)
        assert_condition(any(d.get('mac') == TEST_MAC and d.get('type') == 'LIGHT' for d in ddoc.get('devices', [])), 'Device not present in devices.json with correct type after provisioning')

        udoc = load_json(UNMAPPED_FILE)
        assert_condition(not any(d.get('mac') == TEST_MAC for d in udoc.get('unmappedDevices', [])), 'Device still present in unmapped-devices.json after provisioning')

        print('Simulating unmap...')
        ok = simulate_unmap(TEST_MAC)
        assert_condition(ok, 'Unmap failed')

        udoc = load_json(UNMAPPED_FILE)
        assert_condition(any(d.get('mac') == TEST_MAC and d.get('type') == 'LIGHT' for d in udoc.get('unmappedDevices', [])), 'Device type not preserved when unmapped')

        print('\nALL TESTS PASSED ✅')
        return 0

    except Exception as e:
        print('Exception during test:', e)
        return 2
    finally:
        print('Restoring backup files...')
        restore_files(b1, b2)


if __name__ == '__main__':
    sys.exit(run_test())
