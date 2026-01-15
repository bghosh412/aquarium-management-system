# Build System Instructions - PlatformIO Configuration

**Complete guide for building and deploying Aquarium Management System firmware**

---

## 📋 Overview

This project uses **PlatformIO** as the build system with a single `platformio.ini` configuration file at the project root. The system supports multiple build targets (1 hub + 6 node types) with clean separation and isolated build environments.

---

## 🏗️ Build System Architecture

### Single platformio.ini Design

- **One configuration file** controls all build targets
- **7 separate environments**: 1 hub + 6 node types
- **Custom src_dir per environment**: Each target has isolated source directory
- **Custom data_dir per environment**: Each target has isolated filesystem data
- **Shared libraries**: `lib/NodeBase/` automatically included by all environments
- **Common headers**: `include/protocol/` available to all targets

### Why This Approach?

✅ Clean separation between targets  
✅ No complex src_filter rules needed  
✅ Each target is self-contained  
✅ Easier to understand and maintain  
✅ Better IDE support  
✅ No build conflicts between environments  

---

## 📂 Directory Structure

### Per-Environment Layout

Each environment follows this structure:

```
src/
├── hub/                    # ESP32 Hub
│   ├── src/
│   │   └── main.cpp        # Hub firmware entry point
│   └── data/               # Filesystem files (LittleFS)
│       ├── config/
│       │   └── config.json
│       └── UI/
│           ├── index.html
│           ├── styles/
│           ├── scripts/
│           └── images/
│
└── nodes/                  # ESP8266 Nodes
    ├── fish_feeder/
    │   ├── src/
    │   │   └── main.cpp
    │   └── data/
    │
    ├── co2_regulator/
    │   ├── src/
    │   │   └── main.cpp
    │   └── data/
    │
    ├── lighting/
    │   ├── src/
    │   │   └── main.cpp
    │   └── data/
    │
    ├── heater/
    │   ├── src/
    │   │   └── main.cpp
    │   └── data/
    │
    ├── water_quality/
    │   ├── src/
    │   │   └── main.cpp
    │   └── data/
    │
    └── repeater/
        ├── src/
        │   └── main.cpp
        └── data/
```

### Shared Resources

```
lib/
└── NodeBase/               # Shared node communication library
    ├── node_base.h         # Interface declarations
    ├── node_base.cpp       # ESP-NOW logic, sendStatus(), etc.
    └── library.json        # PlatformIO library metadata

include/
└── protocol/
    └── messages.h          # Shared ESP-NOW protocol definitions
```

---

## 🎯 Environment Naming Conventions

### Hub Environment
- **Name**: `hub_esp32`
- **MCU**: ESP32-S3-N16R8 (16MB Flash, 8MB PSRAM)
- **Framework**: Arduino

### Node Environments
All nodes use ESP8266 with Arduino framework:

| Environment Name | Node Type | Purpose |
|-----------------|-----------|---------|
| `node_fish_feeder` | Fish Feeder | Servo-based feeding |
| `node_co2_regulator` | CO₂ Regulator | Solenoid valve control |
| `node_lighting` | Lighting | 3-channel PWM LED control |
| `node_heater` | Heater | Temperature management |
| `node_water_quality` | Water Quality | pH/TDS/temp sensors |
| `node_repeater` | Repeater | ESP-NOW range extender |

---

## 🔧 platformio.ini Configuration

### Hub Environment Example

```ini
[env:hub_esp32]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
src_dir = src/hub/src          # Isolated source directory
data_dir = src/hub/data        # WebUI and config files
board_build.flash_size = 16MB
board_build.psram_type = opi
lib_deps =
    # Hub-specific libraries
```

### Node Environment Example

```ini
[env:node_lighting]
platform = espressif8266
board = esp12e
framework = arduino
src_dir = src/nodes/lighting/src     # Isolated source directory
data_dir = src/nodes/lighting/data   # Reserved for future use
lib_deps =
    # NodeBase library auto-discovered from lib/
```

