/*
 * Aquarium Management System - Hub Firmware
 * ESP32-S3-N16R8 Central Controller
 * 
 * Architecture:
 * - Core 0: Main loop - ESP-NOW message processing, web server, system orchestration
 * - Core 1: Watchdog task - Device health monitoring, heartbeat timeouts, fail-safe triggers
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
#include <esp_now.h>
#include <ArduinoJson.h>
#include "protocol/messages.h"
#include "models/Aquarium.h"
#include "managers/AquariumManager.h"
#include <HTTPClient.h>
#include "Constant.h"
#include "ESPNowManager.h"

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
};

HubConfig config;

// Task handles
TaskHandle_t watchdogTaskHandle = NULL;

// ESPNowManager callbacks declared here
void onAnnounceReceived(const uint8_t* mac, const AnnounceMessage& msg);
void onHeartbeatReceived(const uint8_t* mac, const HeartbeatMessage& msg);
void onStatusReceived(const uint8_t* mac, const StatusMessage& msg);
void onCommandReceived(const uint8_t* mac, const uint8_t* data, size_t len);

// Web server
AsyncWebServer server(80);

// WiFiManager
WiFiManager wifiManager;

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
// WATCHDOG TASK (Core 1) - Device Health Monitoring
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
    
    // Serve static files from LittleFS
    server.serveStatic("/", LittleFS, "/UI/").setDefaultFile("index.html");
    
    // Serve config directory (read-only)
    server.serveStatic("/config", LittleFS, "/config/");
    
    // API endpoints (placeholder for future)
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request){
        String json = "{";
        json += "\"uptime\":" + String(millis() / 1000) + ",";
        json += "\"heap_free\":" + String(ESP.getFreeHeap()) + ",";
        json += "\"psram_free\":" + String(ESP.getFreePsram()) + ",";
        json += "\"wifi_rssi\":" + String(WiFi.RSSI());
        json += "}";
        request->send(200, "application/json", json);
    });
    
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
            obj["deviceCount"] = aquarium->getDeviceCount();
            
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
    
    // DELETE aquarium
    server.on("^\\/api\\/aquariums\\/([0-9]+)$", HTTP_DELETE, [](AsyncWebServerRequest *request){
        String idStr = request->pathArg(0);
        uint8_t id = idStr.toInt();
        
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
        
        String jsonData = file.readString();
        file.close();
        
        request->send(200, "application/json", jsonData);
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
            
            // Remove from unmapped devices
            unmappedDevices.remove(foundIndex);
            
            // Save updated unmapped devices
            unmappedFile = LittleFS.open("/config/unmapped-devices.json", "w");
            serializeJson(unmappedDoc, unmappedFile);
            unmappedFile.close();
            
            // Add to devices.json
            File devicesFile = LittleFS.open("/config/devices.json", "r");
            DynamicJsonDocument devicesDoc(8192);
            if (devicesFile) {
                deserializeJson(devicesDoc, devicesFile);
                devicesFile.close();
            }
            
            JsonArray devices = devicesDoc["devices"];
            JsonObject newDevice = devices.createNestedObject();
            newDevice["mac"] = macStr;
            newDevice["type"] = foundDevice["type"];
            newDevice["name"] = deviceName;
            newDevice["tankId"] = tankId;
            newDevice["firmwareVersion"] = foundDevice["firmwareVersion"];
            newDevice["enabled"] = true;
            newDevice["status"] = "PROVISIONING";
            
            devicesFile = LittleFS.open("/config/devices.json", "w");
            serializeJson(devicesDoc, devicesFile);
            devicesFile.close();
            
            Serial.printf(" Device provisioned: %s\n", deviceName.c_str());
            
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
            
            // Load devices.json
            File devicesFile = LittleFS.open("/config/devices.json", "r");
            if (!devicesFile) {
                request->send(404, "application/json", "{\"success\":false,\"error\":\"Devices file not found\"}");
                return;
            }
            
            DynamicJsonDocument devicesDoc(8192);
            deserializeJson(devicesDoc, devicesFile);
            devicesFile.close();
            
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
                request->send(404, "application/json", "{\"success\":false,\"error\":\"Device not found\"}");
                return;
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
            
            // Remove from devices.json
            devices.remove(foundIndex);
            
            devicesFile = LittleFS.open("/config/devices.json", "w");
            serializeJson(devicesDoc, devicesFile);
            devicesFile.close();
            
            // Add back to unmapped devices
            File unmappedFile = LittleFS.open("/config/unmapped-devices.json", "r");
            DynamicJsonDocument unmappedDoc(4096);
            if (unmappedFile) {
                deserializeJson(unmappedDoc, unmappedFile);
                unmappedFile.close();
            }
            
            JsonArray unmappedDevices = unmappedDoc["unmappedDevices"];
            JsonObject newUnmapped = unmappedDevices.createNestedObject();
            newUnmapped["mac"] = macStr;
            newUnmapped["type"] = foundDevice["type"];
            newUnmapped["firmwareVersion"] = foundDevice["firmwareVersion"];
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
    
    // 404 handler
    server.onNotFound([](AsyncWebServerRequest *request){
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
    
    // Add peer and send ACK
    ESPNowManager::getInstance().addPeer(mac);
    
    AckMessage ack = {};
    ack.header.type = MessageType::ACK;
    ack.header.tankId = msg.header.tankId;
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
    if (config.debugESPNOW) {
        Serial.printf(" HEARTBEAT from %02X:%02X:%02X:%02X:%02X:%02X | "
                      "Health: %d%% | Uptime: %dmin\n",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                      msg.health, msg.uptimeMinutes);
    }
    
    // Update peer online status
    ESPNowManager::getInstance().updatePeerHeartbeat(mac);
    
    // Forward to AquariumManager
    AquariumManager::getInstance().handleHeartbeat(mac, msg);
}

void onStatusReceived(const uint8_t* mac, const StatusMessage& msg) {
    if (config.debugESPNOW) {
        Serial.println("");
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
    
    // Setup web server
    setupWebServer();
    
    // Load aquariums from JSON file
    loadAquariumsFromFile();
    
    // Setup ESP-NOW (callbacks run on Core 0, processed in main loop)
    setupESPNow();
    
    // Start watchdog task on Core 1 (device health monitoring)
    xTaskCreatePinnedToCore(
        watchdogTask,            // Task function
        "Watchdog",              // Name
        8192,                    // Stack size (8KB - needs more for AquariumManager calls)
        NULL,                    // Parameters
        2,                       // Priority (higher than main loop)
        &watchdogTaskHandle,     // Task handle
        1                        // Core 1 (separate from main processing)
    );
    Serial.printf(" Watchdog task created on Core 1 (priority 2)\n");
    
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
    // Processes ESP-NOW messages from queue
    // Web server runs asynchronously on Core 0
    // Watchdog task runs independently on Core 1
    
    // Process ESP-NOW messages via ESPNowManager (non-blocking)
    ESPNowManager::getInstance().processQueue();
    
    // Check for peer timeouts (60 second timeout)
    ESPNowManager::getInstance().checkPeerTimeouts(60000);
    
    // Update AquariumManager (schedule execution only)
    // Note: Health checks and water monitoring run on Core 1 watchdog task
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
