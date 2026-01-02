# Class Structure Documentation

**Aquarium Management System - Object-Oriented Architecture**

---

## 📊 Overview

This document describes the complete class hierarchy for the Aquarium Management System, designed based on the UI mockups in `/design/hub/`.

## 🏗️ Architecture Diagram

```
┌─────────────────────────────────────────────────────────────┐
│                    AquariumManager                          │
│                      (Singleton)                            │
│  - Manages all aquariums and devices                        │
│  - Handles ESP-NOW communication                            │
│  - Safety monitoring and scheduling                         │
└─────────────────────────────────────────────────────────────┘
                          │
                          │ manages
                          ▼
┌─────────────────────────────────────────────────────────────┐
│                      Aquarium                               │
│  - Represents a single tank                                 │
│  - Tank ID, name, volume, water parameters                  │
│  - Contains multiple devices                                │
└─────────────────────────────────────────────────────────────┘
                          │
                          │ contains
                          ▼
┌─────────────────────────────────────────────────────────────┐
│                     Device (Base)                           │
│  - MAC address, type, status                                │
│  - Heartbeat monitoring                                     │
│  - Command/status handling                                  │
│  - Schedule management                                      │
└─────────────────────────────────────────────────────────────┘
                          │
                          │ inherits
        ┌─────────────────┼─────────────────┐
        │                 │                 │
        ▼                 ▼                 ▼
┌─────────────┐  ┌─────────────┐  ┌─────────────┐
│LightDevice  │  │ CO2Device   │  │HeaterDevice │
│- 3 channels │  │- Solenoid   │  │- Relay      │
│- PWM levels │  │- Safety     │  │- Temp ctrl  │
└─────────────┘  └─────────────┘  └─────────────┘
        │                 │                 │
        ▼                 ▼                 ▼
┌─────────────┐  ┌─────────────┐  ┌─────────────┐
│FeederDevice │  │SensorDevice │  │Repeater     │
│- Servo      │  │- pH/TDS/Temp│  │Device       │
│- Portions   │  │- Readings   │  │- Forwarding │
└─────────────┘  └─────────────┘  └─────────────┘
                          │
                          │ contains
                          ▼
┌─────────────────────────────────────────────────────────────┐
│                      Schedule                               │
│  - Type: Daily/Weekly/Interval/One-time                     │
│  - Execution times                                          │
│  - Command data                                             │
└─────────────────────────────────────────────────────────────┘
```

---

## 📁 File Structure

```
include/
├── models/
│   ├── Aquarium.h          # Aquarium class
│   ├── Device.h            # Device base class
│   ├── Schedule.h          # Schedule class
│   └── devices/
│       ├── LightDevice.h
│       ├── CO2Device.h
│       ├── HeaterDevice.h
│       ├── FeederDevice.h
│       ├── SensorDevice.h
│       └── RepeaterDevice.h
│
└── managers/
    └── AquariumManager.h   # System manager
```

---

## 🎯 Core Classes

### 1. AquariumManager (Singleton)

**Purpose**: Central system controller managing all aquariums, devices, and communication.

**Key Features**:
- Singleton pattern (only one instance)
- Device discovery via ESP-NOW
- Heartbeat monitoring
- Schedule execution
- Safety checks
- WebSocket notifications

**Usage Example**:
```cpp
auto& manager = AquariumManager::getInstance();
manager.initialize();

// Add aquarium
Aquarium* tank1 = new Aquarium(1, "Main Tank");
manager.addAquarium(tank1);

// In loop()
void loop() {
    manager.update();  // Handles everything
}
```

**Responsibilities**:
- ✅ ESP-NOW message routing
- ✅ Device health monitoring (60s heartbeat timeout)
- ✅ Schedule checking (every 1s)
- ✅ Water parameter monitoring (every 10s)
- ✅ Emergency shutdown coordination
- ✅ Configuration persistence

---

### 2. Aquarium

**Purpose**: Represents a single aquarium/tank with all its devices and settings.