### Key Configuration Points

- **src_dir**: Points to environment-specific source code
- **data_dir**: Set per-environment via tools/set_data_dir.py for `buildfs` and `uploadfs`.
- **Hub UI source**: `src/hub/data/UI` is the authoritative UI folder used to build LittleFS.
- **Legacy folder**: The project root `data/` directory is legacy and should not be used for hub filesystem builds.
- **No src_filter needed**: Each environment already isolated by directory
- **lib_deps**: Environment-specific dependencies
- **Common includes**: `include/` directory available to all environments

---

## 🚀 Build Commands

### Compile Firmware

> **Important:** If you are using a Python virtual environment (venv), always activate it before running any PlatformIO (pio) command:

```bash
source .venv/bin/activate
```

All `pio` commands below assume your venv is active.

**Build specific environment:**
```bash
platformio run --environment hub_esp32
platformio run --environment node_lighting
platformio run --environment node_co2_regulator
platformio run --environment node_fish_feeder
platformio run --environment node_heater
platformio run --environment node_water_quality
platformio run --environment node_repeater
```

**Build all environments:**
```bash
platformio run
```

**Clean build artifacts:**
```bash
platformio run --target clean
```

**Clean and rebuild:**
```bash
platformio run --target clean
platformio run
```

---

## 📤 Upload Commands

### Firmware Upload

**Upload to specific device:**
```bash
platformio run --environment hub_esp32 --target upload
platformio run --environment node_lighting --target upload
```

**Specify upload port (if multiple devices connected):**
```bash
platformio run --environment node_lighting --target upload --upload-port /dev/ttyUSB0
```

### Filesystem Upload (Hub Only)

**Upload WebUI and config files to LittleFS:**
```bash
platformio run --environment hub_esp32 --target uploadfs
```

This uploads contents of `src/hub/data/` to the ESP32's filesystem.

**Verify filesystem before upload:**
```bash
ls -R src/hub/data/
```

---

## 📚 Library Management

### Automatic Library Discovery

PlatformIO automatically discovers libraries in `lib/` directory. No manual configuration needed.

### NodeBase Shared Library

Located at `lib/NodeBase/`, this library provides:
- ESP-NOW initialization
- Message handling framework
- Status reporting functions
- Base node functionality

**Structure:**
```
lib/NodeBase/
├── node_base.h         # Interface declarations
├── node_base.cpp       # Implementation
└── library.json        # Metadata
```

**Linking:**
- Automatically linked to all node environments
- No explicit `lib_deps` entry required
- Include in node code: `#include "node_base.h"`

### Adding External Libraries

Add to `lib_deps` in specific environment:

```ini
[env:node_water_quality]
lib_deps =
    adafruit/Adafruit Sensor@^1.1.9
    adafruit/DHT sensor library@^1.4.4
```

Or add to common section for all environments:

```ini
[env]
lib_deps =
    # Common dependencies
```

### Library Troubleshooting

**Issue**: Undefined reference to NodeBase functions  
**Solution**: Ensure `lib/NodeBase/library.json` exists

**Issue**: Multiple definition errors  
**Solution**: Check that `node_base.cpp` is ONLY in `lib/NodeBase/`, not duplicated elsewhere

---

## 🐛 Common Build Issues and Solutions

### 1. Undefined Reference Errors

**Symptom:**
```
undefined reference to `setupESPNow(unsigned char, NodeType)'
```

**Cause**: NodeBase library not found or not properly configured

**Solution:**
```bash
# Verify library.json exists
ls -l lib/NodeBase/library.json

# Clean and rebuild
platformio run --target clean
platformio run --environment node_lighting
```

### 2. Multiple Definition Errors

**Symptom:**
```
multiple definition of `setupESPNow(unsigned char, NodeType)'
```

**Cause**: Source file duplicated in multiple locations

