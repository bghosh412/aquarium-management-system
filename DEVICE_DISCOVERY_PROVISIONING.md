# Device Discovery & Provisioning Flow

**Updated architecture for unmapped device discovery and hub-driven provisioning**

---

## 🔄 Problem with Old Flow

### ❌ Old Flow (Incorrect)
```
1. Node knows tankId + name at boot (hardcoded)
2. Node sends ANNOUNCE with tankId + name
3. Hub validates aquarium exists
4. Hub registers device
```

**Issues:**
- ❌ Node must be pre-configured before deployment
- ❌ Requires flashing firmware per aquarium
- ❌ Can't reassign devices between tanks
- ❌ No centralized device management

---

## ✅ New Flow (Correct)

### Phase 1: Discovery (Unmapped)

```
┌─────────────┐                                  ┌─────────────┐
│  New Node   │                                  │     Hub     │
│  (Unknown)  │                                  │             │
└──────┬──────┘                                  └──────┬──────┘
       │                                                │
       │ Boot & Init                                    │
       │ tankId = 0 (unmapped)                          │
       │ name = "" (empty)                              │
       │                                                │
       │──────ANNOUNCE (broadcast)───────────────────>  │
       │  • MAC: AA:BB:CC:DD:EE:FF                      │
       │  • NodeType: LIGHT (2)                         │
       │  • FW Version: 1                               │
       │  • tankId: 0 (unmapped)                        │
       │  • NO name field                               │
       │                                                │
       │                                      ╔═════════╧═════════╗
       │                                      ║ Check if known    ║
       │                                      ║ → NOT FOUND       ║
       │                                      ║ Add to unmapped   ║
       │                                      ║ devices JSON      ║
       │                                      ╚═════════╤═════════╝
       │                                                │
       │<─────────ACK (unicast)──────────────────────  │
       │  • accepted: true                              │
       │  • assignedNodeId: 42                          │
       │                                                │
       ✅ Connected but UNMAPPED                        ✅ Device discovered
       │                                                │
       │──────HEARTBEAT (every 30s)──────────────────> │
       │  • Health: 100%                                │
       │  • tankId: 0 (unmapped)                        │
       │                                                │
```

**Hub Side:**
- Receives ANNOUNCE with MAC + NodeType only
- Checks `unmapped-devices.json` for existing entry
- If new, adds to unmapped list
- Sends ACK immediately (accepted=true)
- Node appears in Web UI "Add Device" page
- Node sends heartbeats but can't receive commands (tankId=0)

---

### Phase 2: User Provisioning (Web UI)

```
┌──────────────┐                    ┌─────────────┐                    ┌─────────────┐
│   User UI    │                    │     Hub     │                    │    Node     │
└──────┬───────┘                    └──────┬──────┘                    └──────┬──────┘
       │                                   │                                  │
       │ 1. Navigate to "Add Device"       │                                  │
       │─────────────────────────────────> │                                  │
       │                                   │                                  │
       │ 2. See unmapped devices:          │                                  │
       │    "Light (AA:BB:CC:DD:EE:FF)"    │                                  │
       │<────────────────────────────────  │                                  │
       │                                   │                                  │
       │ 3. Fill form:                     │                                  │
       │    - Name: "Living Room Light"    │                                  │
       │    - Tank: "Living Room (ID=1)"   │                                  │
       │    - Submit                       │                                  │
       │─────────────────────────────────> │                                  │
       │                                   │                                  │
       │                         ╔═════════╧═════════╗                        │
       │                         ║ Update devices.json║                       │
       │                         ║ Remove from unmapped║                      │
       │                         ║ Add to tank devices║                       │
       │                         ╚═════════╤═════════╝                        │
       │                                   │                                  │
       │                                   │────CONFIG (unicast)────────────> │
       │                                   │  • tankId: 1                     │
       │                                   │  • name: "Living Room Light"     │
       │                                   │                                  │
       │                                   │                        ╔═════════╧═════════╗
       │                                   │                        ║ Save to LittleFS  ║
       │                                   │                        ║ Update tankId=1   ║
       │                                   │                        ║ Update name       ║
       │                                   │                        ╚═════════╤═════════╝
       │                                   │                                  │
       │                                   │<─────STATUS (ack config)──────   │
       │                                   │  • statusCode: 0 (success)       │
       │                                   │                                  │
       │ 4. Success notification           │                                  │
       │<────────────────────────────────  │                                  │
       │                                   │                                  │
                                           ✅ Device now mapped                ✅ Device configured
```

**Hub Actions:**
1. Move device from `unmapped-devices.json` to `devices.json`
2. Assign tankId and name
3. Send CONFIG message to node
4. Wait for STATUS acknowledgment
5. Update UI to show device in aquarium

**Node Actions:**
1. Receive CONFIG message
2. Save to `/node_config.txt` in LittleFS
3. Update runtime config (tankId, name)
4. Send STATUS acknowledgment
5. Start sending heartbeats with new tankId
6. Ready to receive commands

---

### Phase 3: Normal Operation

```
┌─────────────┐                                  ┌─────────────┐
│    Node     │                                  │     Hub     │
│  (Mapped)   │                                  │             │
└──────┬──────┘                                  └──────┬──────┘
       │                                                │
       │──────HEARTBEAT─────────────────────────────>  │
       │  • tankId: 1 (mapped)                          │
       │  • Health: 100%                                │
       │                                                │
       │<─────COMMAND────────────────────────────────  │
       │  • tankId: 1                                   │
       │  • commandType: 1 (All ON)                     │
       │                                                │
       │──────STATUS─────────────────────────────────> │
       │  • statusCode: 0 (success)                     │
       │  • Current levels                              │
       │                                                │
```

