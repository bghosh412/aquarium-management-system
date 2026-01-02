#include "ESPNowManager.h"

// ============================================================================
// STATIC MEMBERS
// ============================================================================

static ESPNowManager* s_instance = nullptr;

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

ESPNowManager::ESPNowManager()
    : _initialized(false)
    , _isHub(false)
    , _channel(6)
#ifdef ESP32
    , _rxQueue(nullptr)
#endif
    , _commandCallback(nullptr)
    , _statusCallback(nullptr)
    , _heartbeatCallback(nullptr)
    , _announceCallback(nullptr)
    , _ackCallback(nullptr)
    , _configCallback(nullptr)
{
    s_instance = this;
    memset(&_reassembly, 0, sizeof(_reassembly));
    memset(&_stats, 0, sizeof(_stats));
}

ESPNowManager::~ESPNowManager() {
#ifdef ESP32
    if (_rxQueue) {
        vQueueDelete(_rxQueue);
    }
#endif
}

// ============================================================================
// INITIALIZATION
// ============================================================================

bool ESPNowManager::begin(uint8_t channel, bool isHub) {
    if (_initialized) {
        Serial.println("⚠️  ESPNowManager already initialized");
        return true;
    }
    
    _channel = channel;
    _isHub = isHub;
    
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    Serial.printf("🚀 ESPNowManager: Initializing as %s\n", isHub ? "HUB" : "NODE");
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    
    // Create RX queue
#ifdef ESP32
    _rxQueue = xQueueCreate(ESPNOW_RX_QUEUE_SIZE, sizeof(RxQueueEntry));
    if (!_rxQueue) {
        Serial.println("❌ Failed to create RX queue");
        return false;
    }
    Serial.printf("✅ RX Queue created (%d entries)\n", ESPNOW_RX_QUEUE_SIZE);
#else
    Serial.printf("✅ RX Queue initialized (ESP8266 std::queue)\n");
#endif
    
    // Set WiFi channel
#ifdef ESP8266
    wifi_set_channel(_channel);
#else
    esp_wifi_set_channel(_channel, WIFI_SECOND_CHAN_NONE);
#endif
    Serial.printf("✅ WiFi Channel: %d\n", _channel);
    
    // Initialize ESP-NOW
#ifdef ESP8266
    if (esp_now_init() != 0) {
#else
    if (esp_now_init() != ESP_OK) {
#endif
        Serial.println("❌ ESP-NOW init failed");
        return false;
    }
    Serial.println("✅ ESP-NOW initialized");
    
    // Register callbacks
#ifdef ESP8266
    esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
    esp_now_register_recv_cb(onReceiveStatic);
    esp_now_register_send_cb(onSendStatic);
#else
    esp_now_register_recv_cb(onReceiveStatic);
    esp_now_register_send_cb(onSendStatic);
#endif
    Serial.println("✅ Callbacks registered");
    
    _initialized = true;
    
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    Serial.println("✅ ESPNowManager Ready");
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    
    return true;
}

bool ESPNowManager::addPeer(const uint8_t* mac) {
    if (!_initialized) return false;
    
#ifdef ESP8266
    if (esp_now_add_peer((uint8_t*)mac, ESP_NOW_ROLE_COMBO, _channel, NULL, 0) != 0) {
        Serial.printf("❌ Failed to add peer %02X:%02X:...\n", mac[0], mac[1]);
        return false;
    }
#else
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, mac, 6);
    peerInfo.channel = _channel;
    peerInfo.encrypt = false;
    
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.printf("❌ Failed to add peer %02X:%02X:...\n", mac[0], mac[1]);
        return false;
    }
#endif
    
    // Track peer if hub
    if (_isHub) {
        uint64_t key = macToKey(mac);
        PeerStatus& peer = _peers[key];
        memcpy(peer.mac, mac, 6);
        peer.online = true;
        peer.lastHeartbeat = millis();
        peer.lastSeqReceived = 0;
    }
    
    Serial.printf("✅ Added peer %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    return true;
}

