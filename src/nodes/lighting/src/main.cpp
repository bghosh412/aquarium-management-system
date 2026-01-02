#include <Arduino.h>
#ifdef ESP8266
    #include <ESP8266WiFi.h>
    #include <LittleFS.h>
#else
    #include <WiFi.h>
    #include <LITTLEFS.h>
    #define LittleFS LITTLEFS
#endif
#include "protocol/messages.h"
#include "ESPNowManager.h"

// ============================================================================
// LIGHTING NODE - Controls aquarium lighting
// ============================================================================
// Hardware: PWM LED channels for different spectrum control
// Fail-safe: Hold last state or gradually dim to off
// ============================================================================

// Configuration structure
struct NodeConfig {
    uint8_t tankId;
    String nodeName;
    uint8_t firmwareVersion;
    uint8_t espnowChannel;
    bool debugSerial;
    bool debugESPNOW;
    bool debugHardware;
    uint32_t announceIntervalMs;
    uint32_t heartbeatIntervalMs;
    uint32_t connectionTimeoutMs;
};

NodeConfig config;

// Global state variables
uint32_t lastHeartbeatSent = 0;
uint8_t messageSequence = 0;

// Hardware pins (adjust for your setup)
#define PIN_LED_WHITE D1
#define PIN_LED_BLUE D2
#define PIN_LED_RED D3

// Lighting state
struct LightingState {
    uint8_t whiteLevel;  // 0-255
    uint8_t blueLevel;   // 0-255
    uint8_t redLevel;    // 0-255
    bool enabled;
} lightState = {0, 0, 0, false};

// ============================================================================
// CONFIGURATION LOADER
// ============================================================================

void loadConfiguration() {
    // Set defaults
    config.tankId = 1;
    config.nodeName = "LightingNode01";
    config.firmwareVersion = 1;
    config.espnowChannel = 6;
    config.debugSerial = true;
    config.debugESPNOW = true;
    config.debugHardware = false;
    config.announceIntervalMs = 5000;
    config.heartbeatIntervalMs = 30000;
    config.connectionTimeoutMs = 90000;
    
    // Load from filesystem if available
    if (!LittleFS.begin()) {
        Serial.println("⚠️  LittleFS mount failed, using defaults");
        return;
    }
    
    if (!LittleFS.exists("/node_config.txt")) {
        Serial.println("⚠️  Config file not found, using defaults");
        return;
    }
    
    File file = LittleFS.open("/node_config.txt", "r");
    if (!file) {
        Serial.println("❌ Failed to open config file");
        return;
    }
    
    Serial.println("📄 Loading configuration...");
    
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
        if (key == "NODE_TANK_ID") {
            config.tankId = value.toInt();
        } else if (key == "NODE_NAME") {
            config.nodeName = value;
        } else if (key == "FIRMWARE_VERSION") {
            config.firmwareVersion = value.toInt();
        } else if (key == "ESPNOW_CHANNEL") {
            config.espnowChannel = value.toInt();
        } else if (key == "DEBUG_SERIAL") {
            config.debugSerial = (value == "true");
        } else if (key == "DEBUG_ESPNOW") {
            config.debugESPNOW = (value == "true");
        } else if (key == "DEBUG_HARDWARE") {
            config.debugHardware = (value == "true");
        } else if (key == "ANNOUNCE_INTERVAL_MS") {
            config.announceIntervalMs = value.toInt();
        } else if (key == "HEARTBEAT_INTERVAL_MS") {
            config.heartbeatIntervalMs = value.toInt();
        } else if (key == "CONNECTION_TIMEOUT_MS") {
            config.connectionTimeoutMs = value.toInt();
        }
    }
    
    file.close();
    
    Serial.println("✅ Configuration loaded");
    Serial.printf("   - Node: %s (Tank %d)\n", config.nodeName.c_str(), config.tankId);
    Serial.printf("   - FW Version: v%d\n", config.firmwareVersion);
    Serial.printf("   - ESP-NOW Channel: %d\n", config.espnowChannel);
    Serial.printf("   - Debug: Serial=%s | ESP-NOW=%s | Hardware=%s\n",
                  config.debugSerial ? "ON" : "OFF",
                  config.debugESPNOW ? "ON" : "OFF",
                  config.debugHardware ? "ON" : "OFF");
}

