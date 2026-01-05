# ESP-NOW Message Flow: Hub ↔ Lighting Node

**Visual guide to ESP-NOW communication patterns based on actual implementation**

---

## 🔄 Discovery & Connection Flow

```
┌─────────────────┐                                    ┌─────────────────┐
│  Lighting Node  │                                    │   Hub (ESP32)   │
│   (ESP8266)     │                                    │                 │
│   UNMAPPED      │                                    │                 │
└────────┬────────┘                                    └────────┬────────┘
         │                                                      │
         │ Boot & Init ESPNowManager                           │ Listening...
         │ Channel 6                                           │ Channel 6
         │ tankId = 0 (unmapped)                               │
         │ name = "" (unknown)                                 │
         ├──────────────────────────────────────────────────>  │
         │  ANNOUNCE (broadcast FF:FF:FF:FF:FF:FF)             │
         │  • NodeType: LIGHT (2)                              │
         │  • Tank ID: 0 (unmapped)                            │
         │  • FW Version: 1                                    │
         │  • NO NAME FIELD                                    │
         │                                                     │
         │                                           ╔═════════╧═════════╗
         │                                           ║ Processes ANNOUNCE ║
         │                                           ║ • Checks if known  ║
         │                                           ║ • NOT FOUND        ║
         │                                           ║ • Add to unmapped  ║
         │                                           ║   devices JSON     ║
         │                                           ╚═════════╤═════════╝
         │  <──────────────────────────────────────────────── │
         │  ACK (unicast to node MAC)                         │
         │  • Assigned Node ID: 42                            │
         │  • Accepted: true                                  │
         │                                                     │
╔════════╧════════╗                                           │
║ Processes ACK    ║                                           │
║ • Adds hub peer  ║                                           │
║ • Switches to    ║                                           │
║   unicast mode   ║                                           │
║ • tankId still 0 ║                                           │
╚════════╤════════╝                                           │
         │                                                     │
         │ ⚠️  CONNECTED but UNMAPPED                          │ 📋 Device discovered
         │ (Can't receive commands yet)                        │ (Shows in Web UI)
         │                                                     │
         │ ════════ USER PROVISIONS DEVICE ════════           │
         │                                                     │
         │                                           ╔═════════╧═════════╗
         │                                           ║ User maps device   ║
         │                                           ║ • Name: "Light01"  ║
         │                                           ║ • Tank: 1          ║
         │                                           ║ • Move to devices  ║
         │                                           ║   JSON from        ║
         │                                           ║   unmapped JSON    ║
         │                                           ╚═════════╤═════════╝
         │  <──────────────────────────────────────────────── │
         │  CONFIG (unicast to node MAC)                      │
         │  • Tank ID: 1                                      │
         │  • Device Name: "Light01"                          │
         │                                                     │
╔════════╧════════╗                                           │
║ Processes CONFIG ║                                           │
║ • Save to        ║                                           │
║   LittleFS       ║                                           │
║ • Update tankId  ║                                           │
║ • Update name    ║                                           │
╚════════╤════════╝                                           │
         │                                                     │
         │──────────────────────────────────────────────────>  │
         │  STATUS (ack config)                                │
         │  • Status Code: 0 (success)                         │
         │                                                     │
         │ ✅ CONNECTED & MAPPED                               │ ✅ DEVICE PROVISIONED
         │ Ready for commands                                  │ Ready to control
         │                                                     │
```

### Timing Details

| Event | Interval | Retry Logic |
|-------|----------|-------------|
| **ANNOUNCE** | Every 5 seconds | Until ACK received |
| **ACK Response** | Immediate | Single response per ANNOUNCE |
| **CONFIG** | On-demand | After user provisions device |
| **Connection Timeout** | After ACK | No re-announcement unless disconnected |

**Code Reference:**
```cpp
// Node: src/nodes/lighting/src/main.cpp
config.announceIntervalMs = 5000;  // 5 seconds between ANNOUNCEs

// Sends with tankId=0 (unmapped):
AnnounceMessage announce;
announce.header.tankId = 0;  // UNMAPPED
announce.firmwareVersion = 1;
// NO nodeName field
ESPNowManager::getInstance().send(broadcast, &announce, sizeof(announce));
```

