#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <ESP32Servo.h>
#include "protocol/messages.h"
#include "ESPNowManager.h"
#include "node_base.h"

// ============================================================================
// FISH FEEDER NODE - Controls aquarium fish feeding via Continuous Servo
// ============================================================================
// Platform: ESP32-WROOM
// Hardware: Continuous Rotation Servo motor driven via PWM signal
// Fail-safe: Stop servo motor (do nothing - safer to skip feeding than overfeed)
// Pin Configuration: GPIO18 (Servo PWM)
// 
// Servo Control (Microstepping Pattern from test_servo.py):
//   - Uses PWM duty cycle to control rotation
//   - Between pulses: servo.detach() to fully stop motor (prevents drift)
//   - Re-attach for next pulse cycle
//   
// Command Format:
//   data[0] = command type (1 = feed)
//   data[1-2] = PWM duty value (uint16_t, typically 0-180)
//   data[3-6] = duration in milliseconds (uint32_t)
// ============================================================================

// Hardware pins (ESP32-WROOM)
// PIN_SERVO moved to /node_config.txt (SERVO_PIN) - use nodeConfig.servoPin at runtime

// Servo constants
#define SERVO_FREQ 50             // Standard servo frequency (50Hz)
#define SERVO_MIN_US 500          // Minimum pulse width in microseconds
#define SERVO_MAX_US 2500         // Maximum pulse width in microseconds
#define SERVO_MAX_PWM 180         // Maximum PWM value
#define SERVO_MAX_DURATION_MS 10000  // Maximum feed duration (10 seconds safety limit)
#define SERVO_PULSE_INTERVAL_MS 2000 // Time between pulse cycles (2 seconds like test_servo.py)

// Servo object
Servo feederServo;

// Feeder state machine
enum class FeederState : uint8_t {
    IDLE,           // Waiting for commands
    PULSE_ON,       // Motor running for pulse duration
    PULSE_OFF,      // Motor detached, waiting between pulses  
    STOPPING,       // Transitioning to stop
    ERROR           // Error state
};

// Feeder state structure
struct FeederStateData {
    FeederState state;
    uint16_t pwmValue;          // PWM duty value (0-180)
    uint32_t durationMs;        // Total feed duration in milliseconds
    uint32_t pulseDurationMs;   // Single pulse duration (from command)
    uint32_t feedStartTime;     // When feeding started
    uint32_t pulseStartTime;    // When current pulse started
    uint32_t elapsedMs;         // Total elapsed time
    uint8_t lastCommandId;      // Last command ID for acknowledgment
    bool feedInProgress;        // Feed operation active flag
    bool servoAttached;         // Servo currently attached
} feederState = {
    .state = FeederState::IDLE,
    .pwmValue = 0,
    .durationMs = 0,
    .pulseDurationMs = 0,
    .feedStartTime = 0,
    .pulseStartTime = 0,
    .elapsedMs = 0,
    .lastCommandId = 0,
    .feedInProgress = false,
    .servoAttached = false
};

// ============================================================================
// SERVO CONTROL FUNCTIONS (Microstepping pattern)
// ============================================================================

/**
 * @brief Attach servo to pin and apply PWM value
 * @param pwmValue PWM value to apply (0-180)
 */
void attachServo(uint16_t pwmValue) {
    if (!feederState.servoAttached) {
        feederServo.setPeriodHertz(SERVO_FREQ);
        feederServo.attach(nodeConfig.servoPin, SERVO_MIN_US, SERVO_MAX_US);
        feederState.servoAttached = true;
    }
    feederServo.write(pwmValue);
    
    if (nodeConfig.debugHardware) {
        Serial.printf("[SERVO] Attached and set to PWM=%d\n", pwmValue);
    }
}

/**
 * @brief Detach servo (stop motor completely - prevents drift)
 */
void detachServo() {
    if (feederState.servoAttached) {
        feederServo.detach();
        feederState.servoAttached = false;
        
        if (nodeConfig.debugHardware) {
            Serial.println("[SERVO] Detached (motor stopped)");
        }
    }
}

/**
 * @brief Stop the servo motor immediately and reset state
 */
void stopServo() {
    detachServo();
    feederState.feedInProgress = false;
    feederState.state = FeederState::IDLE;
    feederState.pwmValue = 0;
    
    if (nodeConfig.debugHardware) {
        Serial.println("[FEED] Servo stopped, state reset to IDLE");
    }
}

// ============================================================================
// FEED METHOD - Core feeding operation (Microstepping pattern)
// ============================================================================

/**
 * @brief Execute a feeding operation with specified PWM speed and duration
 * 
 * Uses microstepping pattern from test_servo.py:
 * 1. Attach servo with PWM duty
 * 2. Wait for pulse duration
 * 3. Detach servo (complete stop)
 * 4. Wait for hold duration (SERVO_PULSE_INTERVAL_MS)
 * 5. Repeat until total duration elapsed
 * 
 * @param pwmValue PWM duty value (0-180)
 * @param durationMs Duration for single pulse in milliseconds
 * @return true if feed started successfully, false if invalid params or busy
 */
