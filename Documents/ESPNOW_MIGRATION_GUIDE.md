# ESPNowManager Integration Guide

**How to migrate existing code to use the new centralized ESPNowManager library**

---

## Overview

The new **ESPNowManager** library centralizes all ESP-NOW communication, addressing critical gaps:
- ✅ Message fragmentation (hub-side)
- ✅ Message reassembly (node-side)
- ✅ Duplicate protection
- ✅ RX queue (ISR-safe)
- ✅ Retry mechanism
- ✅ Offline checks

This guide shows how to integrate it into existing hub and node code.

---

## Part 1: Hub Integration (main.cpp)

### Current Code Pattern

```cpp
// OLD: Direct ESP-NOW usage
#include <esp_now.h>

void onESPNowRecv(const uint8_t *mac, const uint8_t *data, int len) {
    // Queue message
    ESPNowEvent event;
    memcpy(event.mac, mac, 6);
    memcpy(event.data, data, len);
    event.len = len;
    xQueueSendFromISR(espnowQueue, &event, ...);
}

void setupESPNow() {
    esp_now_init();
    esp_now_register_recv_cb(onESPNowRecv);
}

void loop() {
    ESPNowEvent event;
    while (xQueueReceive(espnowQueue, &event, 0) == pdTRUE) {
        processESPNowMessage(event);
    }
}
```

### New Code Pattern

```cpp
// NEW: ESPNowManager usage
#include "ESPNowManager.h"

// Register callbacks once
void onAnnounceReceived(const uint8_t* mac, const AnnounceMessage& announce) {
    AquariumManager::getInstance().handleAnnounce(mac, announce);
}

void onHeartbeatReceived(const uint8_t* mac, const HeartbeatMessage& heartbeat) {
    AquariumManager::getInstance().handleHeartbeat(mac, heartbeat);
}

void onStatusReceived(const uint8_t* mac, const StatusMessage& status) {
    AquariumManager::getInstance().handleStatus(mac, status);
}

void setup() {
    // ... existing setup code ...
    
    // Initialize ESPNowManager
    ESPNowManager::getInstance().begin(config.espnowChannel, true);  // isHub=true
    
    // Register callbacks
    ESPNowManager::getInstance().onAnnounceReceived(onAnnounceReceived);
    ESPNowManager::getInstance().onHeartbeatReceived(onHeartbeatReceived);
    ESPNowManager::getInstance().onStatusReceived(onStatusReceived);
}

void loop() {
    // Process ESP-NOW messages (replaces manual queue handling)
    ESPNowManager::getInstance().processQueue();
    
    // Check for offline nodes every 60 seconds
    static unsigned long lastTimeoutCheck = 0;
    if (millis() - lastTimeoutCheck > 60000) {
        lastTimeoutCheck = millis();
        int offlineCount = ESPNowManager::getInstance().checkPeerTimeouts(90000);
        if (offlineCount > 0) {
            Serial.printf("⚠️  %d nodes timed out\n", offlineCount);
        }
    }
    
    // ... rest of loop ...
}
```

### Changes to Device.cpp

