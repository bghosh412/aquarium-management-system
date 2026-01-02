# Node Architecture Summary

## Overview

Each node type is a **complete, independent firmware** with hardware-specific functionality. All nodes share the same ESP-NOW communication base but differ in their hardware control logic.

## Node Types & Responsibilities

### 1. Fish Feeder (`node_fish_feeder`)
- **Hardware**: Servo motor for dispensing mechanism
- **Commands**: Dispense food (1-5 portions)
- **Fail-Safe**: Do nothing (safer to skip one feeding)
- **Build**: `pio run -e node_fish_feeder -t upload`

### 2. CO₂ Regulator (`node_co2_regulator`)
- **Hardware**: Solenoid valve for gas control
- **Commands**: Open for duration, Close immediately
- **Fail-Safe**: **CLOSE VALVE** (critical - prevents overdose)
- **Build**: `pio run -e node_co2_regulator -t upload`

### 3. Lighting (`node_lighting`)
- **Hardware**: 3x PWM channels (white, blue, red)
- **Commands**: Set levels 0-255, Enable/disable
- **Fail-Safe**: Hold last state or gradual dim
- **Build**: `pio run -e node_lighting -t upload`

### 4. Heater (`node_heater`)
- **Hardware**: Relay + DS18B20 temperature sensor
- **Commands**: Set target temp, Auto/manual mode
- **Fail-Safe**: **TURN OFF** (critical - prevents overheating)
- **Build**: `pio run -e node_heater -t upload`

### 5. Water Quality (`node_water_quality`)
- **Hardware**: pH sensor, TDS sensor, temp sensor
- **Commands**: Request reading, Calibrate
- **Fail-Safe**: Continue reading (sensors are passive/safe)
- **Build**: `pio run -e node_water_quality -t upload`

## Shared Architecture

All nodes share:
- **`lib/NodeBase/`** - Shared library (PlatformIO convention)
  - `node_base.h` - Common interface declarations
  - `node_base.cpp` - ESP-NOW communication logic
  - `library.json` - Library metadata
- **`include/protocol/messages.h`** - Message structures

Each node implements in `src/nodes/<type>/main.cpp`:
```cpp
void setupHardware();        // Initialize pins/peripherals
void enterFailSafeMode();    // Safe state on connection loss
void handleCommand();        // Process hub commands
void updateHardware();       // Called every loop iteration
```

The `NodeBase` library is automatically discovered and linked by PlatformIO.

## Configuration Per Node

Before uploading to each physical device:

```cpp
const uint8_t NODE_TANK_ID = 1;      // ⚠️ Which tank
const char* NODE_NAME = "Light01";   // ⚠️ Unique identifier
```

Example multi-tank setup:
- Tank 1: `LightNode_T1`, `CO2Node_T1`, `HeaterNode_T1`
- Tank 2: `LightNode_T2`, `CO2Node_T2`, `HeaterNode_T2`

## Development Workflow

1. **Choose node type** (e.g., lighting)
2. **Edit** `src/nodes/lighting/main.cpp`
   - Set `NODE_TANK_ID`
   - Set `NODE_NAME`
   - Adjust pin definitions if needed
3. **Build** `pio run -e node_lighting`
4. **Upload** `pio run -e node_lighting -t upload`
5. **Monitor** `pio device monitor -e node_lighting`

## Adding New Node Type

1. Create directory: `src/nodes/new_type/`
2. Copy `node_base.cpp` from another node
3. Create `main.cpp` with specific implementation
4. Add environment to `platformio.ini`:
   ```ini
   [env:node_new_type]
   platform = ${common_esp8266.platform}
   board = ${common_esp8266.board}
   framework = ${common_esp8266.framework}
   monitor_speed = ${common_esp8266.monitor_speed}
   upload_speed = ${common_esp8266.upload_speed}
   build_flags = ${common_esp8266.build_flags}
   src_filter = 
       +<*>
       -<hub/>
       -<nodes/other_nodes/>
       +<nodes/new_type/>
   ```
5. Update `include/protocol/messages.h` if new `NodeType` enum needed

## Safety Design

| Node Type      | On Connection Loss | Rationale                       |
|----------------|--------------------|---------------------------------|
| Fish Feeder    | Do nothing         | Skip feeding safer than double  |
| CO₂ Regulator  | **CLOSE valve**    | Prevent gas overdose            |
| Lighting       | Hold or dim        | No safety risk                  |
| Heater         | **TURN OFF**       | Prevent overheating/fire        |
| Water Quality  | Continue reading   | Passive sensors, no risk        |

## PlatformIO Build System

The `src_filter` in each environment ensures **only relevant code compiles**:

```ini
src_filter = 
    +<*>              # Include all src/
    -<hub/>           # Exclude hub
    -<nodes/fish_feeder/>
    -<nodes/co2_regulator/>
    # ... exclude others
    +<nodes/lighting/>  # Include only this node
```

This allows all firmware to coexist in one repository while building independently.
