#ifndef LIGHT_DEVICE_H
#define LIGHT_DEVICE_H

#include "Device.h"

/**
 * @brief Light device controller (3-channel PWM LED)
 * 
 * Controls RGB or White/Blue/Red LED lighting with PWM.
 * Supports intensity control, color mixing, and scheduled lighting cycles.
 */
class LightDevice : public Device {
public:
    /**
     * @brief Light channel configuration
     */
    enum class Channel {
        WHITE = 0,
        BLUE = 1,
        RED = 2
    };
    
    /**
     * @brief Light state
     */
    struct LightState {
        uint8_t white;      // 0-255 PWM value
        uint8_t blue;       // 0-255 PWM value
        uint8_t red;        // 0-255 PWM value
        bool isOn;          // Overall on/off state
        
        LightState() : white(0), blue(0), red(0), isOn(false) {}
        
        bool operator==(const LightState& other) const {
            return white == other.white && blue == other.blue && 
                   red == other.red && isOn == other.isOn;
        }
    };
    
    /**
     * @brief Constructor
     * @param mac Device MAC address
     * @param name Device name
     */
    LightDevice(const uint8_t* mac, const String& name);
    
    /**
     * @brief Destructor
     */
    ~LightDevice() override;
    
    // ===== Getters =====
    LightState getCurrentState() const { return _currentState; }
    LightState getTargetState() const { return _targetState; }
    uint8_t getWhiteLevel() const { return _currentState.white; }
    uint8_t getBlueLevel() const { return _currentState.blue; }
    uint8_t getRedLevel() const { return _currentState.red; }
    bool isLightOn() const { return _currentState.isOn; }
    uint16_t getTransitionTimeMs() const { return _transitionTimeMs; }
    bool isFading() const { return _isFading; }
    
    // ===== Control Methods =====
    /**
     * @brief Set all channels
     * @param white White channel (0-255)
     * @param blue Blue channel (0-255)
     * @param red Red channel (0-255)
     * @param transition Transition time in ms (0 = immediate)
     * @return true if command sent successfully
     */
    bool setLevels(uint8_t white, uint8_t blue, uint8_t red, uint16_t transition = 0);
    
    /**
     * @brief Set single channel
     * @param channel Channel to set
     * @param level Level (0-255)
     * @param transition Transition time in ms
     * @return true if command sent successfully
     */
    bool setChannel(Channel channel, uint8_t level, uint16_t transition = 0);
    
    /**
     * @brief Turn light on/off
     * @param on true = on, false = off
     * @param transition Fade time in ms
     * @return true if command sent successfully
     */
    bool setOnOff(bool on, uint16_t transition = 0);
    
    /**
     * @brief Set to predefined preset
     * @param presetId Preset ID
     * @return true if command sent successfully
     */
    bool applyPreset(uint8_t presetId);
    
    /**
     * @brief Fade to state over time
     * @param targetState Target light state
     * @param durationMs Fade duration
     * @return true if command sent successfully
     */
    bool fadeTo(const LightState& targetState, uint16_t durationMs);
    
    // ===== State Management =====
    /**
     * @brief Update current state from device status
     * @param status StatusMessage from device
     */
    void handleStatus(const StatusMessage& status) override;
    
    /**
     * @brief Trigger fail-safe (hold last state)
     */
    void triggerFailSafe() override;
    
    // ===== Presets =====
    /**
     * @brief Light preset
     */
    struct Preset {
        uint8_t id;
        String name;
        LightState state;
        
        Preset() : id(0) {}
        Preset(uint8_t _id, const String& _name, const LightState& _state) 
            : id(_id), name(_name), state(_state) {}
    };
    
    /**
     * @brief Add preset
     * @param preset Preset configuration
     */
    void addPreset(const Preset& preset);
    
    /**
     * @brief Get preset by ID
     * @param id Preset ID
     * @return Preset pointer or nullptr
     */
    const Preset* getPreset(uint8_t id) const;
    
    /**
     * @brief Get all presets
     * @return Vector of presets
     */
    std::vector<Preset> getAllPresets() const { return _presets; }
    
    /**
     * @brief Remove preset
     * @param id Preset ID
     */
    void removePreset(uint8_t id);
    
    // ===== Photo Period Schedule =====
    /**
     * @brief Photo period configuration
     */
    struct PhotoPeriod {
        uint8_t startHour;          // 0-11
        uint8_t startMinute;        // 0, 15, 30, 45
        bool startAM;               // true = AM, false = PM
        uint8_t durationHours;      // 0-12
        uint8_t durationMinutes;    // 0-59
        bool enableRamp;            // Gradual fade in/out
        
        PhotoPeriod() : startHour(0), startMinute(0), startAM(true), 
                       durationHours(0), durationMinutes(0), enableRamp(false) {}
    };
    
    /**
     * @brief Set morning photo period
     */
    void setMorningPhotoPeriod(const PhotoPeriod& period) { _morningPeriod = period; }
    
    /**
     * @brief Set evening photo period
     */
    void setEveningPhotoPeriod(const PhotoPeriod& period) { _eveningPeriod = period; }
    
    /**
     * @brief Get morning photo period
     */
    PhotoPeriod getMorningPhotoPeriod() const { return _morningPeriod; }
    
    /**
     * @brief Get evening photo period
     */
    PhotoPeriod getEveningPhotoPeriod() const { return _eveningPeriod; }
    
    // ===== Serialization =====
    String toJson() const override;
    bool fromJson(const String& json) override;
    
private:
    LightState _currentState;       // Current light state
    LightState _targetState;        // Target state (for fading)
    uint16_t _transitionTimeMs;     // Current transition duration
    bool _isFading;                 // Is currently fading?
    uint32_t _fadeStartTime;        // Fade start timestamp
    
    std::vector<Preset> _presets;   // Saved presets
    
    PhotoPeriod _morningPeriod;     // Morning photo period schedule
    PhotoPeriod _eveningPeriod;     // Evening photo period schedule
    
    /**
     * @brief Build command data for light control
     */
    void _buildLightCommand(uint8_t* buffer, const LightState& state, uint16_t transition);
};

// Command types for light device
namespace LightCommands {
    constexpr uint8_t CMD_ALL_OFF = 0;        // All 3 channels OFF
    constexpr uint8_t CMD_ALL_ON = 1;         // All 3 channels ON
    constexpr uint8_t CMD_CH1_OFF = 10;       // Channel 1 (White) OFF
    constexpr uint8_t CMD_CH1_ON = 11;        // Channel 1 (White) ON
    constexpr uint8_t CMD_CH2_OFF = 20;       // Channel 2 (Blue) OFF
    constexpr uint8_t CMD_CH2_ON = 21;        // Channel 2 (Blue) ON
    constexpr uint8_t CMD_CH3_OFF = 30;       // Channel 3 (Red) OFF
    constexpr uint8_t CMD_CH3_ON = 31;        // Channel 3 (Red) ON
}

// Status data structure (placeholder for future use)
// Byte 0: Channel 1 level (0-255)
// Byte 1: Channel 2 level (0-255)
// Byte 2: Channel 3 level (0-255)
// Byte 3: Enabled (0=off, 1=on)

#endif // LIGHT_DEVICE_H