bool ESPNowManager::removePeer(const uint8_t* mac) {
    if (!_initialized) return false;
    
#ifdef ESP8266
    if (esp_now_del_peer((uint8_t*)mac) != 0) {
#else
    if (esp_now_del_peer(mac) != ESP_OK) {
#endif
        return false;
    }
    
    // Remove from tracking
    if (_isHub) {
        uint64_t key = macToKey(mac);
        _peers.erase(key);
    }
    
    Serial.printf("🗑️  Removed peer %02X:%02X:...\n", mac[0], mac[1]);
    return true;
}

// ============================================================================
// SENDING (HUB-SIDE)
// ============================================================================

bool ESPNowManager::send(const uint8_t* mac, const uint8_t* data, size_t len, bool checkOnline) {
    if (!_initialized) {
        Serial.println("❌ ESPNowManager not initialized");
        return false;
    }
    
    if (len > ESPNOW_MAX_DATA_LEN) {
        Serial.printf("❌ Message too large: %d bytes (max %d)\n", len, ESPNOW_MAX_DATA_LEN);
        return false;
    }
    
    // Check if peer is online (hub only)
    if (_isHub && checkOnline && !isPeerOnline(mac)) {
        Serial.println("⚠️  Peer offline, message not sent");
        return false;
    }
    
    // Send message
#ifdef ESP8266
    int result = esp_now_send((uint8_t*)mac, (uint8_t*)data, len);
    bool success = (result == 0);
#else
    esp_err_t result = esp_now_send(mac, data, len);
    bool success = (result == ESP_OK);
#endif
    
    if (success) {
        _stats.messagesSent++;
    } else {
        _stats.sendFailures++;
        Serial.printf("❌ Send failed: %d\n", result);
    }
    
    return success;
}

bool ESPNowManager::sendFragmented(const uint8_t* mac, uint8_t commandId, 
                                   const uint8_t* data, size_t len, bool checkOnline) {
    if (!_initialized) return false;
    
    if (len > ESPNOW_MAX_MESSAGE_SIZE) {
        Serial.printf("❌ Message too large: %d bytes (max %d)\n", len, ESPNOW_MAX_MESSAGE_SIZE);
        return false;
    }
    
    // Check if peer is online
    if (_isHub && checkOnline && !isPeerOnline(mac)) {
        Serial.println("⚠️  Peer offline, fragmented message not sent");
        return false;
    }
    
    Serial.printf("📦 Fragmenting message: %d bytes into %d-byte chunks\n", 
                  len, ESPNOW_FRAGMENT_SIZE);
    
    size_t offset = 0;
    uint8_t seqID = 0;
    
    while (offset < len) {
        size_t remainingBytes = len - offset;
        size_t chunkSize = (remainingBytes < ESPNOW_FRAGMENT_SIZE) ? remainingBytes : ESPNOW_FRAGMENT_SIZE;
        bool isFinal = (offset + chunkSize >= len);
        
        // Build command message
        CommandMessage cmd = {};
        cmd.header.type = MessageType::COMMAND;
        cmd.header.tankId = 0;  // Set by caller if needed
        cmd.header.nodeType = NodeType::HUB;
        cmd.header.timestamp = millis();
        cmd.header.sequenceNum = 0;  // Will be set by send callback
        
        cmd.commandId = commandId;
        cmd.commandSeqID = seqID;
        cmd.finalCommand = isFinal;
        
        // Copy fragment data
        memcpy(cmd.commandData, data + offset, chunkSize);
        
        // Send fragment
        if (!send(mac, (uint8_t*)&cmd, sizeof(cmd), false)) {
            Serial.printf("❌ Failed to send fragment %d\n", seqID);
            return false;
        }
        
        _stats.fragmentsSent++;
        
        Serial.printf("  📤 Sent fragment %d/%d (%d bytes)%s\n", 
                      seqID + 1, 
                      (len + ESPNOW_FRAGMENT_SIZE - 1) / ESPNOW_FRAGMENT_SIZE,
                      chunkSize,
                      isFinal ? " [FINAL]" : "");
        
        offset += chunkSize;
        seqID++;
        
        // Small delay between fragments to avoid overwhelming receiver
        delay(10);
    }
    
    Serial.printf("✅ Sent %d fragments successfully\n", seqID);
    return true;
}

bool ESPNowManager::sendWithRetry(const uint8_t* mac, const uint8_t* data, size_t len, uint8_t maxRetries) {
    if (!_initialized) return false;
    
    for (uint8_t attempt = 0; attempt <= maxRetries; attempt++) {
        if (attempt > 0) {
            _stats.retries++;
            uint32_t delayMs = ESPNOW_RETRY_BASE_DELAY_MS * (1 << (attempt - 1));  // Exponential backoff
            Serial.printf("🔄 Retry %d/%d (delay %dms)\n", attempt, maxRetries, delayMs);
            delay(delayMs);
        }
        
        if (send(mac, data, len, false)) {
            if (attempt > 0) {
                Serial.printf("✅ Sent successfully after %d retries\n", attempt);
            }
            return true;
        }
    }
    
    Serial.printf("❌ Failed after %d retries\n", maxRetries);
    return false;
}

// ============================================================================
// RECEIVING (COMMON)
// ============================================================================

void ESPNowManager::processQueue() {
    if (!_initialized) return;
    
    // Process retries (hub only)
    if (_isHub) {
        processRetries();
    }
    
    // Process RX queue
#ifdef ESP32
    RxQueueEntry entry;
    while (xQueueReceive(_rxQueue, &entry, 0) == pdTRUE) {
        processReceivedMessage(entry.mac, entry.data, entry.len);
    }
#else
    // ESP8266: Process all queued messages
    while (!_rxQueue.empty()) {
        RxQueueEntry entry = _rxQueue.front();
        _rxQueue.pop();
        processReceivedMessage(entry.mac, entry.data, entry.len);
    }
#endif
    
    // Check reassembly timeout (node only)
    if (!_isHub) {
        checkReassemblyTimeout();
    }
}

void ESPNowManager::onCommandReceived(void (*callback)(const uint8_t* mac, const uint8_t* data, size_t len)) {
    _commandCallback = callback;
}

void ESPNowManager::onStatusReceived(void (*callback)(const uint8_t* mac, const StatusMessage& status)) {
    _statusCallback = callback;
}

void ESPNowManager::onHeartbeatReceived(void (*callback)(const uint8_t* mac, const HeartbeatMessage& heartbeat)) {
    _heartbeatCallback = callback;
}

void ESPNowManager::onAnnounceReceived(void (*callback)(const uint8_t* mac, const AnnounceMessage& announce)) {
    _announceCallback = callback;
}

void ESPNowManager::onAckReceived(void (*callback)(const uint8_t* mac, const AckMessage& ack)) {
    _ackCallback = callback;
}

void ESPNowManager::onConfigReceived(void (*callback)(const uint8_t* mac, const ConfigMessage& config)) {
    _configCallback = callback;
}

// ============================================================================
// PEER STATUS (HUB-SIDE)
// ============================================================================

void ESPNowManager::setPeerOnline(const uint8_t* mac, bool online) {
    if (!_isHub) return;
    
    uint64_t key = macToKey(mac);
    auto it = _peers.find(key);
    
    if (it != _peers.end()) {
        bool wasOnline = it->second.online;
        it->second.online = online;
        
        if (online && !wasOnline) {
            Serial.printf("✅ Peer %02X:%02X:... is now ONLINE\n", mac[0], mac[1]);
        } else if (!online && wasOnline) {
            Serial.printf("⚠️  Peer %02X:%02X:... is now OFFLINE\n", mac[0], mac[1]);
        }
    }
}

bool ESPNowManager::isPeerOnline(const uint8_t* mac) const {
    if (!_isHub) return true;  // Nodes don't track peer status
    
    uint64_t key = macToKey(mac);
    auto it = _peers.find(key);
    
    return (it != _peers.end()) && it->second.online;
}

void ESPNowManager::updatePeerHeartbeat(const uint8_t* mac) {
    if (!_isHub) return;
    
    uint64_t key = macToKey(mac);
    auto it = _peers.find(key);
    
    if (it != _peers.end()) {
        it->second.lastHeartbeat = millis();
        
        // Mark online if was offline
        if (!it->second.online) {
            setPeerOnline(mac, true);
        }
    }
}

int ESPNowManager::checkPeerTimeouts(uint32_t timeoutMs) {
    if (!_isHub) return 0;
    
    int offlineCount = 0;
    uint32_t now = millis();
    
    for (auto& pair : _peers) {
        PeerStatus& peer = pair.second;
        
        if (peer.online && (now - peer.lastHeartbeat > timeoutMs)) {
            setPeerOnline(peer.mac, false);
            offlineCount++;
        }
    }
    
    return offlineCount;
}

// ============================================================================
// DIAGNOSTICS
// ============================================================================

void ESPNowManager::printStatistics() const {
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    Serial.println("📊 ESPNowManager Statistics");
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    Serial.printf("📤 Messages Sent:        %u\n", _stats.messagesSent);
    Serial.printf("📥 Messages Received:    %u\n", _stats.messagesReceived);
    Serial.printf("❌ Send Failures:        %u\n", _stats.sendFailures);
    Serial.printf("🔄 Retries:              %u\n", _stats.retries);
    Serial.printf("📦 Fragments Sent:       %u\n", _stats.fragmentsSent);
    Serial.printf("🧩 Fragments Received:   %u\n", _stats.fragmentsReceived);
    Serial.printf("⏱️  Reassembly Timeouts:  %u\n", _stats.reassemblyTimeouts);
    Serial.printf("🚫 Duplicates Ignored:   %u\n", _stats.duplicatesIgnored);
    
    if (_isHub) {
        Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        Serial.printf("👥 Tracked Peers:        %d\n", _peers.size());
        
        int onlineCount = 0;
        for (const auto& pair : _peers) {
            if (pair.second.online) onlineCount++;
        }
        Serial.printf("   - Online:             %d\n", onlineCount);
        Serial.printf("   - Offline:            %d\n", _peers.size() - onlineCount);
    }
    
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
}

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

uint64_t ESPNowManager::macToKey(const uint8_t* mac) {
    uint64_t key = 0;
    for (int i = 0; i < 6; i++) {
        key |= ((uint64_t)mac[i]) << (i * 8);
    }
    return key;
}

#ifdef ESP8266
void ESPNowManager::onReceiveStatic(uint8_t* mac, uint8_t* data, uint8_t len) {
#else
void ESPNowManager::onReceiveStatic(const uint8_t* mac, const uint8_t* data, int len) {
#endif
    if (!s_instance || !s_instance->_initialized) return;
    
    // Queue message for processing in main loop (ISR-safe)
    RxQueueEntry entry;
    memcpy(entry.mac, mac, 6);
    memcpy(entry.data, data, len);
    entry.len = len;
    
#ifdef ESP32
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(s_instance->_rxQueue, &entry, &xHigherPriorityTaskWoken);
    
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
#else
    // ESP8266: Direct queue (not ISR-safe but no FreeRTOS)
    s_instance->_rxQueue.push(entry);
#endif
}

#ifdef ESP8266
void ESPNowManager::onSendStatic(uint8_t* mac, uint8_t status) {
    // Track send status if needed
}
#else
void ESPNowManager::onSendStatic(const uint8_t* mac, esp_now_send_status_t status) {
    // Track send status if needed
}
#endif

void ESPNowManager::processReceivedMessage(const uint8_t* mac, const uint8_t* data, int len) {
    _stats.messagesReceived++;
    
    // Validate minimum size
    if (len < sizeof(MessageHeader)) {
        Serial.println("❌ Message too small");
        return;
    }
    
    MessageHeader* header = (MessageHeader*)data;
    
    // Check for duplicate (sequence number validation)
    if (isDuplicate(mac, header->sequenceNum)) {
        _stats.duplicatesIgnored++;
        Serial.printf("🚫 Duplicate message ignored (seq %d)\n", header->sequenceNum);
        return;
    }
    
    // Route based on message type
    switch (header->type) {
        case MessageType::COMMAND:
            if (len >= sizeof(CommandMessage)) {
                processCommand(mac, *(CommandMessage*)data);
            }
            break;
            
        case MessageType::STATUS:
            if (len >= sizeof(StatusMessage)) {
                processStatus(mac, *(StatusMessage*)data);
            }
            break;
            
        case MessageType::HEARTBEAT:
            if (len >= sizeof(HeartbeatMessage)) {
                processHeartbeat(mac, *(HeartbeatMessage*)data);
            }
            break;
            
        case MessageType::ANNOUNCE:
            if (len >= sizeof(AnnounceMessage)) {
                processAnnounce(mac, *(AnnounceMessage*)data);
            }
            break;
            
        case MessageType::ACK:
            // Node receives ACK from hub
            if (len >= sizeof(AckMessage)) {
                const AckMessage* ack = (const AckMessage*)data;
                if (_ackCallback) {
                    _ackCallback(mac, *ack);
                }
            }
            break;
            
        case MessageType::CONFIG:
            // Node receives CONFIG from hub (provisioning)
            if (len >= sizeof(ConfigMessage)) {
                const ConfigMessage* config = (const ConfigMessage*)data;
                if (_configCallback) {
                    _configCallback(mac, *config);
                }
            }
            break;
            
        default:
            Serial.printf("⚠️  Unknown message type: 0x%02X\n", (uint8_t)header->type);
            break;
    }
}

void ESPNowManager::processCommand(const uint8_t* mac, const CommandMessage& cmd) {
    // Commands are for nodes only
    if (_isHub) {
        Serial.println("⚠️  Hub received COMMAND (unexpected)");
        return;
    }
    
    _stats.fragmentsReceived++;
    
    // Check if this is a fragmented message
    if (cmd.commandSeqID == 0 && cmd.finalCommand) {
        // Single-frame command - process immediately
        if (_commandCallback) {
            _commandCallback(mac, cmd.commandData, ESPNOW_FRAGMENT_SIZE);
        }
        return;
    }
    
    // Multi-frame command - reassemble
    
    // Check timeout on active reassembly
    if (_reassembly.active && (millis() - _reassembly.startTime > ESPNOW_REASSEMBLY_TIMEOUT_MS)) {
        Serial.println("⏱️  Reassembly timeout, dropping partial message");
        _stats.reassemblyTimeouts++;
        resetReassembly();
    }
    
    // Start new reassembly
    if (!_reassembly.active) {
        if (cmd.commandSeqID != 0) {
            Serial.println("⚠️  Fragment doesn't start at 0, ignoring");
            return;
        }
        
        Serial.printf("🧩 Starting reassembly for command %d\n", cmd.commandId);
        _reassembly.active = true;
        _reassembly.commandId = cmd.commandId;
        _reassembly.expectedSeqID = 0;
        _reassembly.startTime = millis();
        _reassembly.offset = 0;
        memcpy(_reassembly.senderMac, mac, 6);
    }
    
    // Validate sequence
    if (cmd.commandId != _reassembly.commandId) {
        Serial.println("⚠️  Command ID mismatch, dropping reassembly");
        resetReassembly();
        return;
    }
    
    if (cmd.commandSeqID != _reassembly.expectedSeqID) {
        Serial.printf("⚠️  Sequence mismatch: expected %d, got %d\n", 
                     _reassembly.expectedSeqID, cmd.commandSeqID);
        resetReassembly();
        return;
    }
    
    // Append fragment
    if (_reassembly.offset + ESPNOW_FRAGMENT_SIZE > ESPNOW_MAX_MESSAGE_SIZE) {
        Serial.println("❌ Reassembly buffer overflow");
        resetReassembly();
        return;
    }
    
    memcpy(_reassembly.buffer + _reassembly.offset, cmd.commandData, ESPNOW_FRAGMENT_SIZE);
    _reassembly.offset += ESPNOW_FRAGMENT_SIZE;
    _reassembly.expectedSeqID++;
    
    Serial.printf("  🧩 Fragment %d appended (%d bytes total)\n", 
                 cmd.commandSeqID, _reassembly.offset);
    
    // Check if complete
    if (cmd.finalCommand) {
        Serial.printf("✅ Reassembly complete: %d bytes\n", _reassembly.offset);
        
        if (_commandCallback) {
            _commandCallback(_reassembly.senderMac, _reassembly.buffer, _reassembly.offset);
        }
        
        resetReassembly();
    }
}

void ESPNowManager::processStatus(const uint8_t* mac, const StatusMessage& status) {
    if (_statusCallback) {
        _statusCallback(mac, status);
    }
}

void ESPNowManager::processHeartbeat(const uint8_t* mac, const HeartbeatMessage& heartbeat) {
    // Update peer status (hub only)
    if (_isHub) {
        updatePeerHeartbeat(mac);
    }
    
    if (_heartbeatCallback) {
        _heartbeatCallback(mac, heartbeat);
    }
}

void ESPNowManager::processAnnounce(const uint8_t* mac, const AnnounceMessage& announce) {
    if (_announceCallback) {
        _announceCallback(mac, announce);
    }
}

void ESPNowManager::checkReassemblyTimeout() {
    if (_reassembly.active && (millis() - _reassembly.startTime > ESPNOW_REASSEMBLY_TIMEOUT_MS)) {
        Serial.println("⏱️  Reassembly timeout");
        _stats.reassemblyTimeouts++;
        resetReassembly();
    }
}

void ESPNowManager::resetReassembly() {
    memset(&_reassembly, 0, sizeof(_reassembly));
}

bool ESPNowManager::isDuplicate(const uint8_t* mac, uint8_t sequenceNum) {
    if (!_isHub) return false;  // Nodes don't track duplicates (handled by hub)
    
    uint64_t key = macToKey(mac);
    auto it = _peers.find(key);
    
    if (it == _peers.end()) {
        return false;  // Unknown peer, not a duplicate
    }
    
    PeerStatus& peer = it->second;
    
    // Check if this is the same sequence as last received
    bool isDup = (sequenceNum == peer.lastSeqReceived && sequenceNum != 0);
    
    // Update last received sequence
    peer.lastSeqReceived = sequenceNum;
    
    return isDup;
}

void ESPNowManager::processRetries() {
    // Process pending retries (hub only)
    if (_retryQueue.empty()) return;
    
    uint32_t now = millis();
    
    for (auto it = _retryQueue.begin(); it != _retryQueue.end(); ) {
        RetryContext& ctx = *it;
        
        if (!ctx.active) {
            it = _retryQueue.erase(it);
            continue;
        }
        
        // Check if retry time reached
        if (now >= ctx.nextRetryTime) {
            if (ctx.attemptsRemaining > 0) {
                // Retry send
                if (send(ctx.destMac, ctx.data, ctx.len, false)) {
                    // Success - remove from retry queue
                    Serial.printf("✅ Retry successful for %02X:%02X:...\n", 
                                 ctx.destMac[0], ctx.destMac[1]);
                    it = _retryQueue.erase(it);
                    continue;
                } else {
                    // Failed - schedule next retry
                    ctx.attemptsRemaining--;
                    _stats.retries++;
                    
                    uint8_t attemptNum = ESPNOW_MAX_RETRIES - ctx.attemptsRemaining;
                    uint32_t delayMs = ESPNOW_RETRY_BASE_DELAY_MS * (1 << attemptNum);
                    ctx.nextRetryTime = now + delayMs;
                    
                    Serial.printf("🔄 Retry %d/%d scheduled in %dms\n", 
                                 attemptNum, ESPNOW_MAX_RETRIES, delayMs);
                }
            } else {
                // No more retries - give up
                Serial.printf("❌ Retry failed after %d attempts\n", ESPNOW_MAX_RETRIES);
                it = _retryQueue.erase(it);
                continue;
            }
        }
        
        ++it;
    }
}

void ESPNowManager::addToRetryQueue(const uint8_t* mac, const uint8_t* data, size_t len) {
    if (!_isHub || len > ESPNOW_MAX_DATA_LEN) return;
    
    RetryContext ctx;
    memcpy(ctx.destMac, mac, 6);
    memcpy(ctx.data, data, len);
    ctx.len = len;
    ctx.attemptsRemaining = ESPNOW_MAX_RETRIES;
    ctx.nextRetryTime = millis() + ESPNOW_RETRY_BASE_DELAY_MS;
    ctx.active = true;
    
    _retryQueue.push_back(ctx);
    Serial.println("📋 Message added to retry queue");
}
