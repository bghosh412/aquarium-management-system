# Hub Data Directory Structure

This directory contains all filesystem data for the ESP32 hub, organized into two main categories:

## 📁 Directory Structure

```
src/hub/data/
├── config/              # Configuration files
│   └── config.json      # System configuration
│
└── UI/                  # Web user interface
    ├── index.html       # Main dashboard
    ├── images/          # Images, icons, logos
    ├── styles/          # CSS stylesheets
    │   └── styles.css   # Main stylesheet
    └── scripts/         # JavaScript files
        └── app.js       # Main application logic
```

## 🔧 Configuration (`config/`)

Contains JSON and text configuration files:

- **config.json** - Main system configuration
  - Tank definitions
  - ESP-NOW settings (channel, timeouts)
  - Safety limits (temperature, durations)
  - Scheduling configuration

### Usage in Hub Firmware

```cpp
#include <LittleFS.h>
#include <ArduinoJson.h>

// Read configuration
File configFile = LittleFS.open("/config/config.json", "r");
StaticJsonDocument<2048> doc;
deserializeJson(doc, configFile);
configFile.close();

// Access values
int channel = doc["espnow"]["channel"];
int maxHeaterTemp = doc["safety"]["maxHeaterTemp"];
```

## 🎨 User Interface (`UI/`)

Web-based dashboard for monitoring and controlling the aquarium system.

### HTML Files
- **index.html** - Main dashboard page

### Styles (`UI/styles/`)
- **styles.css** - Main stylesheet with dark theme
- Responsive design for mobile and desktop
- CSS variables for easy theming

### Scripts (`UI/scripts/`)
- **app.js** - WebSocket client and UI logic
- Auto-reconnection on disconnect
- Real-time node status updates
- Control panels for all node types

### Images (`UI/images/`)
- Placeholder for logos, icons, backgrounds
- Recommended formats: SVG (scalable), PNG (raster)

## 🚀 Uploading to ESP32

### Using PlatformIO

```bash
# Upload filesystem to ESP32
platformio run --environment hub_esp32 --target uploadfs
```

This uploads the entire `src/hub/data/` directory to the ESP32's LittleFS partition.

### File System Configuration

In `platformio.ini`:
```ini
[env:hub_esp32]
board_build.filesystem = littlefs
data_dir = src/hub/data
```

## 🌐 Web Server Configuration

The hub firmware must serve files from the correct paths:

```cpp
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>

AsyncWebServer server(80);

void setupWebServer() {
    // Initialize filesystem
    if (!LittleFS.begin()) {
        Serial.println("LittleFS mount failed");
        return;
    }
    
    // Serve UI files
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(LittleFS, "/UI/index.html", "text/html");
    });
    
    server.on("/UI/styles/styles.css", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(LittleFS, "/UI/styles/styles.css", "text/css");
    });
    
    server.on("/UI/scripts/app.js", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(LittleFS, "/UI/scripts/app.js", "application/javascript");
    });
    
    // Serve config files (API endpoint)
    server.on("/config/config.json", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(LittleFS, "/config/config.json", "application/json");
    });
    
    // Serve static files from directories
    server.serveStatic("/UI/", LittleFS, "/UI/");
    server.serveStatic("/config/", LittleFS, "/config/");
    
    server.begin();
}
```

## 📦 File Size Considerations

LittleFS partition size is limited (typically 1-2MB). Keep files optimized:

- ✅ Minify CSS and JavaScript for production
- ✅ Compress images (use WebP for modern browsers)
- ✅ Use SVG for icons (smaller than PNG)
- ✅ Avoid large libraries (prefer vanilla JS)

### Current File Sizes
- index.html: ~5KB
- styles.css: ~5KB
- app.js: ~8KB
- config.json: ~1KB
- **Total: ~19KB** (plenty of room for expansion)

## 🔒 Security Notes

⚠️ **Current implementation has NO authentication**

For production:
- Add authentication to web endpoints
- Protect `/config/` directory from direct access
- Use HTTPS (requires certificate)
- Implement API tokens for WebSocket

## 🎯 URL Structure

When accessing from browser:

```
http://<hub-ip>/                          → Dashboard (index.html)
http://<hub-ip>/UI/styles/styles.css     → Stylesheet
http://<hub-ip>/UI/scripts/app.js        → JavaScript
http://<hub-ip>/UI/images/logo.svg       → Images (when added)
http://<hub-ip>/config/config.json       → Configuration (API)
ws://<hub-ip>/ws                          → WebSocket connection
```

## 📝 Adding New Files

### Adding a New Page
1. Create HTML file in `UI/`
2. Link stylesheet: `<link href="/UI/styles/styles.css">`
3. Add route in hub firmware

### Adding Images
1. Place files in `UI/images/`
2. Reference in HTML: `<img src="/UI/images/logo.svg">`
3. Keep total size under 500KB

### Adding Config Files
1. Create JSON/TXT in `config/`
2. Load in firmware using LittleFS
3. Consider EEPROM for critical settings

## 🔄 Development Workflow

1. **Edit files** in `src/hub/data/`
2. **Test locally** (optional: use local web server)
3. **Upload to ESP32**: `pio run -e hub_esp32 -t uploadfs`
4. **Access**: Navigate to `http://<hub-ip>/`

## 📚 Related Documentation

- Hub firmware: `src/hub/src/main.cpp`
- Protocol definitions: `include/protocol/messages.h`
- Build configuration: `platformio.ini`
- Project instructions: `.github/copilot-instructions.md`

---

**Last Updated**: December 29, 2025  
**Structure Version**: 2.0 (organized hierarchy)