**Solution:**
- Ensure `node_base.cpp` exists ONLY in `lib/NodeBase/`
- Do not copy library files to `src/` directories
- Check no duplicate includes in different src_dirs

### 3. ESP-NOW Initialization Fails

**Symptom:**
```
ESP-NOW init failed
```

**Cause**: Wi-Fi channel mismatch or incorrect initialization

**Solution:**
- Verify all devices use Channel 6 (`ESPNOW_CHANNEL` macro)
- Check that `WiFi.mode(WIFI_STA)` called before `esp_now_init()`
- Ensure ESP-NOW initialized before loop()

### 4. Messages Not Received

**Symptom**: Node announces but hub doesn't respond

**Cause**: Peer MAC address issues or send errors

**Solution:**
- Verify hub listening on broadcast address (0xFF:0xFF:0xFF:0xFF:0xFF:0xFF)
- Check `esp_now_send()` return status
- Add debug logging to ESP-NOW send/receive callbacks
- Ensure message size ≤ 250 bytes

### 5. Filesystem Upload Fails

**Symptom:**
```
Error uploading filesystem
```

**Cause**: Incorrect data_dir or LittleFS partition issues

**Solution:**
```bash
# Verify data directory exists and has content
ls -R src/hub/data/

# Check partition scheme in platformio.ini
# Should have sufficient SPIFFS/LittleFS space

# Try manual erase first
platformio run --environment hub_esp32 --target erase
platformio run --environment hub_esp32 --target uploadfs
```

### 6. Include Path Errors

**Symptom:**
```
fatal error: protocol/messages.h: No such file or directory
```

**Cause**: Include directory not in search path

**Solution:**
- Use `#include "protocol/messages.h"` (not `<protocol/messages.h>`)
- The `include/` directory is automatically in the search path
- Verify file exists: `ls include/protocol/messages.h`

### 7. Board Not Found

**Symptom:**
```
Error: Unknown board ID 'esp32-s3-devkitc-1'
```

**Solution:**
```bash
# Update PlatformIO platforms
platformio platform update

# Or install specific platform
platformio platform install espressif32
platformio platform install espressif8266
```

---

## 🧪 Testing and Debugging

### Serial Monitor

**Monitor specific environment:**
```bash
platformio device monitor --environment hub_esp32
```

**Monitor with custom baud rate:**
```bash
platformio device monitor --environment node_lighting --baud 115200
```

**Monitor with filters:**
```bash
platformio device monitor --filter esp8266_exception_decoder
```

**Exit monitor**: `Ctrl+C`

### Build Verification

**Always verify compilation before committing:**
```bash
# Build all environments to catch cross-platform issues
platformio run

# Check for warnings
platformio run 2>&1 | grep warning
```

### Debugging Tips

1. **Enable verbose output:**
   ```ini
   [env:node_lighting]
   build_flags = 
       -DDEBUG_ESP_PORT=Serial
       -DDEBUG_ESP_CORE
   ```

2. **Add debug macros:**
   ```cpp
   #ifdef DEBUG
   #define DEBUG_PRINT(x) Serial.print(x)
   #define DEBUG_PRINTLN(x) Serial.println(x)
   #else
   #define DEBUG_PRINT(x)
   #define DEBUG_PRINTLN(x)
   #endif
   ```

3. **Monitor ESP-NOW callbacks:**
   ```cpp
   void onDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
       Serial.printf("Received %d bytes from %02X:%02X:...\n", len, mac[0], mac[1]);
   }
   ```

4. **Check memory usage:**
   ```bash
   platformio run --environment hub_esp32 --target size
   ```

### Unit Testing

**Create test in `test/` directory:**
```
test/
└── test_protocol/
    └── test_messages.cpp
```

**Run tests:**
```bash
platformio test --environment hub_esp32
```

---

## 📊 Build Output Analysis

### Understanding Build Output

```
RAM:   [====      ]  40.2% (used 32960 bytes from 81920 bytes)
Flash: [======    ]  58.7% (used 612345 bytes from 1044464 bytes)
```

