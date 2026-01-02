#include "managers/AquariumManager.h"
#include <esp_now.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

// ============================================================================
// SINGLETON INSTANCE
// ============================================================================

AquariumManager& AquariumManager::getInstance() {
    static AquariumManager instance;
    return instance;
}

// ============================================================================
// CONSTRUCTOR & DESTRUCTOR
// ============================================================================

AquariumManager::AquariumManager() 
    : _startTime(0),
      _lastScheduleCheck(0),
      _lastHealthCheck(0),
      _lastWaterCheck(0),
      _wsCallback(nullptr) {
    // Initialize statistics
    _stats = Statistics();
}

AquariumManager::~AquariumManager() {
    // Clean up all aquariums (which clean up their devices)
    for (auto& pair : _aquariums) {
        delete pair.second;
    }
    _aquariums.clear();
    _globalDeviceRegistry.clear();
}

// ============================================================================
// INITIALIZATION
// ============================================================================

bool AquariumManager::initialize() {
    _startTime = millis();
    _lastScheduleCheck = millis();
    _lastHealthCheck = millis();
    _lastWaterCheck = millis();
    
    Serial.println("🎮 AquariumManager initialized");
    return true;
}

// ============================================================================
// MAIN UPDATE LOOP
// ============================================================================

void AquariumManager::update() {
    uint32_t now = millis();
    
    // Check schedules every second
    if (now - _lastScheduleCheck >= SCHEDULE_CHECK_INTERVAL_MS) {
        _lastScheduleCheck = now;
        updateSchedules();
    }
    
    // Check device health every 5 seconds
    if (now - _lastHealthCheck >= HEALTH_CHECK_INTERVAL_MS) {
        _lastHealthCheck = now;
        checkDeviceHealth();
    }
    
    // Check water parameters every 10 seconds
    if (now - _lastWaterCheck >= WATER_CHECK_INTERVAL_MS) {
        _lastWaterCheck = now;
        checkWaterParameters();
    }
}

// ============================================================================
// AQUARIUM MANAGEMENT
// ============================================================================

bool AquariumManager::addAquarium(Aquarium* aquarium) {
    if (!aquarium) {
        Serial.println("❌ Cannot add null aquarium");
        return false;
    }
    
    uint8_t id = aquarium->getId();
    
    // Check for duplicate ID
    if (_aquariums.find(id) != _aquariums.end()) {
        Serial.printf("❌ Aquarium with ID %d already exists\n", id);
        return false;
    }
    
    _aquariums[id] = aquarium;
    Serial.printf("✅ Added aquarium: %s (ID: %d)\n", 
                  aquarium->getName().c_str(), id);
    return true;
}

bool AquariumManager::removeAquarium(uint8_t id) {
    auto it = _aquariums.find(id);
    if (it == _aquariums.end()) {
        Serial.printf("❌ Aquarium ID %d not found\n", id);
        return false;
    }
    
    // Remove all devices from global registry
    Aquarium* aquarium = it->second;
    std::vector<Device*> devices = aquarium->getAllDevices();
    for (Device* device : devices) {
        uint64_t key = _macToKey(device->getMac());
        _globalDeviceRegistry.erase(key);
    }
    
    // Delete aquarium (and its devices)
    delete aquarium;
    _aquariums.erase(it);
    
    Serial.printf("✅ Removed aquarium ID: %d\n", id);
    return true;
}

Aquarium* AquariumManager::getAquarium(uint8_t id) {
    auto it = _aquariums.find(id);
    if (it != _aquariums.end()) {
        return it->second;
    }
    return nullptr;
}

std::vector<Aquarium*> AquariumManager::getAllAquariums() const {
    std::vector<Aquarium*> result;
    result.reserve(_aquariums.size());
    
    for (const auto& pair : _aquariums) {
        result.push_back(pair.second);
    }
    
    return result;
}

// ============================================================================
// DEVICE DISCOVERY & REGISTRATION
// ============================================================================

