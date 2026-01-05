# Protocol Update Summary - Unmapped Device Discovery

**Implementation of hub-driven device provisioning architecture**

---

## ✅ Changes Implemented

### 1. Protocol Messages (`include/protocol/messages.h`)

#### Updated Message Types
```cpp
enum class MessageType : uint8_t {
    ANNOUNCE = 0x01,    // Node discovery (no name/tankId)
    ACK = 0x02,         // Hub acknowledges
    CONFIG = 0x03,      // ⭐ NEW - Hub provisions device
    COMMAND = 0x04,     // (was 0x03)
    STATUS = 0x05,      // (was 0x04)  
    HEARTBEAT = 0x06    // (was 0x05)
};
```

#### Modified ANNOUNCE Message
```cpp
// BEFORE (Incorrect)
struct AnnounceMessage {
    MessageHeader header;          // tankId from node
    char nodeName[16];            // From node config
    uint8_t firmwareVersion;
    uint8_t capabilities;
};

// AFTER (Correct)
struct AnnounceMessage {
    MessageHeader header;          // tankId = 0 (unmapped)
    uint8_t firmwareVersion;
    uint8_t capabilities;
    uint8_t reserved[16];
    // NO nodeName field
};
```

#### New CONFIG Message
```cpp
struct ConfigMessage {
    MessageHeader header;          // tankId = assigned by hub
    char deviceName[16];          // Name assigned by hub
    uint8_t configData[32];       // Device-specific config
};
```

---

### 2. Hub Changes (`src/main.cpp`, `src/managers/AquariumManager.cpp`)

#### onAnnounceReceived()
```cpp
void onAnnounceReceived(const uint8_t* mac, const AnnounceMessage& msg) {
    // Removed: msg.nodeName reference
    // Added: Check for tankId == 0 (unmapped)
    // Added: Warning for unmapped devices
    
    if (msg.header.tankId == 0) {
        Serial.println("⚠️  UNMAPPED DEVICE (needs provisioning)");
    }
}
```

#### AquariumManager::handleAnnounce()
```cpp
void AquariumManager::handleAnnounce(...) {
    // Added: Check for tankId == 0
    if (msg.header.tankId == 0) {
        Serial.println("⚠️  Unmapped device, storing for provisioning");
        // TODO: Store in unmapped-devices.json
        _sendAck(mac, 0, true);
        return;
    }
    
    // Changed: Use default name instead of msg.nodeName
    const char* deviceName = "UnknownDevice";
    Device* device = _createDevice(mac, msg.header.nodeType, deviceName);
}
```

---

### 3. Node Changes (`src/nodes/lighting/src/main.cpp`)

#### Send Unmapped ANNOUNCE
```cpp
void setup() {
    // Send ANNOUNCE without name
    AnnounceMessage announce = {};
    announce.header.tankId = config.tankId;  // 0 = unmapped
    announce.firmwareVersion = config.firmwareVersion;
    // NO nodeName field
    
    if (config.tankId == 0) {
        Serial.println("⚠️  Node is UNMAPPED - waiting for provisioning");
    }
}
```

#### Default Config (`node_config.txt`)
```ini
# Node Identity (0 = unmapped)
NODE_TANK_ID=0
NODE_NAME=UnmappedLight
FIRMWARE_VERSION=1
```

---

### 4. New Files Created

#### `unmapped-devices.json`
```json
{
  "unmappedDevices": [
    {
      "mac": "AA:BB:CC:DD:EE:FF",
      "type": "LIGHT",
      "firmwareVersion": 1,
      "discoveredAt": 1704153600000,
      "lastSeen": 1704153600000,
      "status": "DISCOVERED"
    }
  ]
}
```

#### `DEVICE_DISCOVERY_PROVISIONING.md`
Complete documentation of new discovery flow with:
- Phase 1: Discovery (unmapped)
- Phase 2: User provisioning via Web UI
- Phase 3: Normal operation
- API endpoints design
- Testing procedures

#### `ESPNOW_MESSAGE_FLOW.md` (Updated)
Updated message flow diagrams showing:
- Unmapped device discovery
- CONFIG message provisioning
- Three-phase connection lifecycle

---

## 📊 Build Results

```
Environment    Status    Duration     RAM      Flash
-------------  --------  -----------  -------  --------
hub_esp32      SUCCESS   16.20s       15.3%    87.7%
node_lighting  SUCCESS   3.65s        41.0%    30.0%
```

✅ **All compilation successful!**

---

