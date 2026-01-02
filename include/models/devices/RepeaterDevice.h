#ifndef REPEATER_DEVICE_H
#define REPEATER_DEVICE_H

#include "models/Device.h"

/**
 * @brief ESP-NOW Repeater device
 * 
 * Extends ESP-NOW range by relaying messages between hub and nodes.
 * Passive device with no actuators or sensors.
 */
class RepeaterDevice : public Device {
public:
    /**
     * @brief Repeater statistics
     */
    struct Statistics {
        uint32_t messagesForwarded;     // Total messages relayed
        uint32_t messagesDropped;       // Dropped due to errors
        uint32_t hubMessages;           // From hub
        uint32_t nodeMessages;          // From nodes
        uint32_t lastResetTime;         // Stats reset timestamp
        
        Statistics() : messagesForwarded(0), messagesDropped(0), 
                      hubMessages(0), nodeMessages(0), lastResetTime(0) {}
    };
    
    /**
     * @brief Constructor
     * @param mac Device MAC address
     * @param name Device name
     */
    RepeaterDevice(const uint8_t* mac, const String& name);
    
    /**
     * @brief Destructor
     */
    ~RepeaterDevice() override;
    
    // ===== Getters =====
    Statistics getStatistics() const { return _stats; }
    uint32_t getMessagesForwarded() const { return _stats.messagesForwarded; }
    uint32_t getMessagesDropped() const { return _stats.messagesDropped; }
    float getForwardingSuccessRate() const;
    bool isActive() const { return _isActive; }
    
    // ===== Control Methods =====
    /**
     * @brief Enable/disable repeater
     * @param enable true = active, false = standby
     * @return true if command sent successfully
     */
    bool setActive(bool enable);
    
    /**
     * @brief Reset statistics
     * @return true if command sent successfully
     */
    bool resetStatistics();
    
    /**
     * @brief Request statistics update
     * @return true if command sent successfully
     */
    bool requestStatistics();
    
    // ===== State Management =====
    /**
     * @brief Update state from device status
     * @param status StatusMessage from device
     */
    void handleStatus(const StatusMessage& status) override;
    
    /**
     * @brief Trigger fail-safe (continue forwarding - passive)
     */
    void triggerFailSafe() override;
    
    // ===== Statistics Management =====
    /**
     * @brief Update local statistics from device
     * @param stats Statistics from repeater
     */
    void updateStatistics(const Statistics& stats);
    
    /**
     * @brief Get uptime percentage
     * @return Percentage of time online
     */
    float getUptimePercentage() const;
    
    // ===== Serialization =====
    String toJson() const override;
    bool fromJson(const String& json) override;
    
private:
    Statistics _stats;              // Forwarding statistics
    bool _isActive;                 // Is repeater active?
    uint32_t _totalOnlineTime;      // Cumulative online time (seconds)
    uint32_t _totalOfflineTime;     // Cumulative offline time (seconds)
    
    /**
     * @brief Build repeater command
     */
    void _buildRepeaterCommand(uint8_t* buffer, uint8_t cmdType);
    
    /**
     * @brief Parse statistics from status data
     */
    Statistics _parseStatistics(const uint8_t* data) const;
};

// Command types for repeater device
namespace RepeaterCommands {
    constexpr uint8_t CMD_SET_ACTIVE = 0x01;
    constexpr uint8_t CMD_RESET_STATS = 0x02;
    constexpr uint8_t CMD_REQUEST_STATS = 0x03;
}

#endif // REPEATER_DEVICE_H
