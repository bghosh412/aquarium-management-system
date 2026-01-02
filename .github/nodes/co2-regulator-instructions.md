# CO₂ Regulator Node Instructions

**Solenoid valve control for CO₂ injection**

---

## Hardware

- **MCU**: ESP8266
- **Actuator**: Solenoid valve (12V, relay-controlled)
- **Power**: 12V external, 3.3V logic
- **Pin**: GPIO 4 (D2 on NodeMCU)

## Functionality

- Opens valve for specified duration
- Duration range: 1-3600 seconds (1 hour max)
- Immediate close command
- Automatic timeout

## Command Format

```cpp
// commandData[0] = command type
#define CMD_START 0x01
#define CMD_STOP 0x02

// commandData[1-4] = duration in seconds (uint32_t)
uint32_t duration = *(uint32_t*)&commandData[1];
```

## State Machine

```
IDLE → OPENING → INJECTING → CLOSING → IDLE
```

## Safety

- **Fail-safe**: **CLOSE** (critical - prevent CO₂ overdose)
- **Max duration**: 3600 seconds (1 hour)
- **Auto-close**: On communication loss
- **Emergency stop**: Immediate close on STOP command

## Implementation Notes

- Use relay module (active LOW or HIGH)
- Track injection start time
- Monitor elapsed time in loop()
- Force close on timeout or disconnect

---

**File**: `src/nodes/co2_regulator/src/main.cpp`  
**Build**: `platformio run --environment node_co2_regulator`
