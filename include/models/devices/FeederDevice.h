#ifndef FEEDER_DEVICE_H
#define FEEDER_DEVICE_H

#include "models/Device.h"

/**
 * @brief Fish Feeder device controller
 * 
 * Controls servo-based feeding mechanism.
 * Dispenses food in configurable portions.
 */
class FeederDevice : public Device {
public:
    /**
     * @brief Feeding state
     */
    enum class State {
        IDLE,           // Ready to feed
        FEEDING,        // Currently dispensing
        RETURNING,      // Returning to home position
        ERROR           // Error state
    };
    
    /**
     * @brief Constructor
     * @param mac Device MAC address
     * @param name Device name
     */
    FeederDevice(const uint8_t* mac, const String& name);
    
    /**
     * @brief Destructor
     */
    ~FeederDevice() override;
    
    // ===== Getters =====
    State getState() const { return _state; }
    bool isFeeding() const { return _state == State::FEEDING; }
    uint8_t getLastPortions() const { return _lastPortions; }
    uint32_t getLastFeedTime() const { return _lastFeedTime; }
    uint32_t getTotalFeedings() const { return _totalFeedings; }
    uint32_t getTotalPortions() const { return _totalPortions; }
    uint8_t getMaxPortionsPerFeed() const { return _maxPortionsPerFeed; }
    uint32_t getMinFeedInterval() const { return _minFeedInterval; }
    
    // Settings
    void setMaxPortionsPerFeed(uint8_t max) { _maxPortionsPerFeed = max; }
    void setMinFeedInterval(uint32_t seconds) { _minFeedInterval = seconds; }
    
    // ===== Control Methods =====
    /**
     * @brief Feed fish
     * @param portions Number of portions (1-5)
     * @return true if command sent successfully
     */
    bool feed(uint8_t portions);
    
    /**
     * @brief Test feeding mechanism
     * @return true if command sent successfully
     */
    bool testFeed();
    
    /**
     * @brief Cancel ongoing feeding
     * @return true if command sent successfully
     */
    bool cancelFeed();
    
    // ===== State Management =====
    /**
     * @brief Update state from device status
     * @param status StatusMessage from device
     */
    void handleStatus(const StatusMessage& status) override;
    
    /**
     * @brief Trigger fail-safe (do nothing - safer to skip)
     */
    void triggerFailSafe() override;
    
    // ===== Safety Checks =====
    /**
     * @brief Check if can feed (respects interval)
     * @return true if allowed to feed
     */
    bool canFeedNow() const;
    
    /**
     * @brief Get time until next allowed feeding
     * @return Seconds remaining (0 if can feed now)
     */
    uint32_t getTimeUntilNextFeed() const;
    
    /**
     * @brief Validate portions count
     * @param portions Number to validate
     * @return Clamped value within limits
     */
    uint8_t validatePortions(uint8_t portions) const;
    
    // ===== Serialization =====
    String toJson() const override;
    bool fromJson(const String& json) override;
    
private:
    State _state;                   // Current state
    uint8_t _lastPortions;          // Last feed portions
    uint32_t _lastFeedTime;         // Last feed timestamp
    uint32_t _totalFeedings;        // Total feed count
    uint32_t _totalPortions;        // Total portions dispensed
    uint8_t _maxPortionsPerFeed;    // Max portions (default 5)
    uint32_t _minFeedInterval;      // Min seconds between feeds (default 3600)
    
    /**
     * @brief Build feeder command
     */
    void _buildFeederCommand(uint8_t* buffer, uint8_t cmdType, uint8_t portions);
};

// Command types for feeder device
namespace FeederCommands {
    constexpr uint8_t CMD_FEED = 0x01;
    constexpr uint8_t CMD_TEST = 0x02;
    constexpr uint8_t CMD_CANCEL = 0x03;
}

// Safety constants
namespace FeederSafety {
    constexpr uint8_t MAX_PORTIONS_PER_FEED = 5;
    constexpr uint8_t MIN_PORTIONS = 1;
    constexpr uint32_t MIN_FEED_INTERVAL_SEC = 3600;  // 1 hour
    constexpr uint32_t MAX_DAILY_FEEDINGS = 5;
}

#endif // FEEDER_DEVICE_H
