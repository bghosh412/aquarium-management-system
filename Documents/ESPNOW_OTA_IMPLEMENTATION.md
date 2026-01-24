# ESP-NOW OTA Implementation Guide

**Over-the-Air firmware updates for ESP8266 nodes via ESP-NOW protocol**

---

## Table of Contents

1. [Overview](#overview)
2. [Architecture](#architecture)
3. [Protocol Messages](#protocol-messages)
4. [Hub Implementation](#hub-implementation)
5. [Node Implementation](#node-implementation)
6. [Handshake Protocol](#handshake-protocol)
7. [API Endpoints](#api-endpoints)
8. [Web UI](#web-ui)
9. [Testing with Mock Server](#testing-with-mock-server)
10. [Troubleshooting](#troubleshooting)

---

## Overview

The ESP-NOW OTA system enables firmware updates to ESP8266 nodes without direct USB connection. The hub downloads firmware from an HTTP server and streams it to nodes via ESP-NOW messages.

### Key Features

- **Multi-device support**: Updates all online light devices sequentially
- **Chunked transfer**: 29-byte payload chunks (ESP-NOW max is 250 bytes)
- **Handshake protocol**: READY_END confirmation before applying update
- **Automatic cleanup**: Downloaded files deleted after OTA completes
- **Progress tracking**: Real-time progress via web UI and API

### Limitations

- **Transfer speed**: ~30ms per chunk = ~6 minutes for 330KB firmware
- **No retransmission**: Lost chunks will cause OTA failure
- **Sequential updates**: Multi-device updates happen one-by-one, not in parallel

---

## Architecture

```
┌─────────────────┐         ┌─────────────────┐         ┌─────────────────┐
│   OTA Server    │  HTTP   │      Hub        │ ESP-NOW │     Node        │
│  (firmware.bin) │◄───────►│   (ESP32-S3)    │◄───────►│   (ESP8266)     │
└─────────────────┘         └─────────────────┘         └─────────────────┘
        │                           │                           │
        │  1. GET firmware.bin      │                           │
        │◄──────────────────────────│                           │
        │                           │                           │
        │  2. Save to LittleFS      │                           │
        │                           │                           │
        │                           │  3. OTA_BEGIN             │
        │                           │──────────────────────────►│
        │                           │                           │
        │                           │  4. FIRMWARE_CHUNK x N    │
        │                           │──────────────────────────►│
        │                           │                           │
        │                           │  5. READY_END (status)    │
        │                           │◄──────────────────────────│
        │                           │                           │
        │                           │  6. OTA_END               │
        │                           │──────────────────────────►│
        │                           │                           │
        │                           │  7. Node reboots          │
        │                           │                           │
```

---

## Protocol Messages

### OTA Command Codes (in `commandData[0]`)

```cpp
#define OTA_CMD_OTA_BEGIN        0xA0  // Start OTA session
#define OTA_CMD_OTA_END          0xA1  // End OTA session
#define OTA_CMD_FIRMWARE_CHUNK   0xF1  // Firmware data chunk
#define OTA_CMD_CONFIG_CHUNK     0xC1  // Config data chunk (optional)
```

### OTA Status Codes (in `statusCode`)

```cpp
#define OTA_STATUS_BEGIN_OK      0x20  // Node ready to receive
#define OTA_STATUS_CHUNK_OK      0x21  // Chunk received (not used for throughput)
#define OTA_STATUS_NEED_BEGIN    0x22  // Node needs BEGIN re-sent
#define OTA_STATUS_END_OK        0x23  // OTA applied successfully
#define OTA_STATUS_ERROR         0x24  // General error
#define OTA_STATUS_READY_END     0x25  // Node ready for END command
#define OTA_STATUS_CHUNK_ERR     0x26  // Chunk write error
```

### Message Formats

#### OTA_BEGIN Message
```
commandData[0] = OTA_CMD_OTA_BEGIN (0xA0)
commandData[1] = type (0xF1=firmware, 0xC1=config)
commandData[2-5] = totalSize (uint32_t, little-endian)
commandData[6] = chunkSize (29 bytes)
```

#### FIRMWARE_CHUNK Message
```
commandData[0] = OTA_CMD_FIRMWARE_CHUNK (0xF1)
commandData[1-2] = chunkIndex (uint16_t, little-endian)
commandData[3-31] = raw binary data (up to 29 bytes)
finalCommand = true on LAST chunk only
```

#### OTA_END Message
```
commandData[0] = OTA_CMD_OTA_END (0xA1)
commandData[1] = type (0xF1=firmware, 0xC1=config)
finalCommand = true
```

#### READY_END Status Message
```
statusCode = OTA_STATUS_READY_END (0x25)
statusData[0] = type (0xF1 or 0xC1)
statusData[1-2] = chunksReceived (uint16_t)
```

---

## Hub Implementation

### State Structure (`src/main.cpp`)

```cpp
struct NodeOtaState {
    bool active;
    bool completed;
    bool success;
    String error;
    String baseUrl;
    uint8_t targetMac[6];
    uint32_t configChunksSent;
    uint32_t firmwareChunksSent;
    uint32_t totalConfigChunks;
    uint32_t totalFirmwareChunks;
    bool configSaved;
    bool configSent;
    bool firmwareSaved;
    bool firmwareSent;
    bool firmwareWaitingEnd;
    uint8_t lastCommandId;
    // Multi-device support
    uint8_t targetMacs[10][6];
    uint8_t targetCount;
    uint8_t currentTarget;
    uint8_t devicesUpdated;
    uint8_t devicesFailed;
};
```

### OTA Task Flow

1. Download `node_config.txt` (optional) and `firmware.bin` from URL
2. Save files to `/ota/light/` on LittleFS
3. For each online light device:
   - Send OTA_BEGIN (5x for reliability)
   - Stream firmware chunks (30ms delay between chunks)
   - Wait for READY_END status (30s timeout)
   - Send OTA_END (via READY_END handler)
4. Delete downloaded OTA files
5. Mark transfer complete

### READY_END Handler (`onStatusReceived`)

```cpp
if (msg.statusCode == OTA_STATUS_READY_END && nodeOtaState.active) {
    // Clear waiting flag so OTA task can continue
    nodeOtaState.firmwareWaitingEnd = false;
    
    // Send OTA_END 5x for reliability
    CommandMessage endMsg = {};
    endMsg.commandData[0] = OTA_CMD_OTA_END;
    endMsg.commandData[1] = otaType;
    endMsg.finalCommand = true;
    
    for (int i = 0; i < 5; i++) {
        ESPNowManager::getInstance().send(targetMac, ...);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
```

---

## Node Implementation

### OTA State (`lib/NodeBase/node_base.cpp`)

```cpp
struct OtaState {
    bool active;
    uint8_t type;           // 0xF1 or 0xC1
    uint32_t totalSize;
    uint32_t receivedBytes;
    uint32_t totalChunks;
    uint32_t chunksReceived;
    uint32_t expectedChunk;
    uint32_t highestChunkIdx;
    uint8_t chunkSize;
    uint8_t* buffer;        // For config only
};
```

### OTA Handlers

#### handleOtaBegin
1. Parse totalSize and chunkSize from message
2. Calculate totalChunks
3. Begin Update (for firmware) or allocate buffer (for config)
4. Send BEGIN_OK status

#### handleOtaChunk
1. Validate chunk index
2. Write data to flash (firmware) or buffer (config)
3. Increment counters
4. On last chunk (`finalCommand=true` AND all chunks received):
   - Send READY_END status to hub

#### handleOtaEnd
1. Verify all chunks received
2. For firmware: `Update.end(true)` and reboot
3. For config: Save buffer to LittleFS

---

## Handshake Protocol

The critical handshake ensures firmware is fully received before applying:

```
Hub                                    Node
 │                                      │
 │──── FIRMWARE_CHUNK (final=true) ────►│
 │                                      │ Detects all chunks received
 │◄──── STATUS (READY_END) ─────────────│
 │                                      │
 │──── OTA_END ────────────────────────►│
 │                                      │ Update.end(true)
 │                                      │ ESP.restart()
```

**Why this matters:**
- Without READY_END, hub might send END before node receives all chunks
- Node only sends READY_END when `finalCommand=true` AND `chunksReceived >= totalChunks`
- Hub waits up to 30s for READY_END, then sends END anyway as fallback

---

## API Endpoints

### POST /api/settings/ota-urls
Set the OTA base URL:
```bash
curl -X POST -H "Content-Type: application/json" \
  -d '{"lightNodeOtaUrl":"http://192.168.1.34:8088"}' \
  http://192.168.1.53/api/settings/ota-urls
```

### POST /api/nodes/light/check-update
Check for available updates (downloads version.txt):
```bash
curl -X POST http://192.168.1.53/api/nodes/light/check-update
```

### POST /api/nodes/light/apply-update
Start OTA transfer to all online light devices:
```bash
curl -X POST http://192.168.1.53/api/nodes/light/apply-update
```

Response:
```json
{
  "success": true,
  "status": "started",
  "message": "OTA transfer started for 2 device(s)",
  "deviceCount": 2
}
```

### GET /api/nodes/light/ota-status
Poll OTA progress:
```bash
curl http://192.168.1.53/api/nodes/light/ota-status
```

Response:
```json
{
  "active": true,
  "completed": false,
  "success": false,
  "error": null,
  "firmwareSaved": true,
  "firmwareSent": false,
  "firmwareChunks": 5000,
  "firmwareTotalChunks": 11406,
  "progress": 43,
  "deviceCount": 2,
  "currentDevice": 1,
  "devicesUpdated": 0,
  "devicesFailed": 0
}
```

---

## Web UI

Access the OTA UI at: `http://<hub-ip>/settings/update-software.html`

### Features
- Configure OTA URL
- Check for updates
- Apply updates with progress bar
- Multi-device status tracking

### File Locations
- HTML: `src/hub/data/UI/settings/update-software.html`
- JavaScript: `src/hub/data/UI/settings/settings-update-software.js`

---

## Testing with Mock Server

### Prerequisites
- Python 3.x installed
- Node firmware built (`pio run -e node_lighting`)
- Hub and node both running

### Step 1: Prepare OTA Files

```bash
# Create OTA directory
mkdir -p /tmp/ota_server

# Copy firmware
cp .pio/build/node_lighting/firmware.bin /tmp/ota_server/

# (Optional) Create version file
echo "2" > /tmp/ota_server/version.txt

# (Optional) Create config file
echo "TANK_ID=1" > /tmp/ota_server/node_config.txt
```

### Step 2: Start Mock Server

```bash
cd /tmp/ota_server
python3 -m http.server 8088
```

Or run in background:
```bash
nohup python3 -m http.server 8088 > /tmp/ota_server.log 2>&1 &
```

### Step 3: Configure Hub

```bash
# Set OTA URL (replace IP with your machine's IP)
curl -X POST -H "Content-Type: application/json" \
  -d '{"lightNodeOtaUrl":"http://192.168.1.34:8088"}' \
  http://192.168.1.53/api/settings/ota-urls
```

### Step 4: Trigger OTA

```bash
curl -X POST http://192.168.1.53/api/nodes/light/apply-update
```

### Step 5: Monitor Progress

```bash
# Poll status every 5 seconds
while true; do
  curl -s http://192.168.1.53/api/nodes/light/ota-status | \
    python3 -c "import sys,json; d=json.load(sys.stdin); print(f\"Progress: {d['progress']}% Chunks: {d['firmwareChunks']}/{d['firmwareTotalChunks']}\")"
  sleep 5
done
```

### Step 6: Verify Update

After OTA completes, the node reboots with new firmware:
```bash
# Check node serial output
pio device monitor -e node_lighting
```

Look for: `[TX] ANNOUNCE sent (tankId=1, FW=v2, ch=11)`

### Automated Testing Script

Save as `test/ota_test.py`:

```python
#!/usr/bin/env python3
"""
OTA Test Script for Aquarium Management System
Usage: python3 test/ota_test.py
"""

import http.server
import socketserver
import threading
import requests
import time
import shutil
import os

# Configuration
HUB_IP = "192.168.1.53"
OTA_PORT = 8088
FIRMWARE_PATH = ".pio/build/node_lighting/firmware.bin"
OTA_DIR = "/tmp/ota_test_server"

def setup_ota_server():
    """Create OTA directory and copy firmware"""
    os.makedirs(OTA_DIR, exist_ok=True)
    shutil.copy(FIRMWARE_PATH, os.path.join(OTA_DIR, "firmware.bin"))
    
    # Create version file
    with open(os.path.join(OTA_DIR, "version.txt"), "w") as f:
        f.write("99")  # High version to trigger update
    
    print(f"✓ OTA files prepared in {OTA_DIR}")

def start_http_server():
    """Start HTTP server in background thread"""
    os.chdir(OTA_DIR)
    handler = http.server.SimpleHTTPRequestHandler
    httpd = socketserver.TCPServer(("", OTA_PORT), handler)
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    print(f"✓ HTTP server started on port {OTA_PORT}")
    return httpd

def configure_hub():
    """Set OTA URL on hub"""
    import socket
    local_ip = socket.gethostbyname(socket.gethostname())
    ota_url = f"http://{local_ip}:{OTA_PORT}"
    
    response = requests.post(
        f"http://{HUB_IP}/api/settings/ota-urls",
        json={"lightNodeOtaUrl": ota_url}
    )
    
    if response.json().get("success"):
        print(f"✓ Hub configured with OTA URL: {ota_url}")
    else:
        print(f"✗ Failed to configure hub: {response.text}")
        return False
    return True

def trigger_ota():
    """Start OTA transfer"""
    response = requests.post(f"http://{HUB_IP}/api/nodes/light/apply-update")
    data = response.json()
    
    if data.get("success"):
        print(f"✓ OTA started for {data.get('deviceCount', 1)} device(s)")
        return True
    else:
        print(f"✗ OTA failed to start: {data.get('error')}")
        return False

def poll_status():
    """Poll OTA status until complete"""
    while True:
        response = requests.get(f"http://{HUB_IP}/api/nodes/light/ota-status")
        data = response.json()
        
        progress = data.get("progress", 0)
        chunks = data.get("firmwareChunks", 0)
        total = data.get("firmwareTotalChunks", 0)
        
        print(f"  Progress: {progress}% ({chunks}/{total} chunks)", end="\r")
        
        if data.get("completed"):
            print()
            if data.get("success"):
                updated = data.get("devicesUpdated", 1)
                failed = data.get("devicesFailed", 0)
                print(f"✓ OTA completed! {updated} updated, {failed} failed")
            else:
                print(f"✗ OTA failed: {data.get('error')}")
            return data.get("success")
        
        time.sleep(2)

def main():
    print("=" * 50)
    print("ESP-NOW OTA Test")
    print("=" * 50)
    
    # Check firmware exists
    if not os.path.exists(FIRMWARE_PATH):
        print(f"✗ Firmware not found: {FIRMWARE_PATH}")
        print("  Run: pio run -e node_lighting")
        return
    
    setup_ota_server()
    httpd = start_http_server()
    
    if not configure_hub():
        return
    
    if not trigger_ota():
        return
    
    print("\nMonitoring progress...")
    success = poll_status()
    
    httpd.shutdown()
    print("\n✓ Test complete")

if __name__ == "__main__":
    main()
```

Run with:
```bash
python3 test/ota_test.py
```

---

## Troubleshooting

### OTA_BEGIN Never Received

**Symptom**: Hub sends BEGIN but node doesn't respond

**Solutions**:
1. Verify ESP-NOW channel matches (check `ESPNOW_CHANNEL` in platformio.ini)
2. Ensure node is registered as peer on hub
3. Check node is actually running and receiving messages
4. Verify MAC address is correct

### Chunks Lost / Missing

**Symptom**: Node receives fewer chunks than sent

**Solutions**:
1. Increase inter-chunk delay (currently 30ms)
2. Check for WiFi interference
3. Reduce distance between hub and node
4. Use repeater node for range extension

### READY_END Never Sent

**Symptom**: Hub times out waiting for READY_END

**Solutions**:
1. Verify last chunk has `finalCommand = true`
2. Check node's `chunksReceived >= totalChunks` logic
3. Increase wait timeout on hub (currently 30s)
4. Check node serial output for errors

### OTA Applies But Node Crashes

**Symptom**: Node reboots in loop after OTA

**Solutions**:
1. Verify firmware is compiled for correct board
2. Check OTA partition size is sufficient
3. Verify firmware.bin is not corrupted
4. Test firmware via USB upload first

### Out of Memory on Node

**Symptom**: Node crashes during OTA with OOM

**Solutions**:
1. Reduce serial logging (already minimized in current code)
2. Don't allocate large buffers during OTA
3. Use streaming write (already implemented for firmware)
4. Config files should be small (<4KB)

### Hub Runs Out of Memory

**Symptom**: Hub crashes during file download

**Solutions**:
1. Use streaming download (already implemented)
2. Ensure sufficient LittleFS space
3. Check heap before starting OTA
4. Limit concurrent operations

---

## File Locations

| Component | File Path |
|-----------|-----------|
| Hub OTA Task | `src/main.cpp` (nodeOtaTask function) |
| Node OTA Handlers | `lib/NodeBase/node_base.cpp` (handleOta* functions) |
| Protocol Definitions | `include/protocol/messages.h` |
| Web UI HTML | `src/hub/data/UI/settings/update-software.html` |
| Web UI JavaScript | `src/hub/data/UI/settings/settings-update-software.js` |

---

## Version History

| Date | Version | Changes |
|------|---------|---------|
| 2026-01-24 | 1.0 | Initial implementation with multi-device support |
| 2026-01-24 | 1.1 | Added READY_END handshake protocol |
| 2026-01-24 | 1.2 | Added OTA file cleanup after transfer |

---

**Last Updated**: January 24, 2026  
**Author**: Aquarium Management System Team  
**Repository**: bghosh412/aquarium-management-system
