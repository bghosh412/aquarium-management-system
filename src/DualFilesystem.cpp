/**
 * Dual LittleFS Filesystem Implementation
 * 
 * Manages two LittleFS partitions:
 * - static_fs: UI files, OTA, hub_config (OTA-updateable)
 * - user_fs: User JSON data (preserved during OTA)
 */

#include "DualFilesystem.h"
#include <Arduino.h>

// ============================================================================
// FILESYSTEM INSTANCES
// ============================================================================

// Create separate LittleFS instances for each partition
fs::LittleFSFS StaticFS;
fs::LittleFSFS UserFS;

// ============================================================================
// INITIALIZATION
// ============================================================================

bool setupDualFilesystem(bool formatOnFail) {
    Serial.println(" Initializing dual LittleFS partitions...");
    
    // Mount static filesystem (UI, OTA, hub_config)
    Serial.println("   Mounting static_fs partition...");
    if (!StaticFS.begin(formatOnFail, "/static", 10, STATIC_FS_PARTITION)) {
        Serial.println("   ERROR: Failed to mount static_fs!");
        return false;
    }
    Serial.println("   static_fs mounted OK");
    
    // Mount user filesystem (JSON config files)
    Serial.println("   Mounting user_fs partition...");
    if (!UserFS.begin(formatOnFail, "/user", 10, USER_FS_PARTITION)) {
        Serial.println("   ERROR: Failed to mount user_fs!");
        return false;
    }
    Serial.println("   user_fs mounted OK");
    
    // Create default directories if they don't exist
    if (!UserFS.exists("/config")) {
        UserFS.mkdir("/config");
        Serial.println("   Created /config directory in user_fs");
    }
    if (!UserFS.exists("/config/schedule")) {
        UserFS.mkdir("/config/schedule");
        Serial.println("   Created /config/schedule directory in user_fs");
    }
    
    Serial.println(" Dual filesystem initialized successfully");
    return true;
}

void printFilesystemInfo() {
    Serial.println("\n Filesystem Information:");
    
    // Static filesystem info
    size_t staticTotal = StaticFS.totalBytes();
    size_t staticUsed = StaticFS.usedBytes();
    Serial.printf("   static_fs: %u / %u bytes (%.1f%% used)\n", 
                  staticUsed, staticTotal, 
                  staticTotal > 0 ? (staticUsed * 100.0 / staticTotal) : 0);
    
    // User filesystem info
    size_t userTotal = UserFS.totalBytes();
    size_t userUsed = UserFS.usedBytes();
    Serial.printf("   user_fs:   %u / %u bytes (%.1f%% used)\n",
                  userUsed, userTotal,
                  userTotal > 0 ? (userUsed * 100.0 / userTotal) : 0);
    
    // List static_fs root
    Serial.println("\n   static_fs contents:");
    File staticRoot = StaticFS.open("/");
    if (staticRoot) {
        File f = staticRoot.openNextFile();
        while (f) {
            Serial.printf("     %s %s (%u bytes)\n", 
                          f.isDirectory() ? "[D]" : "   ",
                          f.name(), f.size());
            f = staticRoot.openNextFile();
        }
        staticRoot.close();
    }
    
    // List user_fs root
    Serial.println("\n   user_fs contents:");
    File userRoot = UserFS.open("/");
    if (userRoot) {
        File f = userRoot.openNextFile();
        while (f) {
            Serial.printf("     %s %s (%u bytes)\n",
                          f.isDirectory() ? "[D]" : "   ",
                          f.name(), f.size());
            f = userRoot.openNextFile();
        }
        userRoot.close();
    }
    Serial.println();
}
