# ESPNowManager Integration Test Guide

**Complete guide for testing Hub ↔ Lighting Node communication**

---

## 🎯 Test Objective

Verify ESPNowManager library integration with:
- Hub (ESP32-S3) sending commands
- Lighting Node (ESP8266) receiving and executing commands
- Bi-directional STATUS acknowledgments
- Verbose logging for debugging

---

## 📋 Hardware Requirements

1. **Hub**: ESP32-S3-N16R8 (or compatible ESP32)
2. **Node**: ESP8266 (NodeMCU, Wemos D1 Mini, or similar)
3. **USB cables** for programming
4. **LEDs** (optional): Connect to pins D1, D2, D3 on lighting node for visual feedback

---

## 🔧 Software Requirements

- PlatformIO installed
- Python venv activated: `source .venv/bin/activate`
- Serial monitors (2 terminals recommended)

---

## 📝 Step-by-Step Test Procedure

### Step 1: Configure Node

Edit `/src/nodes/lighting/data/node_config.txt`:

```ini
NODE_TANK_ID=1
NODE_NAME=TestLightNode
FIRMWARE_VERSION=1
ESPNOW_CHANNEL=6
DEBUG_SERIAL=true
DEBUG_ESPNOW=true
DEBUG_HARDWARE=true
ANNOUNCE_INTERVAL_MS=5000
HEARTBEAT_INTERVAL_MS=30000
CONNECTION_TIMEOUT_MS=90000
```

### Step 2: Build Hub Firmware

```bash
source .venv/bin/activate
platformio run --environment hub_esp32
```

**Expected output:**
```
RAM:   [====      ]  XX% (used XXXXX bytes from XXXXX bytes)
Flash: [======    ]  XX% (used XXXXXX bytes from XXXXXXX bytes)
Building .pio/build/hub_esp32/firmware.bin
```

### Step 3: Build Lighting Node Firmware

```bash
platformio run --environment node_lighting
```

**Expected output:**
```
RAM:   [====      ]  XX% (used XXXXX bytes from XXXXX bytes)
Flash: [====      ]  XX% (used XXXXXX bytes from XXXXXXX bytes)
Building .pio/build/node_lighting/firmware.bin
```

### Step 4: Upload Filesystem to Node (Config File)

```bash
platformio run --environment node_lighting --target uploadfs
```

**Expected output:**
```
Building FS image from 'src/nodes/lighting/data' directory to .pio/build/node_lighting/littlefs.bin
/node_config.txt
Compressed X KB over X KB
```

### Step 5: Upload Hub Firmware

```bash
platformio run --environment hub_esp32 --target upload --upload-port /dev/ttyUSB0
```

**Replace `/dev/ttyUSB0` with your hub's port. Check with:**
```bash
pio device list
```

### Step 6: Upload Node Firmware

```bash
platformio run --environment node_lighting --target upload --upload-port /dev/ttyUSB1
```

**Replace `/dev/ttyUSB1` with your node's port.**

---

## 📊 Expected Serial Output

### Hub Serial Output (After Boot)

```
╔════════════════════════════════════════════════════════════╗
║  AQUARIUM MANAGEMENT SYSTEM - HUB                          ║
║  ESP32-S3-N16R8 Central Controller                         ║
╚════════════════════════════════════════════════════════════╝

✅ LittleFS mounted (used: XXXX KB / total: XXXX KB)
📄 Loading configuration...
✅ Configuration loaded
   - Heartbeat: ON (30s)
   - Memory Management: AGGRESSIVE
   - mDNS: ams.local
   
📡 WiFi Manager started
   - AP: AquariumHub
   - IP: 192.168.4.1
   
✅ mDNS started: ams.local

🏢 AquariumManager initialized
   - 0 aquariums loaded
   - 0 devices registered
   
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
📡 Initializing ESPNowManager...
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
🚀 ESPNowManager: Initializing as HUB
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
✅ RX Queue created (10 entries)
✅ WiFi Channel: 6
✅ ESP-NOW initialized
✅ Callbacks registered
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
✅ ESPNowManager Ready
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
✅ ESPNowManager ready
   - Channel: 6
   - Mode: HUB (FreeRTOS queue enabled)
   - Debug: ON
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

✅ Web server started on port 80
   - Access: http://ams.local
   - Or: http://192.168.4.1
   
✅ Watchdog task created on Core 1 (priority 2)

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
✅ HUB READY
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

💾 HEAP:  XXX KB free / XXX KB total (XX%)
💾 PSRAM: XXX KB free / XXX KB total (XX%)
⏱️  Uptime: 0 seconds
```

**Wait for node ANNOUNCE...**

```
╔════════════════════════════════════════════════════════╗
║ 📡 ANNOUNCE from AA:BB:CC:DD:EE:FF
║ Node: TestLightNode
║ Type: 2 | Tank: 1 | FW: v1
╚════════════════════════════════════════════════════════╝
✅ ACK sent to TestLightNode
```