// ============================================================================
// Hardware Implementation
// ============================================================================

void setupHardware() {
    pinMode(PIN_LED_WHITE, OUTPUT);
    pinMode(PIN_LED_BLUE, OUTPUT);
    pinMode(PIN_LED_RED, OUTPUT);
    
    // Start with lights off
    analogWrite(PIN_LED_WHITE, 0);
    analogWrite(PIN_LED_BLUE, 0);
    analogWrite(PIN_LED_RED, 0);
    
    if (config.debugSerial) {
        Serial.println("✓ Lighting hardware initialized");
    }
}

void enterFailSafeMode() {
    if (config.debugSerial) {
        Serial.println("⚠️ FAIL-SAFE: Holding last lighting state (safe for lights)");
    }
    // Lights can safely maintain last state or gradually dim
    // Optionally: slowly fade to off
    lightState.enabled = false;
}

void onCommandReceived(const uint8_t* mac, const uint8_t* data, size_t len) {
    if (config.debugESPNOW) {
        Serial.println("╔════════════════════════════════════════════════════════╗");
        Serial.printf("║ 📥 COMMAND received (%d bytes)\n", len);
        Serial.printf("║ From: %02X:%02X:%02X:%02X:%02X:%02X\n",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    
    // Parse as raw command data (already reassembled by ESPNowManager)
    if (len < 1) {
        if (config.debugESPNOW) {
            Serial.println("║ ❌ Command too short");
            Serial.println("╚════════════════════════════════════════════════════════╝");
        }
        return;
    }
    
    uint8_t commandType = data[0];
    
    if (config.debugESPNOW) {
        Serial.printf("║ Command Type: %d\n", commandType);
    }
    
    bool success = true;
    
    switch (commandType) {
        case 0: // All 3 channels OFF
            lightState.whiteLevel = 0;
            lightState.blueLevel = 0;
            lightState.redLevel = 0;
            lightState.enabled = false;
            if (config.debugESPNOW) {
                Serial.println("║ ✓ All channels OFF");
            }
            break;
            
        case 1: // All 3 channels ON (to last known levels or max)
            if (len >= 4) {
                // Use provided levels
                lightState.whiteLevel = data[1];
                lightState.blueLevel = data[2];
                lightState.redLevel = data[3];
            } else {
                // Default to max if no levels provided
                lightState.whiteLevel = 255;
                lightState.blueLevel = 255;
                lightState.redLevel = 255;
            }
            lightState.enabled = true;
            if (config.debugESPNOW) {
                Serial.printf("║ ✓ All channels ON: W=%d B=%d R=%d\n",
                              lightState.whiteLevel, lightState.blueLevel, lightState.redLevel);
            }
            break;
            
        case 10: // Channel 1 (White) OFF
            lightState.whiteLevel = 0;
            if (config.debugESPNOW) {
                Serial.println("║ ✓ Channel 1 (White) OFF");
            }
            break;
            
        case 11: // Channel 1 (White) ON
            if (len >= 2) {
                lightState.whiteLevel = data[1];
            } else {
                lightState.whiteLevel = 255;  // Default to max
            }
            if (config.debugESPNOW) {
                Serial.printf("║ ✓ Channel 1 (White) ON: %d\n", lightState.whiteLevel);
            }
            break;
            
        case 20: // Channel 2 (Blue) OFF
            lightState.blueLevel = 0;
            if (config.debugESPNOW) {
                Serial.println("║ ✓ Channel 2 (Blue) OFF");
            }
            break;
            
        case 21: // Channel 2 (Blue) ON
            if (len >= 2) {
                lightState.blueLevel = data[1];
            } else {
                lightState.blueLevel = 255;  // Default to max
            }
            if (config.debugESPNOW) {
                Serial.printf("║ ✓ Channel 2 (Blue) ON: %d\n", lightState.blueLevel);
            }
            break;
            
        case 30: // Channel 3 (Red) OFF
            lightState.redLevel = 0;
            if (config.debugESPNOW) {
                Serial.println("║ ✓ Channel 3 (Red) OFF");
            }
            break;
            
        case 31: // Channel 3 (Red) ON
            if (len >= 2) {
                lightState.redLevel = data[1];
            } else {
                lightState.redLevel = 255;  // Default to max
            }
            if (config.debugESPNOW) {
                Serial.printf("║ ✓ Channel 3 (Red) ON: %d\n", lightState.redLevel);
            }
            break;
            
        default:
            if (config.debugESPNOW) {
                Serial.printf("║ ❌ Unknown command type: %d\n", commandType);
            }
            success = false;
            break;
    }
    
    if (config.debugESPNOW) {
        Serial.println("╚════════════════════════════════════════════════════════╝");
    }
    
    // Send STATUS acknowledgment (placeholder structure for future use)
    StatusMessage status = {};
    status.header.type = MessageType::STATUS;
    status.header.tankId = config.tankId;
    status.header.nodeType = NodeType::LIGHT;
    status.header.timestamp = millis();
    status.header.sequenceNum = messageSequence++;
    status.commandId = commandType;  // Echo command type as command ID
    status.statusCode = success ? 0 : 1;
    
    // Pack current state into status data (placeholder format)
    // Byte 0: Channel 1 level (0-255)
    // Byte 1: Channel 2 level (0-255)
    // Byte 2: Channel 3 level (0-255)
    // Byte 3: Enabled (0=off, 1=on)
    status.statusData[0] = lightState.whiteLevel;   // Channel 1
    status.statusData[1] = lightState.blueLevel;    // Channel 2
    status.statusData[2] = lightState.redLevel;     // Channel 3
    status.statusData[3] = lightState.enabled ? 1 : 0;
    
    ESPNowManager::getInstance().send(mac, (uint8_t*)&status, sizeof(status));
    
    if (config.debugESPNOW) {
        Serial.printf("📤 STATUS sent (code=%d)\n\n", status.statusCode);
    }
}

void updateHardware() {
    // Apply lighting levels
    if (lightState.enabled) {
        analogWrite(PIN_LED_WHITE, lightState.whiteLevel);
        analogWrite(PIN_LED_BLUE, lightState.blueLevel);
        analogWrite(PIN_LED_RED, lightState.redLevel);
    } else {
        analogWrite(PIN_LED_WHITE, 0);
        analogWrite(PIN_LED_BLUE, 0);
        analogWrite(PIN_LED_RED, 0);
    }
    
    // Debug output periodically
    if (config.debugHardware) {
        static unsigned long lastDebug = 0;
        if (millis() - lastDebug > 5000) {
            lastDebug = millis();
            Serial.printf("💡 Light State: %s | W=%d B=%d R=%d\n",
                          lightState.enabled ? "ON" : "OFF",
                          lightState.whiteLevel, lightState.blueLevel, lightState.redLevel);
        }
    }
}

// ============================================================================
// ESP-NOW CALLBACKS
// ============================================================================

void onAckReceived(const uint8_t* mac, const AckMessage& msg) {
    if (config.debugESPNOW) {
        Serial.println("╔════════════════════════════════════════════════════════╗");
        Serial.printf("║ ✅ ACK received from %02X:%02X:%02X:%02X:%02X:%02X\n",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        Serial.printf("║ Assigned Node ID: %d\n", msg.assignedNodeId);
        Serial.printf("║ Accepted: %s\n", msg.accepted ? "YES" : "NO");
        Serial.println("╚════════════════════════════════════════════════════════╝");
    }
    
    // Add hub as peer
    ESPNowManager::getInstance().addPeer(mac);
    
    if (config.debugSerial) {
        Serial.println("✅ Connected to hub - ready for commands\n");
    }
}

void onConfigReceived(const uint8_t* mac, const ConfigMessage& msg) {
    if (config.debugESPNOW) {
        Serial.println("╔════════════════════════════════════════════════════════╗");
        Serial.printf("║ ⚙️  CONFIG received from %02X:%02X:%02X:%02X:%02X:%02X\n",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        Serial.printf("║ Assigned Tank ID: %d\n", msg.header.tankId);
        Serial.printf("║ Device Name: %s\n", msg.deviceName);
        Serial.println("╚════════════════════════════════════════════════════════╝");
    }
    
    // Update runtime configuration
    config.tankId = msg.header.tankId;
    config.nodeName = String(msg.deviceName);
    
    // Save to LittleFS for persistence
    File file = LittleFS.open("/node_config.txt", "w");
    if (file) {
        file.printf("# Lighting Node Configuration (Provisioned)\n");
        file.printf("# Last updated: %lu ms\n\n", millis());
        file.printf("NODE_TANK_ID=%d\n", config.tankId);
        file.printf("NODE_NAME=%s\n", config.nodeName.c_str());
        file.printf("NODE_FIRMWARE_VERSION=%d\n", config.firmwareVersion);
        file.printf("ESPNOW_CHANNEL=%d\n", config.espnowChannel);
        file.printf("DEBUG_SERIAL=%s\n", config.debugSerial ? "true" : "false");
        file.printf("DEBUG_ESPNOW=%s\n", config.debugESPNOW ? "true" : "false");
        file.printf("DEBUG_HARDWARE=%s\n", config.debugHardware ? "true" : "false");
        file.printf("ANNOUNCE_INTERVAL_MS=%u\n", config.announceIntervalMs);
        file.printf("HEARTBEAT_INTERVAL_MS=%u\n", config.heartbeatIntervalMs);
        file.printf("CONNECTION_TIMEOUT_MS=%u\n", config.connectionTimeoutMs);
        file.close();
        
        Serial.println("✅ Configuration saved to /node_config.txt");
    } else {
        Serial.println("❌ Failed to save configuration to file");
    }
    
    // Send STATUS acknowledgment
    StatusMessage statusMsg = {};
    statusMsg.header.type = MessageType::STATUS;
    statusMsg.header.tankId = config.tankId;
    statusMsg.header.nodeType = NodeType::LIGHT;
    statusMsg.header.timestamp = millis();
    statusMsg.header.sequenceNum = messageSequence++;
    statusMsg.commandId = 0;  // CONFIG doesn't have commandId
    statusMsg.statusCode = 0x00;  // SUCCESS
    
    ESPNowManager::getInstance().send(mac, (uint8_t*)&statusMsg, sizeof(statusMsg));
    
    Serial.printf("✅ Node provisioned: Tank %d, Name '%s'\n", config.tankId, config.nodeName.c_str());
    Serial.println("🔄 Restarting in 2 seconds to apply configuration...\n");
    
    delay(2000);
    ESP.restart();
}

void sendHeartbeat() {
    HeartbeatMessage msg = {};
    msg.header.type = MessageType::HEARTBEAT;
    msg.header.tankId = config.tankId;
    msg.header.nodeType = NodeType::LIGHT;
    msg.header.timestamp = millis();
    msg.header.sequenceNum = messageSequence++;
    msg.health = 100;
    msg.uptimeMinutes = millis() / 60000;
    
    // Broadcast to hub
    uint8_t broadcast[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    ESPNowManager::getInstance().send(broadcast, (uint8_t*)&msg, sizeof(msg));
    
    if (config.debugESPNOW) {
        Serial.printf("💓 Heartbeat sent (uptime: %dmin)\n", msg.uptimeMinutes);
    }
}

// ============================================================================
// Arduino Entry Points
// ============================================================================

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n\n");
    Serial.println("╔═══════════════════════════════════════════════════════════╗");
    Serial.println("║          LIGHTING NODE - Aquarium Management              ║");
    Serial.println("╚═══════════════════════════════════════════════════════════╝");
    
    // Load configuration from filesystem
    loadConfiguration();
    
    Serial.printf("Tank ID: %d | Node: %s | FW: v%d\n\n", 
                  config.tankId, config.nodeName.c_str(), config.firmwareVersion);
    
    // Initialize hardware
    setupHardware();
    
    // Initialize ESPNowManager
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    Serial.println("📡 Initializing ESPNowManager...");
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    
    bool success = ESPNowManager::getInstance().begin(config.espnowChannel, false);
    
    if (!success) {
        Serial.println("❌ ESPNowManager initialization failed!");
        Serial.println("⚠️  Entering fail-safe mode");
        enterFailSafeMode();
        while(1) delay(1000);
    }
    
    // Register callbacks
    ESPNowManager::getInstance().onAckReceived(onAckReceived);
    ESPNowManager::getInstance().onCommandReceived(onCommandReceived);
    ESPNowManager::getInstance().onConfigReceived(onConfigReceived);
    
    Serial.println("✅ ESPNowManager ready");
    Serial.printf("   - Channel: %d\n", config.espnowChannel);
    Serial.printf("   - Mode: NODE (std::queue for ESP8266)\n");
    Serial.printf("   - Debug ESP-NOW: %s\n", config.debugESPNOW ? "ON" : "OFF");
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    
    // Send initial ANNOUNCE (unmapped if tankId=0)
    AnnounceMessage announce = {};
    announce.header.type = MessageType::ANNOUNCE;
    announce.header.tankId = config.tankId;  // 0 = unmapped, >0 = provisioned
    announce.header.nodeType = NodeType::LIGHT;
    announce.header.timestamp = millis();
    announce.header.sequenceNum = messageSequence++;
    announce.firmwareVersion = config.firmwareVersion;
    announce.capabilities = 0;
    
    uint8_t broadcast[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    ESPNowManager::getInstance().send(broadcast, (uint8_t*)&announce, sizeof(announce));
    
    if (config.debugSerial) {
        Serial.printf("📡 ANNOUNCE sent (tankId=%d, FW=v%d)\n", config.tankId, config.firmwareVersion);
        if (config.tankId == 0) {
            Serial.println("⚠️  Node is UNMAPPED - waiting for provisioning from hub");
        }
    }
    
    Serial.println("\n✓ Lighting node ready\n");
    lastHeartbeatSent = millis();
}

void loop() {
    // Process ESP-NOW messages
    ESPNowManager::getInstance().processQueue();
    
    // Update hardware (apply lighting state to pins)
    updateHardware();
    
    // Send periodic heartbeat
    if (millis() - lastHeartbeatSent >= config.heartbeatIntervalMs) {
        sendHeartbeat();
        lastHeartbeatSent = millis();
    }
    
    // Print statistics periodically
    if (config.debugESPNOW) {
        static unsigned long lastStatsTime = 0;
        if (millis() - lastStatsTime > 60000) {  // Every 60 seconds
            lastStatsTime = millis();
            auto stats = ESPNowManager::getInstance().getStatistics();
            Serial.println("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
            Serial.println("📊 ESP-NOW Statistics (Last 60s):");
            Serial.printf("   Messages: %u sent / %u received\n", 
                          stats.messagesSent, stats.messagesReceived);
            Serial.printf("   Fragments: %u sent / %u received\n",
                          stats.fragmentsSent, stats.fragmentsReceived);
            Serial.printf("   Errors: %u send failures / %u reassembly timeouts\n",
                          stats.sendFailures, stats.reassemblyTimeouts);
            Serial.printf("   Duplicates ignored: %u\n", stats.duplicatesIgnored);
            Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
        }
    }
    
    delay(10);
}