```cpp
// OLD: Device::sendCommand()
bool Device::sendCommand(const uint8_t* commandData, size_t length) {
    CommandMessage cmd;
    // ... fill cmd ...
    
    bool success = _sendESPNowMessage((uint8_t*)&cmd, sizeof(cmd));
    return success;
}

// NEW: Device::sendCommand() with offline check and retry
bool Device::sendCommand(const uint8_t* commandData, size_t length) {
    CommandMessage cmd;
    cmd.header.type = MessageType::COMMAND;
    cmd.header.tankId = _tankId;
    cmd.header.nodeType = NodeType::HUB;
    cmd.header.timestamp = millis();
    cmd.header.sequenceNum = 0;
    
    cmd.commandId = random(1, 255);
    cmd.commandSeqID = 0;
    cmd.finalCommand = true;
    
    size_t copyLen = (length > 32) ? 32 : length;
    memcpy(cmd.commandData, commandData, copyLen);
    
    // Use ESPNowManager with online check
    bool success = ESPNowManager::getInstance().send(
        _mac, 
        (uint8_t*)&cmd, 
        sizeof(cmd), 
        true  // checkOnline = true
    );
    
    if (success) {
        _lastCommandSent = millis();
        _commandsSent++;
        _messagesSent++;
    } else {
        _errorCount++;
        Serial.printf("❌ Failed to send command to %s (offline)\n", _name.c_str());
    }
    
    return success;
}

// NEW: For large messages (schedules, configuration)
bool Device::sendLargeCommand(uint8_t commandId, const uint8_t* data, size_t length) {
    // Automatic fragmentation
    bool success = ESPNowManager::getInstance().sendFragmented(
        _mac,
        commandId,
        data,
        length,
        true  // checkOnline = true
    );
    
    if (success) {
        _lastCommandSent = millis();
        _commandsSent++;
    }
    
    return success;
}
```

### Changes to AquariumManager

```cpp
// Add to AquariumManager.cpp

void AquariumManager::handleAnnounce(const uint8_t* mac, const AnnounceMessage& announce) {
    // Check if device already registered
    Device* device = getDeviceByMac(mac);
    
    if (!device) {
        // Create new device
        device = createDeviceFromType(mac, announce.header.nodeType, announce.nodeName);
        addDevice(device);
        Serial.printf("✅ New device registered: %s\n", announce.nodeName);
    }
    
    // Add peer to ESP-NOW
    ESPNowManager::getInstance().addPeer(mac);
    
    // Send ACK
    AckMessage ack;
    ack.header.type = MessageType::ACK;
    ack.header.tankId = announce.header.tankId;
    ack.header.nodeType = NodeType::HUB;
    ack.header.timestamp = millis();
    ack.assignedNodeId = device->getId();
    ack.accepted = true;
    
    ESPNowManager::getInstance().send(mac, (uint8_t*)&ack, sizeof(ack));
}

void AquariumManager::handleHeartbeat(const uint8_t* mac, const HeartbeatMessage& heartbeat) {
    Device* device = getDeviceByMac(mac);
    if (device) {
        device->updateHeartbeat(heartbeat.health, heartbeat.uptimeMinutes);
    }
    
    // Update peer status in ESPNowManager
    ESPNowManager::getInstance().updatePeerHeartbeat(mac);
}

void AquariumManager::checkDeviceHealth() {
    // Let ESPNowManager handle timeouts
    int offlineCount = ESPNowManager::getInstance().checkPeerTimeouts(90000);
    
    // Update device status based on ESPNowManager
    for (Device* device : getAllDevices()) {
        bool online = ESPNowManager::getInstance().isPeerOnline(device->getMac());
        
        if (!online && device->getStatus() == Device::Status::ONLINE) {
            device->setStatus(Device::Status::OFFLINE);
            triggerFailSafe(device);
        }
    }
}
```

---

## Part 2: Node Integration (NodeBase)

### Current node_base.h

```cpp
// OLD: Manual ESP-NOW handling
extern NodeState currentState;
extern uint8_t hubMacAddress[6];
extern bool hubDiscovered;

void sendAnnounce();
void sendHeartbeat();
void sendStatus(...);
void nodeLoop();
```

### New node_base.h (Simplified)

```cpp
// NEW: Use ESPNowManager
#include "ESPNowManager.h"

extern NodeState currentState;
extern uint8_t hubMacAddress[6];
extern bool hubDiscovered;

// Required node-specific implementations (unchanged)
void setupHardware();
void enterFailSafeMode();
void handleCommand(const uint8_t* data, size_t len);  // Changed signature
void updateHardware();

// Simplified interface
void setupNode(const char* nodeName, NodeType nodeType, uint8_t tankId);
void nodeLoop();
```

