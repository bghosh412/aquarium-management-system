# Lighting Node Command Structure

**Reference guide for ESP-NOW lighting commands between Hub and Lighting Node**

---

## 📡 Command Format

All lighting commands follow this structure:

```
CommandMessage {
    MessageHeader header;     // Standard ESP-NOW header
    uint8_t commandId;        // Unique command identifier
    uint8_t commandSeqID;     // Sequence for multi-part commands
    bool finalCommand;        // True if single/final command
    uint8_t commandData[32];  // Command payload
}
```

**Command Payload Structure:**
```
Byte 0:    Command Type (see below)
Byte 1-31: Command-specific data (optional)
```

---

## 🎮 Command Types

### All Channels Control

| Code | Command | Data Bytes | Description |
|------|---------|------------|-------------|
| **0** | All OFF | None | Turn all 3 channels OFF, disabled=true |
| **1** | All ON | Optional: [ch1, ch2, ch3] | Turn all channels ON. If bytes 1-3 provided, use those levels (0-255). Otherwise default to 255 for all. |

**Example - All OFF:**
```cpp
uint8_t cmdData[] = {0};  // Just command type
device.sendCommand(cmdData, 1);
```

**Example - All ON (default max):**
```cpp
uint8_t cmdData[] = {1};  // All channels to 255
device.sendCommand(cmdData, 1);
```

**Example - All ON (custom levels):**
```cpp
uint8_t cmdData[] = {1, 200, 150, 100};  // Ch1=200, Ch2=150, Ch3=100
device.sendCommand(cmdData, 4);
```

---

### Individual Channel Control

| Code | Command | Data Bytes | Description |
|------|---------|------------|-------------|
| **10** | Channel 1 OFF | None | Turn Channel 1 (White) OFF (0) |
| **11** | Channel 1 ON | Optional: [level] | Turn Channel 1 (White) ON. Byte 1 = level (0-255), default 255 |
| **20** | Channel 2 OFF | None | Turn Channel 2 (Blue) OFF (0) |
| **21** | Channel 2 ON | Optional: [level] | Turn Channel 2 (Blue) ON. Byte 1 = level (0-255), default 255 |
| **30** | Channel 3 OFF | None | Turn Channel 3 (Red) OFF (0) |
| **31** | Channel 3 ON | Optional: [level] | Turn Channel 3 (Red) ON. Byte 1 = level (0-255), default 255 |

**Example - Channel 1 OFF:**
```cpp
uint8_t cmdData[] = {10};
device.sendCommand(cmdData, 1);
```

**Example - Channel 2 ON (default max):**
```cpp
uint8_t cmdData[] = {21};  // Blue to 255
device.sendCommand(cmdData, 1);
```

**Example - Channel 3 ON (custom level):**
```cpp
uint8_t cmdData[] = {31, 128};  // Red to 128 (50%)
device.sendCommand(cmdData, 2);
```

---

## 📊 Status Response (Placeholder)

After processing a command, the node sends a STATUS message:

```
StatusMessage {
    MessageHeader header;
    uint8_t commandId;        // Echo of command type
    uint8_t statusCode;       // 0=success, 1=error
    uint8_t statusData[32];   // Status payload
}
```

**Status Payload Structure (for future use):**
```
Byte 0: Channel 1 level (0-255)
Byte 1: Channel 2 level (0-255)
Byte 2: Channel 3 level (0-255)
Byte 3: Enabled (0=off, 1=on)
```

**Note:** Status structure is currently a placeholder and not actively used by the hub.

---

## 🔧 Code Implementation Locations

### Hub Side (Command Sending)

**Device.cpp** (`src/models/Device.cpp` line 121):
```cpp
bool Device::sendCommand(const uint8_t* commandData, size_t length) {
    CommandMessage cmd;
    // ... populate header
    memcpy(cmd.commandData, commandData, length);  // Copy command bytes
    ESPNowManager::getInstance().send(_mac, &cmd, sizeof(cmd));
}
```

**LightDevice.h** (`include/models/devices/LightDevice.h` line 216):
```cpp
namespace LightCommands {
    constexpr uint8_t CMD_ALL_OFF = 0;
    constexpr uint8_t CMD_ALL_ON = 1;
    constexpr uint8_t CMD_CH1_OFF = 10;
    constexpr uint8_t CMD_CH1_ON = 11;
    constexpr uint8_t CMD_CH2_OFF = 20;
    constexpr uint8_t CMD_CH2_ON = 21;
    constexpr uint8_t CMD_CH3_OFF = 30;
    constexpr uint8_t CMD_CH3_ON = 31;
}
```

**Note:** `LightDevice.cpp` does not exist yet. Command building logic needs to be implemented.

---

### Node Side (Command Receiving)

**lighting/main.cpp** (`src/nodes/lighting/src/main.cpp` line 195):
```cpp
void onCommandReceived(const uint8_t* mac, const uint8_t* data, size_t len) {
    uint8_t commandType = data[0];
    
    switch (commandType) {
        case 0:  // All OFF
            lightState.whiteLevel = 0;
            lightState.blueLevel = 0;
            lightState.redLevel = 0;
            lightState.enabled = false;
            break;
            
        case 1:  // All ON
            if (len >= 4) {
                lightState.whiteLevel = data[1];
                lightState.blueLevel = data[2];
                lightState.redLevel = data[3];
            } else {
                // Default to max
                lightState.whiteLevel = 255;
                lightState.blueLevel = 255;
                lightState.redLevel = 255;
            }
            lightState.enabled = true;
            break;
            
        case 10: // Ch1 OFF
            lightState.whiteLevel = 0;
            break;
            
        case 11: // Ch1 ON
            lightState.whiteLevel = (len >= 2) ? data[1] : 255;
            break;
            
        // ... similar for 20/21/30/31
    }
}
```

