# Node Instructions - Common Patterns for ESP8266 Nodes

**Base patterns and architecture for all peripheral nodes in the Aquarium Management System.**

---

## 📋 Overview

All nodes in this system are **specialized ESP8266 devices** that control a single subsystem. Nodes do NOT make global decisions—they execute commands from the hub and report status.

### Current Node Types

- **Fish Feeder** - Servo-based feeding mechanism
- **CO₂ Regulator** - Solenoid valve control for CO₂ injection
- **Lighting** - 3-channel PWM LED control (white/blue/red)
- **Heater** - Temperature management with relay and sensor
- **Water Quality** - pH/TDS/temperature monitoring
- **Repeater** - ESP-NOW range extender (passive relay)

---

## 🏗️ Node Architecture

### Hardware Platform

- **MCU**: ESP8266 (ESP-12E or similar)
- **Framework**: Arduino Core for ESP8266
- **Build System**: PlatformIO
- **Communication**: ESP-NOW only (no Wi-Fi, no MQTT)

### Software Architecture

**Single-threaded, non-blocking, state-machine driven.**

```
┌─────────────────────────────────────────┐
│ ESP8266 Node Architecture               │
├─────────────────────────────────────────┤
│                                         │
│  ┌───────────────────────────────────┐ │
│  │  setup()                          │ │
│  │  ├─ Serial.begin()                │ │
│  │  ├─ setupHardware()               │ │
│  │  └─ setupESPNow()                 │ │
│  └───────────────────────────────────┘ │
│                ↓                        │
│  ┌───────────────────────────────────┐ │
│  │  loop() - Non-blocking            │ │
│  │  ├─ nodeLoop()    ← NodeBase      │ │
│  │  │   ├─ Process ESP-NOW messages │ │
│  │  │   ├─ Handle heartbeat timer   │ │
│  │  │   └─ Check watchdog timeout   │ │
│  │  │                                │ │
│  │  └─ updateHardware() ← Your code  │ │
│  │      ├─ Read sensors              │ │
│  │      ├─ Update actuators          │ │
│  │      └─ State machine logic       │ │
│  └───────────────────────────────────┘ │
│                                         │
└─────────────────────────────────────────┘
```

---

## 📚 NodeBase Library

All nodes use the **NodeBase library** (`lib/NodeBase/`) for communication and common functionality.

### NodeBase API

#### Initialization

```cpp
void setupESPNow(uint8_t tankId, NodeType nodeType);
```

Call once in `setup()` to initialize ESP-NOW communication.

#### Message Handling

```cpp
void nodeLoop();
```

Call in `loop()` to process incoming messages, handle heartbeats, and check timeouts.

#### Status Reporting

```cpp
void sendStatus(uint8_t commandId, uint8_t statusCode, 
                const uint8_t* data, size_t dataLen);
```

Send status updates to hub. Automatically includes:
- Message header with timestamp and sequence number
- Command ID for acknowledgment tracking
- Status code (STATUS_OK, STATUS_ERROR, etc.)
- Optional payload data

#### Callback Registration

```cpp
// Automatically called when messages arrive
extern void handleCommand(const CommandMessage& cmd);
```

Your code must implement this function to process commands from the hub.

---

## 🔧 Required Functions (Your Implementation)

Every node **must** implement these four functions:

### 1. setupHardware()

**Purpose**: Initialize GPIO pins, sensors, actuators, and device-specific hardware.

```cpp
void setupHardware() {
    // Initialize pins
    pinMode(PIN_RELAY, OUTPUT);
    pinMode(PIN_SENSOR, INPUT);
    
    // Set safe default states
    digitalWrite(PIN_RELAY, LOW);
    
    // Initialize libraries (e.g., Servo, DHT, etc.)
    servo.attach(PIN_SERVO);
    
    // Calibrate sensors if needed
    sensor.begin();
}
```

**Requirements**:
- Set all pins to safe default states
- Initialize hardware libraries
- Do NOT use blocking delays
- Log initialization status

---

