# ESPNowManager Library

**Centralized ESP-NOW communication manager with production-ready features**

## Features

### ✅ Message Fragmentation (Hub-Side)
- Automatically splits large messages (> 32 bytes) into fragments
- Sequential transmission with configurable delays
- Supports messages up to 512 bytes
- Transparent to application layer

### ✅ Message Reassembly (Node-Side)
- Automatic reassembly of fragmented messages
- Timeout protection (1.5 seconds)
- Buffer overflow protection
- Out-of-order fragment detection

### ✅ Duplicate Protection
- Sequence number validation
- Prevents duplicate command execution
- Per-peer tracking (hub-side)

### ✅ RX Queue (ISR-Safe)
- Messages queued from ISR context
- Processed in main loop (non-blocking)
- Configurable queue size (default 10 entries)
- ESP32: FreeRTOS queue | ESP8266: std::queue

### ✅ Retry Mechanism
- Automatic retry on send failure
- Exponential backoff strategy
- Configurable max retries (default 3)
- Per-message retry tracking

### ✅ Offline Detection
- Heartbeat-based peer tracking (hub-side)
- Online/offline status per peer
- Optional offline check before sending
- Timeout-based offline detection

### ✅ Statistics & Diagnostics
- Messages sent/received counters
- Send failure tracking
- Fragment statistics
- Duplicate detection counters
- Reassembly timeout tracking

---

## Quick Start

### Hub Example

```cpp
#include <Arduino.h>
#include "ESPNowManager.h"

void onAnnounceReceived(const uint8_t* mac, const AnnounceMessage& announce) {
    Serial.printf("Node announced: %s\n", announce.nodeName);
    ESPNowManager::getInstance().addPeer(mac);
}

void onHeartbeatReceived(const uint8_t* mac, const HeartbeatMessage& heartbeat) {
    Serial.printf("Heartbeat from %02X:%02X:... (health: %d%%)\n", 
                  mac[0], mac[1], heartbeat.health);
}

void setup() {
    Serial.begin(115200);
    WiFi.mode(WIFI_STA);
    
    // Initialize ESP-NOW manager as hub
    ESPNowManager::getInstance().begin(6, true);  // Channel 6, isHub=true
    
    // Register callbacks
    ESPNowManager::getInstance().onAnnounceReceived(onAnnounceReceived);
    ESPNowManager::getInstance().onHeartbeatReceived(onHeartbeatReceived);
}

void loop() {
    // Process queued messages
    ESPNowManager::getInstance().processQueue();
    
    // Send command to node
    uint8_t nodeMac[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    uint8_t commandData[] = {0x01, 0x64};  // Example command
    
    // Option 1: Simple send
    ESPNowManager::getInstance().send(nodeMac, commandData, 2, true);  // checkOnline=true
    
    // Option 2: Send with retry
    ESPNowManager::getInstance().sendWithRetry(nodeMac, commandData, 2);
    
    // Option 3: Send large message (automatic fragmentation)
    uint8_t largeData[128];
    ESPNowManager::getInstance().sendFragmented(nodeMac, 42, largeData, 128, true);
    
    // Check for offline peers every 60 seconds
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck > 60000) {
        lastCheck = millis();
        int offlineCount = ESPNowManager::getInstance().checkPeerTimeouts(90000);  // 90s timeout
        Serial.printf("Offline peers: %d\n", offlineCount);
    }
    
    delay(10);
}
```

### Node Example

