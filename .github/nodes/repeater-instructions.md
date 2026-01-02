# Repeater Node Instructions

**ESP-NOW range extender for hub-node communication**

---

## Hardware

- **MCU**: ESP8266 or ESP32-C3
- **Power**: USB 5V (always-on)
- **No peripherals**: Pure relay function

## Functionality

- Transparent message forwarding
- Auto-discovers hub MAC address
- Bidirectional relay (hub ↔ nodes)
- Can be daisy-chained for extended range

## Architecture

```
Node → Repeater → Hub
Hub → Repeater → Nodes (broadcast)
```

## Operation

1. **Boot**: Sends ANNOUNCE to discover hub
2. **Discovery**: Learns hub MAC from ACK messages
3. **Active**: Forwards all messages transparently
4. **Heartbeat**: Sends periodic status to hub

## Message Routing

- **From Hub**: Broadcast to all nodes
- **From Node**: Unicast to hub only
- **No filtering**: Forwards all message types

## State Machine

```
INIT → DISCOVERING → ACTIVE → FAIL_SAFE
```

## Safety

- **Fail-safe**: Continue forwarding (passive relay)
- **No timeout**: Repeater never stops trying
- **Statistics**: Track messages forwarded

## Implementation Notes

- Use ESP-NOW callbacks (onDataReceived)
- Learn hub MAC dynamically (no hardcoding)
- Forward messages immediately (minimal latency)
- Use COMBO role for bidirectional communication
- No message modification (transparent)

## Range Extension

- **ESP-NOW range**: ~200m outdoor, ~50m indoor
- **With repeater**: 2x-3x range extension
- **Multiple repeaters**: Daisy-chain for larger areas
- **Placement**: Strategic positioning for coverage

---

**File**: `src/nodes/repeater/src/main.cpp`  
**Build**: `platformio run --environment node_repeater`
