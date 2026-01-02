#include "node_base.h"

// ============================================================================
// FISH FEEDER NODE - Automated fish feeding
// ============================================================================
// Hardware: Servo motor for feeder mechanism
// Fail-safe: Do nothing (safe - missing one feeding is better than overfeeding)
// ============================================================================

// Node configuration
const uint8_t NODE_TANK_ID = 1;              //  CONFIGURE PER DEPLOYMENT
const NodeType NODE_TYPE = NodeType::FISH_FEEDER;
const char* NODE_NAME = "FishFeederNode01";   //  CONFIGURE PER DEPLOYMENT
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
#define PIN_SERVO D1

// Feeder state
struct FeederState {
    bool feedInProgress;
    uint32_t feedStartTime;
    uint8_t portionCount;
} feederState = {false, 0, 0};

// ============================================================================
// Hardware Implementation
// ============================================================================

void setupHardware() {
    pinMode(PIN_SERVO, OUTPUT);
    // TODO: Initialize servo library
    // servo.attach(PIN_SERVO);
    // servo.write(0);  // Home position
    
    Serial.println(" Feeder hardware initialized");
}

void enterFailSafeMode() {
    Serial.println(" FAIL-SAFE: Feeder disabled (safe - skip feeding)");
    feederState.feedInProgress = false;
    // Better to miss one feeding than to overfeed
}

void handleCommand(const CommandMessage* msg) {
    Serial.printf("  Command ID: %d\n", msg->commandId);
    
    switch (msg->commandId) {
        case 1: // Dispense food
            if (!feederState.feedInProgress) {
                feederState.portionCount = msg->commandData[0];
                if (feederState.portionCount == 0) feederState.portionCount = 1;
                if (feederState.portionCount > 5) feederState.portionCount = 5;  // Safety limit
                
                feederState.feedInProgress = true;
                feederState.feedStartTime = millis();
                Serial.printf("  Feeding %d portions\n", feederState.portionCount);
            } else {
                Serial.println("  Feeding already in progress");
            }
            break;
            
        default:
            Serial.printf("  Unknown command ID: %d\n", msg->commandId);
            break;
    }
}

void updateHardware() {
    if (feederState.feedInProgress) {
        // TODO: Implement servo-based feeding sequence
        // For now, just simulate with a timeout
        if (millis() - feederState.feedStartTime > 3000) {  // 3 seconds per feeding
            feederState.feedInProgress = false;
            Serial.println("  Feeding complete");
        }
    }
}

// ============================================================================
// Arduino Entry Points
// ============================================================================

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n\n");
    Serial.println("");
    Serial.println("        FISH FEEDER NODE - Aquarium Management             ");
    Serial.println("");
    Serial.printf("Tank ID: %d | Node: %s\n\n", NODE_TANK_ID, NODE_NAME);
    
    setupHardware();
    setupESPNow();
    
    currentState = NodeState::ANNOUNCING;
    Serial.println(" Fish feeder node ready\n");
}

void loop() {
    nodeLoop();  // Handle ESP-NOW communication
    updateHardware();  // Update feeder mechanism
    delay(100);
}