void AquariumManager::handleAnnounce(const uint8_t* mac, const AnnounceMessage& msg) {
    uint64_t macKey = _macToKey(mac);
    
    Serial.printf("📢 ANNOUNCE from %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    Serial.printf("   - Type: %d, Tank: %d, FW: v%d\n",
                  (int)msg.header.nodeType, msg.header.tankId, msg.firmwareVersion);
    
    // Check if device already registered
    if (_globalDeviceRegistry.find(macKey) != _globalDeviceRegistry.end()) {
        Serial.println("   - Device already registered, sending ACK");
        _sendAck(mac, msg.header.tankId, true);
        _stats.totalMessagesReceived++;
        return;
    }
    
    // Check if device is unmapped (tankId == 0)
    if (msg.header.tankId == 0) {
        Serial.println("   - ⚠️  Unmapped device (tankId=0), storing for provisioning");
        
        // Load unmapped devices JSON
        File file = LittleFS.open("/config/unmapped-devices.json", "r");
        DynamicJsonDocument doc(4096);
        
        if (file) {
            deserializeJson(doc, file);
            file.close();
        } else {
            // Create new structure
            doc["metadata"]["lastCleanup"] = 0;
            doc["metadata"]["totalDiscovered"] = 0;
            doc["metadata"]["autoCleanupAfterDays"] = 7;
        }
        
        // Check if already in unmapped list
        JsonArray unmappedDevices = doc["unmappedDevices"];
        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        
        bool alreadyExists = false;
        for (JsonObject device : unmappedDevices) {
            if (device["mac"].as<String>() == String(macStr)) {
                // Update existing entry
                device["lastSeen"] = millis();
                device["announceCount"] = device["announceCount"].as<int>() + 1;
                alreadyExists = true;
                Serial.println("   - Updated existing unmapped device entry");
                break;
            }
        }
        
        if (!alreadyExists) {
            // Add new unmapped device
            JsonObject newDevice = unmappedDevices.createNestedObject();
            newDevice["mac"] = macStr;
            
            // Convert NodeType enum to string
            const char* typeStr = "UNKNOWN";
            switch(msg.header.nodeType) {
                case NodeType::LIGHT: typeStr = "LIGHT"; break;
                case NodeType::CO2: typeStr = "CO2"; break;
                case NodeType::HEATER: typeStr = "HEATER"; break;
                case NodeType::FISH_FEEDER: typeStr = "FISH_FEEDER"; break;
                case NodeType::SENSOR: typeStr = "SENSOR"; break;
                case NodeType::REPEATER: typeStr = "REPEATER"; break;
                default: typeStr = "UNKNOWN"; break;
            }
            newDevice["type"] = typeStr;
            newDevice["firmwareVersion"] = msg.firmwareVersion;
            newDevice["capabilities"] = msg.capabilities;
            newDevice["discoveredAt"] = millis();
            newDevice["lastSeen"] = millis();
            newDevice["announceCount"] = 1;
            newDevice["status"] = "DISCOVERED";
            
            doc["metadata"]["totalDiscovered"] = doc["metadata"]["totalDiscovered"].as<int>() + 1;
            
            Serial.printf("   - ✅ Added to unmapped devices: %s (%s)\n", macStr, typeStr);
        }
        
        // Save back to file
        file = LittleFS.open("/config/unmapped-devices.json", "w");
        if (file) {
            serializeJson(doc, file);
            file.close();
        }
        
        _sendAck(mac, 0, true);  // Still send ACK
        _stats.totalMessagesReceived++;
        return;
    }
    
    // Check if aquarium exists
    Aquarium* aquarium = getAquarium(msg.header.tankId);
    if (!aquarium) {
        Serial.printf("   - ⚠️  Aquarium ID %d not found, rejecting device\n", msg.header.tankId);
        _sendAck(mac, msg.header.tankId, false);
        _stats.totalMessagesReceived++;
        _stats.totalErrors++;
        return;
    }
    
    // Create device (name will be from devices.json or default)
    const char* deviceName = "UnknownDevice";  // TODO: Load from devices.json by MAC
    Device* device = _createDevice(mac, msg.header.nodeType, deviceName);
    if (!device) {
        Serial.println("   - ❌ Failed to create device");
        _sendAck(mac, msg.header.tankId, false);
        _stats.totalMessagesReceived++;
        _stats.totalErrors++;
        return;
    }
    
    // Set tank ID and firmware version
    device->setFirmwareVersion(msg.firmwareVersion);
    
    // Add to aquarium
    if (!aquarium->addDevice(device)) {
        Serial.println("   - ❌ Failed to add device to aquarium");
        delete device;
        _sendAck(mac, msg.header.tankId, false);
        _stats.totalMessagesReceived++;
        _stats.totalErrors++;
        return;
    }
    
    // Add to global registry
    _globalDeviceRegistry[macKey] = device;
    
    // Send ACK
    _sendAck(mac, msg.header.tankId, true);
    
    // Broadcast update
    if (_wsCallback) {
        _wsCallback("deviceDiscovered", device->toJson());
    }
    
    Serial.printf("   - ✅ Device registered successfully\n");
    _stats.totalMessagesReceived++;
}

