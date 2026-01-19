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
// Hub heartbeat tracking (node-side)
uint32_t lastHubHeartbeatReceived = 0;
bool hubHeartbeatLost = false;

// Forward declaration for internal handler
static void onHubHeartbeatReceivedInternal(const uint8_t* mac, const HeartbeatMessage& msg);
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
            // Recovered
            hubHeartbeatLost = false;
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

    // Send our heartbeat periodically
    if (millis() - lastHeartbeatSent >= nodeConfig.heartbeatIntervalMs) {
        sendHeartbeat();
    }

    // Check hub heartbeat timeout
    if (lastHubHeartbeatReceived > 0 && nodeConfig.hubHeartbeatTimeoutMs > 0) {
        uint32_t elapsed = millis() - lastHubHeartbeatReceived;
        if (!hubHeartbeatLost && elapsed > nodeConfig.hubHeartbeatTimeoutMs) {
            hubHeartbeatLost = true;
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