**Periodic heartbeats:**

```
💓 HEARTBEAT from AA:BB:CC:DD:EE:FF | Health: 100% | Uptime: 0min
💓 HEARTBEAT from AA:BB:CC:DD:EE:FF | Health: 100% | Uptime: 1min
```

---

### Lighting Node Serial Output (After Boot)

```
╔═══════════════════════════════════════════════════════════╗
║          LIGHTING NODE - Aquarium Management              ║
╚═══════════════════════════════════════════════════════════╝
📄 Loading configuration...
✅ Configuration loaded
   - Node: TestLightNode (Tank 1)
   - FW Version: v1
   - ESP-NOW Channel: 6
   - Debug: Serial=ON | ESP-NOW=ON | Hardware=ON
Tank ID: 1 | Node: TestLightNode | FW: v1

✓ Lighting hardware initialized
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
📡 Initializing ESPNowManager...
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
🚀 ESPNowManager: Initializing as NODE
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
✅ WiFi Channel: 6
✅ ESP-NOW initialized
✅ Callbacks registered
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
✅ ESPNowManager Ready
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
✅ ESPNowManager ready
   - Channel: 6
   - Mode: NODE (std::queue for ESP8266)
   - Debug ESP-NOW: ON
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
📡 ANNOUNCE sent (Node: TestLightNode)

✓ Lighting node ready

╔════════════════════════════════════════════════════════╗
║ ✅ ACK received from XX:XX:XX:XX:XX:XX
║ Assigned Node ID: 1
║ Accepted: YES
╚════════════════════════════════════════════════════════╝
✅ Connected to hub - ready for commands

💓 Heartbeat sent (uptime: 0min)
💓 Heartbeat sent (uptime: 1min)
```

**When command received:**

```
╔════════════════════════════════════════════════════════╗
║ 📥 COMMAND received (32 bytes)
║ From: XX:XX:XX:XX:XX:XX
║ Command Type: 20
║ ✓ All channels: W=255 B=128 R=64
╚════════════════════════════════════════════════════════╝
📤 STATUS sent (code=0)

💡 Light State: ON | W=255 B=128 R=64
```

---

## 🧪 Test Commands (via Hub Web UI)

### Test 1: Turn On White Channel

**Send command from hub (via web UI or direct API):**

```json
POST /api/devices/{device_id}/command
{
  "commandData": [1, 255]  // Command 1 = White, 255 = max brightness
}
```

**Expected Node Output:**
```
╔════════════════════════════════════════════════════════╗
║ 📥 COMMAND received (32 bytes)
║ From: XX:XX:XX:XX:XX:XX
║ Command Type: 1
║ ✓ White level: 255
╚════════════════════════════════════════════════════════╝
📤 STATUS sent (code=0)
💡 Light State: ON | W=255 B=0 R=0
```

---

### Test 2: Set All Channels

**Command:**
```json
POST /api/devices/{device_id}/command
{
  "commandData": [20, 200, 150, 100]  // Command 20 = All channels, W=200, B=150, R=100
}
```

**Expected Node Output:**
```
╔════════════════════════════════════════════════════════╗
║ 📥 COMMAND received (32 bytes)
║ From: XX:XX:XX:XX:XX:XX
║ Command Type: 20
║ ✓ All channels: W=200 B=150 R=100
╚════════════════════════════════════════════════════════╝
📤 STATUS sent (code=0)
💡 Light State: ON | W=200 B=150 R=100
```

---

### Test 3: Disable Lights

**Command:**
```json
POST /api/devices/{device_id}/command
{
  "commandData": [10, 0]  // Command 10 = Enable/Disable, 0 = disable
}
```

**Expected Node Output:**
```
╔════════════════════════════════════════════════════════╗
║ 📥 COMMAND received (32 bytes)
║ From: XX:XX:XX:XX:XX:XX
║ Command Type: 10
║ ✓ Lights DISABLED
╚════════════════════════════════════════════════════════╝
📤 STATUS sent (code=0)
💡 Light State: OFF | W=200 B=150 R=100
```

---

## 📊 Statistics Monitoring

### Hub Statistics (Every 60 seconds)

```
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
📊 ESP-NOW Statistics (Last 60s):
   Messages: 3 sent / 15 received
   Fragments: 0 sent / 0 received
   Errors: 0 send failures / 0 reassembly timeouts
   Duplicates ignored: 0
   Retries: 0
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

### Node Statistics (Every 60 seconds)

```
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
📊 ESP-NOW Statistics (Last 60s):
   Messages: 12 sent / 3 received
   Fragments: 0 sent / 0 received
   Errors: 0 send failures / 0 reassembly timeouts
   Duplicates ignored: 0
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

---

## 🐛 Troubleshooting

### Issue: Node doesn't announce

**Symptoms:**
- Hub shows no ANNOUNCE messages
- Node sends ANNOUNCE but hub doesn't receive

