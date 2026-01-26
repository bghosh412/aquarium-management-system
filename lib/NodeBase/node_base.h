#ifndef NODE_BASE_H
#define NODE_BASE_H

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

// ============================================================================
// NODEBASE - Shared functionality for all node types
// ============================================================================
// Handles: Configuration, Provisioning, Announcements, Heartbeats, ACK handling
// Each node implements: setupHardware(), enterFailSafeMode(), handleCommand(), updateHardware()
// ============================================================================

// ============================================================================
// STATUS LED CONFIGURATION
// ============================================================================
// All nodes use D7 (GPIO13) for status LED
// States: WAITING_ACK (blink), FAILSAFE (blink), COMMAND_ACTIVE (blink 30s), NORMAL (solid ON)
// ============================================================================

#ifdef ESP8266
    #define PIN_STATUS_LED D7  // GPIO13 on ESP8266
#else
    #define PIN_STATUS_LED 13  // GPIO13 on ESP32
#endif

#define STATUS_LED_BLINK_INTERVAL_MS 300  // Medium blink rate (300ms on/off)
#define STATUS_LED_COMMAND_DURATION_MS 30000  // Blink for 30 seconds after command

// Status LED modes
enum class StatusLEDMode : uint8_t {
    WAITING_ACK,      // Blinking - waiting for hub ACK after boot
    FAILSAFE,         // Blinking - node is in fail-safe mode
    COMMAND_ACTIVE,   // Blinking - executing/just executed hub command
    NORMAL            // Solid ON - connected and idle
};

// Configuration structure used by all nodes
struct NodeConfig {
    uint8_t tankId;              // 0 = unmapped, >0 = assigned tank
    String nodeName;             // Human-readable name
    NodeType nodeType;           // Type of node (LIGHT, CO2, etc)
    uint8_t firmwareVersion;
    uint8_t espnowChannel;
    bool debugSerial;
    bool debugESPNOW;
    bool debugHardware;
    uint32_t announceIntervalMs;
    uint32_t heartbeatIntervalMs;
    uint32_t connectionTimeoutMs;
    uint32_t hubHeartbeatTimeoutMs; // ms before node declares hub offline (default 600000)
    uint8_t hubReturnMac[6];   // Hub AP MAC to reply to (populated from ACK)
    bool hubReturnMacSet;      // True when hubReturnMac contains valid MAC
};

// Last seen time of hub heartbeat on node
extern uint32_t lastHubHeartbeatReceived;
// True when hub heartbeat timeout has been detected
extern bool hubHeartbeatLost;

// Setup base-level callbacks that nodes should call AFTER ESPNow init
void setupNodeBaseCallbacks();

// Main node base loop - nodes should call this each loop iteration
void nodeLoop();

// Global configuration (implemented in node_base.cpp)
extern NodeConfig nodeConfig;

// Connection state
extern bool isConnectedToHub;
extern uint32_t lastHeartbeatSent;
extern uint8_t messageSequence;
extern uint32_t nodeUnixTime;  // Node's synchronized Unix timestamp
extern uint32_t nodeBootMillis;  // millis() when time was last synced

// ============================================================================
// CONFIGURATION MANAGEMENT
// ============================================================================

// Load configuration from LittleFS (/node_config.txt)
void loadNodeConfiguration(NodeType defaultType, const char* defaultName);

// Save configuration to LittleFS (called during provisioning)
void saveNodeConfiguration();

// ============================================================================
// MESSAGE HANDLERS (Base implementations - nodes can override if needed)
// ============================================================================

// Handles ACK messages from hub (marks node as connected)
void onAckReceived(const uint8_t* mac, const AckMessage& msg);

// Handles CONFIG messages from hub (provisions node with tank assignment)
void onConfigReceived(const uint8_t* mac, const ConfigMessage& msg);

// Handles UNMAP messages from hub (removes tank assignment)
void onUnmapReceived(const uint8_t* mac, const UnmapMessage& msg);

// Sends heartbeat to hub (called periodically)
void sendHeartbeat();

// Sends ANNOUNCE message (called when unmapped or disconnected)
void sendAnnounce();

// Sends STATUS acknowledgment after command processing
void sendStatusAck(const uint8_t* mac, uint8_t commandId, uint8_t statusCode, const uint8_t* data = nullptr, size_t dataLen = 0);

// ============================================================================
// STATUS LED MANAGEMENT
// ============================================================================

// Initialize status LED pin (call in setup() before other hardware init)
void setupStatusLED();

// Update status LED state (call in loop() - handles blinking automatically)
void updateStatusLED();

// Set status LED mode (called automatically by NodeBase, but nodes can override)
void setStatusLEDMode(StatusLEDMode mode);

// Trigger command activity indicator (blinks LED for 30 seconds)
// Call this when a hub command is received/processed
void triggerCommandActivity();

// Get current status LED mode
StatusLEDMode getStatusLEDMode();

// ============================================================================
// OTA COMMAND HANDLING (Common to all nodes)
// ============================================================================

// Checks if command is an OTA command and handles it
// Returns true if command was an OTA command (handled internally)
// Returns false if command should be passed to device-specific handleCommand()
bool handleOtaCommand(const uint8_t* mac, const CommandMessage& cmd);

// ============================================================================
// FUNCTIONS THAT MUST BE IMPLEMENTED BY EACH NODE TYPE
// ============================================================================

// Initialize hardware-specific pins/peripherals
void setupHardware();

// Put hardware in safe state (on timeout or error)
void enterFailSafeMode();

// Process command data received from hub
// data[0] = command type, data[1..n] = parameters
void handleCommand(const uint8_t* mac, const uint8_t* data, size_t len);

// Update hardware state (called every loop iteration)
void updateHardware();

#endif // NODE_BASE_H
