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
#include "node_base.h"

// ============================================================================
// LIGHTING NODE - Controls aquarium lighting (3 channels ON/OFF)
// ============================================================================
// Hardware: 3 digital outputs for LED control (White, Blue, Red)
// Fail-safe: Hold last state (safe for lights)
// Pin Configuration: D1 (White), D2 (Blue), D5 (Red)
// ============================================================================

// Hardware pins (per COMMAND_REFERENCE.md)
#define PIN_LED_CHANNEL1 D1  // GPIO 12
#define PIN_LED_CHANNEL2 D2  // GPIO 13
#define PIN_LED_CHANNEL3 D5  // GPIO 15

// Lighting state (ON/OFF only, no PWM)
struct LightingState {
    bool channel1On;
    bool channel2On;
    bool channel3On;
} lightState = {false, false, false};

// ============================================================================
// HARDWARE IMPLEMENTATION (Required by NodeBase)
// ============================================================================

void setupHardware() {
    pinMode(PIN_LED_CHANNEL1, OUTPUT);
    pinMode(PIN_LED_CHANNEL2, OUTPUT);
    pinMode(PIN_LED_CHANNEL3, OUTPUT);
    
    // Start with lights off
    digitalWrite(PIN_LED_CHANNEL1, LOW);
    digitalWrite(PIN_LED_CHANNEL2, LOW);
    digitalWrite(PIN_LED_CHANNEL3, LOW);
    
    if (nodeConfig.debugSerial) {
        Serial.println("[OK] Lighting hardware initialized (D6/D7/D8)");
    }
}

void enterFailSafeMode() {
    if (nodeConfig.debugSerial) {
        Serial.println("[WARN] FAIL-SAFE: Hub heartbeat lost - turning OFF all lights for safety");
    }
    // Force all channels OFF
    lightState.channel1On = false;
    lightState.channel2On = false;
    lightState.channel3On = false;

    // Apply immediately
    updateHardware();

    if (nodeConfig.debugESPNOW) {
        Serial.println("[OK] Fail-safe applied: ch1=0 ch2=0 ch3=0");
    }
}

static void readPinStatus(uint8_t* statusData) {
    statusData[0] = (digitalRead(PIN_LED_CHANNEL1) == HIGH) ? 1 : 0;
    statusData[1] = (digitalRead(PIN_LED_CHANNEL2) == HIGH) ? 1 : 0;
    statusData[2] = (digitalRead(PIN_LED_CHANNEL3) == HIGH) ? 1 : 0;
}