---

## 💓 Heartbeat Flow (After Connection)

```
┌─────────────────┐                                    ┌─────────────────┐
│  Lighting Node  │                                    │   Hub (ESP32)   │
└────────┬────────┘                                    └────────┬────────┘
         │                                                      │
         │◄─────────────── Every 30 seconds ──────────────────►│
         │                                                      │
    t=0s │──────────────────────────────────────────────────>  │
         │  HEARTBEAT (unicast to hub MAC)                     │
         │  • Health: 100%                                     │
         │  • Uptime: 0 minutes                                │
         │                                                     │
         │                                           ╔═════════╧═════════╗
         │                                           ║ Updates timestamp  ║
         │                                           ║ Marks peer ONLINE  ║
         │                                           ╚═════════╤═════════╝
         │                                                     │
   t=30s │──────────────────────────────────────────────────>  │
         │  HEARTBEAT                                          │
         │  • Health: 100%                                     │
         │  • Uptime: 1 minute                                 │
         │                                                     │
   t=60s │──────────────────────────────────────────────────>  │
         │  HEARTBEAT                                          │
         │  • Health: 100%                                     │
         │  • Uptime: 2 minutes                                │
         │                                                     │
         │                                                     │
         │  ⚠️ If > 60s without heartbeat...                   │
         │                                           ╔═════════╧═════════╗
         │                                           ║ Timeout detected!  ║
         │                                           ║ Mark peer OFFLINE  ║
         │                                           ║ Block commands     ║
         │                                           ╚═════════╤═════════╝
         │                                                     │
```

### Timing Details

| Parameter | Value | Purpose |
|-----------|-------|---------|
| **Heartbeat Interval** | 30 seconds | Node sends periodic alive signal |
| **Timeout Threshold** | 60 seconds | Hub marks node offline |
| **Reconnection** | Automatic | Node re-announces if disconnected |

**Code Reference:**
```cpp
// Node: node_config.txt
HEARTBEAT_INTERVAL_MS=30000    // Send every 30s

// Hub: src/main.cpp
ESPNowManager::getInstance().checkPeerTimeouts(60000);  // 60s timeout
```

---

## 📡 Command & Acknowledgment Flow

```
┌─────────────────┐                                    ┌─────────────────┐
│  Lighting Node  │                                    │   Hub (ESP32)   │
└────────┬────────┘                                    └────────┬────────┘
         │                                                      │
         │                                           ╔═════════╧═════════╗
         │                                           ║ User/Schedule      ║
         │                                           ║ triggers command   ║
         │                                           ╚═════════╤═════════╝
         │                                                     │
         │  <──────────────────────────────────────────────── │
         │  COMMAND (unicast to node MAC)                     │
         │  • Command Type: 20 (Set All Channels)             │
         │  • Data: [20, 255, 128, 64, 0, 0, ...]             │
         │  • commandSeqID: 0                                 │
         │  • finalCommand: true                              │
         │                                                     │
╔════════╧════════╗                                           │
║ Processes CMD    ║                                           │
║ • Parse data     ║                                           │
║ • Set W=255      ║                                           │
║ • Set B=128      ║                                           │
║ • Set R=64       ║                                           │
║ • Enable lights  ║                                           │
╚════════╤════════╝                                           │
         │                                                     │
         │──────────────────────────────────────────────────>  │
         │  STATUS (unicast to hub MAC)                        │
         │  • Command ID: 20 (echo)                            │
         │  • Status Code: 0 (SUCCESS)                         │
         │  • Data: [255, 128, 64, 1, 0, ...]                  │
         │    (current state: W, B, R, enabled)                │
         │                                                     │
         │                                           ╔═════════╧═════════╗
         │                                           ║ Receives STATUS    ║
         │                                           ║ Command confirmed  ║
         │                                           ║ Update UI          ║
         │                                           ╚═════════╤═════════╝
         │                                                     │
         │  💡 LEDs update: W=255, B=128, R=64                 │
         │                                                     │
```