**Solutions:**
1. Check ESP-NOW channel (must be 6 on both)
2. Verify both devices powered on
3. Check WiFi.mode() is WIFI_STA on node
4. Re-upload firmware

**Debug commands:**
```bash
# Check node MAC address
pio device monitor --environment node_lighting

# Look for:
MAC Address: AA:BB:CC:DD:EE:FF
```

---

### Issue: Commands not received by node

**Symptoms:**
- Hub sends command
- Node shows no COMMAND received

**Solutions:**
1. Check node shows "Connected to hub"
2. Verify heartbeats are being received by hub
3. Check device is marked ONLINE in hub
4. Check commandData format

**Debug on hub:**
```
⚠️  Device TestLightNode is OFFLINE, command not sent
```

If you see this, wait for heartbeat or restart node.

---

### Issue: Fragmentation errors

**Symptoms:**
- Reassembly timeout messages
- Partial commands received

**Solutions:**
1. Check message size (should be ≤ 32 bytes for non-fragmented)
2. Verify REASSEMBLY_TIMEOUT_MS is 1500ms
3. Check for network congestion

**Debug output:**
```
🧩 Starting reassembly for command 42
⏱️  Reassembly timeout, dropping partial message
```

---

## ✅ Success Criteria

Your integration is successful if:

- [x] Hub boots without errors
- [x] Node boots and sends ANNOUNCE
- [x] Hub receives ANNOUNCE and sends ACK
- [x] Node receives ACK and shows "Connected"
- [x] Periodic heartbeats visible on hub (every 30s)
- [x] Commands sent from hub are received by node
- [x] Node sends STATUS acknowledgment
- [x] Hub receives STATUS
- [x] Statistics show no errors
- [x] LEDs respond to commands (if hardware connected)

---

## 📈 Performance Benchmarks

**Expected values:**

| Metric | Value | Acceptable Range |
|--------|-------|------------------|
| ANNOUNCE → ACK latency | < 50ms | < 100ms |
| Command → STATUS latency | < 50ms | < 100ms |
| Heartbeat interval | 30s | ±1s |
| Message success rate | 100% | > 95% |
| Fragmentation overhead | 0ms | < 100ms |

---

## 🔍 Advanced Testing

### Test Fragmentation (128-byte message)

**Modify hub to send large command:**
```cpp
uint8_t largeData[128];
for (int i = 0; i < 128; i++) {
    largeData[i] = i;
}

ESPNowManager::getInstance().sendFragmented(
    nodeMac, 
    99,           // Command ID
    largeData, 
    128
);
```

**Expected node output:**
```
🧩 Starting reassembly for command 99
  🧩 Fragment 0 appended (32 bytes total)
  🧩 Fragment 1 appended (64 bytes total)
  🧩 Fragment 2 appended (96 bytes total)
  🧩 Fragment 3 appended (128 bytes total)
✅ Reassembly complete: 128 bytes
```

---

### Test Offline Detection

**Simulate node offline:**
1. Unplug node
2. Wait 90 seconds (CONNECTION_TIMEOUT_MS)
3. Try sending command from hub

**Expected hub output:**
```
⚠️  Device TestLightNode is OFFLINE, command not sent
```

---

### Test Duplicate Detection

**Send same command twice:**
```cpp
CommandMessage cmd;
cmd.header.sequenceNum = 42;  // Fixed sequence

ESPNowManager::getInstance().send(mac, (uint8_t*)&cmd, sizeof(cmd));
ESPNowManager::getInstance().send(mac, (uint8_t*)&cmd, sizeof(cmd));
```

**Expected node output:**
```
🚫 Duplicate message ignored (seq 42)
```

---

## 📝 Test Log Template

Use this template to document your test session:

```
Test Date: _______________
Tester: _______________

Hardware:
- Hub: ESP32-S3 (Port: /dev/ttyUSB___)
- Node: ESP8266 (Port: /dev/ttyUSB___)

Test Results:
[ ] Hub boots successfully
[ ] Node boots successfully
[ ] ANNOUNCE/ACK exchange works
[ ] Heartbeats received
[ ] Commands executed
[ ] STATUS acknowledgments received
[ ] Statistics show no errors
[ ] Fragmentation works (optional)
[ ] Offline detection works (optional)

Issues Found:
_________________________________
_________________________________

Notes:
_________________________________
_________________________________
```

---

## 🎓 Next Steps

After successful basic testing:

1. **Add more node types** (CO₂, Heater, Fish Feeder)
2. **Test multi-tank scenarios** (multiple nodes, different tank IDs)
3. **Implement scheduling** (automated light cycles)
4. **Add web UI controls** (real-time command sending)
5. **Long-term stability test** (24+ hour runtime)

---

**Document Version**: 1.0  
**Last Updated**: January 2, 2026  
**Status**: Ready for Testing

**Good luck with your integration testing! 🚀**