### 2. enterFailSafeMode()

**Purpose**: Put device into safe state when communication is lost or error occurs.

```cpp
void enterFailSafeMode() {
    // Device-specific safe behavior
    // Examples:
    // - Lights: Hold last state (safe)
    // - CO₂: Turn OFF (critical)
    // - Heater: Turn OFF (critical)
    // - Feeder: Do nothing (safer to skip)
    
    digitalWrite(PIN_ACTUATOR, LOW);  // Turn off
    currentState = STATE_SAFE_MODE;
    Serial.println("FAIL-SAFE MODE ACTIVATED");
}
```

**Fail-Safe Philosophy** (see table below):

| Node Type | Fail-Safe Action | Reason |
|-----------|------------------|--------|
| Lights | Hold last state | Safe to maintain lighting |
| CO₂ | **OFF** | Critical - prevent CO₂ overdose |
| Heater | **OFF** | Critical - prevent overheating |
| Feeder | Do nothing | Safer to skip feeding |
| Sensors | Continue reading | Passive monitoring |
| Repeater | Continue forwarding | Passive relay |

**Requirements**:
- Must be **safe** for fish and equipment
- Should be **deterministic** (same result every time)
- No blocking operations
- Log fail-safe activation

---

### 3. handleCommand(const CommandMessage& cmd)

**Purpose**: Process commands sent by the hub.

```cpp
void handleCommand(const CommandMessage& cmd) {
    uint8_t cmdType = cmd.commandData[0];
    
    switch(cmdType) {
        case CMD_SET_LEVEL: {
            uint8_t level = cmd.commandData[1];
            
            // Validate input
            if (level > MAX_LEVEL) {
                sendStatus(cmd.commandId, STATUS_ERROR, nullptr, 0);
                return;
            }
            
            // Apply command
            targetLevel = level;
            currentState = STATE_TRANSITIONING;
            
            // Acknowledge
            sendStatus(cmd.commandId, STATUS_OK, nullptr, 0);
            break;
        }
        
        case CMD_EMERGENCY_STOP: {
            enterFailSafeMode();
            sendStatus(cmd.commandId, STATUS_OK, nullptr, 0);
            break;
        }
        
        default:
            // Unknown command
            sendStatus(cmd.commandId, STATUS_ERROR, nullptr, 0);
            break;
    }
}
```

**Requirements**:
- Validate all input parameters
- Always send acknowledgment via `sendStatus()`
- Update state variables, NOT hardware directly
- Let `updateHardware()` apply changes
- Handle unknown commands gracefully

---

### 4. updateHardware()

**Purpose**: Non-blocking hardware updates based on current state.

```cpp
void updateHardware() {
    static unsigned long lastUpdate = 0;
    unsigned long now = millis();
    
    // Rate limiting
    if (now - lastUpdate < UPDATE_INTERVAL) {
        return;
    }
    lastUpdate = now;
    
    // State machine
    switch(currentState) {
        case STATE_IDLE:
            // Do nothing
            break;
            
        case STATE_TRANSITIONING:
            // Gradual change (e.g., PWM fade)
            if (currentLevel < targetLevel) {
                currentLevel++;
                analogWrite(PIN_PWM, currentLevel);
            } else if (currentLevel > targetLevel) {
                currentLevel--;
                analogWrite(PIN_PWM, currentLevel);
            } else {
                currentState = STATE_IDLE;
            }
            break;
            
        case STATE_SAFE_MODE:
            // Maintain safe state
            digitalWrite(PIN_ACTUATOR, LOW);
            break;
    }
}
```

**Requirements**:
- **NO blocking delays** (use `millis()` for timing)
- Implement as state machine
- Rate-limit updates (e.g., 50ms-100ms intervals)
- Keep execution time short (<10ms per call)

---

## 📝 Node Implementation Template

Use this as a starting point for new nodes:

```cpp
#include <Arduino.h>
#include "node_base.h"

// ===== Hardware Configuration =====
#define PIN_ACTUATOR 5
#define PIN_SENSOR 4

// ===== Node Configuration =====
const uint8_t TANK_ID = 1;                    // Set via config or build flag
const NodeType NODE_TYPE = NodeType::XXXX;   // Your node type

// ===== State Machine =====
enum State {
    STATE_IDLE,
    STATE_ACTIVE,
    STATE_TRANSITIONING,
    STATE_SAFE_MODE
};

State currentState = STATE_IDLE;

// ===== Hardware State =====
uint8_t currentLevel = 0;
uint8_t targetLevel = 0;

// ===== Command Types =====
enum CommandType : uint8_t {
    CMD_SET_LEVEL = 0x01,
    CMD_READ_SENSOR = 0x02,
    CMD_EMERGENCY_STOP = 0xFF
};

// ===== Status Codes =====
enum StatusCode : uint8_t {
    STATUS_OK = 0x00,
    STATUS_ERROR = 0x01,
    STATUS_BUSY = 0x02
};

// ===== Required Functions =====

void setupHardware() {
    pinMode(PIN_ACTUATOR, OUTPUT);
    pinMode(PIN_SENSOR, INPUT);
    digitalWrite(PIN_ACTUATOR, LOW);  // Safe default
    
    Serial.println("Hardware initialized");
}

void enterFailSafeMode() {
    digitalWrite(PIN_ACTUATOR, LOW);  // Device-specific safe state
    currentState = STATE_SAFE_MODE;
    targetLevel = 0;
    currentLevel = 0;
    
    Serial.println("FAIL-SAFE MODE");
}

void handleCommand(const CommandMessage& cmd) {
    uint8_t cmdType = cmd.commandData[0];
    
    switch(cmdType) {
        case CMD_SET_LEVEL: {
            uint8_t level = cmd.commandData[1];
            
            if (level > 100) {
                sendStatus(cmd.commandId, STATUS_ERROR, nullptr, 0);
                return;
            }
            
            targetLevel = level;
            currentState = STATE_TRANSITIONING;
            sendStatus(cmd.commandId, STATUS_OK, nullptr, 0);
            break;
        }
        
        case CMD_READ_SENSOR: {
            uint16_t sensorValue = analogRead(PIN_SENSOR);
            uint8_t data[2] = { 
                (uint8_t)(sensorValue >> 8), 
                (uint8_t)(sensorValue & 0xFF) 
            };
            sendStatus(cmd.commandId, STATUS_OK, data, 2);
            break;
        }
        
        case CMD_EMERGENCY_STOP: {
            enterFailSafeMode();
            sendStatus(cmd.commandId, STATUS_OK, nullptr, 0);
            break;
        }
        
        default:
            sendStatus(cmd.commandId, STATUS_ERROR, nullptr, 0);
            break;
    }
}

void updateHardware() {
    static unsigned long lastUpdate = 0;
    unsigned long now = millis();
    
    if (now - lastUpdate < 50) {  // 50ms rate limit
        return;
    }
    lastUpdate = now;
    
    switch(currentState) {
        case STATE_IDLE:
            // Nothing to do
            break;
            
        case STATE_TRANSITIONING:
            // Smooth transition
            if (currentLevel < targetLevel) {
                currentLevel++;
            } else if (currentLevel > targetLevel) {
                currentLevel--;
            } else {
                currentState = STATE_IDLE;
            }
            
            analogWrite(PIN_ACTUATOR, map(currentLevel, 0, 100, 0, 255));
            break;
            
        case STATE_SAFE_MODE:
            digitalWrite(PIN_ACTUATOR, LOW);
            break;
    }
}

// ===== Arduino Entry Points =====

void setup() {
    Serial.begin(115200);
    Serial.println("\n=== Node Starting ===");
    
    setupHardware();
    setupESPNow(TANK_ID, NODE_TYPE);
    
    Serial.println("Node ready");
}

void loop() {
    nodeLoop();        // Process ESP-NOW messages
    updateHardware();  // Update hardware state
}
```

---