**Key Features**:
- Unique tank ID (1-255)
- Device registry (by MAC address)
- Water parameters (target temp, pH ranges)
- Current sensor readings
- Health status

**Usage Example**:
```cpp
Aquarium tank(1, "Main Tank");
tank.setVolume(100.0);  // 100 liters
tank.setTargetTemperature(25.0);
tank.setTemperatureRange(24.0, 26.0);

// Add device
LightDevice* light = new LightDevice(mac, "Main Light");
tank.addDevice(light);

// Update readings
tank.updateTemperature(25.3);
tank.updatePh(7.2);

// Check safety
if (!tank.isTemperatureSafe()) {
    // Alert user
}
```

**Serialization**:
- Saves to JSON file
- Loads from JSON file
- Compatible with WebSocket updates

---

### 3. Device (Base Class)

**Purpose**: Abstract base class for all aquarium devices.

**Key Features**:
- MAC address identification
- NodeType classification
- Connection status tracking
- Heartbeat management
- Schedule container
- Command/status handling

**Common Properties**:
```cpp
class Device {
protected:
    uint8_t _mac[6];              // Unique identifier
    NodeType _type;               // LIGHT, CO2, HEATER, etc.
    String _name;                 // Display name
    uint8_t _tankId;              // Associated tank
    Status _status;               // ONLINE/OFFLINE/ERROR
    uint32_t _lastHeartbeat;      // Timestamp
    std::vector<Schedule*> _schedules;
};
```

**Virtual Methods**:
- `virtual void triggerFailSafe() = 0` - **Must implement** in derived classes
- `virtual String toJson() const` - JSON serialization
- `virtual bool fromJson(const String& json)` - JSON deserialization

**Usage Pattern**:
```cpp
Device* device = getDevice(mac);

// Check status
if (device->isOnline()) {
    // Send command
    device->sendCommand(data, length);
}

// Check heartbeat
if (device->hasHeartbeatTimedOut(60000)) {
    device->triggerFailSafe();
}
```

---

## 🔧 Device Implementations

### 4. LightDevice

**Controls**: 3-channel PWM LED lighting (White, Blue, Red)

**Key Methods**:
```cpp
// Set all channels
light->setLevels(255, 200, 100);  // white, blue, red

// Single channel
light->setChannel(Channel::BLUE, 255);

// On/off with fade
light->setOnOff(true, 2000);  // fade in over 2 seconds

// Presets
light->addPreset(Preset(1, "Morning", {255, 150, 0}));
light->applyPreset(1);
```

**Fail-Safe**: Holds last state (safe for lights)

**Command Types** (LightCommands namespace):
- `CMD_SET_LEVELS` (0x01)
- `CMD_SET_CHANNEL` (0x02)
- `CMD_ON_OFF` (0x03)
- `CMD_APPLY_PRESET` (0x04)
- `CMD_FADE_TO` (0x05)

---

### 5. CO2Device

**Controls**: Solenoid valve for CO₂ injection

**Key Methods**:
```cpp
// Start injection (timed)
co2->timedInjection(300);  // 5 minutes

// Manual control
co2->startInjection();  // Indefinite
co2->stopInjection();

// Emergency stop
co2->emergencyStop();

// Safety check
if (co2->isInjectionDurationExceeded()) {
    co2->emergencyStop();
}
```

**Fail-Safe**: **CRITICAL - Turn OFF** (prevent CO₂ overdose)

**Safety Constants** (CO2Safety namespace):
- `MAX_INJECTION_DURATION_SEC` = 3600 (1 hour max)
- `RECOMMENDED_DURATION_SEC` = 300 (5 minutes)
- `WARNING_THRESHOLD_SEC` = 600 (10 minutes)

---

### 6. HeaterDevice

**Controls**: Relay-controlled heating element with temperature sensor

**Key Methods**:
```cpp
// Auto mode
heater->enableAuto(25.0);  // Target 25°C

// Manual control
heater->manualOn();
heater->manualOff();

// Set hysteresis
heater->setHysteresis(0.5);  // ±0.5°C

// Safety check
if (heater->isOverheating()) {
    heater->manualOff();
}
```

