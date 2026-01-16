Integration tests for unmapped → provision → unmap flows.

Run locally (requires Python 3):

    cd <repo-root>
    python3 test/integration/unmapped_provision_unmap.py

The test operates on copies of the JSON files and restores backups when finished. It asserts:

- discovery adds an entry to `unmapped-devices.json`
- provisioning removes the entry and adds device to `devices.json` with correct `type`
- unmapping removes from `devices.json` and adds a correctly-typed entry back to `unmapped-devices.json`
