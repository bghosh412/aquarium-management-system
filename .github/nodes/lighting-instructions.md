# Lighting Node Instructions

**3-channel PWM LED control system**

---

## Hardware

- **MCU**: ESP8266
- **Channels**: 3 (White, Blue, Red)
- **PWM Range**: 0-255 (8-bit)
- **Pins**: 
  - White: GPIO 12 (D6)
  - Blue: GPIO 13 (D7)
  - Red: GPIO 15 (D8)

## Functionality

- Independent control of 3 LED channels
- PWM dimming (0-100%)
- Enable/disable per channel
- Smooth transitions (optional)

## Command Format

```cpp
// commandData[0] = command type
#define CMD_SET_LEVELS 0x01
#define CMD_ENABLE 0x02
#define CMD_DISABLE 0x03

// commandData[1] = white level (0-255)
// commandData[2] = blue level (0-255)
// commandData[3] = red level (0-255)
uint8_t white = commandData[1];
uint8_t blue = commandData[2];
uint8_t red = commandData[3];
```

## State Machine

```
IDLE → UPDATING → TRANSITIONING → IDLE
```

## Safety

- **Fail-safe**: Hold last state (safe for lights)
- **Max PWM**: 255 (no overdrive)
- **Gradual changes**: Optional smooth dimming

## Implementation Notes

- Use analogWrite() for PWM
- ESP8266 PWM range: 0-1023, scale to 0-255
- Non-blocking transitions using millis()
- Store last known state for fail-safe

---

**File**: `src/nodes/lighting/src/main.cpp`  
**Build**: `platformio run --environment node_lighting`
