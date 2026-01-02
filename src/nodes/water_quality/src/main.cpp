#include "node_base.h"

// ============================================================================
// WATER QUALITY SENSOR NODE - Multi-sensor monitoring
// ============================================================================
// Hardware: pH sensor, TDS sensor, temperature sensor
// Fail-safe: Continue reading (sensors are read-only, no safety risk)
// ============================================================================

// Node configuration
const uint8_t NODE_TANK_ID = 1;              //  CONFIGURE PER DEPLOYMENT
const NodeType NODE_TYPE = NodeType::SENSOR;
const char* NODE_NAME = "WaterQualityNode01"; //  CONFIGURE PER DEPLOYMENT
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
#define PIN_PH_SENSOR A0
#define PIN_TDS_SENSOR A1
#define PIN_TEMP_SENSOR D1

// Sensor state
struct SensorData {
    float pH;
    float tds;           // Total dissolved solids (ppm)
    float temperature;   // C
    uint32_t lastReadTime;
} sensorData = {7.0, 0.0, 25.0, 0};

const uint32_t SENSOR_READ_INTERVAL_MS = 5000;  // Read every 5 seconds

// ============================================================================
// Hardware Implementation
// ============================================================================

void setupHardware() {
    pinMode(PIN_PH_SENSOR, INPUT);
    pinMode(PIN_TDS_SENSOR, INPUT);
    pinMode(PIN_TEMP_SENSOR, INPUT);
    
    Serial.println(" Water quality sensors initialized");
}

void enterFailSafeMode() {
    Serial.println(" FAIL-SAFE: Continuing sensor readings (read-only, safe)");
    // Sensors can continue operating safely
}

void handleCommand(const CommandMessage* msg) {
    Serial.printf("  Command ID: %d\n", msg->commandId);
    
    switch (msg->commandId) {
        case 1: // Request immediate sensor reading
            readSensors();
            sendSensorData();
            break;
            
        case 2: // Calibrate pH sensor
            Serial.println("  pH calibration requested (TODO: implement)");
            break;
            
        default:
            Serial.printf("  Unknown command ID: %d\n", msg->commandId);
            break;
    }
}

void readSensors() {
    // TODO: Implement actual sensor reading
    // For now, use dummy values
    
    // pH reading (typical range 6.0-8.5 for aquariums)
    int phRaw = analogRead(PIN_PH_SENSOR);
    sensorData.pH = map(phRaw, 0, 1023, 4.0 * 100, 10.0 * 100) / 100.0;
    
    // TDS reading (typical range 100-500 ppm)
    int tdsRaw = analogRead(PIN_TDS_SENSOR);
    sensorData.tds = map(tdsRaw, 0, 1023, 0, 1000);
    
    // Temperature reading
    // sensorData.temperature = readDS18B20();
    
    sensorData.lastReadTime = millis();
}

void sendSensorData() {
    // Pack sensor data into status message
    uint8_t sensorDataPayload[32] = {0};
    
    // Pack data: [pH_int, pH_frac, TDS_low, TDS_high, Temp_int, Temp_frac, ...]
    sensorDataPayload[0] = (uint8_t)sensorData.pH;  // Integer part
    sensorDataPayload[1] = (uint8_t)((sensorData.pH - (int)sensorData.pH) * 100);  // Fractional
    
    uint16_t tdsInt = (uint16_t)sensorData.tds;
    sensorDataPayload[2] = tdsInt & 0xFF;        // TDS low byte
    sensorDataPayload[3] = (tdsInt >> 8) & 0xFF; // TDS high byte
    
    sensorDataPayload[4] = (uint8_t)sensorData.temperature;
    sensorDataPayload[5] = (uint8_t)((sensorData.temperature - (int)sensorData.temperature) * 100);
    
    // Send as STATUS message (commandId=0 means unsolicited sensor reading)
    sendStatus(0, 0, sensorDataPayload, 6);
    
    Serial.printf(" Sensors: pH=%.2f, TDS=%.0f ppm, Temp=%.1fC\n", 
                 sensorData.pH, sensorData.tds, sensorData.temperature);
}

void updateHardware() {
    // Periodic sensor readings
    if (millis() - sensorData.lastReadTime > SENSOR_READ_INTERVAL_MS) {
        readSensors();
        
        // Send data to hub if connected
        if (currentState == NodeState::CONNECTED) {
            sendSensorData();
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
    Serial.println("      WATER QUALITY NODE - Aquarium Management             ");
    Serial.println("");
    Serial.printf("Tank ID: %d | Node: %s\n\n", NODE_TANK_ID, NODE_NAME);
    
    setupHardware();
    setupESPNow();
    
    currentState = NodeState::ANNOUNCING;
    Serial.println(" Water quality node ready\n");
}

void loop() {
    nodeLoop();  // Handle ESP-NOW communication
    updateHardware();  // Read and report sensors
    delay(100);
}
