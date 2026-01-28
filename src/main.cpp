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
#include "protocol/messages.h"
#include "models/Aquarium.h"
#include "managers/AquariumManager.h"
#include <HTTPClient.h>
#include <memory>
#include "Constant.h"
#include "ESPNowManager.h"
#include "ws_server.h"

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

static String macToString(const uint8_t* mac) {
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(macStr);
}

HubConfig config;

// Task handles
TaskHandle_t watchdogTaskHandle = NULL;
TaskHandle_t webUiTaskHandle = NULL;
TaskHandle_t schedulerTaskHandle = NULL;
TaskHandle_t nodeOtaTaskHandle = NULL;

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
} nodeOtaState = {0};

// NTP time tracking
static bool ntpSynced = false;

// ESPNowManager callbacks declared here
void onAnnounceReceived(const uint8_t* mac, const AnnounceMessage& msg);
void onHeartbeatReceived(const uint8_t* mac, const HeartbeatMessage& msg);
void onStatusReceived(const uint8_t* mac, const StatusMessage& msg);
void onCommandReceived(const uint8_t* mac, const uint8_t* data, size_t len);

// Peer registration
void registerAllDevicesAsPeers();

// Web server setup
void setupWebServer();

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
    const char* configPath = "/config/hub_config.txt";
    
    if (!LittleFS.exists(configPath)) {
        Serial.println("[Config] hub_config.txt not found");
        return false;
    }
    
    // Read existing content
    File file = LittleFS.open(configPath, "r");
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
    file = LittleFS.open(configPath, "w");
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

bool performOtaUpdate(const String& url, bool isLittleFs, String& errorOut) {
    if (url.length() == 0) {
        errorOut = "OTA URL not set";
        return false;
    }

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
        errorOut = "Failed to start HTTP";
        return false;
    }

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        errorOut = "HTTP error: " + String(httpCode);
        http.end();
        return false;
    }

    int contentLength = http.getSize();
    int updateType = isLittleFs ? U_SPIFFS : U_FLASH;

    if (!Update.begin(contentLength > 0 ? contentLength : UPDATE_SIZE_UNKNOWN, updateType)) {
        errorOut = Update.errorString();
        http.end();
        return false;
    }

    size_t written = Update.writeStream(http.getStream());
    if (contentLength > 0 && written != (size_t)contentLength) {
        errorOut = "Incomplete OTA write";
        Update.abort();
        http.end();
        return false;
    }

    if (!Update.end()) {
        errorOut = Update.errorString();
        http.end();
        return false;
    }

    if (!Update.isFinished()) {
        errorOut = "OTA not finished";
        http.end();
        return false;
    }

    http.end();
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
    Serial.printf(" ⚠️ Device creation not yet implemented (type: %d)\n", (int)type);
    return nullptr;
}

/**
 * @brief Count devices for a specific aquarium from devices.json
 * TEMPORARY WORKAROUND: Since Device objects aren't created yet,
 * we count devices directly from the JSON file
 */
