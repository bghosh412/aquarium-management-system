#ifndef SCHEDULE_H
#define SCHEDULE_H

#include <Arduino.h>
#include <vector>

/**
 * @brief Schedule class for timed device operations
 * 
 * Supports one-time and recurring schedules with flexible time specifications.
 * Can handle daily, weekly, or interval-based scheduling.
 */
class Schedule {
public:
    /**
     * @brief Schedule type
     */
    enum class Type {
        ONE_TIME,       // Execute once at specific time
        DAILY,          // Repeat every day at specific time(s)
        WEEKLY,         // Repeat on specific days of week
        INTERVAL        // Repeat at fixed intervals
    };
    
    /**
     * @brief Days of week bitmask
     */
    enum DayOfWeek : uint8_t {
        SUNDAY    = 0x01,
        MONDAY    = 0x02,
        TUESDAY   = 0x04,
        WEDNESDAY = 0x08,
        THURSDAY  = 0x10,
        FRIDAY    = 0x20,
        SATURDAY  = 0x40,
        WEEKDAYS  = MONDAY | TUESDAY | WEDNESDAY | THURSDAY | FRIDAY,
        WEEKEND   = SATURDAY | SUNDAY,
        ALL_DAYS  = 0x7F
    };
    
    /**
     * @brief Time specification (hours, minutes)
     */
    struct TimeSpec {
        uint8_t hour;       // 0-23
        uint8_t minute;     // 0-59
        
        TimeSpec() : hour(0), minute(0) {}
        TimeSpec(uint8_t h, uint8_t m) : hour(h), minute(m) {}
        
        bool operator==(const TimeSpec& other) const {
            return hour == other.hour && minute == other.minute;
        }
        
        String toString() const {
            char buffer[6];
            snprintf(buffer, sizeof(buffer), "%02d:%02d", hour, minute);
            return String(buffer);
        }
    };
    
    /**
     * @brief Constructor
     * @param id Unique schedule ID
     * @param name Human-readable name
     * @param type Schedule type
     */
    Schedule(uint32_t id, const String& name, Type type);
    
    /**
     * @brief Destructor
     */
    ~Schedule();
    
    // ===== Getters =====
    uint32_t getId() const { return _id; }
    String getName() const { return _name; }
    Type getType() const { return _type; }
    bool isEnabled() const { return _enabled; }
    uint32_t getLastExecution() const { return _lastExecution; }
    uint32_t getNextExecution() const { return _nextExecution; }
    uint32_t getExecutionCount() const { return _executionCount; }
    
    // Time settings
    std::vector<TimeSpec> getTimes() const { return _times; }
    uint8_t getDaysMask() const { return _daysMask; }
    uint32_t getIntervalSeconds() const { return _intervalSeconds; }
    
    // Command data
    const uint8_t* getCommandData() const { return _commandData; }
    size_t getCommandLength() const { return _commandLength; }
    
    // ===== Setters =====
    void setName(const String& name) { _name = name; }
    void setEnabled(bool enabled) { _enabled = enabled; }
    
    /**
     * @brief Set execution time(s) for daily schedule
     * @param times Vector of TimeSpec
     */
    void setTimes(const std::vector<TimeSpec>& times) { _times = times; }
    
    /**
     * @brief Add execution time
     * @param time TimeSpec to add
     */
    void addTime(const TimeSpec& time) { _times.push_back(time); }
    
    /**
     * @brief Set days of week for weekly schedule
     * @param daysMask Bitmask of DayOfWeek values
     */
    void setDaysMask(uint8_t daysMask) { _daysMask = daysMask; }
    
    /**
     * @brief Set interval for interval-based schedule
     * @param seconds Interval in seconds
     */
    void setInterval(uint32_t seconds) { _intervalSeconds = seconds; }
    
    /**
     * @brief Set one-time execution timestamp
     * @param timestamp Unix timestamp or millis()
     */
    void setOneTimeExecution(uint32_t timestamp) { _nextExecution = timestamp; }
    
    /**
     * @brief Set command data to execute
     * @param data Byte array
     * @param length Length of data
     */
    void setCommandData(const uint8_t* data, size_t length);
    
    // ===== Execution Logic =====
    /**
     * @brief Check if schedule is due for execution
     * @param currentTime Current time (Unix timestamp or millis)
     * @return true if should execute now
     */
    bool isDue(uint32_t currentTime) const;
    
    /**
     * @brief Mark schedule as executed
     * @param currentTime Execution timestamp
     */
    void markExecuted(uint32_t currentTime);
    
    /**
     * @brief Calculate next execution time
     * @param currentTime Current time
     * @return Next execution timestamp
     */
    uint32_t calculateNextExecution(uint32_t currentTime) const;
    
    /**
     * @brief Reset execution count
     */
    void resetExecutionCount() { _executionCount = 0; }
    
    // ===== Validation =====
    /**
     * @brief Validate schedule configuration
     * @return true if valid
     */
    bool validate() const;
    
    // ===== Serialization =====
    /**
     * @brief Convert schedule to JSON
     * @return JSON string
     */
    String toJson() const;
    
    /**
     * @brief Load schedule from JSON
     * @param json JSON string
     * @return true if loaded successfully
     */
    bool fromJson(const String& json);
    
private:
    uint32_t _id;                   // Unique ID
    String _name;                   // Display name
    Type _type;                     // Schedule type
    bool _enabled;                  // Is schedule active?
    
    // Time specifications
    std::vector<TimeSpec> _times;   // Execution times (for daily/weekly)
    uint8_t _daysMask;              // Days of week (for weekly)
    uint32_t _intervalSeconds;      // Interval (for interval type)
    
    // Execution tracking
    uint32_t _lastExecution;        // Last execution timestamp
    uint32_t _nextExecution;        // Next execution timestamp
    uint32_t _executionCount;       // Total executions
    
    // Command data
    uint8_t _commandData[32];       // Command to execute
    size_t _commandLength;          // Length of command data
    
    /**
     * @brief Check if current day matches daysMask
     * @param currentTime Current timestamp
     * @return true if day matches
     */
    bool _isDayMatching(uint32_t currentTime) const;
    
    /**
     * @brief Check if current time matches any TimeSpec
     * @param currentTime Current timestamp
     * @return true if time matches
     */
    bool _isTimeMatching(uint32_t currentTime) const;
};

#endif // SCHEDULE_H
