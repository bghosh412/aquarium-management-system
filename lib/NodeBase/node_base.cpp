#include "node_base.h"
#include "ESPNowManager.h"

// ESP8266 OTA support
#ifdef ESP8266
    #include <Updater.h>
#else
    #include <Update.h>
#endif

// ============================================================================
// SHARED NODE BASE IMPLEMENTATION
// ============================================================================
// This file contains configuration, provisioning, and message handling
// logic shared by all node types.
//
// Each specific node (lighting, CO2, etc.) implements:
// - setupHardware()      - Initialize hardware pins/peripherals
// - enterFailSafeMode()  - Put hardware in safe state
// - handleCommand()      - Process command data from hub
// - updateHardware()     - Update hardware state (called every loop)
// ============================================================================

// Global configuration and state
NodeConfig nodeConfig;
bool isConnectedToHub = false;
uint32_t lastHeartbeatSent = 0;
uint8_t messageSequence = 0;
uint32_t nodeUnixTime = 0;  // Synchronized Unix timestamp
uint32_t nodeBootMillis = 0;  // millis() when time was synced
// Hub heartbeat tracking (node-side)
uint32_t lastHubHeartbeatReceived = 0;
bool hubHeartbeatLost = false;

// ============================================================================
// STATUS LED STATE
// ============================================================================
static StatusLEDMode currentLEDMode = StatusLEDMode::WAITING_ACK;  // Start in waiting mode
static uint32_t lastLEDToggle = 0;        // Last time LED was toggled
static bool ledState = false;              // Current LED state (for blinking)
static uint32_t commandActivityStart = 0;  // When command activity started
static bool statusLEDInitialized = false;  // Has setupStatusLED() been called?

// ============================================================================
// OTA STATE (Common to all nodes)
// ============================================================================

// Maximum chunks we can track (12000 chunks = ~348KB firmware)
#define OTA_MAX_CHUNKS 12000
#define OTA_BITSET_SIZE ((OTA_MAX_CHUNKS + 7) / 8)  // 1500 bytes

struct OtaState {
    bool active;            // OTA transfer in progress
    uint8_t type;           // OTA_CMD_FIRMWARE_CHUNK or OTA_CMD_CONFIG_CHUNK
    uint8_t commandId;      // Command ID for this OTA session
    uint32_t totalSize;     // Expected total bytes
    uint32_t receivedBytes; // Bytes received so far
    uint16_t expectedChunk; // Next expected chunk index (for sequential check)
    uint16_t totalChunks;   // Total expected chunks (calculated from totalSize)
    uint16_t chunksReceived; // Count of chunks received
    uint16_t highestChunkIdx; // Highest chunkIndex received
    uint8_t* buffer;        // Buffer for config file (heap allocated)
    size_t bufferSize;      // Size of allocated buffer
    bool beginCalled;       // For firmware: Update.begin() called
    uint8_t* receivedBitset; // Bitset tracking received chunkIndex (NOT commandSeqID)
};

static OtaState otaState = {0};

// Helper to set/check bitset - uses chunkIndex (0-based)
static inline void otaSetChunkReceived(uint16_t chunkIndex) {
    if (otaState.receivedBitset && chunkIndex < OTA_MAX_CHUNKS) {
        otaState.receivedBitset[chunkIndex / 8] |= (1 << (chunkIndex % 8));
    }
}

static inline bool otaIsChunkReceived(uint16_t chunkIndex) {
    if (otaState.receivedBitset && chunkIndex < OTA_MAX_CHUNKS) {
        return (otaState.receivedBitset[chunkIndex / 8] & (1 << (chunkIndex % 8))) != 0;
    }
    return false;
}

// Forward declarations
static void handleOtaBegin(const uint8_t* mac, const CommandMessage& cmd);
static void handleOtaChunk(const uint8_t* mac, const CommandMessage& cmd);
static void handleOtaEnd(const uint8_t* mac, const CommandMessage& cmd);
static void resetOtaState();

// Forward declaration for internal handler
static void onHubHeartbeatReceivedInternal(const uint8_t* mac, const HeartbeatMessage& msg);

// ============================================================================
// STATUS LED IMPLEMENTATION
// ============================================================================

void setupStatusLED() {
    pinMode(nodeConfig.statusLedPin, OUTPUT);
    digitalWrite(nodeConfig.statusLedPin, LOW);  // Start OFF
    ledState = false;
    lastLEDToggle = millis();
    currentLEDMode = StatusLEDMode::WAITING_ACK;  // Start in waiting for ACK mode
    statusLEDInitialized = true;
    
    if (nodeConfig.debugSerial) {
        Serial.printf("[LED] Status LED initialized on pin (GPIO%d)\n", nodeConfig.statusLedPin);
    }
}

void setStatusLEDMode(StatusLEDMode mode) {
    if (currentLEDMode != mode) {
        currentLEDMode = mode;
        lastLEDToggle = millis();  // Reset toggle timing on mode change
        
        if (nodeConfig.debugSerial) {
            const char* modeStr = "UNKNOWN";
            switch (mode) {
                case StatusLEDMode::WAITING_ACK:    modeStr = "WAITING_ACK (blink)"; break;
                case StatusLEDMode::FAILSAFE:       modeStr = "FAILSAFE (blink)"; break;
                case StatusLEDMode::COMMAND_ACTIVE: modeStr = "COMMAND_ACTIVE (blink)"; break;
                case StatusLEDMode::NORMAL:         modeStr = "NORMAL (solid ON)"; break;
            }
            Serial.printf("[LED] Mode changed to: %s\n", modeStr);
        }
    }
}

StatusLEDMode getStatusLEDMode() {
    return currentLEDMode;
}

void triggerCommandActivity() {
    // FAILSAFE mode has priority - don't let commands interrupt failsafe blinking
    // Failsafe will only clear when hub heartbeat is received
    if (currentLEDMode == StatusLEDMode::FAILSAFE) {
        return;  // Stay in failsafe, don't switch to command activity
    }
    
    commandActivityStart = millis();
    setStatusLEDMode(StatusLEDMode::COMMAND_ACTIVE);
}