---

## 📊 Message Updates

### ANNOUNCE Message (Updated)

**Before (Incorrect):**
```cpp
struct AnnounceMessage {
    MessageHeader header;          // tankId from node config
    char nodeName[16];            // From node config
    uint8_t firmwareVersion;
    uint8_t capabilities;
};
```

**After (Correct):**
```cpp
struct AnnounceMessage {
    MessageHeader header;          // tankId = 0 (unmapped)
    uint8_t firmwareVersion;
    uint8_t capabilities;
    uint8_t reserved[16];         // Future use
};
// NO nodeName field - hub assigns name
```

### CONFIG Message (New)

```cpp
struct ConfigMessage {
    MessageHeader header;          // tankId = assigned tank
    char deviceName[16];          // Hub-assigned name
    uint8_t configData[32];       // Device-specific config
};
```

**Node receives this and saves to LittleFS.**

---

## 📁 File Structure

### Hub Side

**unmapped-devices.json** (new)
```json
{
  "unmappedDevices": [
    {
      "mac": "AA:BB:CC:DD:EE:FF",
      "type": "LIGHT",
      "firmwareVersion": 1,
      "capabilities": 0,
      "discoveredAt": 1704153600000,
      "lastSeen": 1704153600000,
      "announceCount": 5,
      "status": "DISCOVERED"
    }
  ],
  "metadata": {
    "lastCleanup": 0,
    "totalDiscovered": 0,
    "autoCleanupAfterDays": 7
  }
}
```

**devices.json** (existing - mapped devices)
```json
{
  "devices": [
    {
      "mac": "AA:BB:CC:DD:EE:FF",
      "type": "LIGHT",
      "name": "Living Room Light",
      "tankId": 1,
      "firmwareVersion": 1,
      "enabled": true,
      "status": "ONLINE"
    }
  ]
}
```

### Node Side

**node_config.txt** (updated after provisioning)
```ini
# Written by hub during CONFIG
NODE_TANK_ID=1
NODE_NAME=Living Room Light

# Device defaults
FIRMWARE_VERSION=1
ESPNOW_CHANNEL=6
DEBUG_SERIAL=true
```

---

## 🔧 API Endpoints

### GET /api/unmapped-devices
Returns list of discovered but unmapped devices.

**Response:**
```json
{
  "devices": [
    {
      "mac": "AA:BB:CC:DD:EE:FF",
      "type": "LIGHT",
      "firmwareVersion": 1,
      "discoveredAt": "2026-01-02T10:30:00Z",
      "lastSeen": "2026-01-02T10:35:00Z",
      "announceCount": 10
    }
  ]
}
```

### POST /api/provision-device
Maps an unmapped device to aquarium.

**Request:**
```json
{
  "mac": "AA:BB:CC:DD:EE:FF",
  "name": "Living Room Light",
  "tankId": 1
}
```

**Response:**
```json
{
  "success": true,
  "device": {
    "mac": "AA:BB:CC:DD:EE:FF",
    "name": "Living Room Light",
    "tankId": 1,
    "status": "PROVISIONED"
  }
}
```

---

## 🎯 Implementation Checklist

### Protocol Updates
- ✅ Add CONFIG message type (0x03)
- ✅ Update COMMAND to 0x04, STATUS to 0x05, HEARTBEAT to 0x06
- ✅ Remove nodeName field from ANNOUNCE
- ✅ Set tankId=0 for unmapped devices

### Hub Updates
- ⬜ Create `unmapped-devices.json`
- ⬜ Update `onAnnounceReceived()` to handle unmapped devices
- ⬜ Add `handleConfigMessage()` callback
- ⬜ Implement `/api/unmapped-devices` endpoint
- ⬜ Implement `/api/provision-device` endpoint
- ⬜ Update Web UI to show unmapped devices

### Node Updates
- ⬜ Remove tankId/name from node_config.txt defaults
- ⬜ Send ANNOUNCE with tankId=0, no name
- ⬜ Implement `onConfigReceived()` callback
- ⬜ Save CONFIG to LittleFS
- ⬜ Reload config after provisioning

### Web UI Updates
- ⬜ Update add-device.html to fetch unmapped devices
- ⬜ Show real-time discovered devices
- ⬜ Implement provision form
- ⬜ Handle provision success/failure

---

## 🧪 Testing Sequence

### Test 1: Fresh Node Discovery
1. Flash fresh node (no config)
2. Power on
3. Hub receives ANNOUNCE
4. Hub adds to `unmapped-devices.json`
5. Hub sends ACK
6. Verify node appears in Web UI "Add Device" page

### Test 2: User Provisioning
1. Navigate to "Add Device"
2. See discovered device
3. Fill form (name + tank)
4. Submit
5. Hub sends CONFIG to node
6. Node saves config
7. Node sends STATUS ack
8. Verify device appears in aquarium dashboard

### Test 3: Device Reboot (Already Mapped)
1. Reboot provisioned node
2. Node loads tankId + name from config
3. Node sends ANNOUNCE with tankId=1
4. Hub recognizes as existing device
5. Hub sends ACK
6. Normal operation resumes

---

**Document Version:** 1.0  
**Last Updated:** January 2, 2026  
**Status:** Protocol updated ✅, Implementation pending ⬜
