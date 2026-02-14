#include <Arduino.h>
#ifdef ESP8266
    #include <ESP8266WiFi.h>
    #include <LittleFS.h>
#else
    #include <WiFi.h>
    #include <LittleFS.h>
#endif
#include "protocol/messages.h"
#include "ESPNowManager.h"
#include "node_base.h"

// ============================================================================
// WAVE MAKER NODE - Sinusoidal aquarium wave maker via PWM-driven pump
// ============================================================================
// Platform: ESP8266 WEMOS D1 Mini
// Hardware: MOSFET-driven 12V DC pump controlled via PWM on D1
// Fail-safe: STOP pump (D1 LOW) - safer to stop flow than risk uncontrolled pump
// Pin Configuration: D1 (GPIO5) for PWM output
//
// PWM Configuration (from hub WaveMaker_config.txt):
//   - Frequency: 200Hz (ESP8266 default analogWriteFreq)
//   - Resolution: 10-bit (0-1023)
//   - Max Duty Cycle: 95% (configurable)
//   - Min Duty Cycle: 30% (configurable, below this pump stalls)
//
// Wave Pattern:
//   - Sinusoidal modulation of duty cycle between min and max
//   - Soft start: gradually ramps from 0% to target over ~3 seconds
//   - Hub sends target duty cycle as floating point percentage
//
// Command Format from Hub:
//   Command Type 0: STOP  - Set D1 LOW, pump off
//   Command Type 1: PWM   - data[1-4] = duty cycle % (float, little-endian)
//   Command Type 40: Status request
//   Command Type 0xFF: Force fail-safe (test)
// ============================================================================

// Hardware pin (WEMOS D1 Mini)
#define PIN_PUMP_PWM D1   // GPIO5

// PWM configuration
#define PWM_FREQUENCY     200    // 200Hz PWM frequency
#define PWM_RESOLUTION    1023   // 10-bit resolution (0-1023)
#define MAX_DUTY_PERCENT  95.0f  // Maximum duty cycle %
#define MIN_DUTY_PERCENT  30.0f  // Minimum duty cycle % (pump stall threshold)

// Soft start configuration
#define SOFT_START_DURATION_MS  3000   // 3 seconds to ramp up
#define SOFT_START_STEP_MS      20     // Update every 20ms during ramp

// Wave maker state machine
enum class WaveMakerState : uint8_t {
    STOPPED,        // Pump off, D1 LOW
    SOFT_STARTING,  // Ramping up from 0 to target duty
    RUNNING,        // Pump running at target duty cycle
    SOFT_STOPPING,  // Ramping down to 0 (graceful stop)
    FAIL_SAFE       // Emergency stop state
};

// Wave maker state data
struct WaveMakerStateData {
    WaveMakerState state;
    float targetDutyPercent;     // Target duty cycle % from hub (0.0 - 95.0)
    float currentDutyPercent;    // Current actual duty cycle %
    uint32_t softStartBegin;    // millis() when soft start began
    uint32_t lastUpdateTime;    // Last hardware update timestamp
    uint16_t currentPwmValue;   // Current raw PWM value (0-1023)
    bool pumpActive;            // Pump is running
} waveMakerState = {
    .state = WaveMakerState::STOPPED,
    .targetDutyPercent = 0.0f,
    .currentDutyPercent = 0.0f,
    .softStartBegin = 0,
    .lastUpdateTime = 0,
    .currentPwmValue = 0,
    .pumpActive = false
};

// ============================================================================
// PWM HELPER FUNCTIONS
// ============================================================================

/**
 * @brief Convert duty cycle percentage to raw 10-bit PWM value
 * @param percent Duty cycle percentage (0.0 - 100.0)
 * @return Raw PWM value (0-1023)
 */
static uint16_t percentToPwm(float percent) {
    if (percent <= 0.0f) return 0;
    if (percent > 100.0f) percent = 100.0f;
    return (uint16_t)((percent / 100.0f) * PWM_RESOLUTION);
}

/**
 * @brief Apply PWM value to pump pin
 * @param pwmValue Raw PWM value (0-1023)
 */
static void applyPwm(uint16_t pwmValue) {
    if (pwmValue > PWM_RESOLUTION) pwmValue = PWM_RESOLUTION;
    waveMakerState.currentPwmValue = pwmValue;
    analogWrite(PIN_PUMP_PWM, pwmValue);
}

/**
 * @brief Immediately stop the pump (D1 LOW)
 */
