#ifndef ESPNOW_MANAGER_H
#define ESPNOW_MANAGER_H

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
#include <queue>
#include <map>

// ============================================================================
// CONSTANTS
// ============================================================================

#define ESPNOW_MAX_DATA_LEN 250
#define ESPNOW_FRAGMENT_SIZE 32  // Size of commandData in CommandMessage
#define ESPNOW_MAX_MESSAGE_SIZE 512  // Maximum size for large messages
#define ESPNOW_REASSEMBLY_TIMEOUT_MS 1500
#define ESPNOW_MAX_RETRIES 3
#define ESPNOW_RETRY_BASE_DELAY_MS 100
#define ESPNOW_RX_QUEUE_SIZE 10

// ============================================================================
// STRUCTURES
// ============================================================================

/**
 * @brief Reassembly context for multi-part messages (node-side)
 */
struct ReassemblyContext {
    bool active;
    uint8_t commandId;        // Command ID being reassembled
    uint8_t expectedSeqID;    // Next expected sequence ID
    uint32_t startTime;       // When reassembly started
    uint8_t buffer[ESPNOW_MAX_MESSAGE_SIZE];
    size_t offset;            // Current buffer offset
    uint8_t senderMac[6];     // MAC of sender
};

/**
 * @brief Message queue entry for RX processing
 */
struct RxQueueEntry {
    uint8_t mac[6];
    uint8_t data[ESPNOW_MAX_DATA_LEN];
    int len;
};

/**
 * @brief TX retry context (hub-side)
 */
struct RetryContext {
    uint8_t destMac[6];
    uint8_t data[ESPNOW_MAX_DATA_LEN];
    size_t len;
    uint8_t attemptsRemaining;
    uint32_t nextRetryTime;
    bool active;
};

/**
 * @brief Peer status tracking
 */
struct PeerStatus {
    uint8_t mac[6];
    bool online;
    uint32_t lastHeartbeat;
    uint8_t lastSeqReceived;  // For duplicate detection
};

// ============================================================================
// ESPNOW MANAGER CLASS
// ============================================================================

class ESPNowManager {
public:
    // ========================================================================
    // SINGLETON ACCESS
    // ========================================================================
    
    static ESPNowManager& getInstance() {
        static ESPNowManager instance;
        return instance;
    }
    
    // Delete copy constructor and assignment operator
    ESPNowManager(const ESPNowManager&) = delete;
    ESPNowManager& operator=(const ESPNowManager&) = delete;
    
    // ========================================================================
    // INITIALIZATION
    // ========================================================================
    
    /**
     * @brief Initialize ESP-NOW manager
     * @param channel WiFi channel (1-13, typically 6)
     * @param isHub True if this is the hub, false if node
     * @return true if successful
     */
    bool begin(uint8_t channel, bool isHub);
    
    /**
     * @brief Add peer to ESP-NOW
     * @param mac MAC address of peer
     * @return true if successful
     */
    bool addPeer(const uint8_t* mac);
    
    /**
     * @brief Remove peer from ESP-NOW
     * @param mac MAC address of peer
     * @return true if successful
     */
    bool removePeer(const uint8_t* mac);
    
    // ========================================================================
    // SENDING (HUB-SIDE)
    // ========================================================================
    
    /**
     * @brief Send small message (single frame)
     * @param mac Destination MAC address
     * @param data Message data
     * @param len Message length
     * @param checkOnline If true, check peer is online before sending
     * @return true if sent successfully
     */
    bool send(const uint8_t* mac, const uint8_t* data, size_t len, bool checkOnline = false);
    
    /**
     * @brief Send large message with automatic fragmentation
     * @param mac Destination MAC address
     * @param commandId Unique command ID
     * @param data Large data buffer
     * @param len Data length (up to ESPNOW_MAX_MESSAGE_SIZE)
     * @param checkOnline If true, check peer is online before sending
     * @return true if all fragments sent successfully
     */
    bool sendFragmented(const uint8_t* mac, uint8_t commandId, 
                       const uint8_t* data, size_t len, bool checkOnline = false);
    
    /**
     * @brief Send with automatic retry on failure
     * @param mac Destination MAC address
     * @param data Message data
     * @param len Message length
     * @param maxRetries Maximum retry attempts
     * @return true if sent successfully (possibly after retries)
     */
    bool sendWithRetry(const uint8_t* mac, const uint8_t* data, size_t len, uint8_t maxRetries = ESPNOW_MAX_RETRIES);
    
    // ========================================================================
    // RECEIVING (COMMON)
    // ========================================================================
    
    /**
     * @brief Process messages from RX queue
     * Must be called regularly from main loop
     */
    void processQueue();
    
    /**
     * @brief Set callback for received complete commands
     * @param callback Function to call when command reassembly complete
     */
    void onCommandReceived(void (*callback)(const uint8_t* mac, const uint8_t* data, size_t len));
    
    /**
     * @brief Set callback for received status messages
     * @param callback Function to call when status received
     */
    void onStatusReceived(void (*callback)(const uint8_t* mac, const StatusMessage& status));
    
    /**
     * @brief Set callback for received heartbeat messages
     * @param callback Function to call when heartbeat received
     */
    void onHeartbeatReceived(void (*callback)(const uint8_t* mac, const HeartbeatMessage& heartbeat));
    
    /**
     * @brief Set callback for received announce messages
     * @param callback Function to call when announce received
     */
    void onAnnounceReceived(void (*callback)(const uint8_t* mac, const AnnounceMessage& announce));
    
