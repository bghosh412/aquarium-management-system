# Copilot Instructions – Aquarium Management System

You are assisting with development of a **modular, distributed aquarium management system**.

Follow the instructions below strictly when generating code, suggestions, or refactors.

---

## 1️⃣ Project Architecture (MANDATORY)

### Core Design

- One **central HUB**:
  - MCU: **ESP32-WROOM**
  - Responsibilities:
    - System logic
    - Scheduling
    - UI (web)
    - Logging
    - Node discovery
    - Safety orchestration
- Multiple **peripheral NODES**:
  - MCU: **ESP8266** or **ESP32-C3**
  - Each node controls **one subsystem only** (light, CO₂, doser, sensors)

⚠️ Nodes must NOT contain global decision logic. Also, Hub and Nodes would have their own platformio.ini file and would be built and deployed seperately.

---

## 2️⃣ Communication Rules

- Communication protocol: **ESP-NOW**
- No MQTT
- No TCP/IP between hub and nodes
- No router required for ESP-NOW operation
- Fixed Wi-Fi channel (defined by `ESPNOW_CHANNEL` macro)

### Discovery Model

- Nodes send **broadcast ANNOUNCE** messages on boot
- Hub listens permanently for broadcasts
- Hub dynamically registers peers using sender MAC
- Hub replies with **unicast ACK**
- Nodes switch to unicast after ACK

Do NOT hardcode MAC addresses.

---

## 3️⃣ Message Model

### Message Types (enum-based)

- ANNOUNCE
- ACK
- COMMAND
- STATUS
- HEARTBEAT

### Identification

- Each message includes:
  - Tank ID
  - Node Type
- MAC address is implicit (from ESP-NOW callback)

Messages must be implemented as **C/C++ structs**, not JSON strings over the air.

---

## 4️⃣ Multi-Aquarium Support

- System must support **multiple aquariums**
- Each aquarium is identified by a **Tank ID**
- Same firmware reused across tanks via configuration
- Hub logic must be tank-aware

Do not assume a single aquarium.

---

## 5️⃣ Threading / Concurrency

### ESP32 (Hub)

- Use **FreeRTOS tasks**
- Tasks must be short, deterministic, and non-blocking
- Use:
  - Queues for message passing
  - Semaphores / mutexes for shared data
  - Event groups for state signaling

Do NOT simulate threads manually.

### ESP8266 (Nodes)

- Single-threaded
- Non-blocking `loop()`
- State-machine driven logic
- Use timers instead of delays

---

## 6️⃣ Fail-Safe Philosophy (CRITICAL)

On communication loss or error:

- Lights → hold last safe state or soft OFF
- CO₂ → OFF
- Dosing → OFF
- Heaters → OFF

Safety always overrides functionality.

Never generate code that leaves actuators ON by default.

---

## 7️⃣ Platform & Tooling

- Build system: **PlatformIO**
- Framework: **Arduino**
- Code must compile under PlatformIO environments
- Avoid Arduino IDE–specific assumptions

---

## 8️⃣ Repository Structure (Expected)

```
aquarium-controller/
├── protocol/      // shared enums & message structs
├── hub/           // ESP32 hub firmware
├── node/          // ESP8266 / ESP32-C3 node firmware
└── README.md
```

Shared protocol code must not be duplicated.

---

## 9️⃣ Coding Style & Principles

- Prefer clarity over cleverness
- Explicit state machines
- No blocking delays
- Deterministic behavior
- Modular files with single responsibility
- Verbose logging is acceptable during development

Avoid “quick hacks”.

---

## 🔟 First Milestone Assumptions

Initial implementation goals:

- Hub boots and listens for ESP-NOW
- Node announces itself
- Hub registers node
- Node receives ACK
- Heartbeat exchange works

No hardware control logic should be added before this milestone works.

---

## 11️⃣ Forbidden Assumptions

Do NOT assume:

- Always-on Wi-Fi
- Internet connectivity
- Cloud services
- Single tank
- Single node
- MQTT / HTTP between nodes

---

## 12️⃣ Design Philosophy

This is a **serious embedded system**, not a demo sketch.

Design for:

- Scalability
- Maintainability
- Fault isolation
- Long-term evolution

When in doubt:

- Choose safer behavior
- Choose simpler design
- Ask for clarification via TODO comments