bool feed(uint16_t pwmValue, uint32_t durationMs) {
    // Validate: cannot start new feed while one is in progress
    if (feederState.feedInProgress) {
        if (nodeConfig.debugHardware) {
            Serial.println("[WARN] Feed rejected - already in progress");
        }
        return false;
    }
    
    // Validate PWM value range
    if (pwmValue > SERVO_MAX_PWM) {
        if (nodeConfig.debugHardware) {
            Serial.printf("[WARN] PWM value %d exceeds max %d, clamping\n", 
                          pwmValue, SERVO_MAX_PWM);
        }
        pwmValue = SERVO_MAX_PWM;
    }
    
    // Validate duration
    if (durationMs == 0) {
        if (nodeConfig.debugHardware) {
            Serial.println("[WARN] Feed rejected - duration is 0");
        }
        return false;
    }
    if (durationMs > SERVO_MAX_DURATION_MS) {
        if (nodeConfig.debugHardware) {
            Serial.printf("[WARN] Duration %u exceeds max %d, clamping\n", 
                          (unsigned int)durationMs, SERVO_MAX_DURATION_MS);
        }
        durationMs = SERVO_MAX_DURATION_MS;
    }
    
    // Start feeding operation (microstepping)
    feederState.pwmValue = pwmValue;
    feederState.pulseDurationMs = durationMs;
    feederState.durationMs = durationMs;  // Single pulse duration from hub
    feederState.feedStartTime = millis();
    feederState.pulseStartTime = millis();
    feederState.elapsedMs = 0;
    feederState.feedInProgress = true;
    feederState.state = FeederState::PULSE_ON;
    
    // Attach and apply PWM (start first pulse)
    attachServo(pwmValue);
    
    if (nodeConfig.debugHardware || nodeConfig.debugSerial) {
        Serial.printf("[FEED] Started: PWM=%d, Pulse Duration=%ums\n", 
                      pwmValue, (unsigned int)durationMs);
    }
    
    return true;
}

// ============================================================================
// HARDWARE IMPLEMENTATION (Required by NodeBase)
// ============================================================================

void setupHardware() {
    // Configure servo pin
    pinMode(nodeConfig.servoPin, OUTPUT);
    digitalWrite(nodeConfig.servoPin, LOW);
    
    // Initialize feeder state (servo NOT attached at startup)
    feederState.state = FeederState::IDLE;
    feederState.pwmValue = 0;
    feederState.feedInProgress = false;
    feederState.servoAttached = false;
    
    if (nodeConfig.debugSerial) {
        Serial.println("[OK] Fish feeder hardware initialized (GPIO18 - Servo)");
        Serial.println("     Servo NOT attached at startup (will attach on feed command)");
    }
}

void enterFailSafeMode() {
    if (nodeConfig.debugSerial) {
        Serial.println("[WARN] FAIL-SAFE: Hub heartbeat lost - stopping servo for safety");
    }
    
    // Stop the servo immediately
    stopServo();
    feederState.state = FeederState::IDLE;
    
    // Note: For fish feeder, fail-safe = do nothing
    // Missing one feeding is safer than potentially overfeeding
    
    if (nodeConfig.debugESPNOW) {
        Serial.println("[OK] Fail-safe applied: servo detached");
    }
}

/**
 * @brief Read current feeder status into status data array
 * @param statusData Array to populate with status (min 4 bytes)
 */
static void readFeederStatus(uint8_t* statusData) {
    // Byte 0: State (0=IDLE, 1=PULSE_ON, 2=PULSE_OFF, 3=STOPPING, 4=ERROR)
    statusData[0] = static_cast<uint8_t>(feederState.state);
    // Byte 1: Current PWM value (low byte)
    statusData[1] = feederState.pwmValue & 0xFF;
    // Byte 2: Feed in progress flag
    statusData[2] = feederState.feedInProgress ? 1 : 0;
    // Byte 3: Servo attached flag
    statusData[3] = feederState.servoAttached ? 1 : 0;
}

