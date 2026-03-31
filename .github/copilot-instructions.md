# Copilot Instructions – Aquarium Management System

**Main instructions file with links to specialized documentation.**

---

## 📚 Quick Navigation

This project uses **modular instructions**. Based on what you're working on, refer to:

### Core Architecture
- **[Hub Instructions](./hub-instructions.md)** - ESP32-S3 hub development
- **[Node Instructions](./node-instructions.md)** - Common node patterns and base functionality

### Specific Node Types
- **[Fish Feeder Node](./nodes/fish-feeder-instructions.md)** - Servo-based feeding
- **[CO₂ Regulator Node](./nodes/co2-regulator-instructions.md)** - Solenoid valve control
- **[Lighting Node](./nodes/lighting-instructions.md)** - 3-channel PWM LED control
- **[Heater Node](./nodes/heater-instructions.md)** - Temperature management
- **[Water Quality Node](./nodes/water-quality-instructions.md)** - Sensor monitoring
- **[Repeater Node](./nodes/repeater-instructions.md)** - ESP-NOW range extender

### System-Wide
- **[Protocol Specification](./protocol-instructions.md)** - Message formats and ESP-NOW rules
- **[ESP-NOW Message Processing Architecture](../Documents/espnow-message-processing.md)** - Fragmentation, reassembly, queuing, and transport rules
- **[Build System](./build-instructions.md)** - PlatformIO configuration

---

## 🎯 Project Overview

**Modular, distributed aquarium management system**

### Architecture
- **1 HUB** (ESP32-S3-N16R8) - Central controller, web UI, safety orchestration
- **N NODES** (ESP8266) - Specialized devices, one function per node
- **Communication**: ESP-NOW (Channel 6, no router required)
- **Multi-tank support**: Tank ID in every message

### Core Principles
1. **Safety First** - Fail-safe behavior on disconnect
2. **No Global Logic in Nodes** - Hub makes all decisions
3. **Non-blocking Code** - State machines, no delays
4. **Modular Design** - Clean separation of concerns
5. **No Unsolicited Documentation** - Do NOT create .md files unless explicitly requested by user

---

## 🚀 Quick Start

### Working on Hub?
```bash
# Read hub instructions
cat .github/hub-instructions.md

# Build hub
platformio run --environment hub_esp32
```

### Working on a Node?
```bash
# Read node base + specific node instructions
cat .github/node-instructions.md
cat .github/nodes/lighting-instructions.md

# Build node
platformio run --environment node_lighting
```

### Working on Protocol?
```bash
# Read protocol instructions
cat .github/protocol-instructions.md

# Edit protocol
vim include/protocol/messages.h
```

---

## 🛡️ Safety Rules (CRITICAL)

**On communication loss or error:**

| Node Type | Fail-Safe Action |
|-----------|-----------------|
| Lights | Hold last state (safe) |
| CO₂ | **OFF** (critical - prevent overdose) |
| Heater | **OFF** (critical - prevent overheating) |
| Feeder | Do nothing (safer to skip) |
| Sensors | Continue reading (passive) |
| Repeater | Continue forwarding (passive) |

**Never generate code that leaves actuators ON by default.**

---

## 📦 Current Status

### ✅ Implemented
- Hub firmware with discovery and watchdog
- 6 node types (feeder, CO₂, lights, heater, sensors, repeater)
- ESP-NOW protocol with multi-part commands
- WebUI with real-time monitoring
- Fail-safe behavior per device

### 🚧 Pending
- Hardware testing on physical devices
- WebSocket server integration in hub
- OTA firmware updates
- Cloud logging

---

## 💡 Tips for Copilot

- **Context matters**: Load the relevant instructions file for your task
- **Follow patterns**: Each node type has a template structure
- **Safety first**: Always implement fail-safe modes
- **No blocking code**: Use state machines and timers
- **Test compilation**: Run `platformio run` before committing
- **No documentation files**: Do NOT create markdown (.md) documentation files unless explicitly requested by the user

### Required Pipeline for Any Code Change
When the user asks for a change that requires code updates (single-item or multi-item), follow this exact pipeline:
1. Initialize eval framework for each line item.
2. Detail requirements and acceptance criteria and update the respective story.md. Also create an implementation plan that details out the required changes (file name, method name, and what to update). Name the implementation plan as per eval framework nomenclature.
3. Generate code.
4. Build and check for build errors.
5. Run eval and show output JSON files in chat.

See [eval_instruction.md](eval_instruction.md) for story/plan formats and artifact conventions.

---

**Last Updated**: January 15, 2026  
**Version**: 2.1 (No Unsolicited Documentation)  
**Repository**: bghosh412/aquarium-management-system