### Message Details

**COMMAND Structure (32 bytes payload):**
```
Byte 0:    Command Type (1=White, 2=Blue, 3=Red, 10=Enable, 20=All)
Byte 1-N:  Command-specific data
```

**STATUS Structure (32 bytes payload):**
```
Byte 0:    White level (0-255)
Byte 1:    Blue level (0-255)
Byte 2:    Red level (0-255)
Byte 3:    Enabled (0=off, 1=on)
```

**Code Reference:**
```cpp
// Node: onCommandReceived() in main.cpp
switch (commandType) {
    case 20: // Set all channels
        lightState.whiteLevel = data[1];  // 255
        lightState.blueLevel = data[2];   // 128
        lightState.redLevel = data[3];    // 64
        lightState.enabled = true;
        break;
}

// Node sends STATUS
status.statusData[0] = lightState.whiteLevel;
status.statusData[1] = lightState.blueLevel;
status.statusData[2] = lightState.redLevel;
status.statusData[3] = lightState.enabled ? 1 : 0;
```

---

## 🔀 Complete Session Timeline

```
TIME    NODE                                    HUB
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
00:00   Boot
00:01   Load config
00:02   Init ESP-NOW (CH 6)
00:03   ────ANNOUNCE───────────────────────────>
        Wait for ACK...
00:03                                           Receive ANNOUNCE
00:03                                           Add peer
00:03   <──────────ACK────────────────────────
        Process ACK
        Add hub as peer
        ✅ CONNECTED

00:33   ────HEARTBEAT──────────────────────────>
                                                Update timestamp
01:03   ────HEARTBEAT──────────────────────────>
                                                Update timestamp
01:33   ────HEARTBEAT──────────────────────────>
                                                Update timestamp

02:00                                           User clicks "Set Lights"
02:00   <──────COMMAND (Type 20)───────────────
        Parse: W=255, B=128, R=64
        Update LEDs
02:00   ────STATUS (Success)───────────────────>
                                                Confirm command
                                                Update UI

02:03   ────HEARTBEAT──────────────────────────>
02:33   ────HEARTBEAT──────────────────────────>
03:03   ────HEARTBEAT──────────────────────────>

        ... continues every 30s ...
```

---

## 📊 Message Frequency Summary

| Message Type | Direction | Frequency | Condition |
|--------------|-----------|-----------|-----------|
| **ANNOUNCE** | Node → Hub | Every 5s | Until ACK received |
| **ACK** | Hub → Node | Once | Response to ANNOUNCE |
| **HEARTBEAT** | Node → Hub | Every 30s | After connection |
| **COMMAND** | Hub → Node | On-demand | User/schedule triggered |
| **STATUS** | Node → Hub | Immediate | Response to COMMAND |

---

## 🎨 Verbose Logging Output

### Node Serial Output
```
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
📡 Initializing ESPNowManager...
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
✅ ESPNowManager ready
📡 ANNOUNCE sent (Node: TestLightNode)

╔════════════════════════════════════════════════════════╗
║ ✅ ACK received from XX:XX:XX:XX:XX:XX
║ Assigned Node ID: 1
║ Accepted: YES
╚════════════════════════════════════════════════════════╝
✅ Connected to hub - ready for commands

💓 Heartbeat sent (uptime: 0min)
💓 Heartbeat sent (uptime: 1min)

╔════════════════════════════════════════════════════════╗
║ 📥 COMMAND received (32 bytes)
║ From: XX:XX:XX:XX:XX:XX
║ Command Type: 20
║ ✓ All channels: W=255 B=128 R=64
╚════════════════════════════════════════════════════════╝
📤 STATUS sent (code=0)
💡 Light State: ON | W=255 B=128 R=64
```

