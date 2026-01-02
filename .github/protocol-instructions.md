# Protocol Instructions – ESP-NOW Communication

**Comprehensive guide to the ESP-NOW communication protocol used in the Aquarium Management System.**

---

## 📋 Table of Contents

1. [Protocol Overview](#protocol-overview)
2. [ESP-NOW Configuration](#esp-now-configuration)
3. [Discovery Model](#discovery-model)
4. [Message Types](#message-types)
5. [Node Types](#node-types)
6. [Message Structures](#message-structures)
7. [Multi-Part Command Protocol](#multi-part-command-protocol)
8. [Auto-Acknowledgment System](#auto-acknowledgment-system)
9. [Message Size Limits](#message-size-limits)
10. [Protocol Implementation Rules](#protocol-implementation-rules)
11. [Protocol Extensions](#protocol-extensions)

---

## Protocol Overview

### Core Communication Rules

- **Protocol**: ESP-NOW only
- **No MQTT**: Direct device-to-device communication
- **No TCP/IP**: ESP-NOW operates below IP layer
- **No Router Required**: Nodes communicate directly with hub
- **Message Format**: C/C++ structs (NOT JSON over the air)
- **MAC-Based Addressing**: Dynamic peer registration

### Why ESP-NOW?

- Low latency (<10ms typical)
- No Wi-Fi infrastructure needed
- Encrypted peer-to-peer communication
- Works even if Wi-Fi AP connection fails
- Low power consumption

---

## ESP-NOW Configuration

### Fixed Wi-Fi Channel

```cpp
#define ESPNOW_CHANNEL 6
```

**Critical Rules:**
- All devices (hub and nodes) must use Channel 6
- Channel must match for ESP-NOW to work
- Set before initializing ESP-NOW
- Do NOT use dynamic channel selection

### Initialization Sequence

```cpp
// 1. Set Wi-Fi mode
WiFi.mode(WIFI_STA);

// 2. Set channel
wifi_set_channel(ESPNOW_CHANNEL);

// 3. Initialize ESP-NOW
esp_now_init();

// 4. Register callbacks
esp_now_register_recv_cb(onDataReceive);
esp_now_register_send_cb(onDataSent);
```

---

## Discovery Model

### Boot-Up Sequence

```
1. Node boots up
   ↓
2. Node sends BROADCAST ANNOUNCE
   ↓
3. Hub receives ANNOUNCE (listens permanently)
   ↓
4. Hub dynamically registers node using sender MAC
   ↓
5. Hub sends UNICAST ACK to node
   ↓
6. Node receives ACK
   ↓
7. Node switches to unicast mode
   ↓
8. Normal operation begins
```

### Broadcast vs Unicast

**Broadcast (Discovery Phase)**
```cpp
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
esp_now_send(broadcastAddress, (uint8_t*)&msg, sizeof(msg));
```

**Unicast (Normal Operation)**
```cpp
// Hub stores node MAC from ANNOUNCE
uint8_t nodeMac[6];  // Extracted from sender
esp_now_send(nodeMac, (uint8_t*)&msg, sizeof(msg));
```

### Key Points

- ✅ Nodes send broadcast ANNOUNCE on boot
- ✅ Hub listens permanently for broadcasts
- ✅ Hub registers peers dynamically (no hardcoded MACs)
- ✅ Nodes switch to unicast after receiving ACK
- ❌ Do NOT hardcode MAC addresses
- ❌ Do NOT assume static network topology

---

## Message Types

### Enum Definition

```cpp
enum class MessageType : uint8_t {
    ANNOUNCE = 0x01,    // Node announces itself to hub
    ACK = 0x02,         // Hub acknowledges node
    COMMAND = 0x03,     // Hub sends command to node
    STATUS = 0x04,      // Node sends status to hub
    HEARTBEAT = 0x05    // Periodic alive signal
};
```

### Message Flow Diagram

```
NODE                          HUB
 |                             |
 |---ANNOUNCE (broadcast)----->|
 |                             | (registers node)
 |<-----ACK (unicast)----------|
 |                             |
 |<----COMMAND (unicast)-------|
 | (processes command)         |
 |-----STATUS (unicast)------->| (auto-ack)
 |                             |
 |----HEARTBEAT (periodic)---->|
 |                             | (monitors health)
```

### Usage Guidelines

| Message Type | Direction | Purpose | Frequency |
|--------------|-----------|---------|-----------|
| ANNOUNCE | Node → Hub | Discovery | On boot only |
| ACK | Hub → Node | Confirm registration | Once per node |
| COMMAND | Hub → Node | Control actuators | As needed |
| STATUS | Node → Hub | Report state | After commands |
| HEARTBEAT | Node → Hub | Alive signal | Every 30s |

---

## Node Types

### Enum Definition

```cpp
enum class NodeType : uint8_t {
    UNKNOWN = 0x00,
    HUB = 0x01,
    LIGHT = 0x02,
    CO2 = 0x03,
    DOSER = 0x04,
    SENSOR = 0x05,      // Water quality sensors
    HEATER = 0x06,
    FILTER = 0x07,
    FISH_FEEDER = 0x08,
    REPEATER = 0x09     // ESP-NOW range extender
};
```

### Node Type Capabilities

| Node Type | Primary Function | Actuator/Sensor | Fail-Safe Mode |
|-----------|------------------|-----------------|----------------|
| HUB | System orchestration | N/A | N/A |
| LIGHT | 3-channel PWM LED | Actuator | Hold last state |
| CO2 | Solenoid valve | Actuator | **OFF** (critical) |
| DOSER | Peristaltic pumps | Actuator | **OFF** |
| SENSOR | pH/TDS/Temp monitoring | Sensor | Continue reading |
| HEATER | Relay + temp sensor | Actuator | **OFF** (critical) |
| FILTER | Pump control | Actuator | **OFF** |
| FISH_FEEDER | Servo-based feeding | Actuator | Do nothing |
| REPEATER | Range extension | Passive relay | Continue forwarding |

---

## Message Structures

All messages defined in `include/protocol/messages.h`.

### Base Header

**Every message starts with this header:**

```cpp
struct MessageHeader {
    MessageType type;       // Message type enum
    uint8_t tankId;         // Multi-tank support (1-255)
    NodeType nodeType;      // Sender's node type
    uint32_t timestamp;     // millis() when sent
    uint8_t sequenceNum;    // For tracking message order
} __attribute__((packed));
```

**Field Purposes:**
- `type`: Identifies message purpose (ANNOUNCE, COMMAND, etc.)
- `tankId`: Supports multiple aquariums (CRITICAL for multi-tank)
- `nodeType`: Identifies sender device type
- `timestamp`: For latency measurement and ordering
- `sequenceNum`: Increments per message, wraps at 255

### ANNOUNCE Message

**Sent by nodes during discovery:**

```cpp
struct AnnounceMessage {
    MessageHeader header;
    char nodeName[MAX_NODE_NAME_LEN];  // Human-readable name
    uint8_t firmwareVersion;           // Firmware version number
    uint8_t capabilities;              // Feature flags (future use)
} __attribute__((packed));
```

**Usage Example:**
```cpp
AnnounceMessage msg;
msg.header.type = MessageType::ANNOUNCE;
msg.header.tankId = TANK_ID;
msg.header.nodeType = NodeType::LIGHT;
msg.header.timestamp = millis();
msg.header.sequenceNum = seqNum++;
strcpy(msg.nodeName, "Main Tank Light");
msg.firmwareVersion = 1;
msg.capabilities = 0x00;

esp_now_send(broadcastAddress, (uint8_t*)&msg, sizeof(msg));
```

### ACK Message

**Sent by hub to confirm node registration:**

```cpp
struct AckMessage {
    MessageHeader header;
    uint8_t ackCode;       // Success/error code
    uint8_t reserved[8];   // Future use
} __attribute__((packed));
```

### COMMAND Message

**Sent by hub to control nodes:**

```cpp
struct CommandMessage {
    MessageHeader header;
    uint8_t commandId;        // Unique command identifier
    uint8_t commandSeqID;     // Sequence for multi-part commands
    bool finalCommand;        // True if last in sequence
    uint8_t commandData[32];  // Generic command payload
} __attribute__((packed));
```

**Command Data Format:**
```
Byte 0: Command type (device-specific)
Byte 1-31: Command parameters
```

**Example - Lighting Control:**
```cpp
commandData[0] = CMD_SET_LEVEL;  // Command type
commandData[1] = whiteLevel;     // 0-255
commandData[2] = blueLevel;      // 0-255
commandData[3] = redLevel;       // 0-255
```

### STATUS Message

**Sent by nodes to report state:**

```cpp
struct StatusMessage {
    MessageHeader header;
    uint8_t commandId;        // CommandID for Hub acknowledgment
    uint8_t statusCode;       // Status/error code
    uint8_t statusData[32];   // Generic status payload
} __attribute__((packed));
```

**Status Codes:**
```cpp
#define STATUS_OK           0x00
#define STATUS_ERROR        0xFF
#define STATUS_INVALID_CMD  0x01
#define STATUS_BUSY         0x02
```

**Auto-Acknowledgment:**
- Every COMMAND must be followed by a STATUS
- STATUS includes original `commandId` for tracking
- Hub uses this to confirm command execution

### HEARTBEAT Message

**Sent periodically by nodes:**

```cpp
struct HeartbeatMessage {
    MessageHeader header;
    uint8_t health;           // 0-100 health indicator
    uint16_t uptimeMinutes;   // Node uptime
} __attribute__((packed));
```

**Heartbeat Timing:**
- Nodes send every 30 seconds
- Hub monitors with 60-second timeout
- Missing heartbeat triggers fail-safe mode

---

## Multi-Part Command Protocol

### Purpose

ESP-NOW has a 250-byte limit. For larger commands (schedules, configurations), use multi-part protocol.

### Protocol Fields

```cpp
struct CommandMessage {
    uint8_t commandSeqID;     // Sequence number (0, 1, 2, ...)
    bool finalCommand;        // True for last part
    uint8_t commandData[32];  // Partial payload
};
```

### Example - 3-Part Command

```cpp
// Part 1 of 3
CommandMessage part1;
part1.commandId = 42;
part1.commandSeqID = 0;
part1.finalCommand = false;
memcpy(part1.commandData, data, 32);

// Part 2 of 3
CommandMessage part2;
part2.commandId = 42;
part2.commandSeqID = 1;
part2.finalCommand = false;
memcpy(part2.commandData, data + 32, 32);

// Part 3 of 3 (final)
CommandMessage part3;
part3.commandId = 42;
part3.commandSeqID = 2;
part3.finalCommand = true;  // Last part
memcpy(part3.commandData, data + 64, 32);
```

### Node-Side Assembly

```cpp
uint8_t assemblyBuffer[96];  // For 3 parts
uint8_t partsReceived = 0;

void handleCommand(const CommandMessage& cmd) {
    // Copy data to buffer
    memcpy(assemblyBuffer + (cmd.commandSeqID * 32), 
           cmd.commandData, 32);
    
    if (cmd.finalCommand) {
        // All parts received, execute command
        processFullCommand(assemblyBuffer);
        partsReceived = 0;
    }
}
```

---

## Auto-Acknowledgment System

### Design Pattern

**Hub sends command:**
```cpp
CommandMessage cmd;
cmd.commandId = nextCommandId++;  // Unique ID
// ... fill command data ...
esp_now_send(nodeMac, (uint8_t*)&cmd, sizeof(cmd));
```

**Node processes and acknowledges:**
```cpp
void handleCommand(const CommandMessage& cmd) {
    // Execute command
    setLightLevel(cmd.commandData[1]);
    
    // Send acknowledgment
    sendStatus(cmd.commandId, STATUS_OK, nullptr, 0);
}
```

**Hub receives acknowledgment:**
```cpp
void onStatusReceived(const StatusMessage& status) {
    if (status.commandId == lastCommandId) {
        // Command confirmed
        markCommandComplete();
    }
}
```

### Benefits

- ✅ Hub knows if command was received
- ✅ Can retry on timeout
- ✅ Tracks command success/failure
- ✅ Prevents duplicate execution

---

## Message Size Limits

### ESP-NOW Constraint

**Maximum message size: 250 bytes**

```cpp
// Compile-time size check
static_assert(sizeof(CommandMessage) <= 250, 
              "CommandMessage exceeds ESP-NOW limit");
```

### Size Guidelines

| Message Type | Typical Size | Max Payload |
|--------------|--------------|-------------|
| ANNOUNCE | ~50 bytes | N/A |
| ACK | ~20 bytes | 8 bytes |
| COMMAND | ~50 bytes | 32 bytes |
| STATUS | ~50 bytes | 32 bytes |
| HEARTBEAT | ~15 bytes | N/A |

### Design Considerations

- Keep headers minimal
- Use fixed-size arrays (not dynamic)
- Use `__attribute__((packed))` to prevent padding
- Validate size with `static_assert`
- Use multi-part protocol for large data

---

## Protocol Implementation Rules

### 1. Message Packing

**Always use packed structs:**
```cpp
struct MyMessage {
    MessageHeader header;
    uint8_t data[10];
} __attribute__((packed));
```

**Why?** Prevents compiler from adding padding bytes.

### 2. Byte Order

**ESP32 and ESP8266 are both little-endian.**

- No byte swapping needed for communication
- If adding external devices, consider endianness

### 3. Message Validation

**Always validate received messages:**
```cpp
void onDataReceive(const uint8_t* mac, const uint8_t* data, int len) {
    // Check minimum size
    if (len < sizeof(MessageHeader)) return;
    
    // Cast to header
    MessageHeader* header = (MessageHeader*)data;
    
    // Validate type
    if (header->type > MessageType::HEARTBEAT) return;
    
    // Validate tank ID
    if (header->tankId != myTankId) return;
    
    // Process message
    switch (header->type) {
        case MessageType::COMMAND:
            handleCommand(*(CommandMessage*)data);
            break;
        // ...
    }
}
```

### 4. Sequence Number Management

```cpp
uint8_t sequenceNum = 0;

void sendMessage() {
    msg.header.sequenceNum = sequenceNum++;
    // sequenceNum wraps automatically at 255
}
```

### 5. Timestamp Usage

```cpp
msg.header.timestamp = millis();

// On receive
uint32_t latency = millis() - msg.header.timestamp;
```

---

## Protocol Extensions

### Adding New Message Type

**Step 1: Update enum in `messages.h`**
```cpp
enum class MessageType : uint8_t {
    ANNOUNCE = 0x01,
    ACK = 0x02,
    COMMAND = 0x03,
    STATUS = 0x04,
    HEARTBEAT = 0x05,
    MY_NEW_TYPE = 0x06  // Add new type
};
```

**Step 2: Define struct**
```cpp
struct MyNewMessage {
    MessageHeader header;
    uint8_t myData[20];
} __attribute__((packed));

static_assert(sizeof(MyNewMessage) <= 250, "Size check");
```

**Step 3: Update hub and nodes**
```cpp
void onDataReceive(const uint8_t* mac, const uint8_t* data, int len) {
    MessageHeader* header = (MessageHeader*)data;
    
    switch (header->type) {
        case MessageType::MY_NEW_TYPE:
            handleNewMessage(*(MyNewMessage*)data);
            break;
    }
}
```

**Step 4: Document and version**
- Update `PROTOCOL_UPDATES.md`
- Increment firmware version
- Test on all node types

### Adding New Command Type

**Step 1: Define command enum**
```cpp
enum class CommandType : uint8_t {
    CMD_SET_LEVEL = 0x01,
    CMD_SET_SCHEDULE = 0x02,
    CMD_MY_NEW_CMD = 0x03  // Add new command
};
```

**Step 2: Handle in node**
```cpp
void handleCommand(const CommandMessage& cmd) {
    uint8_t cmdType = cmd.commandData[0];
    
    switch(cmdType) {
        case CMD_MY_NEW_CMD:
            // Process new command
            uint8_t param1 = cmd.commandData[1];
            uint8_t param2 = cmd.commandData[2];
            doSomething(param1, param2);
            sendStatus(cmd.commandId, STATUS_OK, nullptr, 0);
            break;
    }
}
```

---

## Common Protocol Issues

### Issue 1: Messages Not Received

**Symptoms:**
- Node sends ANNOUNCE but hub doesn't respond
- Commands sent but nodes don't react

**Checklist:**
- [ ] All devices on Channel 6?
- [ ] ESP-NOW initialized before sending?
- [ ] Callbacks registered?
- [ ] Correct MAC address?
- [ ] Message size <= 250 bytes?

### Issue 2: Peer Not Found

**Error:** `ESP_NOW_SEND_FAIL`

**Solutions:**
```cpp
// Add peer before sending
esp_now_peer_info_t peerInfo;
memcpy(peerInfo.peer_addr, nodeMac, 6);
peerInfo.channel = ESPNOW_CHANNEL;
peerInfo.encrypt = false;
esp_now_add_peer(&peerInfo);
```

### Issue 3: Message Corruption

**Symptoms:**
- Random data in received messages
- Struct fields have wrong values

**Solutions:**
- Use `__attribute__((packed))`
- Validate message size
- Check for buffer overruns
- Add CRC if needed (future enhancement)

---

## Protocol Best Practices

1. **Always validate messages**
   - Check size, type, tank ID
   - Ignore malformed messages

2. **Use timeouts**
   - Don't wait forever for ACK
   - Implement retry logic

3. **Log protocol events**
   - Log all sends/receives during development
   - Include MAC addresses and message types

4. **Test edge cases**
   - Rapid message bursts
   - Messages during ESP-NOW init
   - Multiple nodes announcing simultaneously

5. **Version your protocol**
   - Include version in ANNOUNCE
   - Hub can reject incompatible versions

6. **Keep it simple**
   - Binary protocols are fast but hard to debug
   - Add debug modes that print message contents

---

## Protocol Testing Commands

### Test Discovery
```cpp
// Node side
sendAnnounce();
delay(1000);
// Check for ACK

// Hub side
// Monitor serial for ANNOUNCE messages
```

### Test Commands
```cpp
// Hub side
sendCommand(nodeMac, CMD_SET_LEVEL, params);

// Node side
// Verify handleCommand() is called
// Verify STATUS is sent back
```

### Test Heartbeat
```cpp
// Node side
sendHeartbeat();

// Hub side
unsigned long lastHeartbeat = millis();
// Check if (millis() - lastHeartbeat > 60000)
```

---

## Protocol Debugging

### Enable Verbose Logging

```cpp
void onDataReceive(const uint8_t* mac, const uint8_t* data, int len) {
    Serial.printf("ESP-NOW RX: %d bytes from %02X:%02X:%02X:%02X:%02X:%02X\n",
                  len, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    MessageHeader* header = (MessageHeader*)data;
    Serial.printf("  Type: 0x%02X, Tank: %d, Node: 0x%02X\n",
                  header->type, header->tankId, header->nodeType);
}
```

### Monitor ESP-NOW Status

```cpp
void onDataSent(const uint8_t* mac, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) {
        Serial.println("ESP-NOW: Send success");
    } else {
        Serial.println("ESP-NOW: Send failed");
    }
}
```

---

**END OF PROTOCOL INSTRUCTIONS**

*Last Updated: December 29, 2025*  
*Project: Aquarium Management System*  
*Repository: bghosh412/aquarium-management-system*  
*Protocol Version: 1.0*