void updateStatusLED() {
    if (!statusLEDInitialized) return;
    
    uint32_t now = millis();
    
    // Check if command activity period has expired (30 seconds)
    if (currentLEDMode == StatusLEDMode::COMMAND_ACTIVE) {
        if (now - commandActivityStart >= STATUS_LED_COMMAND_DURATION_MS) {
            // Command activity period over, return to normal mode
            // (unless we're in failsafe or waiting for ACK)
            if (hubHeartbeatLost) {
                setStatusLEDMode(StatusLEDMode::FAILSAFE);
            } else if (!isConnectedToHub) {
                setStatusLEDMode(StatusLEDMode::WAITING_ACK);
            } else {
                setStatusLEDMode(StatusLEDMode::NORMAL);
            }
        }
    }
    
    // Handle LED based on current mode
    switch (currentLEDMode) {
        case StatusLEDMode::WAITING_ACK:
        case StatusLEDMode::FAILSAFE:
        case StatusLEDMode::COMMAND_ACTIVE:
            // Blinking mode - toggle LED at interval
            if (now - lastLEDToggle >= STATUS_LED_BLINK_INTERVAL_MS) {
                lastLEDToggle = now;
                ledState = !ledState;
                digitalWrite(nodeConfig.statusLedPin, ledState ? HIGH : LOW);
            }
            break;
            
        case StatusLEDMode::NORMAL:
            // Solid ON
            if (!ledState) {
                ledState = true;
                digitalWrite(PIN_STATUS_LED, HIGH);
            }
            break;
    }
}

// ============================================================================
// CONFIGURATION MANAGEMENT
// ============================================================================

void loadNodeConfiguration(NodeType defaultType, const char* defaultName) {
    // Set defaults (unmapped state)
    nodeConfig.tankId = 0;  // 0 = unmapped
    nodeConfig.nodeName = String(defaultName);
    nodeConfig.nodeType = defaultType;
    nodeConfig.firmwareVersion = 1;
    #ifdef ESPNOW_CHANNEL
    nodeConfig.espnowChannel = ESPNOW_CHANNEL;
    #else
    nodeConfig.espnowChannel = 11;  // Failsafe channel
    #endif
    nodeConfig.debugSerial = true;
    nodeConfig.debugESPNOW = true;
    nodeConfig.debugHardware = false;
    nodeConfig.servoPin = 18;               // Default servo pin (override via /node_config.txt)
    nodeConfig.statusLedPin = PIN_STATUS_LED; // Default status LED pin (can be overridden)
    nodeConfig.announceIntervalMs = 5000;
    nodeConfig.heartbeatIntervalMs = 30000;
    nodeConfig.connectionTimeoutMs = 90000;
    nodeConfig.hubHeartbeatTimeoutMs = 600000; // 10 minutes default
    
    // Attempt to load from filesystem
    if (!LittleFS.begin()) {
        Serial.println("[WARN] LittleFS mount failed, using defaults");
        Serial.printf("[WARN] Failsafe channel in use: %d\n", nodeConfig.espnowChannel);
        return;
    }
    
    if (!LittleFS.exists("/node_config.txt")) {
        Serial.println("[WARN] Config file not found, using defaults");
        Serial.printf("[WARN] Failsafe channel in use: %d\n", nodeConfig.espnowChannel);
        saveNodeConfiguration();
        return;
    }
    
    File file = LittleFS.open("/node_config.txt", "r");
    if (!file) {
        Serial.println("[ERROR] Failed to open config file");
        Serial.printf("[WARN] Failsafe channel in use: %d\n", nodeConfig.espnowChannel);
        return;
    }
    
    Serial.println("[FILE] Loading configuration...");
    
    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        
        // Skip comments and empty lines
        if (line.startsWith("#") || line.length() == 0) {
            continue;
        }
        
        // Parse KEY=VALUE
        int separatorIndex = line.indexOf('=');
        if (separatorIndex == -1) {
            continue;
        }
        
        String key = line.substring(0, separatorIndex);
        String value = line.substring(separatorIndex + 1);
        key.trim();
        value.trim();
        
        // Apply configuration
        if (key == "TANK_ID" || key == "NODE_TANK_ID") {
            nodeConfig.tankId = value.toInt();
        } else if (key == "NODE_NAME") {
            nodeConfig.nodeName = value;
        } else if (key == "FIRMWARE_VERSION") {
            nodeConfig.firmwareVersion = value.toInt();
        } else if (key == "ESPNOW_CHANNEL") {
            nodeConfig.espnowChannel = value.toInt();
        } else if (key == "DEBUG_SERIAL") {
            nodeConfig.debugSerial = (value == "true");
        } else if (key == "DEBUG_ESPNOW") {
            nodeConfig.debugESPNOW = true;  // Always force ON for debugging
        } else if (key == "DEBUG_HARDWARE") {
            nodeConfig.debugHardware = (value == "true");
        } else if (key == "SERVO_PIN") {
            nodeConfig.servoPin = (uint8_t) value.toInt();
        } else if (key == "STATUS_LED_PIN") {
            nodeConfig.statusLedPin = (uint8_t) value.toInt();
        } else if (key == "ANNOUNCE_INTERVAL_MS") {
            nodeConfig.announceIntervalMs = value.toInt();
        } else if (key == "HEARTBEAT_INTERVAL_MS") {
            nodeConfig.heartbeatIntervalMs = value.toInt();
        } else if (key == "CONNECTION_TIMEOUT_MS") {
            nodeConfig.connectionTimeoutMs = value.toInt();
        } else if (key == "HUB_HEARTBEAT_TIMEOUT_MS") {
            nodeConfig.hubHeartbeatTimeoutMs = value.toInt();
        }
    }
    
    file.close();
    
    Serial.println("[OK] Configuration loaded");
    Serial.printf("   - Node: %s (Tank %d)\\n", nodeConfig.nodeName.c_str(), nodeConfig.tankId);
    Serial.printf("   - FW Version: v%d\\n", nodeConfig.firmwareVersion);
    Serial.printf("   - ESP-NOW Channel: %d\\n", nodeConfig.espnowChannel);
}