```cpp
#include <Arduino.h>
#include "ESPNowManager.h"

uint8_t hubMac[6];
bool hubDiscovered = false;

void onCommandReceived(const uint8_t* mac, const uint8_t* data, size_t len) {
    Serial.printf("Command received: %d bytes\n", len);
    
    // Process command
    uint8_t cmdType = data[0];
    switch(cmdType) {
        case 0x01:
            Serial.printf("Set level to %d\n", data[1]);
            break;
    }
    
    // Send acknowledgment (STATUS message)
    StatusMessage status = {};
    status.header.type = MessageType::STATUS;
    status.header.nodeType = NodeType::LIGHT;
    status.commandId = 42;  // Match command ID
    status.statusCode = 0;  // Success
    
    ESPNowManager::getInstance().send(mac, (uint8_t*)&status, sizeof(status));
}

void setup() {
    Serial.begin(115200);
    WiFi.mode(WIFI_STA);
    
    // Initialize ESP-NOW manager as node
    ESPNowManager::getInstance().begin(6, false);  // Channel 6, isHub=false
    
    // Register callback
    ESPNowManager::getInstance().onCommandReceived(onCommandReceived);
    
    // Send ANNOUNCE to discover hub
    AnnounceMessage announce = {};
    announce.header.type = MessageType::ANNOUNCE;
    announce.header.nodeType = NodeType::LIGHT;
    strcpy(announce.nodeName, "Main Tank Light");
    announce.firmwareVersion = 1;
    
    uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    ESPNowManager::getInstance().send(broadcastMac, (uint8_t*)&announce, sizeof(announce));
}

void loop() {
    // Process queued messages (includes reassembly)
    ESPNowManager::getInstance().processQueue();
    
    // Send heartbeat every 30 seconds
    static unsigned long lastHeartbeat = 0;
    if (millis() - lastHeartbeat > 30000) {
        lastHeartbeat = millis();
        
        HeartbeatMessage heartbeat = {};
        heartbeat.header.type = MessageType::HEARTBEAT;
        heartbeat.header.nodeType = NodeType::LIGHT;
        heartbeat.health = 100;
        heartbeat.uptimeMinutes = millis() / 60000;
        
        if (hubDiscovered) {
            ESPNowManager::getInstance().send(hubMac, (uint8_t*)&heartbeat, sizeof(heartbeat));
        }
    }
    
    delay(10);
}
```

---

## API Reference

### Initialization

#### `bool begin(uint8_t channel, bool isHub)`
Initialize ESP-NOW manager.

- **channel**: WiFi channel (1-13, typically 6)
- **isHub**: true for hub, false for node
- **Returns**: true if successful

#### `bool addPeer(const uint8_t* mac)`
Add peer to ESP-NOW peer list.

- **mac**: 6-byte MAC address
- **Returns**: true if successful

#### `bool removePeer(const uint8_t* mac)`
Remove peer from ESP-NOW peer list.

---

### Sending Messages

#### `bool send(const uint8_t* mac, const uint8_t* data, size_t len, bool checkOnline = false)`
Send single-frame message.

- **mac**: Destination MAC address
- **data**: Message data
- **len**: Message length (max 250 bytes)
- **checkOnline**: If true, check peer is online before sending (hub only)
- **Returns**: true if sent successfully

#### `bool sendFragmented(const uint8_t* mac, uint8_t commandId, const uint8_t* data, size_t len, bool checkOnline = false)`
Send large message with automatic fragmentation.

- **mac**: Destination MAC address
- **commandId**: Unique command ID
- **data**: Large data buffer
- **len**: Data length (up to 512 bytes)
- **checkOnline**: Check peer online before sending
- **Returns**: true if all fragments sent successfully

#### `bool sendWithRetry(const uint8_t* mac, const uint8_t* data, size_t len, uint8_t maxRetries = 3)`
Send message with automatic retry on failure.

- **maxRetries**: Maximum retry attempts (default 3)
- **Returns**: true if sent successfully (possibly after retries)

---

### Receiving Messages

#### `void processQueue()`
Process messages from RX queue. **Must be called regularly from main loop.**

#### `void onCommandReceived(callback)`
Register callback for received commands (after reassembly).

```cpp
void callback(const uint8_t* mac, const uint8_t* data, size_t len);
```

#### `void onStatusReceived(callback)`
Register callback for received status messages.

```cpp
void callback(const uint8_t* mac, const StatusMessage& status);
```

#### `void onHeartbeatReceived(callback)`
Register callback for received heartbeat messages.

```cpp
void callback(const uint8_t* mac, const HeartbeatMessage& heartbeat);
```

#### `void onAnnounceReceived(callback)`
Register callback for received announce messages.

```cpp
void callback(const uint8_t* mac, const AnnounceMessage& announce);
```

---

### Peer Management (Hub Only)

#### `void setPeerOnline(const uint8_t* mac, bool online)`
Set peer online/offline status.

#### `bool isPeerOnline(const uint8_t* mac) const`
Check if peer is online.

#### `void updatePeerHeartbeat(const uint8_t* mac)`
Update peer heartbeat timestamp (marks as online if was offline).

#### `int checkPeerTimeouts(uint32_t timeoutMs)`
Check all peers for heartbeat timeout.

- **timeoutMs**: Timeout in milliseconds (e.g., 90000 for 90 seconds)
- **Returns**: Number of peers marked offline

---

### Diagnostics

#### `Statistics getStatistics() const`
Get statistics structure.

```cpp
struct Statistics {
    uint32_t messagesSent;
    uint32_t messagesReceived;
    uint32_t sendFailures;
    uint32_t retries;
    uint32_t fragmentsSent;
    uint32_t fragmentsReceived;
    uint32_t reassemblyTimeouts;
    uint32_t duplicatesIgnored;
};
```