**Fail-Safe**: **CRITICAL - Turn OFF** (prevent overheating)

**Safety Constants** (HeaterSafety namespace):
- `MAX_SAFE_TEMPERATURE` = 35.0°C
- `MIN_SAFE_TEMPERATURE` = 18.0°C
- `DEFAULT_HYSTERESIS` = 0.5°C

---

### 7. FeederDevice

**Controls**: Servo-based feeding mechanism

**Key Methods**:
```cpp
// Feed fish
if (feeder->canFeedNow()) {
    feeder->feed(3);  // 3 portions
}

// Validate portions
uint8_t safe = feeder->validatePortions(10);  // Returns 5 (max)

// Check timing
uint32_t wait = feeder->getTimeUntilNextFeed();
```

**Fail-Safe**: Do nothing (safer to skip feeding than overfeed)

**Safety Constants** (FeederSafety namespace):
- `MAX_PORTIONS_PER_FEED` = 5
- `MIN_FEED_INTERVAL_SEC` = 3600 (1 hour)
- `MAX_DAILY_FEEDINGS` = 5

---

### 8. SensorDevice

**Monitors**: pH, TDS, Temperature

**Key Methods**:
```cpp
// Get readings
auto readings = sensor->getCurrentReadings();
Serial.printf("Temp: %.1f, pH: %.2f, TDS: %d\n", 
              readings.temperature, readings.ph, readings.tds);

// Calibration
sensor->calibratePh(7.0);  // Known pH 7.0 buffer
sensor->calibrateTds(1413);  // Known 1413ppm solution

// History
auto history = sensor->getReadingHistory(50);  // Last 50 readings
auto avg = sensor->getAverageReadings(60);  // Last hour average
```

**Fail-Safe**: Continue reading (passive, no risk)

---

### 9. RepeaterDevice

**Function**: ESP-NOW range extender (passive relay)

**Key Methods**:
```cpp
// Enable/disable
repeater->setActive(true);

// Get statistics
auto stats = repeater->getStatistics();
Serial.printf("Forwarded: %u, Dropped: %u\n", 
              stats.messagesForwarded, stats.messagesDropped);

// Success rate
float rate = repeater->getForwardingSuccessRate();
```

**Fail-Safe**: Continue forwarding (passive relay)

---

## 📅 Schedule Class

**Purpose**: Manages timed operations for devices.

**Schedule Types**:
- `ONE_TIME` - Execute once at specific timestamp
- `DAILY` - Repeat every day at specific time(s)
- `WEEKLY` - Repeat on specific days of week
- `INTERVAL` - Repeat at fixed intervals

**Key Features**:
```cpp
// Create daily schedule
Schedule schedule(1, "Morning Feed", Schedule::Type::DAILY);
schedule.addTime(Schedule::TimeSpec(8, 30));  // 08:30
schedule.setCommandData(feedCmd, sizeof(feedCmd));

// Weekly schedule
Schedule lightSchedule(2, "Weekend Light", Schedule::Type::WEEKLY);
lightSchedule.setDaysMask(Schedule::WEEKEND);  // Saturday + Sunday
lightSchedule.addTime(Schedule::TimeSpec(10, 0));

// Interval schedule
Schedule sensorSchedule(3, "Sensor Reading", Schedule::Type::INTERVAL);
sensorSchedule.setInterval(300);  // Every 5 minutes

// Check if due
if (schedule.isDue(millis())) {
    // Execute command
    schedule.markExecuted(millis());
}
```

**DayOfWeek Bitmask**:
- `SUNDAY` = 0x01
- `MONDAY` = 0x02
- `TUESDAY` = 0x04
- `WEDNESDAY` = 0x08
- `THURSDAY` = 0x10
- `FRIDAY` = 0x20
- `SATURDAY` = 0x40
- `WEEKDAYS` = Mon-Fri
- `WEEKEND` = Sat-Sun
- `ALL_DAYS` = Every day