## ⚙️ Non-Blocking Code Requirements

### ❌ NEVER Use Blocking Delays

```cpp
// ❌ BAD - Blocks entire system
void setBrightness(uint8_t level) {
    for (int i = 0; i < level; i++) {
        analogWrite(PIN_LED, i);
        delay(10);  // ❌ BLOCKS FOR 10ms
    }
}
```

### ✅ Use State Machines with millis()

```cpp
// ✅ GOOD - Non-blocking
void updateBrightness() {
    static unsigned long lastUpdate = 0;
    static uint8_t currentLevel = 0;
    
    unsigned long now = millis();
    if (now - lastUpdate < 10) {
        return;  // Not time yet
    }
    lastUpdate = now;
    
    if (currentLevel < targetLevel) {
        currentLevel++;
        analogWrite(PIN_LED, currentLevel);
    } else if (currentLevel > targetLevel) {
        currentLevel--;
        analogWrite(PIN_LED, currentLevel);
    }
}
```

### Timing Patterns

#### Periodic Actions
```cpp
void checkSensor() {
    static unsigned long lastRead = 0;
    unsigned long now = millis();
    
    if (now - lastRead >= SENSOR_INTERVAL) {
        lastRead = now;
        sensorValue = readSensor();
    }
}
```

#### Timeouts
```cpp
void checkTimeout() {
    unsigned long elapsed = millis() - operationStartTime;
    
    if (elapsed > TIMEOUT_MS) {
        enterFailSafeMode();
    }
}
```

#### Debouncing
```cpp
bool readButtonDebounced() {
    static bool lastState = HIGH;
    static unsigned long lastChange = 0;
    
    bool currentState = digitalRead(PIN_BUTTON);
    
    if (currentState != lastState) {
        lastChange = millis();
        lastState = currentState;
    }
    
    return (millis() - lastChange > DEBOUNCE_MS) && (currentState == LOW);
}
```

---

## 🔄 State Machine Best Practices

### Define Clear States

```cpp
enum State {
    STATE_INIT,           // Initializing
    STATE_IDLE,           // Waiting for commands
    STATE_ACTIVE,         // Performing action
    STATE_TRANSITIONING,  // Gradual change
    STATE_ERROR,          // Error condition
    STATE_SAFE_MODE       // Fail-safe mode
};
```

### Use Switch Statements

```cpp
void updateHardware() {
    switch(currentState) {
        case STATE_INIT:
            // One-time initialization
            currentState = STATE_IDLE;
            break;
            
        case STATE_IDLE:
            // Wait for commands
            break;
            
        case STATE_ACTIVE:
            // Perform actions
            if (isComplete()) {
                currentState = STATE_IDLE;
            }
            break;
            
        case STATE_ERROR:
            enterFailSafeMode();
            currentState = STATE_SAFE_MODE;
            break;
            
        case STATE_SAFE_MODE:
            // Stay in safe mode
            break;
    }
}
```

### Track State Transitions

```cpp
void setState(State newState) {
    Serial.print("State: ");
    Serial.print(currentState);
    Serial.print(" -> ");
    Serial.println(newState);
    
    currentState = newState;
}
```

---

## 📡 ESP-NOW Communication

### Automatic Features (Handled by NodeBase)

- ✅ Discovery (ANNOUNCE message on boot)
- ✅ Peer registration (dynamic MAC-based)
- ✅ Heartbeat (periodic alive signal)
- ✅ Watchdog (60-second timeout → fail-safe)
- ✅ Message sequencing (auto-incremented)
- ✅ Timestamp injection (millis() in header)

### Your Responsibilities

1. **Implement handleCommand()** - Process incoming commands
2. **Call sendStatus()** - Acknowledge commands, report status
3. **Respond promptly** - Handle commands quickly (<100ms)

### Status Reporting Pattern

