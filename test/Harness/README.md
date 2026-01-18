# Test Harness — Hub ↔ Node Integration

Automated integration test harness for the Hub's interactions with nodes.

Usage
-----
- Ensure the Hub is running on the local network and accessible at the URL configured in `harness_config.json` (default: `http://ams.local`).
- Connect the Node(s) you want to test and ensure they are powered/online.
- Run the full harness:

```bash
python3 test/Harness/run_all.py
```

What it tests
-------------
- Provisioning / unmapping workflow (wraps existing test script)
- Hub API availability and hub MACs (STA/AP)
- Peer listing and remote peer removal via API
- Light status request flow (Hub sends COMMAND, Hub receives STATUS)

Files
-----
- `run_all.py` — main test runner
- `utils.py` — HTTP helpers and assertion helpers
- `tests/test_provisioning.py` — wrapper around existing provisioning test
- `tests/test_peers.py` — peers list & removal tests
- `tests/test_light_status_flow.py` — command->status flow tests
- `harness_config.json` — base config (base_url, test mac)

Notes
-----
- This harness uses only Python stdlib (`urllib.request`) so it can run in minimal environments.
- Tests are intentionally conservative (use retries/timeouts) to accommodate asynchronous message delivery.

Contributions
-------------
Add new tests in `test/Harness/tests/` and add them to `run_all.py`.