### New node_base.cpp

```cpp
#include "node_base.h"

// Node configuration
static const char* g_nodeName = nullptr;
static NodeType g_nodeType = NodeType::UNKNOWN;
static uint8_t g_tankId = 0;

NodeState currentState = NodeState::INITIALIZING;
uint8_t hubMacAddress[6] = {0};
bool hubDiscovered = false;
uint32_t lastHeartbeatSent = 0;
uint32_t lastHeartbeatReceived = 0;

// Callbacks for ESPNowManager
void onCommandReceivedCallback(const uint8_t* mac, const uint8_t* data, size_t len) {
    // Update hub MAC if first time
    if (!hubDiscovered) {
        memcpy(hubMacAddress, mac, 6);
        hubDiscovered = true;
        ESPNowManager::getInstance().addPeer(hubMacAddress);
        Serial.println("✅ Hub discovered and added as peer");
    }
    
    lastHeartbeatReceived = millis();
    
    // Call node-specific handler
    handleCommand(data, len);
}

void onAckReceivedCallback(const uint8_t* mac, const AnnounceMessage& ack) {
    if (currentState == NodeState::WAITING_FOR_ACK) {
        Serial.println("✅ ACK received from hub");
        
        if (!hubDiscovered) {
            memcpy(hubMacAddress, mac, 6);
            hubDiscovered = true;
            ESPNowManager::getInstance().addPeer(hubMacAddress);
        }
        
        currentState = NodeState::CONNECTED;
        lastHeartbeatReceived = millis();
    }
}

void setupNode(const char* nodeName, NodeType nodeType, uint8_t tankId) {
    g_nodeName = nodeName;
    g_nodeType = nodeType;
    g_tankId = tankId;
    
    Serial.printf("🚀 Node: %s (Type: %d, Tank: %d)\n", nodeName, (int)nodeType, tankId);
    
    // Initialize ESPNowManager
    ESPNowManager::getInstance().begin(ESPNOW_CHANNEL, false);  // isHub=false
    
    // Register callbacks
    ESPNowManager::getInstance().onCommandReceived(onCommandReceivedCallback);
    
    // Send ANNOUNCE
    AnnounceMessage announce = {};
    announce.header.type = MessageType::ANNOUNCE;
    announce.header.tankId = tankId;
    announce.header.nodeType = nodeType;
    announce.header.timestamp = millis();
    strncpy(announce.nodeName, nodeName, MAX_NODE_NAME_LEN);
    announce.firmwareVersion = 1;
    
    uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    ESPNowManager::getInstance().send(broadcastMac, (uint8_t*)&announce, sizeof(announce));
    
    currentState = NodeState::WAITING_FOR_ACK;
    Serial.println("📡 ANNOUNCE sent, waiting for ACK...");
}

void nodeLoop() {
    // Process ESP-NOW messages (handles reassembly automatically)
    ESPNowManager::getInstance().processQueue();
    
    uint32_t now = millis();
    
    switch (currentState) {
        case NodeState::WAITING_FOR_ACK:
            // Resend ANNOUNCE every 5 seconds
            if (now - lastHeartbeatSent > 5000) {
                AnnounceMessage announce = {};
                announce.header.type = MessageType::ANNOUNCE;
                announce.header.tankId = g_tankId;
                announce.header.nodeType = g_nodeType;
                announce.header.timestamp = millis();
                strncpy(announce.nodeName, g_nodeName, MAX_NODE_NAME_LEN);
                announce.firmwareVersion = 1;
                
                uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
                ESPNowManager::getInstance().send(broadcastMac, (uint8_t*)&announce, sizeof(announce));
                
                lastHeartbeatSent = now;
            }
            break;
            
        case NodeState::CONNECTED:
            // Send heartbeat every 30 seconds
            if (now - lastHeartbeatSent > 30000) {
                HeartbeatMessage heartbeat = {};
                heartbeat.header.type = MessageType::HEARTBEAT;
                heartbeat.header.tankId = g_tankId;
                heartbeat.header.nodeType = g_nodeType;
                heartbeat.header.timestamp = millis();
                heartbeat.health = 100;
                heartbeat.uptimeMinutes = millis() / 60000;
                
                ESPNowManager::getInstance().send(hubMacAddress, (uint8_t*)&heartbeat, sizeof(heartbeat));
                lastHeartbeatSent = now;
            }
            
            // Check connection timeout
            if (now - lastHeartbeatReceived > 90000) {
                Serial.println("⚠️ Connection timeout - entering fail-safe");
                enterFailSafeMode();
                currentState = NodeState::LOST_CONNECTION;
            }
            break;
            
        case NodeState::LOST_CONNECTION:
            // Try to reconnect
            if (now - lastHeartbeatSent > 5000) {
                hubDiscovered = false;
                currentState = NodeState::WAITING_FOR_ACK;
                // Will send ANNOUNCE on next iteration
            }
            break;
    }
}

// Helper to send status (acknowledgment)
void sendStatus(uint8_t commandId, uint8_t statusCode, const uint8_t* data, size_t dataLen) {
    if (!hubDiscovered) return;
    
    StatusMessage status = {};
    status.header.type = MessageType::STATUS;
    status.header.tankId = g_tankId;
    status.header.nodeType = g_nodeType;
    status.header.timestamp = millis();
    status.commandId = commandId;
    status.statusCode = statusCode;
    
    if (data && dataLen > 0) {
        size_t copyLen = (dataLen > 32) ? 32 : dataLen;
        memcpy(status.statusData, data, copyLen);
    }
    
    ESPNowManager::getInstance().send(hubMacAddress, (uint8_t*)&status, sizeof(status));
}
```