```cpp
void handleCommand(const CommandMessage& cmd) {
    // 1. Parse command
    uint8_t cmdType = cmd.commandData[0];
    
    // 2. Validate input
    if (/* invalid */) {
        sendStatus(cmd.commandId, STATUS_ERROR, nullptr, 0);
        return;
    }
    
    // 3. Apply changes
    targetLevel = cmd.commandData[1];
    
    // 4. Acknowledge immediately
    sendStatus(cmd.commandId, STATUS_OK, nullptr, 0);
    
    // 5. Hardware updates happen in updateHardware()
}
```

### Periodic Status Updates

```cpp
void sendPeriodicStatus() {
    static unsigned long lastReport = 0;
    unsigned long now = millis();
    
    if (now - lastReport >= STATUS_INTERVAL) {
        lastReport = now;
        
        uint8_t data[4];
        data[0] = currentLevel;
        data[1] = targetLevel;
        data[2] = currentState;
        data[3] = errorCount;
        
        sendStatus(0xFF, STATUS_OK, data, 4);  // 0xFF = unsolicited
    }
}
```

---

## 🧩 Hardware Abstraction Patterns

### Pin Definitions

```cpp
// Group related pins
#define PIN_RELAY_1    5
#define PIN_RELAY_2    4
#define PIN_SENSOR_1   A0
#define PIN_SENSOR_2   14

// Use descriptive names
#define PIN_HEATER_RELAY   5
#define PIN_TEMP_SENSOR    4
#define PIN_STATUS_LED     2
```

### Hardware Wrappers

```cpp
class RelayController {
private:
    uint8_t pin;
    bool state;
    
public:
    RelayController(uint8_t p) : pin(p), state(false) {}
    
    void begin() {
        pinMode(pin, OUTPUT);
        off();
    }
    
    void on() {
        digitalWrite(pin, HIGH);
        state = true;
    }
    
    void off() {
        digitalWrite(pin, LOW);
        state = false;
    }
    
    bool isOn() { return state; }
};

RelayController heater(PIN_HEATER_RELAY);
```

### Sensor Reading with Filtering

```cpp
class TemperatureSensor {
private:
    uint8_t pin;
    float readings[5];
    uint8_t index = 0;
    
public:
    TemperatureSensor(uint8_t p) : pin(p) {}
    
    void begin() {
        pinMode(pin, INPUT);
    }
    
    float read() {
        // Read raw value
        int raw = analogRead(pin);
        float temp = convertToTemperature(raw);
        
        // Store in circular buffer
        readings[index] = temp;
        index = (index + 1) % 5;
        
        // Return median
        return getMedian();
    }
    
private:
    float convertToTemperature(int raw) {
        // Conversion logic here
        return raw * 0.1;
    }
    
    float getMedian() {
        // Calculate median of last 5 readings
        // Implementation omitted for brevity
        return readings[2];  // Simplified
    }
};
```

---

## 📂 Node Directory Structure

Each node has its own subdirectory under `src/nodes/`:

```
src/nodes/<node_name>/
├── src/
│   └── main.cpp        # Node implementation
└── data/               # Reserved for future use (OTA, config files)
```

### PlatformIO Environment

Each node has a dedicated environment in `platformio.ini`:

```ini
[env:node_<name>]
platform = espressif8266
board = esp12e
framework = arduino
src_dir = src/nodes/<node_name>/src
data_dir = src/nodes/<node_name>/data
```

### Build Commands

```bash
# Build specific node
platformio run --environment node_lighting

# Upload to ESP8266
platformio run --environment node_lighting --target upload

# Monitor serial output
platformio device monitor --environment node_lighting
```

---

## 🧪 Testing & Debugging

### Serial Logging

```cpp
// Verbose logging during development
void logCommand(const CommandMessage& cmd) {
    Serial.print("[CMD] Type: 0x");
    Serial.print(cmd.commandData[0], HEX);
    Serial.print(" Data: ");
    for (int i = 1; i < 8; i++) {
        Serial.print(cmd.commandData[i], HEX);
        Serial.print(" ");
    }
    Serial.println();
}
```

### State Monitoring

