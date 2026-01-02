# Protocol Updates - Command & Status Enhancements

## Overview

The protocol messages have been enhanced to support:
1. **Multi-part commands** - For commands larger than 32 bytes
2. **Command acknowledgment** - Nodes respond with STATUS messages
3. **Command sequencing** - Track order of sub-messages

## Updated Message Structures

### CommandMessage
```cpp
struct CommandMessage {
    MessageHeader header;
    uint8_t commandId;        // Command type (1-255)
    uint8_t commandSeqID;     // Sequence ID for multi-part commands
    bool finalCommand;        // true = last message in sequence
    uint8_t commandData[32];  // Command payload
} __attribute__((packed));
```

### StatusMessage
```cpp
struct StatusMessage {
    MessageHeader header;
    uint8_t commandId;        // Echo of command being acknowledged (0 = unsolicited)
    uint8_t statusCode;       // 0 = success, 1+ = error codes
    uint8_t statusData[32];   // Status payload
} __attribute__((packed));
```

## Usage Patterns

### Single Command (Simple)

**Hub → Node:**
```cpp
CommandMessage cmd = {};
cmd.commandId = 1;           // "Set white level"
cmd.commandSeqID = 0;        // Single message
cmd.finalCommand = true;     // No follow-up messages
cmd.commandData[0] = 255;    // White = 100%
```

**Node → Hub (auto-acknowledgment):**
```cpp
StatusMessage status = {};
status.commandId = 1;        // Acknowledging command 1
status.statusCode = 0;       // Success
status.statusData[0] = 1;    // Simple ACK
```

### Multi-Part Command (Advanced)

When a command needs more than 32 bytes of data:

**Hub → Node (Part 1):**
```cpp
CommandMessage cmd1 = {};
cmd1.commandId = 20;         // "Load schedule"
cmd1.commandSeqID = 0;       // First part
cmd1.finalCommand = false;   // More parts coming
// ... fill commandData[32] ...
```

**Hub → Node (Part 2):**
```cpp
CommandMessage cmd2 = {};
cmd2.commandId = 20;         // Same command
cmd2.commandSeqID = 1;       // Second part
cmd2.finalCommand = false;   // More parts coming
// ... fill commandData[32] ...
```

**Hub → Node (Final):**
```cpp
CommandMessage cmd3 = {};
cmd3.commandId = 20;         // Same command
cmd3.commandSeqID = 2;       // Third part
cmd3.finalCommand = true;    // This is the last one
// ... fill commandData[32] ...
```

**Node processes all parts, then responds:**
```cpp
StatusMessage status = {};
status.commandId = 20;       // Acknowledging command 20
status.statusCode = 0;       // Success (all parts received)
```

### Unsolicited Status (Sensor Data)

When node sends data without being asked:

**Node → Hub:**
```cpp
StatusMessage status = {};
status.commandId = 0;        // 0 = unsolicited (not responding to command)
status.statusCode = 0;       // 0 = normal status
status.statusData[0] = pH_int;
status.statusData[1] = pH_frac;
status.statusData[2] = tds_low;
status.statusData[3] = tds_high;
// ... sensor readings ...
```

## Node Implementation

All nodes automatically send acknowledgment after handling commands:

```cpp
void onDataReceived(...) {
    // ... handle message ...
    
    case MessageType::COMMAND: {
        CommandMessage* msg = (CommandMessage*)data;
        
        // Log command details
        Serial.printf("  Command ID: %d, SeqID: %d, Final: %s\n",
                     msg->commandId, msg->commandSeqID, 
                     msg->finalCommand ? "YES" : "NO");
        
        // Process command
        handleCommand(msg);
        
        // Auto-acknowledge
        uint8_t ackData[1] = {1};
        sendStatus(msg->commandId, 0, ackData, 1);
        break;
    }
}
```

## Status Codes Convention