static void stopPump() {
    analogWrite(PIN_PUMP_PWM, 0);
    digitalWrite(PIN_PUMP_PWM, LOW);
    waveMakerState.currentPwmValue = 0;
    waveMakerState.currentDutyPercent = 0.0f;
    waveMakerState.targetDutyPercent = 0.0f;
    waveMakerState.pumpActive = false;
    waveMakerState.state = WaveMakerState::STOPPED;

    if (nodeConfig.debugHardware) {
        Serial.println("[WAVE] Pump stopped, D1 LOW");
    }
}

/**
 * @brief Start pump with soft-start ramp to target duty cycle
 * @param dutyPercent Target duty cycle percentage
 * @return true if started successfully
 */
static bool startPump(float dutyPercent) {
    // Clamp to valid range
    if (dutyPercent < MIN_DUTY_PERCENT) {
        if (nodeConfig.debugHardware) {
            Serial.printf("[WAVE] Duty %.1f%% below minimum %.1f%%, clamping\n",
                          dutyPercent, MIN_DUTY_PERCENT);
        }
        dutyPercent = MIN_DUTY_PERCENT;
    }
    if (dutyPercent > MAX_DUTY_PERCENT) {
        if (nodeConfig.debugHardware) {
            Serial.printf("[WAVE] Duty %.1f%% above maximum %.1f%%, clamping\n",
                          dutyPercent, MAX_DUTY_PERCENT);
        }
        dutyPercent = MAX_DUTY_PERCENT;
    }

    waveMakerState.targetDutyPercent = dutyPercent;
    waveMakerState.softStartBegin = millis();
    waveMakerState.lastUpdateTime = millis();
    waveMakerState.currentDutyPercent = 0.0f;
    waveMakerState.pumpActive = true;
    waveMakerState.state = WaveMakerState::SOFT_STARTING;

    // Start with PWM = 0 (soft start will ramp up)
    applyPwm(0);

    if (nodeConfig.debugHardware || nodeConfig.debugSerial) {
        Serial.printf("[WAVE] Soft-start initiated: target=%.1f%% (ramp over %dms)\n",
                      dutyPercent, SOFT_START_DURATION_MS);
    }

    return true;
}

// ============================================================================
// HARDWARE IMPLEMENTATION (Required by NodeBase)
// ============================================================================

void setupHardware() {
    // Configure PWM pin
    pinMode(PIN_PUMP_PWM, OUTPUT);
    digitalWrite(PIN_PUMP_PWM, LOW);  // Start with pump OFF

    // Set ESP8266 PWM frequency to 200Hz
    analogWriteFreq(PWM_FREQUENCY);

    // Set 10-bit resolution (0-1023)
    analogWriteRange(PWM_RESOLUTION);

    // Initialize state
    waveMakerState.state = WaveMakerState::STOPPED;
    waveMakerState.currentPwmValue = 0;
    waveMakerState.currentDutyPercent = 0.0f;
    waveMakerState.targetDutyPercent = 0.0f;
    waveMakerState.pumpActive = false;

    if (nodeConfig.debugSerial) {
        Serial.println("[OK] Wave maker hardware initialized");
        Serial.printf("     PWM Pin: D1 (GPIO%d)\n", PIN_PUMP_PWM);
        Serial.printf("     PWM Freq: %dHz, Resolution: %d (10-bit)\n",
                      PWM_FREQUENCY, PWM_RESOLUTION);
        Serial.printf("     Duty Range: %.0f%% - %.0f%%\n",
                      MIN_DUTY_PERCENT, MAX_DUTY_PERCENT);
        Serial.printf("     Soft Start: %dms ramp\n", SOFT_START_DURATION_MS);
    }
}

void enterFailSafeMode() {
    if (nodeConfig.debugSerial) {
        Serial.println("[WARN] FAIL-SAFE: Hub heartbeat lost - stopping pump for safety");
    }

    // Immediately stop the pump
    analogWrite(PIN_PUMP_PWM, 0);
    digitalWrite(PIN_PUMP_PWM, LOW);
    waveMakerState.currentPwmValue = 0;
    waveMakerState.currentDutyPercent = 0.0f;
    waveMakerState.targetDutyPercent = 0.0f;
    waveMakerState.pumpActive = false;
    waveMakerState.state = WaveMakerState::FAIL_SAFE;

    if (nodeConfig.debugESPNOW) {
        Serial.println("[OK] Fail-safe applied: D1 LOW, pump stopped");
    }
}

