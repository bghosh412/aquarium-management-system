#include "node_base.h"
#include "ESPNowManager.h"

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
    nodeConfig.espnowChannel = 6;
    #endif
    nodeConfig.debugSerial = true;
    nodeConfig.debugESPNOW = true;
    nodeConfig.debugHardware = false;
    nodeConfig.announceIntervalMs = 5000;
    nodeConfig.heartbeatIntervalMs = 30000;
    nodeConfig.connectionTimeoutMs = 90000;
    
    // Attempt to load from filesystem
    if (!LittleFS.begin()) {
        Serial.println("[WARN] LittleFS mount failed, using defaults");
        return;
    }
    
    if (!LittleFS.exists("/node_config.txt")) {
        Serial.println("[WARN] Config file not found, using defaults");
        return;
    }
    
    File file = LittleFS.open("/node_config.txt", "r");
    if (!file) {
        Serial.println("[ERROR] Failed to open config file");
        return;
    }
    
    Serial.println("[FILE] Loading configuration...");
    
    while (file.available()) {
        String line = file.readStringUntil('\\n');
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
        } else if (key == "ANNOUNCE_INTERVAL_MS") {
            nodeConfig.announceIntervalMs = value.toInt();
        } else if (key == "HEARTBEAT_INTERVAL_MS") {
            nodeConfig.heartbeatIntervalMs = value.toInt();
        } else if (key == "CONNECTION_TIMEOUT_MS") {
            nodeConfig.connectionTimeoutMs = value.toInt();
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
        Serial.printf("| Assigned Node ID: %d\\n", msg.assignedNodeId);
        Serial.printf("| Accepted: %s\\n", msg.accepted ? "YES" : "NO");
        Serial.println("+========================================================+");
    }
    
    // Add hub as peer
    bool peerAdded = ESPNowManager::getInstance().addPeer(mac);
    
    // Mark as connected to hub
    if (msg.accepted && peerAdded) {
        isConnectedToHub = true;
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
    
    if (nodeConfig.debugESPNOW) {
        Serial.printf("[HB] Heartbeat sent (uptime: %dmin, time: %u)\n", msg.uptimeMinutes, currentUnixTime);
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
        Serial.printf("[TX] ANNOUNCE sent (tankId=%d, FW=v%d)\\n", nodeConfig.tankId, nodeConfig.firmwareVersion);
        if (nodeConfig.tankId == 0) {
            Serial.println("[WARN] Node is UNMAPPED - waiting for provisioning");
        }
    }
}

void sendStatusAck(const uint8_t* mac, uint8_t commandId, uint8_t statusCode, const uint8_t* data, size_t dataLen) {
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
    
    ESPNowManager::getInstance().send(mac, (uint8_t*)&msg, sizeof(msg));
    
    if (nodeConfig.debugESPNOW) {
        Serial.printf("[TX] STATUS sent (cmdId=%d, status=%d)\n", commandId, statusCode);
    }
}
