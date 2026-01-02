#ifndef DEVICE_H
#define DEVICE_H

#include <Arduino.h>
#include <vector>
#include "protocol/messages.h"
#include "Schedule.h"

/**
 * @brief Base class for all aquarium devices
 * 
 * Represents a physical ESP8266/ESP32 node with ESP-NOW communication.
 * Each device has a unique MAC address, type, and associated schedules.
 */
class Device {
public:
    /**
     * @brief Device connection status
     */
    enum class Status {
        UNKNOWN,        // Not yet discovered
        ONLINE,         // Responding to heartbeats
        OFFLINE,        // Missed heartbeat timeout
        ERROR,          // Reported error state
        INITIALIZING    // Announced but not fully registered
    };
    
    /**
     * @brief Constructor
     * @param mac 6-byte MAC address
     * @param type NodeType enum
     * @param name Human-readable device name
     */
    Device(const uint8_t* mac, NodeType type, const String& name);
    
    /**
     * @brief Virtual destructor for polymorphism
     */
    virtual ~Device();
    
    // ===== Getters =====
    const uint8_t* getMac() const { return _mac; }
    String getMacString() const;
    NodeType getType() const { return _type; }
    String getTypeName() const;
    String getName() const { return _name; }
    uint8_t getTankId() const { return _tankId; }
    uint8_t getFirmwareVersion() const { return _firmwareVersion; }
    Status getStatus() const { return _status; }
    String getStatusString() const;
    bool isOnline() const { return _status == Status::ONLINE; }
    bool isEnabled() const { return _enabled; }
    
    // Timing info
    uint32_t getLastHeartbeat() const { return _lastHeartbeat; }
    uint32_t getLastCommandSent() const { return _lastCommandSent; }
    uint32_t getLastStatusReceived() const { return _lastStatusReceived; }
    uint16_t getUptimeMinutes() const { return _uptimeMinutes; }
    uint8_t getHealth() const { return _health; }
    
    // Statistics
    uint32_t getMessagesReceived() const { return _messagesReceived; }
    uint32_t getMessagesSent() const { return _messagesSent; }
    uint32_t getCommandsSent() const { return _commandsSent; }
    uint32_t getErrorCount() const { return _errorCount; }
    
    // ===== Setters =====
    void setName(const String& name) { _name = name; }
    void setTankId(uint8_t tankId) { _tankId = tankId; }
    void setFirmwareVersion(uint8_t version) { _firmwareVersion = version; }
    void setEnabled(bool enabled) { _enabled = enabled; }
    void setStatus(Status status) { _status = status; }
    
    // ===== Heartbeat Management =====
    /**
     * @brief Update heartbeat timestamp
     * @param health Health indicator (0-100)
     * @param uptime Uptime in minutes
     */
    void updateHeartbeat(uint8_t health, uint16_t uptime);
    
    /**
     * @brief Check if heartbeat has timed out
     * @param timeoutMs Timeout threshold in milliseconds
     * @return true if timeout exceeded
     */
    bool hasHeartbeatTimedOut(uint32_t timeoutMs) const;
    
    // ===== Command Management =====
    /**
     * @brief Send command to device
     * @param commandData Byte array of command data
     * @param length Length of command data
     * @return true if sent successfully
     */
    virtual bool sendCommand(const uint8_t* commandData, size_t length);
    
    /**
     * @brief Handle status message from device
     * @param status StatusMessage from device
     */
    virtual void handleStatus(const StatusMessage& status);
    
    /**
     * @brief Trigger fail-safe mode for device
     * Sends appropriate safe command based on device type
     */
    virtual void triggerFailSafe() = 0;  // Pure virtual - must implement
    
    // ===== Schedule Management =====
    /**
     * @brief Add schedule to device
     * @param schedule Schedule pointer (ownership transferred)
     * @return true if added successfully
     */
    bool addSchedule(Schedule* schedule);
    
    /**
     * @brief Remove schedule by ID
     * @param scheduleId Schedule ID to remove
     * @return true if removed successfully
     */
    bool removeSchedule(uint32_t scheduleId);
    
    /**
     * @brief Get schedule by ID
     * @param scheduleId Schedule ID
     * @return Pointer to schedule or nullptr
     */
    Schedule* getSchedule(uint32_t scheduleId);
    
    /**
     * @brief Get all schedules
     * @return Vector of schedule pointers
     */
    std::vector<Schedule*> getAllSchedules() const;
    
    /**
     * @brief Check if any schedules are due
     * @param currentTime Current time (millis or epoch)
     * @return Vector of due schedules
     */
    std::vector<Schedule*> getDueSchedules(uint32_t currentTime) const;
    
    /**
     * @brief Enable/disable all schedules
     */
    void enableSchedules(bool enable);
    
    // ===== Serialization =====
    /**
     * @brief Convert device to JSON
     * @return JSON string representation
     */
    virtual String toJson() const;
    
    /**
     * @brief Load device from JSON
     * @param json JSON string
     * @return true if loaded successfully
     */
    virtual bool fromJson(const String& json);
    
protected:
    // Device identification
    uint8_t _mac[6];                // MAC address
    NodeType _type;                 // Device type
    String _name;                   // Display name
    uint8_t _tankId;                // Associated tank ID
    uint8_t _firmwareVersion;       // Firmware version
    bool _enabled;                  // Is device enabled?
    
    // Connection status
    Status _status;                 // Current status
    uint32_t _lastHeartbeat;        // Last heartbeat millis()
    uint32_t _lastCommandSent;      // Last command sent millis()
    uint32_t _lastStatusReceived;   // Last status received millis()
    uint16_t _uptimeMinutes;        // Device uptime
    uint8_t _health;                // Health indicator (0-100)
    
    // Statistics
    uint32_t _messagesReceived;     // Total messages from device
    uint32_t _messagesSent;         // Total messages to device
    uint32_t _commandsSent;         // Total commands sent
    uint32_t _errorCount;           // Total errors
    
    // Schedules
    std::vector<Schedule*> _schedules;
};

#endif // DEVICE_H
