#ifndef AQUARIUM_MANAGER_H
#define AQUARIUM_MANAGER_H

#include <Arduino.h>
#include <map>
#include <vector>
#include "models/Aquarium.h"
#include "models/Device.h"
#include "protocol/messages.h"

/**
 * @brief Central system manager for all aquariums and devices
 * 
 * Singleton class that manages:
 * - Multiple aquariums
 * - Device discovery and registration
 * - ESP-NOW communication
 * - Scheduling and automation
 * - Safety monitoring
 */
class AquariumManager {
public:
    /**
     * @brief Get singleton instance
     */
    static AquariumManager& getInstance();
    
    // Delete copy/move constructors
    AquariumManager(const AquariumManager&) = delete;
    AquariumManager& operator=(const AquariumManager&) = delete;
    
    /**
     * @brief Initialize manager
     * @return true if initialized successfully
     */
    bool initialize();
    
    /**
     * @brief Main update loop (call from loop())
     */
    void update();
    
    // ===== Aquarium Management =====
    /**
     * @brief Add new aquarium
     * @param aquarium Aquarium pointer (ownership transferred)
     * @return true if added successfully
     */
    bool addAquarium(Aquarium* aquarium);
    
    /**
     * @brief Remove aquarium
     * @param id Aquarium ID
     * @return true if removed successfully
     */
    bool removeAquarium(uint8_t id);
    
    /**
     * @brief Get aquarium by ID
     * @param id Aquarium ID
     * @return Pointer to aquarium or nullptr
     */
    Aquarium* getAquarium(uint8_t id);
    
    /**
     * @brief Get all aquariums
     * @return Vector of aquarium pointers
     */
    std::vector<Aquarium*> getAllAquariums() const;
    
    /**
     * @brief Get aquarium count
     */
    size_t getAquariumCount() const { return _aquariums.size(); }
    
    // ===== Device Discovery & Registration =====
    /**
     * @brief Handle device ANNOUNCE message
     * @param mac Device MAC address
     * @param msg ANNOUNCE message
     */
    void handleAnnounce(const uint8_t* mac, const AnnounceMessage& msg);
    
    /**
     * @brief Handle device HEARTBEAT message
     * @param mac Device MAC address
     * @param msg HEARTBEAT message
     */
    void handleHeartbeat(const uint8_t* mac, const HeartbeatMessage& msg);
    
    /**
     * @brief Handle device STATUS message
     * @param mac Device MAC address
     * @param msg STATUS message
     */
    void handleStatus(const uint8_t* mac, const StatusMessage& msg);
    
    /**
     * @brief Get device by MAC address
     * @param mac Device MAC address
     * @return Pointer to device or nullptr
     */
    Device* getDevice(const uint8_t* mac);
    
    /**
     * @brief Get all devices across all aquariums
     * @return Vector of device pointers
     */
    std::vector<Device*> getAllDevices() const;
    
    /**
     * @brief Get device count
     */
    size_t getDeviceCount() const;
    
    // ===== Scheduling =====
    /**
     * @brief Check and execute due schedules
     */
    void updateSchedules();
    
    /**
     * @brief Get all due schedules across system
     * @return Vector of schedule pointers
     */
    std::vector<Schedule*> getDueSchedules() const;
    
    // ===== Safety Monitoring =====
    /**
     * @brief Check device health (heartbeat timeouts)
     */
    void checkDeviceHealth();
    
    /**
     * @brief Check water parameters
     */
    void checkWaterParameters();
    
    /**
     * @brief Trigger emergency shutdown
     * @param reason Reason for shutdown
     */
    void emergencyShutdown(const String& reason);
    
    /**
     * @brief Get system health status
     * @return 0-100 overall health score
     */
    uint8_t getSystemHealth() const;
    
    // ===== Configuration =====
    /**
     * @brief Load configuration from file
     * @param filename Configuration file path
     * @return true if loaded successfully
     */
    bool loadConfiguration(const String& filename);
    
    /**
     * @brief Save configuration to file
     * @param filename Configuration file path
     * @return true if saved successfully
     */
    bool saveConfiguration(const String& filename);
    
    /**
     * @brief Export system state to JSON
     * @return JSON string
     */
    String toJson() const;
    
    /**
     * @brief Import system state from JSON
     * @param json JSON string
     * @return true if imported successfully
     */
    bool fromJson(const String& json);
    
    // ===== WebSocket Notifications =====
    /**
     * @brief Broadcast update to all WebSocket clients
     * @param event Event name
     * @param data Event data (JSON)
     */
    void broadcastUpdate(const String& event, const String& data);
    
    /**
     * @brief Set WebSocket broadcast callback
     * @param callback Function pointer
     */
    void setWebSocketCallback(void (*callback)(const String&, const String&));
    
    // ===== Statistics =====
    /**
     * @brief Get total uptime
     * @return Uptime in seconds
     */
    uint32_t getUptime() const;
    
    /**
     * @brief Get statistics
     */
    struct Statistics {
        uint32_t totalMessagesReceived;
        uint32_t totalMessagesSent;
        uint32_t totalCommands;
        uint32_t totalErrors;
        uint32_t uptimeSeconds;
        
        Statistics() : totalMessagesReceived(0), totalMessagesSent(0),
                      totalCommands(0), totalErrors(0), uptimeSeconds(0) {}
    };
    
    Statistics getStatistics() const { return _stats; }
    
private:
    /**
     * @brief Private constructor (singleton)
     */
    AquariumManager();
    
    /**
     * @brief Destructor
     */
    ~AquariumManager();
    
    // Aquarium registry (ID -> Aquarium)
    std::map<uint8_t, Aquarium*> _aquariums;
    
    // Device registry (MAC -> Device) for quick lookup
    std::map<uint64_t, Device*> _globalDeviceRegistry;
    
    // Timing
    uint32_t _startTime;
    uint32_t _lastScheduleCheck;
    uint32_t _lastHealthCheck;
    uint32_t _lastWaterCheck;
    
    // Statistics
    Statistics _stats;
    
    // WebSocket callback
    void (*_wsCallback)(const String&, const String&);
    
    // Helper methods
    uint64_t _macToKey(const uint8_t* mac) const;
    Device* _createDevice(const uint8_t* mac, NodeType type, const String& name);
    void _sendAck(const uint8_t* mac, uint8_t tankId, bool accepted);
    
    // Safety intervals
    static constexpr uint32_t HEARTBEAT_TIMEOUT_MS = 60000;     // 60 seconds
    static constexpr uint32_t HEALTH_CHECK_INTERVAL_MS = 5000;  // 5 seconds
    static constexpr uint32_t SCHEDULE_CHECK_INTERVAL_MS = 1000; // 1 second
    static constexpr uint32_t WATER_CHECK_INTERVAL_MS = 10000;  // 10 seconds
};

#endif // AQUARIUM_MANAGER_H
