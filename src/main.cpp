/*
 * Aquarium Management System - Hub Firmware
 * ESP32-S3-N16R8 Central Controller
 * 
 * Architecture:
 * - Core 0: Main loop - ESP-NOW message processing, web server, system orchestration
 * - Core 0: ESP-NOW + scheduler + watchdog task
 * - Core 1: Web UI task
 * 
 * Features:
 * - tzapu WiFiManager for configuration
 * - AsyncWebServer on port 80
 * - mDNS responder (ams.local)
 * - FreeRTOS dual-core task architecture
 * - ESP-NOW message queue for thread-safe processing
 * - Memory monitoring with aggressive management
 * - Configuration-driven heartbeat
 * 
 * MicroCore Framework Integration:
 * - Logger: Leveled logging with timestamps
 * - ConfigManager: KEY=VALUE config file parsing
 * - FileManager: Safe file operations
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <strings.h>  // for strcasecmp
#include <esp_now.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include <map>
#include <time.h>  // For NTP time
#include <esp_task_wdt.h>  // For watchdog feeding during OTA
#include "protocol/messages.h"
#include "models/Aquarium.h"
#include "managers/AquariumManager.h"
#include <HTTPClient.h>
#include <memory>
#include "Constant.h"
#include "ESPNowManager.h"
#include "ws_server.h"

// MicroCore Framework
#include <MicroCore.h>
#include <ConfigManager.h>
#include <FileManager.h>
#include <NotificationManager.h>

// ============================================================================
// DUAL LITTLEFS FILESYSTEM ABSTRACTION
// ============================================================================
// When DUAL_LITTLEFS is defined:
//   - StaticFS: UI files, OTA temp, hub_config.txt (partition: static_fs)
//   - UserFS:   JSON config files, schedules (partition: user_fs)
// 
// LittleFS OTA only updates static_fs - user data is PRESERVED!
// ============================================================================

#ifdef DUAL_LITTLEFS
    #include "DualFilesystem.h"
    // Macros for file access - route to appropriate filesystem
    #define FS_STATIC StaticFS
    #define FS_USER   UserFS
#else
    // Single filesystem mode (backward compatible)
    #define FS_STATIC LittleFS
    #define FS_USER   LittleFS
    // Stub out dual filesystem functions
    #define StaticFS LittleFS
    #define UserFS LittleFS
#endif

// ============================================================================
// CONFIGURATION & CONSTANTS
// ============================================================================

// Configuration structure
struct HubConfig {
    bool heartbeatEnabled;
    uint32_t heartbeatIntervalSec;
    uint32_t hubHeartbeatIntervalSec; // seconds
    bool aggressiveMemoryManagement;
    uint32_t heapWarningThresholdKB;
    uint32_t psramWarningThresholdKB;
    String wifiAPName;
    String wifiAPPassword;
    uint32_t wifiTimeoutSec;
    String mdnsHostname;
    uint8_t espnowChannel;
    uint8_t espnowMaxPeers;
    bool debugSerial;
    bool debugESPNOW;
    bool debugWebSocket;

    // Scheduler sleep settings
    uint32_t schedulerMinSleepSec;
    uint32_t schedulerMaxSleepSec;
    
    // Hub OTA Configuration
    String hubFirmwareOtaUrl;     // Base URL for hub firmware OTA
    String hubLittlefsOtaUrl;     // Base URL for hub LittleFS OTA
    String hubFirmwareVersion;    // Current installed firmware version (e.g., "1.0.0")
    String hubLittlefsVersion;    // Current installed LittleFS version (e.g., "1.0.0")
    
    // Node OTA Configuration
    String lightNodeOtaUrl;       // Base URL for lighting node OTA

    // Test-only: expose test endpoints when true (disabled by default)
    bool hubTestMode; // HUB_TEST_MODE (false by default) 
};

struct LightChannelStatus {
    bool ch1;
    bool ch2;
    bool ch3;
    uint32_t updatedAt;
};

static std::map<String, LightChannelStatus> g_lightStatus;
static std::map<String, uint32_t> g_lightStatusPending;

struct WaveMakerStatus {
    bool pumpActive;
    float dutyPercent;
    uint16_t pwmRaw;
    uint32_t updatedAt;
};

static std::map<String, WaveMakerStatus> g_waveMakerStatus;
static std::map<String, uint32_t> g_waveMakerStatusPending;

static String macToString(const uint8_t* mac) {
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(macStr);
}

HubConfig config;

// MicroCore ConfigManager for hub_config.txt
ConfigManager hubConfigManager;

// Task handles
TaskHandle_t watchdogTaskHandle = NULL;
TaskHandle_t webUiTaskHandle = NULL;
TaskHandle_t schedulerTaskHandle = NULL;
TaskHandle_t nodeOtaTaskHandle = NULL;

// Notification Manager (event-driven, async notifications)
NotificationManager notifier;

// OTA Pending State (for deferred processing in loop)
enum class OtaPendingType { NONE, FIRMWARE, LITTLEFS, BOTH };
volatile OtaPendingType otaPending = OtaPendingType::NONE;
volatile bool otaInProgress = false;
String otaPendingFirmwareVersion = "";
String otaPendingLittlefsVersion = "";

// Node OTA state (for async transfer)
struct NodeOtaState {
    bool active;
    bool completed;
    bool success;
    String error;
    String baseUrl;
    uint8_t targetMac[6];
    uint32_t configChunksSent;
    uint32_t firmwareChunksSent;
    uint32_t totalConfigChunks;
    uint32_t totalFirmwareChunks;
    bool configSaved;
    bool configSent;
    bool firmwareSaved;
    bool firmwareSent;
    bool firmwareWaitingEnd;  // True when waiting for READY_END from node
    uint8_t lastCommandId;    // Track command ID for END response
    // Multi-device support
    uint8_t targetMacs[10][6];  // Up to 10 light devices
    uint8_t targetCount;        // Number of devices to update
    uint8_t currentTarget;      // Current device index being updated
    uint8_t devicesUpdated;     // Number of devices successfully updated
    uint8_t devicesFailed;      // Number of devices that failed
    String deviceType;          // Device type name for notifications (e.g., "light")
} nodeOtaState = {0};

// NTP time tracking
static bool ntpSynced = false;

// ============================================================================
// HUB STATUS LED CONFIGURATION (PWM-controlled brightness)
// ============================================================================
// Four status LEDs indicate different system conditions:
// LED_WIFI: WiFi connectivity status
// LED_DEVICE: Device online/offline status  
// LED_TASK: Scheduled task execution indicator
// LED_UNMAPPED: Unmapped device availability
//
// Safe GPIO pins for ESP32-S3 (avoid strapping pins: GPIO0, 3, 45, 46)
// Brightness is controlled via PWM (LEDC) and persisted in JSON config.
// ============================================================================

#define HUB_LED_WIFI       11  // GPIO11 - WiFi status
#define HUB_LED_DEVICE     12  // GPIO12 - Device offline status
#define HUB_LED_TASK       13  // GPIO13 - Scheduled task indicator
#define HUB_LED_UNMAPPED   14  // GPIO14 - Unmapped device indicator

// LEDC PWM channels for each LED
#define HUB_LED_WIFI_CH      0
#define HUB_LED_DEVICE_CH    1
#define HUB_LED_TASK_CH      2
#define HUB_LED_UNMAPPED_CH  3

#define HUB_LED_PWM_FREQ     5000  // 5 kHz PWM frequency
#define HUB_LED_PWM_RES      8     // 8-bit resolution (0-255)

#define HUB_LED_BLINK_INTERVAL_MS  300    // Blink rate (300ms on/off)
#define HUB_LED_TASK_DURATION_MS   30000  // Task indicator blinks for 30 seconds

#define HUB_LED_BRIGHTNESS_FILE "/config/led-brightness.json"
#define HUB_LED_DEFAULT_BRIGHTNESS 20  // Default brightness: 20%

// Forward declaration
void saveLedBrightnessConfig();

// Hub LED state tracking
static bool hubLedWiFiState = false;
static bool hubLedDeviceState = false;
static bool hubLedTaskState = false;
static bool hubLedUnmappedState = false;
static uint32_t hubLedLastToggle[4] = {0, 0, 0, 0};  // Per-LED toggle timers
static uint32_t hubLedTaskTriggerTime = 0;  // When task LED was triggered
static bool hubLedTaskActive = false;  // Task LED blinking active

// Brightness percentages (0-100) for each LED
static uint8_t hubLedBrightness[4] = {
    HUB_LED_DEFAULT_BRIGHTNESS,
    HUB_LED_DEFAULT_BRIGHTNESS,
    HUB_LED_DEFAULT_BRIGHTNESS,
    HUB_LED_DEFAULT_BRIGHTNESS
};

// Convert brightness percentage (0-100) to PWM duty (0-255)
static uint8_t brightnessToduty(uint8_t pct) {
    if (pct == 0) return 0;
    if (pct >= 100) return 255;
    return (uint8_t)((uint16_t)pct * 255 / 100);
}

// Write PWM value to an LED (ON at configured brightness, or OFF)
static void hubLedWrite(uint8_t channel, bool on) {
    if (on) {
        ledcWrite(channel, brightnessToduty(hubLedBrightness[channel]));
    } else {
        ledcWrite(channel, 0);
    }
}

// Re-apply brightness to all LEDs that are currently ON
// Call this after brightness values are changed at runtime.
static void reapplyLedBrightness() {
    hubLedWrite(HUB_LED_WIFI_CH,     hubLedWiFiState);
    hubLedWrite(HUB_LED_DEVICE_CH,   hubLedDeviceState);
    hubLedWrite(HUB_LED_TASK_CH,     hubLedTaskState);
    hubLedWrite(HUB_LED_UNMAPPED_CH, hubLedUnmappedState);
}

// Load LED brightness config from JSON file (creates default if missing)
void loadLedBrightnessConfig() {
    if (FS_USER.exists(HUB_LED_BRIGHTNESS_FILE)) {
        File f = FS_USER.open(HUB_LED_BRIGHTNESS_FILE, "r");
        if (f) {
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, f);
            f.close();
            if (!err) {
                hubLedBrightness[0] = doc["wifi"] | HUB_LED_DEFAULT_BRIGHTNESS;
                hubLedBrightness[1] = doc["device"] | HUB_LED_DEFAULT_BRIGHTNESS;
                hubLedBrightness[2] = doc["task"] | HUB_LED_DEFAULT_BRIGHTNESS;
                hubLedBrightness[3] = doc["unmapped"] | HUB_LED_DEFAULT_BRIGHTNESS;
                // Clamp values to 0-100
                for (int i = 0; i < 4; i++) {
                    if (hubLedBrightness[i] > 100) hubLedBrightness[i] = 100;
                }
                LOG_INFO("LED brightness loaded: wifi=%d%% device=%d%% task=%d%% unmapped=%d%%",
                         hubLedBrightness[0], hubLedBrightness[1],
                         hubLedBrightness[2], hubLedBrightness[3]);
                return;
            }
        }
    }
    // File doesn't exist or failed to parse — create with defaults
    LOG_INFO("LED brightness config not found, creating defaults (%d%%)", HUB_LED_DEFAULT_BRIGHTNESS);
    saveLedBrightnessConfig();
}

// Save LED brightness config to JSON file
void saveLedBrightnessConfig() {
    JsonDocument doc;
    doc["wifi"]     = hubLedBrightness[0];
    doc["device"]   = hubLedBrightness[1];
    doc["task"]     = hubLedBrightness[2];
    doc["unmapped"] = hubLedBrightness[3];

    File f = FS_USER.open(HUB_LED_BRIGHTNESS_FILE, "w");
    if (f) {
        serializeJson(doc, f);
        f.close();
        LOG_INFO("LED brightness config saved");
    } else {
        LOG_ERROR("Failed to save LED brightness config");
    }
}

// Initialize hub status LEDs with PWM
void setupHubStatusLEDs() {
    // Setup LEDC PWM channels
    ledcSetup(HUB_LED_WIFI_CH,     HUB_LED_PWM_FREQ, HUB_LED_PWM_RES);
    ledcSetup(HUB_LED_DEVICE_CH,   HUB_LED_PWM_FREQ, HUB_LED_PWM_RES);
    ledcSetup(HUB_LED_TASK_CH,     HUB_LED_PWM_FREQ, HUB_LED_PWM_RES);
    ledcSetup(HUB_LED_UNMAPPED_CH, HUB_LED_PWM_FREQ, HUB_LED_PWM_RES);

    // Attach GPIO pins to LEDC channels
    ledcAttachPin(HUB_LED_WIFI,     HUB_LED_WIFI_CH);
    ledcAttachPin(HUB_LED_DEVICE,   HUB_LED_DEVICE_CH);
    ledcAttachPin(HUB_LED_TASK,     HUB_LED_TASK_CH);
    ledcAttachPin(HUB_LED_UNMAPPED, HUB_LED_UNMAPPED_CH);

    // Load brightness from config (creates default file if missing)
    loadLedBrightnessConfig();

    // Start all OFF
    ledcWrite(HUB_LED_WIFI_CH, 0);
    ledcWrite(HUB_LED_DEVICE_CH, 0);
    ledcWrite(HUB_LED_TASK_CH, 0);
    ledcWrite(HUB_LED_UNMAPPED_CH, 0);
    
    LOG_INFO("Hub Status LEDs initialized with PWM (GPIO %d, %d, %d, %d)", 
             HUB_LED_WIFI, HUB_LED_DEVICE, HUB_LED_TASK, HUB_LED_UNMAPPED);
}

// Trigger task completion indicator (blinks for 30 seconds)
void triggerHubTaskLED() {
    hubLedTaskTriggerTime = millis();
    hubLedTaskActive = true;
    LOG_DEBUG("Hub Task LED triggered - will blink for 30s");
}

// Update hub status LEDs (call from loop or task)
void updateHubStatusLEDs() {
    uint32_t now = millis();
    
    // === LED 1: WiFi Status ===
    // Blink if not connected, solid ON if connected
    bool wifiConnected = (WiFi.status() == WL_CONNECTED);
    if (wifiConnected) {
        // Solid ON when connected
        if (!hubLedWiFiState) {
            hubLedWiFiState = true;
            hubLedWrite(HUB_LED_WIFI_CH, true);
        }
    } else {
        // Blink when not connected
        if (now - hubLedLastToggle[0] >= HUB_LED_BLINK_INTERVAL_MS) {
            hubLedLastToggle[0] = now;
            hubLedWiFiState = !hubLedWiFiState;
            hubLedWrite(HUB_LED_WIFI_CH, hubLedWiFiState);
        }
    }
    
    // === LED 2: Device Offline Status ===
    // Blink if any device offline, solid ON if all devices online
    auto& espnow = ESPNowManager::getInstance();
    std::vector<PeerStatus> peers = espnow.getPeers();
    size_t peerCount = peers.size();
    
    // Check if any peer is offline
    bool anyDeviceOffline = false;
    for (const PeerStatus& p : peers) {
        if (!p.online) {
            anyDeviceOffline = true;
            break;
        }
    }
    
    if (!anyDeviceOffline && peerCount > 0) {
        // Solid ON when all devices online
        if (!hubLedDeviceState) {
            hubLedDeviceState = true;
            hubLedWrite(HUB_LED_DEVICE_CH, true);
        }
    } else if (peerCount == 0) {
        // OFF when no devices registered at all
        if (hubLedDeviceState) {
            hubLedDeviceState = false;
            hubLedWrite(HUB_LED_DEVICE_CH, false);
        }
    } else {
        // Blink when some devices offline
        if (now - hubLedLastToggle[1] >= HUB_LED_BLINK_INTERVAL_MS) {
            hubLedLastToggle[1] = now;
            hubLedDeviceState = !hubLedDeviceState;
            hubLedWrite(HUB_LED_DEVICE_CH, hubLedDeviceState);
        }
    }
    
    // === LED 3: Scheduled Task Indicator ===
    // Blink for 30 seconds after task completes, else OFF
    if (hubLedTaskActive) {
        if (now - hubLedTaskTriggerTime >= HUB_LED_TASK_DURATION_MS) {
            // Task indication period over
            hubLedTaskActive = false;
            hubLedTaskState = false;
            hubLedWrite(HUB_LED_TASK_CH, false);
        } else {
            // Blink during task indication period
            if (now - hubLedLastToggle[2] >= HUB_LED_BLINK_INTERVAL_MS) {
                hubLedLastToggle[2] = now;
                hubLedTaskState = !hubLedTaskState;
                hubLedWrite(HUB_LED_TASK_CH, hubLedTaskState);
            }
        }
    } else {
        // OFF when no recent task
        if (hubLedTaskState) {
            hubLedTaskState = false;
            hubLedWrite(HUB_LED_TASK_CH, false);
        }
    }
    
    // === LED 4: Unmapped Device Indicator ===
    // Blink if any unmapped devices exist, else OFF
    bool hasUnmappedDevices = false;
    // Check unmapped-devices.json for any entries
    if (FS_USER.exists("/config/unmapped-devices.json")) {
        File f = FS_USER.open("/config/unmapped-devices.json", "r");
        if (f) {
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, f);
            f.close();
            if (!err && doc.is<JsonArray>()) {
                JsonArray arr = doc.as<JsonArray>();
                hasUnmappedDevices = (arr.size() > 0);
            }
        }
    }
    
    if (hasUnmappedDevices) {
        // Blink when unmapped devices exist
        if (now - hubLedLastToggle[3] >= HUB_LED_BLINK_INTERVAL_MS) {
            hubLedLastToggle[3] = now;
            hubLedUnmappedState = !hubLedUnmappedState;
            hubLedWrite(HUB_LED_UNMAPPED_CH, hubLedUnmappedState);
        }
    } else {
        // OFF when no unmapped devices
        if (hubLedUnmappedState) {
            hubLedUnmappedState = false;
            hubLedWrite(HUB_LED_UNMAPPED_CH, false);
        }
    }
}

// ESPNowManager callbacks declared here
void onAnnounceReceived(const uint8_t* mac, const AnnounceMessage& msg);
void onHeartbeatReceived(const uint8_t* mac, const HeartbeatMessage& msg);
void onStatusReceived(const uint8_t* mac, const StatusMessage& msg);
void onCommandReceived(const uint8_t* mac, const uint8_t* data, size_t len);

// Peer registration
void registerAllDevicesAsPeers();

// Web server setup
void setupWebServer();

// Notification framework setup
void setupNotifications();

// Scheduler function declarations
void rebuildNextTasks();

// Web server
AsyncWebServer server(80);

// Simple in-memory recent-missing list (keeps last N missing asset requests)
static const size_t MISSING_ASSETS_LIMIT = 64;
std::vector<String> recentMissingAssets;

void recordMissingAsset(const String &url) {
    String entry = String(millis()) + ": " + url;
    recentMissingAssets.push_back(entry);
    if (recentMissingAssets.size() > MISSING_ASSETS_LIMIT) {
        recentMissingAssets.erase(recentMissingAssets.begin());
    }
}

struct SettingsUploadContext {
    File file;
    String error;
};

// ============================================================================
// SETTINGS HELPERS
// ============================================================================

static const char* kConfigJsonFiles[] = {
    "aquariums.json",
    "config.json",
    "devices.json",
    "light-devices.json",
    "light-schedule.json",
    "unmapped-devices.json"
};

bool isAllowedConfigFile(const String& name) {
    for (const char* fileName : kConfigJsonFiles) {
        if (name == fileName) {
            return true;
        }
    }
    return false;
}

// Recursively list all files on FS_USER under a given directory
void listUserFilesRecursive(File dir, const String& prefix, JsonArray& files) {
    File entry = dir.openNextFile();
    while (entry) {
        String entryName = String(entry.name());
        // Build relative path from root
        String relativePath;
        if (prefix.length() > 0 && !prefix.equals("/")) {
            relativePath = prefix;
            if (!relativePath.endsWith("/")) relativePath += "/";
            // entry.name() on ESP32 returns just the filename, not the full path
            relativePath += entryName;
        } else {
            relativePath = "/" + entryName;
        }
        if (entry.isDirectory()) {
            File subDir = FS_USER.open(relativePath);
            if (subDir) {
                listUserFilesRecursive(subDir, relativePath, files);
                subDir.close();
            }
        } else {
            JsonObject fileObj = files.createNestedObject();
            fileObj["path"] = relativePath;
            fileObj["size"] = entry.size();
        }
        entry = dir.openNextFile();
    }
}

// ============================================================================
// SEMANTIC VERSION COMPARISON
// ============================================================================

/**
 * @brief Parse semantic version string (e.g., "1.2.3") into components
 * @param version Version string
 * @param major Output major version
 * @param minor Output minor version  
 * @param patch Output patch version
 * @return true if parsing succeeded
 */
bool parseSemanticVersion(const String& version, int& major, int& minor, int& patch) {
    major = minor = patch = 0;
    
    // Trim whitespace and 'v' prefix if present
    String v = version;
    v.trim();
    if (v.startsWith("v") || v.startsWith("V")) {
        v = v.substring(1);
    }
    
    int firstDot = v.indexOf('.');
    if (firstDot < 0) {
        // Single number version (e.g., "1")
        major = v.toInt();
        return true;
    }
    
    major = v.substring(0, firstDot).toInt();
    String rest = v.substring(firstDot + 1);
    
    int secondDot = rest.indexOf('.');
    if (secondDot < 0) {
        // Two-part version (e.g., "1.2")
        minor = rest.toInt();
        return true;
    }
    
    minor = rest.substring(0, secondDot).toInt();
    patch = rest.substring(secondDot + 1).toInt();
    return true;
}

/**
 * @brief Compare two semantic versions
 * @param version1 First version string
 * @param version2 Second version string
 * @return >0 if version1 > version2, <0 if version1 < version2, 0 if equal
 */
int compareSemanticVersions(const String& version1, const String& version2) {
    int major1, minor1, patch1;
    int major2, minor2, patch2;
    
    parseSemanticVersion(version1, major1, minor1, patch1);
    parseSemanticVersion(version2, major2, minor2, patch2);
    
    if (major1 != major2) return major1 - major2;
    if (minor1 != minor2) return minor1 - minor2;
    return patch1 - patch2;
}

/**
 * @brief Fetch remote version from URL
 * @param baseUrl Base URL (trailing slash optional)
 * @param versionOut Output version string
 * @return true if fetch succeeded
 */
bool fetchRemoteVersion(const String& baseUrl, String& versionOut) {
    String url = baseUrl;
    if (!url.endsWith("/")) url += "/";
    url += "version.txt";
    
    std::unique_ptr<WiFiClient> client;
    if (url.startsWith("https://")) {
        std::unique_ptr<WiFiClientSecure> secureClient(new WiFiClientSecure());
        secureClient->setInsecure();
        client = std::move(secureClient);
    } else {
        client = std::unique_ptr<WiFiClient>(new WiFiClient());
    }
    
    HTTPClient http;
    if (!http.begin(*client, url)) {
        Serial.printf("[OTA] Failed to connect to %s\n", url.c_str());
        return false;
    }
    
    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("[OTA] version.txt fetch failed: HTTP %d\n", httpCode);
        http.end();
        return false;
    }
    
    versionOut = http.getString();
    versionOut.trim();
    http.end();
    
    Serial.printf("[OTA] Remote version: %s\n", versionOut.c_str());
    return true;
}

/**
 * @brief Update a specific key in hub_config.txt
 * @param key The key to update
 * @param newValue The new value
 * @return true if update succeeded
 */
bool updateHubConfigValue(const String& key, const String& newValue) {
#ifdef DUAL_LITTLEFS
    // hub_config.txt is on static filesystem inside /config/
    const char* configPath = "/config/hub_config.txt";
    fs::LittleFSFS& fs = StaticFS;
#else
    const char* configPath = "/config/hub_config.txt";
    fs::LittleFSFS& fs = LittleFS;
#endif
    
    if (!fs.exists(configPath)) {
        Serial.println("[Config] hub_config.txt not found");
        return false;
    }
    
    // Read existing content
    File file = fs.open(configPath, "r");
    if (!file) {
        Serial.println("[Config] Failed to open hub_config.txt for reading");
        return false;
    }
    
    String content = file.readString();
    file.close();
    
    // Find and replace the key
    String searchKey = key + "=";
    int keyStart = content.indexOf("\n" + searchKey);
    if (keyStart < 0) {
        keyStart = content.indexOf(searchKey);
        if (keyStart != 0) {
            // Key not found at start, try to append
            Serial.printf("[Config] Key %s not found, appending\n", key.c_str());
            content += "\n" + key + "=" + newValue;
        }
    } else {
        keyStart++; // Skip the newline
    }
    
    if (keyStart >= 0) {
        int valueStart = keyStart + searchKey.length();
        int valueEnd = content.indexOf('\n', valueStart);
        if (valueEnd < 0) valueEnd = content.length();
        
        String before = content.substring(0, valueStart);
        String after = content.substring(valueEnd);
        content = before + newValue + after;
    }
    
    // Write back
    file = fs.open(configPath, "w");
    if (!file) {
        Serial.println("[Config] Failed to open hub_config.txt for writing");
        return false;
    }
    
    file.print(content);
    file.close();
    
    Serial.printf("[Config] Updated %s=%s\n", key.c_str(), newValue.c_str());
    return true;
}

// ============================================================================
// OTA UPDATE FUNCTIONS
// ============================================================================

/**
 * @brief Perform OTA update for firmware or static LittleFS
 * 
 * DUAL LITTLEFS NOTE:
 * When DUAL_LITTLEFS is enabled, LittleFS OTA only updates the static_fs partition.
 * The user_fs partition (containing JSON config files) is PRESERVED during OTA.
 * This is achieved by the partition table ordering - static_fs is the first spiffs partition.
 */
bool performOtaUpdate(const String& url, bool isLittleFs, String& errorOut) {
    if (url.length() == 0) {
        errorOut = "OTA URL not set";
        return false;
    }

#ifdef DUAL_LITTLEFS
    if (isLittleFs) {
        Serial.println("[OTA] DUAL_LITTLEFS mode: Only static_fs will be updated");
        Serial.println("[OTA] User data in user_fs will be PRESERVED!");
    }
#endif

    Serial.printf("[OTA] Starting %s update from: %s\n", isLittleFs ? "LittleFS (static_fs)" : "firmware", url.c_str());
    
    // Temporarily disable WDT for the duration of OTA
    // This is safe because OTA has its own timeout handling
    Serial.println("[OTA] Disabling task watchdog for OTA duration...");
    esp_task_wdt_deinit();

    std::unique_ptr<WiFiClient> client;
    if (url.startsWith("https://")) {
        std::unique_ptr<WiFiClientSecure> secureClient(new WiFiClientSecure());
        secureClient->setInsecure();
        client = std::move(secureClient);
    } else {
        client = std::unique_ptr<WiFiClient>(new WiFiClient());
    }

    HTTPClient http;
    http.setTimeout(120000);  // 120 second timeout for large files
    
    if (!http.begin(*client, url)) {
        errorOut = "Failed to start HTTP";
        esp_task_wdt_init(5, true);  // Re-enable WDT
        return false;
    }

    Serial.println("[OTA] Sending HTTP GET request...");
    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        errorOut = "HTTP error: " + String(httpCode);
        http.end();
        esp_task_wdt_init(5, true);  // Re-enable WDT
        return false;
    }

    int contentLength = http.getSize();
    Serial.printf("[OTA] Content length: %d bytes\n", contentLength);
    
    int updateType = isLittleFs ? U_SPIFFS : U_FLASH;

    if (!Update.begin(contentLength > 0 ? contentLength : UPDATE_SIZE_UNKNOWN, updateType)) {
        errorOut = Update.errorString();
        http.end();
        esp_task_wdt_init(5, true);  // Re-enable WDT
        return false;
    }

    // Chunked download with progress reporting
    WiFiClient* stream = http.getStreamPtr();
    uint8_t buff[4096];  // Larger buffer for faster download
    size_t written = 0;
    unsigned long lastProgressTime = millis();
    unsigned long startTime = millis();
    
    Serial.println("[OTA] Downloading and writing...");
    
    while (http.connected() && (contentLength <= 0 || written < (size_t)contentLength)) {
        size_t available = stream->available();
        if (available) {
            size_t toRead = (available > sizeof(buff)) ? sizeof(buff) : available;
            size_t bytesRead = stream->readBytes(buff, toRead);
            
            if (bytesRead > 0) {
                size_t bytesWritten = Update.write(buff, bytesRead);
                if (bytesWritten != bytesRead) {
                    errorOut = "Write failed";
                    Update.abort();
                    http.end();
                    esp_task_wdt_init(5, true);  // Re-enable WDT
                    return false;
                }
                written += bytesWritten;
                
                // Show progress every second or 100KB
                if (millis() - lastProgressTime > 1000 || written % 102400 < 4096) {
                    int progress = contentLength > 0 ? (written * 100 / contentLength) : 0;
                    Serial.printf("[OTA] Progress: %d%% (%d/%d bytes)\n", progress, written, contentLength);
                    lastProgressTime = millis();
                }
            }
        } else {
            // No data available, small yield
            delay(1);
        }
        
        // Timeout check (5 minutes max for large LittleFS)
        if (millis() - startTime > 300000) {
            errorOut = "Download timeout (5 min)";
            Update.abort();
            http.end();
            esp_task_wdt_init(5, true);  // Re-enable WDT
            return false;
        }
    }
    
    Serial.printf("[OTA] Download complete: %d bytes written in %lu ms\n", written, millis() - startTime);

    if (contentLength > 0 && written != (size_t)contentLength) {
        errorOut = "Incomplete OTA write: " + String(written) + "/" + String(contentLength);
        Update.abort();
        http.end();
        esp_task_wdt_init(5, true);  // Re-enable WDT
        return false;
    }

    if (!Update.end()) {
        errorOut = Update.errorString();
        http.end();
        esp_task_wdt_init(5, true);  // Re-enable WDT
        return false;
    }

    if (!Update.isFinished()) {
        errorOut = "OTA not finished";
        http.end();
        esp_task_wdt_init(5, true);  // Re-enable WDT
        return false;
    }

    http.end();
    Serial.println("[OTA] Update successful!");
    // Note: WDT not re-enabled here since we'll reboot immediately after
    return true;
}

// WiFiManager
WiFiManager wifiManager;

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

/**
 * @brief Generate unique command ID
 */
uint8_t generateCommandId() {
    static uint8_t commandIdCounter = 0;
    return ++commandIdCounter;  // Auto-wraps at 255
}

/**
 * @brief Create device object based on type
 * NOTE: Device subclasses don't have .cpp implementations yet,
 * so we can't instantiate them. This is a placeholder that will
 * be implemented when device .cpp files are created.
 */
Device* createDevice(const uint8_t* mac, NodeType type, const char* name) {
    // TODO: Implement when device .cpp files exist
    Serial.printf("[WARN] Device creation not yet implemented (type: %d)\n", (int)type);
    return nullptr;
}

/**
 * @brief Count devices for a specific aquarium from devices.json
 * TEMPORARY WORKAROUND: Since Device objects aren't created yet,
 * we count devices directly from the JSON file
 */
int countDevicesForAquarium(uint8_t tankId) {
    if (!FS_USER.exists("/config/devices.json")) {
        return 0;
    }
    
    File file = FS_USER.open("/config/devices.json", "r");
    if (!file) {
        return 0;
    }
    
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    
    if (error) {
        return 0;
    }
    
    JsonArray devices = doc["devices"].as<JsonArray>();
    int count = 0;
    
    for (JsonObject obj : devices) {
        if (obj["tankId"].as<uint8_t>() == tankId) {
            count++;
        }
    }
    
    return count;
}

// ============================================================================
// GENERIC DEVICE OTA HELPERS
// ============================================================================

/**
 * @brief Convert lowercase device type ID to uppercase type string for devices.json
 * @param deviceTypeId Lowercase ID (e.g., "light", "co2", "heater")
 * @return Uppercase type string (e.g., "LIGHT", "CO2", "HEATER")
 */
String deviceTypeIdToUpper(const String& deviceTypeId) {
    String typeUpper = deviceTypeId;
    typeUpper.toUpperCase();
    // Handle special cases
    if (typeUpper == "FISH_FEEDER") return "FISH_FEEDER";
    return typeUpper;
}

/**
 * @brief Get OTA URL for a device type from ota.json
 * @param deviceTypeId Lowercase ID (e.g., "light", "co2")
 * @return OTA base URL or empty string if not found
 */
String getDeviceTypeOtaUrl(const String& deviceTypeId) {
    File file = FS_STATIC.open("/config/ota.json", "r");
    if (!file) {
        Serial.println("[DeviceOTA] ota.json not found");
        return "";
    }
    
    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    
    if (error) {
        Serial.printf("[DeviceOTA] ota.json parse error: %s\n", error.c_str());
        return "";
    }
    
    JsonArray deviceTypes = doc["deviceTypes"];
    for (JsonObject dt : deviceTypes) {
        String id = dt["id"].as<String>();
        if (id == deviceTypeId) {
            return dt["otaUrl"].as<String>();
        }
    }
    
    Serial.printf("[DeviceOTA] Device type '%s' not found in ota.json\n", deviceTypeId.c_str());
    return "";
}

/**
 * @brief Check if device type is valid (exists in ota.json)
 * @param deviceTypeId Lowercase ID
 * @return true if valid
 */
bool isValidDeviceType(const String& deviceTypeId) {
    return getDeviceTypeOtaUrl(deviceTypeId).length() > 0 || deviceTypeId.length() == 0;
}

// ============================================================================
// CONFIGURATION LOADER (Using MicroCore ConfigManager)
// ============================================================================

void loadConfiguration() {
    // Set defaults first
    config.heartbeatEnabled = true;
    config.heartbeatIntervalSec = 30;
    config.hubHeartbeatIntervalSec = 120;
    config.aggressiveMemoryManagement = true;
    config.heapWarningThresholdKB = 50;
    config.psramWarningThresholdKB = 100;
    config.wifiAPName = "AquariumHub";
    config.wifiAPPassword = "aquarium123";
    config.wifiTimeoutSec = 180;
    config.mdnsHostname = "ams";
    config.espnowChannel = 6;
    config.espnowMaxPeers = 20;
    config.debugSerial = true;
    config.debugESPNOW = false;
    config.debugWebSocket = false;
    config.schedulerMinSleepSec = 30;
    config.schedulerMaxSleepSec = 300;
    config.hubFirmwareOtaUrl = "";
    config.hubLittlefsOtaUrl = "";
    config.hubFirmwareVersion = "1.0.0";
    config.hubLittlefsVersion = "1.0.0";
    config.lightNodeOtaUrl = "";
    config.hubTestMode = false;
    
    // Use MicroCore ConfigManager to load KEY=VALUE config
    // NOTE: In dual filesystem mode, hub_config.txt is in static_fs at /hub_config.txt
    // ConfigManager uses LittleFS internally, so we need the file accessible via mounted path
#ifdef DUAL_LITTLEFS
    // In dual filesystem mode, hub_config.txt lives inside /config/ on static_fs
    // ConfigManager uses default LittleFS, so we manually load for dual mode
    File cfgFile = StaticFS.open("/config/hub_config.txt", "r");
    if (!cfgFile) {
        LOG_WARN("Config file /config/hub_config.txt not found on static_fs, using defaults");
        return;
    }
    // Read and parse manually for dual filesystem mode
    while (cfgFile.available()) {
        String line = cfgFile.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line.startsWith("#")) continue;
        int sep = line.indexOf('=');
        if (sep < 0) continue;
        String key = line.substring(0, sep);
        String value = line.substring(sep + 1);
        key.trim();
        value.trim();
        // Store in ConfigManager's internal map via set()
        hubConfigManager.set(key.c_str(), value);
    }
    cfgFile.close();
#else
    if (!hubConfigManager.loadKeyValue("/config/hub_config.txt")) {
        LOG_WARN("Config file not found, using defaults");
        return;
    }
#endif
    
    LOG_INFO("Loading configuration via MicroCore ConfigManager...");
    
    // Load values with defaults using ConfigManager getters
    config.heartbeatEnabled = hubConfigManager.getBool("HEARTBEAT_ENABLED", config.heartbeatEnabled);
    config.heartbeatIntervalSec = hubConfigManager.getUInt("HEARTBEAT_INTERVAL_SEC", config.heartbeatIntervalSec);
    config.hubHeartbeatIntervalSec = hubConfigManager.getUInt("HUB_HEARTBEAT_INTERVAL_SEC", config.hubHeartbeatIntervalSec);
    config.aggressiveMemoryManagement = hubConfigManager.getBool("AGGRESSIVE_MEMORY_MANAGEMENT", config.aggressiveMemoryManagement);
    config.heapWarningThresholdKB = hubConfigManager.getUInt("HEAP_WARNING_THRESHOLD_KB", config.heapWarningThresholdKB);
    config.psramWarningThresholdKB = hubConfigManager.getUInt("PSRAM_WARNING_THRESHOLD_KB", config.psramWarningThresholdKB);
    config.wifiAPName = hubConfigManager.getString("WIFI_AP_NAME", config.wifiAPName);
    config.wifiAPPassword = hubConfigManager.getString("WIFI_AP_PASSWORD", config.wifiAPPassword);
    config.wifiTimeoutSec = hubConfigManager.getUInt("WIFI_TIMEOUT_SEC", config.wifiTimeoutSec);
    config.mdnsHostname = hubConfigManager.getString("MDNS_HOSTNAME", config.mdnsHostname);
    config.espnowChannel = hubConfigManager.getUInt("ESPNOW_CHANNEL", config.espnowChannel);
    config.espnowMaxPeers = hubConfigManager.getUInt("ESPNOW_MAX_PEERS", config.espnowMaxPeers);
    config.debugSerial = hubConfigManager.getBool("DEBUG_SERIAL", config.debugSerial);
    config.debugESPNOW = hubConfigManager.getBool("DEBUG_ESPNOW", config.debugESPNOW);
    config.debugWebSocket = hubConfigManager.getBool("DEBUG_WEBSOCKET", config.debugWebSocket);
    config.schedulerMinSleepSec = hubConfigManager.getUInt("SCHEDULER_MIN_SLEEP_SEC", config.schedulerMinSleepSec);
    config.schedulerMaxSleepSec = hubConfigManager.getUInt("SCHEDULER_MAX_SLEEP_SEC", config.schedulerMaxSleepSec);
    config.hubFirmwareOtaUrl = hubConfigManager.getString("HUB_FIRMWARE_OTA_URL", config.hubFirmwareOtaUrl);
    config.hubLittlefsOtaUrl = hubConfigManager.getString("HUB_LITTLEFS_OTA_URL", config.hubLittlefsOtaUrl);
    config.hubFirmwareVersion = hubConfigManager.getString("HUB_FIRMWARE_VERSION", config.hubFirmwareVersion);
    config.hubLittlefsVersion = hubConfigManager.getString("HUB_LITTLEFS_VERSION", config.hubLittlefsVersion);
    config.lightNodeOtaUrl = hubConfigManager.getString("LIGHT_NODE_OTA_URL", config.lightNodeOtaUrl);
    config.hubTestMode = hubConfigManager.getBool("HUB_TEST_MODE", config.hubTestMode);
    
    LOG_INFO("Configuration loaded:");
    LOG_INFO("  - Heartbeat: %s (%ds)", config.heartbeatEnabled ? "ON" : "OFF", config.heartbeatIntervalSec);
    LOG_INFO("  - Hub Heartbeat Interval: %ds", config.hubHeartbeatIntervalSec);
    LOG_INFO("  - Memory Management: %s", config.aggressiveMemoryManagement ? "AGGRESSIVE" : "NORMAL");
    LOG_INFO("  - mDNS: %s.local", config.mdnsHostname.c_str());
    LOG_INFO("  - Scheduler Sleep: min %us, max %us", config.schedulerMinSleepSec, config.schedulerMaxSleepSec);
}

// ============================================================================
// MEMORY MANAGEMENT
// ============================================================================

void printMemoryStatus() {
    uint32_t freeHeap = ESP.getFreeHeap() / 1024;  // KB
    uint32_t totalHeap = ESP.getHeapSize() / 1024;  // KB
    uint32_t freePSRAM = ESP.getFreePsram() / 1024;  // KB
    uint32_t totalPSRAM = ESP.getPsramSize() / 1024;  // KB
    
    LOG_INFO("HEAP:  %u KB free / %u KB total (%.1f%%)", 
                  freeHeap, totalHeap, 
                  totalHeap > 0 ? (freeHeap * 100.0) / totalHeap : 0.0);
    
    if (totalPSRAM > 0) {
        LOG_INFO("PSRAM: %u KB free / %u KB total (%.1f%%)", 
                      freePSRAM, totalPSRAM, 
                      (freePSRAM * 100.0) / totalPSRAM);
    } else {
        LOG_WARN("PSRAM: Not available or not enabled");
    }
    
    LOG_INFO("Uptime: %lu seconds", millis() / 1000);
    
    // Warnings
    if (freeHeap < config.heapWarningThresholdKB) {
        LOG_WARN("HEAP WARNING: Only %u KB free!", freeHeap);
    }
    
    if (totalPSRAM > 0 && freePSRAM < config.psramWarningThresholdKB) {
        LOG_WARN("PSRAM WARNING: Only %u KB free!", freePSRAM);
    }
}

void aggressiveMemoryCleanup() {
    if (!config.aggressiveMemoryManagement) {
        return;
    }
    
    // Force garbage collection
    heap_caps_check_integrity_all(true);
    
    // Log cleanup
    if (config.debugSerial) {
        LOG_DEBUG("Aggressive memory cleanup triggered");
    }
}

// ============================================================================
// LIGHT SCHEDULER TASK - Time-based light control with task persistence
// ============================================================================

// Helper: Parse MAC string to bytes
static bool parseMacAddress(const char* macStr, uint8_t* mac) {
    return sscanf(macStr, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                  &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6;
}

// Helper: Send light command via ESP-NOW using ESPNowManager
static void sendLightCommand(const uint8_t* mac, uint8_t command) {
    CommandMessage cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.header.type = MessageType::COMMAND;
    cmd.header.tankId = 1;
    cmd.header.nodeType = NodeType::HUB;
    cmd.header.timestamp = millis();
    cmd.header.sequenceNum = 0;
    cmd.commandId = millis() & 0xFF;
    cmd.commandSeqID = 0;
    cmd.finalCommand = true;
    cmd.commandData[0] = command;  // Light command: 10/11=CH1, 20/21=CH2, 30/31=CH3
    
    // Use ESPNowManager which handles peer registration - skip online check for scheduler
    bool result = ESPNowManager::getInstance().send(mac, (uint8_t*)&cmd, sizeof(cmd), false);
    Serial.printf("[SCHEDULER] Sent command %d to %02X:%02X:%02X:%02X:%02X:%02X - %s\n",
                  command, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                  result ? "OK" : "FAILED");
}

// Helper: Send feeder command via ESP-NOW (PWM value + duration)
static std::map<String, int> g_pendingFeedAcks;            // pending feed commands waiting for node ACK
static std::map<String, unsigned long> g_pendingFeedTs;   // timestamp of last pending send
static const char* FEED_COUNT_PATH = "/Feed_count.txt";

static void sendFeederCommand(const uint8_t* mac, uint16_t pwmValue, uint32_t durationMs) {
    CommandMessage cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.header.type = MessageType::COMMAND;
    cmd.header.tankId = 1;
    cmd.header.nodeType = NodeType::HUB;
    cmd.header.timestamp = millis();
    cmd.header.sequenceNum = 0;
    cmd.commandId = millis() & 0xFF;
    cmd.commandSeqID = 0;
    cmd.finalCommand = true;

    // Command format: [cmdType=0x01][pwm_low][pwm_high][dur_b0][dur_b1][dur_b2][dur_b3]
    cmd.commandData[0] = 0x01;  // CMD_FEED
    cmd.commandData[1] = pwmValue & 0xFF;         // PWM low byte
    cmd.commandData[2] = (pwmValue >> 8) & 0xFF;  // PWM high byte
    cmd.commandData[3] = durationMs & 0xFF;         // Duration byte 0
    cmd.commandData[4] = (durationMs >> 8) & 0xFF;  // Duration byte 1
    cmd.commandData[5] = (durationMs >> 16) & 0xFF; // Duration byte 2
    cmd.commandData[6] = (durationMs >> 24) & 0xFF; // Duration byte 3

    bool result = ESPNowManager::getInstance().send(mac, (uint8_t*)&cmd, sizeof(cmd), false);
    String macStr = macToString(mac);
    if (result) {
        // Track pending feed ACKs so we decrement count only after node confirms
        g_pendingFeedAcks[macStr] = g_pendingFeedAcks[macStr] + 1;
        g_pendingFeedTs[macStr] = millis();
    }

    Serial.printf("[SCHEDULER] Sent feeder command PWM=%u duration=%ums to %02X:%02X:%02X:%02X:%02X:%02X - %s\n",
                  pwmValue, durationMs, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                  result ? "OK" : "FAILED");
}

// ============================================================================
// WAVE MAKER COMMAND HELPERS
// ============================================================================

// Forward-declare debugLog (defined later near scheduler section)
static void debugLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Helper: Send wave maker PWM command via ESP-NOW (duty cycle % as float)
static void sendWaveMakerCommand(const uint8_t* mac, float dutyPercent) {
    debugLog("sendWaveMakerCommand: dutyPercent=%.2f mac=%02X:%02X:%02X:%02X:%02X:%02X",
             dutyPercent, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    CommandMessage cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.header.type = MessageType::COMMAND;
    cmd.header.tankId = 1;
    cmd.header.nodeType = NodeType::HUB;
    cmd.header.timestamp = millis();
    cmd.header.sequenceNum = 0;
    cmd.commandId = millis() & 0xFF;
    cmd.commandSeqID = 0;
    cmd.finalCommand = true;

    // Command format: [cmdType=0x01][duty_b0][duty_b1][duty_b2][duty_b3]
    cmd.commandData[0] = 0x01;  // CMD_PWM
    memcpy(&cmd.commandData[1], &dutyPercent, sizeof(float));

    // Log raw bytes being sent
    debugLog("sendWaveMakerCommand: cmdData[0]=0x%02X cmdData[1..4]=%02X %02X %02X %02X (float bytes)",
             cmd.commandData[0], cmd.commandData[1], cmd.commandData[2], cmd.commandData[3], cmd.commandData[4]);

    bool result = ESPNowManager::getInstance().send(mac, (uint8_t*)&cmd, sizeof(cmd), false);
    debugLog("sendWaveMakerCommand: send result=%s", result ? "OK" : "FAILED");
    Serial.printf("[WAVEMAKER] Sent PWM command duty=%.1f%% to %02X:%02X:%02X:%02X:%02X:%02X - %s\n",
                  dutyPercent, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                  result ? "OK" : "FAILED");
}

// Helper: Send wave maker STOP command via ESP-NOW
static void sendWaveMakerStop(const uint8_t* mac) {
    debugLog("sendWaveMakerStop: mac=%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    CommandMessage cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.header.type = MessageType::COMMAND;
    cmd.header.tankId = 1;
    cmd.header.nodeType = NodeType::HUB;
    cmd.header.timestamp = millis();
    cmd.header.sequenceNum = 0;
    cmd.commandId = millis() & 0xFF;
    cmd.commandSeqID = 0;
    cmd.finalCommand = true;

    cmd.commandData[0] = 0x00;  // CMD_STOP

    bool result = ESPNowManager::getInstance().send(mac, (uint8_t*)&cmd, sizeof(cmd), false);
    debugLog("sendWaveMakerStop: send result=%s", result ? "OK" : "FAILED");
    Serial.printf("[WAVEMAKER] Sent STOP command to %02X:%02X:%02X:%02X:%02X:%02X - %s\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                  result ? "OK" : "FAILED");
}

// Helper: Load WaveMaker config for a specific MAC from /config/WaveMaker_config.txt
// Returns default values if not found. Populates output params.
static bool loadWaveMakerConfig(const String& macStr, float& maxDuty, float& minDuty, float& defaultDuty) {
    // Set defaults
    maxDuty = 95.0f;
    minDuty = 30.0f;
    defaultDuty = 60.0f;

    if (!FS_STATIC.exists("/config/WaveMaker_config.txt")) {
        Serial.println("[WAVEMAKER] Config file not found, using defaults");
        return false;
    }

    File f = FS_STATIC.open("/config/WaveMaker_config.txt", "r");
    if (!f) return false;

    bool inSection = false;
    bool found = false;

    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line.startsWith("#")) continue;

        int eq = line.indexOf('=');
        if (eq <= 0) continue;

        String key = line.substring(0, eq);
        String value = line.substring(eq + 1);
        key.trim(); value.trim();

        if (key == "MAC") {
            // Check if this section is for our target MAC
            inSection = value.equalsIgnoreCase(macStr);
            if (inSection) found = true;
            continue;
        }

        if (inSection) {
            if (key == "MAX_DUTY_PERCENT") maxDuty = value.toFloat();
            else if (key == "MIN_DUTY_PERCENT") minDuty = value.toFloat();
            else if (key == "DEFAULT_DUTY") defaultDuty = value.toFloat();
        }
    }
    f.close();

    if (found) {
        Serial.printf("[WAVEMAKER] Config loaded for %s: min=%.1f%%, max=%.1f%%, default=%.1f%%\n",
                      macStr.c_str(), minDuty, maxDuty, defaultDuty);
    }
    return found;
}

// Helper: Read feed count for a MAC from central Feed_count.txt (returns -1 if not found)
// If the file doesn't exist, create an empty Feed_count.txt so API callers can rely on its presence.
static int getFeedCountForMac(const String& macStr) {
    if (!FS_USER.exists(FEED_COUNT_PATH)) {
        // create an empty file (user-data partition)
        File wf = FS_USER.open(FEED_COUNT_PATH, "w");
        if (wf) wf.close();
        return -1;
    }

    File f = FS_USER.open(FEED_COUNT_PATH, "r");
    if (!f) {
        // try to create an empty file if open failed for some reason
        File wf = FS_USER.open(FEED_COUNT_PATH, "w");
        if (wf) wf.close();
        return -1;
    }

    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line.startsWith("#")) continue;
        int eq = line.indexOf('=');
        if (eq <= 0) continue;
        String k = line.substring(0, eq);
        String v = line.substring(eq + 1);
        k.trim(); v.trim();
        if (k.equalsIgnoreCase(macStr)) {
            f.close();
            return v.toInt();
        }
    }
    f.close();
    return -1;
}

// Helper: Set feed count for a MAC in Feed_count.txt (creates file if missing)
static bool setFeedCountForMac(const String& macStr, int count) {
    // Normalize MAC to uppercase colon-separated form for human-readability
    String macNorm = macStr;
    macNorm.toUpperCase();

    std::vector<String> lines;
    if (FS_USER.exists(FEED_COUNT_PATH)) {
        File rf = FS_USER.open(FEED_COUNT_PATH, "r");
        if (rf) {
            while (rf.available()) {
                String line = rf.readStringUntil('\n');
                line.trim();
                lines.push_back(line);
            }
            rf.close();
        }
    }

    bool updated = false;
    for (size_t i = 0; i < lines.size(); i++) {
        String l = lines[i];
        if (l.length() == 0 || l.startsWith("#")) continue;
        int eq = l.indexOf('=');
        if (eq <= 0) continue;
        String k = l.substring(0, eq);
        k.trim();
        if (k.equalsIgnoreCase(macNorm)) {
            lines[i] = macNorm + String("=") + String(count);
            updated = true;
            break;
        }
    }
    if (!updated) {
        lines.push_back(macNorm + String("=") + String(count));
    }

    File wf = FS_USER.open(FEED_COUNT_PATH, "w");
    if (!wf) {
        Serial.printf("[FEED-COUNT] Failed to open %s for write\n", FEED_COUNT_PATH);
        return false;
    }
    for (size_t i = 0; i < lines.size(); i++) {
        wf.println(lines[i]);
    }
    wf.close();

    Serial.printf("[FEED-COUNT] Saved %s=%d to %s\n", macNorm.c_str(), count, FEED_COUNT_PATH);
    return true;
}

// Helper: Decrement feed count by one after successful ACK; returns new count or -1 on error/no-change
static int decrementFeedCountForMac(const String& macStr) {
    int cur = getFeedCountForMac(macStr);
    if (cur <= 0) return -1; // nothing to decrement
    int next = cur - 1;
    if (!setFeedCountForMac(macStr, next)) return -1;
    return next;
}

// Structure to track last command sent per channel to avoid duplicate sends
struct ChannelState {
    bool ch1_on;
    bool ch2_on;
    bool ch3_on;
    int lastCheckMinute;
};
static std::map<String, ChannelState> g_channelStates;

// Structure to track wave maker running state per MAC
struct WaveMakerState {
    bool running;
    float dutyPercent;
};
static std::map<String, WaveMakerState> g_waveMakerStates;

// Maintenance mode device state persistence
static const char* MAINTENANCE_STATES_FILE = "/config/maintenance-states.json";

// ============================================================================
// SCHEDULER DEBUG LOG — writes to FS_USER so it appears in download page
// ============================================================================
static const char* SCHEDULER_DEBUG_LOG = "/config/scheduler-debug.log";
static const size_t SCHEDULER_LOG_MAX_SIZE = 64 * 1024;  // 64 KB max, then truncate

static void debugLog(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len <= 0) return;

    // Get timestamp string
    struct tm timeinfo;
    char timeBuf[32] = "??:??:??";
    if (getLocalTime(&timeinfo)) {
        strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    }

    // Check file size, truncate if too large
    if (FS_USER.exists(SCHEDULER_DEBUG_LOG)) {
        File check = FS_USER.open(SCHEDULER_DEBUG_LOG, "r");
        if (check) {
            size_t sz = check.size();
            check.close();
            if (sz > SCHEDULER_LOG_MAX_SIZE) {
                FS_USER.remove(SCHEDULER_DEBUG_LOG);
            }
        }
    }

    File f = FS_USER.open(SCHEDULER_DEBUG_LOG, "a");
    if (f) {
        f.printf("[%s] %s\n", timeBuf, buf);
        f.close();
    }
    // Also print to serial
    Serial.printf("[DBGLOG][%s] %s\n", timeBuf, buf);
}

// Structure for next-task.json persistence
enum class TaskType : uint8_t {
    LIGHT = 0,
    FEEDER = 1,
    WAVEMAKER = 2
};

struct NextTask {
    String mac;
    String scheduleId;       // Unique identifier for this schedule entry
    TaskType taskType;       // LIGHT, FEEDER, or WAVEMAKER
    int channel;             // For LIGHT: 1, 2, or 3
    bool actionOn;           // For LIGHT: true = turn ON, false = turn OFF
    time_t scheduledTime;    // Unix timestamp
    String period;           // For LIGHT: "morning" or "evening"
    // For FEEDER:
    uint16_t pwmValue;       // Servo PWM value (duty cycle)
    uint32_t durationMs;     // Pulse duration in ms
    // For WAVEMAKER:
    float dutyPercent;       // Wave maker duty cycle percentage
};

static const char* NEXT_TASK_FILE = "/config/schedule/next-task.json";

// Helper: Generate a schedule ID from components
static String generateScheduleId(const char* mac, const char* period, int channel = 0, int feedIndex = 0, TaskType type = TaskType::LIGHT) {
    // Format: MAC_PERIOD_CHANNEL for lights, MAC_FEEDn for feeders, MAC_WMn for wavemakers
    String id = String(mac);
    id.replace(":", "");  // Remove colons for cleaner ID
    id.toUpperCase();
    
    if (type == TaskType::FEEDER) {
        id += "_FEED";
        id += String(feedIndex + 1);
    } else if (type == TaskType::WAVEMAKER) {
        id += "_WM";
        id += String(feedIndex + 1);  // reuse feedIndex as time index
    } else {
        id += "_";
        id += String(period);
        id += "_CH";
        id += String(channel);
    }
    return id;
}

// Helper: Get current Unix timestamp
static time_t getCurrentUnixTime() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        return 0;
    }
    return mktime(&timeinfo);
}

// Helper: Convert minutes-of-day to today's Unix timestamp (or tomorrow if past)
static time_t minutesToUnixTime(int minutes, bool forceToday = false) {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        return 0;
    }
    
    int currentMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    
    // Reset to midnight
    timeinfo.tm_hour = minutes / 60;
    timeinfo.tm_min = minutes % 60;
    timeinfo.tm_sec = 0;
    
    time_t result = mktime(&timeinfo);
    
    // If the time has passed today, schedule for tomorrow (unless forceToday)
    if (!forceToday && minutes <= currentMinutes) {
        result += 24 * 60 * 60;  // Add 24 hours
    }
    
    return result;
}

// Helper: Calculate all upcoming tasks from a light device schedule
static void calculateNextTasks(const char* macStr, JsonObject schedule, std::vector<NextTask>& tasks) {
    const char* periods[] = {"morning", "evening"};
    
    for (const char* periodName : periods) {
        if (!schedule.containsKey(periodName)) continue;
        
        JsonObject period = schedule[periodName].as<JsonObject>();
        
        for (int ch = 1; ch <= 3; ch++) {
            char channelKey[10];
            snprintf(channelKey, sizeof(channelKey), "channel%d", ch);
            
            if (!period.containsKey(channelKey)) continue;
            
            JsonObject channel = period[channelKey].as<JsonObject>();
            int startMinutes = channel["start"]["hour"].as<int>() * 60 + channel["start"]["minute"].as<int>();
            int offMinutes = channel["offTime"]["hour"].as<int>() * 60 + channel["offTime"]["minute"].as<int>();
            
            // Generate scheduleId - same for ON and OFF of same period/channel
            String schedId = generateScheduleId(macStr, periodName, ch, 0, TaskType::LIGHT);
            
            // Create ON task
            NextTask onTask;
            onTask.mac = macStr;
            onTask.scheduleId = schedId;
            onTask.taskType = TaskType::LIGHT;
            onTask.channel = ch;
            onTask.actionOn = true;
            onTask.scheduledTime = minutesToUnixTime(startMinutes);
            onTask.period = periodName;
            onTask.pwmValue = 0;
            onTask.durationMs = 0;
            tasks.push_back(onTask);
            
            // Create OFF task (same scheduleId as ON)
            NextTask offTask;
            offTask.mac = macStr;
            offTask.scheduleId = schedId;
            offTask.taskType = TaskType::LIGHT;
            offTask.channel = ch;
            offTask.actionOn = false;
            offTask.scheduledTime = minutesToUnixTime(offMinutes);
            offTask.period = periodName;
            offTask.pwmValue = 0;
            offTask.durationMs = 0;
            tasks.push_back(offTask);
        }
    }
}

// Helper: Calculate all upcoming tasks from a feeder device schedule
static void calculateFeederNextTasks(const char* macStr, JsonObject schedule, 
                                      uint16_t pwmValue, uint32_t durationMs,
                                      std::vector<NextTask>& tasks) {
    // Get the days configuration
    JsonObject days = schedule["days"].as<JsonObject>();
    if (days.isNull()) return;
    
    // Get current time info
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return;
    
    // Day of week: 0=Sunday, 1=Monday, ..., 6=Saturday
    const char* dayNames[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    int todayDow = timeinfo.tm_wday;
    
    // Check if today is enabled
    bool todayEnabled = days[dayNames[todayDow]] | false;
    
    // Get feeding times
    JsonArray feedingTimes = schedule["feedingTimes"].as<JsonArray>();
    if (feedingTimes.isNull()) return;
    
    int currentMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    
    // Iterate through feeding times
    int timeIndex = 0;
    for (JsonObject ft : feedingTimes) {
        bool enabled = ft["enabled"] | false;
        if (!enabled) {
            timeIndex++;
            continue;
        }
        
        int hour = ft["hour"] | 8;
        int minute = ft["minute"] | 0;
        String ampm = ft["ampm"] | "AM";
        
        // Convert to 24-hour format
        if (ampm == "PM" && hour != 12) {
            hour += 12;
        } else if (ampm == "AM" && hour == 12) {
            hour = 0;
        }
        
        int feedMinutes = hour * 60 + minute;
        
        // Generate scheduleId for this feeding time slot
        String schedId = generateScheduleId(macStr, "", 0, timeIndex, TaskType::FEEDER);
        
        // Create task
        NextTask task;
        task.mac = macStr;
        task.scheduleId = schedId;
        task.taskType = TaskType::FEEDER;
        task.channel = 0;  // Not used for feeder
        task.actionOn = true;  // Always "on" action for feeding
        task.period = String("feed") + String(timeIndex + 1);  // "feed1", "feed2", etc.
        task.pwmValue = pwmValue;
        task.durationMs = durationMs;
        
        // Determine scheduled time
        if (todayEnabled && feedMinutes > currentMinutes) {
            // Today, future time
            task.scheduledTime = minutesToUnixTime(feedMinutes, true);
        } else {
            // Find next enabled day
            for (int offset = 1; offset <= 7; offset++) {
                int nextDow = (todayDow + offset) % 7;
                if (days[dayNames[nextDow]] | false) {
                    // This day is enabled
                    task.scheduledTime = minutesToUnixTime(feedMinutes, true);
                    task.scheduledTime += offset * 24 * 60 * 60;  // Add days
                    break;
                }
            }
        }
        
        if (task.scheduledTime > 0) {
            tasks.push_back(task);
        }
        
        timeIndex++;
    }
}

// Helper: Calculate all upcoming tasks from a wavemaker device schedule
static void calculateWaveMakerNextTasks(const char* macStr, JsonObject schedule,
                                         std::vector<NextTask>& tasks) {
    // Get current time info
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return;

    const char* dayNames[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    int todayDow = timeinfo.tm_wday;

    // Get the days configuration
    // WaveMaker UI has no day picker — default ALL days to enabled when missing
    JsonObject days = schedule["days"].as<JsonObject>();
    bool hasDaysConfig = !days.isNull();

    // Helper: check if a day-of-week is enabled (defaults to true for wavemaker)
    auto isDayEnabled = [&](int dow) -> bool {
        if (!hasDaysConfig) return true;              // No days object → all enabled
        if (!days.containsKey(dayNames[dow])) return true; // Key missing → enabled
        return days[dayNames[dow]].as<bool>();         // Explicit bool read
    };

    bool todayEnabled = isDayEnabled(todayDow);

    JsonArray entries = schedule["entries"].as<JsonArray>();
    if (entries.isNull()) return;

    int currentMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;

    Serial.printf("[SCHEDULER] WM calc for %s: today=%s(dow=%d) enabled=%s curMin=%d hasDays=%s\n",
                  macStr, dayNames[todayDow], todayDow, todayEnabled ? "Y" : "N",
                  currentMinutes, hasDaysConfig ? "Y" : "N");

    int timeIndex = 0;
    for (JsonObject entry : entries) {
        bool enabled = entry["enabled"] | true;
        if (!enabled) {
            timeIndex++;
            continue;
        }

        int hour = entry["hour"] | 8;
        int minute = entry["minute"] | 0;
        String ampm = entry["ampm"] | "AM";
        String action = entry["action"] | "speed_change";
        float dutyPct = entry["dutyPercent"] | 0.0f;

        // Convert to 24-hour format
        if (ampm == "PM" && hour != 12) hour += 12;
        else if (ampm == "AM" && hour == 12) hour = 0;

        int wmMinutes = hour * 60 + minute;

        String schedId = generateScheduleId(macStr, "", 0, timeIndex, TaskType::WAVEMAKER);

        NextTask task;
        task.mac = macStr;
        task.scheduleId = schedId;
        task.taskType = TaskType::WAVEMAKER;
        task.channel = 0;
        task.actionOn = (action != "stop");
        task.period = String("wm") + String(timeIndex + 1);
        task.pwmValue = 0;
        task.durationMs = 0;
        task.dutyPercent = dutyPct;

        if (todayEnabled && wmMinutes > currentMinutes) {
            // Today, future time — schedule for today
            task.scheduledTime = minutesToUnixTime(wmMinutes, true);
            Serial.printf("[SCHEDULER] WM entry%d: %02d:%02d -> TODAY at %ld\n",
                          timeIndex, hour, minute, (long)task.scheduledTime);
        } else {
            // Find next enabled day
            for (int offset = 1; offset <= 7; offset++) {
                int nextDow = (todayDow + offset) % 7;
                if (isDayEnabled(nextDow)) {
                    task.scheduledTime = minutesToUnixTime(wmMinutes, true);
                    task.scheduledTime += offset * 24 * 60 * 60;
                    Serial.printf("[SCHEDULER] WM entry%d: %02d:%02d -> +%d days at %ld (todayEn=%d wmMin=%d curMin=%d)\n",
                                  timeIndex, hour, minute, offset, (long)task.scheduledTime,
                                  todayEnabled, wmMinutes, currentMinutes);
                    break;
                }
            }
        }

        if (task.scheduledTime > 0) {
            tasks.push_back(task);
        }

        timeIndex++;
    }
}

// Helper: Find the single next task (soonest) from all tasks
static bool findNextTask(const std::vector<NextTask>& tasks, NextTask& next) {
    if (tasks.empty()) return false;
    
    time_t now = getCurrentUnixTime();
    time_t soonest = LONG_MAX;
    int soonestIdx = -1;
    
    for (size_t i = 0; i < tasks.size(); i++) {
        if (tasks[i].scheduledTime > now && tasks[i].scheduledTime < soonest) {
            soonest = tasks[i].scheduledTime;
            soonestIdx = i;
        }
    }
    
    if (soonestIdx >= 0) {
        next = tasks[soonestIdx];
        return true;
    }
    
    return false;
}

// Save next-task.json with all upcoming tasks
static void saveNextTasks(const std::vector<NextTask>& tasks) {
    DynamicJsonDocument doc(16384);
    JsonArray arr = doc.createNestedArray("tasks");
    
    for (const NextTask& t : tasks) {
        JsonObject obj = arr.createNestedObject();
        obj["mac"] = t.mac;
        obj["scheduleId"] = t.scheduleId;
        obj["taskType"] = (int)t.taskType;  // 0=LIGHT, 1=FEEDER, 2=WAVEMAKER
        obj["channel"] = t.channel;
        obj["actionOn"] = t.actionOn;
        obj["scheduledTime"] = (long)t.scheduledTime;
        obj["period"] = t.period;
        // Include feeder-specific fields
        if (t.taskType == TaskType::FEEDER) {
            obj["pwmValue"] = t.pwmValue;
            obj["durationMs"] = t.durationMs;
        }
        // Include wavemaker-specific fields
        if (t.taskType == TaskType::WAVEMAKER) {
            obj["dutyPercent"] = t.dutyPercent;
        }
    }
    
    doc["updatedAt"] = (long)getCurrentUnixTime();
    
    if (doc.overflowed()) {
        Serial.printf("[SCHEDULER] WARNING: next-task doc overflowed! %d tasks, %u bytes used\n",
                      tasks.size(), (unsigned)doc.memoryUsage());
    }
    
    File file = FS_USER.open(NEXT_TASK_FILE, "w");
    if (file) {
        serializeJson(doc, file);
        file.close();
        Serial.printf("[SCHEDULER] Saved %d tasks to next-task.json (%u bytes used)\n",
                      tasks.size(), (unsigned)doc.memoryUsage());
    } else {
        Serial.println("[SCHEDULER] Failed to write next-task.json");
    }
}

// Load next-task.json
static bool loadNextTasks(std::vector<NextTask>& tasks) {
    tasks.clear();
    
    File file = FS_USER.open(NEXT_TASK_FILE, "r");
    if (!file) {
        Serial.println("[SCHEDULER] next-task.json not found");
        return false;
    }
    
    DynamicJsonDocument doc(16384);
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    
    if (error) {
        Serial.printf("[SCHEDULER] Failed to parse next-task.json: %s\n", error.c_str());
        return false;
    }
    
    JsonArray arr = doc["tasks"].as<JsonArray>();
    debugLog("--- loadNextTasks: parsing %d entries from JSON ---", arr.size());
    int idx = 0;
    for (JsonObject obj : arr) {
        NextTask t;
        t.mac = obj["mac"].as<String>();
        t.scheduleId = obj["scheduleId"].as<String>();

        // Log raw JSON values BEFORE conversion
        int rawTaskType = obj["taskType"] | -999;
        bool rawActionOn = obj["actionOn"] | false;
        long rawSchedTime = obj["scheduledTime"] | 0;
        float rawDutyPct = obj["dutyPercent"] | -1.0f;

        t.taskType = (TaskType)(obj["taskType"] | 0);  // Default to LIGHT
        t.channel = obj["channel"] | 1;
        t.actionOn = obj["actionOn"] | true;
        t.scheduledTime = obj["scheduledTime"] | 0;
        t.period = obj["period"].as<String>();
        t.pwmValue = obj["pwmValue"] | 0;
        t.durationMs = obj["durationMs"] | 0;
        t.dutyPercent = obj["dutyPercent"] | 0.0f;

        debugLog("  task[%d] mac=%s rawTaskType=%d taskType=%d actionOn=%d(raw=%d) schedTime=%ld dutyPct=%.2f(raw=%.2f) period=%s",
                 idx, t.mac.c_str(), rawTaskType, (int)t.taskType, t.actionOn, rawActionOn,
                 (long)t.scheduledTime, t.dutyPercent, rawDutyPct, t.period.c_str());

        tasks.push_back(t);
        idx++;
    }
    
    debugLog("--- loadNextTasks: loaded %d tasks total ---", tasks.size());
    Serial.printf("[SCHEDULER] Loaded %d tasks from next-task.json\n", tasks.size());
    return true;
}

// Delete tasks by scheduleId - removes all tasks with matching scheduleId
static bool deleteTasksByScheduleId(const String& scheduleId) {
    std::vector<NextTask> tasks;
    if (!loadNextTasks(tasks)) {
        return false;
    }

    std::vector<NextTask> remaining;
    remaining.reserve(tasks.size());
    bool removed = false;

    for (const NextTask& t : tasks) {
        if (t.scheduleId == scheduleId) {
            removed = true;
            Serial.printf("[SCHEDULER] Deleting task with scheduleId: %s\n", scheduleId.c_str());
            continue;
        }
        remaining.push_back(t);
    }

    if (!removed) {
        return false;
    }

    saveNextTasks(remaining);
    return true;
}

// Delete tasks by multiple scheduleIds
static int deleteTasksByScheduleIds(const std::vector<String>& scheduleIds) {
    std::vector<NextTask> tasks;
    if (!loadNextTasks(tasks)) {
        return 0;
    }

    std::vector<NextTask> remaining;
    remaining.reserve(tasks.size());
    int removedCount = 0;

    for (const NextTask& t : tasks) {
        bool shouldRemove = false;
        for (const String& id : scheduleIds) {
            if (t.scheduleId == id) {
                shouldRemove = true;
                break;
            }
        }
        
        if (shouldRemove) {
            removedCount++;
            Serial.printf("[SCHEDULER] Deleting task with scheduleId: %s\n", t.scheduleId.c_str());
            continue;
        }
        remaining.push_back(t);
    }

    if (removedCount > 0) {
        saveNextTasks(remaining);
    }
    return removedCount;
}

static bool deleteNextTaskEntry(const NextTask& target) {
    std::vector<NextTask> tasks;
    if (!loadNextTasks(tasks)) {
        return false;
    }

    std::vector<NextTask> remaining;
    remaining.reserve(tasks.size());
    bool removed = false;

    for (const NextTask& t : tasks) {
        bool match = t.mac.equalsIgnoreCase(target.mac) &&
                     t.taskType == target.taskType &&
                     t.channel == target.channel &&
                     t.actionOn == target.actionOn &&
                     t.scheduledTime == target.scheduledTime &&
                     t.period == target.period &&
                     t.pwmValue == target.pwmValue &&
                     t.durationMs == target.durationMs;

        if (match && !removed) {
            removed = true;
            continue;
        }

        remaining.push_back(t);
    }

    if (!removed) {
        return false;
    }

    saveNextTasks(remaining);
    return true;
}
// Rebuild next-task.json from light-schedule.json and feeder-schedule.json
void rebuildNextTasks() {
    Serial.println("[SCHEDULER] Rebuilding next-task.json from schedules...");
    
    // === Load valid (mapped) device MACs from devices.json ===
    // Any device not in devices.json is considered deleted or unmapped
    std::vector<String> validMacs;
    {
        File devFile = FS_USER.open("/config/devices.json", "r");
        if (devFile) {
            DynamicJsonDocument devDoc(8192);
            DeserializationError devErr = deserializeJson(devDoc, devFile);
            devFile.close();
            if (!devErr) {
                JsonArray devices = devDoc["devices"].as<JsonArray>();
                for (JsonObject dev : devices) {
                    String mac = dev["mac"].as<String>();
                    mac.toUpperCase();
                    validMacs.push_back(mac);
                }
            }
        }
        Serial.printf("[SCHEDULER] Valid mapped devices: %d\n", validMacs.size());
    }
    
    std::vector<NextTask> allTasks;
    
    // === Process light schedules ===
    File file = FS_USER.open("/config/schedule/light-schedule.json", "r");
    if (file) {
        DynamicJsonDocument doc(4096);
        DeserializationError error = deserializeJson(doc, file);
        file.close();
        
        if (!error) {
            JsonArray schedules = doc["schedules"].as<JsonArray>();
            for (JsonObject sched : schedules) {
                const char* macStr = sched["mac"];
                if (!macStr) continue;
                
                // Skip if device is deleted/unmapped
                String macUpper = String(macStr);
                macUpper.toUpperCase();
                bool found = false;
                for (const String& vm : validMacs) {
                    if (vm == macUpper) { found = true; break; }
                }
                if (!found) {
                    Serial.printf("[SCHEDULER] Skipping light schedule for deleted/unmapped device: %s\n", macStr);
                    continue;
                }
                
                JsonObject schedule = sched["schedule"];
                calculateNextTasks(macStr, schedule, allTasks);
            }
            Serial.printf("[SCHEDULER] Processed light schedules, %d tasks so far\n", allTasks.size());
        } else {
            Serial.printf("[SCHEDULER] Failed to parse light-schedule.json: %s\n", error.c_str());
        }
    } else {
        Serial.println("[SCHEDULER] No light-schedule.json found");
    }
    
    // === Process feeder schedules ===
    file = FS_USER.open("/config/schedule/feeder-schedule.json", "r");
    if (file) {
        DynamicJsonDocument schedDoc(4096);
        DeserializationError error = deserializeJson(schedDoc, file);
        file.close();
        
        if (!error) {
            // Load calibration data to get PWM values for each feeder
            DynamicJsonDocument calDoc(2048);
            File calFile = FS_USER.open("/config/feeder-calibration.json", "r");
            if (calFile) {
                deserializeJson(calDoc, calFile);
                calFile.close();
            }
            
            JsonArray schedules = schedDoc["schedules"].as<JsonArray>();
            for (JsonObject sched : schedules) {
                const char* macStr = sched["mac"];
                if (!macStr) continue;
                
                // Skip if device is deleted/unmapped
                String macUpper = String(macStr);
                macUpper.toUpperCase();
                bool found = false;
                for (const String& vm : validMacs) {
                    if (vm == macUpper) { found = true; break; }
                }
                if (!found) {
                    Serial.printf("[SCHEDULER] Skipping feeder schedule for deleted/unmapped device: %s\n", macStr);
                    continue;
                }
                
                JsonObject schedule = sched["schedule"];
                
                // Look up calibration for this MAC
                uint16_t pwmValue = 72;  // Default duty cycle
                uint32_t durationMs = 160;  // Default pulse duration
                
                JsonArray calibrations = calDoc["calibrations"].as<JsonArray>();
                for (JsonObject cal : calibrations) {
                    String calMac = cal["mac"] | "";
                    if (calMac.equalsIgnoreCase(macStr)) {
                        JsonObject calibration = cal["calibration"];
                        pwmValue = calibration["dutyCycle"] | 72;
                        durationMs = calibration["pulseDuration"] | 160;
                        break;
                    }
                }
                
                calculateFeederNextTasks(macStr, schedule, pwmValue, durationMs, allTasks);
            }
            Serial.printf("[SCHEDULER] Processed feeder schedules, %d total tasks\n", allTasks.size());
        } else {
            Serial.printf("[SCHEDULER] Failed to parse feeder-schedule.json: %s\n", error.c_str());
        }
    } else {
        Serial.println("[SCHEDULER] No feeder-schedule.json found");
    }
    
    // === Process wavemaker schedules ===
    file = FS_USER.open("/config/schedule/wm-schedule.json", "r");
    if (file) {
        DynamicJsonDocument wmDoc(4096);
        DeserializationError error = deserializeJson(wmDoc, file);
        file.close();
        
        if (!error) {
            JsonArray schedules = wmDoc["schedules"].as<JsonArray>();
            for (JsonObject sched : schedules) {
                const char* macStr = sched["mac"];
                if (!macStr) continue;
                
                // Skip if device is deleted/unmapped
                String macUpper = String(macStr);
                macUpper.toUpperCase();
                bool found = false;
                for (const String& vm : validMacs) {
                    if (vm == macUpper) { found = true; break; }
                }
                if (!found) {
                    Serial.printf("[SCHEDULER] Skipping wm schedule for deleted/unmapped device: %s\n", macStr);
                    continue;
                }
                
                JsonObject schedule = sched["schedule"];
                calculateWaveMakerNextTasks(macStr, schedule, allTasks);
            }
            Serial.printf("[SCHEDULER] Processed wavemaker schedules, %d total tasks\n", allTasks.size());
        } else {
            Serial.printf("[SCHEDULER] Failed to parse wm-schedule.json: %s\n", error.c_str());
        }
    } else {
        Serial.println("[SCHEDULER] No wm-schedule.json found");
    }
    
    saveNextTasks(allTasks);
}

// Helper: Check if a device MAC belongs to an aquarium in maintenance mode
static bool isDeviceInMaintenanceMode(const String& macStr) {
    // Look up the device's tankId from devices.json
    File devFile = FS_USER.open("/config/devices.json", "r");
    if (!devFile) return false;
    
    JsonDocument devDoc;
    DeserializationError err = deserializeJson(devDoc, devFile);
    devFile.close();
    if (err) return false;
    
    uint8_t tankId = 0;
    String macUpper = macStr;
    macUpper.toUpperCase();
    
    JsonArray devices = devDoc["devices"].as<JsonArray>();
    for (JsonObject dev : devices) {
        String devMac = dev["mac"].as<String>();
        devMac.toUpperCase();
        if (devMac == macUpper) {
            tankId = dev["tankId"].as<uint8_t>();
            break;
        }
    }
    
    if (tankId == 0) return false;
    
    // Check if that aquarium has maintenance mode enabled
    Aquarium* aquarium = AquariumManager::getInstance().getAquarium(tankId);
    if (aquarium && aquarium->isMaintenanceMode()) {
        return true;
    }
    
    return false;
}

// Helper: Lookup device name and aquarium name from a MAC string (using devices.json + aquariums.json)
static void lookupDeviceAndAquariumNames(const String& macStr, String& deviceName, String& aquariumName, String& deviceType) {
    deviceName = macStr;   // Fallback
    aquariumName = "Unknown";
    deviceType = "Unknown";
    
    File devFile = FS_USER.open("/config/devices.json", "r");
    if (!devFile) return;
    
    DynamicJsonDocument devDoc(8192);
    if (deserializeJson(devDoc, devFile)) { devFile.close(); return; }
    devFile.close();
    
    uint8_t tankId = 0;
    JsonArray devices = devDoc["devices"].as<JsonArray>();
    for (JsonObject d : devices) {
        String dm = d["mac"].as<String>();
        dm.toUpperCase();
        String mu = macStr;
        mu.toUpperCase();
        if (dm == mu) {
            deviceName = d["name"].as<String>();
            deviceType = d["type"].as<String>();
            tankId = d["tankId"] | 0;
            break;
        }
    }
    
    if (tankId == 0) return;
    
    Aquarium* aq = AquariumManager::getInstance().getAquarium(tankId);
    if (aq) {
        aquariumName = aq->getName();
    }
}

// ============================================================================
// ACTIVITY LOG  — append-only ring stored in /config/activity-log.json
// Keeps at most 7 days of entries.  Called from scheduled + ad-hoc paths.
// ============================================================================
#define ACTIVITY_LOG_PATH "/config/activity-log.json"
#define ACTIVITY_LOG_MAX_AGE_S (7 * 24 * 3600)   // 7 days in seconds
#define ACTIVITY_LOG_MAX_ENTRIES 200               // hard cap

/**
 * Append one activity entry and prune anything older than 7 days.
 *
 * @param source    "scheduled" or "adhoc"
 * @param category  e.g. "light", "feeder", "wavemaker"
 * @param action    human-readable action, e.g. "CH1 ON", "Feed (PWM 72)"
 * @param device    device name or MAC
 * @param aquarium  aquarium name (may be "Unknown")
 */
static void appendActivityLog(const char* source,
                              const char* category,
                              const char* action,
                              const char* device,
                              const char* aquarium)
{
    time_t now = getCurrentUnixTime();
    if (now == 0) return;  // NTP not synced yet

    // --- Read existing log -------------------------------------------------
    JsonDocument doc;
    File f = FS_USER.open(ACTIVITY_LOG_PATH, "r");
    if (f) {
        deserializeJson(doc, f);
        f.close();
    }
    JsonArray entries = doc["entries"].is<JsonArray>()
                            ? doc["entries"].as<JsonArray>()
                            : doc["entries"].to<JsonArray>();

    // --- Prune entries older than 7 days -----------------------------------
    time_t cutoff = now - ACTIVITY_LOG_MAX_AGE_S;
    size_t i = 0;
    while (i < entries.size()) {
        time_t ts = entries[i]["ts"] | 0;
        if (ts < cutoff) {
            entries.remove(i);
        } else {
            i++;
        }
    }

    // --- Hard-cap to keep flash usage bounded ------------------------------
    while (entries.size() >= ACTIVITY_LOG_MAX_ENTRIES) {
        entries.remove(0);   // remove oldest
    }

    // --- Append new entry --------------------------------------------------
    JsonObject entry = entries.add<JsonObject>();
    entry["ts"]       = (long)now;
    entry["src"]      = source;
    entry["cat"]      = category;
    entry["action"]   = action;
    entry["device"]   = device;
    entry["aquarium"] = aquarium;

    // --- Write back --------------------------------------------------------
    File out = FS_USER.open(ACTIVITY_LOG_PATH, "w");
    if (out) {
        serializeJson(doc, out);
        out.close();
    }

    Serial.printf("[ACTIVITY] %s | %s | %s | %s | %s\n",
                  source, category, action, device, aquarium);
}

// Execute a scheduled task
static bool executeTask(const NextTask& task) {
    debugLog(">>> executeTask ENTER: mac=%s taskType=%d actionOn=%d dutyPercent=%.2f channel=%d period=%s schedTime=%ld",
             task.mac.c_str(), (int)task.taskType, task.actionOn, task.dutyPercent,
             task.channel, task.period.c_str(), (long)task.scheduledTime);

    // Check maintenance mode before executing
    if (isDeviceInMaintenanceMode(task.mac)) {
        debugLog(">>> executeTask: MAINTENANCE MODE for %s — returning false", task.mac.c_str());
        Serial.printf("[SCHEDULER] Skipping task for %s (maintenance mode active)\n", task.mac.c_str());
        return false;
    }
    debugLog(">>> executeTask: maintenance check PASSED for %s", task.mac.c_str());
    
    uint8_t mac[6];
    if (!parseMacAddress(task.mac.c_str(), mac)) {
        debugLog(">>> executeTask: parseMacAddress FAILED for '%s' — returning false", task.mac.c_str());
        Serial.printf("[SCHEDULER] Invalid MAC in task: %s\n", task.mac.c_str());
        return false;
    }
    debugLog(">>> executeTask: parseMacAddress OK -> %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    // Check if node is online
    bool isOnline = ESPNowManager::getInstance().isPeerOnline(mac);
    debugLog(">>> executeTask: isPeerOnline(%s) = %s", task.mac.c_str(), isOnline ? "TRUE" : "FALSE");
    if (!isOnline) {
        debugLog(">>> executeTask: node OFFLINE — returning false");
        Serial.printf("[SCHEDULER] Node %s is OFFLINE, task deferred\n", task.mac.c_str());
        return false;  // Will retry later
    }
    
    // Trigger Hub Task LED to blink for 30 seconds
    triggerHubTaskLED();
    debugLog(">>> executeTask: checking taskType=%d (LIGHT=0, FEEDER=1, WAVEMAKER=2)", (int)task.taskType);
    
    if (task.taskType == TaskType::FEEDER) {
        // Execute feeder task
        Serial.printf("[SCHEDULER] Executing FEEDER task: %s PWM=%u duration=%ums\n",
                      task.mac.c_str(), task.pwmValue, task.durationMs);
        sendFeederCommand(mac, task.pwmValue, task.durationMs);
        
        // Notify: scheduled task executed
        String devName, aqName, devType;
        lookupDeviceAndAquariumNames(task.mac, devName, aqName, devType);
        notifier.emitf("scheduler.task", NTFY_MSG_TASK_EXECUTED,
                        aqName.c_str(), devName.c_str(), devType.c_str(), "Feed");
        char feedDesc[64];
        snprintf(feedDesc, sizeof(feedDesc), "Feed (PWM %u, %ums)", task.pwmValue, task.durationMs);
        appendActivityLog("scheduled", "feeder", feedDesc, devName.c_str(), aqName.c_str());
        return true;
    } else if (task.taskType == TaskType::WAVEMAKER) {
        // Execute wavemaker task
        debugLog(">>> executeTask: ENTERED WAVEMAKER branch for %s", task.mac.c_str());
        String wmKey = task.mac;
        wmKey.toUpperCase();
        String actionDesc;
        if (task.actionOn) {
            debugLog(">>> executeTask: WAVEMAKER ON — calling sendWaveMakerCommand(duty=%.2f)", task.dutyPercent);
            Serial.printf("[SCHEDULER] Executing WAVEMAKER task: %s duty=%.1f%%\n",
                          task.mac.c_str(), task.dutyPercent);
            sendWaveMakerCommand(mac, task.dutyPercent);
            g_waveMakerStates[wmKey] = {true, task.dutyPercent};
            actionDesc = "Wavemaker ON (" + String(task.dutyPercent, 0) + "%)";
            debugLog(">>> executeTask: g_waveMakerStates[%s] set to running=true duty=%.1f", wmKey.c_str(), task.dutyPercent);
        } else {
            debugLog(">>> executeTask: WAVEMAKER STOP — calling sendWaveMakerStop()");
            Serial.printf("[SCHEDULER] Executing WAVEMAKER STOP task: %s\n", task.mac.c_str());
            sendWaveMakerStop(mac);
            g_waveMakerStates[wmKey] = {false, 0.0f};
            actionDesc = "Wavemaker OFF";
            debugLog(">>> executeTask: g_waveMakerStates[%s] set to running=false", wmKey.c_str());
        }
        
        // Notify: scheduled task executed
        String devName, aqName, devType;
        lookupDeviceAndAquariumNames(task.mac, devName, aqName, devType);
        debugLog(">>> executeTask: notifying — dev=%s aq=%s type=%s action=%s",
                 devName.c_str(), aqName.c_str(), devType.c_str(), actionDesc.c_str());
        notifier.emitf("scheduler.task", NTFY_MSG_TASK_EXECUTED,
                        aqName.c_str(), devName.c_str(), devType.c_str(), actionDesc.c_str());
        appendActivityLog("scheduled", "wavemaker", actionDesc.c_str(), devName.c_str(), aqName.c_str());
        debugLog(">>> executeTask: WAVEMAKER task complete — returning true");
        return true;
    } else {
        // Execute light task (default)
        // Calculate command code: CH1=10/11, CH2=20/21, CH3=30/31
        uint8_t command = (task.channel * 10) + (task.actionOn ? 1 : 0);
        
        sendLightCommand(mac, command);
        
        // Update channel state tracking
        String macKey = task.mac;
        macKey.toUpperCase();
        if (g_channelStates.find(macKey) == g_channelStates.end()) {
            g_channelStates[macKey] = {false, false, false, -1};
        }
        ChannelState& state = g_channelStates[macKey];
        if (task.channel == 1) state.ch1_on = task.actionOn;
        else if (task.channel == 2) state.ch2_on = task.actionOn;
        else if (task.channel == 3) state.ch3_on = task.actionOn;
        
        // Notify: scheduled task executed
        String devName, aqName, devType;
        lookupDeviceAndAquariumNames(task.mac, devName, aqName, devType);
        String lightAction = "CH" + String(task.channel) + (task.actionOn ? " ON" : " OFF");
        notifier.emitf("scheduler.task", NTFY_MSG_TASK_EXECUTED,
                        aqName.c_str(), devName.c_str(), devType.c_str(), lightAction.c_str());
        appendActivityLog("scheduled", "light", lightAction.c_str(), devName.c_str(), aqName.c_str());
        
        return true;
    }
}

// Find and execute any past-due tasks, with retry logic for offline nodes
static void processPastDueTasks(std::vector<NextTask>& tasks, bool& needsRebuild) {
    time_t now = getCurrentUnixTime();
    if (now == 0) {
        debugLog("processPastDueTasks: NTP not ready (now=0), RETURNING");
        return;  // NTP not ready
    }
    
    debugLog("=== processPastDueTasks: now=%ld, %d tasks in list ===", (long)now, tasks.size());
    for (size_t i = 0; i < tasks.size(); i++) {
        long diff = (long)(tasks[i].scheduledTime - now);
        debugLog("  [%d] mac=%s type=%d actionOn=%d schedTime=%ld diff=%lds duty=%.1f%% period=%s",
                 (int)i, tasks[i].mac.c_str(), (int)tasks[i].taskType, tasks[i].actionOn,
                 (long)tasks[i].scheduledTime, diff, tasks[i].dutyPercent, tasks[i].period.c_str());
    }
    
    std::vector<NextTask> pendingRetry;
    bool anyExecuted = false;
    
    for (auto it = tasks.begin(); it != tasks.end(); ) {
        if (it->scheduledTime <= now) {
            debugLog("  PAST-DUE found: mac=%s type=%d actionOn=%d duty=%.1f%% schedTime=%ld (now=%ld)",
                     it->mac.c_str(), (int)it->taskType, it->actionOn, it->dutyPercent,
                     (long)it->scheduledTime, (long)now);
            // Check if device is in maintenance mode - skip but keep task for later
            if (isDeviceInMaintenanceMode(it->mac)) {
                Serial.printf("[SCHEDULER] SKIPPED (maintenance mode): %s task for %s\n",
                             it->taskType == TaskType::LIGHT ? "LIGHT" : 
                             it->taskType == TaskType::FEEDER ? "FEEDER" : "WAVEMAKER",
                             it->mac.c_str());
                ++it;  // Keep task in list, don't remove or reschedule
                continue;
            }
            
            if (it->taskType == TaskType::FEEDER) {
                Serial.printf("[SCHEDULER] Past-due FEEDER task: %s %s PWM=%u (scheduled %ld, now %ld)\n",
                             it->mac.c_str(), it->period.c_str(), it->pwmValue,
                             (long)it->scheduledTime, (long)now);
            } else if (it->taskType == TaskType::WAVEMAKER) {
                Serial.printf("[SCHEDULER] Past-due WAVEMAKER task: %s %s duty=%.1f%% (scheduled %ld, now %ld)\n",
                             it->mac.c_str(), it->actionOn ? "ON" : "STOP", it->dutyPercent,
                             (long)it->scheduledTime, (long)now);
            } else {
                Serial.printf("[SCHEDULER] Past-due LIGHT task: %s CH%d %s (scheduled %ld, now %ld)\n",
                             it->mac.c_str(), it->channel, it->actionOn ? "ON" : "OFF",
                             (long)it->scheduledTime, (long)now);
            }
            
            debugLog("  Calling executeTask for mac=%s type=%d actionOn=%d duty=%.1f%%",
                     it->mac.c_str(), (int)it->taskType, it->actionOn, it->dutyPercent);
            if (executeTask(*it)) {
                debugLog("  executeTask RETURNED TRUE (success) for mac=%s type=%d", it->mac.c_str(), (int)it->taskType);
                anyExecuted = true;
                it = tasks.erase(it);  // Remove executed task
            } else {
                debugLog("  executeTask RETURNED FALSE (failed/offline) for mac=%s type=%d", it->mac.c_str(), (int)it->taskType);
                // Node offline - keep for retry but update scheduled time to future
                it->scheduledTime = now + 60;  // Retry in 60 seconds
                pendingRetry.push_back(*it);
                it = tasks.erase(it);
            }
        } else {
            ++it;
        }
    }
    
    // Add retry tasks back
    for (const NextTask& t : pendingRetry) {
        tasks.push_back(t);
    }
    
    debugLog("=== processPastDueTasks done: anyExecuted=%d retryCount=%d needsRebuild=%d ===",
             anyExecuted, pendingRetry.size(), anyExecuted || !pendingRetry.empty());
    if (anyExecuted || !pendingRetry.empty()) {
        needsRebuild = true;  // Tasks changed, need to save
    }
}

void schedulerTask(void* parameter) {
    Serial.printf("[SCHEDULER] Task started on core %d\n", xPortGetCoreID());

    uint32_t minSleepMs = config.schedulerMinSleepSec * 1000;
    uint32_t maxSleepMs = config.schedulerMaxSleepSec * 1000;
    if (minSleepMs == 0) {
        minSleepMs = 1000;
    }
    if (maxSleepMs < minSleepMs) {
        maxSleepMs = minSleepMs;
    }
    
    // Wait for NTP to sync (timeout after 30 seconds)
    int waitCount = 0;
    while (!ntpSynced && waitCount < 60) {
        vTaskDelay(pdMS_TO_TICKS(500));
        waitCount++;
    }
    
    if (!ntpSynced) {
        Serial.println("[SCHEDULER] Warning: NTP not synced, scheduler may not work correctly");
    }
    
    // Initial rebuild of next-task.json on startup
    rebuildNextTasks();
    
    // Load tasks
    std::vector<NextTask> tasks;
    loadNextTasks(tasks);
    
    // Process any past-due tasks from before power loss
    bool needsSave = false;
    processPastDueTasks(tasks, needsSave);
    if (needsSave) {
        rebuildNextTasks();  // Recalculate all future tasks
        loadNextTasks(tasks);
    }
    
    // Track last rebuild time to refresh tasks daily
    time_t lastRebuildTime = getCurrentUnixTime();
    
    while (true) {
        time_t now = getCurrentUnixTime();
        
        // Refresh task list once per day (handle day rollover)
        if (now - lastRebuildTime > 12 * 60 * 60) {  // Every 12 hours
            Serial.println("[SCHEDULER] Periodic task rebuild");
            rebuildNextTasks();
            loadNextTasks(tasks);
            lastRebuildTime = now;
        }
        
        // FIRST: Process any past-due tasks that became due during sleep.
        // This MUST run before findNextTask/rebuild, because findNextTask
        // only finds future tasks (scheduledTime > now) and a rebuild would
        // overwrite past-due entries with next-day occurrences, losing them.
        debugLog("--- schedulerLoop iteration: now=%ld, tasks.size=%d ---", (long)now, tasks.size());
        needsSave = false;
        processPastDueTasks(tasks, needsSave);
        if (needsSave) {
            debugLog("--- schedulerLoop: needsSave=true after processPastDueTasks, rebuilding ---");
            rebuildNextTasks();
            loadNextTasks(tasks);
        }
        
        // Find next future task
        NextTask nextTask;
        if (findNextTask(tasks, nextTask)) {
            time_t waitSeconds = nextTask.scheduledTime - now;
            debugLog("--- schedulerLoop: findNextTask found mac=%s type=%d duty=%.1f%% in %lds ---",
                     nextTask.mac.c_str(), (int)nextTask.taskType, nextTask.dutyPercent, (long)waitSeconds);
            
            if (waitSeconds <= 0) {
                // Task is due now (edge case: time advanced between calls)
                debugLog("--- schedulerLoop: waitSeconds<=0, executing NOW: mac=%s type=%d duty=%.1f%% ---",
                         nextTask.mac.c_str(), (int)nextTask.taskType, nextTask.dutyPercent);
                Serial.printf("[SCHEDULER] Executing: %s CH%d %s\n",
                             nextTask.mac.c_str(), nextTask.channel, 
                             nextTask.actionOn ? "ON" : "OFF");
                
                if (executeTask(nextTask)) {
                    debugLog("--- schedulerLoop: direct executeTask returned true, rebuilding ---");
                    // Remove executed task and recalculate next occurrence
                    rebuildNextTasks();
                    loadNextTasks(tasks);
                } else if (isDeviceInMaintenanceMode(nextTask.mac)) {
                    // Maintenance mode - don't retry aggressively, sleep normally
                    debugLog("--- schedulerLoop: direct executeTask failed (maintenance), sleeping 60s ---");
                    Serial.println("[SCHEDULER] Task skipped (maintenance mode), checking again in 60s");
                    vTaskDelay(pdMS_TO_TICKS(60000));
                } else {
                    // Node offline - wait 60 seconds and retry
                    debugLog("--- schedulerLoop: direct executeTask failed (offline?), sleeping 60s ---");
                    Serial.println("[SCHEDULER] Node offline, retrying in 60s");
                    vTaskDelay(pdMS_TO_TICKS(60000));
                }
            } else {
                // Sleep until next task, capped to max sleep
                uint32_t waitMs = (uint32_t)(waitSeconds * 1000);
                if (waitMs < minSleepMs) {
                    waitMs = minSleepMs;
                } else if (waitMs > maxSleepMs) {
                    waitMs = maxSleepMs;
                }
                Serial.printf("[SCHEDULER] Next task in %ld sec, sleeping %lu ms\n", 
                             (long)waitSeconds, waitMs);
                vTaskDelay(pdMS_TO_TICKS(waitMs));
            }
        } else {
            debugLog("--- schedulerLoop: NO upcoming tasks found, sleeping %lu ms ---", maxSleepMs);
            // No upcoming tasks, sleep for 5 minutes before rechecking
            Serial.printf("[SCHEDULER] No upcoming tasks, sleeping %lu ms\n", maxSleepMs);
            vTaskDelay(pdMS_TO_TICKS(maxSleepMs));
            
            // Rebuild in case schedule was updated
            rebuildNextTasks();
            loadNextTasks(tasks);
        }
    }
}

// ============================================================================
// NODE OTA TASK - Async OTA Transfer to Nodes
// ============================================================================

// Helper function to send OTA to a single device
static bool sendOtaToDevice(uint8_t* targetMac, WiFiClient* client) {
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             targetMac[0], targetMac[1], targetMac[2],
             targetMac[3], targetMac[4], targetMac[5]);
    Serial.printf("[NodeOTA] Sending OTA to device %s\n", macStr);
    
    // Copy target MAC for status callbacks
    memcpy(nodeOtaState.targetMac, targetMac, 6);
    
    // Reset per-device state
    nodeOtaState.configChunksSent = 0;
    nodeOtaState.firmwareChunksSent = 0;
    nodeOtaState.configSent = false;
    nodeOtaState.firmwareSent = false;
    nodeOtaState.firmwareWaitingEnd = false;
    
    HTTPClient http;
    bool deviceSuccess = false;

    // Step 1: Send config if available (from previously saved file)
    if (FS_STATIC.exists("/ota/light/node_config.txt")) {
        File configFile = FS_STATIC.open("/ota/light/node_config.txt", "r");
        if (configFile) {
            String configContent = configFile.readString();
            configFile.close();
            
            // Send OTA_BEGIN for config
            CommandMessage beginMsg = {};
            beginMsg.header.type = MessageType::COMMAND;
            beginMsg.header.tankId = 0;
            beginMsg.header.nodeType = NodeType::HUB;
            beginMsg.header.timestamp = millis();
            beginMsg.commandId = generateCommandId();
            beginMsg.commandSeqID = 0;
            beginMsg.finalCommand = false;
            beginMsg.commandData[0] = OTA_CMD_OTA_BEGIN;
            beginMsg.commandData[1] = OTA_CMD_CONFIG_CHUNK;
            uint32_t totalSize = configContent.length();
            memcpy(&beginMsg.commandData[2], &totalSize, 4);
            beginMsg.commandData[6] = 29;

            for (int i = 0; i < 5; i++) {
                ESPNowManager::getInstance().send(targetMac, (uint8_t*)&beginMsg, sizeof(beginMsg));
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            vTaskDelay(pdMS_TO_TICKS(500));

            // Send config chunks
            uint16_t chunkIndex = 0;
            size_t offset = 0;
            const size_t chunkPayloadSize = 29;
            nodeOtaState.totalConfigChunks = (configContent.length() + chunkPayloadSize - 1) / chunkPayloadSize;

            while (offset < configContent.length()) {
                size_t remaining = configContent.length() - offset;
                size_t thisChunkSize = (remaining > chunkPayloadSize) ? chunkPayloadSize : remaining;

                CommandMessage chunkMsg = {};
                chunkMsg.header.type = MessageType::COMMAND;
                chunkMsg.header.tankId = 0;
                chunkMsg.header.nodeType = NodeType::HUB;
                chunkMsg.header.timestamp = millis();
                chunkMsg.commandId = beginMsg.commandId;
                chunkMsg.commandSeqID = chunkIndex + 1;
                chunkMsg.finalCommand = (offset + thisChunkSize >= configContent.length());
                chunkMsg.commandData[0] = OTA_CMD_CONFIG_CHUNK;
                memcpy(&chunkMsg.commandData[1], &chunkIndex, 2);
                memcpy(&chunkMsg.commandData[3], configContent.c_str() + offset, thisChunkSize);

                ESPNowManager::getInstance().send(targetMac, (uint8_t*)&chunkMsg, sizeof(chunkMsg));
                
                offset += thisChunkSize;
                chunkIndex++;
                nodeOtaState.configChunksSent = chunkIndex;
                vTaskDelay(pdMS_TO_TICKS(30));
            }

            // Send OTA_END for config
            CommandMessage endMsg = {};
            endMsg.header.type = MessageType::COMMAND;
            endMsg.header.tankId = 0;
            endMsg.header.nodeType = NodeType::HUB;
            endMsg.header.timestamp = millis();
            endMsg.commandId = beginMsg.commandId;
            endMsg.commandSeqID = chunkIndex + 1;
            endMsg.finalCommand = true;
            endMsg.commandData[0] = OTA_CMD_OTA_END;
            endMsg.commandData[1] = OTA_CMD_CONFIG_CHUNK;

            for (int i = 0; i < 5; i++) {
                ESPNowManager::getInstance().send(targetMac, (uint8_t*)&endMsg, sizeof(endMsg));
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            
            nodeOtaState.configSent = true;
            Serial.printf("[NodeOTA] Config sent to %s: %d chunks\n", macStr, chunkIndex);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    // Step 2: Send firmware if available
    if (FS_STATIC.exists("/ota/light/firmware.bin")) {
        File fwFile = FS_STATIC.open("/ota/light/firmware.bin", "r");
        if (fwFile) {
            size_t fwSize = fwFile.size();
            nodeOtaState.totalFirmwareChunks = (fwSize + 28) / 29;
            Serial.printf("[NodeOTA] Sending firmware to %s (%d bytes, ~%d chunks)\n", 
                          macStr, fwSize, nodeOtaState.totalFirmwareChunks);

            // Send OTA_BEGIN for firmware
            CommandMessage beginMsg = {};
            beginMsg.header.type = MessageType::COMMAND;
            beginMsg.header.tankId = 0;
            beginMsg.header.nodeType = NodeType::HUB;
            beginMsg.header.timestamp = millis();
            beginMsg.commandId = generateCommandId();
            nodeOtaState.lastCommandId = beginMsg.commandId;
            beginMsg.commandSeqID = 0;
            beginMsg.finalCommand = false;
            beginMsg.commandData[0] = OTA_CMD_OTA_BEGIN;
            beginMsg.commandData[1] = OTA_CMD_FIRMWARE_CHUNK;
            uint32_t totalSize = fwSize;
            memcpy(&beginMsg.commandData[2], &totalSize, 4);
            beginMsg.commandData[6] = 29;

            for (int i = 0; i < 5; i++) {
                ESPNowManager::getInstance().send(targetMac, (uint8_t*)&beginMsg, sizeof(beginMsg));
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            vTaskDelay(pdMS_TO_TICKS(500));

            // Stream firmware chunks
            uint8_t buffer[29];
            uint16_t chunkIndex = 0;
            size_t totalSent = 0;

            while (fwFile.available()) {
                size_t bytesRead = fwFile.read(buffer, 29);
                if (bytesRead == 0) break;
                
                bool isLastChunk = !fwFile.available();

                CommandMessage chunkMsg = {};
                chunkMsg.header.type = MessageType::COMMAND;
                chunkMsg.header.tankId = 0;
                chunkMsg.header.nodeType = NodeType::HUB;
                chunkMsg.header.timestamp = millis();
                chunkMsg.commandId = beginMsg.commandId;
                chunkMsg.commandSeqID = chunkIndex + 1;
                chunkMsg.finalCommand = isLastChunk;
                chunkMsg.commandData[0] = OTA_CMD_FIRMWARE_CHUNK;
                memcpy(&chunkMsg.commandData[1], &chunkIndex, 2);
                memcpy(&chunkMsg.commandData[3], buffer, bytesRead);

                ESPNowManager::getInstance().send(targetMac, (uint8_t*)&chunkMsg, sizeof(chunkMsg));
                
                totalSent += bytesRead;
                chunkIndex++;
                nodeOtaState.firmwareChunksSent = chunkIndex;
                
                if (chunkIndex % 500 == 0) {
                    Serial.printf("[NodeOTA] Progress %s: %d/%d bytes (%d%%)\n", 
                                  macStr, totalSent, fwSize, (totalSent * 100) / fwSize);
                }
                
                if (isLastChunk) {
                    Serial.printf("[NodeOTA] Last chunk sent to %s (index %d)\n", macStr, chunkIndex - 1);
                }
                vTaskDelay(pdMS_TO_TICKS(30));
            }

            fwFile.close();

            // Wait for READY_END from node
            nodeOtaState.firmwareSent = true;
            nodeOtaState.firmwareWaitingEnd = true;
            Serial.printf("[NodeOTA] Firmware sent to %s: %d chunks. Waiting for READY_END...\n", macStr, chunkIndex);
            
            uint32_t waitStart = millis();
            while (nodeOtaState.firmwareWaitingEnd && (millis() - waitStart < 30000)) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            
            if (nodeOtaState.firmwareWaitingEnd) {
                Serial.printf("[NodeOTA] WARN: %s timed out waiting for READY_END, sending END anyway\n", macStr);
                CommandMessage endMsg = {};
                endMsg.header.type = MessageType::COMMAND;
                endMsg.header.tankId = 0;
                endMsg.header.nodeType = NodeType::HUB;
                endMsg.header.timestamp = millis();
                endMsg.commandId = beginMsg.commandId;
                endMsg.commandSeqID = chunkIndex + 1;
                endMsg.finalCommand = true;
                endMsg.commandData[0] = OTA_CMD_OTA_END;
                endMsg.commandData[1] = OTA_CMD_FIRMWARE_CHUNK;

                for (int i = 0; i < 5; i++) {
                    ESPNowManager::getInstance().send(targetMac, (uint8_t*)&endMsg, sizeof(endMsg));
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
            }
            
            deviceSuccess = true;
        }
    }

    return deviceSuccess || nodeOtaState.configSent;
}

void nodeOtaTask(void* parameter) {
    Serial.printf("[NodeOTA] Task started on core %d, updating %d device(s)\n", 
                  xPortGetCoreID(), nodeOtaState.targetCount);
    notifier.emitf("system.ota.node", NTFY_MSG_NODE_OTA_STARTED,
                    nodeOtaState.deviceType.c_str(), nodeOtaState.targetCount);
    
    // Download files first (only once)
    std::unique_ptr<WiFiClient> client;
    if (nodeOtaState.baseUrl.startsWith("https://")) {
        std::unique_ptr<WiFiClientSecure> secureClient(new WiFiClientSecure());
        secureClient->setInsecure();
        client = std::move(secureClient);
    } else {
        client = std::unique_ptr<WiFiClient>(new WiFiClient());
    }

    HTTPClient http;
    
    // Create OTA directory
    if (!FS_STATIC.exists("/ota")) FS_STATIC.mkdir("/ota");
    if (!FS_STATIC.exists("/ota/light")) FS_STATIC.mkdir("/ota/light");

    // Download node_config.txt (optional)
    if (http.begin(*client.get(), nodeOtaState.baseUrl + "node_config.txt")) {
        int httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK) {
            String configContent = http.getString();
            File localConfig = FS_STATIC.open("/ota/light/node_config.txt", "w");
            if (localConfig) {
                localConfig.print(configContent);
                localConfig.close();
                nodeOtaState.configSaved = true;
                Serial.printf("[NodeOTA] Downloaded node_config.txt (%d bytes)\n", configContent.length());
            }
        }
        http.end();
    }

    // Download firmware.bin (required)
    if (http.begin(*client.get(), nodeOtaState.baseUrl + "firmware.bin")) {
        int httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK) {
            int contentLength = http.getSize();
            Serial.printf("[NodeOTA] Downloading firmware.bin (%d bytes)\n", contentLength);
            
            File localFirmware = FS_STATIC.open("/ota/light/firmware.bin", "w");
            if (localFirmware) {
                WiFiClient* dlStream = http.getStreamPtr();
                uint8_t dlBuffer[512];
                size_t dlTotal = 0;
                
                while (http.connected() && (contentLength <= 0 || dlTotal < (size_t)contentLength)) {
                    size_t available = dlStream->available();
                    if (available == 0) {
                        vTaskDelay(pdMS_TO_TICKS(1));
                        continue;
                    }
                    size_t toRead = (available > 512) ? 512 : available;
                    size_t bytesRead = dlStream->readBytes(dlBuffer, toRead);
                    if (bytesRead == 0) break;
                    
                    localFirmware.write(dlBuffer, bytesRead);
                    dlTotal += bytesRead;
                }
                localFirmware.close();
                nodeOtaState.firmwareSaved = true;
                Serial.printf("[NodeOTA] Downloaded firmware.bin (%d bytes)\n", dlTotal);
            }
        }
        http.end();
    }

    // Check if we have anything to send
    if (!nodeOtaState.configSaved && !nodeOtaState.firmwareSaved) {
        nodeOtaState.error = "No OTA files downloaded";
        nodeOtaState.completed = true;
        nodeOtaState.success = false;
        Serial.println("[NodeOTA] ERROR: No OTA files available");
        goto cleanup;
    }

    // Send OTA to each target device
    for (nodeOtaState.currentTarget = 0; 
         nodeOtaState.currentTarget < nodeOtaState.targetCount; 
         nodeOtaState.currentTarget++) {
        
        uint8_t* targetMac = nodeOtaState.targetMacs[nodeOtaState.currentTarget];
        
        // Ensure node is registered as peer
        ESPNowManager::getInstance().addPeer(targetMac);
        
        // Send OTA to this device
        bool success = sendOtaToDevice(targetMac, client.get());
        
        if (success) {
            nodeOtaState.devicesUpdated++;
            Serial.printf("[NodeOTA] Device %d/%d updated successfully\n", 
                          nodeOtaState.currentTarget + 1, nodeOtaState.targetCount);
        } else {
            nodeOtaState.devicesFailed++;
            Serial.printf("[NodeOTA] Device %d/%d FAILED\n", 
                          nodeOtaState.currentTarget + 1, nodeOtaState.targetCount);
        }
        
        // Delay between devices to allow reboot
        if (nodeOtaState.currentTarget < nodeOtaState.targetCount - 1) {
            Serial.println("[NodeOTA] Waiting 5 seconds before next device...");
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }

    // Complete
    nodeOtaState.completed = true;
    nodeOtaState.success = (nodeOtaState.devicesUpdated > 0);
    if (nodeOtaState.devicesFailed > 0) {
        char errBuf[64];
        snprintf(errBuf, sizeof(errBuf), "%d of %d devices failed", 
                 nodeOtaState.devicesFailed, nodeOtaState.targetCount);
        nodeOtaState.error = errBuf;
    }
    Serial.printf("[NodeOTA] Complete: %d updated, %d failed\n", 
                  nodeOtaState.devicesUpdated, nodeOtaState.devicesFailed);
    notifier.emitf("system.ota.node", NTFY_MSG_NODE_OTA_COMPLETE,
                    nodeOtaState.devicesUpdated, nodeOtaState.devicesFailed);

cleanup:
    // Delete downloaded OTA files
    if (FS_STATIC.exists("/ota/light/firmware.bin")) {
        FS_STATIC.remove("/ota/light/firmware.bin");
        Serial.println("[NodeOTA] Deleted /ota/light/firmware.bin");
    }
    if (FS_STATIC.exists("/ota/light/node_config.txt")) {
        FS_STATIC.remove("/ota/light/node_config.txt");
        Serial.println("[NodeOTA] Deleted /ota/light/node_config.txt");
    }
    
    nodeOtaState.active = false;
    nodeOtaTaskHandle = NULL;
    vTaskDelete(NULL);
}

// ============================================================================
// WATCHDOG TASK (Core 0) - Device Health Monitoring
// ============================================================================

void watchdogTask(void* parameter) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xCheckInterval = pdMS_TO_TICKS(5000);  // Check every 5 seconds
    
    Serial.printf(" Watchdog task started on core %d\n", xPortGetCoreID());
    
    unsigned long lastMemoryCheck = 0;
    unsigned long lastHealthCheck = 0;
    unsigned long lastWaterCheck = 0;
    unsigned long lastOfflineNotifCheck = 0;
    
    // 6-minute threshold for offline notification (user requirement)
    static constexpr uint32_t OFFLINE_NOTIF_TIMEOUT_MS = 360000;  // 6 minutes
    
    while (true) {
        unsigned long now = millis();
        
        // Device health monitoring (every 5 seconds)
        if (now - lastHealthCheck >= 5000) {
            lastHealthCheck = now;
            AquariumManager::getInstance().checkDeviceHealth();
        }
        
        // Device offline notification check (every 30 seconds)
        // Sends ntfy notification once per device when offline for 6 min
        if (now - lastOfflineNotifCheck >= 30000) {
            lastOfflineNotifCheck = now;
            
            auto allDevices = AquariumManager::getInstance().getAllDevices();
            for (Device* dev : allDevices) {
                if (!dev || !dev->isEnabled()) continue;
                
                bool timedOut = dev->hasHeartbeatTimedOut(OFFLINE_NOTIF_TIMEOUT_MS);
                
                if (timedOut && !dev->isOfflineNotifSent()) {
                    // First time detecting this device offline for 6 min — send notification
                    dev->setOfflineNotifSent(true);
                    
                    // Look up aquarium name
                    String aqName = "Unknown";
                    Aquarium* aq = AquariumManager::getInstance().getAquarium(dev->getTankId());
                    if (aq) aqName = aq->getName();
                    
                    notifier.emitf("node.offline", NTFY_MSG_DEVICE_OFFLINE,
                                    dev->getName().c_str(), aqName.c_str());
                }
                
                if (!timedOut && dev->isOfflineNotifSent()) {
                    // Device came back online — reset flag and notify
                    dev->setOfflineNotifSent(false);
                    
                    String aqName = "Unknown";
                    Aquarium* aq = AquariumManager::getInstance().getAquarium(dev->getTankId());
                    if (aq) aqName = aq->getName();
                    
                    notifier.emitf("node.online", NTFY_MSG_DEVICE_BACK_ONLINE,
                                    dev->getName().c_str(), aqName.c_str());
                }
            }
        }
        
        // Water parameter monitoring (every 10 seconds)
        if (now - lastWaterCheck >= 10000) {
            lastWaterCheck = now;
            AquariumManager::getInstance().checkWaterParameters();
        }
        
        // Memory monitoring (every 30 seconds)
        if (config.heartbeatEnabled && (now - lastMemoryCheck >= config.heartbeatIntervalSec * 1000)) {
            lastMemoryCheck = now;
            printMemoryStatus();
            if (config.aggressiveMemoryManagement) {
                aggressiveMemoryCleanup();
            }
        }
        
        // Wait for next cycle
        vTaskDelayUntil(&xLastWakeTime, xCheckInterval);
    }
}

// ============================================================================
// NOTIFICATION SETUP (called from Web UI task on Core 1)
// ============================================================================

void setupNotifications() {
    // Read NTFY_TOPIC from hub_config.txt (already loaded into hubConfigManager from StaticFS)
    String ntfyTopic = hubConfigManager.getString("NTFY_TOPIC", "");
    ntfyTopic.trim();

    // Register ntfy channel if topic is configured
    if (ntfyTopic.length() > 0) {
        String url = "https://ntfy.sh/" + ntfyTopic;
        notifier.addChannel("ntfy", new NtfyChannel(url.c_str()));
        LOG_INFO("[NTF] ntfy.sh channel registered: %s", url.c_str());
    } else {
        LOG_WARN("[NTF] NTFY_TOPIC not set in hub_config.txt — ntfy channel disabled");
    }

    // Always register serial channel for development logging
    notifier.addChannel("log", new SerialChannel());

    // Load routing rules from JSON config (AOP-style)
    // Use FS_USER explicitly — dual-LittleFS means default LittleFS won't find user files
    bool routesLoaded = false;
    if (FS_USER.exists("/config/notifications.json")) {
        notifier.loadConfig(FS_USER, "/config/notifications.json");
        // loadConfig may overwrite the ntfy channel URL from the JSON file's "channels"
        // section (which could contain a placeholder). Re-apply the correct URL from hub_config.
        if (ntfyTopic.length() > 0) {
            String correctUrl = "https://ntfy.sh/" + ntfyTopic;
            NotifChannel* ch = notifier.getChannel("ntfy");
            if (ch && strcmp(ch->typeName(), "ntfy") == 0) {
                static_cast<NtfyChannel*>(ch)->setUrl(correctUrl.c_str());
                LOG_INFO("[NTF] Ensured ntfy URL from hub_config: %s", correctUrl.c_str());
            }
        }
        if (notifier.getRouteCount() > 0) {
            routesLoaded = true;
            LOG_INFO("[NTF] Loaded %d notification routes from /config/notifications.json", notifier.getRouteCount());
        } else {
            LOG_WARN("[NTF] notifications.json exists but contains no routes, using defaults");
        }
    }
    if (!routesLoaded) {
        // No config file or empty config — set up sensible defaults programmatically
        LOG_INFO("[NTF] Setting up default notification routes");
        if (ntfyTopic.length() > 0) {
            notifier.route("system.*",    "ntfy", NTF_PRIORITY_DEFAULT, "AMS - Hub",           0);
            notifier.route("safety.*",    "ntfy", NTF_PRIORITY_URGENT,  "AMS - SAFETY ALERT",  0);
            notifier.route("node.*",      "ntfy", NTF_PRIORITY_HIGH,    "AMS - Node Event", 10000);
            notifier.route("config.*",    "ntfy", NTF_PRIORITY_DEFAULT, "AMS - Config",        0);
            notifier.route("scheduler.*", "ntfy", NTF_PRIORITY_DEFAULT, "AMS - Scheduler", 30000);
        }
        // Always log everything to Serial
        notifier.route("*", "log", NTF_PRIORITY_DEFAULT, "", 0);
    }

    // Start async worker on Core 1 (same core as WebUI)
    notifier.begin(1);

    notifier.printRoutes();
}

// ============================================================================
// WEB UI TASK (Core 1) - Web server + UI
// ============================================================================

void webUiTask(void* parameter) {
    Serial.printf(" Web UI task started on core %d\n", xPortGetCoreID());

    // Setup web server on Web UI core
    setupWebServer();

    // Setup notification framework (async worker on Core 1)
    setupNotifications();

    // Now that channels and routes are registered, emit boot notification
    notifier.emitf("system.boot", NTFY_MSG_WEBSERVER_UP, WiFi.localIP().toString().c_str());

    // Keep task alive (AsyncWebServer runs in background)
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ============================================================================
// FILESYSTEM SETUP
// ============================================================================

bool setupFilesystem() {
#ifdef DUAL_LITTLEFS
    // Dual filesystem mode: separate partitions for static and user data
    Serial.println(" Initializing dual LittleFS partitions...");
    
    // Mount static filesystem (UI, OTA, hub_config)
    Serial.println("   Mounting static_fs partition (UI + config)...");
    if (!StaticFS.begin(true, "/static", 10, "static_fs")) {
        Serial.println("   ERROR: Failed to mount static_fs!");
        return false;
    }
    size_t staticTotal = StaticFS.totalBytes();
    size_t staticUsed = StaticFS.usedBytes();
    Serial.printf("   static_fs: %u / %u bytes (%.1f%% used)\n", 
                  staticUsed, staticTotal, staticTotal > 0 ? (staticUsed * 100.0 / staticTotal) : 0);
    
    // Mount user filesystem (JSON config files - preserved during OTA)
    Serial.println("   Mounting user_fs partition (JSON data)...");
    if (!UserFS.begin(true, "/user", 10, "user_fs")) {
        Serial.println("   ERROR: Failed to mount user_fs!");
        return false;
    }
    size_t userTotal = UserFS.totalBytes();
    size_t userUsed = UserFS.usedBytes();
    Serial.printf("   user_fs:   %u / %u bytes (%.1f%% used)\n",
                  userUsed, userTotal, userTotal > 0 ? (userUsed * 100.0 / userTotal) : 0);
    
    Serial.println(" Dual LittleFS initialized (user data preserved during OTA!)");
    
#else
    // Single filesystem mode (backward compatible)
    Serial.println(" Initializing LittleFS (single partition mode)...");
    
    if (!LittleFS.begin(true)) {
        Serial.println(" LittleFS mount failed");
        return false;
    }
    
    Serial.println(" LittleFS mounted");
#endif
    
    // Initialize unmapped-devices.json if it doesn't exist
    if (!FS_USER.exists("/config/unmapped-devices.json")) {
        Serial.println(" Creating unmapped-devices.json...");
        // Ensure config directory exists
        if (!FS_USER.exists("/config")) {
            FS_USER.mkdir("/config");
        }
        File file = FS_USER.open("/config/unmapped-devices.json", "w");
        if (file) {
            file.print("{\"metadata\":{\"lastCleanup\":0,\"totalDiscovered\":0,\"autoCleanupAfterDays\":7},\"unmappedDevices\":[]}");
            file.close();
            Serial.println("   - unmapped-devices.json initialized");
        } else {
            Serial.println("   - ERROR: Failed to create unmapped-devices.json");
        }
    }

    // Ensure schedule directory exists
    if (!FS_USER.exists("/config/schedule")) {
        FS_USER.mkdir("/config/schedule");
    }

    // Initialize light-schedule.json if it doesn't exist
    if (!FS_USER.exists("/config/schedule/light-schedule.json")) {
        Serial.println(" Creating light-schedule.json...");
        File file = FS_USER.open("/config/schedule/light-schedule.json", "w");
        if (file) {
            file.print("{\"schedules\":[]}");
            file.close();
            Serial.println("   - light-schedule.json initialized");
        } else {
            Serial.println("   - ERROR: Failed to create light-schedule.json");
        }
    }

    // Initialize wm-schedule.json if it doesn't exist
    if (!FS_USER.exists("/config/schedule/wm-schedule.json")) {
        Serial.println(" Creating wm-schedule.json...");
        File file = FS_USER.open("/config/schedule/wm-schedule.json", "w");
        if (file) {
            file.print("{\"schedules\":[]}");
            file.close();
            Serial.println("   - wm-schedule.json initialized");
        } else {
            Serial.println("   - ERROR: Failed to create wm-schedule.json");
        }
    }
    
    // List files (debug)
    if (config.debugSerial) {
#ifdef DUAL_LITTLEFS
        Serial.println(" Static filesystem contents:");
        File staticRoot = FS_STATIC.open("/");
        File f = staticRoot.openNextFile();
        while (f) {
            Serial.printf("   - %s (%d bytes)\n", f.name(), f.size());
            f = staticRoot.openNextFile();
        }
        Serial.println(" User filesystem contents:");
        File userRoot = FS_USER.open("/");
        f = userRoot.openNextFile();
        while (f) {
            Serial.printf("   - %s (%d bytes)\n", f.name(), f.size());
            f = userRoot.openNextFile();
        }
#else
        Serial.println(" Filesystem contents:");
        File root = LittleFS.open("/");
        File file = root.openNextFile();
        while (file) {
            Serial.printf("   - %s (%d bytes)\n", file.name(), file.size());
            file = root.openNextFile();
        }
#endif
    }
    
    return true;
}

// ============================================================================
// WIFI & NETWORK SETUP
// ============================================================================

void setupWiFi() {
    Serial.println(" Starting WiFi configuration...");
    
    // Use AP+STA mode to allow both:
    // - STA: Connect to home WiFi for NTP time sync
    // - AP: Fallback config portal and ESP-NOW
    WiFi.mode(WIFI_AP_STA);
    Serial.println(" [HUB] WiFi mode set to: WIFI_AP_STA (dual mode for NTP + ESP-NOW)");


    
    // Set hostname before WiFi begins
    WiFi.setHostname(config.mdnsHostname.c_str());
    
    // WiFiManager configuration
    wifiManager.setConfigPortalTimeout(config.wifiTimeoutSec);
    wifiManager.setAPStaticIPConfig(IPAddress(192,168,4,1), 
                                     IPAddress(192,168,4,1), 
                                     IPAddress(255,255,255,0));
    
    // Custom parameters can be added here
    // wifiManager.addParameter(&custom_param);
    
    // Try to connect with saved credentials or start AP
    if (!wifiManager.autoConnect(config.wifiAPName.c_str(), 
                                   config.wifiAPPassword.c_str())) {
        Serial.println(" Failed to connect, restarting...");
        delay(3000);
        ESP.restart();
    }
    
    Serial.println(" WiFi connected");
    Serial.printf("   - IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("   - RSSI: %d dBm\n", WiFi.RSSI());
    Serial.printf("   - Hostname: %s\n", WiFi.getHostname());
    
    // Configure NTP time synchronization
    Serial.println(" Configuring NTP time sync...");
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");  // GMT offset 0, daylight offset 0
    setenv("TZ", "IST-5:30", 1);  // India Standard Time (UTC+5:30)
    tzset();
    
    // Wait for NTP sync (max 10 seconds)
    struct tm timeinfo;
    int ntpRetry = 0;
    while (!getLocalTime(&timeinfo) && ntpRetry < 20) {
        Serial.print(".");
        delay(500);
        ntpRetry++;
    }
    if (ntpRetry < 20) {
        ntpSynced = true;
        Serial.printf("\n   - NTP synced: %04d-%02d-%02d %02d:%02d:%02d\n",
                      timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                      timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    } else {
        Serial.println("\n   - NTP sync failed (will retry in scheduler)");
    }
    
    // CRITICAL: Set WiFi channel to match ESP-NOW channel
    // WiFi STA and ESP-NOW must use the same channel
    // This forces the WiFi connection to use channel 6 for ESP-NOW compatibility
    Serial.println(" Setting WiFi channel for ESP-NOW compatibility...");
    int currentChannel = WiFi.channel();
    Serial.printf("   - Current WiFi channel: %d\n", currentChannel);
    
    if (currentChannel != config.espnowChannel) {
        Serial.printf("   - WARNING: WiFi on channel %d, but ESP-NOW needs channel %d\n", 
                     currentChannel, config.espnowChannel);
        Serial.println("   - ESP-NOW will use WiFi's channel (not configurable in STA mode)");
        Serial.printf("   - SOLUTION: Configure your router to use channel %d\n", config.espnowChannel);
        
        // Update config to match WiFi channel
        config.espnowChannel = currentChannel;
        Serial.printf("   - Updated ESP-NOW channel to %d (WiFi channel)\n", config.espnowChannel);
    } else {
        Serial.printf("   - WiFi channel %d matches ESP-NOW channel (OK)\n", config.espnowChannel);
    }
}

void setupMDNS() {
    Serial.println("[mDNS] Starting mDNS responder...");
    Serial.printf("[mDNS] Hostname: %s\n", config.mdnsHostname.c_str());
    Serial.printf("[mDNS] WiFi status: %s\n", WiFi.isConnected() ? "Connected" : "NOT Connected");
    Serial.printf("[mDNS] WiFi mode: %d (1=STA, 2=AP, 3=AP_STA)\n", WiFi.getMode());
    Serial.printf("[mDNS] IP Address: %s\n", WiFi.localIP().toString().c_str());
    
    // End any existing mDNS instance first
    MDNS.end();
    delay(100);
    
    if (!MDNS.begin(config.mdnsHostname.c_str())) {
        Serial.println("[mDNS] ERROR: MDNS.begin() failed!");
        return;
    }
    
    // Add HTTP service
    MDNS.addService("http", "tcp", 80);
    
    // Add WebSocket service for discovery
    MDNS.addService("ws", "tcp", 80);
    
    // Add aquarium service for easy discovery
    MDNS.addService("aquarium", "tcp", 80);
    
    Serial.printf("[mDNS] Responder started successfully!\n");
    Serial.printf("[mDNS] Access via: http://%s.local\n", config.mdnsHostname.c_str());
    Serial.println("[mDNS] Note: mDNS requires Bonjour (Windows) or Avahi (Linux) on client");
}

// ============================================================================
// JSON FILE OPERATIONS
// ============================================================================

/**
 * @brief Load aquariums from JSON file
 * @return true if loaded successfully
 */
bool loadAquariumsFromFile() {
    if (!FS_USER.exists("/config/aquariums.json")) {
        Serial.println("  aquariums.json not found, creating empty file");
        File file = FS_USER.open("/config/aquariums.json", "w");
        if (file) {
            file.println("{\"aquariums\":[]}");
            file.close();
        }
        return false;
    }
    
    File file = FS_USER.open("/config/aquariums.json", "r");
    if (!file) {
        Serial.println(" Failed to open aquariums.json");
        return false;
    }
    
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    
    if (error) {
        Serial.printf(" Failed to parse aquariums.json: %s\\n", error.c_str());
        return false;
    }
    
    JsonArray aquariums = doc["aquariums"].as<JsonArray>();
    int loadedCount = 0;
    
    for (JsonObject obj : aquariums) {
        uint8_t id = obj["id"] | 0;
        String name = obj["name"] | "";
        
        if (id == 0 || name.isEmpty()) {
            Serial.println("  Skipping invalid aquarium entry");
            continue;
        }
        
        Aquarium* aquarium = new Aquarium(id, name);
        
        // Basic properties
        aquarium->setVolume(obj["volumeLiters"] | 0.0f);
        aquarium->setTankType(obj["tankType"] | "");
        aquarium->setLocation(obj["location"] | "");
        aquarium->setDescription(obj["description"] | "");
        aquarium->setEnabled(obj["enabled"] | true);
        aquarium->setMaintenanceMode(obj["maintenanceMode"] | false);
        
        // Water parameters
        JsonObject waterParams = obj["waterParameters"];
        if (!waterParams.isNull()) {
            aquarium->setTemperatureRange(
                waterParams["temperature"]["min"] | 24.0f,
                waterParams["temperature"]["max"] | 26.0f
            );
            aquarium->setPhRange(
                waterParams["ph"]["min"] | 6.5f,
                waterParams["ph"]["max"] | 7.5f
            );
            aquarium->setTdsRange(
                waterParams["tds"]["min"] | 150,
                waterParams["tds"]["max"] | 300
            );
        }
        
        // Add to manager's registry
        AquariumManager::getInstance().addAquarium(aquarium);
        loadedCount++;
        Serial.printf(" Loaded aquarium: %s (ID: %d)\\n", name.c_str(), id);
    }
    
    Serial.printf(" Loaded %d aquariums from file\\n", loadedCount);
    return loadedCount > 0;
}

/**
 * @brief Register all devices from devices.json as ESP-NOW peers
 * CRITICAL: Must be called AFTER ESP-NOW initialization
 */
void registerAllDevicesAsPeers() {
    Serial.println("");
    Serial.println(" ========================================");
    Serial.println(" Registering devices as ESP-NOW peers...");
    Serial.println(" ========================================");
    
    if (!FS_USER.exists("/config/devices.json")) {
        Serial.println("  devices.json not found - no peers to register");
        return;
    }
    
    File file = FS_USER.open("/config/devices.json", "r");
    if (!file) {
        Serial.println("  Failed to open devices.json");
        return;
    }
    
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    
    if (error) {
        Serial.printf("  Failed to parse devices.json: %s\\n", error.c_str());
        return;
    }
    
    JsonArray devices = doc["devices"].as<JsonArray>();
    int successCount = 0;
    int failCount = 0;
    int skippedCount = 0;
    
    Serial.printf("  Found %d devices in devices.json\n", devices.size());
    
    for (JsonObject obj : devices) {
        String macStr = obj["mac"] | "";
        String name = obj["name"] | "";
        
        if (macStr.isEmpty()) {
            continue;
        }
        
        // Skip dummy test devices (MACs starting with AA:BB:CC)
        if (macStr.startsWith("AA:BB:CC") || macStr.startsWith("aa:bb:cc")) {
            Serial.printf("  ⊘ %s (%s) - SKIPPED (test device)\\n", name.c_str(), macStr.c_str());
            skippedCount++;
            continue;
        }
        
        // Parse MAC address
        uint8_t mac[6];
        if (sscanf(macStr.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                  &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6) {
            
            // Register as peer
            if (ESPNowManager::getInstance().addPeer(mac)) {
                Serial.printf("  ✓ %s (%s)\\n", name.c_str(), macStr.c_str());
                successCount++;
            } else {
                Serial.printf("  ✗ %s (%s) - FAILED\\n", name.c_str(), macStr.c_str());
                failCount++;
            }
        }
    }
    
    Serial.println(" ========================================");
    Serial.printf(" Registered: %d | Failed: %d | Skipped: %d\\n", successCount, failCount, skippedCount);
    Serial.println(" ========================================");
    Serial.println("");
}

/**
 * @brief Load devices from JSON file and associate with aquariums
 * @return true if loaded successfully
 */
bool loadDevicesIntoAquariums() {
    Serial.println(" Loading devices into aquarium objects...");
    
    if (!FS_USER.exists("/config/devices.json")) {
        Serial.println("  devices.json not found");
        return false;
    }
    
    File file = FS_USER.open("/config/devices.json", "r");
    if (!file) {
        Serial.println(" Failed to open devices.json");
        return false;
    }
    
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    
    if (error) {
        Serial.printf(" Failed to parse devices.json: %s\\n", error.c_str());
        return false;
    }
    
    JsonArray devices = doc["devices"].as<JsonArray>();
    int loadedCount = 0;
    int errorCount = 0;
    
    for (JsonObject obj : devices) {
        String macStr = obj["mac"] | "";
        String name = obj["name"] | "";
        uint8_t tankId = obj["tankId"] | 0;
        uint8_t type = obj["type"] | 0;
        
        if (macStr.isEmpty() || name.isEmpty() || tankId == 0) {
            Serial.println("  Skipping invalid device entry");
            errorCount++;
            continue;
        }
        
        // Parse MAC address
        uint8_t mac[6];
        if (sscanf(macStr.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                  &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6) {
            Serial.printf("  Invalid MAC format: %s\\n", macStr.c_str());
            errorCount++;
            continue;
        }
        
        // Get aquarium
        Aquarium* aquarium = AquariumManager::getInstance().getAquarium(tankId);
        if (!aquarium) {
            Serial.printf("  [WARN] Aquarium %d not found for device %s\\n", tankId, name.c_str());
            errorCount++;
            continue;
        }
        
        // Create device object based on type
        NodeType deviceType = static_cast<NodeType>(type);
        Device* device = createDevice(mac, deviceType, name.c_str());
        
        if (!device) {
            Serial.printf("  Failed to create device %s (unknown type)\\n", name.c_str());
            errorCount++;
            continue;
        }
        
        device->setTankId(tankId);
        device->setEnabled(obj["enabled"] | true);
        device->setFirmwareVersion(obj["firmwareVersion"] | 0);
        
        // Add to aquarium
        if (aquarium->addDevice(device)) {
            loadedCount++;
        } else {
            Serial.printf("  Failed to add device %s to aquarium\\n", name.c_str());
            delete device;
            errorCount++;
        }
    }
    
    Serial.printf(" [OK] Loaded %d devices into aquariums (%d errors)\\n", loadedCount, errorCount);
    
    // Print device counts per aquarium
    std::vector<Aquarium*> allAquariums = AquariumManager::getInstance().getAllAquariums();
    for (Aquarium* aquarium : allAquariums) {
        Serial.printf("   - %s: %d devices\\n", aquarium->getName().c_str(), aquarium->getDeviceCount());
    }
    
    return loadedCount > 0;
}

/**
 * @brief Save aquariums to JSON file
 * @return true if saved successfully
 */
bool saveAquariumsToFile() {
    JsonDocument doc;
    JsonArray aquariums = doc["aquariums"].to<JsonArray>();
    
    // Get all aquariums from manager
    std::vector<Aquarium*> allAquariums = AquariumManager::getInstance().getAllAquariums();
    
    for (Aquarium* aquarium : allAquariums) {
        JsonObject obj = aquariums.add<JsonObject>();
        
        // Basic properties
        obj["id"] = aquarium->getId();
        obj["name"] = aquarium->getName();
        obj["volumeLiters"] = aquarium->getVolume();
        obj["tankType"] = aquarium->getTankType();
        obj["location"] = aquarium->getLocation();
        obj["description"] = aquarium->getDescription();
        obj["enabled"] = aquarium->isEnabled();
        obj["maintenanceMode"] = aquarium->isMaintenanceMode();
        
        // Water parameters
        JsonObject waterParams = obj["waterParameters"].to<JsonObject>();
        
        JsonObject temp = waterParams["temperature"].to<JsonObject>();
        temp["min"] = aquarium->getMinTemperature();
        temp["max"] = aquarium->getMaxTemperature();
        
        JsonObject ph = waterParams["ph"].to<JsonObject>();
        ph["min"] = aquarium->getMinPh();
        ph["max"] = aquarium->getMaxPh();
        
        JsonObject tds = waterParams["tds"].to<JsonObject>();
        tds["min"] = aquarium->getMinTds();
        tds["max"] = aquarium->getMaxTds();
        
        // Current readings
        JsonObject currentReadings = obj["currentReadings"].to<JsonObject>();
        currentReadings["temperature"] = aquarium->getCurrentTemperature();
        currentReadings["ph"] = aquarium->getCurrentPh();
        currentReadings["tds"] = aquarium->getCurrentTds();
        currentReadings["lastUpdate"] = aquarium->getLastSensorUpdate();
        
        // Metadata
        obj["createdAt"] = millis(); // Placeholder - should be stored properly
        obj["updatedAt"] = millis();
    }
    
    // Write to file
    File file = FS_USER.open("/config/aquariums.json", "w");
    if (!file) {
        Serial.println(" Failed to open aquariums.json for writing");
        return false;
    }
    
    if (serializeJson(doc, file) == 0) {
        Serial.println(" Failed to write aquariums.json");
        file.close();
        return false;
    }
    
    file.close();
    Serial.println(" Aquariums saved to file");
    return true;
}

/**
 * @brief Get next available aquarium ID
 * @return Next ID (1-255)
 */
uint8_t getNextAquariumId() {
    std::vector<Aquarium*> aquariums = AquariumManager::getInstance().getAllAquariums();
    
    if (aquariums.empty()) {
        return 1;
    }
    
    // Find highest ID and add 1
    uint8_t maxId = 0;
    for (Aquarium* aquarium : aquariums) {
        if (aquarium->getId() > maxId) {
            maxId = aquarium->getId();
        }
    }
    
    if (maxId >= 255) {
        // Find gaps in ID sequence
        for (uint8_t i = 1; i < 255; i++) {
            bool found = false;
            for (Aquarium* aquarium : aquariums) {
                if (aquarium->getId() == i) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                return i;
            }
        }
        return 0; // All IDs used (error)
    }
    
    return maxId + 1;
}

// ============================================================================
// WEB SERVER SETUP
// ============================================================================

void setupWebServer() {
    Serial.println(" Starting web server...");
    
    // ===== IMPORTANT: Register API routes FIRST before static file handlers =====
    // AsyncWebServer processes routes in registration order, so API routes must come first
    // to prevent serveStatic from intercepting /api/* requests
    
    // API endpoints
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request){
        struct tm timeinfo;
        bool hasTime = getLocalTime(&timeinfo);
        
        // Get memory info
        uint32_t heapFree = ESP.getFreeHeap();
        uint32_t heapTotal = ESP.getHeapSize();
        uint32_t psramFree = ESP.getFreePsram();
        uint32_t psramTotal = ESP.getPsramSize();
        
        String json = "{";
        json += "\"uptime\":" + String(millis() / 1000) + ",";
        // Memory object for UI compatibility
        json += "\"memory\":{";
        json += "\"heapFree\":" + String(heapFree) + ",";
        json += "\"heapTotal\":" + String(heapTotal) + ",";
        json += "\"heapUsed\":" + String(heapTotal - heapFree) + ",";
        json += "\"psramFree\":" + String(psramFree) + ",";
        json += "\"psramTotal\":" + String(psramTotal) + ",";
        json += "\"psramUsed\":" + String(psramTotal - psramFree);
        json += "},";
        // Legacy fields for backward compatibility
        json += "\"heap_free\":" + String(heapFree) + ",";
        json += "\"psram_free\":" + String(psramFree) + ",";
        json += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
        json += "\"ntp_synced\":" + String(ntpSynced ? "true" : "false") + ",";
        if (hasTime) {
            char timeBuf[32];
            snprintf(timeBuf, sizeof(timeBuf), "%04d-%02d-%02d %02d:%02d:%02d",
                     timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                     timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
            json += "\"time\":\"" + String(timeBuf) + "\"";
        } else {
            json += "\"time\":null";
        }
        json += "}";
        request->send(200, "application/json", json);
    });

    // GET recent missing assets (for monitoring 404s)
    server.on("/api/missing-assets", HTTP_GET, [](AsyncWebServerRequest *request){
        DynamicJsonDocument doc(2048);
        JsonArray arr = doc.createNestedArray("missing");
        for (const String &entry : recentMissingAssets) {
            arr.add(entry);
        }
        String out;
        serializeJson(doc, out);
        request->send(200, "application/json", out);
    });

    // NOTE: temporary debug endpoints removed for security — use PlatformIO uploadfs or secure shell methods to modify filesystem
    
    server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest *request){
        request->send(200, "text/plain", "Rebooting...");
        delay(1000);
        ESP.restart();
    });
    
    // ===== Aquarium API Endpoints =====
    
    // GET all aquariums
    server.on("/api/aquariums", HTTP_GET, [](AsyncWebServerRequest *request){
        JsonDocument doc;
        JsonArray aquariums = doc["aquariums"].to<JsonArray>();
        
        std::vector<Aquarium*> allAquariums = AquariumManager::getInstance().getAllAquariums();
        
        for (Aquarium* aquarium : allAquariums) {
            JsonObject obj = aquariums.add<JsonObject>();
            obj["id"] = aquarium->getId();
            obj["name"] = aquarium->getName();
            obj["volumeLiters"] = aquarium->getVolume();
            obj["tankType"] = aquarium->getTankType();
            obj["location"] = aquarium->getLocation();
            obj["enabled"] = aquarium->isEnabled();
            obj["maintenanceMode"] = aquarium->isMaintenanceMode();
            // TEMPORARY: Count devices from JSON file since Device objects don't exist yet
            obj["deviceCount"] = countDevicesForAquarium(aquarium->getId());
            
            // Water parameters
            JsonObject waterParams = obj["waterParameters"].to<JsonObject>();
            JsonObject temp = waterParams["temperature"].to<JsonObject>();
            temp["min"] = aquarium->getMinTemperature();
            temp["max"] = aquarium->getMaxTemperature();
            
            JsonObject ph = waterParams["ph"].to<JsonObject>();
            ph["min"] = aquarium->getMinPh();
            ph["max"] = aquarium->getMaxPh();
            
            JsonObject tds = waterParams["tds"].to<JsonObject>();
            tds["min"] = aquarium->getMinTds();
            tds["max"] = aquarium->getMaxTds();
            
            // Current readings
            JsonObject currentReadings = obj["currentReadings"].to<JsonObject>();
            currentReadings["temperature"] = aquarium->getCurrentTemperature();
            currentReadings["ph"] = aquarium->getCurrentPh();
            currentReadings["tds"] = aquarium->getCurrentTds();
        }
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });

    // GET next tasks for dashboard (augment with human-readable names)
    server.on("/api/next-tasks", HTTP_GET, [](AsyncWebServerRequest *request){
        File f = FS_USER.open(NEXT_TASK_FILE, "r");
        if (!f) {
            request->send(200, "application/json", "{\"tasks\":[]}");
            return;
        }
        DynamicJsonDocument doc(16384);
        DeserializationError err = deserializeJson(doc, f);
        f.close();
        if (err) {
            request->send(200, "application/json", "{\"tasks\":[]}");
            return;
        }
        JsonArray arr = doc["tasks"].as<JsonArray>();
        for (JsonObject task : arr) {
            String mac = task["mac"].as<String>();
            if (mac.length() == 0) continue;
            String devName, aqName, devType;
            lookupDeviceAndAquariumNames(mac, devName, aqName, devType);
            task["name"] = devName;
            task["tankName"] = aqName;
            // Compute human-readable description
            String desc = "";
            int type = task["taskType"] | -1;
            if (type == (int)TaskType::LIGHT) {
                int ch = task["channel"] | 0;
                bool on = task["actionOn"] | false;
                desc = "CH" + String(ch) + (on ? " ON" : " OFF");
            } else if (type == (int)TaskType::FEEDER) {
                desc = "Feed";
                if (task.containsKey("pwmValue")) {
                    desc += " PWM " + String(task["pwmValue"].as<int>());
                }
            } else if (type == (int)TaskType::WAVEMAKER) {
                if (task.containsKey("dutyPercent")) {
                    float dp = task["dutyPercent"].as<float>();
                    desc = dp > 0.1 ? "Wavemaker " + String(dp,0) + "%" : "Wavemaker OFF";
                } else {
                    desc = "Wavemaker";
                }
            }
            if (desc.length() == 0) desc = task["scheduleId"].as<String>();
            task["taskDesc"] = desc;
        }
        String out;
        serializeJson(doc, out);
        request->send(200, "application/json", out);
    });
    
    // POST create new aquarium
    server.on("/api/aquariums", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        
        // Parse JSON body
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, (const char*)data, len);
        
        if (error) {
            request->send(400, "text/plain", "Invalid JSON");
            return;
        }
        
        // Validate required fields
        if (!doc["name"].is<String>() || !doc["volumeLiters"].is<float>()) {
            request->send(400, "text/plain", "Missing required fields: name, volumeLiters");
            return;
        }
        
        // Get next available ID
        uint8_t newId = getNextAquariumId();
        if (newId == 0) {
            request->send(507, "text/plain", "No available aquarium IDs (max 255)");
            return;
        }
        
        // Create aquarium
        String name = doc["name"].as<String>();
        Aquarium* aquarium = new Aquarium(newId, name);
        
        // Set properties
        aquarium->setVolume(doc["volumeLiters"] | 0.0f);
        aquarium->setTankType(doc["tankType"] | "");
        aquarium->setLocation(doc["location"] | "");
        aquarium->setDescription(doc["description"] | "");
        
        // Set water parameters
        JsonObject thresholds = doc["thresholds"];
        if (!thresholds.isNull()) {
            aquarium->setTemperatureRange(
                thresholds["temperature"]["min"] | 24.0f,
                thresholds["temperature"]["max"] | 26.0f
            );
            aquarium->setPhRange(
                thresholds["ph"]["min"] | 6.5f,
                thresholds["ph"]["max"] | 7.5f
            );
            aquarium->setTdsRange(
                thresholds["tds"]["min"] | 150,
                thresholds["tds"]["max"] | 300
            );
        }
        
        // Add to manager
        if (!AquariumManager::getInstance().addAquarium(aquarium)) {
            delete aquarium;
            request->send(500, "text/plain", "Failed to add aquarium to manager");
            return;
        }
        
        // Save to file
        if (!saveAquariumsToFile()) {
            Serial.println("  Warning: Failed to save aquariums to file");
        }
        
        // Return success with ID
        JsonDocument responseDoc;
        responseDoc["success"] = true;
        responseDoc["id"] = newId;
        responseDoc["message"] = "Aquarium created successfully";
        
        String response;
        serializeJson(responseDoc, response);
        request->send(201, "application/json", response);
        
        Serial.printf(" Created aquarium: %s (ID: %d)\\n", name.c_str(), newId);
        notifier.emitf("config.aquarium.add", NTFY_MSG_AQUARIUM_CREATED, name.c_str(), newId);
    });
    
    // GET single aquarium
    server.on("^\\/api\\/aquariums\\/([0-9]+)$", HTTP_GET, [](AsyncWebServerRequest *request){
        String idStr = request->pathArg(0);
        uint8_t id = idStr.toInt();
        
        Aquarium* aquarium = AquariumManager::getInstance().getAquarium(id);
        if (!aquarium) {
            request->send(404, "text/plain", "Aquarium not found");
            return;
        }
        
        JsonDocument doc;
        doc["id"] = aquarium->getId();
        doc["name"] = aquarium->getName();
        doc["volumeLiters"] = aquarium->getVolume();
        doc["tankType"] = aquarium->getTankType();
        doc["location"] = aquarium->getLocation();
        doc["description"] = aquarium->getDescription();
        doc["enabled"] = aquarium->isEnabled();
        doc["maintenanceMode"] = aquarium->isMaintenanceMode();
        doc["deviceCount"] = aquarium->getDeviceCount();
        
        // Water parameters
        JsonObject waterParams = doc["waterParameters"].to<JsonObject>();
        JsonObject temp = waterParams["temperature"].to<JsonObject>();
        temp["min"] = aquarium->getMinTemperature();
        temp["max"] = aquarium->getMaxTemperature();
        
        JsonObject ph = waterParams["ph"].to<JsonObject>();
        ph["min"] = aquarium->getMinPh();
        ph["max"] = aquarium->getMaxPh();
        
        JsonObject tds = waterParams["tds"].to<JsonObject>();
        tds["min"] = aquarium->getMinTds();
        tds["max"] = aquarium->getMaxTds();
        
        // Current readings
        JsonObject currentReadings = doc["currentReadings"].to<JsonObject>();
        currentReadings["temperature"] = aquarium->getCurrentTemperature();
        currentReadings["ph"] = aquarium->getCurrentPh();
        currentReadings["tds"] = aquarium->getCurrentTds();
        currentReadings["lastUpdate"] = aquarium->getLastSensorUpdate();
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });
    
    // POST update aquarium (using query parameter)
    server.on("/api/aquarium/update", HTTP_POST, [](AsyncWebServerRequest *request){},
        NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        if (index + len != total) return;  // Wait for full body
        
        if (!request->hasParam("id")) {
            request->send(400, "text/plain", "Missing id parameter");
            return;
        }
        
        uint8_t id = request->getParam("id")->value().toInt();
        
        Aquarium* aquarium = AquariumManager::getInstance().getAquarium(id);
        if (!aquarium) {
            request->send(404, "text/plain", "Aquarium not found");
            return;
        }
        
        // Parse JSON
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, (const char*)data, len);
        
        if (error) {
            request->send(400, "text/plain", "Invalid JSON");
            return;
        }
        
        // Update properties (only if provided)
        if (doc["name"].is<String>()) {
            aquarium->setName(doc["name"].as<String>());
        }
        if (doc["volumeLiters"].is<float>()) {
            aquarium->setVolume(doc["volumeLiters"].as<float>());
        }
        if (doc["tankType"].is<String>()) {
            aquarium->setTankType(doc["tankType"].as<String>());
        }
        if (doc["location"].is<String>()) {
            aquarium->setLocation(doc["location"].as<String>());
        }
        if (doc["description"].is<String>()) {
            aquarium->setDescription(doc["description"].as<String>());
        }
        if (doc["enabled"].is<bool>()) {
            aquarium->setEnabled(doc["enabled"].as<bool>());
        }
        if (doc["maintenanceMode"].is<bool>()) {
            aquarium->setMaintenanceMode(doc["maintenanceMode"].as<bool>());
        }
        
        // Update water parameters if provided
        JsonObject waterParameters = doc["waterParameters"];
        if (!waterParameters.isNull()) {
            JsonObject temp = waterParameters["temperature"];
            if (!temp.isNull()) {
                aquarium->setTemperatureRange(
                    temp["min"].as<float>(),
                    temp["max"].as<float>()
                );
            }
            
            JsonObject ph = waterParameters["ph"];
            if (!ph.isNull()) {
                aquarium->setPhRange(
                    ph["min"].as<float>(),
                    ph["max"].as<float>()
                );
            }
            
            JsonObject tds = waterParameters["tds"];
            if (!tds.isNull()) {
                aquarium->setTdsRange(
                    tds["min"].as<uint16_t>(),
                    tds["max"].as<uint16_t>()
                );
            }
        }
        
        // Save to file
        if (!saveAquariumsToFile()) {
            Serial.println("  Warning: Failed to save aquariums to file");
        }
        
        request->send(200, "text/plain", "Aquarium updated successfully");
        Serial.printf(" Updated aquarium: %s (ID: %d)\\n", aquarium->getName().c_str(), id);
        notifier.emitf("config.aquarium.edit", NTFY_MSG_AQUARIUM_UPDATED, aquarium->getName().c_str(), id);
    });
    
    // ========================================================================
    // MAINTENANCE MODE HELPERS: Save / Restore device states
    // ========================================================================

    // Save current device states before entering maintenance mode
    auto saveMaintenanceStates = [](uint8_t tankId, JsonArray devices) {
        JsonDocument statesDoc;

        // Load existing file (may have states from other aquariums)
        File f = FS_USER.open(MAINTENANCE_STATES_FILE, "r");
        if (f) {
            deserializeJson(statesDoc, f);
            f.close();
        }

        // Ensure the "states" array exists
        if (!statesDoc["states"].is<JsonArray>()) {
            statesDoc["states"].to<JsonArray>();
        }
        JsonArray states = statesDoc["states"].as<JsonArray>();

        // Remove any existing entries for this tank (e.g. re-enable without disable)
        for (int i = states.size() - 1; i >= 0; i--) {
            if (states[i]["aquariumId"].as<uint8_t>() == tankId) {
                states.remove(i);
            }
        }

        // Save current state for each device belonging to this tank
        for (JsonObject dev : devices) {
            if (dev["tankId"].as<uint8_t>() != tankId) continue;

            String macStr = dev["mac"].as<String>();
            String devType = dev["type"].as<String>();
            devType.toUpperCase();

            JsonObject entry = states.add<JsonObject>();
            entry["aquariumId"] = tankId;
            entry["mac"] = macStr;
            entry["type"] = devType;

            String macKey = macStr;
            macKey.toUpperCase();

            if (devType == "LIGHT") {
                if (g_channelStates.find(macKey) != g_channelStates.end()) {
                    ChannelState& cs = g_channelStates[macKey];
                    entry["ch1"] = cs.ch1_on;
                    entry["ch2"] = cs.ch2_on;
                    entry["ch3"] = cs.ch3_on;
                } else {
                    entry["ch1"] = false;
                    entry["ch2"] = false;
                    entry["ch3"] = false;
                }
                Serial.printf("[MAINTENANCE] Saved LIGHT state for %s: ch1=%d ch2=%d ch3=%d\n",
                              macStr.c_str(),
                              entry["ch1"].as<bool>(), entry["ch2"].as<bool>(), entry["ch3"].as<bool>());
            } else if (devType == "WAVE_MAKER") {
                if (g_waveMakerStates.find(macKey) != g_waveMakerStates.end()) {
                    entry["running"] = g_waveMakerStates[macKey].running;
                    entry["dutyPercent"] = g_waveMakerStates[macKey].dutyPercent;
                } else {
                    entry["running"] = false;
                    entry["dutyPercent"] = 0.0f;
                }
                Serial.printf("[MAINTENANCE] Saved WAVEMAKER state for %s: running=%d duty=%.1f%%\n",
                              macStr.c_str(),
                              entry["running"].as<bool>(), entry["dutyPercent"].as<float>());
            } else {
                // CO2, HEATER, etc. - record the entry for completeness
                Serial.printf("[MAINTENANCE] Saved %s entry for %s\n", devType.c_str(), macStr.c_str());
            }
        }

        // Write to file (create if it doesn't exist)
        File wf = FS_USER.open(MAINTENANCE_STATES_FILE, "w");
        if (wf) {
            serializeJson(statesDoc, wf);
            wf.close();
            Serial.printf("[MAINTENANCE] Saved device states for tank %d to %s\n", tankId, MAINTENANCE_STATES_FILE);
        } else {
            Serial.printf("[MAINTENANCE] ERROR: Could not write %s\n", MAINTENANCE_STATES_FILE);
        }
    };

    // Restore device states after exiting maintenance mode, then remove entries
    auto restoreMaintenanceStates = [](uint8_t tankId) {
        File f = FS_USER.open(MAINTENANCE_STATES_FILE, "r");
        if (!f) {
            Serial.printf("[MAINTENANCE] No saved states file found (%s), nothing to restore\n", MAINTENANCE_STATES_FILE);
            return;
        }

        JsonDocument statesDoc;
        DeserializationError err = deserializeJson(statesDoc, f);
        f.close();
        if (err) {
            Serial.printf("[MAINTENANCE] Failed to parse %s: %s\n", MAINTENANCE_STATES_FILE, err.c_str());
            return;
        }

        if (!statesDoc["states"].is<JsonArray>()) {
            Serial.println("[MAINTENANCE] No states array in file");
            return;
        }

        JsonArray states = statesDoc["states"].as<JsonArray>();
        int restoredCount = 0;

        for (JsonObject entry : states) {
            if (entry["aquariumId"].as<uint8_t>() != tankId) continue;

            String macStr = entry["mac"].as<String>();
            String devType = entry["type"].as<String>();

            uint8_t mac[6];
            if (!parseMacAddress(macStr.c_str(), mac)) continue;

            bool isOnline = ESPNowManager::getInstance().isPeerOnline(mac);
            if (!isOnline) {
                Serial.printf("[MAINTENANCE] Restore: %s %s is OFFLINE, skipped\n", devType.c_str(), macStr.c_str());
                continue;
            }

            if (devType == "LIGHT") {
                bool ch1 = entry["ch1"] | false;
                bool ch2 = entry["ch2"] | false;
                bool ch3 = entry["ch3"] | false;

                // Send per-channel commands: CH1=10/11, CH2=20/21, CH3=30/31
                sendLightCommand(mac, ch1 ? 11 : 10);
                sendLightCommand(mac, ch2 ? 21 : 20);
                sendLightCommand(mac, ch3 ? 31 : 30);

                // Restore in-memory channel state tracker
                String macKey = macStr;
                macKey.toUpperCase();
                g_channelStates[macKey] = {ch1, ch2, ch3, -1};

                Serial.printf("[MAINTENANCE] Restored LIGHT %s: ch1=%d ch2=%d ch3=%d\n",
                              macStr.c_str(), ch1, ch2, ch3);
                restoredCount++;

            } else if (devType == "WAVE_MAKER") {
                bool running = entry["running"] | false;
                float duty = entry["dutyPercent"] | 0.0f;

                if (running && duty > 0) {
                    sendWaveMakerCommand(mac, duty);
                    Serial.printf("[MAINTENANCE] Restored WAVEMAKER %s: duty=%.1f%%\n", macStr.c_str(), duty);
                } else {
                    sendWaveMakerStop(mac);
                    Serial.printf("[MAINTENANCE] Restored WAVEMAKER %s: STOPPED\n", macStr.c_str());
                }

                // Restore in-memory state tracker
                String macKey = macStr;
                macKey.toUpperCase();
                g_waveMakerStates[macKey] = {running, duty};
                restoredCount++;

            } else if (devType == "CO2" || devType == "HEATER") {
                // CO2 and heater will resume via the scheduler on the next scheduled event.
                // We don't send restore commands here to avoid conflicting with schedule logic.
                Serial.printf("[MAINTENANCE] %s %s will resume via scheduler\n", devType.c_str(), macStr.c_str());
            }
        }

        // Remove entries for this tank from the file
        for (int i = states.size() - 1; i >= 0; i--) {
            if (states[i]["aquariumId"].as<uint8_t>() == tankId) {
                states.remove(i);
            }
        }

        // Write updated file back (keeps entries from other tanks, creates file if needed)
        File wf = FS_USER.open(MAINTENANCE_STATES_FILE, "w");
        if (wf) {
            serializeJson(statesDoc, wf);
            wf.close();
            Serial.printf("[MAINTENANCE] Removed saved states for tank %d (%d devices restored)\n", tankId, restoredCount);
        } else {
            Serial.printf("[MAINTENANCE] ERROR: Could not write %s\n", MAINTENANCE_STATES_FILE);
        }
    };

    // POST toggle maintenance mode for an aquarium
    server.on("/api/aquarium/maintenance", HTTP_POST, [saveMaintenanceStates, restoreMaintenanceStates](AsyncWebServerRequest *request){},
        NULL,
        [saveMaintenanceStates, restoreMaintenanceStates](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        if (index + len != total) return;  // Wait for full body
        
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, (const char*)data, len);
        if (error) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON\"}");
            return;
        }
        
        if (!doc["id"].is<int>() || !doc["maintenanceMode"].is<bool>()) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing id or maintenanceMode\"}");
            return;
        }
        
        uint8_t tankId = doc["id"].as<uint8_t>();
        bool enabled = doc["maintenanceMode"].as<bool>();
        
        Aquarium* aquarium = AquariumManager::getInstance().getAquarium(tankId);
        if (!aquarium) {
            request->send(404, "application/json", "{\"success\":false,\"error\":\"Aquarium not found\"}");
            return;
        }
        
        aquarium->setMaintenanceMode(enabled);
        
        // Save to file
        saveAquariumsToFile();
        
        // Load devices.json to find devices belonging to this aquarium
        File devFile = FS_USER.open("/config/devices.json", "r");
        if (devFile) {
            JsonDocument devDoc;
            DeserializationError devErr = deserializeJson(devDoc, devFile);
            devFile.close();
            
            if (!devErr) {
                JsonArray devices = devDoc["devices"].as<JsonArray>();

                if (enabled) {
                    // ── MAINTENANCE ON ──────────────────────────────────
                    // 1) Save current device states BEFORE overriding
                    saveMaintenanceStates(tankId, devices);

                    // 2) Override: lights ON (all channels), stop everything else
                    for (JsonObject dev : devices) {
                        if (dev["tankId"].as<uint8_t>() != tankId) continue;

                        String macStr = dev["mac"].as<String>();
                        String devType = dev["type"].as<String>();
                        devType.toUpperCase();

                        uint8_t mac[6];
                        if (!parseMacAddress(macStr.c_str(), mac)) continue;

                        bool isOnline = ESPNowManager::getInstance().isPeerOnline(mac);

                        if (devType == "LIGHT") {
                            if (isOnline) {
                                sendLightCommand(mac, 1);  // Command 1 = All channels ON
                                Serial.printf("[MAINTENANCE] Tank %d: Lights ON for %s\n", tankId, macStr.c_str());
                            } else {
                                Serial.printf("[MAINTENANCE] Tank %d: Light %s OFFLINE, skipped\n", tankId, macStr.c_str());
                            }
                        } else if (devType == "WAVE_MAKER") {
                            if (isOnline) {
                                sendWaveMakerStop(mac);
                                Serial.printf("[MAINTENANCE] Tank %d: WaveMaker STOPPED for %s\n", tankId, macStr.c_str());
                            }
                        } else if (devType == "CO2") {
                            if (isOnline) {
                                CommandMessage cmd;
                                memset(&cmd, 0, sizeof(cmd));
                                cmd.header.type = MessageType::COMMAND;
                                cmd.header.tankId = tankId;
                                cmd.header.nodeType = NodeType::HUB;
                                cmd.header.timestamp = millis();
                                cmd.commandId = millis() & 0xFF;
                                cmd.commandSeqID = 0;
                                cmd.finalCommand = true;
                                cmd.commandData[0] = 0x02;  // CMD_STOP for CO2
                                ESPNowManager::getInstance().send(mac, (uint8_t*)&cmd, sizeof(cmd), false);
                                Serial.printf("[MAINTENANCE] Tank %d: CO2 OFF for %s\n", tankId, macStr.c_str());
                            }
                        } else if (devType == "HEATER") {
                            if (isOnline) {
                                CommandMessage cmd;
                                memset(&cmd, 0, sizeof(cmd));
                                cmd.header.type = MessageType::COMMAND;
                                cmd.header.tankId = tankId;
                                cmd.header.nodeType = NodeType::HUB;
                                cmd.header.timestamp = millis();
                                cmd.commandId = millis() & 0xFF;
                                cmd.commandSeqID = 0;
                                cmd.finalCommand = true;
                                cmd.commandData[0] = 0x02;  // CMD_SET_MODE = manual
                                cmd.commandData[1] = 0;     // mode = manual
                                ESPNowManager::getInstance().send(mac, (uint8_t*)&cmd, sizeof(cmd), false);
                                Serial.printf("[MAINTENANCE] Tank %d: Heater OFF for %s\n", tankId, macStr.c_str());
                            }
                        }
                        // FISH_FEEDER / SENSOR / REPEATER: no action needed
                    }
                } else {
                    // ── MAINTENANCE OFF ─────────────────────────────────
                    // Restore saved device states from maintenance-states.json
                    restoreMaintenanceStates(tankId);
                }
            }
        }
        
        Serial.printf("[MAINTENANCE] Tank %d maintenance mode %s\n", tankId, enabled ? "ENABLED" : "DISABLED");
        
        JsonDocument respDoc;
        respDoc["success"] = true;
        respDoc["maintenanceMode"] = enabled;
        respDoc["message"] = enabled ? "Maintenance mode enabled - lights ON, other devices stopped" 
                                     : "Maintenance mode disabled - devices restored to previous state";
        String response;
        serializeJson(respDoc, response);
        request->send(200, "application/json", response);
    });
    
    // POST delete aquarium (using query parameter)
    server.on("/api/aquarium/delete", HTTP_POST, [](AsyncWebServerRequest *request){
        if (!request->hasParam("id")) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing id parameter\"}");
            return;
        }

        uint8_t id = request->getParam("id")->value().toInt();
        Serial.printf("Request: delete aquarium %d\n", id);

        // 1) Find all provisioned devices that belong to this aquarium (from devices.json)
        DynamicJsonDocument devicesDoc(8192);
        File devicesFile = FS_USER.open("/config/devices.json", "r");
        std::vector<String> devicesToRemove;
        if (devicesFile) {
            DeserializationError derr = deserializeJson(devicesDoc, devicesFile);
            devicesFile.close();
            if (!derr) {
                JsonArray devices = devicesDoc["devices"].as<JsonArray>();
                for (JsonObject d : devices) {
                    uint8_t tank = d["tankId"] | 0;
                    if (tank == id) {
                        String mac = d["mac"].as<String>();
                        if (mac.length()) devicesToRemove.push_back(mac);
                    }
                }
            }
        }

        // 2) Remove schedules (feeder & light) that reference this aquarium or its devices
        auto removeSchedulesForTank = [&](const char* path) -> bool {
            File f = FS_USER.open(path, "r");
            if (!f) return true; // missing file -> nothing to do
            DynamicJsonDocument doc(8192);
            DeserializationError err = deserializeJson(doc, f);
            f.close();
            if (err) {
                Serial.printf("[AUDIT] Failed to parse %s: %s\n", path, err.c_str());
                return false;
            }
            JsonArray schedules = doc["schedules"].as<JsonArray>();
            if (schedules.isNull()) return true;

            for (int i = (int)schedules.size() - 1; i >= 0; --i) {
                JsonObject s = schedules[i];
                // Match by explicit tankId or by device mac
                if (s.containsKey("tankId") && (uint8_t)(s["tankId"].as<uint8_t>()) == id) {
                    schedules.remove(i);
                    continue;
                }
                if (s.containsKey("mac")) {
                    String mac = s["mac"].as<String>();
                    // If mac belongs to this tank, remove
                    for (auto &m : devicesToRemove) {
                        if (mac.equalsIgnoreCase(m)) {
                            schedules.remove(i);
                            break;
                        }
                    }
                }
            }

            // Write back
            File out = FS_USER.open(path, "w");
            if (!out) {
                Serial.printf("[AUDIT] Failed to open %s for write\n", path);
                return false;
            }
            serializeJson(doc, out);
            out.close();
            return true;
        };

        bool ok = true;
        ok &= removeSchedulesForTank("/config/schedule/feeder-schedule.json");
        ok &= removeSchedulesForTank("/config/schedule/light-schedule.json");
        ok &= removeSchedulesForTank("/config/schedule/wm-schedule.json");

        if (!ok) {
            request->send(500, "application/json", "{\"success\":false,\"error\":\"Failed to prune schedules\"}");
            return;
        }

        // 3) Remove devices (JSON + in-memory + peers) belonging to this aquarium
        if (!devicesFile) {
            // reload if needed
            devicesFile = FS_USER.open("/config/devices.json", "r");
        }
        DynamicJsonDocument devicesDoc2(8192);
        if (devicesFile) {
            deserializeJson(devicesDoc2, devicesFile);
            devicesFile.close();
        }
        JsonArray devicesArr = devicesDoc2["devices"].as<JsonArray>();
        if (!devicesArr.isNull()) {
            for (int i = (int)devicesArr.size() - 1; i >= 0; --i) {
                JsonObject d = devicesArr[i];
                uint8_t tank = d["tankId"] | 0;
                if (tank == id) {
                    String mac = d["mac"].as<String>();

                    // Remove from light-devices and schedules (reuse delete-device logic)
                    // Remove from devicesArr
                    devicesArr.remove(i);

                    // Convert mac to bytes and remove from in-memory registry
                    uint8_t macBytes[6] = {0};
                    if (sscanf(mac.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                               &macBytes[0], &macBytes[1], &macBytes[2], &macBytes[3], &macBytes[4], &macBytes[5]) == 6) {
                        AquariumManager::getInstance().removeDevice(macBytes);
                        ESPNowManager::getInstance().removePeer(macBytes);
                    }
                }
            }

            // Persist devices.json
            File outDev = FS_USER.open("/config/devices.json", "w");
            if (!outDev) {
                request->send(500, "application/json", "{\"success\":false,\"error\":\"Failed to write devices.json\"}");
                return;
            }
            serializeJson(devicesDoc2, outDev);
            outDev.close();
        }

        // 4) Rebuild next-task.json from remaining schedules to ensure no tasks remain for this tank
        rebuildNextTasks();

        // 5) Finally remove aquarium from manager and persist
        if (!AquariumManager::getInstance().removeAquarium(id)) {
            request->send(404, "application/json", "{\"success\":false,\"error\":\"Aquarium not found\"}");
            return;
        }

        saveAquariumsToFile();

        request->send(200, "application/json", "{\"success\":true}");
        Serial.printf(" Deleted aquarium ID: %d (cascaded: schedules/devices/next-tasks)\n", id);
        notifier.emitf("config.aquarium.delete", NTFY_MSG_AQUARIUM_DELETED, (int)id);
    });
    
    // GET unmapped devices
    server.on("/api/unmapped-devices", HTTP_GET, [](AsyncWebServerRequest *request){
        File file = FS_USER.open("/config/unmapped-devices.json", "r");
        if (!file) {
            // Return empty list if file doesn't exist
            request->send(200, "application/json", "{\"unmappedDevices\":[]}");
            return;
        }
        
        String jsonData = file.readString();
        file.close();
        
        request->send(200, "application/json", jsonData);
    });
    
    // GET all devices
    server.on("/api/devices", HTTP_GET, [](AsyncWebServerRequest *request){
        File file = FS_USER.open("/config/devices.json", "r");
        if (!file) {
            // Return empty list if file doesn't exist
            request->send(200, "application/json", "{\"devices\":[]}");
            return;
        }
        
        DynamicJsonDocument doc(8192);
        DeserializationError error = deserializeJson(doc, file);
        file.close();

        if (error) {
            Serial.printf(" Failed to parse devices.json: %s\n", error.c_str());
            request->send(500, "application/json", "{\"devices\":[]}");
            return;
        }

        JsonArray devices = doc["devices"].as<JsonArray>();
        for (JsonObject device : devices) {
            String macStr = device["mac"] | "";
            bool online = false;

            if (macStr.length() > 0) {
                uint8_t mac[6];
                if (sscanf(macStr.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                          &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6) {
                    online = ESPNowManager::getInstance().isPeerOnline(mac);
                }
            }

            device["online"] = online;
        }

        String jsonData;
        serializeJson(doc, jsonData);
        request->send(200, "application/json", jsonData);
    });

    // TEST endpoint: force a device into fail-safe (TEST MODE MUST BE ENABLED)
    server.on("/api/test/force-failsafe", HTTP_POST, [](AsyncWebServerRequest *request){
        if (!config.hubTestMode) {
            request->send(403, "application/json", "{\"success\":false,\"error\":\"Test mode disabled\"}");
            return;
        }

        // Accept mac param in POST body (form-data) or as query param
        String macStr;
        if (request->hasParam("mac", true)) {
            macStr = request->getParam("mac", true)->value();
        } else if (request->hasParam("mac")) {
            macStr = request->getParam("mac")->value();
        } else {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing mac parameter\"}");
            return;
        }

        uint8_t mac[6];
        if (sscanf(macStr.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                   &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid mac format\"}");
            return;
        }

        Device* device = AquariumManager::getInstance().getDevice(mac);
        if (!device) {
            request->send(404, "application/json", "{\"success\":false,\"error\":\"Device not found\"}");
            return;
        }

        uint8_t cmd[1] = { 0xFF }; // 0xFF reserved for FORCE_FAILSAFE test command
        bool ok = device->sendCommand(cmd, 1);
        if (!ok) {
            request->send(500, "application/json", "{\"success\":false,\"error\":\"Failed to send command\"}");
            return;
        }

        request->send(200, "application/json", "{\"success\":true,\"message\":\"Force-failsafe command sent\"}");
    });

    // GET light schedule for a device
    server.on("/api/light-schedule", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!request->hasParam("mac")) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing mac\"}");
            return;
        }

        String macStr = request->getParam("mac")->value();
        File file = FS_USER.open("/config/schedule/light-schedule.json", "r");
        if (!file) {
            request->send(200, "application/json", "{\"success\":true,\"schedule\":null}");
            return;
        }

        DynamicJsonDocument doc(4096);
        DeserializationError error = deserializeJson(doc, file);
        file.close();
        if (error) {
            request->send(500, "application/json", "{\"success\":false,\"error\":\"Failed to parse light-schedule.json\"}");
            return;
        }

        JsonArray schedules = doc["schedules"].as<JsonArray>();
        JsonObject found;
        for (JsonObject entry : schedules) {
            String entryMac = entry["mac"] | "";
            if (entryMac.equalsIgnoreCase(macStr)) {
                found = entry;
                break;
            }
        }

        DynamicJsonDocument responseDoc(2048);
        responseDoc["success"] = true;

        if (!found.isNull()) {
            JsonObject responseSchedule = responseDoc.createNestedObject("schedule");
            responseSchedule["mac"] = found["mac"] | "";
            responseSchedule["tankId"] = found["tankId"] | 0;
            responseSchedule["deviceName"] = found["deviceName"] | "";

            JsonObject sched = responseSchedule.createNestedObject("schedule");
            JsonObject storedSched = found["schedule"].as<JsonObject>();

            auto buildUiSection = [&](const char* key) {
                if (storedSched.isNull() || !storedSched.containsKey(key)) {
                    return;
                }

                JsonObject section = storedSched[key].as<JsonObject>();
                JsonObject outSection = sched.createNestedObject(key);

                if (section.containsKey("channel1")) {
                    JsonObject ch1 = section["channel1"].as<JsonObject>();
                    JsonObject ch2 = section["channel2"].as<JsonObject>();
                    JsonObject ch3 = section["channel3"].as<JsonObject>();

                    int sh = ch1["start"]["hour"] | 0;
                    int sm = ch1["start"]["minute"] | 0;
                    int oh = ch1["offTime"]["hour"] | 0;
                    int om = ch1["offTime"]["minute"] | 0;

                    int startMinutes = (sh * 60 + sm) % (24*60);
                    int offMinutes = (oh * 60 + om) % (24*60);
                    int diff = (offMinutes - startMinutes + 24*60) % (24*60);

                    JsonObject start = outSection.createNestedObject("start");
                    start["hour"] = (startMinutes / 60) % 24;
                    start["minute"] = startMinutes % 60;

                    JsonObject duration = outSection.createNestedObject("duration");
                    duration["hour"] = diff / 60;
                    duration["minute"] = diff % 60;

                    bool ramp = false;
                    if (!ch2.isNull() && !ch3.isNull()) {
                        int ch1Start = startMinutes;
                        int ch1Off = offMinutes;
                        int ch2Start = ((ch2["start"]["hour"] | 0) * 60 + (ch2["start"]["minute"] | 0)) % (24*60);
                        int ch2Off = ((ch2["offTime"]["hour"] | 0) * 60 + (ch2["offTime"]["minute"] | 0)) % (24*60);
                        int ch3Start = ((ch3["start"]["hour"] | 0) * 60 + (ch3["start"]["minute"] | 0)) % (24*60);
                        int ch3Off = ((ch3["offTime"]["hour"] | 0) * 60 + (ch3["offTime"]["minute"] | 0)) % (24*60);

                        bool ch2Matches = (ch2Start == (ch1Start + 5) % (24*60)) &&
                                          (ch2Off == (ch1Off - 5 + 24*60) % (24*60));
                        bool ch3Matches = (ch3Start == (ch2Start + 5) % (24*60)) &&
                                          (ch3Off == (ch2Off - 5 + 24*60) % (24*60));
                        ramp = ch2Matches && ch3Matches;
                    }

                    outSection["ramp"] = ramp;
                } else if (section.containsKey("start") && section.containsKey("offTime")) {
                    JsonObject s = section["start"].as<JsonObject>();
                    JsonObject o = section["offTime"].as<JsonObject>();

                    int sh = s["hour"] | 0;
                    int sm = s["minute"] | 0;
                    int oh = o["hour"] | 0;
                    int om = o["minute"] | 0;

                    int startMinutes = (sh * 60 + sm) % (24*60);
                    int offMinutes = (oh * 60 + om) % (24*60);
                    int diff = (offMinutes - startMinutes + 24*60) % (24*60);

                    JsonObject start = outSection.createNestedObject("start");
                    start["hour"] = (startMinutes / 60) % 24;
                    start["minute"] = startMinutes % 60;

                    JsonObject duration = outSection.createNestedObject("duration");
                    duration["hour"] = diff / 60;
                    duration["minute"] = diff % 60;

                    outSection["ramp"] = section["ramp"] | false;
                }
            };

            buildUiSection("morning");
            buildUiSection("evening");
        } else {
            responseDoc["schedule"] = nullptr;
        }

        String response;
        serializeJson(responseDoc, response);
        request->send(200, "application/json", response);
    });

    // POST save light schedule for a device
    server.on("/api/light-schedule", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        if (index + len != total) return;

        DynamicJsonDocument body(4096);
        DeserializationError error = deserializeJson(body, data, len);
        if (error) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON\"}");
            return;
        }

        String macStr = body["mac"] | "";
        if (macStr.length() == 0) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing mac\"}");
            return;
        }

        // Backwards-compatible delete-in-post support: { mac, action: "delete" }
        if (body.containsKey("action") && String(body["action"] | "") == "delete") {
            File file = FS_USER.open("/config/schedule/light-schedule.json", "r");
            DynamicJsonDocument doc(4096);
            bool changed = false;

            if (file) {
                DeserializationError err2 = deserializeJson(doc, file);
                file.close();
                if (!err2) {
                    JsonArray schedules = doc["schedules"].as<JsonArray>();
                    if (!schedules.isNull()) {
                        for (int i = (int)schedules.size() - 1; i >= 0; i--) {
                            String entryMac = schedules[i]["mac"] | "";
                            if (entryMac.equalsIgnoreCase(macStr)) {
                                schedules.remove(i);
                                changed = true;
                            }
                        }
                    }
                }
            }

            if (changed) {
                File out = FS_USER.open("/config/schedule/light-schedule.json", "w");
                if (!out) {
                    request->send(500, "application/json", "{\"success\":false,\"error\":\"Failed to write light-schedule.json\"}");
                    return;
                }
                serializeJson(doc, out);
                out.close();
            }

            // Rebuild next-task.json with updated schedules
            rebuildNextTasks();
            request->send(200, "application/json", "{\"success\":true}");
            return;
        }

        JsonVariant schedule = body["schedule"];

        File file = FS_USER.open("/config/schedule/light-schedule.json", "r");
        DynamicJsonDocument doc(4096);
        if (file) {
            deserializeJson(doc, file);
            file.close();
        }

        JsonArray schedules = doc["schedules"];
        if (schedules.isNull()) {
            schedules = doc.createNestedArray("schedules");
        }

        // Remove existing entry for this MAC
        for (int i = (int)schedules.size() - 1; i >= 0; i--) {
            String entryMac = schedules[i]["mac"] | "";
            if (entryMac.equalsIgnoreCase(macStr)) {
                schedules.remove(i);
            }
        }

        JsonObject newEntry = schedules.createNestedObject();
        newEntry["mac"] = macStr;
        newEntry["tankId"] = body["tankId"] | 0;
        newEntry["deviceName"] = body["deviceName"] | "";

        JsonObject newSched = newEntry.createNestedObject("schedule");
        const char* periods[] = {"morning","evening"};

        for (const char* p : periods) {
            if (!schedule.containsKey(p)) {
                continue;
            }

            JsonObject sec = schedule[p].as<JsonObject>();
            JsonObject dest = newSched.createNestedObject(p);

            JsonObject s = sec["start"].as<JsonObject>();
            JsonObject d = sec["duration"].as<JsonObject>();
            bool ramp = sec["ramp"] | false;

            int sh = s["hour"] | 0;
            int sm = s["minute"] | 0;
            int dh = d["hour"] | 0;
            int dm = d["minute"] | 0;

            sh = ((sh % 24) + 24) % 24;
            sm = ((sm % 60) + 60) % 60;
            dh = max(0, dh);
            dm = ((dm % 60) + 60) % 60;

            int startMinutes = sh * 60 + sm;
            int offMinutes = (startMinutes + dh * 60 + dm) % (24*60);

            auto setChannel = [&](JsonObject channel, int startMin, int offMin) {
                JsonObject startObj = channel.createNestedObject("start");
                startObj["hour"] = (startMin / 60) % 24;
                startObj["minute"] = startMin % 60;
                JsonObject offObj = channel.createNestedObject("offTime");
                offObj["hour"] = (offMin / 60) % 24;
                offObj["minute"] = offMin % 60;
            };

            JsonObject ch1 = dest.createNestedObject("channel1");
            JsonObject ch2 = dest.createNestedObject("channel2");
            JsonObject ch3 = dest.createNestedObject("channel3");

            if (!ramp) {
                setChannel(ch1, startMinutes, offMinutes);
                setChannel(ch2, startMinutes, offMinutes);
                setChannel(ch3, startMinutes, offMinutes);
            } else {
                int ch1Start = startMinutes;
                int ch1Off = offMinutes;
                int ch2Start = (ch1Start + 5) % (24*60);
                int ch2Off = (ch1Off - 5 + 24*60) % (24*60);
                int ch3Start = (ch2Start + 5) % (24*60);
                int ch3Off = (ch2Off - 5 + 24*60) % (24*60);

                setChannel(ch1, ch1Start, ch1Off);
                setChannel(ch2, ch2Start, ch2Off);
                setChannel(ch3, ch3Start, ch3Off);
            }
        }

        newEntry["updatedAt"] = millis();

        file = FS_USER.open("/config/schedule/light-schedule.json", "w");
        if (!file) {
            request->send(500, "application/json", "{\"success\":false,\"error\":\"Failed to write light-schedule.json\"}");
            return;
        }
        serializeJson(doc, file);
        file.close();

        // Rebuild next-task.json with updated schedule
        rebuildNextTasks();

        request->send(200, "application/json", "{\"success\":true}");
    });

    // POST delete light schedule for a device
    server.on("/api/light-schedule/delete", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        if (index + len != total) return;

        DynamicJsonDocument body(512);
        DeserializationError error = deserializeJson(body, data, len);
        if (error) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON\"}");
            return;
        }

        String macStr = body["mac"] | "";
        if (macStr.length() == 0) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing mac\"}");
            return;
        }

        File file = FS_USER.open("/config/schedule/light-schedule.json", "r");
        DynamicJsonDocument doc(4096);
        bool changed = false;

        if (file) {
            DeserializationError err2 = deserializeJson(doc, file);
            file.close();
            if (err2) {
                request->send(500, "application/json", "{\"success\":false,\"error\":\"Failed to parse light-schedule.json\"}");
                return;
            }

            JsonArray schedules = doc["schedules"].as<JsonArray>();
            if (!schedules.isNull()) {
                for (int i = (int)schedules.size() - 1; i >= 0; i--) {
                    String entryMac = schedules[i]["mac"] | "";
                    if (entryMac.equalsIgnoreCase(macStr)) {
                        schedules.remove(i);
                        changed = true;
                    }
                }
            }
        }

        if (changed) {
            File out = FS_USER.open("/config/schedule/light-schedule.json", "w");
            if (!out) {
                request->send(500, "application/json", "{\"success\":false,\"error\":\"Failed to write light-schedule.json\"}");
                return;
            }
            serializeJson(doc, out);
            out.close();
        }

        // Rebuild next-task.json to remove any scheduled tasks for this device
        rebuildNextTasks();

        request->send(200, "application/json", "{\"success\":true}");
    });

    // ========================================================================
    // FEEDER CALIBRATION API
    // ========================================================================

    // GET feeder calibration for a device
    server.on("/api/feeder-calibration", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!request->hasParam("mac")) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing mac\"}");
            return;
        }

        String macStr = request->getParam("mac")->value();
        File file = FS_USER.open("/config/feeder-calibration.json", "r");
        if (!file) {
            request->send(200, "application/json", "{\"success\":true,\"calibration\":null}");
            return;
        }

        DynamicJsonDocument doc(2048);
        DeserializationError error = deserializeJson(doc, file);
        file.close();
        if (error) {
            request->send(500, "application/json", "{\"success\":false,\"error\":\"Failed to parse feeder-calibration.json\"}");
            return;
        }

        JsonArray calibrations = doc["calibrations"].as<JsonArray>();
        JsonObject found;
        for (JsonObject entry : calibrations) {
            String entryMac = entry["mac"] | "";
            if (entryMac.equalsIgnoreCase(macStr)) {
                found = entry;
                break;
            }
        }

        DynamicJsonDocument responseDoc(512);
        responseDoc["success"] = true;

        if (!found.isNull()) {
            JsonObject cal = responseDoc.createNestedObject("calibration");
            cal["dutyCycle"] = found["calibration"]["dutyCycle"] | 72;
            cal["pulseDuration"] = found["calibration"]["pulseDuration"] | 160;
        } else {
            responseDoc["calibration"] = nullptr;
        }

        String response;
        serializeJson(responseDoc, response);
        request->send(200, "application/json", response);
    });

    // POST save feeder calibration
    server.on("/api/feeder-calibration", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        if (index + len != total) return;

        DynamicJsonDocument body(1024);
        DeserializationError error = deserializeJson(body, data, len);
        if (error) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON\"}");
            return;
        }

        String macStr = body["mac"] | "";
        if (macStr.length() == 0) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing mac\"}");
            return;
        }

        // If /api/feeder-calibration/test is routed here, handle test payloads
        String action = body["action"] | "";
        JsonVariant calibration = body["calibration"];
        bool hasDirectParams = body.containsKey("dutyCycle") || body.containsKey("pulseDuration");
        if (action == "test" || (calibration.isNull() && hasDirectParams)) {
            uint16_t pwmValue = 72;
            uint32_t durationMs = 160;
            if (!calibration.isNull()) {
                pwmValue = calibration["dutyCycle"] | pwmValue;
                durationMs = calibration["pulseDuration"] | durationMs;
            } else {
                pwmValue = body["dutyCycle"] | pwmValue;
                durationMs = body["pulseDuration"] | durationMs;
            }

            uint8_t mac[6];
            if (!parseMacAddress(macStr.c_str(), mac)) {
                request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid MAC address\"}");
                return;
            }

            sendFeederCommand(mac, pwmValue, durationMs);

            // Activity log
            {
                String devName, aqName, devType;
                lookupDeviceAndAquariumNames(macStr, devName, aqName, devType);
                char desc[64];
                snprintf(desc, sizeof(desc), "Test feed (PWM %u, %ums)", pwmValue, durationMs);
                appendActivityLog("adhoc", "feeder", desc, devName.c_str(), aqName.c_str());
            }

            DynamicJsonDocument resp(256);
            resp["success"] = true;
            resp["dutyCycle"] = pwmValue;
            resp["pulseDuration"] = durationMs;
            String response;
            serializeJson(resp, response);
            request->send(200, "application/json", response);
            return;
        }

        calibration = body["calibration"];
        if (calibration.isNull()) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing calibration\"}");
            return;
        }

        File file = FS_USER.open("/config/feeder-calibration.json", "r");
        DynamicJsonDocument doc(2048);
        if (file) {
            deserializeJson(doc, file);
            file.close();
        }

        JsonArray calibrations = doc["calibrations"];
        if (calibrations.isNull()) {
            calibrations = doc.createNestedArray("calibrations");
        }

        // Remove existing entry for this MAC
        for (int i = (int)calibrations.size() - 1; i >= 0; i--) {
            String entryMac = calibrations[i]["mac"] | "";
            if (entryMac.equalsIgnoreCase(macStr)) {
                calibrations.remove(i);
            }
        }

        JsonObject newEntry = calibrations.createNestedObject();
        newEntry["mac"] = macStr;
        newEntry["tankId"] = body["tankId"] | 0;
        newEntry["deviceName"] = body["deviceName"] | "";

        JsonObject newCal = newEntry.createNestedObject("calibration");
        newCal["dutyCycle"] = calibration["dutyCycle"] | 72;
        newCal["pulseDuration"] = calibration["pulseDuration"] | 160;
        newEntry["updatedAt"] = millis();

        file = FS_USER.open("/config/feeder-calibration.json", "w");
        if (!file) {
            request->send(500, "application/json", "{\"success\":false,\"error\":\"Failed to write feeder-calibration.json\"}");
            return;
        }
        serializeJson(doc, file);
        file.close();

        request->send(200, "application/json", "{\"success\":true}");
    });

    // POST test feeder calibration (sends a single test feed via ESP-NOW)
    server.on("/api/feeder-calibration/test", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        if (index + len != total) return;

        DynamicJsonDocument body(512);
        DeserializationError error = deserializeJson(body, data, len);
        if (error) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON\"}");
            return;
        }

        String macStr = body["mac"] | "";
        if (macStr.length() == 0) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing mac\"}");
            return;
        }

        uint16_t pwmValue = body["dutyCycle"] | 72;
        uint32_t durationMs = body["pulseDuration"] | 160;

        uint8_t mac[6];
        if (!parseMacAddress(macStr.c_str(), mac)) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid MAC address\"}");
            return;
        }

        sendFeederCommand(mac, pwmValue, durationMs);

        // Activity log
        {
            String devName, aqName, devType;
            lookupDeviceAndAquariumNames(macStr, devName, aqName, devType);
            char desc[64];
            snprintf(desc, sizeof(desc), "Test feed (PWM %u, %ums)", pwmValue, durationMs);
            appendActivityLog("adhoc", "feeder", desc, devName.c_str(), aqName.c_str());
        }

        DynamicJsonDocument resp(256);
        resp["success"] = true;
        resp["dutyCycle"] = pwmValue;
        resp["pulseDuration"] = durationMs;
        String response;
        serializeJson(resp, response);
        request->send(200, "application/json", response);
    });

    // ========================================================================
    // WAVE MAKER API
    // ========================================================================

    // GET wave maker config for a specific MAC
    // Query params: mac
    server.on("/api/wavemaker-config", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!request->hasParam("mac")) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing mac\"}");
            return;
        }

        String macStr = request->getParam("mac")->value();
        float maxDuty, minDuty, defaultDuty;
        bool found = loadWaveMakerConfig(macStr, maxDuty, minDuty, defaultDuty);

        DynamicJsonDocument resp(512);
        resp["success"] = true;
        resp["mac"] = macStr;
        resp["found"] = found;
        resp["maxDutyPercent"] = maxDuty;
        resp["minDutyPercent"] = minDuty;
        resp["defaultDuty"] = defaultDuty;
        resp["pwmFrequency"] = 120;
        resp["pwmResolution"] = 10;
        resp["softStart"] = false;

        String response;
        serializeJson(resp, response);
        request->send(200, "application/json", response);
    });

    // POST wave maker command (STOP or PWM with duty cycle)
    // Body JSON: {"mac":"AA:BB:CC:DD:EE:FF", "command":"PWM", "dutyPercent": 65.0}
    //        or: {"mac":"AA:BB:CC:DD:EE:FF", "command":"STOP"}
    server.on("/api/wavemaker-command", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        if (index + len != total) return;

        DynamicJsonDocument body(512);
        DeserializationError error = deserializeJson(body, data, len);
        if (error) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON\"}");
            return;
        }

        String macStr = body["mac"] | "";
        String command = body["command"] | "";
        if (macStr.length() == 0 || command.length() == 0) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing mac or command\"}");
            return;
        }

        uint8_t mac[6];
        if (!parseMacAddress(macStr.c_str(), mac)) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid MAC address\"}");
            return;
        }

        if (command.equalsIgnoreCase("STOP")) {
            sendWaveMakerStop(mac);

            // Track state
            String wmKey = macStr;
            wmKey.toUpperCase();
            g_waveMakerStates[wmKey] = {false, 0.0f};

            // Activity log
            String devName, aqName, devType;
            lookupDeviceAndAquariumNames(macStr, devName, aqName, devType);
            appendActivityLog("adhoc", "wavemaker", "STOP", devName.c_str(), aqName.c_str());

            DynamicJsonDocument resp(256);
            resp["success"] = true;
            resp["command"] = "STOP";
            resp["mac"] = macStr;
            String response;
            serializeJson(resp, response);
            request->send(200, "application/json", response);
        } else if (command.equalsIgnoreCase("PWM")) {
            float dutyPercent = body["dutyPercent"] | 0.0f;

            // Load config to validate against min/max
            float maxDuty, minDuty, defaultDuty;
            loadWaveMakerConfig(macStr, maxDuty, minDuty, defaultDuty);

            if (dutyPercent <= 0.0f) {
                dutyPercent = defaultDuty;  // Use default from config
            }
            if (dutyPercent < minDuty) dutyPercent = minDuty;
            if (dutyPercent > maxDuty) dutyPercent = maxDuty;

            sendWaveMakerCommand(mac, dutyPercent);

            // Track state
            String wmKey = macStr;
            wmKey.toUpperCase();
            g_waveMakerStates[wmKey] = {true, dutyPercent};

            // Activity log
            String devName, aqName, devType;
            lookupDeviceAndAquariumNames(macStr, devName, aqName, devType);
            char wmDesc[48];
            snprintf(wmDesc, sizeof(wmDesc), "PWM %.0f%%", dutyPercent);
            appendActivityLog("adhoc", "wavemaker", wmDesc, devName.c_str(), aqName.c_str());

            DynamicJsonDocument resp(256);
            resp["success"] = true;
            resp["command"] = "PWM";
            resp["mac"] = macStr;
            resp["dutyPercent"] = dutyPercent;
            String response;
            serializeJson(resp, response);
            request->send(200, "application/json", response);
        } else {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Unknown command. Use STOP or PWM\"}");
        }
    });

    // GET wavemaker schedule for a specific MAC
    server.on("/api/wm-schedule", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!request->hasParam("mac")) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing mac\"}");
            return;
        }

        String macStr = request->getParam("mac")->value();
        File file = FS_USER.open("/config/schedule/wm-schedule.json", "r");
        if (!file) {
            request->send(200, "application/json", "{\"success\":true,\"schedule\":null}");
            return;
        }

        DynamicJsonDocument doc(4096);
        DeserializationError error = deserializeJson(doc, file);
        file.close();
        if (error) {
            request->send(500, "application/json", "{\"success\":false,\"error\":\"Failed to parse wm-schedule.json\"}");
            return;
        }

        JsonArray schedules = doc["schedules"].as<JsonArray>();
        JsonObject found;
        for (JsonObject entry : schedules) {
            String entryMac = entry["mac"] | "";
            if (entryMac.equalsIgnoreCase(macStr)) {
                found = entry;
                break;
            }
        }

        DynamicJsonDocument responseDoc(2048);
        responseDoc["success"] = true;

        if (!found.isNull()) {
            JsonObject sched = responseDoc.createNestedObject("schedule");
            sched["mac"] = found["mac"] | "";
            sched["tankId"] = found["tankId"] | 0;
            sched["deviceName"] = found["deviceName"] | "";

            JsonObject storedSchedule = found["schedule"].as<JsonObject>();
            if (!storedSchedule.isNull()) {
                JsonArray ent = sched.createNestedArray("entries");
                JsonArray storedEntries = storedSchedule["entries"].as<JsonArray>();
                for (JsonObject e : storedEntries) {
                    JsonObject ne = ent.createNestedObject();
                    ne["enabled"] = e["enabled"] | true;
                    ne["hour"] = e["hour"] | 8;
                    ne["minute"] = e["minute"] | 0;
                    ne["ampm"] = e["ampm"] | "AM";
                    ne["action"] = e["action"] | "speed_change";
                    ne["dutyPercent"] = e["dutyPercent"] | 0.0f;
                }

                JsonObject days = sched.createNestedObject("days");
                JsonObject storedDays = storedSchedule["days"].as<JsonObject>();
                days["Sunday"] = storedDays["Sunday"] | true;
                days["Monday"] = storedDays["Monday"] | true;
                days["Tuesday"] = storedDays["Tuesday"] | true;
                days["Wednesday"] = storedDays["Wednesday"] | true;
                days["Thursday"] = storedDays["Thursday"] | true;
                days["Friday"] = storedDays["Friday"] | true;
                days["Saturday"] = storedDays["Saturday"] | true;
            }
        } else {
            responseDoc["schedule"] = nullptr;
        }

        String response;
        serializeJson(responseDoc, response);
        request->send(200, "application/json", response);
    });

    // POST save wavemaker schedule
    server.on("/api/wm-schedule", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        if (index + len != total) return;

        DynamicJsonDocument body(2048);
        DeserializationError error = deserializeJson(body, data, len);
        if (error) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON\"}");
            return;
        }

        String macStr = body["mac"] | "";
        if (macStr.length() == 0) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing mac\"}");
            return;
        }

        // Backwards-compatible delete-in-post support: { mac, action: "delete" }
        if (body.containsKey("action") && String(body["action"] | "") == "delete") {
            File file = FS_USER.open("/config/schedule/wm-schedule.json", "r");
            DynamicJsonDocument doc(4096);
            bool changed = false;

            if (file) {
                DeserializationError err2 = deserializeJson(doc, file);
                file.close();
                if (!err2) {
                    JsonArray schedules = doc["schedules"].as<JsonArray>();
                    if (!schedules.isNull()) {
                        for (int i = (int)schedules.size() - 1; i >= 0; i--) {
                            String entryMac = schedules[i]["mac"] | "";
                            if (entryMac.equalsIgnoreCase(macStr)) {
                                schedules.remove(i);
                                changed = true;
                            }
                        }
                    }
                }
            }

            if (changed) {
                File out = FS_USER.open("/config/schedule/wm-schedule.json", "w");
                if (!out) {
                    request->send(500, "application/json", "{\"success\":false,\"error\":\"Failed to write wm-schedule.json\"}");
                    return;
                }
                serializeJson(doc, out);
                out.close();
            }

            rebuildNextTasks();
            request->send(200, "application/json", "{\"success\":true}");
            return;
        }

        JsonVariant schedule = body["schedule"];

        File file = FS_USER.open("/config/schedule/wm-schedule.json", "r");
        DynamicJsonDocument doc(4096);
        if (file) {
            deserializeJson(doc, file);
            file.close();
        }

        JsonArray schedules = doc["schedules"];
        if (schedules.isNull()) {
            schedules = doc.createNestedArray("schedules");
        }

        // Remove existing entry for this MAC
        for (int i = (int)schedules.size() - 1; i >= 0; i--) {
            String entryMac = schedules[i]["mac"] | "";
            if (entryMac.equalsIgnoreCase(macStr)) {
                schedules.remove(i);
            }
        }

        JsonObject newEntry = schedules.createNestedObject();
        newEntry["mac"] = macStr;
        newEntry["tankId"] = body["tankId"] | 0;
        newEntry["deviceName"] = body["deviceName"] | "";

        JsonObject newSched = newEntry.createNestedObject("schedule");

        // Copy entries
        JsonArray ent = newSched.createNestedArray("entries");
        JsonArray srcEnt = schedule["entries"].as<JsonArray>();
        for (JsonObject e : srcEnt) {
            JsonObject ne = ent.createNestedObject();
            ne["enabled"] = e["enabled"] | true;
            ne["hour"] = e["hour"] | 8;
            ne["minute"] = e["minute"] | 0;
            ne["ampm"] = e["ampm"] | "AM";
            ne["action"] = e["action"] | "speed_change";
            ne["dutyPercent"] = e["dutyPercent"] | 0.0f;
        }

        // Copy days
        JsonObject days = newSched.createNestedObject("days");
        JsonObject srcDays = schedule["days"].as<JsonObject>();
        days["Sunday"] = srcDays["Sunday"] | true;
        days["Monday"] = srcDays["Monday"] | true;
        days["Tuesday"] = srcDays["Tuesday"] | true;
        days["Wednesday"] = srcDays["Wednesday"] | true;
        days["Thursday"] = srcDays["Thursday"] | true;
        days["Friday"] = srcDays["Friday"] | true;
        days["Saturday"] = srcDays["Saturday"] | true;

        newEntry["updatedAt"] = millis();

        file = FS_USER.open("/config/schedule/wm-schedule.json", "w");
        if (!file) {
            request->send(500, "application/json", "{\"success\":false,\"error\":\"Failed to write wm-schedule.json\"}");
            return;
        }
        serializeJson(doc, file);
        file.close();

        rebuildNextTasks();

        request->send(200, "application/json", "{\"success\":true}");
    });

    // POST delete wavemaker schedule for a device (dedicated endpoint)
    server.on("/api/wm-schedule/delete", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        if (index + len != total) return;

        DynamicJsonDocument body(512);
        DeserializationError error = deserializeJson(body, data, len);
        if (error) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON\"}");
            return;
        }

        String macStr = body["mac"] | "";
        if (macStr.length() == 0) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing mac\"}");
            return;
        }

        File file = FS_USER.open("/config/schedule/wm-schedule.json", "r");
        DynamicJsonDocument doc(4096);
        bool changed = false;

        if (file) {
            DeserializationError err2 = deserializeJson(doc, file);
            file.close();
            if (err2) {
                request->send(500, "application/json", "{\"success\":false,\"error\":\"Failed to parse wm-schedule.json\"}");
                return;
            }

            JsonArray schedules = doc["schedules"].as<JsonArray>();
            if (!schedules.isNull()) {
                for (int i = (int)schedules.size() - 1; i >= 0; i--) {
                    String entryMac = schedules[i]["mac"] | "";
                    if (entryMac.equalsIgnoreCase(macStr)) {
                        schedules.remove(i);
                        changed = true;
                    }
                }
            }
        }

        if (changed) {
            File out = FS_USER.open("/config/schedule/wm-schedule.json", "w");
            if (!out) {
                request->send(500, "application/json", "{\"success\":false,\"error\":\"Failed to write wm-schedule.json\"}");
                return;
            }
            serializeJson(doc, out);
            out.close();
        }

        rebuildNextTasks();

        request->send(200, "application/json", "{\"success\":true}");
    });

    // GET wavemaker status (fetch from node via ESP-NOW status request)
    server.on("/api/wm-status", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!request->hasParam("mac")) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing mac\"}");
            return;
        }

        String macStr = request->getParam("mac")->value();
        String macKey = macStr;
        macKey.toUpperCase();

        uint8_t mac[6];
        if (sscanf(macStr.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                  &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid MAC address\"}");
            return;
        }

        // Send status request command (0x28) to the wavemaker node
        CommandMessage cmd = {};
        cmd.header.type = MessageType::COMMAND;
        cmd.header.tankId = 0;
        cmd.header.nodeType = NodeType::HUB;
        cmd.header.timestamp = millis();
        cmd.header.sequenceNum = 0;
        cmd.commandId = generateCommandId();
        cmd.commandSeqID = 0;
        cmd.finalCommand = true;
        cmd.commandData[0] = 0x28;  // STATUS_REQUEST

        uint8_t apMac[6] = {0};
#ifdef ESP32
        esp_read_mac(apMac, ESP_MAC_WIFI_SOFTAP);
#endif
        memcpy(cmd.returnMac, apMac, 6);

#ifdef ESP32
        ESPNowManager::getInstance().addPeer(mac, WIFI_IF_AP);
#else
        ESPNowManager::getInstance().addPeer(mac);
#endif
        ESPNowManager::getInstance().send(mac, (uint8_t*)&cmd, sizeof(cmd));

        g_waveMakerStatusPending[macKey] = millis();

        // Return cached status if available
        auto it = g_waveMakerStatus.find(macKey);
        if (it != g_waveMakerStatus.end()) {
            WaveMakerStatus status = it->second;

            DynamicJsonDocument responseDoc(256);
            responseDoc["success"] = true;
            JsonObject statusObj = responseDoc.createNestedObject("status");
            statusObj["pumpActive"] = status.pumpActive;
            statusObj["dutyPercent"] = status.dutyPercent;
            statusObj["pwmRaw"] = status.pwmRaw;
            responseDoc["updatedAt"] = status.updatedAt;

            String response;
            serializeJson(responseDoc, response);
            request->send(200, "application/json", response);
            return;
        }

        request->send(200, "application/json", "{\"success\":false,\"pending\":true}");
    });

    // ========================================================================
    // NODE CONFIG API (per-node node_config.txt stored under /config as node_config_<MAC>.txt)
    // ========================================================================

    // GET a specific key from a node's node_config file
    // Query params: mac, key
    server.on("/api/node-config", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!request->hasParam("mac") || !request->hasParam("key")) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing mac or key\"}");
            return;
        }

        String macStr = request->getParam("mac")->value();
        String key = request->getParam("key")->value();
        if (macStr.length() == 0 || key.length() == 0) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid parameters\"}");
            return;
        }

        DynamicJsonDocument resp(256);
        resp["success"] = true;
        resp["value"] = nullptr;

        // Special-case AVAILABLE_FEED: stored centrally in Feed_count.txt (one file for all feeders)
        if (key.equalsIgnoreCase("AVAILABLE_FEED")) {
            int val = getFeedCountForMac(macStr);
            if (val >= 0) resp["value"] = val; else resp["value"] = nullptr;

            String out;
            serializeJson(resp, out);
            request->send(200, "application/json", out);
            return;
        }

        // Fallback: per-node node_config_<mac>.txt (other keys)
        // normalize MAC for filename (remove colons)
        String macNo = macStr;
        macNo.replace(":", "");
        macNo.toLowerCase();
        String path = String("/config/node_config_") + macNo + String(".txt");

        if (!FS_USER.exists(path)) {
            String out;
            serializeJson(resp, out);
            request->send(200, "application/json", out);
            return;
        }

        File f = FS_USER.open(path, "r");
        if (!f) {
            request->send(500, "application/json", "{\"success\":false,\"error\":\"Failed to open config file\"}");
            return;
        }

        while (f.available()) {
            String line = f.readStringUntil('\n');
            line.trim();
            if (line.length() == 0 || line.startsWith("#")) continue;
            int eq = line.indexOf('=');
            if (eq <= 0) continue;
            String k = line.substring(0, eq);
            String v = line.substring(eq + 1);
            k.trim(); v.trim();
            if (k.equalsIgnoreCase(key)) {
                resp["value"] = v;
                break;
            }
        }
        f.close();

        String out;
        serializeJson(resp, out);
        request->send(200, "application/json", out);
    });

    // POST update a key in the node_config file for a specific node
    // Body: { mac: "AA:BB:...", key: "AVAILABLE_FEED", value: 5 }
    server.on("/api/node-config", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        if (index + len != total) return;

        DynamicJsonDocument body(512);
        DeserializationError err = deserializeJson(body, data, len);
        if (err) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON\"}");
            return;
        }

        String macStr = body["mac"] | "";
        String key = body["key"] | "";
        // Accept numeric or string values
        String value = body["value"] | "";
        if (macStr.length() == 0 || key.length() == 0) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing mac/key\"}");
            return;
        }

        // Special-case AVAILABLE_FEED -> centralized Feed_count.txt in user-data
        if (key.equalsIgnoreCase("AVAILABLE_FEED")) {
            // Log the incoming request so we can see what was received from the UI
            char rawValBuf[64] = {0};
            serializeJson(body["value"], rawValBuf, sizeof(rawValBuf));
            Serial.printf("[API] /api/node-config POST received mac=%s key=%s rawValue=%s\n", macStr.c_str(), key.c_str(), rawValBuf);

            // Read numeric value robustly from JSON (default 0)
            int v = body["value"] | 0;
            if (v < 0) v = 0;

            bool ok = setFeedCountForMac(macStr, v);
            if (!ok) {
                request->send(500, "application/json", "{\"success\":false,\"error\":\"Failed to write Feed_count.txt\"}");
                return;
            }

            // Notify UI about the updated count
            DynamicJsonDocument doc(256);
            doc["mac"] = macStr;
            doc["available"] = v;
            String payload;
            serializeJson(doc, payload);
            AquariumManager::getInstance().broadcastUpdate("feedCountUpdated", payload);

            // Return the saved value so the caller can confirm what was persisted
            DynamicJsonDocument resp(128);
            resp["success"] = true;
            resp["value"] = v;
            String out;
            serializeJson(resp, out);
            request->send(200, "application/json", out);
            return;
        }

        // Fallback: update per-node node_config_<mac>.txt for other keys
        String macNo = macStr;
        macNo.replace(":", "");
        macNo.toLowerCase();
        String path = String("/config/node_config_") + macNo + String(".txt");

        // Read existing file (if any) and update or append the key
        std::vector<String> lines;
        if (FS_USER.exists(path)) {
            File rf = FS_USER.open(path, "r");
            if (rf) {
                while (rf.available()) {
                    String line = rf.readStringUntil('\n');
                    line.trim();
                    lines.push_back(line);
                }
                rf.close();
            }
        }

        bool updated = false;
        for (size_t i = 0; i < lines.size(); i++) {
            String l = lines[i];
            if (l.length() == 0 || l.startsWith("#")) continue;
            int eq = l.indexOf('=');
            if (eq <= 0) continue;
            String k = l.substring(0, eq);
            k.trim();
            if (k.equalsIgnoreCase(key)) {
                lines[i] = key + String("=") + value;
                updated = true;
                break;
            }
        }
        if (!updated) {
            lines.push_back(key + String("=") + value);
        }

        // Write back file
        File wf = FS_USER.open(path, "w");
        if (!wf) {
            request->send(500, "application/json", "{\"success\":false,\"error\":\"Failed to open file for write\"}");
            return;
        }
        for (size_t i = 0; i < lines.size(); i++) {
            wf.println(lines[i]);
        }
        wf.close();

        request->send(200, "application/json", "{\"success\":true}");
    });

    // ========================================================================
    // FEEDER SCHEDULE API
    // ========================================================================

    // GET feeder schedule for a device
    server.on("/api/feeder-schedule", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!request->hasParam("mac")) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing mac\"}");
            return;
        }

        String macStr = request->getParam("mac")->value();
        File file = FS_USER.open("/config/schedule/feeder-schedule.json", "r");
        if (!file) {
            request->send(200, "application/json", "{\"success\":true,\"schedule\":null}");
            return;
        }

        DynamicJsonDocument doc(4096);
        DeserializationError error = deserializeJson(doc, file);
        file.close();
        if (error) {
            request->send(500, "application/json", "{\"success\":false,\"error\":\"Failed to parse feeder-schedule.json\"}");
            return;
        }

        JsonArray schedules = doc["schedules"].as<JsonArray>();
        JsonObject found;
        for (JsonObject entry : schedules) {
            String entryMac = entry["mac"] | "";
            if (entryMac.equalsIgnoreCase(macStr)) {
                found = entry;
                break;
            }
        }

        DynamicJsonDocument responseDoc(2048);
        responseDoc["success"] = true;

        if (!found.isNull()) {
            JsonObject sched = responseDoc.createNestedObject("schedule");
            sched["mac"] = found["mac"] | "";
            sched["tankId"] = found["tankId"] | 0;
            sched["deviceName"] = found["deviceName"] | "";
            
            JsonObject storedSchedule = found["schedule"].as<JsonObject>();
            if (!storedSchedule.isNull()) {
                JsonArray ft = sched.createNestedArray("feedingTimes");
                JsonArray storedFt = storedSchedule["feedingTimes"].as<JsonArray>();
                for (JsonObject time : storedFt) {
                    JsonObject t = ft.createNestedObject();
                    t["enabled"] = time["enabled"] | false;
                    t["hour"] = time["hour"] | 8;
                    t["minute"] = time["minute"] | 0;
                    t["ampm"] = time["ampm"] | "AM";
                }
                
                JsonObject days = sched.createNestedObject("days");
                JsonObject storedDays = storedSchedule["days"].as<JsonObject>();
                days["Sunday"] = storedDays["Sunday"] | true;
                days["Monday"] = storedDays["Monday"] | true;
                days["Tuesday"] = storedDays["Tuesday"] | true;
                days["Wednesday"] = storedDays["Wednesday"] | true;
                days["Thursday"] = storedDays["Thursday"] | true;
                days["Friday"] = storedDays["Friday"] | true;
                days["Saturday"] = storedDays["Saturday"] | true;
            }
        } else {
            responseDoc["schedule"] = nullptr;
        }

        String response;
        serializeJson(responseDoc, response);
        request->send(200, "application/json", response);
    });

    // POST save feeder schedule
    server.on("/api/feeder-schedule", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        if (index + len != total) return;

        DynamicJsonDocument body(2048);
        DeserializationError error = deserializeJson(body, data, len);
        if (error) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON\"}");
            return;
        }

        String macStr = body["mac"] | "";
        if (macStr.length() == 0) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing mac\"}");
            return;
        }

        // Backwards-compatible delete-in-post support: { mac, action: "delete" }
        if (body.containsKey("action") && String(body["action"] | "") == "delete") {
            File file = FS_USER.open("/config/schedule/feeder-schedule.json", "r");
            DynamicJsonDocument doc(4096);
            bool changed = false;

            if (file) {
                DeserializationError err2 = deserializeJson(doc, file);
                file.close();
                if (!err2) {
                    JsonArray schedules = doc["schedules"].as<JsonArray>();
                    if (!schedules.isNull()) {
                        for (int i = (int)schedules.size() - 1; i >= 0; i--) {
                            String entryMac = schedules[i]["mac"] | "";
                            if (entryMac.equalsIgnoreCase(macStr)) {
                                schedules.remove(i);
                                changed = true;
                            }
                        }
                    }
                }
            }

            if (changed) {
                File out = FS_USER.open("/config/schedule/feeder-schedule.json", "w");
                if (!out) {
                    request->send(500, "application/json", "{\"success\":false,\"error\":\"Failed to write feeder-schedule.json\"}");
                    return;
                }
                serializeJson(doc, out);
                out.close();
            }

            // Rebuild next-task.json with updated schedules
            rebuildNextTasks();
            request->send(200, "application/json", "{\"success\":true}");
            return;
        }

        JsonVariant schedule = body["schedule"];

        File file = FS_USER.open("/config/schedule/feeder-schedule.json", "r");
        DynamicJsonDocument doc(4096);
        if (file) {
            deserializeJson(doc, file);
            file.close();
        }

        JsonArray schedules = doc["schedules"];
        if (schedules.isNull()) {
            schedules = doc.createNestedArray("schedules");
        }

        // Remove existing entry for this MAC
        for (int i = (int)schedules.size() - 1; i >= 0; i--) {
            String entryMac = schedules[i]["mac"] | "";
            if (entryMac.equalsIgnoreCase(macStr)) {
                schedules.remove(i);
            }
        }

        JsonObject newEntry = schedules.createNestedObject();
        newEntry["mac"] = macStr;
        newEntry["tankId"] = body["tankId"] | 0;
        newEntry["deviceName"] = body["deviceName"] | "";

        JsonObject newSched = newEntry.createNestedObject("schedule");
        
        // Copy feeding times
        JsonArray ft = newSched.createNestedArray("feedingTimes");
        JsonArray srcFt = schedule["feedingTimes"].as<JsonArray>();
        for (JsonObject time : srcFt) {
            JsonObject t = ft.createNestedObject();
            t["enabled"] = time["enabled"] | false;
            t["hour"] = time["hour"] | 8;
            t["minute"] = time["minute"] | 0;
            t["ampm"] = time["ampm"] | "AM";
        }
        
        // Copy days
        JsonObject days = newSched.createNestedObject("days");
        JsonObject srcDays = schedule["days"].as<JsonObject>();
        days["Sunday"] = srcDays["Sunday"] | true;
        days["Monday"] = srcDays["Monday"] | true;
        days["Tuesday"] = srcDays["Tuesday"] | true;
        days["Wednesday"] = srcDays["Wednesday"] | true;
        days["Thursday"] = srcDays["Thursday"] | true;
        days["Friday"] = srcDays["Friday"] | true;
        days["Saturday"] = srcDays["Saturday"] | true;

        newEntry["updatedAt"] = millis();

        file = FS_USER.open("/config/schedule/feeder-schedule.json", "w");
        if (!file) {
            request->send(500, "application/json", "{\"success\":false,\"error\":\"Failed to write feeder-schedule.json\"}");
            return;
        }
        serializeJson(doc, file);
        file.close();

        // Rebuild next-task.json with updated schedule (includes feeder tasks)
        rebuildNextTasks();

        request->send(200, "application/json", "{\"success\":true}");
    });

    // POST delete feeder schedule for a device (dedicated endpoint)
    server.on("/api/feeder-schedule/delete", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        if (index + len != total) return;

        DynamicJsonDocument body(512);
        DeserializationError error = deserializeJson(body, data, len);
        if (error) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON\"}");
            return;
        }

        String macStr = body["mac"] | "";
        if (macStr.length() == 0) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing mac\"}");
            return;
        }

        File file = FS_USER.open("/config/schedule/feeder-schedule.json", "r");
        DynamicJsonDocument doc(4096);
        bool changed = false;

        if (file) {
            DeserializationError err2 = deserializeJson(doc, file);
            file.close();
            if (err2) {
                request->send(500, "application/json", "{\"success\":false,\"error\":\"Failed to parse feeder-schedule.json\"}");
                return;
            }

            JsonArray schedules = doc["schedules"].as<JsonArray>();
            if (!schedules.isNull()) {
                for (int i = (int)schedules.size() - 1; i >= 0; i--) {
                    String entryMac = schedules[i]["mac"] | "";
                    if (entryMac.equalsIgnoreCase(macStr)) {
                        schedules.remove(i);
                        changed = true;
                    }
                }
            }
        }

        if (changed) {
            File out = FS_USER.open("/config/schedule/feeder-schedule.json", "w");
            if (!out) {
                request->send(500, "application/json", "{\"success\":false,\"error\":\"Failed to write feeder-schedule.json\"}");
                return;
            }
            serializeJson(doc, out);
            out.close();
        }

        // Rebuild next-task.json to remove any scheduled tasks for this device
        rebuildNextTasks();

        request->send(200, "application/json", "{\"success\":true}");
    });

    // POST delete an upcoming next-task entry by scheduleId
    server.on("/api/next-task/delete", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        if (index + len != total) return;

        DynamicJsonDocument body(1024);
        DeserializationError error = deserializeJson(body, data, len);
        if (error) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON\"}");
            return;
        }

        // Prefer scheduleId-based deletion
        String scheduleId = body["scheduleId"] | "";
        if (scheduleId.length() > 0) {
            if (deleteTasksByScheduleId(scheduleId)) {
                request->send(200, "application/json", "{\"success\":true}");
                return;
            }
            request->send(404, "application/json", "{\"success\":false,\"error\":\"Task not found\"}");
            return;
        }

        // Fallback to legacy field-based deletion
        String macStr = body["mac"] | "";
        if (macStr.length() == 0) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing scheduleId or mac\"}");
            return;
        }

        NextTask target;
        target.mac = macStr;
        target.taskType = (TaskType)(body["taskType"] | 0);
        target.channel = body["channel"] | 0;
        target.actionOn = body["actionOn"] | false;
        target.scheduledTime = (time_t)(body["scheduledTime"] | 0);
        target.period = body["period"] | "";
        target.pwmValue = body["pwmValue"] | 0;
        target.durationMs = body["durationMs"] | 0;

        if (deleteNextTaskEntry(target)) {
            request->send(200, "application/json", "{\"success\":true}");
            return;
        }

        request->send(404, "application/json", "{\"success\":false,\"error\":\"Task not found\"}");
    });

    // POST delete selected tasks by scheduleIds and rebuild
    server.on("/api/next-task/delete-selected", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        if (index + len != total) return;

        DynamicJsonDocument body(4096);
        DeserializationError error = deserializeJson(body, data, len);
        if (error) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON\"}");
            return;
        }

        JsonArray idsArray = body["scheduleIds"].as<JsonArray>();
        if (idsArray.isNull() || idsArray.size() == 0) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing or empty scheduleIds array\"}");
            return;
        }

        std::vector<String> scheduleIds;
        for (JsonVariant v : idsArray) {
            String id = v.as<String>();
            if (id.length() > 0) {
                scheduleIds.push_back(id);
            }
        }

        int deleted = deleteTasksByScheduleIds(scheduleIds);
        Serial.printf("[SCHEDULER] Deleted %d tasks by scheduleIds\n", deleted);

        // Rebuild next-task.json from schedule files to recalculate future occurrences
        rebuildNextTasks();

        DynamicJsonDocument response(256);
        response["success"] = true;
        response["deleted"] = deleted;
        
        String responseStr;
        serializeJson(response, responseStr);
        request->send(200, "application/json", responseStr);
    });

    // GET light channel status (fetch from node)
    server.on("/api/light-status", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!request->hasParam("mac")) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing mac\"}");
            return;
        }

        String macStr = request->getParam("mac")->value();
        String macKey = macStr;
        macKey.toUpperCase();

        uint8_t mac[6];
        if (sscanf(macStr.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                  &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid MAC address\"}");
            return;
        }


        if (config.debugESPNOW) {
            Serial.printf("[LIGHT] Status request for %s\n", macKey.c_str());
        }

        CommandMessage cmd = {};
        cmd.header.type = MessageType::COMMAND;
        cmd.header.tankId = 0;
        cmd.header.nodeType = NodeType::HUB;
        cmd.header.timestamp = millis();
        cmd.header.sequenceNum = 0;
        cmd.commandId = generateCommandId();
        cmd.commandSeqID = 0;
        cmd.finalCommand = true;
        cmd.commandData[0] = 0x28;  // LIGHT_STATUS request

        // Include Hub AP MAC in the command so node knows where to reply
        uint8_t apMac[6] = {0};
#ifdef ESP32
        esp_read_mac(apMac, ESP_MAC_WIFI_SOFTAP);
#else
        String apStr = WiFi.softAPmacAddress();
        sscanf(apStr.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
               &apMac[0], &apMac[1], &apMac[2], &apMac[3], &apMac[4], &apMac[5]);
#endif
        memcpy(cmd.returnMac, apMac, 6);

        Serial.printf("[HUB] Re-adding peer before status request: %02X:%02X:%02X:%02X:%02X:%02X\n",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        // DEBUG: print Hub WiFi mode
        wifi_mode_t hubMode = WiFi.getMode();
        const char* hubModeStr = (hubMode == WIFI_MODE_STA) ? "STA" : (hubMode == WIFI_MODE_AP) ? "AP" : (hubMode == WIFI_MODE_APSTA) ? "AP+STA" : "UNKNOWN";
        Serial.printf("[HUB] WiFi mode: %s\n", hubModeStr);
        Serial.flush();
#ifdef ESP32
        bool peerAdded = ESPNowManager::getInstance().addPeer(mac, WIFI_IF_AP);
#else
        bool peerAdded = ESPNowManager::getInstance().addPeer(mac);
#endif
        Serial.printf("[HUB] Peer add: %s\n", peerAdded ? "OK" : "FAIL");
        
        Serial.printf("[HUB] Sending COMMAND (type=0x%02X, len=%d) to node...\n", 
                      (uint8_t)cmd.header.type, sizeof(cmd));
        bool sendResult = ESPNowManager::getInstance().send(mac, (uint8_t*)&cmd, sizeof(cmd));
        Serial.printf("[HUB] Send result: %s\n", sendResult ? "SUCCESS" : "FAILED");
        Serial.println("[HUB] Waiting for STATUS response (watch for [RX-RAW])...");

        g_lightStatusPending[macKey] = millis();

        auto it = g_lightStatus.find(macKey);
        if (it != g_lightStatus.end()) {
            LightChannelStatus status = it->second;
            if (config.debugESPNOW) {
                Serial.printf("[LIGHT] Returning cached status for %s -> ch1=%d ch2=%d ch3=%d\n",
                              macKey.c_str(), status.ch1 ? 1 : 0, status.ch2 ? 1 : 0, status.ch3 ? 1 : 0);
            }

            DynamicJsonDocument responseDoc(256);
            responseDoc["success"] = true;
            JsonObject statusObj = responseDoc.createNestedObject("status");
            statusObj["1"] = status.ch1;
            statusObj["2"] = status.ch2;
            statusObj["3"] = status.ch3;
            responseDoc["updatedAt"] = status.updatedAt;

            String response;
            serializeJson(responseDoc, response);
            request->send(200, "application/json", response);
            return;
        }

        if (config.debugESPNOW) {
            Serial.printf("[LIGHT] No cached status for %s (request sent)\n", macKey.c_str());
        }
        request->send(200, "application/json", "{\"success\":false,\"pending\":true}");
    });

// GET peers list (Hub diagnostics)
server.on("/api/peers", HTTP_GET, [](AsyncWebServerRequest *request){
    DynamicJsonDocument doc(4096);
    JsonArray peersArr = doc.createNestedArray("peers");

    auto peers = ESPNowManager::getInstance().getPeers();
    for (auto &p : peers) {
        char macStr[32];
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 p.mac[0], p.mac[1], p.mac[2], p.mac[3], p.mac[4], p.mac[5]);
        JsonObject o = peersArr.createNestedObject();
        o["mac"] = macStr;
        o["online"] = p.online;
        o["lastHeartbeat"] = p.lastHeartbeat;
    }

    String resp;
    serializeJson(doc, resp);
    request->send(200, "application/json", resp);
});

// POST remove peer
server.on("/api/peer/remove", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!request->hasParam("mac", true)) {
        request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing mac\"}");
        return;
    }

    String macStr = request->getParam("mac", true)->value();
    uint8_t mac[6];
    if (sscanf(macStr.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
               &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6) {
        request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid MAC\"}");
        return;
    }

    bool removed = ESPNowManager::getInstance().removePeer(mac);
    if (removed) {
        request->send(200, "application/json", "{\"success\":true}");
    } else {
        request->send(500, "application/json", "{\"success\":false,\"error\":\"Remove failed\"}");
    }
});

// GET hub macs (STA and AP)
server.on("/api/hub-macs", HTTP_GET, [](AsyncWebServerRequest *request){
    DynamicJsonDocument doc(256);
    doc["sta"] = WiFi.macAddress();
    doc["ap"] = WiFi.softAPmacAddress();
    String resp;
    serializeJson(doc, resp);
    request->send(200, "application/json", resp);
});

    // DELETE scheduler debug log — clears the file for a fresh test run
    server.on("/api/scheduler-debug-log", HTTP_DELETE, [](AsyncWebServerRequest *request){
        if (FS_USER.exists(SCHEDULER_DEBUG_LOG)) {
            FS_USER.remove(SCHEDULER_DEBUG_LOG);
        }
        // Write a fresh header
        File f = FS_USER.open(SCHEDULER_DEBUG_LOG, "w");
        if (f) {
            f.println("=== Scheduler debug log cleared ===");
            f.close();
        }
        request->send(200, "application/json", "{\"success\":true,\"message\":\"Debug log cleared\"}");
    });

    // GET settings file list — list ALL files on FS_USER recursively
    server.on("/api/settings/files", HTTP_GET, [](AsyncWebServerRequest *request){
        DynamicJsonDocument doc(4096);
        JsonArray files = doc.createNestedArray("files");

        File root = FS_USER.open("/");
        if (root && root.isDirectory()) {
            listUserFilesRecursive(root, "/", files);
            root.close();
        }

        String resp;
        serializeJson(doc, resp);
        request->send(200, "application/json", resp);
    });

    // GET settings file download — download any file from FS_USER by path
    server.on("/api/settings/download", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!request->hasParam("name")) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing file name\"}");
            return;
        }

        String name = request->getParam("name")->value();

        // Ensure path starts with /
        String path = name;
        if (!path.startsWith("/")) {
            path = "/" + path;
        }

        if (!FS_USER.exists(path)) {
            request->send(404, "application/json", "{\"success\":false,\"error\":\"File not found\"}");
            return;
        }

        // Determine content type
        String contentType = "application/octet-stream";
        if (path.endsWith(".json")) contentType = "application/json";
        else if (path.endsWith(".txt")) contentType = "text/plain";

        request->send(FS_USER, path, contentType, true);
    });

    // POST settings file upload — upload any file to FS_USER by target path
    server.on("/api/settings/upload", HTTP_POST,
        [](AsyncWebServerRequest *request){
            SettingsUploadContext* ctx = (SettingsUploadContext*)request->_tempObject;
            if (!ctx || ctx->error.length() == 0) {
                request->send(200, "application/json", "{\"success\":true}");
            } else {
                String response = String("{\"success\":false,\"error\":\"") + ctx->error + "\"}";
                request->send(400, "application/json", response);
            }

            if (ctx) {
                delete ctx;
                request->_tempObject = nullptr;
            }
        },
        [](AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final){
            SettingsUploadContext* ctx = (SettingsUploadContext*)request->_tempObject;
            if (!ctx) {
                ctx = new SettingsUploadContext();
                request->_tempObject = ctx;
            }

            if (index == 0) {
                if (!request->hasParam("target")) {
                    ctx->error = "Missing target";
                    return;
                }

                String target = request->getParam("target")->value();
                // Ensure path starts with /
                if (!target.startsWith("/")) {
                    target = "/" + target;
                }

                // Create parent directories if needed
                int lastSlash = target.lastIndexOf('/');
                if (lastSlash > 0) {
                    String parentDir = target.substring(0, lastSlash);
                    FS_USER.mkdir(parentDir);
                }

                ctx->file = FS_USER.open(target, "w");
                if (!ctx->file) {
                    ctx->error = "Failed to open file";
                    return;
                }
            }

            if (ctx->error.length() > 0) {
                return;
            }

            if (len) {
                ctx->file.write(data, len);
            }

            if (final && ctx->file) {
                ctx->file.close();
            }
        }
    );

    // GET OTA URLs and versions
    server.on("/api/settings/ota-urls", HTTP_GET, [](AsyncWebServerRequest *request){
        JsonDocument doc;
        doc["hubFirmwareUrl"] = config.hubFirmwareOtaUrl;
        doc["hubLittlefsUrl"] = config.hubLittlefsOtaUrl;
        doc["hubFirmwareVersion"] = config.hubFirmwareVersion;
        doc["hubLittlefsVersion"] = config.hubLittlefsVersion;
        doc["lightNodeOtaUrl"] = config.lightNodeOtaUrl;

        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });

    // POST Set OTA URLs (runtime only, not persisted to hub_config.txt)
    server.on("/api/settings/ota-urls", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
            if (index + len == total) {
                JsonDocument doc;
                DeserializationError error = deserializeJson(doc, (const char*)data, len);
                if (error) {
                    request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON\"}");
                    return;
                }
                
                if (doc.containsKey("hubFirmwareUrl")) {
                    config.hubFirmwareOtaUrl = doc["hubFirmwareUrl"].as<String>();
                }
                if (doc.containsKey("hubLittlefsUrl")) {
                    config.hubLittlefsOtaUrl = doc["hubLittlefsUrl"].as<String>();
                }
                if (doc.containsKey("lightNodeOtaUrl")) {
                    config.lightNodeOtaUrl = doc["lightNodeOtaUrl"].as<String>();
                }
                
                Serial.printf("[Config] OTA URLs updated\n");
                request->send(200, "application/json", "{\"success\":true}");
            }
        }
    );

    // GET Hub OTA check - check for available updates
    server.on("/api/hub/ota/check", HTTP_GET, [](AsyncWebServerRequest *request){
        JsonDocument doc;
        
        // Check firmware update
        if (config.hubFirmwareOtaUrl.length() > 0) {
            String remoteVersion;
            if (fetchRemoteVersion(config.hubFirmwareOtaUrl, remoteVersion)) {
                doc["firmware"]["currentVersion"] = config.hubFirmwareVersion;
                doc["firmware"]["availableVersion"] = remoteVersion;
                doc["firmware"]["hasUpdate"] = compareSemanticVersions(remoteVersion, config.hubFirmwareVersion) > 0;
                doc["firmware"]["url"] = config.hubFirmwareOtaUrl;
            } else {
                doc["firmware"]["error"] = "Failed to fetch firmware version";
            }
        } else {
            doc["firmware"]["error"] = "HUB_FIRMWARE_OTA_URL not configured";
        }
        
        // Check LittleFS update
        if (config.hubLittlefsOtaUrl.length() > 0) {
            String remoteVersion;
            if (fetchRemoteVersion(config.hubLittlefsOtaUrl, remoteVersion)) {
                doc["littlefs"]["currentVersion"] = config.hubLittlefsVersion;
                doc["littlefs"]["availableVersion"] = remoteVersion;
                doc["littlefs"]["hasUpdate"] = compareSemanticVersions(remoteVersion, config.hubLittlefsVersion) > 0;
                doc["littlefs"]["url"] = config.hubLittlefsOtaUrl;
            } else {
                doc["littlefs"]["error"] = "Failed to fetch LittleFS version";
            }
        } else {
            doc["littlefs"]["error"] = "HUB_LITTLEFS_OTA_URL not configured";
        }

        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });

    // POST OTA firmware update with version check - DEFERRED APPROACH
    server.on("/api/ota/firmware", HTTP_POST, [](AsyncWebServerRequest *request){
        // Check if OTA already in progress
        if (otaInProgress || otaPending != OtaPendingType::NONE) {
            request->send(409, "application/json", "{\"success\":false,\"error\":\"OTA already in progress\"}");
            return;
        }
        
        // Check if URL is configured
        if (config.hubFirmwareOtaUrl.length() == 0) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"HUB_FIRMWARE_OTA_URL not configured\"}");
            return;
        }
        
        // Check version before updating
        String remoteVersion;
        if (!fetchRemoteVersion(config.hubFirmwareOtaUrl, remoteVersion)) {
            request->send(500, "application/json", "{\"success\":false,\"error\":\"Failed to fetch remote version\"}");
            return;
        }
        
        if (compareSemanticVersions(remoteVersion, config.hubFirmwareVersion) <= 0) {
            String response = "{\"success\":false,\"error\":\"No update available. Current: " + 
                              config.hubFirmwareVersion + ", Remote: " + remoteVersion + "\"}";
            request->send(200, "application/json", response);
            return;
        }
        
        // Set pending OTA - will be processed in main loop
        otaPendingFirmwareVersion = remoteVersion;
        otaPending = OtaPendingType::FIRMWARE;
        
        Serial.printf("[OTA] Firmware update queued: %s -> %s\n", 
                      config.hubFirmwareVersion.c_str(), remoteVersion.c_str());
        
        // Respond immediately - OTA will happen in loop()
        request->send(200, "application/json", "{\"success\":true,\"message\":\"Firmware update started, hub will reboot shortly...\"}");
    });

    // POST OTA LittleFS update with version check - DEFERRED APPROACH
    server.on("/api/ota/littlefs", HTTP_POST, [](AsyncWebServerRequest *request){
        // Check if OTA already in progress
        if (otaInProgress || otaPending != OtaPendingType::NONE) {
            request->send(409, "application/json", "{\"success\":false,\"error\":\"OTA already in progress\"}");
            return;
        }
        
        // Check if URL is configured
        if (config.hubLittlefsOtaUrl.length() == 0) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"HUB_LITTLEFS_OTA_URL not configured\"}");
            return;
        }
        
        // Check version before updating
        String remoteVersion;
        if (!fetchRemoteVersion(config.hubLittlefsOtaUrl, remoteVersion)) {
            request->send(500, "application/json", "{\"success\":false,\"error\":\"Failed to fetch remote version\"}");
            return;
        }
        
        if (compareSemanticVersions(remoteVersion, config.hubLittlefsVersion) <= 0) {
            String response = "{\"success\":false,\"error\":\"No update available. Current: " + 
                              config.hubLittlefsVersion + ", Remote: " + remoteVersion + "\"}";
            request->send(200, "application/json", response);
            return;
        }
        
        // Set pending OTA - will be processed in main loop  
        otaPendingLittlefsVersion = remoteVersion;
        otaPending = OtaPendingType::LITTLEFS;
        
        Serial.printf("[OTA] LittleFS update queued: %s -> %s\n", 
                      config.hubLittlefsVersion.c_str(), remoteVersion.c_str());
        
        // Respond immediately - OTA will happen in loop()
        request->send(200, "application/json", "{\"success\":true,\"message\":\"LittleFS update started, hub will reboot shortly...\"}");
    });

    // POST OTA all updates (firmware + littlefs) - applies all before single reboot
    server.on("/api/ota/all", HTTP_POST, [](AsyncWebServerRequest *request){
        // Check if OTA already in progress
        if (otaInProgress || otaPending != OtaPendingType::NONE) {
            request->send(409, "application/json", "{\"success\":false,\"error\":\"OTA already in progress\"}");
            return;
        }
        
        bool hasFirmwareUpdate = false;
        bool hasLittlefsUpdate = false;
        String firmwareVersion, littlefsVersion;
        
        // Check firmware update availability
        if (config.hubFirmwareOtaUrl.length() > 0) {
            if (fetchRemoteVersion(config.hubFirmwareOtaUrl, firmwareVersion)) {
                if (compareSemanticVersions(firmwareVersion, config.hubFirmwareVersion) > 0) {
                    hasFirmwareUpdate = true;
                }
            }
        }
        
        // Check LittleFS update availability
        if (config.hubLittlefsOtaUrl.length() > 0) {
            if (fetchRemoteVersion(config.hubLittlefsOtaUrl, littlefsVersion)) {
                if (compareSemanticVersions(littlefsVersion, config.hubLittlefsVersion) > 0) {
                    hasLittlefsUpdate = true;
                }
            }
        }
        
        if (!hasFirmwareUpdate && !hasLittlefsUpdate) {
            request->send(200, "application/json", "{\"success\":false,\"error\":\"No updates available\"}");
            return;
        }
        
        // Set pending updates
        if (hasFirmwareUpdate && hasLittlefsUpdate) {
            otaPendingFirmwareVersion = firmwareVersion;
            otaPendingLittlefsVersion = littlefsVersion;
            otaPending = OtaPendingType::BOTH;
            Serial.printf("[OTA] Both updates queued: Firmware %s -> %s, LittleFS %s -> %s\n",
                          config.hubFirmwareVersion.c_str(), firmwareVersion.c_str(),
                          config.hubLittlefsVersion.c_str(), littlefsVersion.c_str());
        } else if (hasFirmwareUpdate) {
            otaPendingFirmwareVersion = firmwareVersion;
            otaPending = OtaPendingType::FIRMWARE;
            Serial.printf("[OTA] Firmware update queued: %s -> %s\n",
                          config.hubFirmwareVersion.c_str(), firmwareVersion.c_str());
        } else {
            otaPendingLittlefsVersion = littlefsVersion;
            otaPending = OtaPendingType::LITTLEFS;
            Serial.printf("[OTA] LittleFS update queued: %s -> %s\n",
                          config.hubLittlefsVersion.c_str(), littlefsVersion.c_str());
        }
        
        // Build response with what will be updated
        String updates = "";
        if (hasFirmwareUpdate) updates += "Firmware";
        if (hasFirmwareUpdate && hasLittlefsUpdate) updates += " + ";
        if (hasLittlefsUpdate) updates += "LittleFS";
        
        String response = "{\"success\":true,\"message\":\"" + updates + " update started, hub will reboot after all updates complete...\"}";
        request->send(200, "application/json", response);
    });

    // ========================================================================
    // NODE OTA ENDPOINTS (Generic for all device types)
    // ========================================================================

    // GET Device list for a specific type: /api/nodes/{type}/list
    server.on("^\\/api\\/nodes\\/([a-z_]+)\\/list$", HTTP_GET, [](AsyncWebServerRequest *request){
        String deviceTypeId = request->pathArg(0);
        String deviceTypeUpper = deviceTypeIdToUpper(deviceTypeId);
        
        Serial.printf("[NodeOTA] GET /api/nodes/%s/list\n", deviceTypeId.c_str());
        
        DynamicJsonDocument responseDoc(8192);
        JsonArray deviceArray = responseDoc.createNestedArray("devices");

        // Load tank name map from aquariums.json
        std::map<uint8_t, String> tankNames;
        File tanksFile = FS_USER.open("/config/aquariums.json", "r");
        if (tanksFile) {
            DynamicJsonDocument tanksDoc(8192);
            if (!deserializeJson(tanksDoc, tanksFile)) {
                JsonArray tanks = tanksDoc["aquariums"];
                for (JsonObject tank : tanks) {
                    uint8_t tankId = tank["id"] | 0;
                    const char* tankName = tank["name"] | "";
                    if (tankId > 0 && tankName && strlen(tankName) > 0) {
                        tankNames[tankId] = String(tankName);
                    }
                }
            }
            tanksFile.close();
        }

        // Load devices
        File devFile = FS_USER.open("/config/devices.json", "r");
        if (!devFile) {
            responseDoc["error"] = "devices.json not found";
            String response;
            serializeJson(responseDoc, response);
            request->send(200, "application/json", response);
            return;
        }

        DynamicJsonDocument devDoc(8192);
        DeserializationError err = deserializeJson(devDoc, devFile);
        devFile.close();
        if (err) {
            responseDoc["error"] = "JSON parse error";
            String response;
            serializeJson(responseDoc, response);
            request->send(200, "application/json", response);
            return;
        }

        JsonArray devices = devDoc["devices"];
        for (JsonObject device : devices) {
            String type = device["type"].as<String>();
            if (type.length() == 0) continue;
            String typeUpper = type;
            typeUpper.toUpperCase();
            if (typeUpper != deviceTypeUpper) continue;

            String macStr = device["mac"].as<String>();
            if (macStr.length() == 0) continue;

            uint8_t mac[6];
            if (sscanf(macStr.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                      &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6) {
                continue;
            }

            uint8_t tankId = device["tankId"] | 0;
            String tankName = tankNames.count(tankId) ? tankNames[tankId] : String("--");

            JsonObject out = deviceArray.createNestedObject();
            out["mac"] = macToString(mac);
            out["name"] = device["name"] | "Unknown";
            out["tankId"] = tankId;
            out["tankName"] = tankName;
            out["firmwareVersion"] = device["firmwareVersion"] | 0;
            out["online"] = ESPNowManager::getInstance().isPeerOnline(mac);
        }

        String response;
        serializeJson(responseDoc, response);
        request->send(200, "application/json", response);
    });

    // POST Check for device update: /api/nodes/{type}/check-update
    server.on("^\\/api\\/nodes\\/([a-z_]+)\\/check-update$", HTTP_POST, [](AsyncWebServerRequest *request){
        String deviceTypeId = request->pathArg(0);
        String deviceTypeUpper = deviceTypeIdToUpper(deviceTypeId);
        
        Serial.printf("[NodeOTA] POST /api/nodes/%s/check-update\n", deviceTypeId.c_str());
        
        // Get OTA URL from ota.json
        String baseUrl = getDeviceTypeOtaUrl(deviceTypeId);
        if (baseUrl.length() == 0) {
            String error = "{\"error\":\"OTA URL not configured for device type: " + deviceTypeId + "\"}";
            request->send(200, "application/json", error);
            return;
        }
        if (!baseUrl.endsWith("/")) baseUrl += "/";

        // Fetch version.txt
        std::unique_ptr<WiFiClient> client;
        if (baseUrl.startsWith("https://")) {
            std::unique_ptr<WiFiClientSecure> secureClient(new WiFiClientSecure());
            secureClient->setInsecure();
            client = std::move(secureClient);
        } else {
            client = std::unique_ptr<WiFiClient>(new WiFiClient());
        }

        HTTPClient http;
        String versionUrl = baseUrl + "version.txt";
        versionUrl += "?ts=" + String(millis());
        if (!http.begin(*client, versionUrl)) {
            request->send(200, "application/json", "{\"error\":\"Failed to connect to version URL\"}");
            return;
        }

        int httpCode = http.GET();
        if (httpCode != HTTP_CODE_OK) {
            http.end();
            String response = "{\"error\":\"version.txt not found (HTTP " + String(httpCode) + ")\"}";
            request->send(200, "application/json", response);
            return;
        }

        String versionStr = http.getString();
        http.end();
        versionStr.trim();
        if (versionStr.length() > 0 && (versionStr[0] == 'v' || versionStr[0] == 'V')) {
            versionStr = versionStr.substring(1);
            versionStr.trim();
        }
        int availableVersion = versionStr.toInt();

        // Get current version from the first online device of this type
        int currentVersion = 0;
        File devFile = FS_USER.open("/config/devices.json", "r");
        if (devFile) {
            DynamicJsonDocument devDoc(8192);
            if (!deserializeJson(devDoc, devFile)) {
                JsonArray devices = devDoc["devices"];
                for (JsonObject device : devices) {
                    String type = device["type"].as<String>();
                    if (type.length() == 0) continue;

                    String typeUpper = type;
                    typeUpper.toUpperCase();
                    if (typeUpper != deviceTypeUpper) continue;

                    String macStr = device["mac"].as<String>();
                    if (macStr.length() == 0) continue;

                    uint8_t mac[6];
                    if (sscanf(macStr.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                              &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6) {
                        continue;
                    }

                    if (ESPNowManager::getInstance().isPeerOnline(mac)) {
                        currentVersion = device["firmwareVersion"] | 0;
                        break;
                    }
                }
            }
            devFile.close();
        }

        bool hasUpdate = (availableVersion > currentVersion);

        // Check for firmware.bin and node_config.txt
        bool hasFirmware = false;
        bool hasConfig = false;

        if (!http.begin(*client, baseUrl + "firmware.bin")) {
            Serial.println(" Failed to begin firmware.bin check");
        } else {
            httpCode = http.sendRequest("HEAD");
            hasFirmware = (httpCode == HTTP_CODE_OK);
            http.end();
        }

        if (!http.begin(*client, baseUrl + "node_config.txt")) {
            Serial.println(" Failed to begin node_config.txt check");
        } else {
            httpCode = http.sendRequest("HEAD");
            hasConfig = (httpCode == HTTP_CODE_OK);
            http.end();
        }

        // Build response
        DynamicJsonDocument responseDoc(256);
        responseDoc["hasUpdate"] = hasUpdate;
        responseDoc["currentVersion"] = currentVersion;
        responseDoc["availableVersion"] = availableVersion;
        responseDoc["hasFirmware"] = hasFirmware;
        responseDoc["hasConfig"] = hasConfig;
        responseDoc["deviceType"] = deviceTypeId;

        String response;
        serializeJson(responseDoc, response);
        request->send(200, "application/json", response);
    });

    // POST Apply device update (async): /api/nodes/{type}/apply-update
    server.on("^\\/api\\/nodes\\/([a-z_]+)\\/apply-update$", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        if (index == 0) {
            Serial.println("[NodeOTA] Received apply-update request");
        }

        if (index + len != total) {
            return;
        }

        String deviceTypeId = request->pathArg(0);
        Serial.printf("[NodeOTA] POST /api/nodes/%s/apply-update\n", deviceTypeId.c_str());

        // Check if OTA already in progress
        if (nodeOtaState.active) {
            request->send(200, "application/json", "{\"success\":false,\"error\":\"OTA transfer already in progress\"}");
            return;
        }

        // Get OTA URL from ota.json
        String baseUrl = getDeviceTypeOtaUrl(deviceTypeId);
        if (baseUrl.length() == 0) {
            String error = "{\"success\":false,\"error\":\"OTA URL not configured for device type: " + deviceTypeId + "\"}";
            request->send(200, "application/json", error);
            return;
        }
        if (!baseUrl.endsWith("/")) baseUrl += "/";

        DynamicJsonDocument bodyDoc(1024);
        DeserializationError bodyError = deserializeJson(bodyDoc, data, len);
        if (bodyError) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON\"}");
            return;
        }

        JsonArray macs = bodyDoc["macs"].as<JsonArray>();
        if (macs.isNull() || macs.size() == 0) {
            request->send(200, "application/json", "{\"success\":false,\"error\":\"No devices selected\"}");
            return;
        }

        uint8_t targetCount = 0;
        uint8_t targetMacs[10][6] = {0};

        for (JsonVariant macValue : macs) {
            if (targetCount >= 10) break;
            String macStr = macValue.as<String>();
            if (macStr.length() == 0) continue;

            uint8_t mac[6];
            if (sscanf(macStr.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                      &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6) {
                continue;
            }

            if (!ESPNowManager::getInstance().isPeerOnline(mac)) {
                continue;
            }

            memcpy(targetMacs[targetCount], mac, 6);
            targetCount++;
        }

        if (targetCount == 0) {
            String error = "{\"success\":false,\"error\":\"No selected online " + deviceTypeId + " devices found\"}";
            request->send(200, "application/json", error);
            return;
        }

        // Initialize OTA state
        memset(&nodeOtaState, 0, sizeof(nodeOtaState));
        nodeOtaState.active = true;
        nodeOtaState.baseUrl = baseUrl;
        nodeOtaState.deviceType = deviceTypeId;
        nodeOtaState.targetCount = targetCount;
        memcpy(nodeOtaState.targetMacs, targetMacs, sizeof(targetMacs));
        memcpy(nodeOtaState.targetMac, targetMacs[0], 6);  // First target

        // Start OTA task
        xTaskCreatePinnedToCore(
            nodeOtaTask,
            "NodeOTA",
            8192,
            NULL,
            1,
            &nodeOtaTaskHandle,
            0  // Core 0
        );

        Serial.printf("[NodeOTA] Started async transfer to %d %s device(s)\n", targetCount, deviceTypeId.c_str());
        for (int i = 0; i < targetCount; i++) {
            Serial.printf("  [%d] %02X:%02X:%02X:%02X:%02X:%02X\n", i + 1,
                          targetMacs[i][0], targetMacs[i][1], targetMacs[i][2],
                          targetMacs[i][3], targetMacs[i][4], targetMacs[i][5]);
        }

        char response[128];
        snprintf(response, sizeof(response),
                 "{\"success\":true,\"status\":\"started\",\"message\":\"OTA transfer started for %d device(s)\",\"deviceCount\":%d}",
                 targetCount, targetCount);
        request->send(200, "application/json", response);
    });

    // GET OTA transfer status: /api/nodes/{type}/ota-status (type is ignored - single OTA state)
    server.on("^\\/api\\/nodes\\/([a-z_]+)\\/ota-status$", HTTP_GET, [](AsyncWebServerRequest *request){
        DynamicJsonDocument doc(512);
        doc["active"] = nodeOtaState.active;
        doc["completed"] = nodeOtaState.completed;
        doc["success"] = nodeOtaState.success;
        doc["error"] = nodeOtaState.error;
        doc["configSaved"] = nodeOtaState.configSaved;
        doc["configSent"] = nodeOtaState.configSent;
        doc["configChunks"] = nodeOtaState.configChunksSent;
        doc["configTotalChunks"] = nodeOtaState.totalConfigChunks;
        doc["firmwareSaved"] = nodeOtaState.firmwareSaved;
        doc["firmwareSent"] = nodeOtaState.firmwareSent;
        doc["firmwareChunks"] = nodeOtaState.firmwareChunksSent;
        doc["firmwareTotalChunks"] = nodeOtaState.totalFirmwareChunks;
        doc["deviceCount"] = nodeOtaState.targetCount;
        doc["currentDevice"] = nodeOtaState.currentTarget + 1;
        doc["devicesUpdated"] = nodeOtaState.devicesUpdated;
        doc["devicesFailed"] = nodeOtaState.devicesFailed;

        if (nodeOtaState.targetCount > 0 && nodeOtaState.currentTarget < nodeOtaState.targetCount) {
            String currentMac = macToString(nodeOtaState.targetMacs[nodeOtaState.currentTarget]);
            doc["currentDeviceMac"] = currentMac;

            // Attempt to resolve device name from devices.json
            String currentName = "";
            File devFile = FS_USER.open("/config/devices.json", "r");
            if (devFile) {
                DynamicJsonDocument devDoc(8192);
                if (!deserializeJson(devDoc, devFile)) {
                    JsonArray devices = devDoc["devices"];
                    for (JsonObject device : devices) {
                        String macStr = device["mac"].as<String>();
                        if (macStr.length() == 0) continue;
                        if (macStr.equalsIgnoreCase(currentMac)) {
                            currentName = device["name"] | "";
                            break;
                        }
                    }
                }
                devFile.close();
            }
            if (currentName.length() > 0) {
                doc["currentDeviceName"] = currentName;
            }
        }
        
        if (nodeOtaState.totalFirmwareChunks > 0) {
            doc["progress"] = (nodeOtaState.firmwareChunksSent * 100) / nodeOtaState.totalFirmwareChunks;
        } else {
            doc["progress"] = 0;
        }

        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });
    
    // POST provision device
    server.on("/api/provision-device", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        if (index == 0) {
            Serial.println(" Received provision-device request");
        }
        
        if (index + len == total) {
            // Parse JSON
            DynamicJsonDocument doc(1024);
            DeserializationError error = deserializeJson(doc, data, len);
            
            if (error) {
                Serial.printf(" JSON parse error: %s\n", error.c_str());
                request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON\"}");
                return;
            }
            
            // Extract device info
            String macStr = doc["mac"].as<String>();
            String deviceName = doc["name"].as<String>();
            uint8_t tankId = doc["tankId"];
            
            Serial.printf(" Provisioning device: %s -> %s (Tank %d)\n",
                         macStr.c_str(), deviceName.c_str(), tankId);
            
            // Convert MAC string to bytes
            uint8_t mac[6];
            if (sscanf(macStr.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                      &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6) {
                request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid MAC address\"}");
                return;
            }
            
            // Load unmapped devices
            File unmappedFile = FS_USER.open("/config/unmapped-devices.json", "r");
            if (!unmappedFile) {
                request->send(404, "application/json", "{\"success\":false,\"error\":\"Unmapped devices file not found\"}");
                return;
            }
            
            DynamicJsonDocument unmappedDoc(4096);
            deserializeJson(unmappedDoc, unmappedFile);
            unmappedFile.close();
            
            // Find device in unmapped list
            JsonArray unmappedDevices = unmappedDoc["unmappedDevices"];
            int foundIndex = -1;
            JsonObject foundDevice;
            
            for (size_t i = 0; i < unmappedDevices.size(); i++) {
                if (unmappedDevices[i]["mac"].as<String>() == macStr) {
                    foundIndex = i;
                    foundDevice = unmappedDevices[i];
                    break;
                }
            }
            
            if (foundIndex == -1) {
                request->send(404, "application/json", "{\"success\":false,\"error\":\"Device not found in unmapped list\"}");
                return;
            }
            
            // Send CONFIG message to node
            if (!ESPNowManager::getInstance().addPeer(mac)) {
                Serial.println(" Failed to add peer before CONFIG send");
                request->send(500, "application/json", "{\"success\":false,\"error\":\"Failed to add ESP-NOW peer. Ensure device is powered on and announcing.\"}");
                return;
            }

            ConfigMessage configMsg = {};
            configMsg.header.type = MessageType::CONFIG;
            configMsg.header.tankId = tankId;
            configMsg.header.nodeType = NodeType::HUB;
            configMsg.header.timestamp = millis();
            configMsg.header.sequenceNum = 0;
            strncpy(configMsg.deviceName, deviceName.c_str(), MAX_NODE_NAME_LEN - 1);
            
            bool sent = ESPNowManager::getInstance().send(mac, (uint8_t*)&configMsg, sizeof(configMsg));
            
            if (!sent) {
                Serial.println(" Failed to send CONFIG message");
                request->send(500, "application/json", "{\"success\":false,\"error\":\"Failed to send CONFIG to device\"}");
                return;
            }
            
            Serial.println(" CONFIG message sent to device");
            
            // Preserve found device details before we modify the unmapped list
            String foundTypeStr = foundDevice["type"].as<String>();
            if (foundTypeStr.length() == 0) foundTypeStr = "UNKNOWN";
            uint8_t foundFw = foundDevice["firmwareVersion"].as<uint8_t>();

            // Remove all entries for this MAC from unmapped devices (avoid duplicates/re-appear race)
            for (int i = (int)unmappedDevices.size() - 1; i >= 0; i--) {
                if (unmappedDevices[i]["mac"].as<String>() == macStr) {
                    unmappedDevices.remove(i);
                }
            }

            // Save updated unmapped devices
            unmappedFile = FS_USER.open("/config/unmapped-devices.json", "w");
            serializeJson(unmappedDoc, unmappedFile);
            unmappedFile.close();

            // Remove from deleted-devices.json (allow re-provisioning)
            File deletedFile = FS_USER.open("/config/deleted-devices.json", "r");
            DynamicJsonDocument deletedDoc(2048);
            if (deletedFile) {
                deserializeJson(deletedDoc, deletedFile);
                deletedFile.close();

                JsonArray deletedDevices = deletedDoc["deletedDevices"];
                if (!deletedDevices.isNull()) {
                    for (int i = (int)deletedDevices.size() - 1; i >= 0; i--) {
                        if (deletedDevices[i]["mac"].as<String>().equalsIgnoreCase(macStr)) {
                            deletedDevices.remove(i);
                        }
                    }
                    deletedFile = FS_USER.open("/config/deleted-devices.json", "w");
                    serializeJson(deletedDoc, deletedFile);
                    deletedFile.close();
                }
            }

            // Add to devices.json (create devices array if missing)
            File devicesFile = FS_USER.open("/config/devices.json", "r");
            DynamicJsonDocument devicesDoc(8192);
            if (devicesFile) {
                deserializeJson(devicesDoc, devicesFile);
                devicesFile.close();
            }

            JsonArray devices = devicesDoc["devices"];
            if (devices.isNull()) {
                devices = devicesDoc.createNestedArray("devices");
            }

            // Prefer type provided by client (doc["type"]) if present, otherwise use discovered type
            String requestedType = String();
            if (doc.containsKey("type")) {
                requestedType = doc["type"].as<String>();
            }
            String finalType = (requestedType.length() > 0) ? requestedType : foundTypeStr;
            finalType.toUpperCase();

            JsonObject newDevice = devices.createNestedObject();
            newDevice["mac"] = macStr;
            newDevice["type"] = finalType;
            newDevice["name"] = deviceName;
            newDevice["tankId"] = tankId;
            newDevice["firmwareVersion"] = (doc.containsKey("firmwareVersion") ? doc["firmwareVersion"].as<uint8_t>() : foundFw);
            newDevice["enabled"] = true;
            newDevice["status"] = "PROVISIONING";

            devicesFile = FS_USER.open("/config/devices.json", "w");
            serializeJson(devicesDoc, devicesFile);
            devicesFile.close();
            
            Serial.printf(" Device provisioned: %s\n", deviceName.c_str());
            notifier.emitf("config.device.add", NTFY_MSG_DEVICE_PROVISIONED,
                            deviceName.c_str(), macStr.c_str(), (int)tankId);
            
            // **NOTE**: Device object creation disabled until Device subclass .cpp files exist
            // TEMPORARY WORKAROUND: Devices are tracked via JSON file only
            // This is sufficient for deviceCount calculation in /api/aquariums
            /*
            Aquarium* aquarium = AquariumManager::getInstance().getAquarium(tankId);
            if (aquarium) {
                NodeType deviceType = static_cast<NodeType>(foundDevice["type"].as<uint8_t>());
                Device* device = createDevice(mac, deviceType, deviceName.c_str());
                
                if (device) {
                    device->setTankId(tankId);
                    device->setEnabled(true);
                    device->setFirmwareVersion(foundDevice["firmwareVersion"].as<uint8_t>());
                    
                    if (aquarium->addDevice(device)) {
                        Serial.printf(" [OK] Device added to aquarium object (deviceCount now: %d)\n", 
                                     aquarium->getDeviceCount());
                    } else {
                        Serial.println(" [WARN] Failed to add device to aquarium object");
                        delete device;
                    }
                } else {
                    Serial.println(" [WARN] Failed to create device object (unknown type)");
                }
            } else {
                Serial.printf(" [WARN] Aquarium %d not found in memory!\n", tankId);
            }
            */
            
            // Send success response
            String response = "{\"success\":true,\"device\":{";
            response += "\"mac\":\"" + macStr + "\",";
            response += "\"name\":\"" + deviceName + "\",";
            response += "\"tankId\":" + String(tankId) + ",";
            response += "\"status\":\"PROVISIONED\"";
            response += "}}";
            
            request->send(200, "application/json", response);
        }
    });

    // POST delete device (remove from devices.json, unmapped-devices.json, light-devices.json, light-schedule.json)
    server.on("/api/delete-device", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        if (index == 0) {
            Serial.println(" Received delete-device request");
        }
        if (index + len != total) return;

        DynamicJsonDocument doc(512);
        DeserializationError error = deserializeJson(doc, data, len);
        if (error) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON\"}");
            return;
        }

        String macStr = doc["mac"].as<String>();
        if (macStr.length() == 0) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing mac\"}");
            return;
        }

        Serial.printf(" Deleting device: %s\n", macStr.c_str());

        // Normalize
        macStr.toUpperCase();

        // Remove from devices.json
        File devicesFile = FS_USER.open("/config/devices.json", "r");
        DynamicJsonDocument devicesDoc(8192);
        if (devicesFile) {
            deserializeJson(devicesDoc, devicesFile);
            devicesFile.close();
        }
        JsonArray devices = devicesDoc["devices"];
        if (!devices.isNull()) {
            for (int i = (int)devices.size() - 1; i >= 0; i--) {
                if (devices[i]["mac"].as<String>() == macStr) {
                    devices.remove(i);
                }
            }
            devicesFile = FS_USER.open("/config/devices.json", "w");
            serializeJson(devicesDoc, devicesFile);
            devicesFile.close();
        }

        // Remove from unmapped-devices.json
        File unmappedFile = FS_USER.open("/config/unmapped-devices.json", "r");
        DynamicJsonDocument unmappedDoc(4096);
        if (unmappedFile) {
            deserializeJson(unmappedDoc, unmappedFile);
            unmappedFile.close();
        }
        JsonArray unmapped = unmappedDoc["unmappedDevices"];
        if (!unmapped.isNull()) {
            for (int i = (int)unmapped.size() - 1; i >= 0; i--) {
                if (unmapped[i]["mac"].as<String>() == macStr) {
                    unmapped.remove(i);
                }
            }
            unmappedFile = FS_USER.open("/config/unmapped-devices.json", "w");
            serializeJson(unmappedDoc, unmappedFile);
            unmappedFile.close();
        }

        // Remove from light-devices.json
        File lightFile = FS_USER.open("/config/light-devices.json", "r");
        DynamicJsonDocument lightDoc(8192);
        if (lightFile) {
            deserializeJson(lightDoc, lightFile);
            lightFile.close();
        }
        JsonArray lightDevices = lightDoc["lightDevices"];
        if (!lightDevices.isNull()) {
            for (int i = (int)lightDevices.size() - 1; i >= 0; i--) {
                if (lightDevices[i]["mac"].as<String>() == macStr) {
                    lightDevices.remove(i);
                }
            }
            lightFile = FS_USER.open("/config/light-devices.json", "w");
            serializeJson(lightDoc, lightFile);
            lightFile.close();
        }

        // Remove from light-schedule.json
        File schedFile = FS_USER.open("/config/schedule/light-schedule.json", "r");
        DynamicJsonDocument schedDoc(4096);
        if (schedFile) {
            deserializeJson(schedDoc, schedFile);
            schedFile.close();
        }
        JsonArray schedules = schedDoc["schedules"];
        if (!schedules.isNull()) {
            for (int i = (int)schedules.size() - 1; i >= 0; i--) {
                if (schedules[i]["mac"].as<String>() == macStr) {
                    schedules.remove(i);
                }
            }
            schedFile = FS_USER.open("/config/schedule/light-schedule.json", "w");
            serializeJson(schedDoc, schedFile);
            schedFile.close();
        }

        // Remove from feeder-schedule.json
        File feederSchedFile = FS_USER.open("/config/schedule/feeder-schedule.json", "r");
        DynamicJsonDocument feederSchedDoc(4096);
        if (feederSchedFile) {
            deserializeJson(feederSchedDoc, feederSchedFile);
            feederSchedFile.close();
        }
        JsonArray feederSchedules = feederSchedDoc["schedules"];
        if (!feederSchedules.isNull()) {
            for (int i = (int)feederSchedules.size() - 1; i >= 0; i--) {
                if (feederSchedules[i]["mac"].as<String>() == macStr) {
                    feederSchedules.remove(i);
                }
            }
            feederSchedFile = FS_USER.open("/config/schedule/feeder-schedule.json", "w");
            serializeJson(feederSchedDoc, feederSchedFile);
            feederSchedFile.close();
        }

        // Remove from feeder-calibration.json
        File calFile = FS_USER.open("/config/feeder-calibration.json", "r");
        DynamicJsonDocument calDoc(2048);
        if (calFile) {
            deserializeJson(calDoc, calFile);
            calFile.close();
        }
        JsonArray calibrations = calDoc["calibrations"];
        if (!calibrations.isNull()) {
            for (int i = (int)calibrations.size() - 1; i >= 0; i--) {
                if (calibrations[i]["mac"].as<String>() == macStr) {
                    calibrations.remove(i);
                }
            }
            calFile = FS_USER.open("/config/feeder-calibration.json", "w");
            serializeJson(calDoc, calFile);
            calFile.close();
        }

        // Remove from wm-schedule.json
        File wmSchedFile = FS_USER.open("/config/schedule/wm-schedule.json", "r");
        DynamicJsonDocument wmSchedDoc(4096);
        if (wmSchedFile) {
            deserializeJson(wmSchedDoc, wmSchedFile);
            wmSchedFile.close();
        }
        JsonArray wmSchedules = wmSchedDoc["schedules"];
        if (!wmSchedules.isNull()) {
            for (int i = (int)wmSchedules.size() - 1; i >= 0; i--) {
                if (wmSchedules[i]["mac"].as<String>() == macStr) {
                    wmSchedules.remove(i);
                }
            }
            wmSchedFile = FS_USER.open("/config/schedule/wm-schedule.json", "w");
            serializeJson(wmSchedDoc, wmSchedFile);
            wmSchedFile.close();
        }

        // Add to deleted-devices.json so future ANNOUNCE/HEARTBEAT won't re-add to unmapped
        File deletedFile = FS_USER.open("/config/deleted-devices.json", "r");
        DynamicJsonDocument deletedDoc(2048);
        if (deletedFile) {
            deserializeJson(deletedDoc, deletedFile);
            deletedFile.close();
        } else {
            deletedDoc["metadata"]["totalDeleted"] = 0;
        }

        JsonArray deletedDevices = deletedDoc["deletedDevices"];
        if (deletedDevices.isNull()) {
            deletedDevices = deletedDoc.createNestedArray("deletedDevices");
        }

        for (int i = (int)deletedDevices.size() - 1; i >= 0; i--) {
            if (deletedDevices[i]["mac"].as<String>().equalsIgnoreCase(macStr)) {
                deletedDevices.remove(i);
            }
        }

        JsonObject newDeleted = deletedDevices.createNestedObject();
        newDeleted["mac"] = macStr;
        newDeleted["deletedAt"] = millis();
        deletedDoc["metadata"]["totalDeleted"] = (int)deletedDevices.size();

        deletedFile = FS_USER.open("/config/deleted-devices.json", "w");
        serializeJson(deletedDoc, deletedFile);
        deletedFile.close();

        // Also remove from memory and peer list
        uint8_t mac[6];
        if (sscanf(macStr.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                  &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6) {
            AquariumManager::getInstance().removeDevice(mac);
            ESPNowManager::getInstance().removePeer(mac);
        }

        // Rebuild next-task.json to purge any remaining tasks for this device
        rebuildNextTasks();

        Serial.printf(" [OK] Device deleted: %s (cascaded: schedules/calibration/next-tasks)\n", macStr.c_str());
        notifier.emitf("config.device.delete", NTFY_MSG_DEVICE_DELETED, macStr.c_str());
        request->send(200, "application/json", "{\"success\":true}");
    });

    // POST device command (send command to specific device)
    server.on("^\\/api\\/devices\\/([0-9a-f]{2}:[0-9a-f]{2}:[0-9a-f]{2}:[0-9a-f]{2}:[0-9a-f]{2}:[0-9a-f]{2})\\/command$", HTTP_POST,
        [](AsyncWebServerRequest *request){}, NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        if (index + len != total) return;  // Wait for full body
        
        String macStr = request->pathArg(0);
        Serial.printf(" Received device command request for %s\\n", macStr.c_str());
        
        // Parse MAC address
        uint8_t mac[6];
        if (sscanf(macStr.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                  &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid MAC address\"}");
            return;
        }
        
        // Parse JSON command
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, (const char*)data, len);
        
        if (error) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON\"}");
            return;
        }
        
        // Build command message
        CommandMessage cmd = {};
        cmd.header.type = MessageType::COMMAND;
        cmd.header.tankId = 0;  // Will be filled from device registry
        cmd.header.nodeType = NodeType::HUB;
        cmd.header.timestamp = millis();
        cmd.header.sequenceNum = 0;
        cmd.commandId = generateCommandId();
        cmd.commandSeqID = 0;
        cmd.finalCommand = true;
        
        // Extract command data from JSON
        String commandType = doc["command"] | "";
        if (commandType.isEmpty()) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing command field\"}");
            return;
        }
        
        // Map command string to command code
        if (commandType == "TURN_ON") {
            cmd.commandData[0] = 0x01;
        } else if (commandType == "TURN_OFF") {
            cmd.commandData[0] = 0x02;
        } else if (commandType == "SET_LEVEL") {
            cmd.commandData[0] = 0x03;
            cmd.commandData[1] = doc["level"] | 0;
        } else if (commandType == "SET_RGB") {
            cmd.commandData[0] = 0x04;
            cmd.commandData[1] = doc["white"] | 0;
            cmd.commandData[2] = doc["blue"] | 0;
            cmd.commandData[3] = doc["red"] | 0;
        } else if (commandType == "FEED") {
            cmd.commandData[0] = 0x05;
            cmd.commandData[1] = doc["portions"] | 1;
        } else if (commandType == "LIGHT_CH1_ON") {
            cmd.commandData[0] = 0x0B;
        } else if (commandType == "LIGHT_CH1_OFF") {
            cmd.commandData[0] = 0x0A;
        } else if (commandType == "LIGHT_CH2_ON") {
            cmd.commandData[0] = 0x15;
        } else if (commandType == "LIGHT_CH2_OFF") {
            cmd.commandData[0] = 0x14;
        } else if (commandType == "LIGHT_CH3_ON") {
            cmd.commandData[0] = 0x1F;
        } else if (commandType == "LIGHT_CH3_OFF") {
            cmd.commandData[0] = 0x1E;
        } else if (commandType == "LIGHT_STATUS") {
            cmd.commandData[0] = 0x28;
        } else {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Unknown command type\"}");
            return;
        }
        
        // Include Hub AP MAC in command so node knows where to reply
        uint8_t apMac[6] = {0};
#ifdef ESP32
        esp_read_mac(apMac, ESP_MAC_WIFI_SOFTAP);
#else
        String apStr = WiFi.softAPmacAddress();
        sscanf(apStr.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
               &apMac[0], &apMac[1], &apMac[2], &apMac[3], &apMac[4], &apMac[5]);
#endif
        memcpy(cmd.returnMac, apMac, 6);

        // Re-add peer using AP interface for unicast robustness
#ifdef ESP32
        ESPNowManager::getInstance().addPeer(mac, WIFI_IF_AP);
#else
        ESPNowManager::getInstance().addPeer(mac);
#endif

        // Send command via ESP-NOW
        bool sent = ESPNowManager::getInstance().send(mac, (uint8_t*)&cmd, sizeof(cmd));
        
        if (!sent) {
            request->send(500, "application/json", "{\"success\":false,\"error\":\"Failed to send command to device\"}");
            return;
        }
        
        // Return success
        JsonDocument response;
        response["success"] = true;
        response["commandId"] = cmd.commandId;
        response["message"] = "Command sent successfully";
        
        String responseStr;
        serializeJson(response, responseStr);
        request->send(200, "application/json", responseStr);
        
        // Activity log for ad-hoc device command
        {
            String devName, aqName, devType;
            lookupDeviceAndAquariumNames(macStr, devName, aqName, devType);
            // Derive category from command type
            const char* cat = "device";
            if (commandType.startsWith("LIGHT") || commandType == "TURN_ON" || commandType == "TURN_OFF" || commandType == "SET_RGB") cat = "light";
            else if (commandType == "FEED") cat = "feeder";
            appendActivityLog("adhoc", cat, commandType.c_str(), devName.c_str(), aqName.c_str());
        }

        Serial.printf(" [OK] Command sent to device %s\\n", macStr.c_str());
    });
    
    // POST unmap device
    server.on("/api/unmap-device", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        if (index == 0) {
            Serial.println(" Received unmap-device request");
        }
        
        if (index + len == total) {
            // Parse JSON
            DynamicJsonDocument doc(512);
            DeserializationError error = deserializeJson(doc, data, len);
            
            if (error) {
                Serial.printf(" JSON parse error: %s\n", error.c_str());
                request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON\"}");
                return;
            }
            
            // Extract device MAC
            String macStr = doc["mac"].as<String>();
            Serial.printf(" Unmapping device: %s\n", macStr.c_str());
            
            // Convert MAC string to bytes
            uint8_t mac[6];
            if (sscanf(macStr.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                      &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6) {
                request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid MAC address\"}");
                return;
            }
            
            // Load devices.json (if missing, treat as empty and still proceed)
            DynamicJsonDocument devicesDoc(8192);
            File devicesFile = FS_USER.open("/config/devices.json", "r");
            if (devicesFile) {
                deserializeJson(devicesDoc, devicesFile);
                devicesFile.close();
            } else {
                devicesDoc["devices"] = JsonArray();
            }
            
            // Find and remove device
            JsonArray devices = devicesDoc["devices"];
            int foundIndex = -1;
            JsonObject foundDevice;
            
            for (size_t i = 0; i < devices.size(); i++) {
                if (devices[i]["mac"].as<String>() == macStr) {
                    foundIndex = i;
                    foundDevice = devices[i];
                    break;
                }
            }
            
            if (foundIndex == -1) {
                // Device not found in registry; still send UNMAP and add to unmapped list
                foundDevice = devicesDoc.createNestedObject();
                foundDevice["type"] = "UNKNOWN";
                foundDevice["firmwareVersion"] = 0;
                foundDevice["tankId"] = 0;
            }
            
            // Send UNMAP message to node
            UnmapMessage unmapMsg = {};
            unmapMsg.header.type = MessageType::UNMAP;
            unmapMsg.header.tankId = 0;  // Reset to unmapped
            unmapMsg.header.nodeType = NodeType::HUB;
            unmapMsg.header.timestamp = millis();
            unmapMsg.header.sequenceNum = 0;
            unmapMsg.reason = 1;  // User-initiated unmap
            
            bool sent = ESPNowManager::getInstance().send(mac, (uint8_t*)&unmapMsg, sizeof(unmapMsg));
            
            if (sent) {
                Serial.println(" UNMAP message sent to device");
            } else {
                Serial.println(" Warning: Failed to send UNMAP message (device may be offline)");
            }
            
            // Remove from devices.json if present
            // Capture important metadata BEFORE removing the object to avoid invalid references
            String detectedType = "UNKNOWN";
            uint8_t detectedFirmware = 0;
            uint8_t tankId = 0;

            if (foundIndex != -1) {
                tankId = foundDevice["tankId"].as<uint8_t>();
                detectedType = foundDevice.containsKey("type") ? foundDevice["type"].as<String>() : String("UNKNOWN");
                if (detectedType.length() == 0) detectedType = "UNKNOWN";
                detectedFirmware = foundDevice.containsKey("firmwareVersion") ? foundDevice["firmwareVersion"].as<uint8_t>() : 0;

                devices.remove(foundIndex);

                devicesFile = FS_USER.open("/config/devices.json", "w");
                serializeJson(devicesDoc, devicesFile);
                devicesFile.close();
            }

            // Remove from in-memory registry and ESP-NOW peer tracking
            AquariumManager::getInstance().removeDevice(mac);
            ESPNowManager::getInstance().removePeer(mac);
            
            // **NOTE**: Device object removal disabled until Device subclass .cpp files exist
            // Devices are only tracked in JSON for now
            /*
            Aquarium* aquarium = AquariumManager::getInstance().getAquarium(tankId);
            if (aquarium && aquarium->hasDevice(mac)) {
                if (aquarium->removeDevice(mac)) {
                    Serial.printf(" [OK] Device removed from aquarium object (deviceCount now: %d)\n", 
                                 aquarium->getDeviceCount());
                } else {
                    Serial.println(" [WARN] Failed to remove device from aquarium object");
                }
            } else {
                Serial.printf(" [WARN] Aquarium %d not found or device not in aquarium!\n", tankId);
            }
            */

            // Normalize MAC for case-insensitive comparison
            String macUpper = macStr;
            macUpper.toUpperCase();

            // Remove from light-devices.json
            {
                File f = FS_USER.open("/config/light-devices.json", "r");
                DynamicJsonDocument ldDoc(8192);
                if (f) { deserializeJson(ldDoc, f); f.close(); }
                JsonArray arr = ldDoc["lightDevices"];
                if (!arr.isNull()) {
                    for (int i = (int)arr.size() - 1; i >= 0; i--) {
                        String m = arr[i]["mac"].as<String>(); m.toUpperCase();
                        if (m == macUpper) arr.remove(i);
                    }
                    f = FS_USER.open("/config/light-devices.json", "w");
                    serializeJson(ldDoc, f); f.close();
                }
            }

            // Remove from light-schedule.json
            {
                File f = FS_USER.open("/config/schedule/light-schedule.json", "r");
                DynamicJsonDocument lsDoc(4096);
                if (f) { deserializeJson(lsDoc, f); f.close(); }
                JsonArray arr = lsDoc["schedules"];
                if (!arr.isNull()) {
                    for (int i = (int)arr.size() - 1; i >= 0; i--) {
                        String m = arr[i]["mac"].as<String>(); m.toUpperCase();
                        if (m == macUpper) arr.remove(i);
                    }
                    f = FS_USER.open("/config/schedule/light-schedule.json", "w");
                    serializeJson(lsDoc, f); f.close();
                }
            }

            // Remove from feeder-schedule.json
            {
                File f = FS_USER.open("/config/schedule/feeder-schedule.json", "r");
                DynamicJsonDocument fsDoc(4096);
                if (f) { deserializeJson(fsDoc, f); f.close(); }
                JsonArray arr = fsDoc["schedules"];
                if (!arr.isNull()) {
                    for (int i = (int)arr.size() - 1; i >= 0; i--) {
                        String m = arr[i]["mac"].as<String>(); m.toUpperCase();
                        if (m == macUpper) arr.remove(i);
                    }
                    f = FS_USER.open("/config/schedule/feeder-schedule.json", "w");
                    serializeJson(fsDoc, f); f.close();
                }
            }

            // Remove from feeder-calibration.json
            {
                File f = FS_USER.open("/config/feeder-calibration.json", "r");
                DynamicJsonDocument fcDoc(2048);
                if (f) { deserializeJson(fcDoc, f); f.close(); }
                JsonArray arr = fcDoc["calibrations"];
                if (!arr.isNull()) {
                    for (int i = (int)arr.size() - 1; i >= 0; i--) {
                        String m = arr[i]["mac"].as<String>(); m.toUpperCase();
                        if (m == macUpper) arr.remove(i);
                    }
                    f = FS_USER.open("/config/feeder-calibration.json", "w");
                    serializeJson(fcDoc, f); f.close();
                }
            }

            // Remove from wm-schedule.json
            {
                File f = FS_USER.open("/config/schedule/wm-schedule.json", "r");
                DynamicJsonDocument wmDoc(4096);
                if (f) { deserializeJson(wmDoc, f); f.close(); }
                JsonArray arr = wmDoc["schedules"];
                if (!arr.isNull()) {
                    for (int i = (int)arr.size() - 1; i >= 0; i--) {
                        String m = arr[i]["mac"].as<String>(); m.toUpperCase();
                        if (m == macUpper) arr.remove(i);
                    }
                    f = FS_USER.open("/config/schedule/wm-schedule.json", "w");
                    serializeJson(wmDoc, f); f.close();
                }
            }
            
            // Add back to unmapped devices
            File unmappedFile = FS_USER.open("/config/unmapped-devices.json", "r");
            DynamicJsonDocument unmappedDoc(4096);
            if (unmappedFile) {
                deserializeJson(unmappedDoc, unmappedFile);
                unmappedFile.close();
            } else {
                unmappedDoc["metadata"]["lastCleanup"] = 0;
                unmappedDoc["metadata"]["totalDiscovered"] = 0;
                unmappedDoc["metadata"]["autoCleanupAfterDays"] = 7;
            }

            JsonArray unmappedDevices = unmappedDoc["unmappedDevices"];
            if (unmappedDevices.isNull()) {
                unmappedDevices = unmappedDoc.createNestedArray("unmappedDevices");
            }

            // Remove any existing entries for this MAC to avoid duplicates
            for (int i = (int)unmappedDevices.size() - 1; i >= 0; i--) {
                if (unmappedDevices[i]["mac"].as<String>() == macStr) {
                    unmappedDevices.remove(i);
                }
            }

            // Use captured metadata if available (captured earlier to avoid invalid ref after removal)
            JsonObject newUnmapped = unmappedDevices.createNestedObject();
            newUnmapped["mac"] = macStr;
            newUnmapped["type"] = detectedType;
            newUnmapped["firmwareVersion"] = detectedFirmware;
            newUnmapped["discoveredAt"] = millis();
            newUnmapped["announceCount"] = 0;

            unmappedFile = FS_USER.open("/config/unmapped-devices.json", "w");
            serializeJson(unmappedDoc, unmappedFile);
            unmappedFile.close();
            
            // Rebuild next-task.json to purge any tasks for this unmapped device
            rebuildNextTasks();

            Serial.printf(" Device unmapped: %s (next-tasks rebuilt)\n", macStr.c_str());
            
            // Send success response
            String response = "{\"success\":true,\"message\":\"Device unmapped successfully\"}";
            request->send(200, "application/json", response);
        }
    });
    

    // ===== Static File Serving (MUST be registered LAST) =====
    // These catch-all handlers should come after all API routes

    // ========================================================================
    // ACTIVITY LOG API
    // ========================================================================

    // GET /api/activity-log  — returns recent activity entries (last 7 days)
    // Optional query param: limit (default 50)
    server.on("/api/activity-log", HTTP_GET, [](AsyncWebServerRequest *request){
        int limit = 50;
        if (request->hasParam("limit")) {
            limit = request->getParam("limit")->value().toInt();
            if (limit <= 0) limit = 50;
            if (limit > ACTIVITY_LOG_MAX_ENTRIES) limit = ACTIVITY_LOG_MAX_ENTRIES;
        }

        File f = FS_USER.open(ACTIVITY_LOG_PATH, "r");
        if (!f) {
            request->send(200, "application/json", "{\"entries\":[]}");
            return;
        }

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, f);
        f.close();
        if (err) {
            request->send(200, "application/json", "{\"entries\":[]}");
            return;
        }

        JsonArray all = doc["entries"].as<JsonArray>();

        // Return the most recent `limit` entries (array is chronological)
        JsonDocument resp;
        JsonArray out = resp["entries"].to<JsonArray>();
        int startIdx = (int)all.size() - limit;
        if (startIdx < 0) startIdx = 0;
        for (int i = startIdx; i < (int)all.size(); i++) {
            out.add(all[i]);
        }

        String json;
        serializeJson(resp, json);
        request->send(200, "application/json", json);
    });

    // ========================================================================
    // DIAGNOSTICS API
    // ========================================================================
    server.on("/api/diagnostics", HTTP_GET, [](AsyncWebServerRequest *request){
        DynamicJsonDocument doc(1024);

        // WiFi info
        doc["wifi"]["ssid"] = WiFi.SSID();
        doc["wifi"]["rssi"] = WiFi.RSSI();
        doc["wifi"]["ip"] = WiFi.localIP().toString();
        doc["wifi"]["channel"] = WiFi.channel();

        // ESP-NOW info
        doc["espnow"]["expectedChannel"] = config.espnowChannel;
        doc["espnow"]["currentChannel"] = WiFi.channel();
        doc["espnow"]["channelMatch"] = (WiFi.channel() == config.espnowChannel);
        auto peers = ESPNowManager::getInstance().getPeers();
        int onlineCount = 0;
        for (auto &p : peers) { if (p.online) onlineCount++; }
        doc["espnow"]["totalPeers"] = (int)peers.size();
        doc["espnow"]["onlinePeers"] = onlineCount;

        // Memory info
        doc["memory"]["freeHeap"] = ESP.getFreeHeap();
        doc["memory"]["totalHeap"] = ESP.getHeapSize();
        doc["memory"]["freePsram"] = ESP.getFreePsram();
        doc["memory"]["totalPsram"] = ESP.getPsramSize();

        // Filesystem info
#ifdef DUAL_LITTLEFS
        doc["filesystem"]["staticFs"]["total"] = StaticFS.totalBytes();
        doc["filesystem"]["staticFs"]["used"] = StaticFS.usedBytes();
        doc["filesystem"]["staticFs"]["free"] = StaticFS.totalBytes() - StaticFS.usedBytes();
        doc["filesystem"]["userFs"]["total"] = UserFS.totalBytes();
        doc["filesystem"]["userFs"]["used"] = UserFS.usedBytes();
        doc["filesystem"]["userFs"]["free"] = UserFS.totalBytes() - UserFS.usedBytes();
#else
        doc["filesystem"]["singleFs"]["total"] = LittleFS.totalBytes();
        doc["filesystem"]["singleFs"]["used"] = LittleFS.usedBytes();
        doc["filesystem"]["singleFs"]["free"] = LittleFS.totalBytes() - LittleFS.usedBytes();
#endif

        // Uptime
        doc["uptime"] = millis() / 1000;

        // LED Brightness
        doc["ledBrightness"]["wifi"]     = hubLedBrightness[0];
        doc["ledBrightness"]["device"]   = hubLedBrightness[1];
        doc["ledBrightness"]["task"]     = hubLedBrightness[2];
        doc["ledBrightness"]["unmapped"] = hubLedBrightness[3];

        String resp;
        serializeJson(doc, resp);
        request->send(200, "application/json", resp);
    });

    // ========================================================================
    // LED BRIGHTNESS API
    // ========================================================================

    // GET /api/led-brightness — returns current brightness for all 4 LEDs
    server.on("/api/led-brightness", HTTP_GET, [](AsyncWebServerRequest *request){
        JsonDocument doc;
        doc["wifi"]     = hubLedBrightness[0];
        doc["device"]   = hubLedBrightness[1];
        doc["task"]     = hubLedBrightness[2];
        doc["unmapped"] = hubLedBrightness[3];

        String resp;
        serializeJson(doc, resp);
        request->send(200, "application/json", resp);
    });

    // POST /api/led-brightness — save brightness for all 4 LEDs
    // Body: {"wifi":20,"device":20,"task":20,"unmapped":20}
    server.on("/api/led-brightness", HTTP_POST,
        // Request handler (no body here, body is in onBody)
        [](AsyncWebServerRequest *request){},
        // Upload handler (unused)
        NULL,
        // Body handler
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
            // Only process when we have the full body
            if (index + len != total) return;

            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, (const char*)data, total);
            if (err) {
                request->send(400, "application/json",
                    "{\"success\":false,\"message\":\"Invalid JSON\"}");
                return;
            }

            // Update brightness values (clamp to 0-100)
            auto clamp = [](int v) -> uint8_t {
                if (v < 0) return 0;
                if (v > 100) return 100;
                return (uint8_t)v;
            };

            if (doc.containsKey("wifi"))     hubLedBrightness[0] = clamp(doc["wifi"].as<int>());
            if (doc.containsKey("device"))   hubLedBrightness[1] = clamp(doc["device"].as<int>());
            if (doc.containsKey("task"))     hubLedBrightness[2] = clamp(doc["task"].as<int>());
            if (doc.containsKey("unmapped")) hubLedBrightness[3] = clamp(doc["unmapped"].as<int>());

            // Save to file
            saveLedBrightnessConfig();

            // Immediately apply new brightness to currently-lit LEDs
            reapplyLedBrightness();

            LOG_INFO("LED brightness updated: wifi=%d%% device=%d%% task=%d%% unmapped=%d%%",
                     hubLedBrightness[0], hubLedBrightness[1],
                     hubLedBrightness[2], hubLedBrightness[3]);

            request->send(200, "application/json",
                "{\"success\":true,\"message\":\"LED brightness saved\"}");
        }
    );

    // ========================================================================
    // WIFI RESET API
    // ========================================================================
    server.on("/api/wifi/reset", HTTP_POST, [](AsyncWebServerRequest *request){
        request->send(200, "application/json", "{\"success\":true,\"message\":\"WiFi credentials cleared. Hub will restart in AP mode.\"}");
        // Delay to let the response be sent, then reset
        delay(500);
        wifiManager.resetSettings();
        delay(500);
        ESP.restart();
    });

    // ========================================================================
    // FACTORY RESET API — wipe all user data files back to empty defaults
    // ========================================================================
    server.on("/api/factory-reset", HTTP_POST, [](AsyncWebServerRequest *request){
        Serial.println("[FACTORY RESET] ⚠️  Factory reset requested!");

        // Files to wipe (paths relative to /config/ on FS_USER)
        struct ResetFile {
            const char* path;
            const char* emptyContent;
        };

        static const ResetFile filesToReset[] = {
            { "/config/aquariums.json",                   "{\"aquariums\":[]}" },
            { "/config/devices.json",                     "{\"devices\":[]}" },
            { "/config/unmapped-devices.json",            "{\"metadata\":{\"lastCleanup\":0,\"totalDiscovered\":0,\"autoCleanupAfterDays\":7},\"unmappedDevices\":[]}" },
            { "/config/light-devices.json",               "{\"devices\":[]}" },
            { "/config/schedule/light-schedule.json",     "{\"schedules\":[]}" },
            { "/config/schedule/feeder-schedule.json",    "{\"schedules\":[]}" },
            { "/config/schedule/wm-schedule.json",        "{\"schedules\":[]}" },
            { "/config/schedule/next-task.json",          "{}" },
            { "/config/feeder-calibration.json",          "{\"feeders\":[]}" },
            { "/config/activity-log.json",                "{\"entries\":[]}" },
            { "/config/deleted-devices.json",             "{\"metadata\":{\"totalDeleted\":0},\"deletedDevices\":[]}" },
        };

        int ok = 0, fail = 0;
        for (const auto& f : filesToReset) {
            File file = FS_USER.open(f.path, "w");
            if (file) {
                file.print(f.emptyContent);
                file.close();
                ok++;
                Serial.printf("[FACTORY RESET]   ✓ %s\n", f.path);
            } else {
                fail++;
                Serial.printf("[FACTORY RESET]   ✗ Failed: %s\n", f.path);
            }
        }

        Serial.printf("[FACTORY RESET] Done: %d files reset, %d failures\n", ok, fail);

        if (fail == 0) {
            request->send(200, "application/json",
                "{\"success\":true,\"message\":\"Factory reset complete. Hub will restart now.\"}");
        } else {
            char buf[128];
            snprintf(buf, sizeof(buf),
                "{\"success\":true,\"message\":\"Reset with %d warnings. Hub will restart now.\",\"failures\":%d}",
                fail, fail);
            request->send(200, "application/json", buf);
        }

        // Restart after a short delay to let the response reach the client
        delay(500);
        ESP.restart();
    });

    // WebSocket setup
    ws.onEvent(onWebSocketEvent);
    server.addHandler(&ws);

    // Explicit static handlers for resource directories to ensure correct mapping
    // UI files are served from static filesystem
    server.serveStatic("/styles", FS_STATIC, "/UI/styles");
    server.serveStatic("/images", FS_STATIC, "/UI/images");
    server.serveStatic("/scripts", FS_STATIC, "/UI/scripts");
    server.serveStatic("/fonts", FS_STATIC, "/UI/fonts");
    // Fallback: map root to UI directory and serve index.html by default
    server.serveStatic("/", FS_STATIC, "/UI/").setDefaultFile("index.html");

    // Serve ota.json from static filesystem (it lives on FS_STATIC, not FS_USER)
    server.serveStatic("/config/ota.json", FS_STATIC, "/config/ota.json");

    // Serve config directory from user filesystem (read-only for browser)
    server.serveStatic("/config", FS_USER, "/config/");

    // 404 handler: attempt to serve files directly from /UI if present
    server.onNotFound([](AsyncWebServerRequest *request){
        String url = request->url();
        String fsPath = "/UI" + url;

        // Normalize directory requests to index.html
        if (url.endsWith("/")) {
            fsPath += "index.html";
        }

        // Try to serve the file from static filesystem
        if (FS_STATIC.exists(fsPath)) {
            // Simple mime type guessing
            String contentType = "text/plain";
            if (url.endsWith(".html") || url == "/") contentType = "text/html";
            else if (url.endsWith(".css")) contentType = "text/css";
            else if (url.endsWith(".js")) contentType = "application/javascript";
            else if (url.endsWith(".png")) contentType = "image/png";
            else if (url.endsWith(".woff2")) contentType = "font/woff2";
            else if (url.endsWith(".svg")) contentType = "image/svg+xml";
            if (config.debugSerial) {
                Serial.printf(" UI fallback: %s -> %s (type: %s)\n", url.c_str(), fsPath.c_str(), contentType.c_str());
            }

            request->send(FS_STATIC, fsPath.c_str(), contentType.c_str());
            return;
        }
        if (config.debugSerial) {
            Serial.printf(" 404 Not Found: %s\n", url.c_str());
        }
        // Record in recent missing list
        recordMissingAsset(url);
        request->send(404, "text/plain", "Not found");
    });



    // Start server
    server.begin();

    Serial.println(" Web server started on port 80");
    Serial.printf("   - Access: http://%s.local\n", config.mdnsHostname.c_str());
    Serial.printf("   - Or: http://%s\n", WiFi.localIP().toString().c_str());

    // NOTE: system.boot notification is emitted from webUiTask() AFTER
    // setupNotifications() registers channels and routes.
}

// ============================================================================
// ESP-NOW MANAGER CALLBACKS
// ============================================================================

void onAnnounceReceived(const uint8_t* mac, const AnnounceMessage& msg) {
    // Skip dummy test devices (MACs starting with AA:BB:CC)
    if (mac[0] == 0xAA && mac[1] == 0xBB && mac[2] == 0xCC) {
        Serial.printf("[HUB] Ignoring ANNOUNCE from dummy device: %02X:%02X:%02X:%02X:%02X:%02X\n",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return;
    }
    
    if (config.debugESPNOW) {
        Serial.println("");
        Serial.printf("  ANNOUNCE from %02X:%02X:%02X:%02X:%02X:%02X\n",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        Serial.printf(" Type: %d | Tank: %d | FW: v%d\n",
                      (int)msg.header.nodeType, msg.header.tankId, msg.firmwareVersion);
        if (msg.header.tankId == 0) {
            Serial.println("   UNMAPPED DEVICE (needs provisioning)");
        }
        Serial.println("");
    }
    
    // Forward to AquariumManager
    AquariumManager::getInstance().handleAnnounce(mac, msg);
    
    // CRITICAL: Add peer for bidirectional unicast communication
    Serial.printf("[HUB] Adding peer %02X:%02X:%02X:%02X:%02X:%02X (ANNOUNCE)...\n", 
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    bool peerAdded = ESPNowManager::getInstance().addPeer(mac);
    if (peerAdded) {
        Serial.println("[HUB] ✓ Peer registered - unicast ready");
    } else {
        Serial.println("[HUB] ✗ Peer registration FAILED - unicast will NOT work!");
    }
    
    // Check devices.json FIRST — this is the source of truth for provisioned state.
    // Nodes always send tankId=0 in their ANNOUNCE on every boot (they don't know
    // their assigned tank until they receive the ACK). Using the node's self-reported
    // tankId to decide "new vs known" causes false "new device discovered" notifications
    // on every reconnect of a provisioned device.
    int ackTankId = msg.header.tankId;
    bool isProvisioned = false;
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    File devFile = FS_USER.open("/config/devices.json", "r");
    if (devFile) {
        DynamicJsonDocument devDoc(8192);
        deserializeJson(devDoc, devFile);
        devFile.close();

        JsonArray devicesArr = devDoc["devices"];
        if (!devicesArr.isNull()) {
            for (JsonObject d : devicesArr) {
                const char* listedMac = d["mac"];
                if (listedMac && strcasecmp(listedMac, macStr) == 0) {
                    ackTankId = d["tankId"] | 0;
                    isProvisioned = true;
                    Serial.printf(" [HUB] ANNOUNCE: device found in devices.json (tank %d), using that for ACK\n", ackTankId);
                    break;
                }
            }
        }
    }

    // Notify: new device discovery — only when the device is NOT in devices.json.
    // Do NOT rely on msg.header.tankId == 0 here: provisioned nodes still send
    // tankId=0 on every reboot before they receive the ACK with their assigned tank.
    if (!isProvisioned && peerAdded) {
        const char* typeStr = "UNKNOWN";
        switch(msg.header.nodeType) {
            case NodeType::LIGHT: typeStr = "LIGHT"; break;
            case NodeType::CO2: typeStr = "CO2"; break;
            case NodeType::HEATER: typeStr = "HEATER"; break;
            case NodeType::FISH_FEEDER: typeStr = "FISH_FEEDER"; break;
            case NodeType::SENSOR: typeStr = "SENSOR"; break;
            case NodeType::REPEATER: typeStr = "REPEATER"; break;
            case NodeType::WAVE_MAKER: typeStr = "WAVE_MAKER"; break;
            default: break;
        }
        notifier.emitf("node.discovered", NTFY_MSG_DEVICE_DISCOVERED, macStr, typeStr);
    }

    AckMessage ack = {};
    ack.header.type = MessageType::ACK;
    ack.header.tankId = ackTankId;
    ack.header.nodeType = NodeType::HUB;
    ack.header.timestamp = millis();
    ack.header.sequenceNum = 0;
    ack.assignedNodeId = 1;  // Simple assignment for now
    ack.accepted = true;

    ESPNowManager::getInstance().send(mac, (uint8_t*)&ack, sizeof(ack));

    if (config.debugESPNOW) {
        Serial.printf(" ACK sent to device\n\n");
    }
}

void sendHubHeartbeat() {
    HeartbeatMessage msg = {};
    msg.header.type = MessageType::HEARTBEAT;
    msg.header.tankId = 0;  // Hub-level heartbeat
    msg.header.nodeType = NodeType::HUB;
    msg.header.timestamp = millis();
    msg.header.sequenceNum = 0;
    msg.health = 100;
    msg.uptimeMinutes = millis() / 60000;
    msg.nodeUnixTime = 0;

    uint8_t broadcast[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    ESPNowManager::getInstance().send(broadcast, (uint8_t*)&msg, sizeof(msg));

    if (config.debugESPNOW) {
        Serial.printf("[HUB HB] Broadcast heartbeat (interval=%ds)\n", config.hubHeartbeatIntervalSec);
    }
}

void hubHeartbeatTask(void* param) {
    (void)param;
    while(true) {
        if (config.hubHeartbeatIntervalSec > 0) {
            sendHubHeartbeat();
            vTaskDelay(pdMS_TO_TICKS((uint32_t)config.hubHeartbeatIntervalSec * 1000));
        } else {
            // Sleep for a while if disabled
            vTaskDelay(pdMS_TO_TICKS(60000));
        }
    }
}


void onHeartbeatReceived(const uint8_t* mac, const HeartbeatMessage& msg) {
    // Skip dummy test devices (MACs starting with AA:BB:CC)
    if (mac[0] == 0xAA && mac[1] == 0xBB && mac[2] == 0xCC) {
        return;  // Silently ignore
    }
    
    // FAILSAFE: Ensure peer is registered (in case missed during ANNOUNCE)
    // This allows recovery if hub rebooted or peer list was cleared
    static std::map<String, unsigned long> lastPeerCheck;
    String macKey = macToString(mac);
    unsigned long now = millis();
    
    if (lastPeerCheck.find(macKey) == lastPeerCheck.end() || (now - lastPeerCheck[macKey] > 60000)) {
        ESPNowManager::getInstance().addPeer(mac);  // Idempotent - won't fail if already exists
        lastPeerCheck[macKey] = now;
    }
    
    // Update peer online status
    ESPNowManager::getInstance().updatePeerHeartbeat(mac);
    
    // Forward to AquariumManager
    AquariumManager::getInstance().handleHeartbeat(mac, msg);
}

void onStatusReceived(const uint8_t* mac, const StatusMessage& msg) {
    if (true) {
        //Serial.println("");
        Serial.printf("  STATUS from %02X:%02X:%02X:%02X:%02X:%02X\n",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        Serial.printf(" Command ID: %d | Status Code: %d\n",
                      msg.commandId, msg.statusCode);
        Serial.printf(" Type: %d | Tank: %d\n",
                      (int)msg.header.nodeType, msg.header.tankId);
        
        // Print status data if present
        bool hasData = false;
        for (int i = 0; i < 32; i++) {
            if (msg.statusData[i] != 0) {
                hasData = true;
                break;
            }
        }
        
        if (hasData) {
            Serial.print(" Data: ");
            for (int i = 0; i < 8; i++) {  // Print first 8 bytes
                Serial.printf("%02X ", msg.statusData[i]);
            }
            Serial.println();
        }
        
        Serial.println("");
    }
    
    // Handle OTA_STATUS_NEED_BEGIN - node missed OTA_BEGIN, re-send it
    if (msg.statusCode == OTA_STATUS_NEED_BEGIN && nodeOtaState.active) {
        uint8_t neededType = msg.statusData[0];  // 0xF1 (firmware) or 0xC1 (config)
        Serial.printf("[NodeOTA] Node missed BEGIN! Re-sending OTA_BEGIN for type 0x%02X\n", neededType);
        
        // Construct and re-send OTA_BEGIN
        CommandMessage beginMsg = {};
        beginMsg.header.type = MessageType::COMMAND;
        beginMsg.header.tankId = 0;
        beginMsg.header.nodeType = NodeType::HUB;
        beginMsg.header.timestamp = millis();
        beginMsg.commandId = generateCommandId();
        beginMsg.commandSeqID = 0;
        beginMsg.finalCommand = false;
        beginMsg.commandData[0] = OTA_CMD_OTA_BEGIN;
        beginMsg.commandData[1] = neededType;
        
        // Calculate total size based on what we're sending
        uint32_t totalSize = 0;
        if (neededType == OTA_CMD_FIRMWARE_CHUNK && nodeOtaState.firmwareSaved) {
            File fwFile = FS_STATIC.open("/ota/light/firmware.bin", "r");
            if (fwFile) {
                totalSize = fwFile.size();
                fwFile.close();
            }
        } else if (neededType == OTA_CMD_CONFIG_CHUNK) {
            // Config size from earlier transfer
            totalSize = 500;  // Approximate, config is small
        }
        
        memcpy(&beginMsg.commandData[2], &totalSize, 4);
        beginMsg.commandData[6] = 29;  // chunk size
        
        // Send multiple times for reliability
        for (int i = 0; i < 5; i++) {
            Serial.printf("[NodeOTA] Re-sending OTA_BEGIN (attempt %d/5)\n", i + 1);
            ESPNowManager::getInstance().send(nodeOtaState.targetMac, (uint8_t*)&beginMsg, sizeof(beginMsg));
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        
        Serial.println("[NodeOTA] BEGIN re-sent. Node should now be ready for chunks.");
    }
    
    // Handle OTA_STATUS_READY_END - node received all chunks, send OTA_END
    if (msg.statusCode == OTA_STATUS_READY_END && nodeOtaState.active) {
        uint8_t otaType = msg.statusData[0];  // 0xF1 (firmware) or 0xC1 (config)
        uint16_t chunksReceived = 0;
        memcpy(&chunksReceived, &msg.statusData[1], 2);
        
        Serial.printf("[NodeOTA] READY_END received! Type 0x%02X, node received %u chunks\n", otaType, chunksReceived);
        
        // Clear the waiting flag so the OTA task can continue
        if (otaType == OTA_CMD_FIRMWARE_CHUNK) {
            nodeOtaState.firmwareWaitingEnd = false;
        }
        
        // Send OTA_END
        CommandMessage endMsg = {};
        endMsg.header.type = MessageType::COMMAND;
        endMsg.header.tankId = 0;
        endMsg.header.nodeType = NodeType::HUB;
        endMsg.header.timestamp = millis();
        endMsg.commandId = nodeOtaState.lastCommandId;  // Use the same command ID
        endMsg.finalCommand = true;
        endMsg.commandData[0] = OTA_CMD_OTA_END;
        endMsg.commandData[1] = otaType;
        
        // Send OTA_END multiple times for reliability
        for (int i = 0; i < 5; i++) {
            Serial.printf("[NodeOTA] Sending OTA_END (attempt %d/5) in response to READY_END\n", i + 1);
            ESPNowManager::getInstance().send(nodeOtaState.targetMac, (uint8_t*)&endMsg, sizeof(endMsg));
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        
        Serial.println("[NodeOTA] END sent in response to node READY_END status.");
    }
    
    // Update light channel status cache
    String macStr = macToString(mac);
    bool pendingLightStatus = (g_lightStatusPending.find(macStr) != g_lightStatusPending.end());
    if (msg.header.nodeType == NodeType::LIGHT || pendingLightStatus) {
        LightChannelStatus status;
        status.ch1 = msg.statusData[0] != 0;
        status.ch2 = msg.statusData[1] != 0;
        status.ch3 = msg.statusData[2] != 0;
        status.updatedAt = millis();
        g_lightStatus[macStr] = status;

        if (pendingLightStatus) {
            g_lightStatusPending.erase(macStr);
        }

        if (config.debugESPNOW) {
            Serial.printf("[LIGHT] STATUS update %s -> ch1=%d ch2=%d ch3=%d (nodeType=%d, pending=%s)\n",
                          macStr.c_str(), status.ch1 ? 1 : 0, status.ch2 ? 1 : 0, status.ch3 ? 1 : 0,
                          (int)msg.header.nodeType, pendingLightStatus ? "yes" : "no");
        }
    }

    // Update wavemaker status cache
    bool pendingWmStatus = (g_waveMakerStatusPending.find(macStr) != g_waveMakerStatusPending.end());
    if (msg.header.nodeType == NodeType::WAVE_MAKER || pendingWmStatus) {
        WaveMakerStatus wmStatus;
        // statusData[0] = state enum, [1-2] = PWM uint16 LE, [3] = pumpActive, [4-7] = dutyPercent float LE
        wmStatus.pumpActive = (msg.statusData[3] != 0);
        uint16_t pwmRaw = 0;
        memcpy(&pwmRaw, &msg.statusData[1], sizeof(uint16_t));
        wmStatus.pwmRaw = pwmRaw;
        float dutyPct = 0.0f;
        memcpy(&dutyPct, &msg.statusData[4], sizeof(float));
        wmStatus.dutyPercent = dutyPct;
        wmStatus.updatedAt = millis();
        g_waveMakerStatus[macStr] = wmStatus;

        if (pendingWmStatus) {
            g_waveMakerStatusPending.erase(macStr);
        }

        if (config.debugESPNOW) {
            Serial.printf("[WAVEMAKER] STATUS update %s -> active=%d duty=%.1f%% pwm=%u (pending=%s)\n",
                          macStr.c_str(), wmStatus.pumpActive ? 1 : 0, wmStatus.dutyPercent,
                          wmStatus.pwmRaw, pendingWmStatus ? "yes" : "no");
        }
    }

    // If this is a feeder and node reports feed-in-progress AND we have a pending feed send,
    // treat the STATUS as the ACK for a previously-sent feed command and decrement count.
    if (msg.header.nodeType == NodeType::FISH_FEEDER) {
        // statusData[2] = feedInProgress flag in feeder node
        if (msg.statusData[2] == 1 && msg.statusCode == 0x00) {
            String macKey = macToString(mac);
            auto it = g_pendingFeedAcks.find(macKey);
            if (it != g_pendingFeedAcks.end() && it->second > 0) {
                int newVal = decrementFeedCountForMac(macKey);
                if (newVal >= 0) {
                    Serial.printf("[FEED-COUNT] Decremented available feed for %s -> %d\n", macKey.c_str(), newVal);

                    // Notify UI via AquariumManager websocket callback
                    DynamicJsonDocument doc(256);
                    doc["mac"] = macKey;
                    doc["available"] = newVal;
                    String payload;
                    serializeJson(doc, payload);
                    AquariumManager::getInstance().broadcastUpdate("feedCountUpdated", payload);
                } else {
                    Serial.printf("[FEED-COUNT] No feed count entry to decrement for %s\n", macKey.c_str());
                }

                // consume one pending ack
                it->second = it->second - 1;
                if (it->second <= 0) g_pendingFeedAcks.erase(it);
            }
        }
    }

    // Forward to AquariumManager
    AquariumManager::getInstance().handleStatus(mac, msg);
}

void onCommandReceived(const uint8_t* mac, const uint8_t* data, size_t len) {
    // Hub doesn't receive commands (only sends them)
    if (config.debugESPNOW) {
        Serial.printf("  Unexpected COMMAND received from %02X:%02X:%02X:%02X:%02X:%02X\n",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
}

void setupESPNow() {
    Serial.println("");
    Serial.println(" Initializing ESPNowManager...");
    Serial.println("");
    
    // CRITICAL: Disable WiFi power save mode for reliable ESP-NOW reception
    // Power save mode can cause ESP-NOW unicast messages to be dropped
    esp_wifi_set_ps(WIFI_PS_NONE);
    Serial.println(" [HUB] WiFi power save: DISABLED (for ESP-NOW reliability)");
    
    // Set WiFi protocol to support ESP-NOW
    esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);
    Serial.println(" [HUB] WiFi protocol: 11B/G/N/LR enabled");
    
    // Initialize ESPNowManager as hub
    bool success = ESPNowManager::getInstance().begin(config.espnowChannel, true);
    
    if (!success) {
        Serial.println(" ESPNowManager initialization failed!");
        return;
    }
    
    // Register callbacks
    ESPNowManager::getInstance().onAnnounceReceived(onAnnounceReceived);
    ESPNowManager::getInstance().onHeartbeatReceived(onHeartbeatReceived);
    ESPNowManager::getInstance().onStatusReceived(onStatusReceived);
    ESPNowManager::getInstance().onCommandReceived(onCommandReceived);
    
    Serial.println(" ESPNowManager ready");
    Serial.printf("   - Channel: %d\n", config.espnowChannel);
    Serial.printf("   - Mode: HUB (FreeRTOS queue enabled)\n");
    Serial.printf("   - Debug: %s\n", config.debugESPNOW ? "ON" : "OFF");
    Serial.println("");


    // Print initial statistics
    if (config.debugESPNOW) {
        auto stats = ESPNowManager::getInstance().getStatistics();
        Serial.println(" Initial Statistics:");
        Serial.printf("   Messages sent/received: %u / %u\n", 
                      stats.messagesSent, stats.messagesReceived);
        Serial.printf("   Fragments sent/received: %u / %u\n",
                      stats.fragmentsSent, stats.fragmentsReceived);
        Serial.println();
    }
}

// ============================================================================
// MAIN SETUP & LOOP
// ============================================================================

void setup() {
    // ESP32-S3 USB CDC Serial Configuration
    // When ARDUINO_USB_CDC_ON_BOOT=1, Serial is automatically initialized as HWCDC
    // We need to wait for USB CDC to enumerate and set up properly
    
    Serial.begin(115200);  // Initialize HWCDC (baud rate is virtual but required)
    Serial.setTxTimeoutMs(0);  // Non-blocking serial output
    
    // Wait for USB CDC to be ready (with timeout)
    unsigned long startMs = millis();
    while (!Serial && (millis() - startMs < 5000)) {
        delay(10);
    }
    
    // Small delay after serial ready
    delay(500);
    
    // Test basic serial output
    if (Serial) {
        Serial.println();
        Serial.println("==============================================");
        Serial.println("  Aquarium Management System - Hub Starting");
        Serial.println("  ESP32-S3 USB CDC Serial (HWCDC)");
        Serial.printf("  Free Heap: %u KB, PSRAM: %u KB\n", 
                      ESP.getFreeHeap() / 1024, 
                      ESP.getFreePsram() / 1024);
        Serial.println("==============================================");
        Serial.flush();
    }
    
    // Initialize MicroCore Logger (Serial already initialized)
    Logger::begin(115200, LOG_DEBUG);
    
    LOG_INFO("=== LOGGER INITIALIZED ===");
    LOG_INFO("");
    LOG_INFO("   AQUARIUM MANAGEMENT SYSTEM - HUB");
    LOG_INFO("   ESP32-S3-N16R8 Central Controller");
    LOG_INFO("   MicroCore Framework v1.0.0");
    LOG_INFO("");
    
    // Initialize filesystem
#ifdef DUAL_LITTLEFS
    // Dual filesystem mode: static_fs + user_fs partitions
    if (!setupDualFilesystem(true)) {
        LOG_ERROR("CRITICAL: Dual filesystem failed, halting");
        while (1) delay(1000);
    }
    LOG_INFO("Dual LittleFS initialized (static_fs + user_fs)");
    printFilesystemInfo();
#else
    // Single filesystem mode using MicroCore FileManager
    if (!FileManager::begin(true)) {
        LOG_ERROR("CRITICAL: Filesystem failed, halting");
        while (1) delay(1000);
    }
    LOG_INFO("FileManager initialized (LittleFS)");
#endif
    
    // Load configuration using MicroCore ConfigManager
    loadConfiguration();
    
    // Setup WiFi
    setupWiFi();
    
    // Small delay to let WiFi stack fully stabilize before starting mDNS
    delay(200);

    // Setup mDNS
    setupMDNS();
    
    // Initialize AquariumManager
    AquariumManager::getInstance().initialize();
    
    // Web server setup moved to Web UI task (Core 1)
    
    // Load aquariums from JSON file
    loadAquariumsFromFile();
    
    // **NOTE**: Device loading disabled until Device subclass .cpp files are implemented
    // loadDevicesIntoAquariums();
    // TEMPORARY WORKAROUND: deviceCount is calculated from JSON in /api/aquariums endpoint
    
    // Setup ESP-NOW (callbacks run on Core 0, processed in main loop)
    setupESPNow();
    
    // CRITICAL: Register all devices as ESP-NOW peers for bidirectional unicast
    registerAllDevicesAsPeers();
    
    // Initialize Hub Status LEDs
    setupHubStatusLEDs();
    
    // Start watchdog task on Core 0 (ESP-NOW + scheduler core)
    xTaskCreatePinnedToCore(
        watchdogTask,            // Task function
        "Watchdog",              // Name
        8192,                    // Stack size (8KB - needs more for AquariumManager calls)
        NULL,                    // Parameters
        2,                       // Priority (higher than main loop)
        &watchdogTaskHandle,     // Task handle
        0                        // Core 0 (ESP-NOW + scheduler)
    );
    LOG_INFO("Watchdog task created on Core 0 (priority 2)");

    // Start hub heartbeat broadcaster task (if enabled in config)
    if (config.hubHeartbeatIntervalSec > 0) {
        xTaskCreatePinnedToCore(
            hubHeartbeatTask,      // Task function
            "HubHeartbeat",       // Name
            4096,                  // Stack size
            NULL,                  // Parameters
            1,                     // Priority
            NULL,                  // Task handle
            1                      // Core 1
        );
        LOG_INFO("Hub Heartbeat task created on Core 1");
    }

    // Start Web UI task on Core 1 (Web UI core)
    xTaskCreatePinnedToCore(
        webUiTask,               // Task function
        "WebUI",                 // Name
        8192,                    // Stack size
        NULL,                    // Parameters
        1,                       // Priority
        &webUiTaskHandle,        // Task handle
        1                        // Core 1 (Web UI)
    );
    LOG_INFO("Web UI task created on Core 1 (priority 1)");
    
    // Start Light Scheduler task on Core 1
    xTaskCreatePinnedToCore(
        schedulerTask,           // Task function
        "LightScheduler",        // Name
        8192,                    // Stack size (needs JSON parsing)
        NULL,                    // Parameters
        1,                       // Priority
        &schedulerTaskHandle,    // Task handle
        1                        // Core 1
    );
    LOG_INFO("Light Scheduler task created on Core 1 (priority 1)");
    
    LOG_INFO("");
    LOG_INFO("========================================");
    LOG_INFO("           HUB READY");
    LOG_INFO("========================================");
    LOG_INFO("");
    
    // Print initial memory status
    printMemoryStatus();
}

void loop() {
    // Main loop runs on Core 0
    // Processes ESP-NOW messages from queue and scheduler
    // Web UI runs on Core 1
    // Watchdog task runs independently on Core 0
    
    // ========================================================================
    // DEFERRED OTA PROCESSING
    // OTA must happen in main loop to avoid blocking async_tcp task
    // When BOTH updates are needed, apply firmware first, then LittleFS, then reboot once
    // ========================================================================
    if (otaPending != OtaPendingType::NONE && !otaInProgress) {
        otaInProgress = true;
        
        // Small delay to allow response to be sent
        delay(500);
        
        bool allSuccess = true;
        String lastError;
        
        // Handle FIRMWARE update (for FIRMWARE or BOTH)
        if (otaPending == OtaPendingType::FIRMWARE || otaPending == OtaPendingType::BOTH) {
            String firmwareUrl = config.hubFirmwareOtaUrl;
            if (!firmwareUrl.endsWith("/")) firmwareUrl += "/";
            firmwareUrl += "firmware.bin";
            
            Serial.printf("[OTA] Processing FIRMWARE update to version %s\n", otaPendingFirmwareVersion.c_str());
            notifier.emitf("system.ota.start", NTFY_MSG_HUB_OTA_STARTED,
                            "Firmware", config.hubFirmwareVersion.c_str(), otaPendingFirmwareVersion.c_str());
            
            String error;
            bool ok = performOtaUpdate(firmwareUrl, false, error);
            
            if (ok) {
                // Update version in hub_config.txt
                updateHubConfigValue("HUB_FIRMWARE_VERSION", otaPendingFirmwareVersion);
                config.hubFirmwareVersion = otaPendingFirmwareVersion;
                Serial.println("[OTA] Firmware update successful!");
                notifier.emitf("system.ota.success", NTFY_MSG_HUB_OTA_SUCCESS,
                                "Firmware", otaPendingFirmwareVersion.c_str());
            } else {
                Serial.printf("[OTA] Firmware update FAILED: %s\n", error.c_str());
                allSuccess = false;
                lastError = "Firmware: " + error;
                notifier.emitf("system.ota.fail", NTFY_MSG_HUB_OTA_FAILED,
                                "Firmware", error.c_str());
            }
        }
        
        // Handle LITTLEFS update (for LITTLEFS or BOTH, only if firmware succeeded or wasn't needed)
        if (allSuccess && (otaPending == OtaPendingType::LITTLEFS || otaPending == OtaPendingType::BOTH)) {
            String littlefsUrl = config.hubLittlefsOtaUrl;
            if (!littlefsUrl.endsWith("/")) littlefsUrl += "/";
            littlefsUrl += "littlefs.bin";
            
            Serial.printf("[OTA] Processing LITTLEFS update to version %s\n", otaPendingLittlefsVersion.c_str());
            notifier.emitf("system.ota.start", NTFY_MSG_HUB_OTA_STARTED,
                            "LittleFS", config.hubLittlefsVersion.c_str(), otaPendingLittlefsVersion.c_str());
            
            String error;
            bool ok = performOtaUpdate(littlefsUrl, true, error);
            
            if (ok) {
                // Update version in hub_config.txt
                updateHubConfigValue("HUB_LITTLEFS_VERSION", otaPendingLittlefsVersion);
                config.hubLittlefsVersion = otaPendingLittlefsVersion;
                Serial.println("[OTA] LittleFS update successful!");
                notifier.emitf("system.ota.success", NTFY_MSG_HUB_OTA_SUCCESS,
                                "LittleFS", otaPendingLittlefsVersion.c_str());
            } else {
                Serial.printf("[OTA] LittleFS update FAILED: %s\n", error.c_str());
                allSuccess = false;
                lastError = "LittleFS: " + error;
                notifier.emitf("system.ota.fail", NTFY_MSG_HUB_OTA_FAILED,
                                "LittleFS", error.c_str());
            }
        }
        
        // Reboot only after ALL updates are done (or if any succeeded)
        if (allSuccess || 
            (otaPending == OtaPendingType::BOTH && config.hubFirmwareVersion == otaPendingFirmwareVersion)) {
            // At least firmware was updated, or all succeeded - reboot
            Serial.println("[OTA] All updates complete! Rebooting in 2 seconds...");
            delay(2000);
            ESP.restart();
        } else {
            // All updates failed
            Serial.printf("[OTA] Update FAILED: %s\n", lastError.c_str());
            otaPending = OtaPendingType::NONE;
            otaInProgress = false;
            otaPendingFirmwareVersion = "";
            otaPendingLittlefsVersion = "";
        }
    }
    
    // Process ESP-NOW messages via ESPNowManager (non-blocking)
    ESPNowManager::getInstance().processQueue();
    
    // Check for peer timeouts (60 second timeout)
    ESPNowManager::getInstance().checkPeerTimeouts(60000);
    
    // Update AquariumManager (schedule execution only)
    // Note: Health checks and water monitoring run on Core 0 watchdog task
    AquariumManager::getInstance().updateSchedules();
    
    // Update Hub Status LEDs
    updateHubStatusLEDs();
    
    // Print WiFi channel status periodically
    static unsigned long lastChannelCheckTime = 0;
    if (millis() - lastChannelCheckTime > 30000) {  // Every 30 seconds
        lastChannelCheckTime = millis();
        int currentChannel = WiFi.channel();
        Serial.println("\n");
        Serial.println(" WiFi/ESP-NOW Status:");
        Serial.printf("   WiFi Channel: %d\n", currentChannel);
        Serial.printf("   ESP-NOW Expected Channel: %d\n", config.espnowChannel);
        if (currentChannel != config.espnowChannel) {
            Serial.printf("   WARNING: Channel mismatch! ESP-NOW will NOT work!\n");
            Serial.printf("   SOLUTION: Configure router to use channel %d\n", config.espnowChannel);
        } else {
            Serial.println("   Channel OK - ESP-NOW should work");
        }
        Serial.println("");
    }
    
    // Print ESP-NOW statistics periodically
    static unsigned long lastStatsTime = 0;
    if (config.debugESPNOW && (millis() - lastStatsTime > 60000)) {  // Every 60 seconds
        lastStatsTime = millis();
        auto stats = ESPNowManager::getInstance().getStatistics();
        Serial.println("\n");
        Serial.println(" ESP-NOW Statistics (Last 60s):");
        Serial.printf("   Messages: %u sent / %u received\n", 
                      stats.messagesSent, stats.messagesReceived);
        Serial.printf("   Fragments: %u sent / %u received\n",
                      stats.fragmentsSent, stats.fragmentsReceived);
        Serial.printf("   Errors: %u send failures / %u reassembly timeouts\n",
                      stats.sendFailures, stats.reassemblyTimeouts);
        Serial.printf("   Duplicates ignored: %u\n", stats.duplicatesIgnored);
        Serial.printf("   Retries: %u\n", stats.retries);
        Serial.println("\n");
    }
    
    // Small delay to prevent watchdog issues
    delay(10);
}
