#ifndef HEATER_DEVICE_H
#define HEATER_DEVICE_H

#include "models/Device.h"

/**
 * @brief Heater device controller
 * 
 * Controls heating element with relay and monitors temperature.
 * CRITICAL SAFETY: Fails to OFF to prevent overheating.
 */
class HeaterDevice : public Device {
public:
    /**
     * @brief Heater operation mode
     */
    enum class Mode {
        OFF,            // Manually off
        ON,             // Manually on (override)
        AUTO,           // Automatic temperature control
        ERROR           // Error state
    };
    
    /**
     * @brief Constructor
     * @param mac Device MAC address
     * @param name Device name
     */
    HeaterDevice(const uint8_t* mac, const String& name);
    
    /**
     * @brief Destructor
     */
    ~HeaterDevice() override;
    
    // ===== Getters =====
    Mode getMode() const { return _mode; }
    bool isHeating() const { return _isHeating; }
    float getTargetTemperature() const { return _targetTemperature; }
    float getCurrentTemperature() const { return _currentTemperature; }
    float getHysteresis() const { return _hysteresis; }
    uint32_t getHeatingTime() const { return _heatingTime; }
    uint32_t getHeatingCycles() const { return _heatingCycles; }
    uint32_t getLastTemperatureUpdate() const { return _lastTemperatureUpdate; }
    
    // Safety limits
    float getMaxSafeTemperature() const { return _maxSafeTemperature; }
    void setMaxSafeTemperature(float temp) { _maxSafeTemperature = temp; }
    
    // ===== Control Methods =====
    /**
     * @brief Set heater mode
     * @param mode Operating mode
     * @return true if command sent successfully
     */
    bool setMode(Mode mode);
    
    /**
     * @brief Set target temperature (auto mode)
     * @param temperature Target temp in °C
     * @return true if command sent successfully
     */
    bool setTargetTemperature(float temperature);
    
    /**
     * @brief Set hysteresis for auto mode
     * @param hysteresis Temp difference in °C (e.g., 0.5)
     * @return true if command sent successfully
     */
    bool setHysteresis(float hysteresis);
    
    /**
     * @brief Manual on (override mode)
     * @return true if command sent successfully
     */
    bool manualOn();
    
    /**
     * @brief Manual off
     * @return true if command sent successfully
     */
    bool manualOff();
    
    /**
     * @brief Enable auto mode with target
     * @param targetTemp Target temperature
     * @return true if command sent successfully
     */
    bool enableAuto(float targetTemp);
    
    // ===== State Management =====
    /**
     * @brief Update state from device status
     * @param status StatusMessage from device
     */
    void handleStatus(const StatusMessage& status) override;
    
    /**
     * @brief Trigger fail-safe (CRITICAL: Turn OFF)
     */
    void triggerFailSafe() override;
    
    // ===== Safety Checks =====
    /**
     * @brief Check if temperature exceeds safe limit
     * @return true if over limit
     */
    bool isOverheating() const;
    
    /**
     * @brief Check if temperature sensor is responding
     * @param timeoutMs Timeout in milliseconds
     * @return true if sensor recent
     */
    bool isSensorResponding(uint32_t timeoutMs = 60000) const;
    
    // ===== Serialization =====
    String toJson() const override;
    bool fromJson(const String& json) override;
    
private:
    Mode _mode;                     // Operating mode
    bool _isHeating;                // Currently heating?
    float _targetTemperature;       // Target temp (°C)
    float _currentTemperature;      // Current reading (°C)
    float _hysteresis;              // Hysteresis (°C)
    float _maxSafeTemperature;      // Safety limit (default 35°C)
    
    // Statistics
    uint32_t _heatingTime;          // Total seconds heating
    uint32_t _heatingCycles;        // Total on/off cycles
    uint32_t _lastTemperatureUpdate; // Last sensor update
    
    /**
     * @brief Build heater command
     */
    void _buildHeaterCommand(uint8_t* buffer, uint8_t cmdType, float value);
};

// Command types for heater device
namespace HeaterCommands {
    constexpr uint8_t CMD_SET_MODE = 0x01;
    constexpr uint8_t CMD_SET_TARGET = 0x02;
    constexpr uint8_t CMD_SET_HYSTERESIS = 0x03;
    constexpr uint8_t CMD_MANUAL_ON = 0x04;
    constexpr uint8_t CMD_MANUAL_OFF = 0x05;
    constexpr uint8_t CMD_ENABLE_AUTO = 0x06;
}

// Safety constants
namespace HeaterSafety {
    constexpr float MAX_SAFE_TEMPERATURE = 35.0f;      // °C
    constexpr float MIN_SAFE_TEMPERATURE = 18.0f;      // °C
    constexpr float DEFAULT_HYSTERESIS = 0.5f;         // °C
    constexpr uint32_t SENSOR_TIMEOUT_MS = 60000;     // 1 minute
}

#endif // HEATER_DEVICE_H
