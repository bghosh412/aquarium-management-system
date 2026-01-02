#ifndef SENSOR_DEVICE_H
#define SENSOR_DEVICE_H

#include "models/Device.h"

/**
 * @brief Water Quality Sensor device
 * 
 * Monitors pH, TDS, and temperature.
 * Passive monitoring device (no actuators).
 */
class SensorDevice : public Device {
public:
    /**
     * @brief Sensor readings
     */
    struct Readings {
        float temperature;      // °C
        float ph;               // pH value
        uint16_t tds;           // ppm (parts per million)
        uint32_t timestamp;     // Reading timestamp
        
        Readings() : temperature(0), ph(0), tds(0), timestamp(0) {}
        
        bool isValid() const {
            return temperature > 0 && ph > 0 && tds > 0;
        }
    };
    
    /**
     * @brief Calibration data
     */
    struct Calibration {
        float phOffset;         // pH calibration offset
        float phSlope;          // pH calibration slope
        float tempOffset;       // Temperature offset
        float tdsMultiplier;    // TDS calibration multiplier
        
        Calibration() : phOffset(0), phSlope(1.0), tempOffset(0), tdsMultiplier(1.0) {}
    };
    
    /**
     * @brief Constructor
     * @param mac Device MAC address
     * @param name Device name
     */
    SensorDevice(const uint8_t* mac, const String& name);
    
    /**
     * @brief Destructor
     */
    ~SensorDevice() override;
    
    // ===== Getters =====
    Readings getCurrentReadings() const { return _currentReadings; }
    float getTemperature() const { return _currentReadings.temperature; }
    float getPh() const { return _currentReadings.ph; }
    uint16_t getTds() const { return _currentReadings.tds; }
    uint32_t getLastReadingTime() const { return _currentReadings.timestamp; }
    
    Calibration getCalibration() const { return _calibration; }
    uint32_t getReadingInterval() const { return _readingInterval; }
    uint32_t getTotalReadings() const { return _totalReadings; }
    
    // ===== Control Methods =====
    /**
     * @brief Request immediate reading
     * @return true if command sent successfully
     */
    bool requestReading();
    
    /**
     * @brief Set reading interval
     * @param seconds Interval in seconds
     * @return true if command sent successfully
     */
    bool setReadingInterval(uint32_t seconds);
    
    /**
     * @brief Set calibration data
     * @param calibration Calibration parameters
     * @return true if command sent successfully
     */
    bool setCalibration(const Calibration& calibration);
    
    /**
     * @brief Reset calibration to defaults
     * @return true if command sent successfully
     */
    bool resetCalibration();
    
    /**
     * @brief Calibrate pH sensor
     * @param knownPh Known pH value for calibration
     * @return true if command sent successfully
     */
    bool calibratePh(float knownPh);
    
    /**
     * @brief Calibrate TDS sensor
     * @param knownTds Known TDS value for calibration
     * @return true if command sent successfully
     */
    bool calibrateTds(uint16_t knownTds);
    
    // ===== State Management =====
    /**
     * @brief Update readings from device status
     * @param status StatusMessage from device
     */
    void handleStatus(const StatusMessage& status) override;
    
    /**
     * @brief Trigger fail-safe (continue reading - passive)
     */
    void triggerFailSafe() override;
    
    // ===== Data Analysis =====
    /**
     * @brief Get average readings over time window
     * @param minutes Time window in minutes
     * @return Average readings
     */
    Readings getAverageReadings(uint32_t minutes) const;
    
    /**
     * @brief Check if sensor is responding
     * @param timeoutMs Timeout in milliseconds
     * @return true if recent reading
     */
    bool isSensorResponding(uint32_t timeoutMs = 60000) const;
    
    /**
     * @brief Add reading to history
     * @param reading New reading
     */
    void addReadingToHistory(const Readings& reading);
    
    /**
     * @brief Get reading history
     * @param maxCount Max readings to return
     * @return Vector of readings (newest first)
     */
    std::vector<Readings> getReadingHistory(size_t maxCount = 100) const;
    
    /**
     * @brief Clear reading history
     */
    void clearHistory();
    
    // ===== Serialization =====
    String toJson() const override;
    bool fromJson(const String& json) override;
    
private:
    Readings _currentReadings;      // Latest readings
    Calibration _calibration;       // Calibration data
    uint32_t _readingInterval;      // Reading interval (seconds)
    uint32_t _totalReadings;        // Total readings taken
    
    // Reading history (ring buffer)
    std::vector<Readings> _history;
    size_t _maxHistorySize;
    
    /**
     * @brief Build sensor command
     */
    void _buildSensorCommand(uint8_t* buffer, uint8_t cmdType, const uint8_t* data, size_t len);
    
    /**
     * @brief Parse readings from status data
     */
    Readings _parseReadings(const uint8_t* data) const;
};

// Command types for sensor device
namespace SensorCommands {
    constexpr uint8_t CMD_REQUEST_READING = 0x01;
    constexpr uint8_t CMD_SET_INTERVAL = 0x02;
    constexpr uint8_t CMD_SET_CALIBRATION = 0x03;
    constexpr uint8_t CMD_RESET_CALIBRATION = 0x04;
    constexpr uint8_t CMD_CALIBRATE_PH = 0x05;
    constexpr uint8_t CMD_CALIBRATE_TDS = 0x06;
}

// Default settings
namespace SensorDefaults {
    constexpr uint32_t DEFAULT_INTERVAL_SEC = 30;
    constexpr size_t MAX_HISTORY_SIZE = 100;
    constexpr uint32_t SENSOR_TIMEOUT_MS = 60000;  // 1 minute
}

#endif // SENSOR_DEVICE_H