#### `void resetStatistics()`
Reset all statistics counters to zero.

#### `void printStatistics() const`
Print formatted statistics to Serial.

---

## Configuration

Edit constants in `ESPNowManager.h`:

```cpp
#define ESPNOW_MAX_DATA_LEN 250              // ESP-NOW message size limit
#define ESPNOW_FRAGMENT_SIZE 32              // Bytes per fragment
#define ESPNOW_MAX_MESSAGE_SIZE 512          // Max reassembled message size
#define ESPNOW_REASSEMBLY_TIMEOUT_MS 1500    // Reassembly timeout
#define ESPNOW_MAX_RETRIES 3                 // Default max retries
#define ESPNOW_RETRY_BASE_DELAY_MS 100       // Base retry delay (exponential backoff)
#define ESPNOW_RX_QUEUE_SIZE 10              // RX queue depth
```

---

## Architecture Compliance

This library implements the architecture defined in `espnow-message-processing.md`:

- ✅ **Fragmentation**: Messages > 32 bytes split into fragments
- ✅ **Reassembly**: Nodes reassemble fragmented messages with timeout
- ✅ **Duplicate Protection**: Sequence number validation
- ✅ **RX Queue**: ISR-safe message queuing
- ✅ **Retry Logic**: Exponential backoff retries
- ✅ **Offline Checks**: Hub tracks peer online status
- ✅ **Safety Rules**: Safety commands remain single-frame
- ✅ **Non-blocking**: All operations non-blocking in main loop

---

## Testing

### Fragmentation Test

```cpp
// Hub sends large message
uint8_t largeData[256];
for (int i = 0; i < 256; i++) largeData[i] = i;

ESPNowManager::getInstance().sendFragmented(nodeMac, 42, largeData, 256);

// Node receives and reassembles
void onCommandReceived(const uint8_t* mac, const uint8_t* data, size_t len) {
    Serial.printf("Reassembled: %d bytes\n", len);
    // Verify data integrity
    for (int i = 0; i < len; i++) {
        if (data[i] != (i % 256)) {
            Serial.println("ERROR: Data corruption!");
        }
    }
}
```

### Retry Test

```cpp
// Simulate send failure
ESPNowManager::getInstance().removePeer(nodeMac);  // Force failure

// Send with retry
bool success = ESPNowManager::getInstance().sendWithRetry(nodeMac, data, len, 5);

// Re-add peer
ESPNowManager::getInstance().addPeer(nodeMac);
```

### Offline Check Test

```cpp
// Mark peer offline
ESPNowManager::getInstance().setPeerOnline(nodeMac, false);

// Try send with online check
bool sent = ESPNowManager::getInstance().send(nodeMac, data, len, true);  // Will fail

// Mark online
ESPNowManager::getInstance().setPeerOnline(nodeMac, true);
sent = ESPNowManager::getInstance().send(nodeMac, data, len, true);  // Will succeed
```

---

## Troubleshooting

### Messages Not Received
- Ensure both devices on same WiFi channel
- Call `processQueue()` regularly in loop
- Check RX queue isn't overflowing (increase `ESPNOW_RX_QUEUE_SIZE`)

### Fragmentation Fails
- Verify message size ≤ `ESPNOW_MAX_MESSAGE_SIZE`
- Check fragment size matches protocol (`ESPNOW_FRAGMENT_SIZE`)
- Add delay between fragments if node can't keep up

### Reassembly Timeout
- Increase `ESPNOW_REASSEMBLY_TIMEOUT_MS` for poor WiFi
- Reduce fragment transmission delay in sender
- Check for packet loss (statistics)

### Duplicate Messages
- Verify sequence numbers incrementing correctly
- Check `duplicatesIgnored` counter in statistics
- Ensure only one instance of ESPNowManager

---

## Performance

| Metric | Value |
|--------|-------|
| Max single message | 250 bytes |
| Max fragmented message | 512 bytes (configurable) |
| Fragment size | 32 bytes |
| Reassembly timeout | 1.5 seconds |
| Retry delay (1st) | 100ms |
| Retry delay (2nd) | 200ms |
| Retry delay (3rd) | 400ms |
| RX queue depth | 10 messages |
| Latency (single message) | < 10ms |
| Latency (8 fragments) | ~100ms |

---

## License

MIT License - See LICENSE file for details

---

## Contributing

This library is part of the Aquarium Management System project.  
Repository: https://github.com/bghosh412/aquarium-management-system

---

**Version**: 1.0.0  
**Last Updated**: January 2, 2026  
**Maintainer**: Aquarium Management System Team