void saveNodeConfiguration() {
    if (!LittleFS.begin()) {
        Serial.println("[ERROR] LittleFS mount failed");
        return;
    }
    
    File file = LittleFS.open("/node_config.txt", "w");
    if (!file) {
        Serial.println("[ERROR] Failed to create config file");
        return;
    }
    
    file.printf("# Node Configuration (Provisioned)\\n");
    file.printf("# Last updated: %lu ms\\n\\n", millis());
    file.printf("TANK_ID=%d\\n", nodeConfig.tankId);
    file.printf("NODE_NAME=%s\\n", nodeConfig.nodeName.c_str());
    file.printf("FIRMWARE_VERSION=%d\\n", nodeConfig.firmwareVersion);
    file.printf("ESPNOW_CHANNEL=%d\\n", nodeConfig.espnowChannel);
    file.printf("DEBUG_SERIAL=%s\\n", nodeConfig.debugSerial ? "true" : "false");
    file.printf("DEBUG_ESPNOW=%s\\n", nodeConfig.debugESPNOW ? "true" : "false");
    file.printf("DEBUG_HARDWARE=%s\\n", nodeConfig.debugHardware ? "true" : "false");
    file.printf("SERVO_PIN=%d\\n", nodeConfig.servoPin);
    file.printf("STATUS_LED_PIN=%d\\n", nodeConfig.statusLedPin);
    file.printf("ANNOUNCE_INTERVAL_MS=%u\\n", nodeConfig.announceIntervalMs);
    file.printf("HEARTBEAT_INTERVAL_MS=%u\\n", nodeConfig.heartbeatIntervalMs);
    file.printf("CONNECTION_TIMEOUT_MS=%u\\n", nodeConfig.connectionTimeoutMs);
    file.close();
    
    Serial.println("[OK] Configuration saved to /node_config.txt");
}

// ============================================================================\n// MESSAGE HANDLERS\n// ============================================================================

