# MicroCore Framework Integration Instructions

**Instructions for using MicroCore framework in the Aquarium Management System.**

---

## 📚 Framework Overview

**MicroCore** is a modular framework for ESP32/ESP8266 IoT projects. This project uses the following MicroCore libraries:

| Library | Include | Purpose |
|---------|---------|---------|
| Core | `<MicroCore.h>` | Logger, MacUtils, TimeUtils, Constants |
| ConfigManager | `<ConfigManager.h>` | JSON/KEY=VALUE config files |
| FileManager | `<FileManager.h>` | Safe file operations, atomic writes |
| StateManager | `<StateManager.h>` | State with observer pattern |
| WebApiFramework | `<WebApiFramework.h>` | REST API helpers (ESP32 only) |
| OtaManager | `<OtaManager.h>` | OTA firmware updates |

**Note**: This project uses its own ESPNowManager (not MicroCore's ESPNowLib) for ESP-NOW communication.

---

## 🔧 Installation

MicroCore is installed via `lib_extra_dirs` in platformio.ini:

```ini
[env:hub_esp32]
lib_extra_dirs = /home/pi/Desktop/MicroCore/lib
```

---

## 📝 Logger Usage

**Replace Serial.printf/println with LOG macros:**

```cpp
#include <MicroCore.h>

void setup() {
    Logger::begin(115200, LOG_DEBUG);
    
    LOG_DEBUG("Debug message: value=%d", 42);
    LOG_INFO("Info message");
    LOG_WARN("Warning message");
    LOG_ERROR("Error message");
}
```

**Migration from Serial:**
```cpp
// OLD - DON'T USE
Serial.printf("[OTA] Starting update from: %s\n", url.c_str());

// NEW - USE THIS
LOG_INFO("[OTA] Starting update from: %s", url.c_str());
```

---

## ⚙️ ConfigManager Usage

**For hub_config.txt (KEY=VALUE format):**

```cpp
#include <ConfigManager.h>

ConfigManager config;

void loadHubConfig() {
    if (config.loadKeyValue("/config/hub_config.txt")) {
        // Get values with defaults
        bool heartbeatEnabled = config.getBool("HEARTBEAT_ENABLED", true);
        uint32_t interval = config.getUInt("HEARTBEAT_INTERVAL_SEC", 30);
        String otaUrl = config.getString("HUB_FIRMWARE_OTA_URL", "");
    }
}
```

**For JSON config files:**

```cpp
ConfigManager jsonConfig;

void loadJsonConfig() {
    if (jsonConfig.loadJson("/config/config.json")) {
        int maxDevices = jsonConfig.getInt("maxDevices", 10);
    }
}
```

---

## 📁 FileManager Usage

**Replace direct LittleFS calls:**

```cpp
#include <FileManager.h>

void setup() {
    // Initialize LittleFS
    FileManager::begin(true);  // true = format on fail
}

// Writing JSON safely (atomic write prevents corruption)
void saveDevices(const JsonDocument& doc) {
    FileManager::writeJsonAtomic("/config/devices.json", doc);
}

// Reading JSON
void loadDevices() {
    JsonDocument doc;
    if (FileManager::readJson("/config/devices.json", doc)) {
        // Process doc
    }
}

// Check if file exists
if (FileManager::exists("/config/hub_config.txt")) { ... }

// Read raw text
String content = FileManager::readString("/config/hub_config.txt");
```

---

## 📊 StateManager Usage (Optional)

**For centralized state tracking:**

```cpp
#include <StateManager.h>

StateManager hubState;

void setup() {
    // Track hub state
    hubState.set("wifiConnected", false);
    hubState.set("ntpSynced", false);
    hubState.set("nodesOnline", 0);
    
    // Register observer for state changes
    hubState.onChange("nodesOnline", [](const char* key, const StateValue& val) {
        LOG_INFO("Nodes online changed to: %d", val.intValue);
    });
}
```

---

## 🌐 WebApiFramework Usage

**Simplify API endpoint registration:**

```cpp
#include <WebApiFramework.h>

AsyncWebServer server(80);
WebApi api(server);

void setupWebServer() {
    api.enableCors();
    api.enableLogging(true);
    
    // GET endpoint
    api.get("/api/status", [](AsyncWebServerRequest* request) {
        JsonDocument doc;
        doc["uptime"] = millis() / 1000;
        doc["freeHeap"] = ESP.getFreeHeap();
        WebApi::sendData(request, doc);
    });
    
    // POST endpoint
    api.post("/api/command", [](AsyncWebServerRequest* request) {
        // Parse body
        JsonDocument body;
        if (!WebApi::parseJsonBody(request, body)) {
            WebApi::sendBadRequest(request, "Invalid JSON");
            return;
        }
        // Process command
        WebApi::sendSuccess(request, "Command executed");
    });
    
    // Error handling
    api.setNotFoundHandler([](AsyncWebServerRequest* request) {
        WebApi::sendNotFound(request);
    });
    
    server.begin();
}
```

**Response helpers:**

```cpp
// Send JSON data (200 OK)
WebApi::sendData(request, doc);

// Send success message (200 OK)
WebApi::sendSuccess(request, "Operation completed");

// Send error (with status code)
WebApi::sendError(request, 400, "Invalid request");

// Send 404 Not Found
WebApi::sendNotFound(request);

// Send 500 Server Error
WebApi::sendServerError(request, "Internal error");
```

---

## 🔄 OtaManager Usage

**For Hub OTA updates:**

```cpp
#include <OtaManager.h>

void setupOta() {
    OtaManager::begin("aquarium-hub");
    OtaManager::setVersion(config.getString("HUB_FIRMWARE_VERSION", "1.0.0").c_str());
    
    OtaManager::onStart([]() {
        LOG_INFO("[OTA] Update starting...");
    });
    
    OtaManager::onProgress([](int percent) {
        LOG_INFO("[OTA] Progress: %d%%", percent);
    });
    
    OtaManager::onEnd([]() {
        LOG_INFO("[OTA] Update complete, rebooting...");
    });
    
    OtaManager::onError([](const char* error) {
        LOG_ERROR("[OTA] Error: %s", error);
    });
}

void loop() {
    OtaManager::loop();  // Handle ArduinoOTA requests
}

// HTTP OTA
bool performUpdate(const String& url) {
    return OtaManager::updateFromUrl(url.c_str());
}
```

---

## ⚠️ Important Rules

### DO:
- ✅ Use `LOG_DEBUG/INFO/WARN/ERROR` macros (not `Serial.println`)
- ✅ Use `FileManager::writeJsonAtomic()` for safe persistence
- ✅ Initialize `Logger::begin()` first in `setup()`
- ✅ Use ArduinoJson v7 API (`JsonDocument`, not `DynamicJsonDocument`)
- ✅ Check return values from file operations
- ✅ Use `millis()` for non-blocking timing

### DON'T:
- ❌ Use `DynamicJsonDocument` (deprecated in v7)
- ❌ Use `delay()` in main loop (blocks everything)
- ❌ Mix Serial.print with LOG macros in new code
- ❌ Forget to call `OtaManager::loop()` in hub

---

## 🔄 Migration Checklist

When migrating existing code to MicroCore:

1. **Replace Serial.printf/println with LOG macros**
   ```cpp
   // Before
   Serial.printf("[Module] Message: %d\n", value);
   // After
   LOG_INFO("[Module] Message: %d", value);
   ```

2. **Replace LittleFS file operations with FileManager**
   ```cpp
   // Before
   File file = LittleFS.open(path, "r");
   // After
   String content = FileManager::readString(path);
   ```

3. **Replace config parsing with ConfigManager**
   ```cpp
   // Before: Manual KEY=VALUE parsing
   // After
   config.loadKeyValue("/config/hub_config.txt");
   bool enabled = config.getBool("HEARTBEAT_ENABLED", true);
   ```

4. **Use WebApi response helpers**
   ```cpp
   // Before
   request->send(200, "application/json", json);
   // After
   WebApi::sendData(request, doc);
   ```

---

## 📂 Library Location

MicroCore is located at: `/home/pi/Desktop/MicroCore/lib/`

Libraries included:
- Core (Logger, MacUtils, TimeUtils, Constants)
- ConfigManager
- FileManager  
- StateManager
- WebApiFramework
- OtaManager

---

**MicroCore Version:** 1.0.0  
**Last Updated:** January 2026
