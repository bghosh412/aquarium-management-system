# Aquarium Management System

A **modular, distributed aquarium management system** built on ESP32/ESP8266 microcontrollers using ESP-NOW for reliable, low-latency communication.

## 🏗️ Architecture

### Central Hub
- **MCU**: ESP32-WROOM
- **Role**: System orchestrator
- **Responsibilities**:
  - Node discovery and registration
  - Scheduling and automation
  - Web UI and logging
  - Safety monitoring
  - Multi-tank coordination

### Peripheral Nodes
- **MCU**: ESP8266 (all nodes)
- **Role**: Subsystem controllers
- **Types**:
  - **Fish Feeder** - Automated feeding system
  - **CO₂ Regulator** - CO₂ injection control
  - **Lighting** - Multi-channel LED control
  - **Heater** - Temperature control with sensor
  - **Water Quality** - pH, TDS, temperature sensors

**Key Principle**: Each node controls exactly ONE subsystem.

## 📡 Communication

- **Protocol**: ESP-NOW (peer-to-peer, no router required)
- **Discovery**: Dynamic broadcast/unicast
- **Messages**: Binary C/C++ structs (not JSON)
- **Channel**: Fixed channel (default: 1)

### Message Flow
1. Node boots → broadcasts ANNOUNCE
2. Hub receives → registers node → sends unicast ACK
3. Node receives ACK → switches to unicast mode
4. Periodic HEARTBEAT exchange
5. COMMAND (hub→node) / STATUS (node→hub) as needed
6. **NEW:** Nodes auto-acknowledge commands with STATUS messages
7. **NEW:** Multi-part commands supported via commandSeqID

See `PROTOCOL_UPDATES.md` for details on command/status enhancements.

## 🔒 Safety

**Fail-Safe Philosophy**: On communication loss or error:
- ✅ Lights → hold last safe state or soft OFF
- ❌ CO₂ → OFF
- ❌ Dosing → OFF
- ❌ Heaters → OFF

Safety always overrides functionality.

## 📁 Project Structure

```
aquarium-controller/
├── include/
│   └── protocol/          # Shared message definitions (headers)
│       └── messages.h     # ESP-NOW message structs & enums
│
├── lib/
│   └── NodeBase/          # 📚 Shared node library (PlatformIO convention)
│       ├── node_base.h    # Interface declarations
│       ├── node_base.cpp  # ESP-NOW communication logic
│       └── library.json   # Library metadata
│
├── src/
│   ├── hub/              # Hub firmware
│   │   └── main.cpp      # ESP32 hub implementation
│   └── nodes/            # All node firmware
│       ├── fish_feeder/
│       │   └── main.cpp  # Hardware-specific logic only
│       ├── co2_regulator/
│       │   └── main.cpp
│       ├── lighting/
│       │   └── main.cpp
│       ├── heater/
│       │   └── main.cpp
│       └── water_quality/
│           └── main.cpp
│
├── platformio.ini        # Multi-environment build configuration
└── Documents/            # All documentation
```

**Note**: `NodeBase` library is automatically linked to all node builds by PlatformIO.

## 🚀 Getting Started

### Prerequisites
- [PlatformIO](https://platformio.org/) installed
- ESP32-WROOM for hub
- ESP8266 or ESP32-C3 for nodes

### Building Hub Firmware

```bash
# In VS Code: Use PlatformIO sidebar, select "hub_esp32" environment
# Or via PlatformIO CLI:
pio run -e hub_esp32
pio run -e hub_esp32 -t upload
pio device monitor -e hub_esp32
```

### Building Node Firmware
### Building Node Firmware

```bash
# Fish Feeder
pio run -e node_fish_feeder
pio run -e node_fish_feeder -t upload

# CO2 Regulator
pio run -e node_co2_regulator
pio run -e node_co2_regulator -t upload

# Lighting
pio run -e node_lighting
pio run -e node_lighting -t upload

# Heater
pio run -e node_heater
pio run -e node_heater -t upload

# Water Quality Sensors
pio run -e node_water_quality
pio run -e node_water_quality -t upload

# Monitor any node
pio device monitor -e node_lighting
```

### Configuring a Node

Edit the specific node's `main.cpp` before building (e.g., `src/nodes/lighting/main.cpp`):

```cpp
const uint8_t NODE_TANK_ID = 1;          // Which tank (1-255)
const char* NODE_NAME = "LightingNode01"; // Unique name for this instance
```

The `NODE_TYPE` is pre-configured for each node type and doesn't need to be changed.
## 🎯 Current Milestone: Discovery & Heartbeat

**Goal**: Establish basic ESP-NOW communication before adding hardware control.

- [x] Hub listens for broadcasts
- [x] Node announces itself
- [x] Hub registers node dynamically
- [x] Hub sends ACK
- [x] Node receives ACK and switches to unicast
- [x] Heartbeat exchange works
- [x] Connection timeout detection
- [ ] Multi-tank support tested
- [ ] Web UI for hub
- [ ] Hardware control modules

## 🧪 Testing

1. Flash hub firmware to ESP32
2. Flash node firmware to ESP8266 (configure node type first)
3. Power both devices
4. Monitor serial output:
   - Hub should show node discovery and registration
   - Node should show ACK received and heartbeat exchange

## 🐛 Debugging

Enable verbose ESP-NOW logging in `hub/platformio.ini`:

```ini
build_flags = 
    -DCORE_DEBUG_LEVEL=5  ; VERBOSE level
```

## 📚 Design Principles

- **Clarity over cleverness** - Explicit state machines
- **Non-blocking** - No delays in production code
- **Deterministic** - Predictable behavior under all conditions
- **Modular** - Single responsibility per component
- **Fail-safe** - Safety is paramount

## 🔮 Future Enhancements

- Web UI for monitoring and control
- SPIFFS/LittleFS for configuration persistence
- OTA firmware updates
- Historical data logging
- Advanced scheduling (sunrise/sunset simulation, feeding schedules)
- Mobile app integration
- Alarm/notification system

## 📄 License

This is a personal/educational project. Use and modify as needed.

## 🤝 Contributing

This is currently a single-user project, but suggestions and improvements are welcome via issues or pull requests.

---

**Status**: 🚧 Work in Progress - Milestone 1 (Discovery & Heartbeat) ✅ Complete
