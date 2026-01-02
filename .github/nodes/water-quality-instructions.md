# Water Quality Node Instructions

**Multi-sensor monitoring system (pH, TDS, Temperature)**

---

## Hardware

- **MCU**: ESP8266
- **Sensors**:
  - pH sensor (analog, A0)
  - TDS sensor (analog, via multiplexer or second ADC)
  - DS18B20 temperature sensor (digital, GPIO 4)
- **Power**: 5V sensors, 3.3V logic

## Functionality

- Reads pH, TDS, and temperature
- Periodic sampling (every 5 seconds)
- Sends STATUS messages with sensor data
- No actuators (passive monitoring)

## Status Data Format

```cpp
// statusData layout:
// [0-3] = pH value (float)
// [4-7] = TDS value (uint16_t, ppm)
// [8-11] = temperature (float, °C)

float pH = *(float*)&statusData[0];
uint16_t tds = *(uint16_t*)&statusData[4];
float temp = *(float*)&statusData[8];
```

## State Machine

```
IDLE → READING → CALIBRATING → IDLE
```

## Safety

- **Fail-safe**: Continue reading (passive, no risk)
- **Sensor validation**: Check for out-of-range values
- **Calibration**: Store offsets in EEPROM

## Implementation Notes

- Use OneWire + DallasTemperature for DS18B20
- pH sensor: Analog read with voltage-to-pH conversion
- TDS sensor: Analog read with temperature compensation
- Apply moving average filter to reduce noise
- Send periodic STATUS updates to hub

---

**File**: `src/nodes/water_quality/src/main.cpp`  
**Build**: `platformio run --environment node_water_quality`
