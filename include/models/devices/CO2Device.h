#ifndef CO2_DEVICE_H
#define CO2_DEVICE_H

#include "models/Device.h"

/**
 * @brief CO₂ Regulator device controller
 * 
 * Controls solenoid valve for CO₂ injection.
 * CRITICAL SAFETY: Always fails to OFF state to prevent CO₂ overdose.
 */
class CO2Device : public Device {
public:
    /**
     * @brief CO₂ injection state
     */
    enum class InjectionState {
        OFF,            // Valve closed
        ON,             // Valve open (injecting)
        TIMED,          // Timed injection in progress
        ERROR           // Error state
    };
    
    /**
     * @brief Constructor
     * @param mac Device MAC address
     * @param name Device name
     */
    CO2Device(const uint8_t* mac, const String& name);
    
    /**
     * @brief Destructor
     */
    ~CO2Device() override;
    
    // ===== Getters =====
    InjectionState getState() const { return _state; }
    bool isInjecting() const { return _state == InjectionState::ON || _state == InjectionState::TIMED; }
    uint32_t getInjectionStartTime() const { return _injectionStartTime; }
    uint32_t getInjectionDuration() const { return _injectionDuration; }
    uint32_t getTotalInjectionTime() const { return _totalInjectionTime; }
    uint32_t getInjectionCount() const { return _injectionCount; }
    uint32_t getMaxInjectionDuration() const { return _maxInjectionDuration; }
    
    // Safety limits
    void setMaxInjectionDuration(uint32_t seconds) { _maxInjectionDuration = seconds; }
    
    // ===== Control Methods =====
    /**
     * @brief Start CO₂ injection
     * @param durationSeconds Duration in seconds (0 = indefinite until stop)
     * @return true if command sent successfully
     */
    bool startInjection(uint32_t durationSeconds = 0);
    
    /**
     * @brief Stop CO₂ injection
     * @return true if command sent successfully
     */
    bool stopInjection();
    
    /**
     * @brief Timed injection (safer)
     * @param durationSeconds Duration in seconds
     * @return true if command sent successfully
     */
    bool timedInjection(uint32_t durationSeconds);
    
    /**
     * @brief Emergency stop
     * @return true if command sent successfully
     */
    bool emergencyStop();
    
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
     * @brief Check if injection duration exceeds safety limit
     * @return true if over limit
     */
    bool isInjectionDurationExceeded() const;
    
    /**
     * @brief Get remaining injection time
     * @return Seconds remaining (0 if indefinite or not injecting)
     */
    uint32_t getRemainingTime() const;
    
    // ===== Serialization =====
    String toJson() const override;
    bool fromJson(const String& json) override;
    
private:
    InjectionState _state;          // Current state
    uint32_t _injectionStartTime;   // Start timestamp (millis)
    uint32_t _injectionDuration;    // Duration in seconds (0 = indefinite)
    uint32_t _totalInjectionTime;   // Total seconds injected (lifetime)
    uint32_t _injectionCount;       // Total injection cycles
    uint32_t _maxInjectionDuration; // Safety limit (default 3600s = 1 hour)
    
    /**
     * @brief Build CO₂ command
     */
    void _buildCO2Command(uint8_t* buffer, uint8_t cmdType, uint32_t duration);
};

// Command types for CO₂ device
namespace CO2Commands {
    constexpr uint8_t CMD_START = 0x01;
    constexpr uint8_t CMD_STOP = 0x02;
    constexpr uint8_t CMD_TIMED = 0x03;
    constexpr uint8_t CMD_EMERGENCY_STOP = 0xFF;
}

// Safety constants
namespace CO2Safety {
    constexpr uint32_t MAX_INJECTION_DURATION_SEC = 3600;  // 1 hour max
    constexpr uint32_t RECOMMENDED_DURATION_SEC = 300;     // 5 minutes recommended
    constexpr uint32_t WARNING_THRESHOLD_SEC = 600;        // Warn after 10 minutes
}

#endif // CO2_DEVICE_H
