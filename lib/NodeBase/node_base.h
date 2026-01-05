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
};

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
