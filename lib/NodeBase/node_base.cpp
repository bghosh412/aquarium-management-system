#include "node_base.h"

// ============================================================================
// SHARED NODE BASE IMPLEMENTATION
// ============================================================================
// This file contains ESP-NOW communication logic shared by all node types
// Each specific node (lighting, CO2, etc.) implements:
// - setupHardware()
// - enterFailSafeMode()
// - handleCommand()
// - updateHardware()
// ============================================================================

// ============================================================================
// ESP-NOW Communication Functions
// ============================================================================

void sendAnnounce() {
    AnnounceMessage msg = {};
    msg.header.type = MessageType::ANNOUNCE;
    msg.header.tankId = NODE_TANK_ID;
    msg.header.nodeType = NODE_TYPE;
    msg.header.timestamp = millis();
    msg.header.sequenceNum = messageSequence++;
    strncpy(msg.nodeName, NODE_NAME, MAX_NODE_NAME_LEN);
    msg.firmwareVersion = FIRMWARE_VERSION;
    msg.capabilities = 0;
    
    uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    
    #ifdef ESP8266
        esp_now_send(broadcastAddress, (uint8_t*)&msg, sizeof(msg));
    #else
        esp_now_send(broadcastAddress, (uint8_t*)&msg, sizeof(msg));
    #endif
    
    Serial.printf("📡 ANNOUNCE sent (attempt %lu)\n", announceAttempts++);
}

void sendHeartbeat() {
    if (!hubDiscovered) return;
    
    HeartbeatMessage msg = {};
    msg.header.type = MessageType::HEARTBEAT;
    msg.header.tankId = NODE_TANK_ID;
    msg.header.nodeType = NODE_TYPE;
    msg.header.timestamp = millis();
    msg.header.sequenceNum = messageSequence++;
    msg.health = 100;
    msg.uptimeMinutes = millis() / 60000;
    
    #ifdef ESP8266
        esp_now_send(hubMacAddress, (uint8_t*)&msg, sizeof(msg));
    #else
        esp_now_send(hubMacAddress, (uint8_t*)&msg, sizeof(msg));
    #endif
    
    lastHeartbeatSent = millis();
    Serial.println("💓 Heartbeat sent");
}

void sendStatus(uint8_t commandId, uint8_t statusCode, const uint8_t* data, size_t dataLen) {
    if (!hubDiscovered) return;
    
    StatusMessage msg = {};
    msg.header.type = MessageType::STATUS;
    msg.header.tankId = NODE_TANK_ID;
    msg.header.nodeType = NODE_TYPE;
    msg.header.timestamp = millis();
    msg.header.sequenceNum = messageSequence++;
    msg.commandId = commandId;  // Echo back the command ID for acknowledgment
    msg.statusCode = statusCode;
    
    // Copy status data (up to 32 bytes)
    if (data != nullptr && dataLen > 0) {
        size_t copyLen = (dataLen > 32) ? 32 : dataLen;
        memcpy(msg.statusData, data, copyLen);
    }
    
    #ifdef ESP8266
        esp_now_send(hubMacAddress, (uint8_t*)&msg, sizeof(msg));
    #else
        esp_now_send(hubMacAddress, (uint8_t*)&msg, sizeof(msg));
    #endif
    
    Serial.printf("📤 STATUS sent (cmdId=%d, status=%d)\n", commandId, statusCode);
}

// ============================================================================
// ESP-NOW Callbacks
// ============================================================================

