# System Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         AQUARIUM MANAGEMENT SYSTEM                       │
└─────────────────────────────────────────────────────────────────────────┘

                        ESP-NOW Communication (No Router)
                                  Channel 1

    ┌───────────────────────────────────────────────────────────────┐
    │                    HUB (ESP32-WROOM)                          │
    │  ┌─────────────────────────────────────────────────────────┐  │
    │  │  • Node Discovery (broadcast listener)                  │  │
    │  │  • Peer Registration (dynamic MAC tracking)             │  │
    │  │  • FreeRTOS Tasks (watchdog, scheduling)                │  │
    │  │  • Web UI / Logging                                     │  │
    │  │  • Safety Orchestration                                 │  │
    │  │  • Multi-Tank Coordinator                               │  │
    │  └─────────────────────────────────────────────────────────┘  │
    │              Env: hub_esp32 | Board: upesy_wroom              │
    └───────────────────────────────────────────────────────────────┘
                                    │
                    ┌───────────────┼───────────────┐
                    │               │               │
            ┌───────▼──────┐ ┌─────▼─────┐  ┌─────▼─────┐
            │   ANNOUNCE   │ │  COMMAND  │  │ HEARTBEAT │
            │ (broadcast)  │ │ (unicast) │  │ (unicast) │
            └───────┬──────┘ └─────┬─────┘  └─────┬─────┘
                    │               │               │
        ┌───────────┼───────────────┼───────────────┼───────────────┐
        │           │               │               │               │
    ┌───▼───┐   ┌──▼────┐      ┌──▼────┐      ┌──▼────┐      ┌───▼───┐
    │ Fish  │   │  CO₂  │      │Light  │      │Heater │      │ Water │
    │Feeder │   │  Reg  │      │Control│      │Control│      │Quality│
    └───────┘   └───────┘      └───────┘      └───────┘      └───────┘
    ESP8266     ESP8266        ESP8266        ESP8266        ESP8266
    
    Servo       Solenoid       3x PWM         Relay+Sensor   3x Sensors
    
    Fail-Safe:  Fail-Safe:     Fail-Safe:     Fail-Safe:     Fail-Safe:
    Skip        ⚠️ CLOSE       Hold/Dim       ⚠️ OFF         Continue
    feeding     valve          LEDs           heater         reading


══════════════════════════════════════════════════════════════════════════
                           MESSAGE FLOW
══════════════════════════════════════════════════════════════════════════

1. NODE BOOT → Discovery Phase
   ┌──────┐                                              ┌─────┐
   │ Node │──────── ANNOUNCE (broadcast) ──────────────→│ Hub │
   └──────┘                                              └─────┘
            Tank ID: 1, Type: LIGHT, Name: "Light01"       │
                                                            │
   ┌──────┐                                              ┌─────┐
   │ Node │←──────── ACK (unicast) ──────────────────────│ Hub │
   └──────┘       Assigned ID: 42, Accepted: true       └─────┘
            Saves Hub MAC, adds as peer

2. CONNECTED → Normal Operation
   ┌──────┐                                              ┌─────┐
   │ Node │──────── HEARTBEAT (every 30s) ─────────────→│ Hub │
   └──────┘       Health: 100, Uptime: 120 min         └─────┘
   
   ┌──────┐                                              ┌─────┐
   │ Node │←──────── COMMAND ─────────────────────────────│ Hub │
   └──────┘       ID: 1, Data: [255, 0, 128, ...]      └─────┘
            Executes hardware-specific action
   
   ┌──────┐                                              ┌─────┐
   │ Node │──────── STATUS ──────────────────────────────→│ Hub │
   └──────┘       Sensor readings, state info           └─────┘

3. CONNECTION LOSS → Fail-Safe Mode
   ┌──────┐                                              ┌─────┐
   │ Node │  ✗ No heartbeat for 90s                     │ Hub │
   └──────┘                                              └─────┘
       │
       ├──→ enters FAIL_SAFE mode
       ├──→ CO₂: CLOSE valve
       ├──→ Heater: TURN OFF
       ├──→ Light: HOLD state
       └──→ Attempts reconnection every 5s


══════════════════════════════════════════════════════════════════════════
                      BUILD ENVIRONMENTS
══════════════════════════════════════════════════════════════════════════

PlatformIO Project: Single platformio.ini with 6 environments

┌────────────────────┬──────────────┬─────────────────────────────────┐
│ Environment        │ MCU          │ Source Path                     │
├────────────────────┼──────────────┼─────────────────────────────────┤
│ hub_esp32          │ ESP32-WROOM  │ src/hub/                        │
│ node_fish_feeder   │ ESP8266      │ src/nodes/fish_feeder/          │
│ node_co2_regulator │ ESP8266      │ src/nodes/co2_regulator/        │
│ node_lighting      │ ESP8266      │ src/nodes/lighting/             │
│ node_heater        │ ESP8266      │ src/nodes/heater/               │
│ node_water_quality │ ESP8266      │ src/nodes/water_quality/        │
└────────────────────┴──────────────┴─────────────────────────────────┘

Build:   pio run -e <environment>
Upload:  pio run -e <environment> -t upload
Monitor: pio device monitor -e <environment>


══════════════════════════════════════════════════════════════════════════
                      MULTI-TANK EXAMPLE
══════════════════════════════════════════════════════════════════════════

                            ┌─────────────────┐
                            │   SINGLE HUB    │
                            │   (ESP32)       │
                            └────────┬────────┘
                                     │
                ┌────────────────────┼────────────────────┐
                │                    │                    │
          Tank ID: 1             Tank ID: 2          Tank ID: 3
          ┌──────────┐          ┌──────────┐        ┌──────────┐
          │ Light_T1 │          │ Light_T2 │        │ Light_T3 │
          │  CO2_T1  │          │  CO2_T2  │        │ Heat_T3  │
          │ Heat_T1  │          │ Sensor_T2│        └──────────┘
          │Sensor_T1 │          └──────────┘
          └──────────┘

Each node configured with its tank ID before deployment.
Hub routes commands/reads based on tank ID in message header.


══════════════════════════════════════════════════════════════════════════
                      SAFETY MATRIX
══════════════════════════════════════════════════════════════════════════

Node Type       │ Normal Mode      │ Fail-Safe Mode   │ Safety Level
────────────────┼──────────────────┼──────────────────┼──────────────
Fish Feeder     │ Dispense on cmd  │ Skip feeding     │ Low risk
CO₂ Regulator   │ Timed injection  │ ⚠️ VALVE CLOSED  │ ⚠️ CRITICAL
Lighting        │ PWM levels       │ Hold last state  │ Low risk
Heater          │ Auto temp ctrl   │ ⚠️ HEATER OFF    │ ⚠️ CRITICAL
Water Quality   │ Read & report    │ Continue reading │ No risk

⚠️ CRITICAL: These nodes MUST fail to safe state on hub loss
```
