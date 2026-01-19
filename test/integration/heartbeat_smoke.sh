#!/usr/bin/env bash
# Smoke test for hub heartbeat / node fail-safe behavior
# Usage:
#   HUB_PORT=/dev/ttyUSB2 NODE_PORT=/dev/ttyUSB1 bash test/integration/heartbeat_smoke.sh
# The script will build (platformio), upload (requires ports set), and monitor serial

set -euo pipefail

HUB_ENV=hub_esp32
NODE_ENV=node_lighting
HUB_PORT=${HUB_PORT:-}
NODE_PORT=${NODE_PORT:-}
DURATION=${DURATION:-30}  # seconds to capture serial logs

if ! command -v platformio >/dev/null 2>&1; then
  echo "platformio not found in PATH. Please install PlatformIO CLI to run this smoke test."
  exit 2
fi

# Build
echo "Building hub and node firmwares..."
platformio run --environment ${HUB_ENV}
platformio run --environment ${NODE_ENV}

echo "Build complete."

if [ -z "$HUB_PORT" ] || [ -z "$NODE_PORT" ]; then
  echo "HUB_PORT and NODE_PORT must be set to upload and monitor devices."
  echo "Example: HUB_PORT=/dev/ttyUSB2 NODE_PORT=/dev/ttyUSB1 bash $0"
  exit 2
fi

# Upload
echo "Uploading hub to $HUB_PORT..."
platformio run --environment ${HUB_ENV} --target upload --upload-port ${HUB_PORT}

echo "Uploading node to $NODE_PORT..."
platformio run --environment ${NODE_ENV} --target upload --upload-port ${NODE_PORT}

# Monitor serial outputs in background
HUB_LOG=$(mktemp)
NODE_LOG=$(mktemp)

echo "Capturing serial logs for ${DURATION}s..."
# Start monitors in background
platformio device monitor --environment ${HUB_ENV} --port ${HUB_PORT} --baud 115200 > ${HUB_LOG} 2>&1 &
HUB_MON_PID=$!
platformio device monitor --environment ${NODE_ENV} --port ${NODE_PORT} --baud 115200 > ${NODE_LOG} 2>&1 &
NODE_MON_PID=$!

# Wait for the capture duration
sleep ${DURATION}

# Kill the monitors
kill ${HUB_MON_PID} ${NODE_MON_PID} 2>/dev/null || true
sleep 1

echo "--- Hub log ---"
cat ${HUB_LOG} | tail -n 200

echo "--- Node log ---"
cat ${NODE_LOG} | tail -n 200

# Check for expected heartbeats
HUB_HB_COUNT=$(grep -c "\[HUB HB\]" ${HUB_LOG} || true)
NODE_HB_RECV=$(grep -c "\[HB\] Hub heartbeat received" ${NODE_LOG} || true)

echo "Hub heartbeat lines: ${HUB_HB_COUNT}"
echo "Node received hub heartbeat lines: ${NODE_HB_RECV}"

if [ "${HUB_HB_COUNT}" -eq 0 ]; then
  echo "ERROR: No hub heartbeat messages were observed in hub serial output."
  exit 3
fi

if [ "${NODE_HB_RECV}" -eq 0 ]; then
  echo "ERROR: Node did not log receiving hub heartbeat."
  exit 3
fi

echo "Smoke test PASSED: hub is broadcasting and node receives heartbeat."

# If hub test mode enabled, attempt to force fail-safe via hub API (requires hub API reachable)
HUB_TEST_MODE_ENABLED=$(grep -c "HUB_TEST_MODE=true" src/hub/data/config/hub_config.txt || true)
if [ "${HUB_TEST_MODE_ENABLED}" -gt 0 ]; then
  echo "Hub test mode enabled — attempting to force fail-safe via hub API"
  # Try to get device MAC from devices.json (first device)
  DEVICE_MAC=$(jq -r '.devices[0].mac' src/hub/data/config/devices.json || true)
  if [ -z "${DEVICE_MAC}" ] || [ "${DEVICE_MAC}" = "null" ]; then
    echo "ERROR: Could not determine device MAC from devices.json"
    rm -f ${HUB_LOG} ${NODE_LOG}
    exit 4
  fi

  # Attempt POST to hub (common AP IP 192.168.4.1) - this will only work if host can reach hub
  echo "Posting force-failsafe to hub for MAC ${DEVICE_MAC}"
  curl -s -X POST "http://192.168.4.1/api/test/force-failsafe" -d "mac=${DEVICE_MAC}" || true

  sleep 2

  # Check node log for fail-safe messages
  NODE_FS_COUNT=$(grep -c "FAIL-SAFE" ${NODE_LOG} || true)
  echo "Node FAIL-SAFE lines: ${NODE_FS_COUNT}"
  if [ "${NODE_FS_COUNT}" -eq 0 ]; then
    echo "ERROR: Node did not enter fail-safe after hub test trigger."
    rm -f ${HUB_LOG} ${NODE_LOG}
    exit 5
  fi

  echo "Smoke test PASS: Force-failsafe via hub API verified."
fi

# Cleanup
rm -f ${HUB_LOG} ${NODE_LOG}
exit 0
