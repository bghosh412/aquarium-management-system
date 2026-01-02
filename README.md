# Aquarium Management System

A modular, distributed aquarium management system using ESP32/ESP8266 microcontrollers with ESP-NOW communication.

## 🌟 Features

- **Distributed Architecture**: Central ESP32 hub + multiple ESP8266 nodes
- **ESP-NOW Communication**: Direct peer-to-peer, no router required
- **Auto-Discovery**: Nodes announce themselves, hub registers dynamically
- **Multi-Tank Support**: Manage multiple aquariums from one hub
- **Fail-Safe Design**: Critical systems turn OFF on connection loss
- **Modular Nodes**: Fish feeder, CO₂ regulator, lighting, heater, water quality sensors

## 🚀 Quick Start

```bash
# Clone repository
git clone <your-repo-url>
cd Aquarium-Management-System

# Build hub
pio run -e hub_esp32 -t upload

# Build a node (example: lighting)
pio run -e node_lighting -t upload

# Monitor
pio device monitor
```

## 📦 Node Types

| Node | Hardware | Fail-Safe | Description |
|------|----------|-----------|-------------|
| **Fish Feeder** | Servo | Skip feeding | Automated feeding system |
| **CO₂ Regulator** | Solenoid | ⚠️ CLOSE valve | CO₂ injection control |
| **Lighting** | 3x PWM LED | Hold state | Multi-channel LED control |
| **Heater** | Relay + Sensor | ⚠️ TURN OFF | Temperature management |
| **Water Quality** | pH/TDS/Temp | Continue | Real-time monitoring |

## 📚 Documentation

- **[README](Documents/README.md)** - Complete project overview
- **[QUICK_START](Documents/QUICK_START.md)** - Build and setup guide
- **[NODE_ARCHITECTURE](Documents/NODE_ARCHITECTURE.md)** - Node design patterns
- **[SYSTEM_DIAGRAM](Documents/SYSTEM_DIAGRAM.md)** - Visual architecture
- **[PROTOCOL_UPDATES](Documents/PROTOCOL_UPDATES.md)** - Message protocol details
- **[MIGRATION](Documents/MIGRATION.md)** - Project history

## 🏗️ Architecture

```
         ESP-NOW (No Router)
                │
        ┌───────▼──────┐
        │  Hub (ESP32) │
        │  - Discovery │
        │  - Safety    │
        │  - Web UI    │
        └───────┬──────┘
                │
    ┌───────────┼───────────┐
    │           │           │
┌───▼───┐   ┌──▼───┐   ┌──▼───┐
│ Light │   │ CO₂  │   │Heater│  ...
│ESP8266│   │ESP8266│   │ESP8266│
└───────┘   └──────┘   └──────┘
```

## 🛠️ Tech Stack

- **Platform**: PlatformIO
- **Framework**: Arduino
- **Hub MCU**: ESP32-WROOM
- **Node MCUs**: ESP8266 (NodeMCU v2)
- **Protocol**: ESP-NOW
- **Hub OS**: FreeRTOS

## 📋 Requirements

- PlatformIO (CLI or VS Code extension)
- ESP32-WROOM board (hub)
- ESP8266 boards (nodes)
- Hardware per node type (servos, relays, sensors, etc.)

## 🔧 Configuration

Edit each node's `src/nodes/<type>/main.cpp` before building:

```cpp
const uint8_t NODE_TANK_ID = 1;          // Which tank (1-255)
const char* NODE_NAME = "LightNode01";    // Unique identifier
```

## 🔐 Safety

Critical safety features:
- **CO₂**: Valve CLOSES on connection loss (prevent overdose)
- **Heater**: TURNS OFF on connection loss (prevent overheating)
- **Lighting**: Safe to hold last state
- **Feeder**: Safe to skip feeding

All nodes monitor connection and enter fail-safe mode after 90 seconds without heartbeat.

## 📊 Build Environments

| Environment | Board | Description |
|-------------|-------|-------------|
| `hub_esp32` | ESP32-WROOM | Central controller |
| `node_fish_feeder` | ESP8266 | Fish feeding system |
| `node_co2_regulator` | ESP8266 | CO₂ injection |
| `node_lighting` | ESP8266 | LED lighting |
| `node_heater` | ESP8266 | Temperature control |
| `node_water_quality` | ESP8266 | Sensor array |

## 🤝 Contributing

This is a personal/educational project. Feel free to fork and adapt for your needs.

## 📝 License

Open source - use and modify as needed for your aquarium projects.

## ⚠️ Disclaimer

This system controls critical aquarium functions. Always:
- Test thoroughly before deploying
- Have backup systems for critical functions
- Monitor system regularly
- Never rely solely on automation for animal welfare

## 🌐 Future Plans

- [ ] Web UI for hub
- [ ] Mobile app
- [ ] Cloud logging
- [ ] Advanced scheduling
- [ ] Email/SMS alerts
- [ ] OTA firmware updates

---

**Built with ❤️ for the aquarium hobby**