/**
 * @brief Read current wave maker status into status data array
 * @param statusData Array to populate with status (min 8 bytes)
 */
static void readWaveMakerStatus(uint8_t* statusData) {
    // Byte 0: State enum
    statusData[0] = static_cast<uint8_t>(waveMakerState.state);
    // Byte 1-2: Current PWM value (uint16_t, little-endian)
    statusData[1] = waveMakerState.currentPwmValue & 0xFF;
    statusData[2] = (waveMakerState.currentPwmValue >> 8) & 0xFF;
    // Byte 3: Pump active flag
    statusData[3] = waveMakerState.pumpActive ? 1 : 0;
    // Byte 4-7: Current duty cycle % as float (little-endian)
    memcpy(&statusData[4], &waveMakerState.currentDutyPercent, sizeof(float));
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
        case 0: // STOP - Set D1 LOW, pump off
            stopPump();
            if (nodeConfig.debugESPNOW) {
                Serial.println("| [OK] STOP command - pump off, D1 LOW");
            }
            break;

        case 1: { // PWM command: data[1-4] = duty cycle % (float, little-endian)
            // Command format: [cmdType=0x01][duty_b0][duty_b1][duty_b2][duty_b3]
            // Total: 5 bytes minimum
            if (len >= 5) {
                float dutyPercent = 0.0f;
                memcpy(&dutyPercent, &data[1], sizeof(float));

                if (nodeConfig.debugESPNOW) {
                    Serial.printf("| [CMD] PWM: duty=%.1f%%\n", dutyPercent);
                }

                // Validate range
                if (dutyPercent < 0.0f || dutyPercent > 100.0f) {
                    success = false;
                    if (nodeConfig.debugESPNOW) {
                        Serial.printf("| [ERROR] Invalid duty cycle: %.1f%%\n", dutyPercent);
                    }
                } else if (dutyPercent == 0.0f) {
                    // Treat 0% as STOP
                    stopPump();
                    if (nodeConfig.debugESPNOW) {
                        Serial.println("| [OK] Duty 0% = STOP");
                    }
                } else {
                    success = startPump(dutyPercent);
                    if (success) {
                        if (nodeConfig.debugESPNOW) {
                            Serial.printf("| [OK] Pump soft-starting to %.1f%%\n", dutyPercent);
                        }
                    } else {
                        if (nodeConfig.debugESPNOW) {
                            Serial.println("| [ERROR] Failed to start pump");
                        }
                    }
                }
            } else {
                success = false;
                if (nodeConfig.debugESPNOW) {
                    Serial.printf("| [ERROR] PWM command requires 5 bytes, got %zu\n", len);
                }
            }
            break;
        }

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
    uint8_t statusData[8] = {0};
    readWaveMakerStatus(statusData);

    if (nodeConfig.debugESPNOW) {
        Serial.printf("| [STATUS] state=%d, PWM=%d, active=%d, duty=%.1f%%\n",
                      statusData[0],
                      statusData[1] | (statusData[2] << 8),
                      statusData[3],
                      waveMakerState.currentDutyPercent);
    }

    sendStatusAck(mac, 0, statusCode, statusData, 8);
}

