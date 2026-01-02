#include "models/Aquarium.h"
#include <esp_now.h>

/**
 * @brief Constructor
 */
Aquarium::Aquarium(uint8_t id, const String& name)
    : _id(id)
    , _name(name)
    , _volumeLiters(0.0f)
    , _tankType("Mix")
    , _location("")
    , _description("")
    , _enabled(true)
    , _targetTemperature(25.0f)
    , _minTemperature(23.0f)
    , _maxTemperature(27.0f)
    , _targetPh(7.0f)
    , _minPh(6.5f)
    , _maxPh(7.5f)
    , _minTds(150)
    , _maxTds(300)
    , _currentTemperature(0.0f)
    , _currentPh(0.0f)
    , _currentTds(0)
    , _lastSensorUpdate(0)
{
    Serial.printf(" Created aquarium: %s (ID: %d)\n", _name.c_str(), _id);
}

/**
 * @brief Destructor - cleans up all devices
 */
Aquarium::~Aquarium() {
    Serial.printf("  Destroying aquarium: %s (ID: %d)\n", _name.c_str(), _id);
    
    // Delete all devices
    for (auto& pair : _devices) {
        delete pair.second;
    }
    _devices.clear();
}

/**
 * @brief Update current temperature reading
 */
void Aquarium::updateTemperature(float temp) {
    _currentTemperature = temp;
    _lastSensorUpdate = millis();
}

/**
 * @brief Update current pH reading
 */
void Aquarium::updatePh(float ph) {
    _currentPh = ph;
    _lastSensorUpdate = millis();
}

/**
 * @brief Update current TDS reading
 */
void Aquarium::updateTds(uint16_t tds) {
    _currentTds = tds;
    _lastSensorUpdate = millis();
}

/**
 * @brief Add a device to this aquarium
 */
bool Aquarium::addDevice(Device* device) {
    if (!device) {
        Serial.println(" Cannot add null device");
        return false;
    }
    
    uint64_t key = _macToKey(device->getMac());
    
    // Check if already exists
    if (_devices.find(key) != _devices.end()) {
        Serial.printf("  Device %s already exists in aquarium %s\n", 
                     device->getMacString().c_str(), _name.c_str());
        return false;
    }
    
    // Set device tank ID
    device->setTankId(_id);
    
    // Add to registry
    _devices[key] = device;
    
    Serial.printf(" Added device %s (%s) to aquarium %s\n", 
                 device->getName().c_str(),
                 device->getMacString().c_str(),
                 _name.c_str());
    
    return true;
}

/**
 * @brief Remove device by MAC address
 */
bool Aquarium::removeDevice(const uint8_t* mac) {
    uint64_t key = _macToKey(mac);
    
    auto it = _devices.find(key);
    if (it == _devices.end()) {
        Serial.println("  Device not found");
        return false;
    }
    
    Device* device = it->second;
    Serial.printf("  Removing device %s from aquarium %s\n",
                 device->getName().c_str(), _name.c_str());
    
    delete device;
    _devices.erase(it);
    
    return true;
}

/**
 * @brief Get device by MAC address
 */
Device* Aquarium::getDevice(const uint8_t* mac) {
    uint64_t key = _macToKey(mac);
    
    auto it = _devices.find(key);
    if (it != _devices.end()) {
        return it->second;
    }
    
    return nullptr;
}

/**
 * @brief Get all devices
 */
std::vector<Device*> Aquarium::getAllDevices() const {
    std::vector<Device*> devices;
    devices.reserve(_devices.size());
    
    for (const auto& pair : _devices) {
        devices.push_back(pair.second);
    }
    
    return devices;
}

/**
 * @brief Get devices by type
 */
std::vector<Device*> Aquarium::getDevicesByType(NodeType type) const {
    std::vector<Device*> devices;
    
    for (const auto& pair : _devices) {
        if (pair.second->getType() == type) {
            devices.push_back(pair.second);
        }
    }
    
    return devices;
}

/**
 * @brief Check if device exists
 */
bool Aquarium::hasDevice(const uint8_t* mac) const {
    uint64_t key = _macToKey(mac);
    return _devices.find(key) != _devices.end();
}

/**
 * @brief Check if temperature is within safe range
 */
bool Aquarium::isTemperatureSafe() const {
    // If no sensor data yet, assume safe
    if (_lastSensorUpdate == 0) {
        return true;
    }
    
    // Check if reading is recent (within 5 minutes)
    if (millis() - _lastSensorUpdate > 300000) {
        return true;  // Assume safe if stale data
    }
    
    return (_currentTemperature >= _minTemperature && 
            _currentTemperature <= _maxTemperature);
}

/**
 * @brief Check if pH is within safe range
 */
