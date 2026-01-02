#ifndef NODE_BASE_H
#define NODE_BASE_H

#include <Arduino.h>
#ifdef ESP8266
    #include <ESP8266WiFi.h>
    #include <espnow.h>
#else
    #include <WiFi.h>
    #include <esp_now.h>
    #include <esp_wifi.h>
#endif
#include "protocol/messages.h"

// State machine for all nodes
enum class NodeState : uint8_t {
    INITIALIZING,
    ANNOUNCING,
    WAITING_FOR_ACK,
    CONNECTED,
    LOST_CONNECTION
};

// Global state (extern declarations - defined in node implementation)
extern NodeState currentState;
extern uint8_t hubMacAddress[6];
extern bool hubDiscovered;
extern uint32_t lastHeartbeatSent;
extern uint32_t lastHeartbeatReceived;
extern uint32_t announceAttempts;
extern uint8_t messageSequence;

// Node configuration (defined by each specific node)
extern const uint8_t NODE_TANK_ID;
extern const NodeType NODE_TYPE;
extern const char* NODE_NAME;
extern const uint8_t FIRMWARE_VERSION;

// Timing constants
const uint32_t ANNOUNCE_INTERVAL_MS = 5000;
const uint32_t HEARTBEAT_INTERVAL_MS = 30000;
const uint32_t CONNECTION_TIMEOUT_MS = 90000;

// Functions that must be implemented by each specific node
void setupHardware();           // Initialize hardware-specific pins/peripherals
void enterFailSafeMode();       // Put hardware in safe state
void handleCommand(const CommandMessage* msg);  // Process commands from hub
void updateHardware();          // Called in main loop when connected

// Shared ESP-NOW functions (implemented in node_base.cpp)
void sendAnnounce();
void sendHeartbeat();
void sendStatus(uint8_t commandId, uint8_t statusCode, const uint8_t* data, size_t dataLen);
void setupESPNow();
void nodeLoop();

#ifdef ESP8266
void onDataReceived(uint8_t* mac, uint8_t* data, uint8_t len);
void onDataSent(uint8_t* mac, uint8_t status);
#else
void onDataReceived(const uint8_t* mac, const uint8_t* data, int len);
void onDataSent(const uint8_t* mac, esp_now_send_status_t status);
#endif

#endif // NODE_BASE_H
