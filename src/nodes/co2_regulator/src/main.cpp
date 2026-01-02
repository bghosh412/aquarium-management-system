#include "node_base.h"

// ============================================================================
// CO2 REGULATOR NODE - Controls CO2 injection
// ============================================================================
// Hardware: Solenoid valve for CO2 control
// Fail-safe: TURN OFF CO2 (critical safety requirement)
// ============================================================================

// Node configuration
const uint8_t NODE_TANK_ID = 1;              // ⚠️ CONFIGURE PER DEPLOYMENT
const NodeType NODE_TYPE = NodeType::CO2;
const char* NODE_NAME = "CO2RegulatorNode01"; // ⚠️ CONFIGURE PER DEPLOYMENT
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
#define PIN_CO2_SOLENOID D1

// CO2 state
struct CO2State {
    bool solenoidOpen;
    uint32_t onDurationMs;    // How long to keep open
    uint32_t onStartTime;     // When it was opened
} co2State = {false, 0, 0};

// ============================================================================
// Hardware Implementation
// ============================================================================

void setupHardware() {
    pinMode(PIN_CO2_SOLENOID, OUTPUT);
    digitalWrite(PIN_CO2_SOLENOID, LOW);  // Solenoid closed (CO2 OFF)
    
    Serial.println("✓ CO2 hardware initialized - SOLENOID CLOSED");
}

void enterFailSafeMode() {
    Serial.println("⚠️ FAIL-SAFE: CLOSING CO2 SOLENOID");
    digitalWrite(PIN_CO2_SOLENOID, LOW);
    co2State.solenoidOpen = false;
    co2State.onDurationMs = 0;
}

void handleCommand(const CommandMessage* msg) {
    Serial.printf("  Command ID: %d\n", msg->commandId);
    
    switch (msg->commandId) {
        case 1: // Open solenoid for duration
            {
                uint16_t durationSec = (msg->commandData[0] << 8) | msg->commandData[1];
                if (durationSec > 0 && durationSec <= 3600) {  // Max 1 hour
                    co2State.solenoidOpen = true;
                    co2State.onDurationMs = durationSec * 1000;
                    co2State.onStartTime = millis();
                    Serial.printf("  CO2 ON for %d seconds\n", durationSec);
                }
            }
            break;
            
        case 2: // Close solenoid immediately
            co2State.solenoidOpen = false;
            co2State.onDurationMs = 0;
            Serial.println("  CO2 OFF");
            break;
            
        default:
            Serial.printf("  Unknown command ID: %d\n", msg->commandId);
            break;
    }
}

void updateHardware() {
    // Check if timed duration expired
    if (co2State.solenoidOpen && co2State.onDurationMs > 0) {
        if (millis() - co2State.onStartTime >= co2State.onDurationMs) {
            co2State.solenoidOpen = false;
            co2State.onDurationMs = 0;
            Serial.println("  CO2 duration expired - closing solenoid");
        }
    }
    
    // Apply solenoid state
    digitalWrite(PIN_CO2_SOLENOID, co2State.solenoidOpen ? HIGH : LOW);
}

// ============================================================================
// Arduino Entry Points
// ============================================================================

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n\n");
    Serial.println("╔═══════════════════════════════════════════════════════════╗");
    Serial.println("║        CO2 REGULATOR NODE - Aquarium Management           ║");
    Serial.println("╚═══════════════════════════════════════════════════════════╝");
    Serial.printf("Tank ID: %d | Node: %s\n\n", NODE_TANK_ID, NODE_NAME);
    
    setupHardware();
    setupESPNow();
    
    currentState = NodeState::ANNOUNCING;
    Serial.println("✓ CO2 regulator node ready\n");
}

void loop() {
    nodeLoop();  // Handle ESP-NOW communication
    updateHardware();  // Update CO2 solenoid
    delay(100);
}
