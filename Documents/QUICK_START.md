# Quick Reference - PlatformIO Build Commands

## Project Structure (Standard PlatformIO)

```
Aquarium-Management-System/
├── platformio.ini           # Single config, 6 environments
├── include/
│   └── protocol/
│       └── messages.h      # Shared ESP-NOW protocol
├── lib/
│   └── NodeBase/           # 📚 Shared library for all nodes
│       ├── node_base.h
│       ├── node_base.cpp
│       └── library.json
├── src/
│   ├── hub/
│   │   └── main.cpp        # ESP32 hub firmware
│   └── nodes/              # All node types (ESP8266)
│       ├── fish_feeder/main.cpp
│       ├── co2_regulator/main.cpp
│       ├── lighting/main.cpp
│       ├── heater/main.cpp
│       └── water_quality/main.cpp
└── Documents/              # All documentation
```

## Build Commands (VS Code)

### Using PlatformIO Sidebar (Recommended)
1. Click PlatformIO icon in sidebar
2. Expand "PROJECT TASKS"
3. Select environment: `hub_esp32`, `node_esp8266`, or `node_esp32c3`
4. Click "Build" or "Upload"

### Using Command Palette
- `Ctrl+Shift+P` → "PlatformIO: Build"
- `Ctrl+Shift+P` → "PlatformIO: Upload"

## Build Environments

### `hub_esp32` - Central Controller
- **Board**: ESP32-WROOM (upesy_wroom)
- **Source**: `src/hub/main.cpp`
- **Features**: FreeRTOS, node discovery, safety orchestration

### Node Environments (All ESP8266)

#### `node_fish_feeder` - Fish Feeder
- **Source**: `src/nodes/fish_feeder/`
- **Hardware**: Servo for feeding mechanism
- **Fail-safe**: Do nothing (safer to skip feeding)

#### `node_co2_regulator` - CO₂ Control
- **Source**: `src/nodes/co2_regulator/`
- **Hardware**: Solenoid valve
## Configuration

### Node Configuration
Edit the specific node's `main.cpp` before building:

**Example**: `src/nodes/lighting/main.cpp`
```cpp
const uint8_t NODE_TANK_ID = 1;          // Which tank (1-255)
const char* NODE_NAME = "LightingNode01"; // Unique name
```

**Example**: `src/nodes/co2_regulator/main.cpp`
```cpp
const uint8_t NODE_TANK_ID = 2;          // Different tank
const char* NODE_NAME = "CO2Node_Tank2";
```*Hardware**: Relay + temp sensor
- **Fail-safe**: Turn OFF (critical safety)

#### `node_water_quality` - Sensors
- **Source**: `src/nodes/water_quality/`
- **Hardware**: pH, TDS, temperature sensors
- **Fail-safe**: Continue reading (read-only)

## Configuration

### Node Configuration
Edit `src/node/main.cpp` before building each node:

```cpp
#define NODE_TANK_ID 1               // Which tank (1-255)
#define NODE_TYPE NodeType::LIGHT    // LIGHT, CO2, DOSER, SENSOR, etc.
#define NODE_NAME "LightNode01"      // Unique name
```

### ESP-NOW Channel
Set in `platformio.ini` (applies to all):

```ini
[common]
build_flags = 
    -DESPNOW_CHANNEL=1
```

## How It Works

PlatformIO uses **source filters** to compile only relevant code:
## First Build

1. Open project in VS Code
2. PlatformIO will auto-detect platformio.ini
3. Build hub: Select `hub_esp32` environment → Build → Upload
4. Build a node:
   - Configure `src/nodes/lighting/main.cpp` (set TANK_ID and NAME)
   - Select `node_lighting` environment → Build → Upload
5. Monitor both devices and watch them discover each other!

1. Open project in VS Code
2. PlatformIO will auto-detect platformio.ini
3. Select `hub_esp32` environment
4. Build → Upload → Monitor
5. Then do the same for a node (configure it first!)