int countDevicesForAquarium(uint8_t tankId) {
    if (!LittleFS.exists("/config/devices.json")) {
        return 0;
    }
    
    File file = LittleFS.open("/config/devices.json", "r");
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
// CONFIGURATION LOADER
// ============================================================================

void loadConfiguration() {
    // Set defaults
    config.heartbeatEnabled = true;
    config.heartbeatIntervalSec = 30;
    // Hub will broadcast its own heartbeat periodically (seconds)
    config.hubHeartbeatIntervalSec = 120;  // Default 120s

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
    
    // Hub OTA defaults
    config.hubFirmwareOtaUrl = "";
    config.hubLittlefsOtaUrl = "";
    config.hubFirmwareVersion = "1.0.0";
    config.hubLittlefsVersion = "1.0.0";
    
    // Node OTA defaults
    config.lightNodeOtaUrl = "";

    // Test mode disabled by default (only enable in test environments)
    config.hubTestMode = false;
    
    // Load from file
    if (!LittleFS.exists("/config/hub_config.txt")) {
        Serial.println("  Config file not found, using defaults");
        return;
    }
    
    File file = LittleFS.open("/config/hub_config.txt", "r");
    if (!file) {
        Serial.println(" Failed to open config file");
        return;
    }
    
    Serial.println(" Loading configuration...");
    
    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        
        // Skip comments and empty lines
        if (line.startsWith("#") || line.length() == 0) {
            continue;
        }
        
        // Parse KEY=VALUE
        int separatorIndex = line.indexOf('=');
        if (separatorIndex == -1) {
            continue;
        }
        
        String key = line.substring(0, separatorIndex);
        String value = line.substring(separatorIndex + 1);
        key.trim();
        value.trim();
        
        // Apply configuration
        if (key == "HEARTBEAT_ENABLED") {
            config.heartbeatEnabled = (value == "true");
        } else if (key == "HEARTBEAT_INTERVAL_SEC") {
            config.heartbeatIntervalSec = value.toInt();
        } else if (key == "HUB_HEARTBEAT_INTERVAL_SEC") {
            config.hubHeartbeatIntervalSec = value.toInt();
        } else if (key == "AGGRESSIVE_MEMORY_MANAGEMENT") {
            config.aggressiveMemoryManagement = (value == "true");
        } else if (key == "HEAP_WARNING_THRESHOLD_KB") {
            config.heapWarningThresholdKB = value.toInt();
        } else if (key == "PSRAM_WARNING_THRESHOLD_KB") {
            config.psramWarningThresholdKB = value.toInt();
        } else if (key == "WIFI_AP_NAME") {
            config.wifiAPName = value;
        } else if (key == "WIFI_AP_PASSWORD") {
            config.wifiAPPassword = value;
        } else if (key == "WIFI_TIMEOUT_SEC") {
            config.wifiTimeoutSec = value.toInt();
        } else if (key == "MDNS_HOSTNAME") {
            config.mdnsHostname = value;
        } else if (key == "ESPNOW_CHANNEL") {
            config.espnowChannel = value.toInt();
        } else if (key == "ESPNOW_MAX_PEERS") {
            config.espnowMaxPeers = value.toInt();
        } else if (key == "DEBUG_SERIAL") {
            config.debugSerial = (value == "true");
        } else if (key == "DEBUG_ESPNOW") {
            config.debugESPNOW = (value == "true");
        } else if (key == "DEBUG_WEBSOCKET") {
            config.debugWebSocket = (value == "true");
        } else if (key == "HUB_FIRMWARE_OTA_URL") {
            config.hubFirmwareOtaUrl = value;
        } else if (key == "HUB_LITTLEFS_OTA_URL") {
            config.hubLittlefsOtaUrl = value;
        } else if (key == "HUB_FIRMWARE_VERSION") {
            config.hubFirmwareVersion = value;
        } else if (key == "HUB_LITTLEFS_VERSION") {
            config.hubLittlefsVersion = value;
        } else if (key == "LIGHT_NODE_OTA_URL") {
            config.lightNodeOtaUrl = value;
        } else if (key == "HUB_TEST_MODE") {
            config.hubTestMode = (value == "true");
        }
    }
    
    file.close();
    
    Serial.println(" Configuration loaded");
    Serial.printf("   - Heartbeat: %s (%ds)\n", 
                  config.heartbeatEnabled ? "ON" : "OFF", 
                  config.heartbeatIntervalSec);
    Serial.printf("   - Hub Heartbeat Interval: %ds\n", config.hubHeartbeatIntervalSec);
    Serial.printf("   - Memory Management: %s\n", 
                  config.aggressiveMemoryManagement ? "AGGRESSIVE" : "NORMAL");
    Serial.printf("   - mDNS: %s.local\n", config.mdnsHostname.c_str());
}

// ============================================================================
// MEMORY MANAGEMENT
// ============================================================================

void printMemoryStatus() {
    uint32_t freeHeap = ESP.getFreeHeap() / 1024;  // KB
    uint32_t totalHeap = ESP.getHeapSize() / 1024;  // KB
    uint32_t freePSRAM = ESP.getFreePsram() / 1024;  // KB
    uint32_t totalPSRAM = ESP.getPsramSize() / 1024;  // KB
    
    Serial.println("");
    Serial.printf(" HEAP:  %u KB free / %u KB total (%.1f%%)\n", 
                  freeHeap, totalHeap, 
                  (freeHeap * 100.0) / totalHeap);
    Serial.printf(" PSRAM: %u KB free / %u KB total (%.1f%%)\n", 
                  freePSRAM, totalPSRAM, 
                  (freePSRAM * 100.0) / totalPSRAM);
    Serial.printf("  Uptime: %lu seconds\n", millis() / 1000);
    Serial.println("");
    
    // Warnings
    if (freeHeap < config.heapWarningThresholdKB) {
        Serial.printf("  HEAP WARNING: Only %u KB free!\n", freeHeap);
    }
    
    if (freePSRAM < config.psramWarningThresholdKB) {
        Serial.printf("  PSRAM WARNING: Only %u KB free!\n", freePSRAM);
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
        Serial.println(" Aggressive memory cleanup triggered");
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

// Structure to track last command sent per channel to avoid duplicate sends
struct ChannelState {
    bool ch1_on;
    bool ch2_on;
    bool ch3_on;
    int lastCheckMinute;
};
static std::map<String, ChannelState> g_channelStates;

// Structure for next-task.json persistence
struct NextTask {
    String mac;
    int channel;     // 1, 2, or 3
    bool actionOn;   // true = turn ON, false = turn OFF
    time_t scheduledTime;  // Unix timestamp
    String period;   // "morning" or "evening"
};

static const char* NEXT_TASK_FILE = "/config/schedule/next-task.json";

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

// Helper: Calculate all upcoming tasks from a device schedule
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
            
            // Create ON task
            NextTask onTask;
            onTask.mac = macStr;
            onTask.channel = ch;
            onTask.actionOn = true;
            onTask.scheduledTime = minutesToUnixTime(startMinutes);
            onTask.period = periodName;
            tasks.push_back(onTask);
            
            // Create OFF task
            NextTask offTask;
            offTask.mac = macStr;
            offTask.channel = ch;
            offTask.actionOn = false;
            offTask.scheduledTime = minutesToUnixTime(offMinutes);
            offTask.period = periodName;
            tasks.push_back(offTask);
        }
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
    DynamicJsonDocument doc(4096);
    JsonArray arr = doc.createNestedArray("tasks");
    
    for (const NextTask& t : tasks) {
        JsonObject obj = arr.createNestedObject();
        obj["mac"] = t.mac;
        obj["channel"] = t.channel;
        obj["actionOn"] = t.actionOn;
        obj["scheduledTime"] = (long)t.scheduledTime;
        obj["period"] = t.period;
    }
    
    doc["updatedAt"] = (long)getCurrentUnixTime();
    
    File file = LittleFS.open(NEXT_TASK_FILE, "w");
    if (file) {
        serializeJson(doc, file);
        file.close();
        Serial.printf("[SCHEDULER] Saved %d tasks to next-task.json\n", tasks.size());
    } else {
        Serial.println("[SCHEDULER] Failed to write next-task.json");
    }
}

