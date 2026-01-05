# Lighting Node Command Reference

**Node Type:** `LIGHT` (NodeType::LIGHT)
**Firmware Version:** 1.0
**Last Updated:** January 6, 2026

---

## Command Structure

All commands sent to the lighting node follow the ESP-NOW protocol format:

```cpp
struct CommandMessage {
    MessageHeader header;
    uint8_t commandId;
    uint8_t commandSeqID;
    bool finalCommand;
    uint8_t commandData[32];
};
```

The `commandData[0]` field contains the command type, and subsequent bytes contain parameters.

---

## Command Types

### Global Control Commands

#### 30: Channel 3 OFF

**Description:** Turn off channel 3

**Payload:**

- `commandData[0]` = 30

**Example:**

```cpp
commandData[0] = 30;
```

**Result:**

- Channel 3: OFF
- Channel 2: OFF
- Channel 3: OFF

---

#### 1: All Channels ON

**Description:** Turn on all 3 LED channels

**Payload:**

- `commandData[0]` = 1

**Example:**

```cpp
commandData[0] = 1;  // All channels ON
```

**Result:**

- Channel 1: ON
- Channel 2: ON
- Channel 3: ON

---

### Channel 1 Control

#### 10: Channel 1 OFF

**Description:** Turn off white LED channel**Payload:**

- `commandData[0]` = 10

**Example:**

```cpp
commandData[0] = 10;
```

**Result:**

- White channel: 0

---

#### 11: Channel 1 ON

**Description:** Turn on channel 1

**Payload:**

- `commandData[0]` = 11

**Example:**

```cpp
commandData[0] = 11;  // Channel 1 ON
```

**Result:**

- Channel 1: ON

---

### Channel 2 Control

#### 20: Channel 2 OFF

**Description:** Turn off blue LED channel**Payload:**

- `commandData[0]` = 20

**Example:**

```cpp
commandData[0] = 20;
```

**Result:**

- Blue channel: 0

---

#### 21: Channel 2 ON

**Description:** Turn on channel 2

**Payload:**

- `commandData[0]` = 21

**Example:**

```cpp
commandData[0] = 21;  // Channel 2 ON
```

**Result:**

- Channel 2: ON

---

### Channel 3 Control

#### 30: Channel 3 OFF

**Description:** Turn off red LED channel**Payload:**

- `commandData[0]` = 30

**Example:**

```cpp
commandData[0] = 30;
```

**Result:**

- Red channel: 0

---

#### 31: Channel 3 ON

**Description:** Turn on channel 3

**Payload:**

- `commandData[0]` = 31

**Example:**

```cpp
commandData[0] = 31;  // Channel 3 ON
```

**Result:**

- Channel 3: ON

---

## Status Responses

After processing each command, the lighting node sends a STATUS message:

```cpp
struct StatusMessage {
    MessageHeader header;
    uint8_t commandId;     // Echoes original command ID
    uint8_t statusCode;    // 0x00 = success, 0xFF = error
    uint8_t statusData[32];
};
```

**Status Codes:**

- `0x00`: Command executed successfully
- `0xFF`: Error (unknown command type or validation failure)

---

## Hardware Mapping

| Channel | GPIO Pin | ESP8266 Pin | Control Mode |
| ------- | -------- | ----------- | ------------ |
| 1       |          | D1          | ON/OFF       |
| 2       |          | D2          | ON/OFF       |
| 3       |          | D5          | ON/OFF       |

**Note:** This version uses simple ON/OFF control (digital HIGH/LOW). PWM dimming is not implemented in firmware v1.0.

---

## Protocol Messages

### Discovery Messages

#### ANNOUNCE (sent by node)

Broadcast message sent every 5 seconds when unmapped (tankId=0):

```cpp
struct AnnounceMessage {
    MessageHeader header;      // type: ANNOUNCE, tankId: 0
    char nodeName[32];         // "UnmappedLight"
    uint8_t firmwareVersion;   // 1
    uint8_t capabilities;      // 0x00
};
```

#### ACK (received by node)

Hub response to ANNOUNCE with time synchronization:

```cpp
struct AckMessage {
    MessageHeader header;
    uint8_t assignedNodeId;    // Hub-assigned ID
    bool accepted;             // true if accepted
    uint32_t unixTimestamp;    // Current Unix timestamp from hub
};
```

**Node Response:**

1. Synchronizes internal clock with hub time
2. Marks connection as established
3. Stops announcing (waits for CONFIG or starts heartbeats)

---

### Provisioning Messages

#### CONFIG (received by node)

Hub sends this to provision the node with tank assignment:

```cpp
struct ConfigMessage {
    MessageHeader header;
    uint8_t targetTankId;
    char targetNodeName[32];
};
```