---

## 🔄 Data Flow

### Device Discovery Flow

```
1. Node boots → Sends ANNOUNCE (broadcast)
2. Hub receives → AquariumManager::handleAnnounce()
3. Manager creates device → new LightDevice(mac, name)
4. Manager assigns to aquarium → aquarium->addDevice(device)
5. Manager sends ACK → esp_now_send(mac, ack)
6. Node switches to unicast mode
7. Node sends HEARTBEAT (every 30s)
8. Manager updates → device->updateHeartbeat()
```

### Command Execution Flow

```
1. User clicks "Feed Fish" in UI
2. WebSocket message → handleWebSocketMessage()
3. Manager finds device → getDevice(mac)
4. Device executes → feeder->feed(portions)
5. Device builds command → _buildFeederCommand()
6. ESP-NOW send → esp_now_send()
7. Node processes → handleCommand()
8. Node sends STATUS → StatusMessage
9. Manager receives → handleStatus()
10. Device updates → device->handleStatus()
11. WebSocket broadcast → broadcastUpdate()
12. UI updates
```

### Schedule Execution Flow

```
1. Manager checks schedules → updateSchedules() (every 1s)
2. For each aquarium:
   - For each device:
     - Get due schedules → getDueSchedules()
     - Execute commands
     - Mark executed → schedule->markExecuted()
3. Device sends STATUS confirmation
4. Manager logs execution
```

---

## 💾 Serialization

All classes support JSON serialization for:
- Configuration persistence
- WebSocket communication
- Debugging

### Example JSON Structure

**Aquarium**:
```json
{
  "id": 1,
  "name": "Main Tank",
  "volume": 100.0,
  "enabled": true,
  "targetTemperature": 25.0,
  "minTemperature": 24.0,
  "maxTemperature": 26.0,
  "targetPh": 7.0,
  "minPh": 6.5,
  "maxPh": 7.5,
  "currentTemperature": 25.3,
  "currentPh": 7.1,
  "currentTds": 450,
  "devices": [
    {
      "mac": "AA:BB:CC:DD:EE:FF",
      "type": "LIGHT",
      "name": "Main Light",
      "enabled": true,
      "status": "ONLINE"
    }
  ]
}
```

**Schedule**:
```json
{
  "id": 1,
  "name": "Morning Feed",
  "type": "DAILY",
  "enabled": true,
  "times": ["08:30", "18:00"],
  "commandData": "0x01030000..."
}
```

---

## 🛡️ Safety Features

### Fail-Safe Modes

| Device Type | Fail-Safe Action | Reason |
|-------------|------------------|--------|
| LightDevice | Hold last state | Safe to maintain |
| CO2Device | **OFF** (critical) | Prevent CO₂ overdose |
| HeaterDevice | **OFF** (critical) | Prevent overheating |
| FeederDevice | Do nothing | Safer to skip |
| SensorDevice | Continue reading | Passive |
| RepeaterDevice | Continue forwarding | Passive |

### Heartbeat Monitoring

```cpp
// In AquariumManager::checkDeviceHealth()
for (auto device : getAllDevices()) {
    if (device->hasHeartbeatTimedOut(60000)) {  // 60 seconds
        device->triggerFailSafe();
        device->setStatus(Device::Status::OFFLINE);
        broadcastUpdate("deviceOffline", device->toJson());
    }
}
```

### Water Parameter Monitoring

```cpp
// In AquariumManager::checkWaterParameters()
for (auto aquarium : getAllAquariums()) {
    if (!aquarium->isTemperatureSafe()) {
        // Alert user
        broadcastUpdate("temperatureAlert", aquarium->toJson());
    }
    if (!aquarium->isPhSafe()) {
        // Alert user
        broadcastUpdate("phAlert", aquarium->toJson());
    }
}
```

---

## 🚀 Usage Examples

### Complete Setup Example

