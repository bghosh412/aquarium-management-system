# CO2 Regulator Node — Implementation Summary

**pH-based CO2 injection control for aquarium management**

---

## Files Changed

1. **`src/nodes/co2_regulator/src/main.cpp`** — Complete rewrite following the fish_feeder architecture pattern
2. **`src/nodes/co2_regulator/data/node_config.txt`** — New config with all CO2-specific settings
3. **`platformio.ini`** — Board changed to `d1_mini`, added `lib_deps`

---

## Architecture & Design

| Aspect | Detail |
|--------|--------|
| **Platform** | ESP8266 D1 Mini |
| **pH Sensor** | DFRobot Gravity Analog on **A0** (only ADC on ESP8266) |
| **Relay** | **D7/GPIO13** (HIGH=CO2 ON, LOW=CO2 OFF) |
| **Status LED** | **D5/GPIO14** (moved from D7 to avoid relay pin conflict) |
| **Fail-safe** | **CO2 OFF** — always OFF at boot, on disconnect, and on any error |

---

## pH Reading

- Reads every **5 minutes** (configurable via `PH_READ_INTERVAL_MS`)
- Takes **10 ADC samples**, sorts them, discards **2 outliers from each end**, averages the rest
- Converts to pH via configurable calibration: `pH = slope × voltage + offset`
- Default calibration matches DFRobot Gravity pH Sensor V2

---

## Command Protocol (Hub → Node)

| Cmd | Action |
|-----|--------|
| `0` | Emergency stop (CO2 OFF) |
| `1` | CO2 ON (relay HIGH) |
| `2` | CO2 OFF (relay LOW) |
| `40` | Status request (immediate pH read + report) |
| `0xFF` | Force fail-safe (test) |

---

## Status Data (Node → Hub, 8 bytes)

| Bytes | Content |
|-------|---------|
| `[0]` | Relay state (0=OFF, 1=ON) |
| `[1-4]` | pH value (float, little-endian) |
| `[5-6]` | Raw ADC (uint16_t) |
| `[7]` | Error flags (0=OK, 1=sensor_error, 2=safety_cutoff) |

---

## Safety Layers

1. **Boot safety** — Relay always starts OFF
2. **Local emergency cutoff** — If pH < 5.0 (configurable), CO2 forced OFF regardless of hub state
3. **Hub disconnect fail-safe** — CO2 OFF when hub heartbeat lost
4. **Safety cutoff lock** — After local cutoff, CO2 can't be re-enabled until hub sends emergency stop (cmd 0) to clear

---

## Build

```bash
# Compile
pio run -e node_co2_regulator

# Upload firmware
pio run -e node_co2_regulator -t upload

# Upload filesystem (node_config.txt)
pio run -e node_co2_regulator -t uploadfs

# Serial monitor
pio device monitor -e node_co2_regulator
```

**Build stats:** RAM 48.1%, Flash 31.8% — plenty of headroom on D1 Mini.

---

**Date:** March 31, 2026
