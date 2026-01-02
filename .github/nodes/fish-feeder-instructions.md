# Fish Feeder Node Instructions

**Servo-based automatic fish feeding system**

---

## Hardware

- **MCU**: ESP8266
- **Actuator**: Servo motor (0-180°)
- **Power**: 5V servo, 3.3V logic
- **Pin**: GPIO 5 (D1 on NodeMCU)

## Functionality

- Dispenses 1-5 portions of food
- Servo rotation per portion (configurable angle)
- Sequential dispensing with delays
- Returns to home position after feeding

## Command Format

```cpp
// commandData[0] = command type
#define CMD_FEED 0x01

// commandData[1] = number of portions (1-5)
uint8_t portions = commandData[1];
```

## State Machine

```
IDLE → FEEDING → DISPENSING → RETURNING → IDLE
```

## Safety

- **Fail-safe**: Do nothing (safer to skip feeding than overfeed)
- **Max portions**: 5 (hardcoded limit)
- **Timeout**: 30 seconds max for complete cycle

## Implementation Notes

- Use Servo.h library
- Non-blocking delays between portions
- Track dispense count in state
- Return to 0° when idle

---

**File**: `src/nodes/fish_feeder/src/main.cpp`  
**Build**: `platformio run --environment node_fish_feeder`