```cpp
#include "managers/AquariumManager.h"
#include "models/devices/LightDevice.h"
#include "models/devices/FeederDevice.h"
#include "models/Schedule.h"

void setup() {
    // Initialize manager
    auto& manager = AquariumManager::getInstance();
    manager.initialize();
    
    // Create aquarium
    Aquarium* mainTank = new Aquarium(1, "Main Tank");
    mainTank->setVolume(100.0);
    mainTank->setTargetTemperature(25.0);
    mainTank->setTemperatureRange(24.0, 26.0);
    mainTank->setTargetPh(7.0);
    mainTank->setPhRange(6.5, 7.5);
    
    manager.addAquarium(mainTank);
    
    // Devices will be auto-discovered via ESP-NOW ANNOUNCE
    
    // Load saved configuration
    manager.loadConfiguration("/config/system.json");
}

void loop() {
    // Manager handles everything
    AquariumManager::getInstance().update();
}

// ESP-NOW callback
void onESPNowRecv(const uint8_t* mac, const uint8_t* data, int len) {
    auto& manager = AquariumManager::getInstance();
    
    MessageHeader* header = (MessageHeader*)data;
    
    switch(header->type) {
        case MessageType::ANNOUNCE:
            manager.handleAnnounce(mac, *(AnnounceMessage*)data);
            break;
        case MessageType::HEARTBEAT:
            manager.handleHeartbeat(mac, *(HeartbeatMessage*)data);
            break;
        case MessageType::STATUS:
            manager.handleStatus(mac, *(StatusMessage*)data);
            break;
    }
}
```

### Creating Schedules

```cpp
// Get device
auto device = manager.getDevice(lightMac);

// Create morning schedule
Schedule* morning = new Schedule(1, "Morning Light", Schedule::Type::DAILY);
morning->addTime(Schedule::TimeSpec(7, 0));  // 07:00

uint8_t cmd[4] = {LightCommands::CMD_SET_LEVELS, 255, 200, 100};
morning->setCommandData(cmd, 4);

device->addSchedule(morning);

// Create weekly feeding
Schedule* weekendFeed = new Schedule(2, "Weekend Feed", Schedule::Type::WEEKLY);
weekendFeed->setDaysMask(Schedule::WEEKEND);
weekendFeed->addTime(Schedule::TimeSpec(10, 0));

uint8_t feedCmd[2] = {FeederCommands::CMD_FEED, 3};  // 3 portions
weekendFeed->setCommandData(feedCmd, 2);

feederDevice->addSchedule(weekendFeed);
```

---

## 📊 Class Relationships

### Ownership

- **AquariumManager** owns all **Aquarium** instances
- **Aquarium** owns all **Device** instances within it
- **Device** owns all **Schedule** instances attached to it

### Lifetime Management

- Use `new` for dynamic allocation
- Transfer ownership when adding to containers
- Destructors handle cleanup of owned objects

### Thread Safety

- **NOT thread-safe** (single-threaded ESP32 Arduino)
- Call all methods from main loop or FreeRTOS tasks with proper synchronization

---

## 🔧 Extension Points

### Adding New Device Type

1. Create header file: `include/models/devices/MyDevice.h`
2. Inherit from `Device` base class
3. Implement `triggerFailSafe()` (required)
4. Add device type to `NodeType` enum in `protocol/messages.h`
5. Update `AquariumManager::_createDevice()` factory method
6. Add command type constants namespace

### Adding New Schedule Type

1. Add enum value to `Schedule::Type`
2. Implement logic in `Schedule::isDue()`
3. Update `Schedule::toJson()` and `fromJson()`

---

## 📝 Notes

- All timestamps use `millis()` internally
- MAC addresses are 6-byte arrays
- JSON serialization uses ArduinoJson library
- ESP-NOW communication handled by AquariumManager
- WebSocket broadcasting via callback pattern

---

**Version**: 1.0  
**Created**: January 1, 2026  
**Based on**: UI mockups in `/design/hub/`  
**Project**: Aquarium Management System
