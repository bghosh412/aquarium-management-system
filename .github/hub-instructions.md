# Hub Instructions - ESP32-S3 Central Controller

**Complete guide for ESP32-S3 hub development in the Aquarium Management System.**

---

## 📋 Table of Contents

1. [Hub Hardware Specifications](#hub-hardware-specifications)
2. [Hub Responsibilities](#hub-responsibilities)
3. [FreeRTOS Task Architecture](#freertos-task-architecture)
4. [Aggressive Memory Management](#aggressive-memory-management)
5. [Configuration System](#configuration-system)
6. [Node Discovery & Registration](#node-discovery--registration)
7. [Watchdog & Heartbeat Monitoring](#watchdog--heartbeat-monitoring)
8. [WebServer & UI Setup](#webserver--ui-setup)
9. [WebSocket Communication](#websocket-communication)
10. [Hub Firmware Structure](#hub-firmware-structure)
11. [Hub Data Organization](#hub-data-organization)
12. [Safety Orchestration](#safety-orchestration)
13. [Multi-Tank Support](#multi-tank-support)
14. [Build & Deploy](#build--deploy)

---

## Hub Hardware Specifications

### MCU: ESP32-S3-N16R8

- **Flash**: 16MB (plenty for firmware + web UI)
- **PSRAM**: 8MB (for complex WebSocket handling)
- **Cores**: Dual-core Xtensa LX7 @ 240MHz
- **Wi-Fi**: 2.4GHz (for ESP-NOW + web UI)
- **GPIO**: Sufficient for future expansion
- **Advantages over ESP8266**:
  - More memory for complex UI
  - FreeRTOS native support
  - Better processing power for multi-tank orchestration
  - PSRAM for buffering large data

### Pin Assignments

*To be defined during hardware integration phase*

Hub typically does NOT control hardware directly - it orchestrates nodes.

---

## Hub Responsibilities

The hub is the **central brain** of the aquarium system. It does NOT execute actions directly but coordinates all nodes.

### Core Functions

1. **System Logic**
   - All decision-making happens here
   - Scheduling (feeding times, light cycles, CO₂ injection)
   - Multi-tank coordination
   - Safety rules enforcement

2. **Node Discovery**
   - Listen for broadcast ANNOUNCE messages
   - Dynamically register nodes by MAC address
   - Send unicast ACK to confirm registration
   - Maintain registry of all active nodes

3. **Safety Orchestration**
   - Monitor heartbeats from all nodes
   - Detect communication loss
   - Send fail-safe commands (e.g., CO₂ OFF, heater OFF)
   - Track device health status

4. **Web UI (Dashboard)**
   - Real-time system monitoring
   - Manual control interface
   - Configuration management
   - Logging and alerts

5. **Data Management**
   - Store configuration (LittleFS)
   - Log events (future: cloud integration)
   - Track historical data (sensor readings, actions)

6. **Communication Hub**
   - ESP-NOW message routing
   - WebSocket updates to browser clients
   - Command queuing and acknowledgment tracking

### What Hub Does NOT Do

- ❌ Direct hardware control (relays, servos, sensors)
- ❌ Low-level timing (PWM, precise delays)
- ❌ Sensor reading (delegates to nodes)

**Philosophy**: Hub orchestrates, nodes execute.

---

## FreeRTOS Task Architecture

The ESP32-S3 runs FreeRTOS natively. Use tasks for concurrent operations.

### Task Design Principles

1. **Short & Deterministic**: Tasks should complete quickly
2. **Non-Blocking**: Use queues/semaphores, not delays
3. **Priority-Based**: Critical tasks (watchdog) get higher priority
4. **Resource Protection**: Use mutexes for shared data

### Current Task Structure

```cpp
// Main loop (runs on core 0)
void loop() {
    // Handle incoming ESP-NOW messages
    // Process message queue
    // Update node registry
    // Send pending commands
}

// Watchdog task (runs on core 1)
void watchdogTask(void* parameter) {
    while(true) {
        // Check heartbeat timestamps
        // Trigger fail-safe if timeout
        // Log disconnections
        vTaskDelay(pdMS_TO_TICKS(5000));  // Check every 5s
    }
}
```

### Task Communication Patterns

```cpp
// Message queue for ESP-NOW events
QueueHandle_t espnowQueue;

// Mutex for node registry access
SemaphoreHandle_t registryMutex;

// Event group for system state
EventGroupHandle_t systemEvents;
```

### Example Task Creation

```cpp
void setup() {
    // Create queues
    espnowQueue = xQueueCreate(10, sizeof(ESPNowEvent));
    
    // Create mutexes
    registryMutex = xSemaphoreCreateMutex();
    
    // Create watchdog task on core 1
    xTaskCreatePinnedToCore(
        watchdogTask,      // Function
        "Watchdog",        // Name
        4096,              // Stack size
        NULL,              // Parameters
        2,                 // Priority (higher than default)
        NULL,              // Task handle
        1                  // Core 1
    );
}
```

### Task Priorities

| Task | Priority | Core | Purpose |
|------|----------|------|---------|
| Main Loop | 1 (default) | 0 | ESP-NOW handling |
| Watchdog | 2 | 1 | Safety monitoring |
| WebSocket | 1 | 0 | UI updates |
| Scheduler | 1 | 0 | Timed commands |

**Rule**: Never block the main loop. Use queues to defer work.

---

## Aggressive Memory Management

The ESP32-S3 has abundant memory (8MB PSRAM + internal heap), but proper management is critical for long-term stability.

### Memory Architecture

**ESP32-S3-N16R8 Memory Layout:**
- **Internal SRAM**: ~512KB (fast, for critical operations)
- **PSRAM**: 8MB (slower, for buffers and large data)
- **Flash**: 16MB (code + filesystem)

### Memory Management Strategy

#### 1. Heap Monitoring

**Continuous monitoring prevents memory leaks:**

```cpp
void printMemoryStatus() {
    uint32_t freeHeap = ESP.getFreeHeap() / 1024;  // KB
    uint32_t totalHeap = ESP.getHeapSize() / 1024;  // KB
    uint32_t freePSRAM = ESP.getFreePsram() / 1024;  // KB
    uint32_t totalPSRAM = ESP.getPsramSize() / 1024;  // KB
    
    Serial.printf("💾 HEAP:  %u KB free / %u KB total (%.1f%%)\n", 
                  freeHeap, totalHeap, 
                  (freeHeap * 100.0) / totalHeap);
    Serial.printf("💾 PSRAM: %u KB free / %u KB total (%.1f%%)\n", 
                  freePSRAM, totalPSRAM, 
                  (freePSRAM * 100.0) / totalPSRAM);
    
    // Warnings
    if (freeHeap < HEAP_WARNING_THRESHOLD_KB) {
        Serial.printf("⚠️  HEAP WARNING: Only %u KB free!\n", freeHeap);
    }
}
```

**Call from heartbeat task every 30 seconds.**

#### 2. Aggressive Cleanup

**Periodic integrity checks and garbage collection:**

```cpp
void aggressiveMemoryCleanup() {
    // Force heap integrity check
    heap_caps_check_integrity_all(true);
    
    // Clear WebSocket client buffers if inactive
    ws.cleanupClients();
    
    // Flush any pending file operations
    LittleFS.end();
    LittleFS.begin(false);  // Don't format, just remount
    
    // Log cleanup
    Serial.println("🧹 Aggressive cleanup complete");
}
```

**Trigger every 60 seconds or when free heap < 100KB.**

#### 3. Configuration-Driven Heartbeat

**hub_config.txt controls monitoring:**

```ini
# Enable/disable heartbeat monitoring
HEARTBEAT_ENABLED=true
HEARTBEAT_INTERVAL_SEC=30

# Memory thresholds
HEAP_WARNING_THRESHOLD_KB=50
PSRAM_WARNING_THRESHOLD_KB=100

# Aggressive management
AGGRESSIVE_MEMORY_MANAGEMENT=true
```

**Load in setup():**

```cpp
void loadConfiguration() {
    File file = LittleFS.open("/config/hub_config.txt", "r");
    while (file.available()) {
        String line = file.readStringUntil('\n');
        // Parse KEY=VALUE
        if (key == "HEARTBEAT_ENABLED") {
            config.heartbeatEnabled = (value == "true");
        }
        // ... parse other settings
    }
    file.close();
}
```

#### 4. Memory Allocation Best Practices

**Use PSRAM for large buffers:**

```cpp
// Allocate WebSocket buffer in PSRAM
char* wsBuffer = (char*)ps_malloc(8192);  // 8KB buffer

// Always check allocation
if (wsBuffer == NULL) {
    Serial.println("❌ PSRAM allocation failed");
    return;
}

// Free when done
free(wsBuffer);
```

**Use internal heap for small, frequent allocations:**

```cpp
// Small message buffer (internal heap is faster)
char* msg = (char*)malloc(256);
```

**Prefer stack allocation when possible:**

```cpp
void handleMessage() {
    char buffer[256];  // Stack allocation (fastest)
    // Use buffer
    // Automatically freed when function returns
}
```

#### 5. String Management

**Avoid excessive String concatenation:**

```cpp
// ❌ BAD - Creates many temporary objects
String json = "{";
json += "\"status\":\"ok\",";
json += "\"value\":" + String(value);
json += "}";

// ✅ GOOD - Single allocation
char json[128];
snprintf(json, sizeof(json), 
         "{\"status\":\"ok\",\"value\":%d}", value);
```

**Use ArduinoJson for complex JSON:**

```cpp
// Efficient JSON creation
DynamicJsonDocument doc(512);  // Size in bytes
doc["status"] = "ok";
doc["value"] = value;

String json;
serializeJson(doc, json);
```

#### 6. WebSocket Buffer Management

**AsyncWebSocket can consume significant memory:**

```cpp
// Limit WebSocket clients
#define MAX_WS_CLIENTS 4

void onWebSocketEvent(AsyncWebSocket *server, 
                     AsyncWebSocketClient *client,
                     AwsEventType type, ...) {
    switch(type) {
        case WS_EVT_CONNECT:
            if (server->count() > MAX_WS_CLIENTS) {
                client->close();
                Serial.println("⚠️  Too many WebSocket clients");
            }
            break;
    }
}

// Periodic cleanup
void loop() {
    static unsigned long lastCleanup = 0;
    if (millis() - lastCleanup > 60000) {
        ws.cleanupClients();  // Close stale connections
        lastCleanup = millis();
    }
}
```

#### 7. FreeRTOS Task Stack Sizes

**Monitor task stack usage:**

```cpp
void heartbeatTask(void* parameter) {
    while(true) {
        // Check own stack usage
        UBaseType_t stackHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
        Serial.printf("Heartbeat stack: %u words free\n", stackHighWaterMark);
        
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}
```

**Adjust stack sizes based on monitoring:**

```cpp
xTaskCreatePinnedToCore(
    heartbeatTask,
    "Heartbeat",
    4096,              // Stack size (increase if high water mark < 512)
    NULL,
    1,
    &heartbeatTaskHandle,
    0
);
```

#### 8. Memory Leak Detection

**Enable heap tracing during development:**

```cpp
#ifdef DEBUG_MEMORY
#include "esp_heap_trace.h"

void setup() {
    heap_trace_init_standalone(trace_record, NUM_RECORDS);
    heap_trace_start(HEAP_TRACE_LEAKS);
}

void loop() {
    static unsigned long lastDump = 0;
    if (millis() - lastDump > 300000) {  // Every 5 minutes
        heap_trace_dump();
        lastDump = millis();
    }
}
#endif
```

### Memory Management Checklist

- ✅ Monitor heap and PSRAM every 30 seconds
- ✅ Set warning thresholds (50KB heap, 100KB PSRAM)
- ✅ Enable aggressive cleanup every 60 seconds
- ✅ Limit WebSocket clients to 4
- ✅ Use PSRAM for buffers > 4KB
- ✅ Prefer stack allocation when possible
- ✅ Avoid String concatenation in loops
- ✅ Monitor task stack usage
- ✅ Clean up inactive WebSocket clients
- ✅ Test with heap tracing enabled

### Critical Memory Thresholds

| Metric | Warning | Critical | Action |
|--------|---------|----------|--------|
| Free Heap | < 100KB | < 50KB | Trigger cleanup |
| Free PSRAM | < 500KB | < 100KB | Close WebSocket clients |
| Task Stack | < 512 words | < 256 words | Increase stack size |
| Uptime | N/A | > 7 days | Scheduled reboot |

**Philosophy**: Proactive monitoring prevents crashes. Better to warn early than fail silently.

---

## Configuration System

Hub behavior is controlled by `hub_config.txt` in LittleFS.

### Configuration File Format

**Location**: `/config/hub_config.txt`

**Format**: `KEY=VALUE` (one per line, no spaces around `=`)

**Example**:

```ini
# Heartbeat Monitoring
HEARTBEAT_ENABLED=true
HEARTBEAT_INTERVAL_SEC=30

# Memory Management
AGGRESSIVE_MEMORY_MANAGEMENT=true
HEAP_WARNING_THRESHOLD_KB=50
PSRAM_WARNING_THRESHOLD_KB=100

# WiFi Settings
WIFI_AP_NAME=AquariumHub
WIFI_AP_PASSWORD=aquarium123
WIFI_TIMEOUT_SEC=180

# mDNS Configuration
MDNS_HOSTNAME=ams

# ESP-NOW Settings
ESPNOW_CHANNEL=6
ESPNOW_MAX_PEERS=20

# Debug Settings
DEBUG_SERIAL=true
DEBUG_ESPNOW=false
DEBUG_WEBSOCKET=false
```

### Loading Configuration

```cpp
struct HubConfig {
    bool heartbeatEnabled;
    uint32_t heartbeatIntervalSec;
    bool aggressiveMemoryManagement;
    // ... other fields
};

HubConfig config;

void loadConfiguration() {
    // Set defaults first
    config.heartbeatEnabled = true;
    config.heartbeatIntervalSec = 30;
    // ... set all defaults
    
    // Load from file
    if (!LittleFS.exists("/config/hub_config.txt")) {
        Serial.println("⚠️  Config file not found, using defaults");
        return;
    }
    
    File file = LittleFS.open("/config/hub_config.txt", "r");
    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        
        // Skip comments and empty lines
        if (line.startsWith("#") || line.length() == 0) {
            continue;
        }
        
        // Parse KEY=VALUE
        int sepIndex = line.indexOf('=');
        if (sepIndex == -1) continue;
        
        String key = line.substring(0, sepIndex);
        String value = line.substring(sepIndex + 1);
        key.trim();
        value.trim();
        
        // Apply settings
        if (key == "HEARTBEAT_ENABLED") {
            config.heartbeatEnabled = (value == "true");
        } else if (key == "HEARTBEAT_INTERVAL_SEC") {
            config.heartbeatIntervalSec = value.toInt();
        }
        // ... parse other keys
    }
    file.close();
    
    Serial.println("✅ Configuration loaded");
}
```

### Runtime Modification

**Configuration can be changed via web API:**

```cpp
server.on("/api/config", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("key", true) && request->hasParam("value", true)) {
        String key = request->getParam("key", true)->value();
        String value = request->getParam("value", true)->value();
        
        // Update runtime config
        if (key == "HEARTBEAT_ENABLED") {
            config.heartbeatEnabled = (value == "true");
        }
        
        // Save to file
        saveConfiguration();
        
        request->send(200, "text/plain", "Config updated");
    } else {
        request->send(400, "text/plain", "Missing parameters");
    }
});
```

---

## Node Discovery & Registration

Hub dynamically discovers nodes without hardcoded MAC addresses.

### Discovery Flow

```
1. Hub boots → Initializes ESP-NOW in promiscuous mode
2. Hub listens → Waits for broadcast ANNOUNCE
3. Node boots → Sends broadcast ANNOUNCE
4. Hub receives → Extracts sender MAC from esp_now_recv_cb
5. Hub registers → Adds MAC to peer list
6. Hub replies → Sends unicast ACK to node MAC
7. Node receives ACK → Switches to unicast mode
8. Node starts → Sends periodic HEARTBEAT
```

### Implementation Pattern

```cpp
#include <esp_now.h>
#include <WiFi.h>
#include <map>

// Node registry
struct NodeInfo {
    uint8_t mac[6];
    NodeType type;
    uint8_t tankId;
    char name[32];
    uint32_t lastHeartbeat;
    bool isRegistered;
};

std::map<uint64_t, NodeInfo> nodeRegistry;  // Key: MAC as uint64_t

// ESP-NOW receive callback
void onESPNowRecv(const uint8_t* mac, const uint8_t* data, int len) {
    MessageHeader* header = (MessageHeader*)data;
    
    switch(header->type) {
        case MessageType::ANNOUNCE: {
            AnnounceMessage* msg = (AnnounceMessage*)data;
            registerNode(mac, msg);
            sendAck(mac, msg->header.tankId);
            break;
        }
        case MessageType::HEARTBEAT: {
            updateHeartbeat(mac);
            break;
        }
        case MessageType::STATUS: {
            handleStatus(mac, (StatusMessage*)data);
            break;
        }
    }
}

void registerNode(const uint8_t* mac, AnnounceMessage* msg) {
    uint64_t macKey = macToKey(mac);
    
    // Check if already registered
    if(nodeRegistry.find(macKey) != nodeRegistry.end()) {
        Serial.println("Node already registered, updating info");
    }
    
    // Add node to registry
    NodeInfo info;
    memcpy(info.mac, mac, 6);
    info.type = msg->header.nodeType;
    info.tankId = msg->header.tankId;
    strncpy(info.name, msg->nodeName, sizeof(info.name));
    info.lastHeartbeat = millis();
    info.isRegistered = true;
    
    nodeRegistry[macKey] = info;
    
    // Add as ESP-NOW peer
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, mac, 6);
    peerInfo.channel = ESPNOW_CHANNEL;
    peerInfo.encrypt = false;
    
    if(esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("Failed to add peer");
    }
    
    Serial.printf("Registered node: %s (Tank %d, Type %d)\n", 
                  info.name, info.tankId, (int)info.type);
}

void sendAck(const uint8_t* mac, uint8_t tankId) {
    AckMessage ack;
    ack.header.type = MessageType::ACK;
    ack.header.tankId = tankId;
    ack.header.nodeType = NodeType::HUB;
    ack.header.timestamp = millis();
    
    esp_now_send(mac, (uint8_t*)&ack, sizeof(ack));
}

uint64_t macToKey(const uint8_t* mac) {
    uint64_t key = 0;
    for(int i = 0; i < 6; i++) {
        key |= ((uint64_t)mac[i]) << (i * 8);
    }
    return key;
}
```

### Key Points

- ✅ **No hardcoded MACs**: Discovery is fully dynamic
- ✅ **Broadcast ANNOUNCE**: Nodes find hub automatically
- ✅ **Unicast after ACK**: Reduces network traffic
- ✅ **Re-registration safe**: Handles node reboots gracefully
- ✅ **MAC-based registry**: Uses MAC as unique identifier

---

## Watchdog & Heartbeat Monitoring

Hub monitors node health via periodic heartbeats.

### Watchdog Design

**Timeout**: 60 seconds (nodes send heartbeat every 30s)

If no heartbeat received within 60s → trigger fail-safe for that node.

### Implementation

```cpp
#define HEARTBEAT_TIMEOUT_MS 60000  // 60 seconds

void watchdogTask(void* parameter) {
    while(true) {
        xSemaphoreTake(registryMutex, portMAX_DELAY);
        
        uint32_t now = millis();
        
        for(auto& pair : nodeRegistry) {
            NodeInfo& node = pair.second;
            
            uint32_t timeSinceHeartbeat = now - node.lastHeartbeat;
            
            if(timeSinceHeartbeat > HEARTBEAT_TIMEOUT_MS && node.isRegistered) {
                Serial.printf("⚠️ Node %s timeout! Triggering fail-safe\n", node.name);
                triggerFailSafe(node);
                node.isRegistered = false;  // Mark as disconnected
            }
        }
        
        xSemaphoreGive(registryMutex);
        
        vTaskDelay(pdMS_TO_TICKS(5000));  // Check every 5 seconds
    }
}

void triggerFailSafe(NodeInfo& node) {
    // Send fail-safe command based on node type
    CommandMessage cmd;
    cmd.header.type = MessageType::COMMAND;
    cmd.header.tankId = node.tankId;
    cmd.header.nodeType = NodeType::HUB;
    cmd.header.timestamp = millis();
    cmd.commandId = generateCommandId();
    cmd.finalCommand = true;
    
    switch(node.type) {
        case NodeType::CO2:
            // CRITICAL: Turn off CO₂
            cmd.commandData[0] = CMD_TURN_OFF;
            Serial.println("Fail-safe: CO₂ OFF");
            break;
            
        case NodeType::HEATER:
            // CRITICAL: Turn off heater
            cmd.commandData[0] = CMD_TURN_OFF;
            Serial.println("Fail-safe: Heater OFF");
            break;
            
        case NodeType::LIGHT:
            // Safe: Lights can stay on (or configurable)
            // No action or send last known state
            return;
            
        case NodeType::FISH_FEEDER:
            // Safe: Do nothing (skip feeding)
            return;
            
        default:
            return;
    }
    
    esp_now_send(node.mac, (uint8_t*)&cmd, sizeof(cmd));
}

void updateHeartbeat(const uint8_t* mac) {
    uint64_t macKey = macToKey(mac);
    
    xSemaphoreTake(registryMutex, portMAX_DELAY);
    
    if(nodeRegistry.find(macKey) != nodeRegistry.end()) {
        nodeRegistry[macKey].lastHeartbeat = millis();
        
        // Re-register if was previously disconnected
        if(!nodeRegistry[macKey].isRegistered) {
            nodeRegistry[macKey].isRegistered = true;
            Serial.printf("✅ Node %s reconnected\n", nodeRegistry[macKey].name);
        }
    }
    
    xSemaphoreGive(registryMutex);
}
```

### Safety Guarantees

| Node Type | Timeout Action | Reason |
|-----------|---------------|--------|
| CO₂ Regulator | **Turn OFF** | Prevent CO₂ overdose (lethal) |
| Heater | **Turn OFF** | Prevent overheating (lethal) |
| Lighting | Hold last state | Safe to stay on |
| Fish Feeder | Do nothing | Safer to skip feeding |
| Water Quality | Continue reading | Passive monitoring |
| Repeater | Continue forwarding | Passive relay |

**Critical Rule**: Always fail in the safest direction.

---

## WebServer & UI Setup

Hub hosts a web interface for monitoring and control.

### AsyncWebServer Configuration

```cpp
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <LittleFS.h>

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

void setupWebServer() {
    // Mount filesystem
    if(!LittleFS.begin(true)) {
        Serial.println("LittleFS mount failed");
        return;
    }
    
    // Serve static files
    server.serveStatic("/", LittleFS, "/UI/").setDefaultFile("index.html");
    
    // WebSocket handler
    ws.onEvent(onWebSocketEvent);
    server.addHandler(&ws);
    
    // API endpoints
    server.on("/api/nodes", HTTP_GET, handleGetNodes);
    server.on("/api/config", HTTP_GET, handleGetConfig);
    server.on("/api/command", HTTP_POST, handleCommand);
    
    // Start server
    server.begin();
    Serial.println("Web server started on http://<hub-ip>/");
}

void handleGetNodes(AsyncWebServerRequest* request) {
    String json = "[";
    
    xSemaphoreTake(registryMutex, portMAX_DELAY);
    
    bool first = true;
    for(auto& pair : nodeRegistry) {
        if(!first) json += ",";
        first = false;
        
        NodeInfo& node = pair.second;
        json += "{";
        json += "\"name\":\"" + String(node.name) + "\",";
        json += "\"type\":" + String((int)node.type) + ",";
        json += "\"tankId\":" + String(node.tankId) + ",";
        json += "\"online\":" + String(node.isRegistered ? "true" : "false");
        json += "}";
    }
    
    xSemaphoreGive(registryMutex);
    
    json += "]";
    request->send(200, "application/json", json);
}
```

### LittleFS File Structure

Hub uses LittleFS to store web UI and configuration.

```
data/                       (uploaded to ESP32 flash)
├── UI/
│   ├── index.html         # Main dashboard
│   ├── styles/
│   │   └── styles.css     # Stylesheet
│   ├── scripts/
│   │   └── app.js         # JavaScript (WebSocket client)
│   └── images/            # Icons, logos
│
└── config/
    └── config.json        # System configuration
```

### Upload Filesystem

```bash
platformio run --environment hub_esp32 --target uploadfs
```

This uploads `src/hub/data/` to LittleFS on the ESP32.

---

## WebSocket Communication

Real-time updates between hub and browser clients.

### WebSocket Pattern

```cpp
void onWebSocketEvent(AsyncWebSocket* server, 
                     AsyncWebSocketClient* client,
                     AwsEventType type,
                     void* arg,
                     uint8_t* data,
                     size_t len) {
    switch(type) {
        case WS_EVT_CONNECT:
            Serial.printf("WebSocket client #%u connected\n", client->id());
            sendFullState(client);  // Send current state on connect
            break;
            
        case WS_EVT_DISCONNECT:
            Serial.printf("WebSocket client #%u disconnected\n", client->id());
            break;
            
        case WS_EVT_DATA:
            handleWebSocketMessage(arg, data, len);
            break;
    }
}

void handleWebSocketMessage(void* arg, uint8_t* data, size_t len) {
    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    
    if(info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
        data[len] = 0;  // Null-terminate
        
        // Parse JSON command from browser
        // Example: {"action":"feedFish","tankId":1,"portions":2}
        
        DynamicJsonDocument doc(256);
        deserializeJson(doc, (char*)data);
        
        String action = doc["action"];
        uint8_t tankId = doc["tankId"];
        
        if(action == "feedFish") {
            sendFeedCommand(tankId, doc["portions"]);
        } else if(action == "setLight") {
            sendLightCommand(tankId, doc["level"]);
        }
        // ... handle other commands
    }
}

void broadcastUpdate(const char* event, const char* data) {
    // Send update to all connected clients
    String msg = "{\"event\":\"" + String(event) + "\",\"data\":" + String(data) + "}";
    ws.textAll(msg);
}

// Example: Broadcast when status received from node
void handleStatus(const uint8_t* mac, StatusMessage* msg) {
    // Update internal state
    // ...
    
    // Notify all browser clients
    String json = "{\"mac\":\"" + macToString(mac) + "\",";
    json += "\"status\":" + String(msg->statusCode) + "}";
    broadcastUpdate("nodeStatus", json.c_str());
}
```

### Client-Side (app.js)

```javascript
const ws = new WebSocket('ws://' + window.location.hostname + '/ws');

ws.onopen = () => {
    console.log('Connected to hub');
};

ws.onmessage = (event) => {
    const msg = JSON.parse(event.data);
    
    switch(msg.event) {
        case 'nodeStatus':
            updateNodeDisplay(msg.data);
            break;
        case 'sensorReading':
            updateSensorChart(msg.data);
            break;
    }
};

function feedFish(tankId, portions) {
    const cmd = {
        action: 'feedFish',
        tankId: tankId,
        portions: portions
    };
    ws.send(JSON.stringify(cmd));
}
```

---

## Hub Firmware Structure

### File Organization

```
src/hub/src/main.cpp        # Hub entry point
```

**Single-file approach** for now. Can be split later:

```cpp
// Future modular structure
src/hub/src/
├── main.cpp               # Setup and main loop
├── espnow_handler.cpp     # ESP-NOW logic
├── web_server.cpp         # Web server and WebSocket
├── node_registry.cpp      # Node discovery and management
├── watchdog.cpp           # Heartbeat monitoring
├── scheduler.cpp          # Timed commands
└── safety.cpp             # Fail-safe logic
```

### Current main.cpp Structure

```cpp
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include "protocol/messages.h"

// Global objects
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
std::map<uint64_t, NodeInfo> nodeRegistry;
SemaphoreHandle_t registryMutex;

// Function declarations
void setupWiFi();
void setupESPNow();
void setupWebServer();
void watchdogTask(void* parameter);
void onESPNowRecv(const uint8_t* mac, const uint8_t* data, int len);
void registerNode(const uint8_t* mac, AnnounceMessage* msg);
void sendAck(const uint8_t* mac, uint8_t tankId);
void updateHeartbeat(const uint8_t* mac);
void triggerFailSafe(NodeInfo& node);

void setup() {
    Serial.begin(115200);
    
    setupWiFi();
    setupESPNow();
    setupWebServer();
    
    registryMutex = xSemaphoreCreateMutex();
    
    xTaskCreatePinnedToCore(watchdogTask, "Watchdog", 4096, NULL, 2, NULL, 1);
    
    Serial.println("Hub ready");
}

void loop() {
    // ESP-NOW callbacks handle most work
    // WebSocket sends updates asynchronously
    delay(10);
}

// ... function implementations
```

---

## Hub Data Organization

### LittleFS Layout

```
src/hub/data/
├── UI/                    # Web interface files
│   ├── index.html        # Dashboard HTML
│   ├── styles/
│   │   └── styles.css    # Main stylesheet
│   ├── scripts/
│   │   └── app.js        # WebSocket client JavaScript
│   └── images/           # Logos, icons
│       ├── logo.png
│       └── favicon.ico
│
└── config/               # System configuration
    └── config.json       # Tank definitions, schedules
```

### config.json Example

```json
{
  "tanks": [
    {
      "id": 1,
      "name": "Main Tank",
      "volume": 100,
      "schedules": {
        "feeding": [
          {"time": "08:00", "portions": 2},
          {"time": "18:00", "portions": 3}
        ],
        "lighting": [
          {"time": "07:00", "white": 100, "blue": 80, "red": 0},
          {"time": "12:00", "white": 100, "blue": 100, "red": 50},
          {"time": "20:00", "white": 0, "blue": 0, "red": 0}
        ],
        "co2": [
          {"time": "07:00", "action": "on"},
          {"time": "20:00", "action": "off"}
        ]
      },
      "thresholds": {
        "temperature": {"min": 24, "max": 26},
        "ph": {"min": 6.5, "max": 7.5}
      }
    },
    {
      "id": 2,
      "name": "Breeding Tank",
      "volume": 50,
      "schedules": {
        "feeding": [
          {"time": "09:00", "portions": 1},
          {"time": "19:00", "portions": 1}
        ]
      }
    }
  ],
  "wifi": {
    "ssid": "AquariumHub",
    "password": "secure_password"
  }
}
```

### Loading Configuration

```cpp
void loadConfig() {
    File file = LittleFS.open("/config/config.json", "r");
    if(!file) {
        Serial.println("Config file not found");
        return;
    }
    
    DynamicJsonDocument doc(4096);
    deserializeJson(doc, file);
    file.close();
    
    JsonArray tanks = doc["tanks"];
    for(JsonObject tank : tanks) {
        uint8_t id = tank["id"];
        const char* name = tank["name"];
        
        // Load schedules, thresholds, etc.
        Serial.printf("Loaded tank %d: %s\n", id, name);
    }
}
```

---

## Safety Orchestration

Hub enforces safety rules across all nodes.

### Safety Principles

1. **Fail-Safe Defaults**: Always assume worst case
2. **Redundant Checks**: Multiple layers of protection
3. **Active Monitoring**: Continuous health checks
4. **Explicit Commands**: No implicit assumptions

### Safety Rules Implementation

```cpp
// Temperature safety
void checkTemperatureSafety(uint8_t tankId, float temp) {
    const float MAX_TEMP = 32.0;  // Critical maximum
    const float MIN_TEMP = 18.0;  // Critical minimum
    
    if(temp > MAX_TEMP) {
        Serial.printf("🚨 CRITICAL: Tank %d temp %.1f°C > %.1f°C\n", tankId, temp, MAX_TEMP);
        sendHeaterOff(tankId);
        broadcastAlert("Temperature critical - heater disabled");
    } else if(temp < MIN_TEMP) {
        Serial.printf("⚠️ WARNING: Tank %d temp %.1f°C < %.1f°C\n", tankId, temp, MIN_TEMP);
        broadcastAlert("Temperature low");
    }
}

// CO₂ safety
void checkCO2Safety(uint8_t tankId, uint32_t injectionTime) {
    const uint32_t MAX_INJECTION_MS = 300000;  // 5 minutes max
    
    if(injectionTime > MAX_INJECTION_MS) {
        Serial.printf("🚨 CRITICAL: Tank %d CO₂ injection exceeded safe limit\n", tankId);
        sendCO2Off(tankId);
        broadcastAlert("CO₂ safety timeout - valve closed");
    }
}

// Feeding safety
void checkFeedingSafety(uint8_t tankId, uint8_t portions) {
    const uint8_t MAX_PORTIONS = 5;
    const uint32_t MIN_FEED_INTERVAL_MS = 3600000;  // 1 hour
    
    if(portions > MAX_PORTIONS) {
        Serial.printf("⚠️ WARNING: Tank %d feeding portions %d > %d (clamped)\n", 
                     tankId, portions, MAX_PORTIONS);
        portions = MAX_PORTIONS;
    }
    
    uint32_t lastFeed = getLastFeedTime(tankId);
    if(millis() - lastFeed < MIN_FEED_INTERVAL_MS) {
        Serial.printf("⚠️ WARNING: Tank %d feeding too frequent (blocked)\n", tankId);
        return;
    }
    
    sendFeedCommand(tankId, portions);
}
```

### Critical Safety Matrix

| Hazard | Detection | Response | Recovery |
|--------|-----------|----------|----------|
| Overheating | Temp > 32°C | Heater OFF immediately | Manual intervention |
| CO₂ Overdose | Injection > 5 min | Valve OFF immediately | Wait 30 min |
| Overfeeding | > 5 portions/day | Block command | Reset at midnight |
| Node Timeout | No heartbeat 60s | Device-specific fail-safe | Auto-reconnect |
| Hub Crash | Watchdog timeout | ESP32 reset | Nodes enter fail-safe |

---

## Multi-Tank Support

Hub manages multiple aquariums simultaneously.

### Tank Identification

Every message includes `tankId` in header:

```cpp
struct MessageHeader {
    MessageType type;
    uint8_t tankId;        // 0 = broadcast, 1-255 = specific tank
    NodeType nodeType;
    uint32_t timestamp;
    uint8_t sequenceNum;
} __attribute__((packed));
```

### Tank-Aware Command Routing

```cpp
void sendFeedCommand(uint8_t tankId, uint8_t portions) {
    // Find all fish feeder nodes for this tank
    for(auto& pair : nodeRegistry) {
        NodeInfo& node = pair.second;
        
        if(node.type == NodeType::FISH_FEEDER && node.tankId == tankId && node.isRegistered) {
            CommandMessage cmd;
            cmd.header.type = MessageType::COMMAND;
            cmd.header.tankId = tankId;
            cmd.header.nodeType = NodeType::HUB;
            cmd.header.timestamp = millis();
            cmd.commandId = generateCommandId();
            cmd.finalCommand = true;
            cmd.commandData[0] = CMD_FEED;
            cmd.commandData[1] = portions;
            
            esp_now_send(node.mac, (uint8_t*)&cmd, sizeof(cmd));
            Serial.printf("Sent feed command to %s (Tank %d, %d portions)\n", 
                         node.name, tankId, portions);
        }
    }
}
```

### Per-Tank Scheduling

```cpp
struct TankSchedule {
    uint8_t tankId;
    std::vector<FeedingEvent> feedings;
    std::vector<LightingEvent> lightings;
    std::vector<CO2Event> co2Events;
};

std::map<uint8_t, TankSchedule> tankSchedules;

void checkSchedules() {
    uint32_t now = millis();
    
    for(auto& pair : tankSchedules) {
        TankSchedule& schedule = pair.second;
        
        // Check feeding schedule
        for(auto& event : schedule.feedings) {
            if(shouldTrigger(event, now)) {
                sendFeedCommand(schedule.tankId, event.portions);
            }
        }
        
        // Check lighting schedule
        for(auto& event : schedule.lightings) {
            if(shouldTrigger(event, now)) {
                sendLightCommand(schedule.tankId, event.white, event.blue, event.red);
            }
        }
        
        // Check CO₂ schedule
        for(auto& event : schedule.co2Events) {
            if(shouldTrigger(event, now)) {
                sendCO2Command(schedule.tankId, event.action);
            }
        }
    }
}
```

### Broadcast vs Unicast

- **Broadcast (tankId = 0)**: All nodes respond (e.g., emergency shutdown)
- **Unicast (tankId = 1-255)**: Only nodes for that tank respond

```cpp
void emergencyShutdown() {
    CommandMessage cmd;
    cmd.header.type = MessageType::COMMAND;
    cmd.header.tankId = 0;  // Broadcast to all tanks
    cmd.header.nodeType = NodeType::HUB;
    cmd.commandData[0] = CMD_EMERGENCY_STOP;
    
    // Send to all registered nodes
    for(auto& pair : nodeRegistry) {
        esp_now_send(pair.second.mac, (uint8_t*)&cmd, sizeof(cmd));
    }
    
    Serial.println("🚨 EMERGENCY SHUTDOWN TRIGGERED");
}
```

---

## Build & Deploy

### PlatformIO Environment

```ini
[env:hub_esp32]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
src_dir = src/hub/src
data_dir = src/hub/data
board_build.flash_size = 16MB
board_build.psram_type = opi
build_flags = 
    -DBOARD_HAS_PSRAM
    -DESPNOW_CHANNEL=6
lib_deps = 
    ESP Async WebServer
    AsyncTCP
    ArduinoJson
```

### Build Commands

```bash
# Compile hub firmware
platformio run --environment hub_esp32

# Upload firmware
platformio run --environment hub_esp32 --target upload

# Upload filesystem (web UI)
platformio run --environment hub_esp32 --target uploadfs

# Monitor serial output
platformio device monitor --environment hub_esp32
```

### Development Workflow

1. **Edit Code**: `src/hub/src/main.cpp`
2. **Edit UI**: `src/hub/data/UI/`
3. **Build**: `platformio run --environment hub_esp32`
4. **Upload Firmware**: `--target upload`
5. **Upload Filesystem**: `--target uploadfs` (only if UI changed)
6. **Test**: Monitor serial output and access web UI

### Debugging

```cpp
// Enable verbose logging
#define DEBUG_ESPNOW
#define DEBUG_WATCHDOG
#define DEBUG_WEBSOCKET

#ifdef DEBUG_ESPNOW
#define LOG_ESPNOW(...) Serial.printf(__VA_ARGS__)
#else
#define LOG_ESPNOW(...)
#endif
```

---

## Best Practices

### Hub Development Guidelines

1. **Non-Blocking Code**
   - Use FreeRTOS tasks, not delays
   - Process messages asynchronously
   - Use queues for inter-task communication

2. **Resource Management**
   - Protect shared data with mutexes
   - Free allocated memory promptly
   - Monitor heap usage (PSRAM available)

3. **Error Handling**
   - Always check return values
   - Log errors verbosely
   - Implement recovery strategies

4. **Safety First**
   - Validate all commands before sending
   - Implement redundant safety checks
   - Test fail-safe behavior thoroughly

5. **Scalability**
   - Design for multiple tanks
   - Support dynamic node registration
   - Keep node registry size-limited

6. **Testing**
   - Simulate node timeouts
   - Test multi-tank scenarios
   - Verify WebSocket stability under load

---

## Common Issues & Solutions

### ESP-NOW Init Fails

**Symptom**: `esp_now_init()` returns error

**Solution**:
```cpp
WiFi.mode(WIFI_STA);  // Must be in STA mode
WiFi.disconnect();     // Disconnect from any AP
esp_now_init();       // Then initialize
```

### LittleFS Mount Fails

**Symptom**: Web UI not accessible

**Solution**:
```bash
# Reformat and upload filesystem
platformio run --environment hub_esp32 --target uploadfs
```

### Watchdog False Positives

**Symptom**: Nodes marked as disconnected when online

**Solution**: Increase `HEARTBEAT_TIMEOUT_MS` or decrease watchdog check interval

### WebSocket Disconnects

**Symptom**: Browser loses connection frequently

**Solution**: Increase WebSocket buffer size, reduce broadcast frequency

---

## Future Enhancements

### Planned Features

- [ ] OTA firmware updates for hub and nodes
- [ ] Cloud logging (MQTT/HTTP)
- [ ] Mobile app integration
- [ ] Advanced scheduling (sunrise/sunset simulation)
- [ ] Machine learning for anomaly detection
- [ ] Remote access via VPN/tunnel

### Expansion Possibilities

- Additional node types (doser, auto water changer)
- Integration with smart home systems
- Multi-hub federation for large installations
- Historical data visualization

---

**Last Updated**: December 29, 2025  
**Hub Firmware Version**: 1.0 (Development)  
**Target Hardware**: ESP32-S3-N16R8  
**Repository**: bghosh412/aquarium-management-system