    /**
     * @brief Set callback for received ACK messages
     * @param callback Function to call when ACK received
     */
    void onAckReceived(void (*callback)(const uint8_t* mac, const AckMessage& ack));
    
    /**
     * @brief Set callback for received CONFIG messages
     * @param callback Function to call when CONFIG received
     */
    void onConfigReceived(void (*callback)(const uint8_t* mac, const ConfigMessage& config));
    
    /**
     * @brief Set callback for received UNMAP messages
     * @param callback Function to call when UNMAP received
     */
    void onUnmapReceived(void (*callback)(const uint8_t* mac, const UnmapMessage& unmap));
    
    // ========================================================================
    // PEER STATUS (HUB-SIDE)
    // ========================================================================
    
    /**
     * @brief Update peer status (online/offline)
     * @param mac Peer MAC address
     * @param online True if online
     */
    void setPeerOnline(const uint8_t* mac, bool online);
    
    /**
     * @brief Check if peer is online
     * @param mac Peer MAC address
     * @return true if peer is online
     */
    bool isPeerOnline(const uint8_t* mac) const;
    
    /**
     * @brief Update peer heartbeat timestamp
     * @param mac Peer MAC address
     */
    void updatePeerHeartbeat(const uint8_t* mac);
    
    /**
     * @brief Check all peers for heartbeat timeout
     * @param timeoutMs Timeout in milliseconds
     * @return Number of peers marked offline
     */
    int checkPeerTimeouts(uint32_t timeoutMs);
    
    // ========================================================================
    // DIAGNOSTICS
    // ========================================================================
    
    /**
     * @brief Get statistics
     */
    struct Statistics {
        uint32_t messagesSent;
        uint32_t messagesReceived;
        uint32_t sendFailures;
        uint32_t retries;
        uint32_t fragmentsSent;
        uint32_t fragmentsReceived;
        uint32_t reassemblyTimeouts;
        uint32_t duplicatesIgnored;
    };
    
    Statistics getStatistics() const { return _stats; }
    void resetStatistics() { _stats = {}; }
    void printStatistics() const;
    
private:
    // ========================================================================
    // PRIVATE CONSTRUCTOR
    // ========================================================================
    
    ESPNowManager();
    ~ESPNowManager();
    
    // ========================================================================
    // INTERNAL STATE
    // ========================================================================
    
    bool _initialized;
    bool _isHub;
    uint8_t _channel;
    
    // Message queue (for ISR-safe processing)
#ifdef ESP32
    QueueHandle_t _rxQueue;
#else
    std::queue<RxQueueEntry> _rxQueue;  // ESP8266 doesn't have FreeRTOS queues
#endif
    
    // Reassembly context (node-side)
    ReassemblyContext _reassembly;
    
    // Peer tracking (hub-side)
    std::map<uint64_t, PeerStatus> _peers;  // Key: MAC as uint64_t
    
    // Retry contexts (hub-side)
    std::vector<RetryContext> _retryQueue;
    
    // Callbacks
    void (*_commandCallback)(const uint8_t* mac, const uint8_t* data, size_t len);
    void (*_statusCallback)(const uint8_t* mac, const StatusMessage& status);
    void (*_heartbeatCallback)(const uint8_t* mac, const HeartbeatMessage& heartbeat);
    void (*_ackCallback)(const uint8_t* mac, const AckMessage& ack);
    void (*_announceCallback)(const uint8_t* mac, const AnnounceMessage& announce);
    void (*_configCallback)(const uint8_t* mac, const ConfigMessage& config);
    void (*_unmapCallback)(const uint8_t* mac, const UnmapMessage& unmap);
    
    // Statistics
    Statistics _stats;
    
    // ========================================================================
    // INTERNAL HELPERS
    // ========================================================================
    
    /**
     * @brief Convert MAC address to uint64_t key
     */
    static uint64_t macToKey(const uint8_t* mac);
    
    /**
     * @brief ESP-NOW receive callback (static, forwards to instance)
     */
#ifdef ESP8266
    static void onReceiveStatic(uint8_t* mac, uint8_t* data, uint8_t len);
    static void onSendStatic(uint8_t* mac, uint8_t status);
#else
    static void onReceiveStatic(const uint8_t* mac, const uint8_t* data, int len);
    static void onSendStatic(const uint8_t* mac, esp_now_send_status_t status);
#endif
    
    /**
     * @brief Process received message (called from processQueue)
     */
    void processReceivedMessage(const uint8_t* mac, const uint8_t* data, int len);
    
    /**
     * @brief Process command message (handles reassembly)
     */
    void processCommand(const uint8_t* mac, const CommandMessage& cmd);
    
    /**
     * @brief Process status message
     */
    void processStatus(const uint8_t* mac, const StatusMessage& status);
    
    /**
     * @brief Process heartbeat message
     */
    void processHeartbeat(const uint8_t* mac, const HeartbeatMessage& heartbeat);
    
    /**
     * @brief Process announce message
     */
    void processAnnounce(const uint8_t* mac, const AnnounceMessage& announce);
    
    /**
     * @brief Check reassembly timeout and reset if needed
     */
    void checkReassemblyTimeout();
    
    /**
     * @brief Reset reassembly context
     */
    void resetReassembly();
    
    /**
     * @brief Check if message is duplicate
     * @return true if duplicate (should be ignored)
     */
    bool isDuplicate(const uint8_t* mac, uint8_t sequenceNum);
    
    /**
     * @brief Process retry queue
     */
    void processRetries();
    
    /**
     * @brief Add message to retry queue
     */
    void addToRetryQueue(const uint8_t* mac, const uint8_t* data, size_t len);
};

#endif // ESPNOW_MANAGER_H