bool Aquarium::isPhSafe() const {
    // If no sensor data yet, assume safe
    if (_lastSensorUpdate == 0) {
        return true;
    }
    
    // Check if reading is recent (within 5 minutes)
    if (millis() - _lastSensorUpdate > 300000) {
        return true;  // Assume safe if stale data
    }
    
    return (_currentPh >= _minPh && _currentPh <= _maxPh);
}

/**
 * @brief Check if all critical devices are online
 */
bool Aquarium::areDevicesHealthy() const {
    for (const auto& pair : _devices) {
        Device* device = pair.second;
        
        // Skip disabled devices
        if (!device->isEnabled()) {
            continue;
        }
        
        // Critical devices must be online
        NodeType type = device->getType();
        if (type == NodeType::HEATER || type == NodeType::CO2) {
            if (!device->isOnline()) {
                return false;
            }
        }
    }
    
    return true;
}

/**
 * @brief Get overall aquarium health score
 */
uint8_t Aquarium::getHealthScore() const {
    uint8_t score = 100;
    
    // Deduct points for unsafe parameters
    if (!isTemperatureSafe()) score -= 30;
    if (!isPhSafe()) score -= 20;
    
    // Deduct points for offline devices
    size_t onlineCount = 0;
    for (const auto& pair : _devices) {
        if (pair.second->isOnline()) {
            onlineCount++;
        }
    }
    
    if (_devices.size() > 0) {
        float onlinePercent = (float)onlineCount / _devices.size();
        score -= (uint8_t)((1.0f - onlinePercent) * 30);
    }
    
    return score;
}

/**
 * @brief Convert aquarium to JSON
 */
String Aquarium::toJson() const {
    String json = "{";
    json += "\"id\":" + String(_id) + ",";
    json += "\"name\":\"" + _name + "\",";
    json += "\"volumeLiters\":" + String(_volumeLiters) + ",";
    json += "\"tankType\":\"" + _tankType + "\",";
    json += "\"location\":\"" + _location + "\",";
    json += "\"description\":\"" + _description + "\",";
    json += "\"enabled\":" + String(_enabled ? "true" : "false") + ",";
    
    // Water parameters
    json += "\"waterParameters\":{";
    json += "\"temperature\":{";
    json += "\"target\":" + String(_targetTemperature) + ",";
    json += "\"min\":" + String(_minTemperature) + ",";
    json += "\"max\":" + String(_maxTemperature);
    json += "},";
    json += "\"ph\":{";
    json += "\"target\":" + String(_targetPh) + ",";
    json += "\"min\":" + String(_minPh) + ",";
    json += "\"max\":" + String(_maxPh);
    json += "},";
    json += "\"tds\":{";
    json += "\"min\":" + String(_minTds) + ",";
    json += "\"max\":" + String(_maxTds);
    json += "}";
    json += "},";
    
    // Current readings
    json += "\"currentReadings\":{";
    json += "\"temperature\":" + String(_currentTemperature) + ",";
    json += "\"ph\":" + String(_currentPh) + ",";
    json += "\"tds\":" + String(_currentTds) + ",";
    json += "\"lastUpdate\":" + String(_lastSensorUpdate);
    json += "},";
    
    // Health status
    json += "\"health\":{";
    json += "\"score\":" + String(getHealthScore()) + ",";
    json += "\"temperatureSafe\":" + String(isTemperatureSafe() ? "true" : "false") + ",";
    json += "\"phSafe\":" + String(isPhSafe() ? "true" : "false") + ",";
    json += "\"devicesHealthy\":" + String(areDevicesHealthy() ? "true" : "false");
    json += "},";
    
    // Device count
    json += "\"deviceCount\":" + String(_devices.size());
    
    json += "}";
    
    return json;
}

/**
 * @brief Load aquarium from JSON (stub)
 */
bool Aquarium::fromJson(const String& json) {
    // TODO: Implement JSON parsing
    Serial.println("  fromJson not yet implemented");
    return false;
}

/**
 * @brief Save aquarium configuration to file (stub)
 */
bool Aquarium::saveToFile(const String& filename) const {
    // TODO: Implement file saving
    Serial.println("  saveToFile not yet implemented");
    return false;
}

/**
 * @brief Load aquarium configuration from file (stub)
 */
bool Aquarium::loadFromFile(const String& filename) {
    // TODO: Implement file loading
    Serial.println("  loadFromFile not yet implemented");
    return false;
}

/**
 * @brief Convert MAC address to uint64_t key
 */
uint64_t Aquarium::_macToKey(const uint8_t* mac) const {
    uint64_t key = 0;
    for (int i = 0; i < 6; i++) {
        key |= ((uint64_t)mac[i]) << (i * 8);
    }
    return key;
}