### Update Node main.cpp

```cpp
// Example: lighting node
#include "node_base.h"

// Node configuration
const char* NODE_NAME = "Main Tank Light";
const NodeType NODE_TYPE = NodeType::LIGHT;
const uint8_t TANK_ID = 1;

void setupHardware() {
    pinMode(PIN_WHITE, OUTPUT);
    pinMode(PIN_BLUE, OUTPUT);
    pinMode(PIN_RED, OUTPUT);
}

void enterFailSafeMode() {
    // Hold last state (safe for lights)
    Serial.println("🛡️ Fail-safe: Maintaining last state");
}

void handleCommand(const uint8_t* data, size_t len) {
    // Now receives reassembled data (can be > 32 bytes!)
    Serial.printf("📥 Command received: %d bytes\n", len);
    
    uint8_t cmdType = data[0];
    
    switch(cmdType) {
        case 0x01: // Set levels
            if (len >= 4) {
                uint8_t white = data[1];
                uint8_t blue = data[2];
                uint8_t red = data[3];
                
                analogWrite(PIN_WHITE, white);
                analogWrite(PIN_BLUE, blue);
                analogWrite(PIN_RED, red);
                
                // Send acknowledgment
                sendStatus(data[4], 0, nullptr, 0);  // Assuming command ID in data[4]
            }
            break;
    }
}

void updateHardware() {
    // State machine updates
}

void setup() {
    Serial.begin(115200);
    setupHardware();
    setupNode(NODE_NAME, NODE_TYPE, TANK_ID);
}

void loop() {
    nodeLoop();        // Process ESP-NOW (now with reassembly!)
    updateHardware();  // Update hardware state
}
```

---

## Part 3: Testing Migration

### Test 1: Basic Communication
```cpp
// Hub sends simple command
uint8_t cmd[] = {0x01, 0xFF, 0x80, 0x00};  // Set lights
ESPNowManager::getInstance().send(nodeMac, cmd, 4, true);

// Node receives and processes
void handleCommand(const uint8_t* data, size_t len) {
    Serial.printf("Received: %d bytes\n", len);  // Should print "4 bytes"
}
```