// Load next-task.json
static bool loadNextTasks(std::vector<NextTask>& tasks) {
    tasks.clear();
    
    File file = LittleFS.open(NEXT_TASK_FILE, "r");
    if (!file) {
        Serial.println("[SCHEDULER] next-task.json not found");
        return false;
    }
    
    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    
    if (error) {
        Serial.printf("[SCHEDULER] Failed to parse next-task.json: %s\n", error.c_str());
        return false;
    }
    
    JsonArray arr = doc["tasks"].as<JsonArray>();
    for (JsonObject obj : arr) {
        NextTask t;
        t.mac = obj["mac"].as<String>();
        t.channel = obj["channel"] | 1;
        t.actionOn = obj["actionOn"] | true;
        t.scheduledTime = obj["scheduledTime"] | 0;
        t.period = obj["period"].as<String>();
        tasks.push_back(t);
    }
    
    Serial.printf("[SCHEDULER] Loaded %d tasks from next-task.json\n", tasks.size());
    return true;
}

// Rebuild next-task.json from light-schedule.json (called when schedule changes or on startup)
void rebuildNextTasks() {
    Serial.println("[SCHEDULER] Rebuilding next-task.json from schedules...");
    
    std::vector<NextTask> allTasks;
    
    File file = LittleFS.open("/config/schedule/light-schedule.json", "r");
    if (!file) {
        Serial.println("[SCHEDULER] No light-schedule.json found");
        saveNextTasks(allTasks);  // Save empty
        return;
    }
    
    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    
    if (error) {
        Serial.printf("[SCHEDULER] Failed to parse light-schedule.json: %s\n", error.c_str());
        saveNextTasks(allTasks);
        return;
    }
    
    JsonArray schedules = doc["schedules"].as<JsonArray>();
    for (JsonObject sched : schedules) {
        const char* macStr = sched["mac"];
        if (!macStr) continue;
        
        JsonObject schedule = sched["schedule"];
        calculateNextTasks(macStr, schedule, allTasks);
    }
    
    saveNextTasks(allTasks);
}

// Execute a scheduled task
static bool executeTask(const NextTask& task) {
    uint8_t mac[6];
    if (!parseMacAddress(task.mac.c_str(), mac)) {
        Serial.printf("[SCHEDULER] Invalid MAC in task: %s\n", task.mac.c_str());
        return false;
    }
    
    // Check if node is online
    bool isOnline = ESPNowManager::getInstance().isPeerOnline(mac);
    if (!isOnline) {
        Serial.printf("[SCHEDULER] Node %s is OFFLINE, task deferred\n", task.mac.c_str());
        return false;  // Will retry later
    }
    
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
    
    return true;
}