void AquariumManager::handleHeartbeat(const uint8_t* mac, const HeartbeatMessage& msg) {
    uint64_t macKey = _macToKey(mac);
    
    auto it = _globalDeviceRegistry.find(macKey);
    if (it == _globalDeviceRegistry.end()) {
        // Unknown device, ignore
        return;
    }
    
    Device* device = it->second;
    device->updateHeartbeat(msg.health, msg.uptimeMinutes);
    
    // Update status to online if it was offline
    if (device->getStatus() != Device::Status::ONLINE) {
        device->setStatus(Device::Status::ONLINE);
        
        if (_wsCallback) {
            _wsCallback("deviceOnline", device->toJson());
        }
    }
    
    _stats.totalMessagesReceived++;
}

void AquariumManager::handleStatus(const uint8_t* mac, const StatusMessage& msg) {
    uint64_t macKey = _macToKey(mac);
    
    auto it = _globalDeviceRegistry.find(macKey);
    if (it == _globalDeviceRegistry.end()) {
        // Unknown device, ignore
        return;
    }
    
    Device* device = it->second;
    device->handleStatus(msg);
    
    // Broadcast update
    if (_wsCallback) {
        _wsCallback("deviceStatus", device->toJson());
    }
    
    _stats.totalMessagesReceived++;
}

Device* AquariumManager::getDevice(const uint8_t* mac) {
    uint64_t macKey = _macToKey(mac);
    
    auto it = _globalDeviceRegistry.find(macKey);
    if (it != _globalDeviceRegistry.end()) {
        return it->second;
    }
    
    return nullptr;
}

std::vector<Device*> AquariumManager::getAllDevices() const {
    std::vector<Device*> result;
    result.reserve(_globalDeviceRegistry.size());
    
    for (const auto& pair : _globalDeviceRegistry) {
        result.push_back(pair.second);
    }
    
    return result;
}

size_t AquariumManager::getDeviceCount() const {
    return _globalDeviceRegistry.size();
}

// ============================================================================
// SCHEDULING
// ============================================================================

void AquariumManager::updateSchedules() {
    uint32_t now = millis();
    
    // Check all devices for due schedules
    for (auto& pair : _globalDeviceRegistry) {
        Device* device = pair.second;
        
        if (!device->isEnabled() || device->getStatus() != Device::Status::ONLINE) {
            continue;
        }
        
        std::vector<Schedule*> dueSchedules = device->getDueSchedules(now);
        
        for (Schedule* schedule : dueSchedules) {
            // Execute schedule
            const uint8_t* cmdData = schedule->getCommandData();
            size_t cmdLen = schedule->getCommandLength();
            
            if (cmdData && cmdLen > 0) {
                device->sendCommand(cmdData, cmdLen);
                schedule->markExecuted(now);
                
                Serial.printf("📅 Executed schedule: %s for device %s\n",
                             schedule->getName().c_str(),
                             device->getName().c_str());
                
                _stats.totalCommands++;
            }
        }
    }
}

std::vector<Schedule*> AquariumManager::getDueSchedules() const {
    std::vector<Schedule*> result;
    uint32_t now = millis();
    
    for (const auto& pair : _globalDeviceRegistry) {
        Device* device = pair.second;
        
        if (!device->isEnabled()) {
            continue;
        }
        
        std::vector<Schedule*> dueSchedules = device->getDueSchedules(now);
        result.insert(result.end(), dueSchedules.begin(), dueSchedules.end());
    }
    
    return result;
}

// ============================================================================
// SAFETY MONITORING
// ============================================================================

void AquariumManager::checkDeviceHealth() {
    uint32_t now = millis();
    
    for (auto& pair : _globalDeviceRegistry) {
        Device* device = pair.second;
        
        if (device->getStatus() == Device::Status::ONLINE) {
            if (device->hasHeartbeatTimedOut(HEARTBEAT_TIMEOUT_MS)) {
                Serial.printf("⚠️  Device %s heartbeat timeout!\n", 
                             device->getName().c_str());
                
                // Trigger fail-safe
                device->triggerFailSafe();
                device->setStatus(Device::Status::OFFLINE);
                
                // Broadcast alert
                if (_wsCallback) {
                    _wsCallback("deviceOffline", device->toJson());
                }
                
                _stats.totalErrors++;
            }
        }
    }
}

void AquariumManager::checkWaterParameters() {
    for (auto& pair : _aquariums) {
        Aquarium* aquarium = pair.second;
        
        // Check temperature
        if (!aquarium->isTemperatureSafe()) {
            Serial.printf("⚠️  Aquarium %s temperature unsafe: %.1f°C\n",
                         aquarium->getName().c_str(),
                         aquarium->getCurrentTemperature());
            
            if (_wsCallback) {
                _wsCallback("temperatureAlert", aquarium->toJson());
            }
        }
        
        // Check pH
        if (!aquarium->isPhSafe()) {
            Serial.printf("⚠️  Aquarium %s pH unsafe: %.2f\n",
                         aquarium->getName().c_str(),
                         aquarium->getCurrentPh());
            
            if (_wsCallback) {
                _wsCallback("phAlert", aquarium->toJson());
            }
        }
    }
}

