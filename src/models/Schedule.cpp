#include "models/Schedule.h"
#include <time.h>

/**
 * @brief Constructor
 */
Schedule::Schedule(uint32_t id, const String& name, Type type)
    : _id(id)
    , _name(name)
    , _type(type)
    , _enabled(true)
    , _daysMask(ALL_DAYS)
    , _intervalSeconds(0)
    , _lastExecution(0)
    , _nextExecution(0)
    , _executionCount(0)
    , _commandLength(0)
{
    memset(_commandData, 0, sizeof(_commandData));
    Serial.printf(" Created schedule: %s (ID: %d, Type: %d)\n", 
                 _name.c_str(), _id, (int)_type);
}

/**
 * @brief Destructor
 */
Schedule::~Schedule() {
    Serial.printf("  Destroying schedule: %s (ID: %d)\n", _name.c_str(), _id);
}

/**
 * @brief Set command data to execute
 */
void Schedule::setCommandData(const uint8_t* data, size_t length) {
    if (!data || length == 0) {
        Serial.println(" Invalid command data");
        return;
    }
    
    // Limit to max size
    _commandLength = (length > 32) ? 32 : length;
    memcpy(_commandData, data, _commandLength);
    
    Serial.printf(" Set command data (%d bytes) for schedule %s\n", 
                 _commandLength, _name.c_str());
}

/**
 * @brief Check if schedule is due for execution
 */
bool Schedule::isDue(uint32_t currentTime) const {
    if (!_enabled) {
        return false;
    }
    
    // Check if already executed recently (within 1 minute to prevent duplicates)
    if (_lastExecution > 0 && (currentTime - _lastExecution) < 60000) {
        return false;
    }
    
    switch (_type) {
        case Type::ONE_TIME:
            return currentTime >= _nextExecution && _executionCount == 0;
            
        case Type::DAILY:
            return _isTimeMatching(currentTime);
            
        case Type::WEEKLY:
            return _isDayMatching(currentTime) && _isTimeMatching(currentTime);
            
        case Type::INTERVAL:
            if (_lastExecution == 0) {
                return true;  // Execute immediately on first run
            }
            return (currentTime - _lastExecution) >= (_intervalSeconds * 1000);
            
        default:
            return false;
    }
}

/**
 * @brief Mark schedule as executed
 */
void Schedule::markExecuted(uint32_t currentTime) {
    _lastExecution = currentTime;
    _executionCount++;
    
    // Calculate next execution time
    _nextExecution = calculateNextExecution(currentTime);
    
    Serial.printf(" Executed schedule '%s' (count: %d)\n", 
                 _name.c_str(), _executionCount);
}

/**
 * @brief Calculate next execution time
 */
uint32_t Schedule::calculateNextExecution(uint32_t currentTime) const {
    switch (_type) {
        case Type::ONE_TIME:
            return 0;  // One-time schedules don't repeat
            
        case Type::INTERVAL:
            return currentTime + (_intervalSeconds * 1000);
            
        case Type::DAILY:
        case Type::WEEKLY:
            // For time-based schedules, next execution is calculated at check time
            // This is a simplified implementation
            return currentTime + (24 * 3600 * 1000);  // Next day
            
        default:
            return 0;
    }
}

/**
 * @brief Validate schedule configuration
 */
bool Schedule::validate() const {
    // Check if name is set
    if (_name.length() == 0) {
        Serial.println(" Schedule name is empty");
        return false;
    }
    
    // Check if command data is set
    if (_commandLength == 0) {
        Serial.println(" Schedule has no command data");
        return false;
    }
    
    // Type-specific validation
    switch (_type) {
        case Type::ONE_TIME:
            if (_nextExecution == 0) {
                Serial.println(" One-time schedule has no execution time");
                return false;
            }
            break;
            
        case Type::DAILY:
        case Type::WEEKLY:
            if (_times.empty()) {
                Serial.println(" Daily/Weekly schedule has no execution times");
                return false;
            }
            break;
            
        case Type::INTERVAL:
            if (_intervalSeconds == 0) {
                Serial.println(" Interval schedule has zero interval");
                return false;
            }
            break;
    }
    
    return true;
}

/**
 * @brief Convert schedule to JSON
 */
String Schedule::toJson() const {
    String json = "{";
    json += "\"id\":" + String(_id) + ",";
    json += "\"name\":\"" + _name + "\",";
    json += "\"type\":" + String((int)_type) + ",";
    json += "\"enabled\":" + String(_enabled ? "true" : "false") + ",";
    json += "\"daysMask\":" + String(_daysMask) + ",";
    json += "\"intervalSeconds\":" + String(_intervalSeconds) + ",";
    json += "\"lastExecution\":" + String(_lastExecution) + ",";
    json += "\"nextExecution\":" + String(_nextExecution) + ",";
    json += "\"executionCount\":" + String(_executionCount) + ",";
    json += "\"commandLength\":" + String(_commandLength);
    
    // Add times array
    if (!_times.empty()) {
        json += ",\"times\":[";
        for (size_t i = 0; i < _times.size(); i++) {
            if (i > 0) json += ",";
            json += "\"" + _times[i].toString() + "\"";
        }
        json += "]";
    }
    
    json += "}";
    
    return json;
}

/**
 * @brief Load schedule from JSON (stub)
 */
bool Schedule::fromJson(const String& json) {
    // TODO: Implement JSON parsing
    Serial.println("  Schedule::fromJson not yet implemented");
    return false;
}

/**
 * @brief Check if current day matches daysMask
 */
bool Schedule::_isDayMatching(uint32_t currentTime) const {
    // Convert millis to day of week (0=Sunday, 6=Saturday)
    // This is a simplified implementation
    // In production, use proper time library or RTC
    
    time_t now = currentTime / 1000;
    struct tm* timeinfo = localtime(&now);
    uint8_t dayOfWeek = timeinfo->tm_wday;
    
    uint8_t dayBit = (1 << dayOfWeek);
    return (_daysMask & dayBit) != 0;
}

/**
 * @brief Check if current time matches any TimeSpec
 */
bool Schedule::_isTimeMatching(uint32_t currentTime) const {
    if (_times.empty()) {
        return false;
    }
    
    // Convert millis to hour and minute
    time_t now = currentTime / 1000;
    struct tm* timeinfo = localtime(&now);
    uint8_t currentHour = timeinfo->tm_hour;
    uint8_t currentMinute = timeinfo->tm_min;
    
    // Check if current time matches any scheduled time
    for (const TimeSpec& time : _times) {
        if (time.hour == currentHour && time.minute == currentMinute) {
            return true;
        }
    }
    
    return false;
}
