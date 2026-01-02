#include "node_base.h"

// ============================================================================
// HEATER NODE - Temperature control
// ============================================================================
// Hardware: Relay for heater control, temperature sensor
// Fail-safe: TURN OFF HEATER (critical safety requirement)
// ============================================================================

// Node configuration
const uint8_t NODE_TANK_ID = 1;              // ⚠️ CONFIGURE PER DEPLOYMENT
const NodeType NODE_TYPE = NodeType::HEATER;
const char* NODE_NAME = "HeaterNode01";       // ⚠️ CONFIGURE PER DEPLOYMENT
const uint8_t FIRMWARE_VERSION = 1;

// Global state variables
NodeState currentState = NodeState::INITIALIZING;
uint8_t hubMacAddress[6] = {0};
bool hubDiscovered = false;
uint32_t lastHeartbeatSent = 0;
uint32_t lastHeartbeatReceived = 0;
uint32_t announceAttempts = 0;
uint8_t messageSequence = 0;

// Hardware pins
#define PIN_HEATER_RELAY D1
#define PIN_TEMP_SENSOR D2  // For DS18B20 or similar

// Heater state
struct HeaterState {
    bool heaterOn;
    float currentTemp;
    float targetTemp;
    bool autoMode;
} heaterState = {false, 0.0, 25.0, false};

// ============================================================================
// Hardware Implementation
// ============================================================================

void setupHardware() {
    pinMode(PIN_HEATER_RELAY, OUTPUT);
    digitalWrite(PIN_HEATER_RELAY, LOW);  // Heater OFF
    
    // TODO: Initialize temperature sensor
    // oneWire.begin(PIN_TEMP_SENSOR);
    
    Serial.println("✓ Heater hardware initialized - HEATER OFF");
}

void enterFailSafeMode() {
    Serial.println("⚠️ FAIL-SAFE: TURNING OFF HEATER");
    digitalWrite(PIN_HEATER_RELAY, LOW);
    heaterState.heaterOn = false;
    heaterState.autoMode = false;
}

void handleCommand(const CommandMessage* msg) {
    Serial.printf("  Command ID: %d\n", msg->commandId);
    
    switch (msg->commandId) {
        case 1: // Set target temperature
            {
                float temp = *((float*)msg->commandData);
                if (temp >= 18.0 && temp <= 32.0) {  // Reasonable aquarium range
                    heaterState.targetTemp = temp;
                    Serial.printf("  Target temp set to: %.1f°C\n", temp);
                }
            }
            break;
            
        case 2: // Enable auto mode
            heaterState.autoMode = msg->commandData[0] != 0;
            Serial.printf("  Auto mode: %s\n", heaterState.autoMode ? "ON" : "OFF");
            break;
            
        case 3: // Manual heater control
            if (!heaterState.autoMode) {
                heaterState.heaterOn = msg->commandData[0] != 0;
                Serial.printf("  Manual heater: %s\n", heaterState.heaterOn ? "ON" : "OFF");
            }
            break;
            
        default:
            Serial.printf("  Unknown command ID: %d\n", msg->commandId);
            break;
    }
}

void updateHardware() {
    // TODO: Read temperature sensor
    // heaterState.currentTemp = readTemperature();
    
    // Auto temperature control
    if (heaterState.autoMode) {
        if (heaterState.currentTemp < heaterState.targetTemp - 0.5) {
            heaterState.heaterOn = true;
        } else if (heaterState.currentTemp > heaterState.targetTemp + 0.5) {
            heaterState.heaterOn = false;
        }
    }
    
    // Apply heater state (only when connected to hub for safety)
    if (currentState == NodeState::CONNECTED) {
        digitalWrite(PIN_HEATER_RELAY, heaterState.heaterOn ? HIGH : LOW);
    } else {
        digitalWrite(PIN_HEATER_RELAY, LOW);  // Safety: OFF when disconnected
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
    Serial.println("║          HEATER NODE - Aquarium Management                ║");
    Serial.println("╚═══════════════════════════════════════════════════════════╝");
    Serial.printf("Tank ID: %d | Node: %s\n\n", NODE_TANK_ID, NODE_NAME);
    
    setupHardware();
    setupESPNow();
    
    currentState = NodeState::ANNOUNCING;
    Serial.println("✓ Heater node ready\n");
}

void loop() {
    nodeLoop();  // Handle ESP-NOW communication
    updateHardware();  // Update heater control
    delay(100);
}