**Node Response:**

1. Saves configuration to `/node_config.txt`
2. Sends STATUS acknowledgment
3. Restarts to apply configuration

---

#### UNMAP (received by node)

Hub sends this to remove tank assignment:

```cpp
struct UnmapMessage {
    MessageHeader header;
    uint8_t reason;
};
```

**Node Response:**

1. Resets tankId to 0
2. Deletes `/node_config.txt`
3. Turns off all lights (fail-safe)
4. Restarts to enter discovery mode

---

### Heartbeat Messages

#### HEARTBEAT (sent by node)

Periodic alive signal sent every 30 seconds with time sync validation:

```cpp
struct HeartbeatMessage {
    MessageHeader header;      // type: HEARTBEAT, tankId: assigned
    uint8_t health;            // 0-100 (always 100)
    uint16_t uptimeMinutes;    // Node uptime in minutes
    uint32_t nodeUnixTime;     // Node's current Unix timestamp
};
```

**Time Drift Detection:**

- Hub compares nodeUnixTime with its own clock
- If drift > 60 seconds, hub sends CONFIG to resync time
- Node recalculates time: `nodeUnixTime = lastSyncTime + (millis() - syncMillis) / 1000`

---

## Configuration File

Node configuration is stored in LittleFS at `/node_config.txt`:

```ini
# Lighting Node Configuration
TANK_ID=1
NODE_NAME=MainTankLight
FIRMWARE_VERSION=1
ESPNOW_CHANNEL=6
DEBUG_SERIAL=true
DEBUG_ESPNOW=true
DEBUG_HARDWARE=false
ANNOUNCE_INTERVAL_MS=5000
HEARTBEAT_INTERVAL_MS=30000
CONNECTION_TIMEOUT_MS=90000
```

---

## Fail-Safe Behavior

**On communication timeout (90 seconds):**

- Hold last state (lights remain at current levels)
- Continue sending heartbeats

**On UNMAP command:**

- Turn off all channels (safe state)
- Delete configuration
- Restart to discovery mode

**On hub disconnect:**

- No automatic action (lights maintain state)
- Hub must send explicit commands

---

## Debug Output

When `DEBUG_ESPNOW=true`, the node prints detailed command processing info:

```
+========================================================+
| [CMD] COMMAND received from 9C:13:9E:AC:59:C0
| Tank ID: 1
| Command ID: 42
| Command Type: 1 (All Channels ON)
| Data: [01 C8 96 64]
| [OK] All channels ON: W=200 B=150 R=100
+========================================================+
```

---

## Usage Examples

### Example 1: Single Channel Mode (Only channel 2)

```cpp
// Turn off channel 1 and channel 3
CommandMessage cmd;
cmd.header.type = MessageType::COMMAND;
cmd.header.tankId = 1;
cmd.header.nodeType = NodeType::LIGHT;
cmd.commandId = 1;

// Channel 1 OFF
cmd.commandData[0] = 10;
// ... send and wait for STATUS ...

// Channel 3 OFF  
cmd.commandData[0] = 30;
// ... send and wait for STATUS ...

// Channel 2 ON
cmd.commandData[0] = 21;
```

**Result:** Only channel 2 active

### Example 2: All Lights OFF

```cpp
CommandMessage cmd;
cmd.header.type = MessageType::COMMAND;
cmd.header.tankId = 1;
cmd.header.nodeType = NodeType::LIGHT;
cmd.commandId = 2;
cmd.commandData[0] = 0;  // All OFF
```

**Result:** All channels turned off (night mode)

### Example 3: All Lights ON

```cpp
CommandMessage cmd;
cmd.header.type = MessageType::COMMAND;
cmd.header.tankId = 1;
cmd.header.nodeType = NodeType::LIGHT;
cmd.commandId = 3;
cmd.commandData[0] = 1;  // All ON
```

**Result:** All channels turned on (full daylight)

### Example 4: Custom Mix (Channel 1 + Channel 2, No Channel 3)

```cpp
// Channel 1 ON
cmd.commandData[0] = 11;
// ... send and wait for STATUS ...

// Channel 2 ON
cmd.commandData[0] = 21;
// ... send and wait for STATUS ...

// Channel 3 OFF
cmd.commandData[0] = 30;
```

**Result:** Channel 1 and 2 active, channel 3 off

---

## Notes

- Channels are simple ON/OFF control (digital HIGH/LOW)
- No PWM dimming in firmware v1.0
- Commands are processed immediately upon receipt
- Each channel is independent and can be controlled separately
- Node does not store scheduled changes (hub controls scheduling)
- For gradual lighting transitions, hub must send multiple timed commands

---

**End of Command Reference**