## 🔄 New Message Flow

### Phase 1: Discovery
```
Node (tankId=0) ──ANNOUNCE──> Hub
                              Hub stores in unmapped-devices.json
Node            <───ACK─────  Hub
```

### Phase 2: Provisioning
```
User fills form in Web UI
Hub updates devices.json
Hub ──CONFIG(tankId=1, name="Light01")──> Node
                                           Node saves to LittleFS
Hub <───STATUS(ack)────────────────────── Node
```

### Phase 3: Operation
```
Node (tankId=1) ──HEARTBEAT──> Hub
Node            <──COMMAND───  Hub
Node            ──STATUS────>  Hub
```

---

## ⬜ Pending Implementation

### Hub Side (Critical)
1. ⬜ Load/save `unmapped-devices.json`
2. ⬜ Add device to unmapped list in `handleAnnounce()`
3. ⬜ Implement `onConfigReceived()` callback (for node STATUS ack)
4. ⬜ Implement `/api/unmapped-devices` endpoint
5. ⬜ Implement `/api/provision-device` endpoint
6. ⬜ Send CONFIG message when user provisions device

### Node Side (Critical)
1. ⬜ Implement `onConfigReceived()` callback
2. ⬜ Save CONFIG to `/node_config.txt` in LittleFS
3. ⬜ Send STATUS acknowledgment after CONFIG
4. ⬜ Reload config without reboot (or trigger soft reboot)

### Web UI (add-device.html)
1. ⬜ Fetch `/api/unmapped-devices` on page load
2. ⬜ Display unmapped devices in real-time
3. ⬜ Add provision form with tank selection
4. ⬜ POST to `/api/provision-device` on submit
5. ⬜ Show success/failure notifications
6. ⬜ Auto-refresh device list after provisioning

### Documentation
1. ✅ Protocol messages updated
2. ✅ Discovery flow documented
3. ✅ Message flow diagrams updated
4. ⬜ API documentation
5. ⬜ Web UI integration guide

---

## 🧪 Next Steps for Testing

### Test 1: Fresh Node Boot
```bash
# Flash node with tankId=0
pio run --environment node_lighting --target uploadfs  # Upload config
pio run --environment node_lighting --target upload    # Upload firmware
pio device monitor --environment node_lighting

# Expected output:
# "📡 ANNOUNCE sent (tankId=0, FW=v1)"
# "⚠️  Node is UNMAPPED - waiting for provisioning from hub"
```

### Test 2: Hub Reception
```bash
pio device monitor --environment hub_esp32

# Expected output:
# "📡 ANNOUNCE from AA:BB:CC:DD:EE:FF"
# "Type: 2 | Tank: 0 | FW: v1"
# "⚠️  UNMAPPED DEVICE (needs provisioning)"
# "✅ ACK sent to device"
```

### Test 3: Web UI Discovery
```
1. Navigate to http://<hub-ip>/device/add-device.html
2. Should see "Discovered Devices" section
3. Should list: "Light (AA:BB:CC:DD:EE:FF) - FW v1"
4. Fill form: Name="Living Room Light", Tank="Living Room"
5. Submit → Hub sends CONFIG
6. Node receives CONFIG, saves, sends STATUS
7. Device appears in aquarium dashboard
```

---

## 🎯 Key Benefits

1. ✅ **No pre-configuration** - Flash once, use anywhere
2. ✅ **Centralized management** - Hub controls all mappings
3. ✅ **Easy reassignment** - Move devices between tanks
4. ✅ **Discoverable** - New devices auto-appear in UI
5. ✅ **Fail-safe** - Unmapped devices can't execute commands (tankId=0)

---

## 📝 Code Locations

| Component | File | Lines |
|-----------|------|-------|
| Protocol | `include/protocol/messages.h` | 11-27, 43-52 |
| Hub ANNOUNCE handler | `src/main.cpp` | 791-822 |
| Hub device registration | `src/managers/AquariumManager.cpp` | 146-185 |
| Node ANNOUNCE send | `src/nodes/lighting/src/main.cpp` | 433-449 |
| Node config file | `src/nodes/lighting/data/node_config.txt` | 5-7 |
| Unmapped devices JSON | `src/hub/data/config/unmapped-devices.json` | All |

---

**Last Updated:** January 2, 2026  
**Status:** Protocol ✅, Hub partial ⬜, Node partial ⬜, Web UI ⬜  
**Build:** Both environments compile successfully  
**Ready for:** Hardware testing with unmapped device discovery
