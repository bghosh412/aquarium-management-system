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
    String otaFirmwareUrl;
    String otaLittlefsUrl;
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

// ESPNowManager callbacks declared here
void onAnnounceReceived(const uint8_t* mac, const AnnounceMessage& msg);
void onHeartbeatReceived(const uint8_t* mac, const HeartbeatMessage& msg);
void onStatusReceived(const uint8_t* mac, const StatusMessage& msg);
void onCommandReceived(const uint8_t* mac, const uint8_t* data, size_t len);

// Peer registration
void registerAllDevicesAsPeers();

// Web server setup
void setupWebServer();

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
    config.otaFirmwareUrl = "";
    config.otaLittlefsUrl = "";
    
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
        } else if (key == "FIRMWARE_OTA_URL") {
            config.otaFirmwareUrl = value;
        } else if (key == "LITTLEFS_OTA_URL") {
            config.otaLittlefsUrl = value;
        }
    }
    
    file.close();
    
    Serial.println(" Configuration loaded");
    Serial.printf("   - Heartbeat: %s (%ds)\n", 
                  config.heartbeatEnabled ? "ON" : "OFF", 
                  config.heartbeatIntervalSec);
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

    // Initialize light-schedule.json if it doesn't exist
    if (!LittleFS.exists("/config/light-schedule.json")) {
        Serial.println(" Creating light-schedule.json...");
        File file = LittleFS.open("/config/light-schedule.json", "w");
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
    
    // CRITICAL: Force WiFi to STA mode only (not AP+STA)
    // ESP-NOW peer registration with ifidx=WIFI_IF_STA requires STA-only mode
    // Set WiFi to AP only for test: Hub acting purely as AP (receiver)
    WiFi.mode(WIFI_AP);
    Serial.println(" [HUB] WiFi mode set to: WIFI_AP (AP only - test)");


    
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
        String json = "{";
        json += "\"uptime\":" + String(millis() / 1000) + ",";
        json += "\"heap_free\":" + String(ESP.getFreeHeap()) + ",";
        json += "\"psram_free\":" + String(ESP.getFreePsram()) + ",";
        json += "\"wifi_rssi\":" + String(WiFi.RSSI());
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

    // GET light schedule for a device
    server.on("/api/light-schedule", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!request->hasParam("mac")) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing mac\"}");
            return;
        }

        String macStr = request->getParam("mac")->value();
        File file = LittleFS.open("/config/light-schedule.json", "r");
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
            responseDoc["schedule"] = found;
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

        File file = LittleFS.open("/config/light-schedule.json", "r");
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
        newEntry["schedule"] = schedule;
        newEntry["updatedAt"] = millis();

        file = LittleFS.open("/config/light-schedule.json", "w");
        if (!file) {
            request->send(500, "application/json", "{\"success\":false,\"error\":\"Failed to write light-schedule.json\"}");
            return;
        }
        serializeJson(doc, file);
        file.close();

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

    // GET OTA URLs
    server.on("/api/settings/ota-urls", HTTP_GET, [](AsyncWebServerRequest *request){
        JsonDocument doc;
        doc["firmwareUrl"] = config.otaFirmwareUrl;
        doc["littlefsUrl"] = config.otaLittlefsUrl;

        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });

    // POST OTA firmware update
    server.on("/api/ota/firmware", HTTP_POST, [](AsyncWebServerRequest *request){
        String error;
        bool ok = performOtaUpdate(config.otaFirmwareUrl, false, error);
        if (!ok) {
            String response = String("{\"success\":false,\"error\":\"") + error + "\"}";
            request->send(500, "application/json", response);
            return;
        }

        request->send(200, "application/json", "{\"success\":true}");
        delay(1000);
        ESP.restart();
    });

    // POST OTA LittleFS update
    server.on("/api/ota/littlefs", HTTP_POST, [](AsyncWebServerRequest *request){
        String error;
        bool ok = performOtaUpdate(config.otaLittlefsUrl, true, error);
        if (!ok) {
            String response = String("{\"success\":false,\"error\":\"") + error + "\"}";
            request->send(500, "application/json", response);
            return;
        }

        request->send(200, "application/json", "{\"success\":true}");
        delay(1000);
        ESP.restart();
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
        File schedFile = LittleFS.open("/config/light-schedule.json", "r");
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
            schedFile = LittleFS.open("/config/light-schedule.json", "w");
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
    // Initialize serial
    Serial.begin(115200);
    delay(1000);  // Wait for serial to stabilize
    
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
