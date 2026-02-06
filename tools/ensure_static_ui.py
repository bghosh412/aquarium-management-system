"""
PlatformIO Pre-Script: ensure static UI is present and up-to-date

Purpose:
- Enforce the project convention that hub front-end changes MUST be applied to
  `src/hub/data_static/` (the authoritative static UI used for OTA builds).
- Fail the build (with a clear message) when the dev UI (`src/hub/data/`) is newer
  than the static UI, or when required static files are missing.

Behavior:
- Exits with code 1 (failing the build) when an out-of-sync or missing-static-UI
  condition is detected, unless the environment variable
  `ALLOW_STATIC_UI_OUT_OF_SYNC=1` is set (developer override).
- Prints clear remediation instructions.

This script is deliberately conservative and fast (mtime checks only).
"""

from SCons.Script import Import
Import("env")
import os
import sys

project_dir = env["PROJECT_DIR"]
dev_dir = os.path.join(project_dir, "src", "hub", "data")
static_dir = os.path.join(project_dir, "src", "hub", "data_static")

required_files = [
    os.path.join(static_dir, "UI", "index.html"),
    os.path.join(static_dir, "UI", "device", "manage-devices.html"),
    os.path.join(static_dir, "UI", "styles", "styles.css"),
]

# Helper
def fail(msg):
    print("[ensure_static_ui] ERROR: " + msg)
    print("[ensure_static_ui] To fix: copy or sync changes from src/hub/data/ → src/hub/data_static/ and re-run the build.")
    print("[ensure_static_ui] If you intentionally want to bypass this check, set ALLOW_STATIC_UI_OUT_OF_SYNC=1 in your environment.")
    sys.exit(1)

# If override present, only warn
if os.environ.get('ALLOW_STATIC_UI_OUT_OF_SYNC') == '1':
    print('[ensure_static_ui] ALLOW_STATIC_UI_OUT_OF_SYNC=1 set — skipping strict checks (will only warn)')
    strict = False
else:
    strict = True

# Check existence
if not os.path.isdir(static_dir):
    if strict:
        fail(f"Missing static UI directory: {os.path.relpath(static_dir, project_dir)}")
    else:
        print(f"[ensure_static_ui] WARNING: static UI dir not found: {static_dir}")
        sys.exit(0)

# Check required files present
missing = [f for f in required_files if not os.path.isfile(f)]
if missing:
    msg = "Required static UI files missing:\n  " + "\n  ".join([os.path.relpath(m, project_dir) for m in missing])
    if strict:
        fail(msg)
    else:
        print("[ensure_static_ui] WARNING: " + msg)

# Quick mtime-based sync check: if any file in dev/UI is newer than corresponding static/UI -> fail
out_of_sync = []
for root, dirs, files in os.walk(os.path.join(dev_dir, 'UI')):
    for fn in files:
        rel = os.path.relpath(os.path.join(root, fn), os.path.join(dev_dir, 'UI'))
        dev_path = os.path.join(dev_dir, 'UI', rel)
        static_path = os.path.join(static_dir, 'UI', rel)
        if not os.path.exists(static_path):
            # missing in static
            out_of_sync.append((rel, 'missing-in-static'))
            continue
        try:
            if os.path.getmtime(dev_path) > os.path.getmtime(static_path):
                out_of_sync.append((rel, 'dev-newer'))
        except OSError:
            out_of_sync.append((rel, 'stat-failed'))

if out_of_sync:
    lines = []
    for rel, reason in out_of_sync[:20]:
        lines.append(f"{rel} — {reason}")
    summary = "Files that need syncing from src/hub/data/UI → src/hub/data_static/UI:\n  " + "\n  ".join(lines)
    if strict:
        fail(summary)
    else:
        print('[ensure_static_ui] WARNING: ' + summary)

print('[ensure_static_ui] OK — static UI present and appears up-to-date')