// Find and execute any past-due tasks, with retry logic for offline nodes
static void processPastDueTasks(std::vector<NextTask>& tasks, bool& needsRebuild) {
    time_t now = getCurrentUnixTime();
    if (now == 0) return;  // NTP not ready
    
    std::vector<NextTask> pendingRetry;
    bool anyExecuted = false;
    
    for (auto it = tasks.begin(); it != tasks.end(); ) {
        if (it->scheduledTime <= now) {
            Serial.printf("[SCHEDULER] Past-due task: %s CH%d %s (scheduled %ld, now %ld)\n",
                         it->mac.c_str(), it->channel, it->actionOn ? "ON" : "OFF",
                         (long)it->scheduledTime, (long)now);
            
            if (executeTask(*it)) {
                anyExecuted = true;
                it = tasks.erase(it);  // Remove executed task
            } else {
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
    
    if (anyExecuted || !pendingRetry.empty()) {
        needsRebuild = true;  // Tasks changed, need to save
    }
}

void schedulerTask(void* parameter) {
    Serial.printf("[SCHEDULER] Task started on core %d\n", xPortGetCoreID());
    
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
        
        // Find next task
        NextTask nextTask;
        if (findNextTask(tasks, nextTask)) {
            time_t waitSeconds = nextTask.scheduledTime - now;
            
            if (waitSeconds <= 0) {
                // Task is due now
                Serial.printf("[SCHEDULER] Executing: %s CH%d %s\n",
                             nextTask.mac.c_str(), nextTask.channel, 
                             nextTask.actionOn ? "ON" : "OFF");
                
                if (executeTask(nextTask)) {
                    // Remove executed task and recalculate next occurrence
                    rebuildNextTasks();
                    loadNextTasks(tasks);
                } else {
                    // Node offline - wait 60 seconds and retry
                    Serial.println("[SCHEDULER] Node offline, retrying in 60s");
                    vTaskDelay(pdMS_TO_TICKS(60000));
                }
            } else {
                // Wait until next task (max 30 seconds between checks)
                uint32_t waitMs = (waitSeconds > 30) ? 30000 : (waitSeconds * 1000);
                Serial.printf("[SCHEDULER] Next task in %ld sec, sleeping %lu ms\n", 
                             (long)waitSeconds, waitMs);
                vTaskDelay(pdMS_TO_TICKS(waitMs));
            }
        } else {
            // No upcoming tasks, check again in 30 seconds
            Serial.println("[SCHEDULER] No upcoming tasks, sleeping 30s");
            vTaskDelay(pdMS_TO_TICKS(30000));
            
            // Rebuild in case schedule was updated
            rebuildNextTasks();
            loadNextTasks(tasks);
        }
        
        // Process any past-due tasks (handles multi-task scenarios)
        needsSave = false;
        processPastDueTasks(tasks, needsSave);
        if (needsSave) {
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
    if (LittleFS.exists("/ota/light/node_config.txt")) {
        File configFile = LittleFS.open("/ota/light/node_config.txt", "r");
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
    if (LittleFS.exists("/ota/light/firmware.bin")) {
        File fwFile = LittleFS.open("/ota/light/firmware.bin", "r");
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
    if (!LittleFS.exists("/ota")) LittleFS.mkdir("/ota");
    if (!LittleFS.exists("/ota/light")) LittleFS.mkdir("/ota/light");

    // Download node_config.txt (optional)
    if (http.begin(*client.get(), nodeOtaState.baseUrl + "node_config.txt")) {
        int httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK) {
            String configContent = http.getString();
            File localConfig = LittleFS.open("/ota/light/node_config.txt", "w");
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
            
            File localFirmware = LittleFS.open("/ota/light/firmware.bin", "w");
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

cleanup:
    // Delete downloaded OTA files
    if (LittleFS.exists("/ota/light/firmware.bin")) {
        LittleFS.remove("/ota/light/firmware.bin");
        Serial.println("[NodeOTA] Deleted /ota/light/firmware.bin");
    }
    if (LittleFS.exists("/ota/light/node_config.txt")) {
        LittleFS.remove("/ota/light/node_config.txt");
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
    
    while (true) {
        unsigned long now = millis();
        
        // Device health monitoring (every 5 seconds)
        if (now - lastHealthCheck >= 5000) {
            lastHealthCheck = now;
            AquariumManager::getInstance().checkDeviceHealth();
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
// WEB UI TASK (Core 1) - Web server + UI
// ============================================================================

void webUiTask(void* parameter) {
    Serial.printf(" Web UI task started on core %d\n", xPortGetCoreID());

    // Setup web server on Web UI core
    setupWebServer();

    // Keep task alive (AsyncWebServer runs in background)
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ============================================================================
// FILESYSTEM SETUP
// ============================================================================

bool setupFilesystem() {
    Serial.println(" Initializing LittleFS...");
    
    if (!LittleFS.begin(true)) {
        Serial.println(" LittleFS mount failed");
        return false;
    }
    
    Serial.println(" LittleFS mounted");
    
    // Initialize unmapped-devices.json if it doesn't exist
    if (!LittleFS.exists("/config/unmapped-devices.json")) {
        Serial.println(" Creating unmapped-devices.json...");
        File file = LittleFS.open("/config/unmapped-devices.json", "w");
        if (file) {
            file.print("{\"metadata\":{\"lastCleanup\":0,\"totalDiscovered\":0,\"autoCleanupAfterDays\":7},\"unmappedDevices\":[]}");
            file.close();
            Serial.println("   - unmapped-devices.json initialized");
        } else {
            Serial.println("   - ERROR: Failed to create unmapped-devices.json");
        }
    }

    // Ensure schedule directory exists
    if (!LittleFS.exists("/config/schedule")) {
        LittleFS.mkdir("/config/schedule");
    }

    // Initialize light-schedule.json if it doesn't exist
    if (!LittleFS.exists("/config/schedule/light-schedule.json")) {
        Serial.println(" Creating light-schedule.json...");
        File file = LittleFS.open("/config/schedule/light-schedule.json", "w");
        if (file) {
            file.print("{\"schedules\":[]}");
            file.close();
            Serial.println("   - light-schedule.json initialized");
        } else {
            Serial.println("   - ERROR: Failed to create light-schedule.json");
        }
    }
    
    // List files (debug)
    if (config.debugSerial) {
        Serial.println(" Filesystem contents:");
        File root = LittleFS.open("/");
        File file = root.openNextFile();
        while (file) {
            Serial.printf("   - %s (%d bytes)\n", file.name(), file.size());
            file = root.openNextFile();
        }
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
    Serial.println(" Starting mDNS responder...");
    
    if (!MDNS.begin(config.mdnsHostname.c_str())) {
        Serial.println(" mDNS failed to start");
        return;
    }
    
    // Add service
    MDNS.addService("http", "tcp", 80);
    
    Serial.printf(" mDNS responder started: http://%s.local\n", 
                  config.mdnsHostname.c_str());
}

// ============================================================================
// JSON FILE OPERATIONS
// ============================================================================

/**
 * @brief Load aquariums from JSON file
 * @return true if loaded successfully
 */
bool loadAquariumsFromFile() {
    if (!LittleFS.exists("/config/aquariums.json")) {
        Serial.println("  aquariums.json not found, creating empty file");
        File file = LittleFS.open("/config/aquariums.json", "w");
        if (file) {
            file.println("{\"aquariums\":[]}");
            file.close();
        }
        return false;
    }
    
    File file = LittleFS.open("/config/aquariums.json", "r");
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
    
    if (!LittleFS.exists("/config/devices.json")) {
        Serial.println("  devices.json not found - no peers to register");
        return;
    }
    
    File file = LittleFS.open("/config/devices.json", "r");
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
    
    if (!LittleFS.exists("/config/devices.json")) {
        Serial.println("  devices.json not found");
        return false;
    }
    
    File file = LittleFS.open("/config/devices.json", "r");
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
            Serial.printf("  ⚠️  Aquarium %d not found for device %s\\n", tankId, name.c_str());
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
    
    Serial.printf(" ✅ Loaded %d devices into aquariums (%d errors)\\n", loadedCount, errorCount);
    
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
    File file = LittleFS.open("/config/aquariums.json", "w");
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
        
        String json = "{";
        json += "\"uptime\":" + String(millis() / 1000) + ",";
        json += "\"heap_free\":" + String(ESP.getFreeHeap()) + ",";
        json += "\"psram_free\":" + String(ESP.getFreePsram()) + ",";
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
    });
    
    // POST delete aquarium (using query parameter)
    server.on("/api/aquarium/delete", HTTP_POST, [](AsyncWebServerRequest *request){
        if (!request->hasParam("id")) {
            request->send(400, "text/plain", "Missing id parameter");
            return;
        }
        
        uint8_t id = request->getParam("id")->value().toInt();
        
        if (!AquariumManager::getInstance().removeAquarium(id)) {
            request->send(404, "text/plain", "Aquarium not found");
            return;
        }
        
        // Save to file
        saveAquariumsToFile();
        
        request->send(200, "text/plain", "Aquarium deleted successfully");
        Serial.printf(" Deleted aquarium ID: %d\\n", id);
    });
    
    // GET unmapped devices
    server.on("/api/unmapped-devices", HTTP_GET, [](AsyncWebServerRequest *request){
        File file = LittleFS.open("/config/unmapped-devices.json", "r");
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
        File file = LittleFS.open("/config/devices.json", "r");
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
        File file = LittleFS.open("/config/schedule/light-schedule.json", "r");
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

        JsonVariant schedule = body["schedule"];
        if (schedule.isNull()) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing schedule\"}");
            return;
        }

        File file = LittleFS.open("/config/schedule/light-schedule.json", "r");
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

        file = LittleFS.open("/config/schedule/light-schedule.json", "w");
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

    // GET settings file list
    server.on("/api/settings/files", HTTP_GET, [](AsyncWebServerRequest *request){
        DynamicJsonDocument doc(256);
        JsonArray files = doc.createNestedArray("files");

        for (const char* fileName : kConfigJsonFiles) {
            String path = String("/config/") + fileName;
            if (LittleFS.exists(path)) {
                files.add(String(fileName));
            }
        }

        String resp;
        serializeJson(doc, resp);
        request->send(200, "application/json", resp);
    });

    // GET settings file download
    server.on("/api/settings/download", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!request->hasParam("name")) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing file name\"}");
            return;
        }

        String name = request->getParam("name")->value();

        if (!isAllowedConfigFile(name)) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid file name\"}");
            return;
        }

        String path = String("/config/") + name;
        if (!LittleFS.exists(path)) {
            request->send(404, "application/json", "{\"success\":false,\"error\":\"File not found\"}");
            return;
        }

        request->send(LittleFS, path, "application/json", true);
    });

    // POST settings file upload
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
                if (!isAllowedConfigFile(target)) {
                    ctx->error = "Invalid target filename";
                    return;
                }

                String path = String("/config/") + target;
                ctx->file = LittleFS.open(path, "w");
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

    // POST OTA firmware update with version check
    server.on("/api/ota/firmware", HTTP_POST, [](AsyncWebServerRequest *request){
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
        
        // Build firmware URL
        String firmwareUrl = config.hubFirmwareOtaUrl;
        if (!firmwareUrl.endsWith("/")) firmwareUrl += "/";
        firmwareUrl += "firmware.bin";
        
        Serial.printf("[OTA] Updating firmware from %s to %s\n", 
                      config.hubFirmwareVersion.c_str(), remoteVersion.c_str());
        
        String error;
        bool ok = performOtaUpdate(firmwareUrl, false, error);
        if (!ok) {
            String response = String("{\"success\":false,\"error\":\"") + error + "\"}";
            request->send(500, "application/json", response);
            return;
        }
        
        // Update version in hub_config.txt before reboot
        updateHubConfigValue("HUB_FIRMWARE_VERSION", remoteVersion);
        config.hubFirmwareVersion = remoteVersion;

        request->send(200, "application/json", "{\"success\":true,\"message\":\"Firmware updated, rebooting...\"}");
        delay(1000);
        ESP.restart();
    });

    // POST OTA LittleFS update with version check
    server.on("/api/ota/littlefs", HTTP_POST, [](AsyncWebServerRequest *request){
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
        
        // Build LittleFS URL
        String littlefsUrl = config.hubLittlefsOtaUrl;
        if (!littlefsUrl.endsWith("/")) littlefsUrl += "/";
        littlefsUrl += "littlefs.bin";
        
        Serial.printf("[OTA] Updating LittleFS from %s to %s\n", 
                      config.hubLittlefsVersion.c_str(), remoteVersion.c_str());
        
        String error;
        bool ok = performOtaUpdate(littlefsUrl, true, error);
        if (!ok) {
            String response = String("{\"success\":false,\"error\":\"") + error + "\"}";
            request->send(500, "application/json", response);
            return;
        }
        
        // Update version in hub_config.txt before reboot
        updateHubConfigValue("HUB_LITTLEFS_VERSION", remoteVersion);
        config.hubLittlefsVersion = remoteVersion;

        request->send(200, "application/json", "{\"success\":true,\"message\":\"LittleFS updated, rebooting...\"}");
        delay(1000);
        ESP.restart();
    });

    // ========================================================================
    // NODE OTA ENDPOINTS
    // ========================================================================

    // GET Light node current version (from first online light device)
    server.on("/api/nodes/light/version", HTTP_GET, [](AsyncWebServerRequest *request){
        // Find first online light device from devices.json
        File file = LittleFS.open("/config/devices.json", "r");
        if (!file) {
            request->send(200, "application/json", "{\"version\":null,\"error\":\"devices.json not found\"}");
            return;
        }

        DynamicJsonDocument doc(8192);
        DeserializationError error = deserializeJson(doc, file);
        file.close();

        if (error) {
            request->send(200, "application/json", "{\"version\":null,\"error\":\"JSON parse error\"}");
            return;
        }

        JsonArray devices = doc["devices"];
        for (JsonObject device : devices) {
            String type = device["type"].as<String>();
            String status = device["status"].as<String>();
            if (type == "LIGHT" && status == "ONLINE") {
                uint8_t version = device["firmwareVersion"] | 0;
                String response = "{\"version\":" + String(version) + "}";
                request->send(200, "application/json", response);
                return;
            }
        }

        request->send(200, "application/json", "{\"version\":null,\"error\":\"No online light device\"}");
    });

    // GET Light node list (all light devices with online status and tank names)
    server.on("/api/nodes/light/list", HTTP_GET, [](AsyncWebServerRequest *request){
        DynamicJsonDocument responseDoc(8192);
        JsonArray deviceArray = responseDoc.createNestedArray("devices");

        // Load tank name map from aquariums.json
        std::map<uint8_t, String> tankNames;
        File tanksFile = LittleFS.open("/config/aquariums.json", "r");
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
        File devFile = LittleFS.open("/config/devices.json", "r");
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
            if (typeUpper != "LIGHT") continue;

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

    // POST Check for light node update
    server.on("/api/nodes/light/check-update", HTTP_POST, [](AsyncWebServerRequest *request){
        if (config.lightNodeOtaUrl.length() == 0) {
            request->send(200, "application/json", "{\"error\":\"LIGHT_NODE_OTA_URL not configured in hub_config.txt\"}");
            return;
        }

        // Fetch version.txt
        std::unique_ptr<WiFiClient> client;
        String baseUrl = config.lightNodeOtaUrl;
        if (!baseUrl.endsWith("/")) baseUrl += "/";

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

        // Get current version from the first *actually online* light device (use live peer status)
        int currentVersion = 0;
        File devFile = LittleFS.open("/config/devices.json", "r");
        if (devFile) {
            DynamicJsonDocument devDoc(8192);
            if (!deserializeJson(devDoc, devFile)) {
                JsonArray devices = devDoc["devices"];
                for (JsonObject device : devices) {
                    String type = device["type"].as<String>();
                    if (type.length() == 0) continue;

                    // Case-insensitive type check
                    String typeUpper = type;
                    typeUpper.toUpperCase();
                    if (typeUpper != "LIGHT") continue;

                    String macStr = device["mac"].as<String>();
                    if (macStr.length() == 0) continue;

                    uint8_t mac[6];
                    if (sscanf(macStr.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                              &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6) {
                        continue;
                    }

                    // Use live peer status (heartbeat/announce) to determine online
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

        // Check firmware.bin
        if (!http.begin(*client, baseUrl + "firmware.bin")) {
            Serial.println(" Failed to begin firmware.bin check");
        } else {
            httpCode = http.sendRequest("HEAD");
            hasFirmware = (httpCode == HTTP_CODE_OK);
            http.end();
        }

        // Check node_config.txt
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

        String response;
        serializeJson(responseDoc, response);
        request->send(200, "application/json", response);
    });

    // POST Apply light node update (async) - updates selected LIGHT devices
    server.on("/api/nodes/light/apply-update", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        if (index == 0) {
            Serial.println("[NodeOTA] Received apply-update request");
        }

        if (index + len != total) {
            return;
        }

        // Check if OTA already in progress
        if (nodeOtaState.active) {
            request->send(200, "application/json", "{\"success\":false,\"error\":\"OTA transfer already in progress\"}");
            return;
        }

        if (config.lightNodeOtaUrl.length() == 0) {
            request->send(200, "application/json", "{\"success\":false,\"error\":\"LIGHT_NODE_OTA_URL not configured\"}");
            return;
        }

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

        String baseUrl = config.lightNodeOtaUrl;
        if (!baseUrl.endsWith("/")) baseUrl += "/";

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
            request->send(200, "application/json", "{\"success\":false,\"error\":\"No selected online light devices found\"}");
            return;
        }

        // Initialize OTA state
        memset(&nodeOtaState, 0, sizeof(nodeOtaState));
        nodeOtaState.active = true;
        nodeOtaState.baseUrl = baseUrl;
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

        Serial.printf("[NodeOTA] Started async transfer to %d device(s)\n", targetCount);
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

    // GET OTA transfer status
    server.on("/api/nodes/light/ota-status", HTTP_GET, [](AsyncWebServerRequest *request){
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
            File devFile = LittleFS.open("/config/devices.json", "r");
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
            File unmappedFile = LittleFS.open("/config/unmapped-devices.json", "r");
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
            unmappedFile = LittleFS.open("/config/unmapped-devices.json", "w");
            serializeJson(unmappedDoc, unmappedFile);
            unmappedFile.close();

            // Add to devices.json (create devices array if missing)
            File devicesFile = LittleFS.open("/config/devices.json", "r");
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

            devicesFile = LittleFS.open("/config/devices.json", "w");
            serializeJson(devicesDoc, devicesFile);
            devicesFile.close();
            
            Serial.printf(" Device provisioned: %s\n", deviceName.c_str());
            
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
                        Serial.printf(" ✅ Device added to aquarium object (deviceCount now: %d)\n", 
                                     aquarium->getDeviceCount());
                    } else {
                        Serial.println(" ⚠️ Failed to add device to aquarium object");
                        delete device;
                    }
                } else {
                    Serial.println(" ⚠️ Failed to create device object (unknown type)");
                }
            } else {
                Serial.printf(" ⚠️ Aquarium %d not found in memory!\n", tankId);
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
        File devicesFile = LittleFS.open("/config/devices.json", "r");
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
            devicesFile = LittleFS.open("/config/devices.json", "w");
            serializeJson(devicesDoc, devicesFile);
            devicesFile.close();
        }

        // Remove from unmapped-devices.json
        File unmappedFile = LittleFS.open("/config/unmapped-devices.json", "r");
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
            unmappedFile = LittleFS.open("/config/unmapped-devices.json", "w");
            serializeJson(unmappedDoc, unmappedFile);
            unmappedFile.close();
        }

        // Remove from light-devices.json
        File lightFile = LittleFS.open("/config/light-devices.json", "r");
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
            lightFile = LittleFS.open("/config/light-devices.json", "w");
            serializeJson(lightDoc, lightFile);
            lightFile.close();
        }

        // Remove from light-schedule.json
        File schedFile = LittleFS.open("/config/schedule/light-schedule.json", "r");
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
            schedFile = LittleFS.open("/config/schedule/light-schedule.json", "w");
            serializeJson(schedDoc, schedFile);
            schedFile.close();
        }

        // Also remove from memory and peer list
        uint8_t mac[6];
        if (sscanf(macStr.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                  &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6) {
            AquariumManager::getInstance().removeDevice(mac);
            ESPNowManager::getInstance().removePeer(mac);
        }

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
        
        Serial.printf(" ✅ Command sent to device %s\\n", macStr.c_str());
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
            File devicesFile = LittleFS.open("/config/devices.json", "r");
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

                devicesFile = LittleFS.open("/config/devices.json", "w");
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
                    Serial.printf(" ✅ Device removed from aquarium object (deviceCount now: %d)\n", 
                                 aquarium->getDeviceCount());
                } else {
                    Serial.println(" ⚠️ Failed to remove device from aquarium object");
                }
            } else {
                Serial.printf(" ⚠️ Aquarium %d not found or device not in aquarium!\n", tankId);
            }
            */
            
            // Add back to unmapped devices
            File unmappedFile = LittleFS.open("/config/unmapped-devices.json", "r");
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

            unmappedFile = LittleFS.open("/config/unmapped-devices.json", "w");
            serializeJson(unmappedDoc, unmappedFile);
            unmappedFile.close();
            
            Serial.printf(" Device unmapped: %s\n", macStr.c_str());
            
            // Send success response
            String response = "{\"success\":true,\"message\":\"Device unmapped successfully\"}";
            request->send(200, "application/json", response);
        }
    });
    

    // ===== Static File Serving (MUST be registered LAST) =====
    // These catch-all handlers should come after all API routes

    // WebSocket setup
    ws.onEvent(onWebSocketEvent);
    server.addHandler(&ws);

    // Explicit static handlers for resource directories to ensure correct mapping
    server.serveStatic("/styles", LittleFS, "/UI/styles");
    server.serveStatic("/images", LittleFS, "/UI/images");
    server.serveStatic("/scripts", LittleFS, "/UI/scripts");
    server.serveStatic("/fonts", LittleFS, "/UI/fonts");
    // Fallback: map root to UI directory and serve index.html by default
    server.serveStatic("/", LittleFS, "/UI/").setDefaultFile("index.html");

    // Serve config directory (read-only)
    server.serveStatic("/config", LittleFS, "/config/");

    // 404 handler: attempt to serve files directly from /UI if present
    server.onNotFound([](AsyncWebServerRequest *request){
        String url = request->url();
        String fsPath = "/UI" + url;

        // Normalize directory requests to index.html
        if (url.endsWith("/")) {
            fsPath += "index.html";
        }

        // Try to serve the file from LittleFS
        if (LittleFS.exists(fsPath)) {
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

            request->send(LittleFS, fsPath.c_str(), contentType.c_str());
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

    // --- ntfy.sh notification ---
    // Load topic from config file
    String ntfyTopic = "";
    File configFile = LittleFS.open("/config/hub_config.txt", "r");
    if (configFile) {
        while (configFile.available()) {
            String line = configFile.readStringUntil('\n');
            line.trim();
            if (line.startsWith("NTFY_TOPIC=")) {
                ntfyTopic = line.substring(String("NTFY_TOPIC=").length());
                ntfyTopic.trim();
                break;
            }
        }
        configFile.close();
    }
    if (ntfyTopic.length() > 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), NTFY_MSG_WEBSERVER_UP, WiFi.localIP().toString().c_str());
        String url = "https://ntfy.sh/" + ntfyTopic;
        HTTPClient http;
        http.begin(url);
        http.addHeader("Title", "AMS Hub WebUI");
        int httpCode = http.POST(msg);
        if (httpCode > 0) {
            Serial.printf("[ntfy] Notification sent: %s\n", msg);
        } else {
            Serial.printf("[ntfy] Notification failed: %d\n", httpCode);
        }
        http.end();
    } else {
        Serial.println("[ntfy] NTFY_TOPIC not set in config, notification not sent.");
    }
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
    
    // Determine tankId to include in ACK. Prefer the hub's devices.json record
    int ackTankId = msg.header.tankId;
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    File devFile = LittleFS.open("/config/devices.json", "r");
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
                    Serial.printf(" [HUB] ANNOUNCE: device found in devices.json (tank %d), using that for ACK\n", ackTankId);
                    break;
                }
            }
        }
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
            File fwFile = LittleFS.open("/ota/light/firmware.bin", "r");
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
    // Initialize serial - HWCDC needs time to enumerate after boot
    delay(2000);  // Wait for USB CDC to enumerate
    Serial.begin(115200);
    while (!Serial && millis() < 5000);  // Wait up to 5s for serial connection
    delay(500);
    
    Serial.println("=== SERIAL INITIALIZED ===");
    Serial.println("\n\n");
    Serial.println("");
    Serial.println("   AQUARIUM MANAGEMENT SYSTEM - HUB");
    Serial.println("   ESP32-S3-N16R8 Central Controller");
    Serial.println("");
    Serial.println();
    
    // Initialize filesystem
    if (!setupFilesystem()) {
        Serial.println(" CRITICAL: Filesystem failed, halting");
        while (1) delay(1000);
    }
    
    // Load configuration
    loadConfiguration();
    
    // Setup WiFi
    setupWiFi();
    
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
    Serial.printf(" Watchdog task created on Core 0 (priority 2)\n");

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
        Serial.println(" Hub Heartbeat task created on Core 1");
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
    Serial.printf(" Web UI task created on Core 1 (priority 1)\n");
    
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
    Serial.printf(" Light Scheduler task created on Core 1 (priority 1)\n");
    
    Serial.println();
    Serial.println("");
    Serial.println(" HUB READY");
    Serial.println("");
    Serial.println();
    
    // Print initial memory status
    printMemoryStatus();
}

void loop() {
    // Main loop runs on Core 0
    // Processes ESP-NOW messages from queue and scheduler
    // Web UI runs on Core 1
    // Watchdog task runs independently on Core 0
    
    // Process ESP-NOW messages via ESPNowManager (non-blocking)
    ESPNowManager::getInstance().processQueue();
    
    // Check for peer timeouts (60 second timeout)
    ESPNowManager::getInstance().checkPeerTimeouts(60000);
    
    // Update AquariumManager (schedule execution only)
    // Note: Health checks and water monitoring run on Core 0 watchdog task
    AquariumManager::getInstance().updateSchedules();
    
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