| Code | Meaning                        |
|------|--------------------------------|
| 0    | Success / Normal               |
| 1    | Generic error                  |
| 2    | Invalid parameter              |
| 3    | Hardware failure               |
| 4    | Timeout                        |
| 5    | Not supported                  |
| 6    | Busy / In progress             |
| 10+  | Node-specific error codes      |

## Example: Water Quality Sensor

### Periodic Sensor Reading (Unsolicited)

```cpp
void sendSensorData() {
    uint8_t sensorDataPayload[32] = {0};
    
    // Pack data
    sensorDataPayload[0] = (uint8_t)sensorData.pH;
    sensorDataPayload[1] = (uint8_t)((sensorData.pH - (int)sensorData.pH) * 100);
    sensorDataPayload[2] = sensorData.tds & 0xFF;
    sensorDataPayload[3] = (sensorData.tds >> 8) & 0xFF;
    sensorDataPayload[4] = (uint8_t)sensorData.temperature;
    sensorDataPayload[5] = (uint8_t)((sensorData.temperature - (int)sensorData.temperature) * 100);
    
    // Send unsolicited status (commandId=0)
    sendStatus(0, 0, sensorDataPayload, 6);
}
```

### On-Demand Reading (Hub Request)

**Hub sends:**
```cpp
CommandMessage cmd = {};
cmd.commandId = 1;  // "Request immediate reading"
cmd.commandSeqID = 0;
cmd.finalCommand = true;
```

**Node responds:**
```cpp
void handleCommand(const CommandMessage* msg) {
    if (msg->commandId == 1) {  // Immediate reading request
        readSensors();
        sendSensorData();  // Will use commandId=0 (unsolicited format)
    }
}
```

## Example: Multi-Channel Lighting

### Simple Command
```cpp
// Hub: Set all channels at once
CommandMessage cmd = {};
cmd.commandId = 100;  // "Set all channels"
cmd.commandData[0] = 255;  // White
cmd.commandData[1] = 128;  // Blue
cmd.commandData[2] = 64;   // Red
```

### Complex Schedule (Multi-Part)
```cpp
// Hub: Load 24-hour lighting schedule
// Each hour needs: white, blue, red (3 bytes)
// Total: 24 hours × 3 bytes = 72 bytes → needs 3 messages

// Part 1: Hours 0-10 (33 bytes, fits in 32+header)
CommandMessage cmd1 = {};
cmd1.commandId = 101;  // "Load schedule"
cmd1.commandSeqID = 0;
cmd1.finalCommand = false;
// ... pack hours 0-10 ...

// Part 2: Hours 11-21
CommandMessage cmd2 = {};
cmd2.commandId = 101;
cmd2.commandSeqID = 1;
cmd2.finalCommand = false;
// ... pack hours 11-21 ...

// Part 3: Hours 22-23
CommandMessage cmd3 = {};
cmd3.commandId = 101;
cmd3.commandSeqID = 2;
cmd3.finalCommand = true;
// ... pack hours 22-23 ...
```

## Hub Tracking

Hub should track pending multi-part commands:

```cpp
struct PendingCommand {
    uint8_t commandId;
    uint8_t expectedParts;
    uint8_t receivedParts;
    uint8_t nodeId;
    uint32_t timestamp;
};
```

## Benefits

1. **Acknowledgment** - Hub knows command was received and executed
2. **Error Handling** - Nodes can report failures with status codes
3. **Large Commands** - No 32-byte limit for complex operations
4. **Bidirectional** - Nodes can report data proactively
5. **Traceable** - commandId links requests to responses

## Migration Notes

**Old code (pre-update):**
```cpp
// Node just handled command silently
handleCommand(msg);
```

**New code (post-update):**
```cpp
// Node handles command AND acknowledges
handleCommand(msg);
sendStatus(msg->commandId, 0, ackData, 1);  // Auto-added in node_base.cpp
```

All existing nodes have been updated automatically via `node_base.cpp`.
