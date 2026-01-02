/**
 * ESP-NOW Repeater Node
 * 
 * Purpose: Extends the range of the hub by forwarding ESP-NOW messages
 * 
 * Features:
 * - Receives messages from distant nodes
 * - Forwards to hub (and vice versa)
 * - Low latency message relay
 * - Transparent to other nodes
 * - Can be daisy-chained for extended range
 * 
 * Hardware: ESP8266 or ESP32-C3
 * Power: Can be powered from USB (always on)
 */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <espnow.h>
#include "protocol/messages.h"

// Repeater Configuration
const uint8_t TANK_ID = 1;
const char REPEATER_NAME[] = "Repeater-01";
const uint8_t FIRMWARE_VERSION = 1;

// Hub MAC address (learned dynamically)
uint8_t hubMAC[6] = {0};
bool hubRegistered = false;

// Statistics
unsigned long messagesForwarded = 0;
unsigned long lastForwardTime = 0;
unsigned long bootTime = 0;

// State
enum class RepeaterState {
    INIT,
    DISCOVERING,
    ACTIVE,
    FAIL_SAFE
};
RepeaterState currentState = RepeaterState::INIT;

/**
 * ESP-NOW Receive Callback
 * Forwards all messages transparently
 */
void onDataReceived(uint8_t *senderMAC, uint8_t *data, uint8_t len) {
    MessageHeader* header = (MessageHeader*)data;
    
    Serial.print("[RX] From: ");
    for (int i = 0; i < 6; i++) {
        Serial.printf("%02X", senderMAC[i]);
        if (i < 5) Serial.print(":");
    }
    Serial.printf(" | Type: %d | Tank: %d | Len: %d\n", 
                  (uint8_t)header->type, header->tankId, len);
    
    // Learn hub MAC from ACK messages
    if (header->type == MessageType::ACK && header->nodeType == NodeType::HUB) {
        if (!hubRegistered) {
            memcpy(hubMAC, senderMAC, 6);
            hubRegistered = true;
            esp_now_add_peer(hubMAC, ESP_NOW_ROLE_COMBO, ESPNOW_CHANNEL, NULL, 0);
            Serial.println("[HUB] Learned hub MAC address");
            currentState = RepeaterState::ACTIVE;
        }
    }
    
    // Forward message
    if (hubRegistered) {
        // If message is from hub, broadcast it (for nodes to receive)
        if (memcmp(senderMAC, hubMAC, 6) == 0) {
            esp_now_send(NULL, data, len);  // Broadcast
            Serial.println("[FWD] Hub  Nodes (broadcast)");
        }
        // If message is from node, send to hub
        else {
            esp_now_send(hubMAC, data, len);
            Serial.println("[FWD] Node  Hub");
        }
        
        messagesForwarded++;
        lastForwardTime = millis();
    }
}

/**
 * ESP-NOW Send Callback
 */
void onDataSent(uint8_t *mac, uint8_t status) {
    if (status != 0) {
        Serial.printf("[TX] Send failed to ");
        for (int i = 0; i < 6; i++) {
            Serial.printf("%02X", mac[i]);
            if (i < 5) Serial.print(":");
        }
        Serial.println();
    }
}

/**
 * Send ANNOUNCE message to discover hub
 */
void sendAnnounce() {
    AnnounceMessage msg;
    msg.header.type = MessageType::ANNOUNCE;
    msg.header.tankId = TANK_ID;
    msg.header.nodeType = NodeType::UNKNOWN;  // Repeater doesn't have a specific type yet
    msg.header.timestamp = millis();
    msg.header.sequenceNum = 0;
    
    strncpy(msg.nodeName, REPEATER_NAME, MAX_NODE_NAME_LEN);
    msg.firmwareVersion = FIRMWARE_VERSION;
    msg.capabilities = 0xFF;  // Special capability flag for repeater
    
    esp_now_send(NULL, (uint8_t*)&msg, sizeof(msg));
    Serial.println("[TX] ANNOUNCE (broadcast)");
}

/**
 * Send periodic heartbeat
 */
void sendHeartbeat() {
    if (!hubRegistered) return;
    
    HeartbeatMessage msg;
    msg.header.type = MessageType::HEARTBEAT;
    msg.header.tankId = TANK_ID;
    msg.header.nodeType = NodeType::UNKNOWN;
    msg.header.timestamp = millis();
    msg.header.sequenceNum = 0;
    
    msg.health = 100;
    msg.uptimeMinutes = (millis() - bootTime) / 60000;
    
    esp_now_send(hubMAC, (uint8_t*)&msg, sizeof(msg));
}

/**
 * Setup ESP-NOW
 */
void setupESPNow() {
    // Set device as WiFi station
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    
    // Set WiFi channel
    wifi_set_channel(ESPNOW_CHANNEL);
    
    Serial.print("MAC Address: ");
    Serial.println(WiFi.macAddress());
    
    // Initialize ESP-NOW
    if (esp_now_init() != 0) {
        Serial.println("ESP-NOW init failed");
        currentState = RepeaterState::FAIL_SAFE;
        return;
    }
    
    Serial.println("ESP-NOW initialized");
    
    // Register callbacks
    esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
    esp_now_register_recv_cb(onDataReceived);
    esp_now_register_send_cb(onDataSent);
    
    currentState = RepeaterState::DISCOVERING;
}

/**
 * Print statistics
 */
void printStats() {
    Serial.println("\n=== Repeater Statistics ===");
    Serial.printf("Uptime: %lu minutes\n", (millis() - bootTime) / 60000);
    Serial.printf("State: %d\n", (int)currentState);
    Serial.printf("Hub registered: %s\n", hubRegistered ? "YES" : "NO");
    Serial.printf("Messages forwarded: %lu\n", messagesForwarded);
    Serial.printf("Last forward: %lu ms ago\n", millis() - lastForwardTime);
    
    if (hubRegistered) {
        Serial.print("Hub MAC: ");
        for (int i = 0; i < 6; i++) {
            Serial.printf("%02X", hubMAC[i]);
            if (i < 5) Serial.print(":");
        }
        Serial.println();
    }
    Serial.println("===========================\n");
}

void setup() {
    Serial.begin(115200);
    delay(100);
    
    bootTime = millis();
    
    Serial.println("\n\n========================================");
    Serial.println("    ESP-NOW Repeater Node");
    Serial.println("    Range Extender for Hub");
    Serial.println("========================================\n");
    
    setupESPNow();
    
    // Send initial announce
    delay(1000);
    sendAnnounce();
    
    Serial.println("Repeater ready - listening for messages...\n");
}

void loop() {
    static unsigned long lastAnnounce = 0;
    static unsigned long lastHeartbeat = 0;
    static unsigned long lastStats = 0;
    
    unsigned long now = millis();
    
    // Send ANNOUNCE every 30 seconds if hub not registered
    if (!hubRegistered && (now - lastAnnounce >= 30000)) {
        sendAnnounce();
        lastAnnounce = now;
    }
    
    // Send heartbeat every 60 seconds if hub registered
    if (hubRegistered && (now - lastHeartbeat >= 60000)) {
        sendHeartbeat();
        lastHeartbeat = now;
    }
    
    // Print statistics every 5 minutes
    if (now - lastStats >= 300000) {
        printStats();
        lastStats = now;
    }
    
    // Check for fail-safe timeout (no hub communication for 3 minutes)
    if (hubRegistered && (now - lastForwardTime >= 180000)) {
        Serial.println("[WARN] No messages for 3 minutes - hub may be offline");
    }
    
    delay(10);  // Small delay to prevent watchdog issues
}
