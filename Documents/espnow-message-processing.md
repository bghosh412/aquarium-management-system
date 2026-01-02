# ESP-NOW Message Processing Architecture
## Aquarium Management System (Hub + Nodes)

This document defines authoritative rules for how ESP-NOW messages are sent, received, fragmented, reassembled, queued, and processed between the Hub (ESP32-S3) and Nodes (ESP8266 / ESP32).
This file is intended to be used as context for GitHub Copilot.

---

## 1. Roles

### Hub
- Central controller
- Maintains node registry and online/offline status
- Sends commands and configuration to nodes
- Receives telemetry, heartbeats, and alerts
- Bridges ESP-NOW to MQTT / Web UI / Home Assistant

### Node
- Peripheral controller (light, CO₂, heater, doser, sensors)
- Executes commands from hub
- Sends telemetry and heartbeats
- Maintains minimal state
- Safety-first behavior (fail-safe defaults)

---

## 2. Transport Rules (ESP-NOW)

- Connectionless, best-effort
- No buffering for offline peers
- Broadcast = fire-and-forget
- Unicast = limited retries only
- Reliability handled at application layer

---

## 3. Message Categories

| Category | Fragmented |
|--------|------------|
Heartbeat | No |
Telemetry | No |
Safety Command | No |
Control Command | No |
Configuration | Yes |
Schedules | Yes |

Safety commands must always be single-frame and idempotent.

---

## 4. Message Header

```cpp
struct MsgHeader {
  uint16_t seq;
  bool     is_last;
  uint16_t payload_len;
};
```

Rules:
- seq identifies logical message
- Same seq for all fragments
- seq increments after completion
- One fragmented message in-flight per sender

---

## 5. Fragmentation Rules

- Fragment size fits ESP-NOW MTU
- Sent strictly in order
- No interleaving
- No retransmission

---

## 6. Node Reassembly

```cpp
struct ReassemblyContext {
  bool     active;
  uint16_t seq;
  uint16_t offset;
  uint32_t start_time_ms;
  uint8_t  buffer[MAX_MESSAGE_SIZE];
};
```

Algorithm:
1. If inactive → start
2. seq mismatch → drop + restart
3. Append payload
4. If is_last → process + reset

---

## 7. Timeout

```cpp
#define REASSEMBLY_TIMEOUT_MS 1500
```

Timeout drops partial message.

---

## 8. Duplicate Protection

Nodes track last completed seq.
Duplicate seq is ignored.

---

## 9. Queues

- Node RX fragment queue: 3–4 entries
- Hub TX queue: per-node, fragmented messages only

---

## 10. Offline Handling

Hub:
- Detects offline via heartbeat or send failure
- Stops fragmented sends
- Queues important messages only

Node:
- On boot sends heartbeat
- Resets reassembly state

---

## 11. Heartbeats

- Interval: 2–5s
- Broadcast
- Used for discovery and liveness

---

## 12. Safety Rules

- Never fragment safety commands
- Commands must be repeatable
- Node defaults to safe state on error

---

## 13. Forbidden Behaviors

Copilot must NOT generate:
- TCP-like ACK/NACK layers
- Fragment retransmission logic
- Multiple reassembly contexts
- Fragment interleaving
- Blocking delays in RX callbacks

---

## 14. Philosophy

- Deterministic
- Fail-safe
- Minimal state
- Hub smart, nodes simple

---

## End of Specification