**Critical thresholds:**
- **RAM > 80%**: Risk of stack overflow, optimize memory usage
- **Flash > 90%**: Limited space for OTA updates, reduce code size

### Optimization Flags

**Reduce binary size:**
```ini
[env:node_lighting]
build_flags = 
    -Os              # Optimize for size
    -ffunction-sections
    -fdata-sections
```

**Increase performance:**
```ini
build_flags = 
    -O2              # Optimize for speed
```

---

## 🔄 OTA Updates (Future)

### Preparation for OTA

**Reserve space in partition table:**
```ini
[env:hub_esp32]
board_build.partitions = default_16MB.csv
```

**Enable OTA in code:**
```cpp
#include <ArduinoOTA.h>

void setupOTA() {
    ArduinoOTA.setHostname("aquarium-hub");
    ArduinoOTA.begin();
}
```

**Build for OTA:**
```bash
platformio run --environment hub_esp32 --target upload --upload-port 192.168.1.100
```

---

## 🎯 Best Practices

### Before Committing Code

```bash
# 1. Build all environments
platformio run

# 2. Check for warnings
platformio run 2>&1 | grep -i warning

# 3. Test firmware on target device
platformio run --environment node_lighting --target upload
platformio device monitor --environment node_lighting

# 4. Verify no build artifacts committed
git status
# Ensure .pio/ and .vscode/ excluded by .gitignore
```

### Development Workflow

1. **Edit source code** in `src/<target>/src/main.cpp`
2. **Build**: `platformio run --environment <target>`
3. **Fix errors** if any
4. **Upload**: `platformio run --environment <target> --target upload`
5. **Monitor**: `platformio device monitor --environment <target>`
6. **Test functionality**
7. **Commit** if working

### Multi-Device Testing

```bash
# Terminal 1: Upload to first node
platformio run --environment node_lighting --target upload --upload-port /dev/ttyUSB0

# Terminal 2: Upload to second node
platformio run --environment node_co2_regulator --target upload --upload-port /dev/ttyUSB1

# Terminal 3: Upload to hub
platformio run --environment hub_esp32 --target upload --upload-port /dev/ttyUSB2

# Terminal 4: Monitor hub
platformio device monitor --environment hub_esp32 --port /dev/ttyUSB2
```

---

## 📝 Configuration Management

### platformio.ini Best Practices

**Use common sections for shared configuration:**
```ini
[env]
framework = arduino
monitor_speed = 115200
lib_deps =
    # Common libraries for all environments

[env:hub_esp32]
extends = env
platform = espressif32
src_dir = src/hub/src

[env:node_lighting]
extends = env
platform = espressif8266
src_dir = src/nodes/lighting/src
```

### Build Flags Management

**Common flags:**
```ini
[env]
build_flags =
    -DESPNOW_CHANNEL=6
    -DMAX_NODE_NAME_LEN=32
```

**Environment-specific flags:**
```ini
[env:hub_esp32]
build_flags =
    ${env.build_flags}
    -DBOARD_HAS_PSRAM
```

---

## 🔐 Security Considerations

### Production Builds

**Disable debug output:**
```ini
[env:node_lighting]
build_flags =
    -DNDEBUG              # Disable assert()
    -DNO_GLOBAL_SERIAL    # Disable Serial
```

**Enable security features:**
```ini
build_flags =
    -DESP_NOW_ENCRYPT=1   # Enable ESP-NOW encryption
```

---

## 📖 Additional Resources

- **PlatformIO Docs**: https://docs.platformio.org
- **ESP32 Arduino Core**: https://github.com/espressif/arduino-esp32
- **ESP8266 Arduino Core**: https://github.com/esp8266/Arduino
- **ESP-NOW Protocol**: https://www.espressif.com/en/products/software/esp-now

---

**Last Updated**: December 29, 2025  
**Version**: 1.0  
**Project**: Aquarium Management System  
**Repository**: bghosh412/aquarium-management-system