---

## 🎯 Usage Examples

### Example 1: Turn All Lights OFF
```cpp
// Hub code
LightDevice* light = getLightDevice();
uint8_t cmd[] = {LightCommands::CMD_ALL_OFF};
light->sendCommand(cmd, 1);
```

**Result on Node:**
- Channel 1 (White): 0
- Channel 2 (Blue): 0
- Channel 3 (Red): 0
- Enabled: false

---

### Example 2: Set Specific Color Mix
```cpp
// Hub code - Warm white (high white, some red, low blue)
uint8_t cmd[] = {
    LightCommands::CMD_ALL_ON,
    255,  // White full
    50,   // Blue low
    150   // Red medium
};
light->sendCommand(cmd, 4);
```

**Result on Node:**
- Channel 1 (White): 255
- Channel 2 (Blue): 50
- Channel 3 (Red): 150
- Enabled: true

---

### Example 3: Individual Channel Control
```cpp
// Hub code - Blue only at 50%
uint8_t cmd1[] = {LightCommands::CMD_CH1_OFF};  // White off
light->sendCommand(cmd1, 1);

uint8_t cmd2[] = {LightCommands::CMD_CH2_ON, 128};  // Blue 50%
light->sendCommand(cmd2, 2);

uint8_t cmd3[] = {LightCommands::CMD_CH3_OFF};  // Red off
light->sendCommand(cmd3, 1);
```

**Result on Node:**
- Channel 1 (White): 0
- Channel 2 (Blue): 128
- Channel 3 (Red): 0

---

## 🧪 Testing Commands

### Serial Monitor Test (Node Side)

When commands are received, the node logs:

```
╔════════════════════════════════════════════════════════╗
║ 📥 COMMAND received (4 bytes)
║ From: AA:BB:CC:DD:EE:FF
║ Command Type: 1
║ ✓ All channels ON: W=255 B=128 R=64
╚════════════════════════════════════════════════════════╝
📤 STATUS sent (code=0)
💡 Light State: ON | W=255 B=128 R=64
```

### Manual Testing via Serial (Hub Side)

You can trigger commands via web API or directly in code:

```cpp
// In hub setup() for testing
LightDevice* testLight = getDeviceByMAC(nodeMac);

// Test sequence
delay(5000);
uint8_t off[] = {0};
testLight->sendCommand(off, 1);  // All OFF

delay(3000);
uint8_t on[] = {1, 200, 100, 50};
testLight->sendCommand(on, 4);  // Custom levels

delay(3000);
uint8_t ch2[] = {21, 255};
testLight->sendCommand(ch2, 2);  // Blue full
```

---

## 📝 Future Enhancements

When `LightDevice.cpp` is implemented, it should provide helper methods:

```cpp
class LightDevice : public Device {
public:
    // High-level API (to be implemented)
    bool allOff();
    bool allOn(uint8_t ch1 = 255, uint8_t ch2 = 255, uint8_t ch3 = 255);
    bool setChannel1(bool on, uint8_t level = 255);
    bool setChannel2(bool on, uint8_t level = 255);
    bool setChannel3(bool on, uint8_t level = 255);
    
private:
    void buildCommand(uint8_t cmdType, const uint8_t* data, size_t len);
};
```

**Usage would become:**
```cpp
light->allOn(255, 128, 64);      // Instead of raw bytes
light->setChannel2(true, 128);   // Blue to 50%
light->allOff();                 // All channels off
```

---

## 🔍 Command Type Summary

| Command | Code | Bytes | Ch1 | Ch2 | Ch3 | Enabled |
|---------|------|-------|-----|-----|-----|---------|
| All OFF | 0 | 1 | 0 | 0 | 0 | false |
| All ON (default) | 1 | 1 | 255 | 255 | 255 | true |
| All ON (custom) | 1 | 4 | data[1] | data[2] | data[3] | true |
| Ch1 OFF | 10 | 1 | 0 | - | - | - |
| Ch1 ON (default) | 11 | 1 | 255 | - | - | - |
| Ch1 ON (custom) | 11 | 2 | data[1] | - | - | - |
| Ch2 OFF | 20 | 1 | - | 0 | - | - |
| Ch2 ON (default) | 21 | 1 | - | 255 | - | - |
| Ch2 ON (custom) | 21 | 2 | - | data[1] | - | - |
| Ch3 OFF | 30 | 1 | - | - | 0 | - |
| Ch3 ON (default) | 31 | 1 | - | - | 255 | - |
| Ch3 ON (custom) | 31 | 2 | - | - | data[1] | - |

**Legend:**
- **-** = unchanged from current state
- **Bytes** = minimum command length (including command type byte)

---

**Document Version:** 1.0  
**Last Updated:** January 2, 2026  
**Node Firmware:** v1.0.0  
**Build Status:** ✅ Verified (RAM 40.9%, Flash 30.0%)