### Test 2: Large Message (Fragmentation)
```cpp
// Hub sends schedule (128 bytes)
uint8_t schedule[128];
ESPNowManager::getInstance().sendFragmented(nodeMac, 42, schedule, 128, true);

// Node receives complete message
void handleCommand(const uint8_t* data, size_t len) {
    Serial.printf("Received: %d bytes\n", len);  // Should print "128 bytes"
}
```

### Test 3: Offline Check
```cpp
// Mark node offline
ESPNowManager::getInstance().setPeerOnline(nodeMac, false);

// Try send (should fail gracefully)
bool sent = ESPNowManager::getInstance().send(nodeMac, cmd, 4, true);
Serial.printf("Send result: %s\n", sent ? "SUCCESS" : "FAILED");  // "FAILED"
```

### Test 4: Retry Mechanism
```cpp
// Send with retry
bool sent = ESPNowManager::getInstance().sendWithRetry(nodeMac, cmd, 4, 3);

// Check statistics
ESPNowManager::getInstance().printStatistics();
```

---

## Migration Checklist

### Hub Side
- [ ] Replace direct `esp_now_init()` with `ESPNowManager::getInstance().begin()`
- [ ] Replace manual queue creation with built-in queue
- [ ] Replace `onESPNowRecv` with callback registration
- [ ] Update `Device::sendCommand()` to use ESPNowManager
- [ ] Add `Device::sendLargeCommand()` for fragmented messages
- [ ] Remove manual peer tracking (use `isPeerOnline()`)
- [ ] Update `AquariumManager::handleAnnounce()` to use `addPeer()`
- [ ] Update timeout checks to use `checkPeerTimeouts()`

### Node Side
- [ ] Replace direct ESP-NOW calls with ESPNowManager
- [ ] Update `handleCommand()` signature to receive full data
- [ ] Remove manual reassembly code (now automatic)
- [ ] Simplify `nodeLoop()` (remove manual queue processing)
- [ ] Update `sendStatus()` to use ESPNowManager
- [ ] Test with large messages (> 32 bytes)

### Testing
- [ ] Test basic command send/receive
- [ ] Test fragmented messages (> 32 bytes)
- [ ] Test retry mechanism
- [ ] Test offline detection
- [ ] Test duplicate protection
- [ ] Verify statistics collection
- [ ] Test node reconnection after timeout

---

## Benefits After Migration

✅ **Fragmented Messages**: Send schedules, configuration (up to 512 bytes)  
✅ **Automatic Reassembly**: Nodes handle multi-part messages transparently  
✅ **Reliable Delivery**: Retry with exponential backoff  
✅ **Duplicate Protection**: No more double-execution of commands  
✅ **Offline Awareness**: Hub won't send to offline nodes  
✅ **ISR-Safe**: All callbacks queued, no blocking in ISR  
✅ **Diagnostics**: Built-in statistics and monitoring  

---

## Troubleshooting

### Issue: Commands not received after migration
- Ensure `processQueue()` called in main loop
- Check callbacks registered correctly
- Verify WiFi channel matches (usually 6)

### Issue: Fragmented messages fail
- Check `ESPNOW_MAX_MESSAGE_SIZE` limit (512 bytes)
- Verify node's `handleCommand()` can receive large buffers
- Add delay between fragments if needed

### Issue: Duplicate protection too aggressive
- Verify sequence numbers incrementing correctly
- Check statistics: `duplicatesIgnored` counter

### Issue: Performance degraded
- Monitor statistics: check for excessive retries
- Reduce message frequency if queue overflows
- Increase `ESPNOW_RX_QUEUE_SIZE` if needed

---

**Version**: 1.0  
**Last Updated**: January 2, 2026  
**Part of**: Aquarium Management System