void updateHardware() {
    // Non-blocking state machine for wave maker operation
    uint32_t now = millis();

    switch (waveMakerState.state) {
        case WaveMakerState::STOPPED:
            // Nothing to do, pump should be off
            break;

        case WaveMakerState::SOFT_STARTING: {
            // Ramp up from 0 to target duty over SOFT_START_DURATION_MS
            // Update every SOFT_START_STEP_MS
            if (now - waveMakerState.lastUpdateTime >= SOFT_START_STEP_MS) {
                waveMakerState.lastUpdateTime = now;

                uint32_t elapsed = now - waveMakerState.softStartBegin;

                if (elapsed >= SOFT_START_DURATION_MS) {
                    // Ramp complete - switch to running state
                    waveMakerState.currentDutyPercent = waveMakerState.targetDutyPercent;
                    applyPwm(percentToPwm(waveMakerState.currentDutyPercent));
                    waveMakerState.state = WaveMakerState::RUNNING;

                    if (nodeConfig.debugHardware || nodeConfig.debugSerial) {
                        Serial.printf("[WAVE] Soft-start complete, running at %.1f%% (PWM=%d)\n",
                                      waveMakerState.currentDutyPercent,
                                      waveMakerState.currentPwmValue);
                    }
                } else {
                    // Sinusoidal ramp: use sin(0..π/2) for smooth acceleration
                    float progress = (float)elapsed / (float)SOFT_START_DURATION_MS;
                    float sinFactor = sinf(progress * (PI / 2.0f));  // 0.0 → 1.0 smooth curve
                    waveMakerState.currentDutyPercent = waveMakerState.targetDutyPercent * sinFactor;
                    applyPwm(percentToPwm(waveMakerState.currentDutyPercent));

                    if (nodeConfig.debugHardware && (elapsed % 500 < SOFT_START_STEP_MS)) {
                        Serial.printf("[WAVE] Soft-start: %.1f%% (progress=%.0f%%, PWM=%d)\n",
                                      waveMakerState.currentDutyPercent,
                                      progress * 100.0f,
                                      waveMakerState.currentPwmValue);
                    }
                }
            }
            break;
        }

        case WaveMakerState::RUNNING: {
            // Pump running at target duty cycle - maintain PWM
            // Re-apply periodically to guard against glitches
            if (now - waveMakerState.lastUpdateTime >= 1000) {
                waveMakerState.lastUpdateTime = now;
                waveMakerState.currentDutyPercent = waveMakerState.targetDutyPercent;
                applyPwm(percentToPwm(waveMakerState.currentDutyPercent));
            }
            break;
        }

        case WaveMakerState::SOFT_STOPPING: {
            // Graceful ramp-down (reverse of soft start)
            if (now - waveMakerState.lastUpdateTime >= SOFT_START_STEP_MS) {
                waveMakerState.lastUpdateTime = now;

                uint32_t elapsed = now - waveMakerState.softStartBegin;

                if (elapsed >= SOFT_START_DURATION_MS) {
                    stopPump();
                } else {
                    float progress = (float)elapsed / (float)SOFT_START_DURATION_MS;
                    float sinFactor = cosf(progress * (PI / 2.0f));  // 1.0 → 0.0 smooth curve
                    waveMakerState.currentDutyPercent = waveMakerState.targetDutyPercent * sinFactor;
                    applyPwm(percentToPwm(waveMakerState.currentDutyPercent));
                }
            }
            break;
        }

        case WaveMakerState::FAIL_SAFE:
            // Ensure pump stays off
            if (waveMakerState.currentPwmValue != 0) {
                analogWrite(PIN_PUMP_PWM, 0);
                digitalWrite(PIN_PUMP_PWM, LOW);
                waveMakerState.currentPwmValue = 0;
            }
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
    // CRITICAL: Ensure pump pin is LOW immediately on boot
    pinMode(PIN_PUMP_PWM, OUTPUT);
    digitalWrite(PIN_PUMP_PWM, LOW);

    Serial.begin(115200);
    delay(2000);

    Serial.println("\n\n\n");
    Serial.println("================================");
    Serial.println("ESP8266 BOOT - Serial Working!");
    Serial.println("================================");

    Serial.println("\n\n");
    Serial.println("+===========================================================+");
    Serial.println("|         WAVE MAKER NODE - Aquarium Management             |");
    Serial.println("|         Platform: ESP8266 WEMOS D1 Mini / D1 (GPIO5)      |");
    Serial.println("+===========================================================+");

    // Load configuration using NodeBase
    Serial.println("[1] Loading configuration...");
    loadNodeConfiguration(NodeType::WAVE_MAKER, "UnmappedWaveMaker");
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
    Serial.printf("   - Mode: NODE (ESP8266)\n");
    Serial.printf("   - Debug ESP-NOW: %s\n", nodeConfig.debugESPNOW ? "ON" : "OFF");

    // Send initial ANNOUNCE using NodeBase
    Serial.println("[5] Sending initial ANNOUNCE...");
    sendAnnounce();

    Serial.println("\n[OK] Wave maker node ready (ESP8266 WEMOS D1 Mini)\n");
    lastHeartbeatSent = millis();
}

void loop() {
    // Delegate processing, heartbeats, and hub-liveness checks to NodeBase
    nodeLoop();

    // Update hardware state (non-blocking PWM / soft-start)
    updateHardware();

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
        Serial.printf("[HEARTBEAT] Free heap: %u bytes | State: %d | Duty: %.1f%%\n",
                      ESP.getFreeHeap(),
                      (int)waveMakerState.state,
                      waveMakerState.currentDutyPercent);
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