void onAckReceived(const uint8_t* mac, const AckMessage& msg) {
    if (nodeConfig.debugESPNOW) {
        Serial.println("+========================================================+");
        Serial.printf("| [ACK] ACK received from %02X:%02X:%02X:%02X:%02X:%02X\\n",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        Serial.printf("| Channel: %d\n", nodeConfig.espnowChannel);
        Serial.printf("| Assigned Node ID: %d\\n", msg.assignedNodeId);
        Serial.printf("| Accepted: %s\\n", msg.accepted ? "YES" : "NO");
        Serial.println("+========================================================+");
    }
    
    // Prefer return MAC from ACK if provided (hub AP MAC)
    bool peerAdded = false;
    if (msg.accepted) {
        // If ACK carries a returnMac, use that as the hub peer for replies
        bool hasReturnMac = false;
        for (int i = 0; i < 6; ++i) {
            if (msg.returnMac[i] != 0x00) { hasReturnMac = true; break; }
        }

        if (hasReturnMac) {
            memcpy(nodeConfig.hubReturnMac, msg.returnMac, 6);
            nodeConfig.hubReturnMacSet = true;
            if (nodeConfig.debugESPNOW) {
                Serial.printf("[INFO] ACK provided returnMAC %02X:%02X:%02X:%02X:%02X:%02X\n",
                              nodeConfig.hubReturnMac[0], nodeConfig.hubReturnMac[1], nodeConfig.hubReturnMac[2],
                              nodeConfig.hubReturnMac[3], nodeConfig.hubReturnMac[4], nodeConfig.hubReturnMac[5]);
            }
            peerAdded = ESPNowManager::getInstance().addPeer(nodeConfig.hubReturnMac);
        } else {
            // Fallback to sender MAC
            peerAdded = ESPNowManager::getInstance().addPeer(mac);
        }
    }

    // Mark as connected to hub
    if (msg.accepted && peerAdded) {
        isConnectedToHub = true;
        setStatusLEDMode(StatusLEDMode::NORMAL);  // Connected - solid LED
        if (nodeConfig.debugSerial) {
            Serial.println("[OK] Connected to hub - ready for commands\\n");
        }
    } else if (msg.accepted && !peerAdded) {
        if (nodeConfig.debugSerial) {
            Serial.println("[WARN] ACK accepted but peer add failed; staying in discovery mode");
        }
    }
}

void onConfigReceived(const uint8_t* mac, const ConfigMessage& msg) {
    if (nodeConfig.debugESPNOW) {
        Serial.println("+========================================================+");
        Serial.printf("| [CFG]  CONFIG received from %02X:%02X:%02X:%02X:%02X:%02X\\n",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        Serial.printf("| Channel: %d\n", nodeConfig.espnowChannel);
        Serial.printf("| Assigned Tank ID: %d\\n", msg.header.tankId);
        Serial.printf("| Device Name: %s\\n", msg.deviceName);
        Serial.println("+========================================================+");
    }
    
    // Update configuration
    nodeConfig.tankId = msg.header.tankId;
    nodeConfig.nodeName = String(msg.deviceName);
    
    // Save to filesystem
    saveNodeConfiguration();
    
    // Send STATUS acknowledgment
    StatusMessage statusMsg = {};
    statusMsg.header.type = MessageType::STATUS;
    statusMsg.header.tankId = nodeConfig.tankId;
    statusMsg.header.nodeType = nodeConfig.nodeType;
    statusMsg.header.timestamp = millis();
    statusMsg.header.sequenceNum = messageSequence++;
    statusMsg.commandId = 0;
    statusMsg.statusCode = 0x00;  // SUCCESS
    
    ESPNowManager::getInstance().send(mac, (uint8_t*)&statusMsg, sizeof(statusMsg));
    
    Serial.printf("[OK] Node provisioned: Tank %d, Name '%s'\\n", nodeConfig.tankId, nodeConfig.nodeName.c_str());
    Serial.println("[RST] Restarting in 2 seconds to apply configuration...\\n");
    
    delay(2000);
    ESP.restart();
}

void onUnmapReceived(const uint8_t* mac, const UnmapMessage& msg) {
    if (nodeConfig.debugESPNOW) {
        Serial.println("+========================================================+");
        Serial.printf("| [UNMAP] UNMAP received from %02X:%02X:%02X:%02X:%02X:%02X\\n",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        Serial.printf("| Channel: %d\n", nodeConfig.espnowChannel);
        Serial.printf("| Reason: %d\\n", msg.reason);
        Serial.println("+========================================================+");
    }
    
    // Reset to discovery mode
    nodeConfig.tankId = 0;  // Unmapped
    isConnectedToHub = false;
    
    // Delete configuration file
    if (LittleFS.exists("/node_config.txt")) {
        LittleFS.remove("/node_config.txt");
        Serial.println("[OK] Configuration file deleted");
    }
    
    // Enter safe state
    enterFailSafeMode();
    
    Serial.println("[RST] Device unmapped - restarting in 2 seconds...\\n");
    Serial.println("[INFO] Device will enter discovery mode and start announcing\\n");
    
    delay(2000);
    ESP.restart();
}

void sendHeartbeat() {
    // Calculate current Unix time based on synced time + elapsed millis
    uint32_t currentUnixTime = 0;
    if (nodeUnixTime > 0 && nodeBootMillis > 0) {
        uint32_t elapsedSeconds = (millis() - nodeBootMillis) / 1000;
        currentUnixTime = nodeUnixTime + elapsedSeconds;
    }
    
    HeartbeatMessage msg = {};
    msg.header.type = MessageType::HEARTBEAT;
    msg.header.tankId = nodeConfig.tankId;
    msg.header.nodeType = nodeConfig.nodeType;
    msg.header.timestamp = millis();
    msg.header.sequenceNum = messageSequence++;
    msg.health = 100;
    msg.uptimeMinutes = millis() / 60000;
    msg.nodeUnixTime = currentUnixTime;
    
    // Broadcast to hub
    uint8_t broadcast[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    ESPNowManager::getInstance().send(broadcast, (uint8_t*)&msg, sizeof(msg));
    lastHeartbeatSent = millis();
    
    if (nodeConfig.debugESPNOW) {
        Serial.printf("[HB] Heartbeat sent (uptime: %dmin, time: %u, ch=%d)\n",
                      msg.uptimeMinutes, currentUnixTime, nodeConfig.espnowChannel);
    }
}

// Internal handler for hub heartbeat
static void onHubHeartbeatReceivedInternal(const uint8_t* mac, const HeartbeatMessage& msg) {
    // Only treat heartbeats from a HUB nodeType as hub heartbeat
    if (msg.header.nodeType == NodeType::HUB) {
        lastHubHeartbeatReceived = millis();
        if (hubHeartbeatLost) {
            // Recovered from failsafe
            hubHeartbeatLost = false;
            setStatusLEDMode(StatusLEDMode::NORMAL);  // Restored - solid LED
            if (nodeConfig.debugSerial) {
                Serial.println("[HB] Hub heartbeat restored - leaving fail-safe");
            }
        }
        if (nodeConfig.debugESPNOW) {
            Serial.printf("[HB] Hub heartbeat received (time: %u)\n", msg.header.timestamp);
        }
    }
}

void setupNodeBaseCallbacks() {
    // Register hub heartbeat handler so nodes can detect hub liveness
    ESPNowManager::getInstance().onHeartbeatReceived(onHubHeartbeatReceivedInternal);
}

void nodeLoop() {
    // Process ESP-NOW messages
    ESPNowManager::getInstance().processQueue();

    // Update status LED (handles blinking, mode transitions)
    updateStatusLED();

    // Send our heartbeat periodically
    if (millis() - lastHeartbeatSent >= nodeConfig.heartbeatIntervalMs) {
        sendHeartbeat();
    }

    // Check hub heartbeat timeout
    if (lastHubHeartbeatReceived > 0 && nodeConfig.hubHeartbeatTimeoutMs > 0) {
        uint32_t elapsed = millis() - lastHubHeartbeatReceived;
        if (!hubHeartbeatLost && elapsed > nodeConfig.hubHeartbeatTimeoutMs) {
            hubHeartbeatLost = true;
            setStatusLEDMode(StatusLEDMode::FAILSAFE);  // Set LED to failsafe blink
            if (nodeConfig.debugSerial) {
                Serial.printf("[HB] Hub heartbeat missed for %u ms - entering fail-safe\n", elapsed);
            }
            enterFailSafeMode();
        }
    }
}

void sendAnnounce() {
    AnnounceMessage msg = {};
    msg.header.type = MessageType::ANNOUNCE;
    msg.header.tankId = nodeConfig.tankId;
    msg.header.nodeType = nodeConfig.nodeType;
    msg.header.timestamp = millis();
    msg.header.sequenceNum = messageSequence++;
    msg.firmwareVersion = nodeConfig.firmwareVersion;
    msg.capabilities = 0;
    
    uint8_t broadcast[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    ESPNowManager::getInstance().send(broadcast, (uint8_t*)&msg, sizeof(msg));
    
    if (nodeConfig.debugESPNOW) {
        Serial.printf("[TX] ANNOUNCE sent (tankId=%d, FW=v%d, ch=%d)\n",
                      nodeConfig.tankId, nodeConfig.firmwareVersion, nodeConfig.espnowChannel);
        if (nodeConfig.tankId == 0) {
            Serial.println("[WARN] Node is UNMAPPED - waiting for provisioning");
        }
    }
}

void sendStatusAck(const uint8_t* mac, uint8_t commandId, uint8_t statusCode, const uint8_t* data, size_t dataLen) {
    // Trigger command activity LED (blink for 30 seconds)
    triggerCommandActivity();
    
    StatusMessage msg = {};
    msg.header.type = MessageType::STATUS;
    msg.header.tankId = nodeConfig.tankId;
    msg.header.nodeType = nodeConfig.nodeType;
    msg.header.timestamp = millis();
    msg.header.sequenceNum = messageSequence++;
    msg.commandId = commandId;
    msg.statusCode = statusCode;
    
    // Copy status data if provided
    if (data != nullptr && dataLen > 0) {
        size_t copyLen = (dataLen > 32) ? 32 : dataLen;
        memcpy(msg.statusData, data, copyLen);
    }
    
    // CRITICAL FAILSAFE: Always ensure hub is registered as peer before sending
    // Prefer a returnMAC advertised by the hub (hub AP MAC). Fallback to sender MAC.
    uint8_t destMac[6];
    if (nodeConfig.hubReturnMacSet) {
        memcpy(destMac, nodeConfig.hubReturnMac, 6);
    } else {
        memcpy(destMac, mac, 6);
    }

    bool peerExists = false;
#ifdef ESP8266
    // Check if hub exists in peer table
    peerExists = (esp_now_is_peer_exist((uint8_t*)destMac) == 1);

    // ESP8266 FIX: If peer doesn't exist, preemptively clean up peer table
    if (!peerExists) {
        if (nodeConfig.debugESPNOW) {
            Serial.println("[WARN] Hub not in peer table - cleaning up before add");
            Serial.flush();
        }

        // Delete hub MAC if it exists (might be stale)
        esp_now_del_peer((uint8_t*)destMac);

        // Re-add broadcast peer to ensure it's there
        uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        esp_now_del_peer(broadcastMac);  // Remove first
        esp_now_add_peer(broadcastMac, ESP_NOW_ROLE_COMBO, nodeConfig.espnowChannel, NULL, 0);

        if (nodeConfig.debugESPNOW) {
            Serial.println("[INFO] Peer table cleaned, attempting hub add...");
            Serial.flush();
        }
    }
#else
    peerExists = esp_now_is_peer_exist(destMac);
#endif
    
    if (!peerExists) {
        // Try to add hub as peer (prefer AP interface if hubReturnMacSet)
        bool addSuccess = false;
        if (nodeConfig.hubReturnMacSet) {
#ifdef ESP32
            addSuccess = ESPNowManager::getInstance().addPeer(destMac, WIFI_IF_AP);
#else
            addSuccess = ESPNowManager::getInstance().addPeer(destMac);
#endif
        } else {
            addSuccess = ESPNowManager::getInstance().addPeer(destMac);
        }

        if (!addSuccess && nodeConfig.debugESPNOW) {
            Serial.println("[ERR] Hub add still failed after cleanup");
            Serial.flush();
        } else if (addSuccess && nodeConfig.debugESPNOW) {
            Serial.println("[OK] Hub successfully added to peer table");
            Serial.flush();
        }
    }
    
    if (nodeConfig.debugESPNOW) {
        Serial.printf("[TX] STATUS sending to %02X:%02X:%02X:%02X:%02X:%02X (cmdId=%d, status=%d, ch=%d, msgType=%d)\n",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                      commandId, statusCode, nodeConfig.espnowChannel, (uint8_t)msg.header.type);
        Serial.flush();  // Ensure output is sent
        
        // Re-check after potential re-add
#ifdef ESP8266
        int recheck = esp_now_is_peer_exist((uint8_t*)mac);
        peerExists = (recheck == 1);
        Serial.printf("[TX] esp_now_is_peer_exist recheck: %d\n", recheck);
#else
        peerExists = esp_now_is_peer_exist(mac);
#endif
        Serial.printf("[TX] Hub is %s as peer (after check)\n", peerExists ? "REGISTERED" : "NOT REGISTERED");
        Serial.flush();  // Ensure output is sent
    }
    
    bool sendResult = ESPNowManager::getInstance().send(mac, (uint8_t*)&msg, sizeof(msg));
    
    if (nodeConfig.debugESPNOW) {
        Serial.printf("[TX] STATUS send() returned: %s\n", sendResult ? "SUCCESS" : "FAILED");
        Serial.flush();  // Ensure output is sent
        
        if (!sendResult) {
            Serial.println("[ERR] STATUS message failed to send - check peer registration and channel");
            Serial.flush();
        }
    }
}

// ============================================================================
// OTA COMMAND HANDLERS (Common to all nodes)
// ============================================================================

static void resetOtaState() {
    if (otaState.buffer != nullptr) {
        free(otaState.buffer);
        otaState.buffer = nullptr;
    }
    if (otaState.receivedBitset != nullptr) {
        free(otaState.receivedBitset);
        otaState.receivedBitset = nullptr;
    }
    if (otaState.type == OTA_CMD_FIRMWARE_CHUNK && otaState.beginCalled) {
        Update.end(false);  // ESP8266 doesn't have abort(), use end(false)
    }
    memset(&otaState, 0, sizeof(otaState));
}

static void handleOtaBegin(const uint8_t* mac, const CommandMessage& cmd) {
    // Reset any previous OTA state
    resetOtaState();
    
    otaState.type = cmd.commandData[1];  // Firmware or config
    memcpy(&otaState.totalSize, &cmd.commandData[2], 4);
    uint8_t chunkSize = cmd.commandData[6];
    
    otaState.active = true;
    otaState.commandId = cmd.commandId;
    otaState.receivedBytes = 0;
    otaState.expectedChunk = 0;
    otaState.chunksReceived = 0;
    otaState.highestChunkIdx = 0;
    
    // Calculate total expected chunks
    otaState.totalChunks = (otaState.totalSize + chunkSize - 1) / chunkSize;
    
    // Allocate bitset for tracking received chunks (by chunkIndex)
    otaState.receivedBitset = (uint8_t*)malloc(OTA_BITSET_SIZE);
    if (otaState.receivedBitset) {
        memset(otaState.receivedBitset, 0, OTA_BITSET_SIZE);
    } else {
        Serial.println("[OTA] WARN: Could not allocate bitset for chunk tracking");
    }
    
    if (nodeConfig.debugSerial) {
        Serial.printf("[OTA] BEGIN type=0x%02X size=%u chunkSize=%u totalChunks=%u\n", 
                      otaState.type, otaState.totalSize, chunkSize, otaState.totalChunks);
    }
    
    if (otaState.type == OTA_CMD_CONFIG_CHUNK) {
        // Allocate buffer for config file
        otaState.buffer = (uint8_t*)malloc(otaState.totalSize + 1);
        if (otaState.buffer == nullptr) {
            Serial.println("[OTA] ERROR: Failed to allocate config buffer");
            sendStatusAck(mac, cmd.commandId, OTA_STATUS_BEGIN_ERR, nullptr, 0);
            resetOtaState();
            return;
        }
        otaState.bufferSize = otaState.totalSize + 1;
        memset(otaState.buffer, 0, otaState.bufferSize);
        
    } else if (otaState.type == OTA_CMD_FIRMWARE_CHUNK) {
        // Start firmware update
        if (!Update.begin(otaState.totalSize)) {
#ifdef ESP8266
            Serial.printf("[OTA] ERROR: Update.begin failed: %s\n", Update.getErrorString());
#else
            Serial.printf("[OTA] ERROR: Update.begin failed: %s\n", Update.errorString());
#endif
            sendStatusAck(mac, cmd.commandId, OTA_STATUS_BEGIN_ERR, nullptr, 0);
            resetOtaState();
            return;
        }
        otaState.beginCalled = true;
        Serial.println("[OTA] Firmware update started");
    }
    
    sendStatusAck(mac, cmd.commandId, OTA_STATUS_BEGIN_OK, nullptr, 0);
}

// Track when we last requested a BEGIN re-send to avoid spamming
static uint32_t lastBeginRequestTime = 0;
static const uint32_t BEGIN_REQUEST_INTERVAL_MS = 500;  // Don't spam requests

static void handleOtaChunk(const uint8_t* mac, const CommandMessage& cmd) {
    uint8_t chunkType = cmd.commandData[0];  // 0xF1 or 0xC1
    uint16_t chunkIndex;
    memcpy(&chunkIndex, &cmd.commandData[1], 2);
    
    if (!otaState.active) {
        // We're receiving chunks without BEGIN - request hub to re-send BEGIN
        // Only log every 500 chunks to reduce serial flooding
        if (chunkIndex % 500 == 0) {
            Serial.printf("[OTA] WARN: Chunk %u (type 0x%02X) no OTA active\n", 
                          chunkIndex, chunkType);
        }
        
        // Request BEGIN re-send (but don't spam - limit to every 500ms)
        uint32_t now = millis();
        if (now - lastBeginRequestTime > BEGIN_REQUEST_INTERVAL_MS) {
            lastBeginRequestTime = now;
            
            // Send a special status indicating we need BEGIN
            // Use OTA_STATUS_NEED_BEGIN (0x10) to tell hub to re-send BEGIN
            uint8_t requestData[2];
            requestData[0] = chunkType;  // Tell hub what type of BEGIN we need
            requestData[1] = 0;
            Serial.printf("[OTA] Requesting BEGIN re-send for type 0x%02X\n", chunkType);
            sendStatusAck(mac, cmd.commandId, OTA_STATUS_NEED_BEGIN, requestData, 2);
        }
        return;
    }
    
    // chunkIndex already parsed above
    
    // Track this chunk as received by chunkIndex
    otaSetChunkReceived(chunkIndex);
    otaState.chunksReceived++;
    if (chunkIndex > otaState.highestChunkIdx) {
        otaState.highestChunkIdx = chunkIndex;
    }
    
    // Data is in commandData[3..31] (up to 29 bytes)
    const uint8_t* chunkData = &cmd.commandData[3];
    size_t remaining = otaState.totalSize - otaState.receivedBytes;
    size_t chunkDataLen = (remaining > 29) ? 29 : remaining;
    
    if (otaState.type == OTA_CMD_CONFIG_CHUNK) {
        // Append to buffer at correct position based on chunkIndex
        size_t offset = chunkIndex * 29;
        if (offset + chunkDataLen <= otaState.bufferSize - 1) {
            memcpy(otaState.buffer + offset, chunkData, chunkDataLen);
        }
    } else if (otaState.type == OTA_CMD_FIRMWARE_CHUNK) {
        // For firmware, we need sequential writes - track if out of order
        if (chunkIndex != otaState.expectedChunk) {
            Serial.printf("[OTA] WARN: Expected chunk %u, got %u\n", 
                         otaState.expectedChunk, chunkIndex);
        }
        // Write to flash
        size_t written = Update.write((uint8_t*)chunkData, chunkDataLen);
        if (written != chunkDataLen) {
            Serial.printf("[OTA] ERROR: Write failed, expected %u, wrote %u\n", chunkDataLen, written);
            sendStatusAck(mac, cmd.commandId, OTA_STATUS_CHUNK_ERR, nullptr, 0);
            return;
        }
    }
    
    otaState.receivedBytes += chunkDataLen;
    otaState.expectedChunk = chunkIndex + 1;
    
    // Progress logging every 500 chunks
    if (otaState.chunksReceived % 500 == 0 && nodeConfig.debugSerial) {
        Serial.printf("[OTA] Progress: %u/%u bytes (chunk %u/%u, received %u)\n", 
                      otaState.receivedBytes, otaState.totalSize, 
                      chunkIndex, otaState.totalChunks, otaState.chunksReceived);
    }
    
    // Check if this was the last chunk (finalCommand=true from hub)
    // When all chunks received, send READY_END status to request END command
    if (cmd.finalCommand && otaState.chunksReceived >= otaState.totalChunks) {
        Serial.printf("[OTA] All %u chunks received! Sending READY_END status\n", otaState.chunksReceived);
        uint8_t readyData[4];
        readyData[0] = otaState.type;  // Firmware or config
        memcpy(&readyData[1], &otaState.chunksReceived, 2);  // Chunks received
        sendStatusAck(mac, cmd.commandId, OTA_STATUS_READY_END, readyData, 3);
    }
    
    // Don't send per-chunk ACK to improve throughput
    // sendStatusAck(mac, cmd.commandId, OTA_STATUS_CHUNK_OK, nullptr, 0);
}

static void handleOtaEnd(const uint8_t* mac, const CommandMessage& cmd) {
    if (!otaState.active) {
        if (nodeConfig.debugSerial) {
            Serial.println("[OTA] WARN: END received but no OTA active");
        }
        return;
    }
    
    uint8_t otaType = cmd.commandData[1];
    
    if (nodeConfig.debugSerial) {
        Serial.printf("[OTA] END type=0x%02X received=%u total=%u finalCmd=%d\n", 
                      otaType, otaState.receivedBytes, otaState.totalSize, 
                      cmd.finalCommand);
        Serial.printf("[OTA] Chunks received: %u/%u, highestChunkIdx: %u\n",
                      otaState.chunksReceived, otaState.totalChunks, otaState.highestChunkIdx);
    }
    
    // Check for missing chunks using chunkIndex tracking (0 to totalChunks-1)
    bool hasMissing = false;
    uint16_t missingCount = 0;
    
    Serial.printf("[OTA] Checking for missing chunks (expected: 0 to %u)...\n", otaState.totalChunks - 1);
    
    for (uint16_t i = 0; i < otaState.totalChunks && i < OTA_MAX_CHUNKS; i++) {
        if (!otaIsChunkReceived(i)) {
            if (!hasMissing) {
                Serial.println("[OTA] MISSING CHUNKS:");
                hasMissing = true;
            }
            // Print first 20 missing chunks individually, then summarize
            if (missingCount < 20) {
                Serial.printf("[OTA]   Missing chunkIndex: %u\n", i);
            }
            missingCount++;
        }
    }
    
    if (hasMissing) {
        Serial.printf("[OTA] ERROR: Total missing chunks: %u out of %u\n", missingCount, otaState.totalChunks);
        Serial.printf("[OTA] Transfer FAILED due to packet loss\n");
        sendStatusAck(mac, cmd.commandId, OTA_STATUS_APPLY_ERR, nullptr, 0);
        resetOtaState();
        return;
    }
    
    Serial.printf("[OTA] All %u chunks received successfully!\n", otaState.totalChunks);
    
    if (otaType == OTA_CMD_CONFIG_CHUNK && otaState.type == OTA_CMD_CONFIG_CHUNK) {
        // Write config to filesystem
        otaState.buffer[otaState.receivedBytes] = '\0';  // Null terminate
        
        // Ensure filesystem is mounted
        if (!LittleFS.begin()) {
            Serial.println("[OTA] ERROR: LittleFS mount failed");
            sendStatusAck(mac, cmd.commandId, OTA_STATUS_APPLY_ERR, nullptr, 0);
            resetOtaState();
            return;
        }
        
        File file = LittleFS.open("/node_config.txt", "w");
        if (!file) {
            Serial.println("[OTA] ERROR: Failed to open config file for writing");
            sendStatusAck(mac, cmd.commandId, OTA_STATUS_APPLY_ERR, nullptr, 0);
            resetOtaState();
            return;
        }
        
        file.write(otaState.buffer, otaState.receivedBytes);
        file.close();
        
        Serial.println("[OTA] Config file saved successfully");
        sendStatusAck(mac, cmd.commandId, OTA_STATUS_APPLY_OK, nullptr, 0);
        resetOtaState();
        
        // Reload configuration
        Serial.println("[OTA] Reloading configuration...");
        loadNodeConfiguration(nodeConfig.nodeType, nodeConfig.nodeName.c_str());
        
    } else if (otaType == OTA_CMD_FIRMWARE_CHUNK && otaState.type == OTA_CMD_FIRMWARE_CHUNK) {
        // Finish firmware update
        if (!Update.end(true)) {
#ifdef ESP8266
            Serial.printf("[OTA] ERROR: Update.end failed: %s\n", Update.getErrorString());
#else
            Serial.printf("[OTA] ERROR: Update.end failed: %s\n", Update.errorString());
#endif
            sendStatusAck(mac, cmd.commandId, OTA_STATUS_APPLY_ERR, nullptr, 0);
            resetOtaState();
            return;
        }
        
        if (!Update.isFinished()) {
            Serial.println("[OTA] ERROR: Update not finished");
            sendStatusAck(mac, cmd.commandId, OTA_STATUS_APPLY_ERR, nullptr, 0);
            resetOtaState();
            return;
        }
        
        Serial.println("[OTA] Firmware update successful! Rebooting in 2 seconds...");
        sendStatusAck(mac, cmd.commandId, OTA_STATUS_APPLY_OK, nullptr, 0);
        resetOtaState();
        
        delay(2000);
        ESP.restart();
    }
}

// Called by ESPNowManager when a command is received
// This function checks for OTA commands and handles them
bool handleOtaCommand(const uint8_t* mac, const CommandMessage& cmd) {
    uint8_t cmdType = cmd.commandData[0];
    
    // Only log BEGIN and END commands to avoid OOM (chunks = 11,000+)
    if (cmdType == OTA_CMD_OTA_BEGIN || cmdType == OTA_CMD_OTA_END) {
        Serial.printf("[OTA] cmd=0x%02X seqID=%u final=%d\n", 
                      cmdType, cmd.commandSeqID, cmd.finalCommand);
    }
    
    switch (cmdType) {
        case OTA_CMD_OTA_BEGIN:
            Serial.println("[OTA] BEGIN");
            handleOtaBegin(mac, cmd);
            return true;
            
        case OTA_CMD_CONFIG_CHUNK:
        case OTA_CMD_FIRMWARE_CHUNK:
            handleOtaChunk(mac, cmd);
            return true;
            
        case OTA_CMD_OTA_END:
            Serial.println("[OTA] END");
            handleOtaEnd(mac, cmd);
            return true;
            
        default:
            return false;  // Not an OTA command
    }
}

// ============================================================================
// PERSISTENT PIN STATE MANAGEMENT
// ============================================================================
// Persists digital pin states to JSON file for power failure recovery.
// When the node restarts, it reads the file and restores pin states.
// ============================================================================

#include <ArduinoJson.h>

// Structure to track pin states
struct TrackedPin {
    uint8_t pin;
    uint8_t state;  // HIGH or LOW
    char name[16];  // Optional human-readable name
    bool active;    // Is this slot in use?
};

static TrackedPin trackedPins[MAX_TRACKED_PINS];
static bool pinPersistenceInitialized = false;
static uint32_t lastPinStateSave = 0;
static bool pinStatesDirty = false;

// Debounce saves - don't save more often than this (ms)
#define PIN_SAVE_DEBOUNCE_MS 500

void initPinStatePersistence() {
    // Clear tracked pins array
    memset(trackedPins, 0, sizeof(trackedPins));
    for (int i = 0; i < MAX_TRACKED_PINS; i++) {
        trackedPins[i].active = false;
    }
    pinPersistenceInitialized = true;
    pinStatesDirty = false;
    
    if (nodeConfig.debugSerial) {
        Serial.println("[PIN] Pin state persistence initialized");
    }
}

bool registerPersistentPin(uint8_t pin, const char* name) {
    if (!pinPersistenceInitialized) {
        initPinStatePersistence();
    }
    
    // Check if already registered
    for (int i = 0; i < MAX_TRACKED_PINS; i++) {
        if (trackedPins[i].active && trackedPins[i].pin == pin) {
            if (nodeConfig.debugSerial) {
                Serial.printf("[PIN] Pin %d already registered\n", pin);
            }
            return true;  // Already registered
        }
    }
    
    // Find empty slot
    for (int i = 0; i < MAX_TRACKED_PINS; i++) {
        if (!trackedPins[i].active) {
            trackedPins[i].pin = pin;
            trackedPins[i].state = LOW;  // Default to LOW
            trackedPins[i].active = true;
            if (name) {
                strncpy(trackedPins[i].name, name, sizeof(trackedPins[i].name) - 1);
                trackedPins[i].name[sizeof(trackedPins[i].name) - 1] = '\0';
            } else {
                snprintf(trackedPins[i].name, sizeof(trackedPins[i].name), "pin%d", pin);
            }
            
            if (nodeConfig.debugSerial) {
                Serial.printf("[PIN] Registered pin %d as '%s'\n", pin, trackedPins[i].name);
            }
            return true;
        }
    }
    
    Serial.printf("[PIN] ERROR: Cannot register pin %d - max pins reached\n", pin);
    return false;
}

void savePinStates() {
    if (!pinPersistenceInitialized) return;
    
    // Create JSON document (ArduinoJson v7 API)
    JsonDocument doc;
    JsonArray pins = doc["pins"].to<JsonArray>();
    
    for (int i = 0; i < MAX_TRACKED_PINS; i++) {
        if (trackedPins[i].active) {
            JsonObject pinObj = pins.add<JsonObject>();
            pinObj["pin"] = trackedPins[i].pin;
            pinObj["state"] = trackedPins[i].state;
            pinObj["name"] = trackedPins[i].name;
        }
    }
    
    doc["savedAt"] = millis();
    
    // Write to file
    File file = LittleFS.open(PIN_STATE_FILE, "w");
    if (!file) {
        Serial.println("[PIN] ERROR: Failed to open pin state file for writing");
        return;
    }
    
    serializeJson(doc, file);
    file.close();
    
    pinStatesDirty = false;
    lastPinStateSave = millis();
    
    if (nodeConfig.debugSerial) {
        Serial.print("[PIN] Saved pin states: ");
        serializeJson(doc, Serial);
        Serial.println();
    }
}

int restorePinStates() {
    if (!pinPersistenceInitialized) {
        initPinStatePersistence();
    }
    
    // Check if file exists
    if (!LittleFS.exists(PIN_STATE_FILE)) {
        if (nodeConfig.debugSerial) {
            Serial.println("[PIN] No saved pin states found");
        }
        return 0;
    }
    
    File file = LittleFS.open(PIN_STATE_FILE, "r");
    if (!file) {
        Serial.println("[PIN] ERROR: Failed to open pin state file");
        return 0;
    }
    
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    
    if (error) {
        Serial.printf("[PIN] ERROR: Failed to parse pin state file: %s\n", error.c_str());
        return 0;
    }
    
    int restored = 0;
    JsonArray pins = doc["pins"];
    
    for (JsonObject pinObj : pins) {
        uint8_t pin = pinObj["pin"];
        uint8_t state = pinObj["state"];
        const char* name = pinObj["name"];
        
        // Register the pin if not already registered
        registerPersistentPin(pin, name);
        
        // Find the pin in our tracking array and restore its state
        for (int i = 0; i < MAX_TRACKED_PINS; i++) {
            if (trackedPins[i].active && trackedPins[i].pin == pin) {
                trackedPins[i].state = state;
                // Apply the state to the physical pin
                digitalWrite(pin, state);
                restored++;
                
                if (nodeConfig.debugSerial) {
                    Serial.printf("[PIN] Restored pin %d (%s) to %s\n", 
                                  pin, trackedPins[i].name, state == HIGH ? "HIGH" : "LOW");
                }
                break;
            }
        }
    }
    
    if (nodeConfig.debugSerial) {
        Serial.printf("[PIN] Restored %d pin states from file\n", restored);
    }
    
    return restored;
}

void persistentDigitalWrite(uint8_t pin, uint8_t value) {
    // Always perform the actual digitalWrite first
    digitalWrite(pin, value);
    
    if (!pinPersistenceInitialized) {
        // Persistence not initialized - just do regular digitalWrite
        return;
    }
    
    // Find the pin in our tracking array
    bool found = false;
    for (int i = 0; i < MAX_TRACKED_PINS; i++) {
        if (trackedPins[i].active && trackedPins[i].pin == pin) {
            // Only save if state actually changed
            if (trackedPins[i].state != value) {
                trackedPins[i].state = value;
                pinStatesDirty = true;
                
                if (nodeConfig.debugSerial) {
                    Serial.printf("[PIN] Pin %d (%s) changed to %s\n", 
                                  pin, trackedPins[i].name, value == HIGH ? "HIGH" : "LOW");
                }
                
                // Debounced save - save immediately if enough time has passed
                uint32_t now = millis();
                if (now - lastPinStateSave >= PIN_SAVE_DEBOUNCE_MS) {
                    savePinStates();
                }
            }
            found = true;
            break;
        }
    }
    
    if (!found && nodeConfig.debugSerial) {
        // Pin not registered - this is fine, just won't be persisted
        // Don't spam the log for every untracked pin
    }
}

int getPersistedPinState(uint8_t pin) {
    if (!pinPersistenceInitialized) return -1;
    
    for (int i = 0; i < MAX_TRACKED_PINS; i++) {
        if (trackedPins[i].active && trackedPins[i].pin == pin) {
            return trackedPins[i].state;
        }
    }
    return -1;  // Pin not tracked
}

// Call this periodically to flush any pending saves
void flushPinStateIfDirty() {
    if (pinStatesDirty && (millis() - lastPinStateSave >= PIN_SAVE_DEBOUNCE_MS)) {
        savePinStates();
    }
}
