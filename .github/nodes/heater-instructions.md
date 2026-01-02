# Heater Node Instructions

**Temperature control with relay and sensor**

---

## Hardware

- **MCU**: ESP8266
- **Actuator**: Relay module (heater control)
- **Sensor**: DS18B20 or DHT22 temperature sensor
- **Pins**:
  - Relay: GPIO 5 (D1)
  - Sensor: GPIO 4 (D2)

## Functionality

- Auto mode: Maintains target temperature
- Manual mode: Direct ON/OFF control
- Temperature range: 18-32°C
- Hysteresis: ±0.5°C

## Command Format

```cpp
// commandData[0] = command type
#define CMD_SET_TEMP 0x01
#define CMD_SET_MODE 0x02

// commandData[1] = mode (0=manual, 1=auto)
// commandData[2-5] = target temp (float, °C)
uint8_t mode = commandData[1];
float targetTemp = *(float*)&commandData[2];
```

## State Machine

```
IDLE → HEATING → MAINTAINING → COOLING → IDLE
```

## Safety

- **Fail-safe**: **OFF** (critical - prevent overheating)
- **Temp limits**: 18-32°C (enforced)
- **Timeout**: Turn off on communication loss
- **Overheat protection**: Force off at 35°C

## Implementation Notes

- Read temperature every 5 seconds
- Use hysteresis to prevent relay chatter
- Auto mode: Compare current vs target
- Manual mode: Direct relay control
- Always monitor temperature

---

**File**: `src/nodes/heater/src/main.cpp`  
**Build**: `platformio run --environment node_heater`