void handleCommand(const uint8_t* mac, const uint8_t* data, size_t len) {
    if (len == 0 || data == nullptr) return;
    uint8_t commandType = data[0];
    
    if (nodeConfig.debugESPNOW) {
        Serial.println("+========================================================+");
        Serial.printf("| [CMD] COMMAND received\n");
        Serial.printf("| Tank ID: %d\n", nodeConfig.tankId);
        Serial.printf("| Command Type: %d\n", commandType);
    }
    
    bool success = true;
    
    switch (commandType) {
        case 0: // All channels OFF
            lightState.channel1On = false;
            lightState.channel2On = false;
            lightState.channel3On = false;
            if (nodeConfig.debugESPNOW) {
                Serial.println("| [OK] All channels OFF");
            }
            break;
            
        case 1: // All channels ON
            lightState.channel1On = true;
            lightState.channel2On = true;
            lightState.channel3On = true;
            if (nodeConfig.debugESPNOW) {
                Serial.println("| [OK] All channels ON");
            }
            break;
            
        case 10: // Channel1 OFF
            lightState.channel1On = false;
            if (nodeConfig.debugESPNOW) {
                Serial.println("| [OK] Channel1 OFF");
            }
            break;
            
        case 11: // Channel1 ON
            lightState.channel1On = true;
            if (nodeConfig.debugESPNOW) {
                Serial.println("| [OK] Channel1 ON");
            }
            break;
            
        case 20: // Channel2 OFF
            lightState.channel2On = false;
            if (nodeConfig.debugESPNOW) {
                Serial.println("| [OK] Channel2 OFF");
            }
            break;
            
        case 21: // Channel2 ON
            lightState.channel2On = true;
            if (nodeConfig.debugESPNOW) {
                Serial.println("| [OK] Channel2 ON");
            }
            break;
            
        case 30: // Channel3 OFF
            lightState.channel3On = false;
            if (nodeConfig.debugESPNOW) {
                Serial.println("| [OK] Channel3 OFF");
            }
            break;
            
        case 31: // Channel3 ON
            lightState.channel3On = true;
            if (nodeConfig.debugESPNOW) {
                Serial.println("| [OK] Channel3 ON");
            }
            break;
            
        case 40: // Status request
            if (nodeConfig.debugESPNOW) {
                Serial.println("| [OK] Status request");
            }
            break;

        case 0xFF: // TEST: Force fail-safe
            if (nodeConfig.debugSerial) {
                Serial.println("[TEST] Force-failsafe command received - forcing fail-safe");
            }
            enterFailSafeMode();
            break;

        default:
            success = false;
            if (nodeConfig.debugESPNOW) {
                Serial.printf("| [ERROR] Unknown command type: %d\n", commandType);
            }
            break;
    }
    
    if (nodeConfig.debugESPNOW) {
        Serial.println("+========================================================+");
    }
    
    // Send acknowledgment
    // Apply state immediately so status reflects actual pins
    updateHardware();

    uint8_t statusCode = success ? 0x00 : 0xFF;
    uint8_t statusData[3] = {0, 0, 0};
    readPinStatus(statusData);
    if (nodeConfig.debugESPNOW) {
        Serial.printf("| [STATUS] Pin states: ch1=%d ch2=%d ch3=%d\n",
                      statusData[0], statusData[1], statusData[2]);
    }

    // Send status directly to hub MAC (commandId not available in data-only callback)
    sendStatusAck(mac, 0, statusCode, statusData, 3);
}

void updateHardware() {
    // Apply lighting state to hardware pins
    digitalWrite(PIN_LED_CHANNEL1, lightState.channel1On ? HIGH : LOW);
    digitalWrite(PIN_LED_CHANNEL2, lightState.channel2On ? HIGH : LOW);
    digitalWrite(PIN_LED_CHANNEL3, lightState.channel3On ? HIGH : LOW);
}

// ============================================================================
// ESPNOW CALLBACK WRAPPERS (Match ESPNowManager signatures)
// ============================================================================

void onAckReceivedWrapper(const uint8_t* mac, const AckMessage& msg) {
    onAckReceived(mac, msg);
}

void onConfigReceivedWrapper(const uint8_t* mac, const ConfigMessage& msg) {
    onConfigReceived(mac, msg);
}

void onUnmapReceivedWrapper(const uint8_t* mac, const UnmapMessage& msg) {
    onUnmapReceived(mac, msg);
}

void onCommandReceivedWrapper(const uint8_t* mac, const uint8_t* data, size_t len) {
    handleCommand(mac, data, len);
}

// ============================================================================
// Arduino Entry Points
// ============================================================================

