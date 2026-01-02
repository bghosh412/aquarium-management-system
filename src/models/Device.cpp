#include "models/Device.h"
#include "ESPNowManager.h"

/**
 * @brief Constructor
 */
Device::Device(const uint8_t* mac, NodeType type, const String& name)
    : _type(type)
    , _name(name)
    , _tankId(0)
    , _firmwareVersion(0)
    , _enabled(true)
    , _status(Status::UNKNOWN)
    , _lastHeartbeat(0)
    , _lastCommandSent(0)
    , _lastStatusReceived(0)
    , _uptimeMinutes(0)
    , _health(100)
    , _messagesReceived(0)
    , _messagesSent(0)
    , _commandsSent(0)
    , _errorCount(0)
{
    memcpy(_mac, mac, 6);
    Serial.printf(" Created device: %s (%s)\n", _name.c_str(), getMacString().c_str());
}

/**
 * @brief Virtual destructor
 */
Device::~Device() {
    Serial.printf("  Destroying device: %s\n", _name.c_str());
    
    // Delete all schedules
    for (Schedule* schedule : _schedules) {
        delete schedule;
    }
    _schedules.clear();
}

/**
 * @brief Get MAC address as string
 */
String Device::getMacString() const {
    char buffer[18];
    snprintf(buffer, sizeof(buffer), "%02X:%02X:%02X:%02X:%02X:%02X",
             _mac[0], _mac[1], _mac[2], _mac[3], _mac[4], _mac[5]);
    return String(buffer);
}

/**
 * @brief Get device type name
 */
String Device::getTypeName() const {
    switch (_type) {
        case NodeType::HUB: return "Hub";
        case NodeType::LIGHT: return "Light";
        case NodeType::CO2: return "CO2 Regulator";
        case NodeType::DOSER: return "Doser";
        case NodeType::SENSOR: return "Water Quality Sensor";
        case NodeType::HEATER: return "Heater";
        case NodeType::FILTER: return "Filter";
        case NodeType::FISH_FEEDER: return "Fish Feeder";
        case NodeType::REPEATER: return "Repeater";
        default: return "Unknown";
    }
}

/**
 * @brief Get status as string
 */
String Device::getStatusString() const {
    switch (_status) {
        case Status::UNKNOWN: return "Unknown";
        case Status::ONLINE: return "Online";
        case Status::OFFLINE: return "Offline";
        case Status::ERROR: return "Error";
        case Status::INITIALIZING: return "Initializing";
        default: return "Unknown";
    }
}

/**
 * @brief Update heartbeat timestamp
 */
void Device::updateHeartbeat(uint8_t health, uint16_t uptime) {
    _lastHeartbeat = millis();
    _health = health;
    _uptimeMinutes = uptime;
    _messagesReceived++;
    
    // Update status to online
    if (_status != Status::ONLINE) {
        _status = Status::ONLINE;
        Serial.printf(" Device %s is now ONLINE\n", _name.c_str());
    }
}

/**
 * @brief Check if heartbeat has timed out
 */
bool Device::hasHeartbeatTimedOut(uint32_t timeoutMs) const {
    // Never timed out if we haven't received any heartbeat yet
    if (_lastHeartbeat == 0) {
        return false;
    }
    
    return (millis() - _lastHeartbeat) > timeoutMs;
}

/**
 * @brief Send command to device
 */
bool Device::sendCommand(const uint8_t* commandData, size_t length) {
    if (!commandData || length == 0) {
        Serial.println(" Invalid command data");
        return false;
    }
    
    // Create command message
    CommandMessage cmd;
    cmd.header.type = MessageType::COMMAND;
    cmd.header.tankId = _tankId;
    cmd.header.nodeType = NodeType::HUB;
    cmd.header.timestamp = millis();
    cmd.header.sequenceNum = 0;  // ESPNowManager handles sequence tracking
    
    cmd.commandId = random(1, 255);  // TODO: Implement proper command ID tracking
    cmd.commandSeqID = 0;
    cmd.finalCommand = true;
    
    // Copy command data (max 32 bytes)
    size_t copyLen = (length > 32) ? 32 : length;
    memcpy(cmd.commandData, commandData, copyLen);
    
    // Check if peer is online before sending
    if (!ESPNowManager::getInstance().isPeerOnline(_mac)) {
        Serial.printf("  Device %s is OFFLINE, command not sent\n", _name.c_str());
        _errorCount++;
        return false;
    }
    
    // Send via ESPNowManager
    bool success = ESPNowManager::getInstance().send(_mac, (uint8_t*)&cmd, sizeof(cmd), true);
    
    if (success) {
        _lastCommandSent = millis();
        _commandsSent++;
        _messagesSent++;
        Serial.printf(" Sent command to %s (online check passed)\n", _name.c_str());
    } else {
        _errorCount++;
        Serial.printf(" Failed to send command to %s\n", _name.c_str());
    }
    
    return success;
}

