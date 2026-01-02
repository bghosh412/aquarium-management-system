#ifndef AQUARIUM_H
#define AQUARIUM_H

#include <Arduino.h>
#include <vector>
#include <map>
#include "Device.h"

/**
 * @brief Aquarium class representing a single aquarium/tank
 * 
 * Manages all devices, schedules, and settings for one aquarium.
 * Supports multi-tank setups where hub can control multiple aquariums.
 */
class Aquarium {
public:
    /**
     * @brief Constructor
     * @param id Unique tank ID (1-255)
     * @param name Human-readable aquarium name
     */
    Aquarium(uint8_t id, const String& name);
    
    /**
     * @brief Destructor - cleans up all devices
     */
    ~Aquarium();
    
    // ===== Getters =====
    uint8_t getId() const { return _id; }
    String getName() const { return _name; }
    float getVolume() const { return _volumeLiters; }
    String getTankType() const { return _tankType; }
    String getLocation() const { return _location; }
    String getDescription() const { return _description; }
    bool isEnabled() const { return _enabled; }
    
    // Water parameters
    float getTargetTemperature() const { return _targetTemperature; }
    float getMinTemperature() const { return _minTemperature; }
    float getMaxTemperature() const { return _maxTemperature; }
    float getTargetPh() const { return _targetPh; }
    float getMinPh() const { return _minPh; }
    float getMaxPh() const { return _maxPh; }
    uint16_t getMinTds() const { return _minTds; }
    uint16_t getMaxTds() const { return _maxTds; }
    
    // Current readings (from sensors)
    float getCurrentTemperature() const { return _currentTemperature; }
    float getCurrentPh() const { return _currentPh; }
    uint16_t getCurrentTds() const { return _currentTds; }
    uint32_t getLastSensorUpdate() const { return _lastSensorUpdate; }
    
    // ===== Setters =====
    void setName(const String& name) { _name = name; }
    void setVolume(float liters) { _volumeLiters = liters; }
    void setTankType(const String& type) { _tankType = type; }
    void setLocation(const String& loc) { _location = loc; }
    void setDescription(const String& desc) { _description = desc; }
    void setEnabled(bool enabled) { _enabled = enabled; }
    
    // Water parameters
    void setTargetTemperature(float temp) { _targetTemperature = temp; }
    void setTemperatureRange(float min, float max) { 
        _minTemperature = min; 
        _maxTemperature = max; 
    }
    void setTargetPh(float ph) { _targetPh = ph; }
    void setPhRange(float min, float max) { 
        _minPh = min; 
        _maxPh = max; 
    }
    void setTdsRange(uint16_t min, uint16_t max) {
        _minTds = min;
        _maxTds = max;
    }
    
    // Current readings update
    void updateTemperature(float temp);
    void updatePh(float ph);
    void updateTds(uint16_t tds);
    
    // ===== Device Management =====
    /**
     * @brief Add a device to this aquarium
     * @param device Pointer to device (ownership transferred)
     * @return true if added successfully
     */
    bool addDevice(Device* device);
    
    /**
     * @brief Remove device by MAC address
     * @param mac Device MAC address
     * @return true if removed successfully
     */
    bool removeDevice(const uint8_t* mac);
    
    /**
     * @brief Get device by MAC address
     * @param mac Device MAC address
     * @return Pointer to device or nullptr if not found
     */
    Device* getDevice(const uint8_t* mac);
    
    /**
     * @brief Get all devices
     * @return Vector of device pointers
     */
    std::vector<Device*> getAllDevices() const;
    
    /**
     * @brief Get devices by type
     * @param type NodeType to filter
     * @return Vector of matching devices
     */
    std::vector<Device*> getDevicesByType(NodeType type) const;
    
    /**
     * @brief Get device count
     * @return Number of registered devices
     */
    size_t getDeviceCount() const { return _devices.size(); }
    
    /**
     * @brief Check if device exists
     * @param mac Device MAC address
     * @return true if device is registered
     */
    bool hasDevice(const uint8_t* mac) const;
    
    // ===== Status Checks =====
    /**
     * @brief Check if temperature is within safe range
     * @return true if within range
     */
    bool isTemperatureSafe() const;
    
    /**
     * @brief Check if pH is within safe range
     * @return true if within range
     */
    bool isPhSafe() const;
    
    /**
     * @brief Check if all critical devices are online
     * @return true if all critical devices responding
     */
    bool areDevicesHealthy() const;
    
    /**
     * @brief Get overall aquarium health status
     * @return 0-100 health score
     */
    uint8_t getHealthScore() const;
    
    // ===== Serialization =====
    /**
     * @brief Convert aquarium to JSON for storage/WebSocket
     * @return JSON string representation
     */
    String toJson() const;
    
    /**
     * @brief Load aquarium from JSON
     * @param json JSON string
     * @return true if loaded successfully
     */
    bool fromJson(const String& json);
    
    /**
     * @brief Save aquarium configuration to file
     * @param filename File path
     * @return true if saved successfully
     */
    bool saveToFile(const String& filename) const;
    
    /**
     * @brief Load aquarium configuration from file
     * @param filename File path
     * @return true if loaded successfully
     */
    bool loadFromFile(const String& filename);
    
private:
    // Basic info
    uint8_t _id;                    // Unique tank ID (1-255)
    String _name;                   // Display name
    float _volumeLiters;            // Tank volume
    String _tankType;               // Tank type: Planted, Cichlids, Discuss, Shrimp, Mix
    String _location;               // Physical location (optional)
    String _description;            // Optional description
    bool _enabled;                  // Is this tank active?
    
    // Target water parameters
    float _targetTemperature;       // Target temp in °C
    float _minTemperature;          // Min safe temp
    float _maxTemperature;          // Max safe temp
    float _targetPh;                // Target pH
    float _minPh;                   // Min safe pH
    float _maxPh;                   // Max safe pH
    uint16_t _minTds;               // Min TDS (ppm)
    uint16_t _maxTds;               // Max TDS (ppm)
    
    // Current sensor readings
    float _currentTemperature;      // Last reading (°C)
    float _currentPh;               // Last reading
    uint16_t _currentTds;           // Last reading (ppm)
    uint32_t _lastSensorUpdate;     // millis() of last update
    
    // Device registry (MAC address -> Device pointer)
    std::map<uint64_t, Device*> _devices;
    
    /**
     * @brief Convert MAC address to uint64_t key
     */
    uint64_t _macToKey(const uint8_t* mac) const;
};

#endif // AQUARIUM_H