void AquariumManager::emergencyShutdown(const String& reason) {
    Serial.println("🚨 EMERGENCY SHUTDOWN: " + reason);
    
    // Trigger fail-safe on all devices
    for (auto& pair : _globalDeviceRegistry) {
        Device* device = pair.second;
        device->triggerFailSafe();
        device->setStatus(Device::Status::ERROR);
    }
    
    // Broadcast emergency
    if (_wsCallback) {
        _wsCallback("emergencyShutdown", "{\"reason\":\"" + reason + "\"}");
    }
    
    _stats.totalErrors++;
}

uint8_t AquariumManager::getSystemHealth() const {
    if (_globalDeviceRegistry.empty()) {
        return 100;  // No devices, system is "healthy"
    }
    
    uint32_t totalHealth = 0;
    uint32_t deviceCount = 0;
    
    for (const auto& pair : _globalDeviceRegistry) {
        Device* device = pair.second;
        
        if (device->getStatus() == Device::Status::ONLINE) {
            totalHealth += device->getHealth();
            deviceCount++;
        }
    }
    
    if (deviceCount == 0) {
        return 0;  // All devices offline
    }
    
    return totalHealth / deviceCount;
}

// ============================================================================
// CONFIGURATION
// ============================================================================

bool AquariumManager::loadConfiguration(const String& filename) {
    // TODO: Implement JSON configuration loading
    Serial.println("⚠️  Configuration loading not yet implemented");
    return false;
}

bool AquariumManager::saveConfiguration(const String& filename) {
    // TODO: Implement JSON configuration saving
    Serial.println("⚠️  Configuration saving not yet implemented");
    return false;
}

String AquariumManager::toJson() const {
    // TODO: Implement full system JSON export
    return "{}";
}

bool AquariumManager::fromJson(const String& json) {
    // TODO: Implement full system JSON import
    return false;
}

// ============================================================================
// WEBSOCKET NOTIFICATIONS
// ============================================================================

void AquariumManager::broadcastUpdate(const String& event, const String& data) {
    if (_wsCallback) {
        _wsCallback(event, data);
    }
}

void AquariumManager::setWebSocketCallback(void (*callback)(const String&, const String&)) {
    _wsCallback = callback;
}

// ============================================================================
// STATISTICS
// ============================================================================

uint32_t AquariumManager::getUptime() const {
    return (millis() - _startTime) / 1000;
}

// ============================================================================
// PRIVATE HELPER METHODS
// ============================================================================

uint64_t AquariumManager::_macToKey(const uint8_t* mac) const {
    uint64_t key = 0;
    for (int i = 0; i < 6; i++) {
        key |= ((uint64_t)mac[i]) << (i * 8);
    }
    return key;
}

Device* AquariumManager::_createDevice(const uint8_t* mac, NodeType type, const String& name) {
    // Base Device class is abstract, so we need concrete implementations
    // For now, return nullptr - concrete device classes need to be instantiated
    // based on NodeType
    
    switch (type) {
        case NodeType::LIGHT:
            // return new LightDevice(mac, name);
            break;
        case NodeType::CO2:
            // return new CO2Device(mac, name);
            break;
        case NodeType::HEATER:
            // return new HeaterDevice(mac, name);
            break;
        case NodeType::FISH_FEEDER:
            // return new FeederDevice(mac, name);
            break;
        case NodeType::SENSOR:
            // return new SensorDevice(mac, name);
            break;
        case NodeType::REPEATER:
            // return new RepeaterDevice(mac, name);
            break;
        default:
            Serial.printf("❌ Unknown device type: %d\n", (int)type);
            return nullptr;
    }
    
    // Temporary: Create a basic device instance
    // This will be replaced when concrete device classes are implemented
    Serial.printf("⚠️  Device type %d not yet implemented, skipping\n", (int)type);
    return nullptr;
}

void AquariumManager::_sendAck(const uint8_t* mac, uint8_t tankId, bool accepted) {
    AckMessage ack;
    ack.header.type = MessageType::ACK;
    ack.header.tankId = tankId;
    ack.header.nodeType = NodeType::HUB;
    ack.header.timestamp = millis();
    ack.header.sequenceNum = 0;
    ack.assignedNodeId = 0;  // Not used for now
    ack.accepted = accepted;
    
    esp_err_t result = esp_now_send(mac, (uint8_t*)&ack, sizeof(ack));
    
    if (result == ESP_OK) {
        Serial.println("   - ACK sent successfully");
        _stats.totalMessagesSent++;
    } else {
        Serial.printf("   - ❌ ACK send failed: %d\n", result);
        _stats.totalErrors++;
    }
}