/**
 * @brief Handle status message from device
 */
void Device::handleStatus(const StatusMessage& status) {
    _lastStatusReceived = millis();
    _messagesReceived++;
    
    Serial.printf(" Received status from %s: code=%d\n", 
                 _name.c_str(), status.statusCode);
}

/**
 * @brief Add schedule to device
 */
bool Device::addSchedule(Schedule* schedule) {
    if (!schedule) {
        Serial.println(" Cannot add null schedule");
        return false;
    }
    
    // Check if schedule already exists
    for (Schedule* existing : _schedules) {
        if (existing->getId() == schedule->getId()) {
            Serial.printf("  Schedule %d already exists\n", schedule->getId());
            return false;
        }
    }
    
    _schedules.push_back(schedule);
    Serial.printf(" Added schedule '%s' to device %s\n", 
                 schedule->getName().c_str(), _name.c_str());
    
    return true;
}

/**
 * @brief Remove schedule by ID
 */
bool Device::removeSchedule(uint32_t scheduleId) {
    for (auto it = _schedules.begin(); it != _schedules.end(); ++it) {
        if ((*it)->getId() == scheduleId) {
            delete *it;
            _schedules.erase(it);
            Serial.printf("  Removed schedule %d\n", scheduleId);
            return true;
        }
    }
    
    Serial.printf("  Schedule %d not found\n", scheduleId);
    return false;
}

/**
 * @brief Get schedule by ID
 */
Schedule* Device::getSchedule(uint32_t scheduleId) {
    for (Schedule* schedule : _schedules) {
        if (schedule->getId() == scheduleId) {
            return schedule;
        }
    }
    
    return nullptr;
}

/**
 * @brief Get all schedules
 */
std::vector<Schedule*> Device::getAllSchedules() const {
    return _schedules;
}

/**
 * @brief Check if any schedules are due
 */
std::vector<Schedule*> Device::getDueSchedules(uint32_t currentTime) const {
    std::vector<Schedule*> dueSchedules;
    
    for (Schedule* schedule : _schedules) {
        if (schedule->isEnabled() && schedule->isDue(currentTime)) {
            dueSchedules.push_back(schedule);
        }
    }
    
    return dueSchedules;
}

/**
 * @brief Enable/disable all schedules
 */
void Device::enableSchedules(bool enable) {
    for (Schedule* schedule : _schedules) {
        schedule->setEnabled(enable);
    }
    
    Serial.printf("%s all schedules for device %s\n", 
                 enable ? "Enabled" : "Disabled", _name.c_str());
}

/**
 * @brief Convert device to JSON
 */
String Device::toJson() const {
    String json = "{";
    json += "\"mac\":\"" + getMacString() + "\",";
    json += "\"type\":\"" + getTypeName() + "\",";
    json += "\"name\":\"" + _name + "\",";
    json += "\"tankId\":" + String(_tankId) + ",";
    json += "\"firmwareVersion\":" + String(_firmwareVersion) + ",";
    json += "\"enabled\":" + String(_enabled ? "true" : "false") + ",";
    json += "\"status\":\"" + getStatusString() + "\",";
    json += "\"health\":" + String(_health) + ",";
    json += "\"uptimeMinutes\":" + String(_uptimeMinutes) + ",";
    json += "\"lastHeartbeat\":" + String(_lastHeartbeat) + ",";
    json += "\"messagesReceived\":" + String(_messagesReceived) + ",";
    json += "\"messagesSent\":" + String(_messagesSent) + ",";
    json += "\"commandsSent\":" + String(_commandsSent) + ",";
    json += "\"errorCount\":" + String(_errorCount) + ",";
    json += "\"scheduleCount\":" + String(_schedules.size());
    json += "}";
    
    return json;
}

/**
 * @brief Load device from JSON (stub)
 */
bool Device::fromJson(const String& json) {
    // TODO: Implement JSON parsing
    Serial.println("  Device::fromJson not yet implemented");
    return false;
}