void setup() {
    Serial.begin(115200);
    delay(2000);
    
    Serial.println("\n\n\n");
    Serial.println("================================");
    Serial.println("ESP8266 BOOT - Serial Working!");
    Serial.println("================================");
    
    Serial.println("\n\n");
    Serial.println("+===========================================================+");
    Serial.println("|          LIGHTING NODE - Aquarium Management              |");
    Serial.println("+===========================================================+");
    
    // Load configuration using NodeBase
    Serial.println("[1] Loading configuration...");
    loadNodeConfiguration(NodeType::LIGHT, "UnmappedLight");
    Serial.println("[1] Configuration loaded OK");
    
    Serial.printf("Tank ID: %d | Node: %s | FW: v%d\n\n", 
                  nodeConfig.tankId, nodeConfig.nodeName.c_str(), nodeConfig.firmwareVersion);
    
    // Initialize hardware
    Serial.println("[2] Initializing hardware...");
    setupHardware();
    Serial.println("[2] Hardware initialized OK");
    
    // Initialize ESPNowManager
    Serial.println("[3] Starting ESP-NOW initialization...");
    bool success = ESPNowManager::getInstance().begin(nodeConfig.espnowChannel, false);
    Serial.printf("[3] ESP-NOW init returned: %s\n", success ? "SUCCESS" : "FAILED");
    
    if (!success) {
        Serial.println("[ERROR] ESPNowManager initialization failed!");
        Serial.println("[WARN]  Entering fail-safe mode");
        enterFailSafeMode();
        while(1) delay(1000);
    }
    
    // Register callbacks
    ESPNowManager::getInstance().onAckReceived(onAckReceivedWrapper);
    ESPNowManager::getInstance().onCommandReceived(onCommandReceivedWrapper);
    ESPNowManager::getInstance().onConfigReceived(onConfigReceivedWrapper);
    ESPNowManager::getInstance().onUnmapReceived(onUnmapReceivedWrapper);
    
    // Register OTA handler (common to all nodes via NodeBase)
    ESPNowManager::getInstance().onRawCommandReceived(handleOtaCommand);

    // Register base node callbacks (hub heartbeat handler, etc.)
    setupNodeBaseCallbacks();
    
    Serial.println("[OK] ESPNowManager ready");
    Serial.printf("   - Channel: %d\n", nodeConfig.espnowChannel);
    Serial.printf("   - Mode: NODE (std::queue for ESP8266)\n");
    Serial.printf("   - Debug ESP-NOW: %s\n", nodeConfig.debugESPNOW ? "ON" : "OFF");
    
    // Send initial ANNOUNCE using NodeBase
    Serial.println("[4] Sending initial ANNOUNCE...");
    sendAnnounce();
    
    Serial.println("\n[OK] Lighting node ready\n");
    lastHeartbeatSent = millis();
}

void loop() {
    // Delegate processing, heartbeats, and hub-liveness checks to NodeBase
    nodeLoop();
    
    // Update hardware state
    updateHardware();
    
    // Heartbeat is handled by NodeBase::nodeLoop() - no action required here
    
    // Send periodic ANNOUNCE for discovery (only when unmapped)
    static unsigned long lastAnnounce = 0;
    if (!isConnectedToHub && (millis() - lastAnnounce >= nodeConfig.announceIntervalMs)) {
        lastAnnounce = millis();
        sendAnnounce();
    }
    
    // Print memory status every 60 seconds
    static unsigned long lastMemoryPrint = 0;
    if (millis() - lastMemoryPrint >= 60000) {
        lastMemoryPrint = millis();
        Serial.printf("[HEARTBEAT] Free heap: %u bytes\n", ESP.getFreeHeap());
    }
    
    // Print ESP-NOW statistics
    if (nodeConfig.debugESPNOW) {
        static unsigned long lastStatsTime = 0;
        if (millis() - lastStatsTime > 60000) {
            lastStatsTime = millis();
            auto stats = ESPNowManager::getInstance().getStatistics();
            Serial.println("\n-----------------------------------------");
            Serial.println("[STATS] ESP-NOW Statistics (Last 60s):");
            Serial.printf("   Messages: %u sent / %u received\n", 
                          stats.messagesSent, stats.messagesReceived);
            Serial.printf("   Fragments: %u sent / %u received\n",
                          stats.fragmentsSent, stats.fragmentsReceived);
            Serial.printf("   Errors: %u send failures / %u reassembly timeouts\n",
                          stats.sendFailures, stats.reassemblyTimeouts);
            Serial.printf("   Duplicates ignored: %u\n", stats.duplicatesIgnored);
            Serial.println("-----------------------------------------\n");
        }
    }


    
    delay(10);
}