```cpp
void printState() {
    static unsigned long lastPrint = 0;
    if (millis() - lastPrint < 1000) return;
    lastPrint = millis();
    
    Serial.print("State: ");
    Serial.print(currentState);
    Serial.print(" | Level: ");
    Serial.print(currentLevel);
    Serial.print("/");
    Serial.println(targetLevel);
}
```

### Watchdog Testing

```cpp
// Simulate communication loss
void testFailSafe() {
    Serial.println("Testing fail-safe in 5 seconds...");
    delay(5000);
    
    // Trigger watchdog by not calling nodeLoop()
    for (int i = 0; i < 70; i++) {
        delay(1000);
        updateHardware();
        // nodeLoop() NOT called → watchdog triggers
    }
}
```

---

## ⚠️ Common Pitfalls

### ❌ Don't Make Global Decisions

```cpp
// ❌ BAD - Node makes global decision
void handleTemperature() {
    if (temperature > 28.0) {
        turnOffHeater();  // ❌ Node deciding
    }
}
```

```cpp
// ✅ GOOD - Hub makes decision, node executes
void handleCommand(const CommandMessage& cmd) {
    if (cmd.commandData[0] == CMD_HEATER_OFF) {
        turnOffHeater();  // ✅ Hub decided
    }
}
```

### ❌ Don't Block in handleCommand()

```cpp
// ❌ BAD - Blocking in command handler
void handleCommand(const CommandMessage& cmd) {
    if (cmd.commandData[0] == CMD_FEED) {
        servo.write(90);
        delay(2000);  // ❌ BLOCKS
        servo.write(0);
    }
}
```

```cpp
// ✅ GOOD - Set state, let updateHardware() handle it
void handleCommand(const CommandMessage& cmd) {
    if (cmd.commandData[0] == CMD_FEED) {
        currentState = STATE_FEEDING;
        feedStartTime = millis();
        sendStatus(cmd.commandId, STATUS_OK, nullptr, 0);
    }
}

void updateHardware() {
    if (currentState == STATE_FEEDING) {
        if (millis() - feedStartTime < 2000) {
            servo.write(90);
        } else {
            servo.write(0);
            currentState = STATE_IDLE;
        }
    }
}
```

### ❌ Don't Ignore Input Validation

```cpp
// ❌ BAD - No validation
void handleCommand(const CommandMessage& cmd) {
    uint8_t level = cmd.commandData[1];
    analogWrite(PIN_PWM, level);  // ❌ What if level > 255?
}
```

```cpp
// ✅ GOOD - Always validate
void handleCommand(const CommandMessage& cmd) {
    uint8_t level = cmd.commandData[1];
    
    if (level > 100) {
        sendStatus(cmd.commandId, STATUS_ERROR, nullptr, 0);
        return;
    }
    
    targetLevel = level;
    sendStatus(cmd.commandId, STATUS_OK, nullptr, 0);
}
```

---

## 📚 Next Steps

### For Node-Specific Details, See:

- **[Fish Feeder Node](.github/nodes/fish-feeder-instructions.md)** - Servo control, portion management
- **[CO₂ Regulator Node](.github/nodes/co2-regulator-instructions.md)** - Solenoid timing, safety logic
- **[Lighting Node](.github/nodes/lighting-instructions.md)** - 3-channel PWM, color mixing
- **[Heater Node](.github/nodes/heater-instructions.md)** - Temperature control, relay management
- **[Water Quality Node](.github/nodes/water-quality-instructions.md)** - Multi-sensor reading, calibration
- **[Repeater Node](.github/nodes/repeater-instructions.md)** - ESP-NOW forwarding logic

### For System-Level Information:

- **[Hub Instructions](.github/hub-instructions.md)** - Central controller architecture
- **[Protocol Specification](.github/protocol-instructions.md)** - Message formats and communication rules
- **[Build System](.github/build-instructions.md)** - PlatformIO configuration and compilation

---

**Last Updated**: December 29, 2025  
**Version**: 2.0 (Modular Instructions)  
**Repository**: bghosh412/aquarium-management-system