#ifdef ESP8266
void onDataReceived(uint8_t* mac, uint8_t* data, uint8_t len) {
#else
void onDataReceived(const uint8_t* mac, const uint8_t* data, int len) {
#endif
    if (len < sizeof(MessageHeader)) {
        Serial.println("ERROR: Received message too small");
        return;
    }

    MessageHeader* header = (MessageHeader*)data;
    
    Serial.printf("RX from %02X:%02X:%02X:%02X:%02X:%02X - Type: %d\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                  (int)header->type);

    if (hubDiscovered && memcmp(mac, hubMacAddress, 6) != 0) {
        Serial.println("  Ignoring message from unknown sender");
        return;
    }

    lastHeartbeatReceived = millis();

    switch (header->type) {
        case MessageType::ACK: {
            if (len != sizeof(AckMessage)) {
                Serial.println("ERROR: Invalid ACK message size");
                return;
            }
            AckMessage* msg = (AckMessage*)data;
            
            if (msg->accepted && currentState == NodeState::WAITING_FOR_ACK) {
                Serial.printf("✓ ACK received - Assigned Node ID: %d\n", msg->assignedNodeId);
                
                if (!hubDiscovered) {
                    memcpy(hubMacAddress, mac, 6);
                    hubDiscovered = true;
                    
                    #ifdef ESP8266
                        esp_now_add_peer(hubMacAddress, ESP_NOW_ROLE_COMBO, ESPNOW_CHANNEL, NULL, 0);
                    #else
                        esp_now_peer_info_t peerInfo = {};
                        memcpy(peerInfo.peer_addr, hubMacAddress, 6);
                        peerInfo.channel = ESPNOW_CHANNEL;
                        peerInfo.encrypt = false;
                        esp_now_add_peer(&peerInfo);
                    #endif
                    
                    Serial.println("✓ Hub peer added - switching to unicast mode");
                }
                
                currentState = NodeState::CONNECTED;
            }
            break;
        }
        
        case MessageType::COMMAND: {
            if (len != sizeof(CommandMessage)) {
                Serial.println("ERROR: Invalid COMMAND message size");
                return;
            }
            
            if (currentState != NodeState::CONNECTED) {
                Serial.println("  Ignoring command - not connected");
                return;
            }
            
            CommandMessage* msg = (CommandMessage*)data;
            Serial.printf("  Command ID: %d, SeqID: %d, Final: %s\n",
                         msg->commandId, msg->commandSeqID, 
                         msg->finalCommand ? "YES" : "NO");
            
            handleCommand(msg);  // Call node-specific implementation
            
            // Send acknowledgment STATUS message
            uint8_t ackData[1] = {1};  // Simple ACK
            sendStatus(msg->commandId, 0, ackData, 1);  // statusCode 0 = success
            break;
        }
        
        case MessageType::HEARTBEAT: {
            Serial.println("  Hub heartbeat received");
            break;
        }
        
        default:
            Serial.printf("  Unknown message type: %d\n", (int)header->type);
            break;
    }
}

#ifdef ESP8266
void onDataSent(uint8_t* mac, uint8_t status) {
    Serial.printf("TX to %02X:%02X:%02X:%02X:%02X:%02X - %s\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                  status == 0 ? "OK" : "FAIL");
}
#else
void onDataSent(const uint8_t* mac, esp_now_send_status_t status) {
    Serial.printf("TX to %02X:%02X:%02X:%02X:%02X:%02X - %s\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                  status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}
#endif

// ============================================================================
// Setup and Loop Functions
// ============================================================================

void setupESPNow() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    
    Serial.print("MAC Address: ");
    Serial.println(WiFi.macAddress());
    
    #ifdef ESP8266
        wifi_set_channel(ESPNOW_CHANNEL);
    #else
        esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    #endif
    Serial.printf("WiFi Channel: %d\n", ESPNOW_CHANNEL);
    
    #ifdef ESP8266
        if (esp_now_init() != 0) {
    #else
        if (esp_now_init() != ESP_OK) {
    #endif
        Serial.println("✗ ESP-NOW init failed!");
        return;
    }
    Serial.println("✓ ESP-NOW initialized");
    
    #ifdef ESP8266
        esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
        esp_now_register_recv_cb(onDataReceived);
        esp_now_register_send_cb(onDataSent);
    #else
        esp_now_register_recv_cb(onDataReceived);
        esp_now_register_send_cb(onDataSent);
    #endif
}

void nodeLoop() {
    uint32_t now = millis();
    
    switch (currentState) {
        case NodeState::ANNOUNCING:
        case NodeState::WAITING_FOR_ACK:
            if (now - lastHeartbeatSent > ANNOUNCE_INTERVAL_MS) {
                sendAnnounce();
                lastHeartbeatSent = now;
                currentState = NodeState::WAITING_FOR_ACK;
            }
            break;
            
        case NodeState::CONNECTED:
            if (now - lastHeartbeatSent > HEARTBEAT_INTERVAL_MS) {
                sendHeartbeat();
            }
            
            if (now - lastHeartbeatReceived > CONNECTION_TIMEOUT_MS) {
                Serial.println("⚠️ Connection timeout - hub not responding");
                enterFailSafeMode();  // Call node-specific fail-safe
                currentState = NodeState::LOST_CONNECTION;
            }
            break;
            
        case NodeState::LOST_CONNECTION:
            if (now - lastHeartbeatSent > ANNOUNCE_INTERVAL_MS) {
                Serial.println("Attempting to reconnect...");
                hubDiscovered = false;
                announceAttempts = 0;
                currentState = NodeState::ANNOUNCING;
            }
            break;
            
        default:
            break;
    }
}