void handleCommand(const uint8_t* mac, const uint8_t* data, size_t len) {
    if (len == 0 || data == nullptr) return;
    
    uint8_t commandType = data[0];
    
    if (nodeConfig.debugESPNOW) {
        Serial.println("+========================================================+");
        Serial.printf("| [CMD] COMMAND received\n");
        Serial.printf("| Tank ID: %d\n", nodeConfig.tankId);
        Serial.printf("| Command Type: %d\n", commandType);
        Serial.printf("| Data length: %zu bytes\n", len);
    }
    
    bool success = true;
    
    switch (commandType) {
        case 0: // Stop/Emergency stop
            stopServo();
            if (nodeConfig.debugESPNOW) {
                Serial.println("| [OK] Emergency stop - servo detached");
            }
            break;
            
        case 1: // Feed command: data[1-2] = PWM (uint16_t), data[3-6] = duration (uint32_t)
            // Command format: [cmdType(1)] [pwm_low(1)] [pwm_high(1)] [dur_b0(1)] [dur_b1(1)] [dur_b2(1)] [dur_b3(1)]
            // Total: 7 bytes
            if (len >= 7) {
                // Parse PWM value as uint16_t (little endian)
                uint16_t pwmValue = data[1] | (data[2] << 8);
                // Parse duration as uint32_t (little endian)
                uint32_t durationMs = data[3] | (data[4] << 8) | 
                                     (data[5] << 16) | (data[6] << 24);
                
                if (nodeConfig.debugESPNOW) {
                    Serial.printf("| [CMD] Feed: PWM=%d, Duration=%ums\n", pwmValue, (unsigned int)durationMs);
                }
                
                success = feed(pwmValue, durationMs);
                
                if (success) {
                    if (nodeConfig.debugESPNOW) {
                        Serial.println("| [OK] Feed started successfully");
                    }
                } else {
                    if (nodeConfig.debugESPNOW) {
                        Serial.println("| [ERROR] Feed failed (busy or invalid params)");
                    }
                }
            } else {
                success = false;
                if (nodeConfig.debugESPNOW) {
                    Serial.printf("| [ERROR] Feed command requires 7 bytes, got %zu\n", len);
                }
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
    
    // Send acknowledgment with current status
    uint8_t statusCode = success ? 0x00 : 0xFF;
    uint8_t statusData[4] = {0};
    readFeederStatus(statusData);
    
    if (nodeConfig.debugESPNOW) {
        Serial.printf("| [STATUS] Feeder state=%d, PWM=%d, feeding=%d, attached=%d\n",
                      statusData[0], statusData[1], statusData[2], statusData[3]);
    }
    
    sendStatusAck(mac, 0, statusCode, statusData, 4);
}

void updateHardware() {
    // Non-blocking state machine for feeder operation (microstepping)
    
    switch (feederState.state) {
        case FeederState::IDLE:
            // Nothing to do, servo should be detached
            break;
            
        case FeederState::PULSE_ON:
            // Motor running - check if pulse duration elapsed
            if (feederState.feedInProgress) {
                uint32_t pulseElapsed = millis() - feederState.pulseStartTime;
                
                if (pulseElapsed >= feederState.pulseDurationMs) {
                    // Pulse complete, detach servo
                    detachServo();
                    
                    // Single pulse mode: just stop after one pulse
                    if (nodeConfig.debugHardware || nodeConfig.debugSerial) {
                        Serial.printf("[FEED] Pulse complete after %ums, stopping\n", 
                                      (unsigned int)pulseElapsed);
                    }
                    stopServo();
                }
            }
            break;
            
        case FeederState::PULSE_OFF:
            // Waiting between pulses - for continuous mode (not currently used)
            break;
            
        case FeederState::STOPPING:
            // Transition state - detach servo and go to IDLE
            stopServo();
            break;
            
        case FeederState::ERROR:
            // In error state, ensure servo is detached
            detachServo();
            break;
    }
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
    Serial.println("ESP32 BOOT - Serial Working!");
    Serial.println("================================");
    
    Serial.println("\n\n");
    Serial.println("+===========================================================+");
    Serial.println("|         FISH FEEDER NODE - Aquarium Management            |");
    Serial.println("|         Platform: ESP32-WROOM / GPIO18                    |");
    Serial.println("+===========================================================+");
    
    // Load configuration using NodeBase
    Serial.println("[1] Loading configuration...");
    loadNodeConfiguration(NodeType::FISH_FEEDER, "UnmappedFeeder");
    Serial.println("[1] Configuration loaded OK");
    
    Serial.printf("Tank ID: %d | Node: %s | FW: v%d\n\n", 
                  nodeConfig.tankId, nodeConfig.nodeName.c_str(), nodeConfig.firmwareVersion);
    
    // Initialize status LED (starts blinking - waiting for ACK)
    Serial.println("[2] Initializing status LED...");
    setupStatusLED();
    Serial.println("[2] Status LED initialized OK");
    
    // Initialize hardware
    Serial.println("[3] Initializing hardware...");
    setupHardware();
    Serial.println("[3] Hardware initialized OK");
    
    // Initialize ESPNowManager
    Serial.println("[4] Starting ESP-NOW initialization...");
    bool success = ESPNowManager::getInstance().begin(nodeConfig.espnowChannel, false);
    Serial.printf("[4] ESP-NOW init returned: %s\n", success ? "SUCCESS" : "FAILED");
    
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
    Serial.printf("   - Mode: NODE (ESP32)\n");
    Serial.printf("   - Debug ESP-NOW: %s\n", nodeConfig.debugESPNOW ? "ON" : "OFF");
    
    // Send initial ANNOUNCE using NodeBase
    Serial.println("[5] Sending initial ANNOUNCE...");
    sendAnnounce();
    
    Serial.println("\n[OK] Fish feeder node ready (ESP32-WROOM)\n");
    lastHeartbeatSent = millis();
}

void loop() {
    // Delegate processing, heartbeats, and hub-liveness checks to NodeBase
    nodeLoop();
    
    // Update hardware state (non-blocking feed operation check)
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