### Hub Serial Output
```
╔════════════════════════════════════════════════════════╗
║ 📡 ANNOUNCE from AA:BB:CC:DD:EE:FF
║ Node: TestLightNode
║ Type: 2 | Tank: 1 | FW: v1
╚════════════════════════════════════════════════════════╝
✅ ACK sent to TestLightNode

💓 HEARTBEAT from AA:BB:CC:DD:EE:FF | Health: 100% | Uptime: 0min
💓 HEARTBEAT from AA:BB:CC:DD:EE:FF | Health: 100% | Uptime: 1min

╔════════════════════════════════════════════════════════╗
║ 📊 STATUS from AA:BB:CC:DD:EE:FF
║ Command ID: 20 | Status Code: 0
║ Type: 2 | Tank: 1
║ Data: FF 80 40 01 00 00 00 00
╚════════════════════════════════════════════════════════╝
```

---

## 🔧 Configuration Impact

### Node Config (`node_config.txt`)
```ini
ANNOUNCE_INTERVAL_MS=5000      # How often to retry ANNOUNCE
HEARTBEAT_INTERVAL_MS=30000    # How often to send HEARTBEAT
CONNECTION_TIMEOUT_MS=90000    # When to enter fail-safe (not used actively)
DEBUG_ESPNOW=true              # Enable verbose logging above
```

### Hub Config (`hub_config.txt`)
```ini
DEBUG_ESPNOW=true              # Enable verbose logging above
```

### Hub Timeout Check (in code)
```cpp
// main.cpp loop():
ESPNowManager::getInstance().checkPeerTimeouts(60000);  // 60s timeout
```

---

## 🚨 Error & Recovery Scenarios

### Scenario 1: Node Loses Connection
```
NODE                                    HUB
 │                                       │
 │──HEARTBEAT (t=0s)──────────────────> │ ✓
 │──HEARTBEAT (t=30s)─────────────────> │ ✓
 │                                       │
 ✗ Network issue / power glitch          │
 │                                       │
 │ (t=60s - no heartbeat received)      │
 │                                ╔══════╧══════╗
 │                                ║ TIMEOUT!     ║
 │                                ║ Mark OFFLINE ║
 │                                ╚══════╤══════╝
 │                                       │
 │ Node reboots                          │
 │──ANNOUNCE──────────────────────────> │ ✓ Re-register
 │<─────────ACK────────────────────────  │
 │ ✅ Reconnected                         │ ✅ Back online
```

### Scenario 2: Command Fails (Node Offline)
```
NODE (OFFLINE)                          HUB
 │                                       │
 │                                ╔══════╧══════╗
 │                                ║ Check online ║
 │                                ║ → OFFLINE!   ║
 │                                ╚══════╤══════╝
 │                                       │
 │                                       ⚠️ Command blocked
 │                                       Log: "Device OFFLINE"
 │                                       │
```

**Code Reference:**
```cpp
// Hub: Device.cpp sendCommand()
if (!ESPNowManager::getInstance().isPeerOnline(_mac)) {
    Serial.printf("⚠️  Device %s is OFFLINE, command not sent\n", _name.c_str());
    return false;
}
```

---

## 📈 Statistics Tracking

Both hub and node track ESP-NOW statistics:

```cpp
struct Statistics {
    uint32_t messagesSent;
    uint32_t messagesReceived;
    uint32_t fragmentsSent;
    uint32_t fragmentsReceived;
    uint32_t duplicatesIgnored;
    uint32_t reassemblyTimeouts;
    uint32_t retries;
    uint32_t sendFailures;
};
```

**Logged every 60 seconds when `DEBUG_ESPNOW=true`**

---

## 🎯 Key Takeaways

1. **ANNOUNCE retries every 5s** until ACK received
2. **HEARTBEAT every 30s** after connection established
3. **Hub timeout: 60s** without heartbeat → mark offline
4. **COMMAND/STATUS** are immediate (user/schedule triggered)
5. **All timings configurable** via config files
6. **Verbose logging** shows exact message flow

---

**Document Version:** 1.0  
**Last Updated:** January 2, 2026  
**Based on:** ESPNowManager v1.0.0
