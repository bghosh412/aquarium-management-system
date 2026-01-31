/**
 * Dual LittleFS Filesystem Abstraction for Hub
 * 
 * Architecture:
 * - STATIC_FS: UI files (HTML/CSS/JS), OTA temp files, hub_config.txt
 *   Partition: static_fs @ 0x610000 (7MB)
 *   Mount: "static_fs" with LittleFS
 * 
 * - USER_FS: User data (JSON config files, schedules)
 *   Partition: user_fs @ 0xD10000 (~3MB)
 *   Mount: "user_fs" with LittleFS
 * 
 * Benefits:
 * - LittleFS OTA only affects static_fs partition
 * - User data in user_fs is PRESERVED during OTA updates
 * - Separate backup/restore for user data
 */

#ifndef DUAL_FILESYSTEM_H
#define DUAL_FILESYSTEM_H

#include <LittleFS.h>
#include <FS.h>

// ============================================================================
// PARTITION NAMES (must match partition table CSV)
// ============================================================================
#define STATIC_FS_PARTITION "static_fs"
#define USER_FS_PARTITION   "user_fs"

// ============================================================================
// FILESYSTEM OBJECTS
// ============================================================================
// We'll use the default LittleFS for static files and a second instance for user data
// ESP32 Arduino supports multiple LittleFS instances with different partition labels

extern fs::LittleFSFS StaticFS;
extern fs::LittleFSFS UserFS;

// ============================================================================
// PATH PREFIXES
// ============================================================================
// Static filesystem paths (OTA-updateable)
#define STATIC_PATH_UI      "/UI"
#define STATIC_PATH_OTA     "/ota"
#define STATIC_PATH_HUBCFG  "/hub_config.txt"

// User filesystem paths (preserved during OTA)  
#define USER_PATH_CONFIG    "/config"

// ============================================================================
// HELPER MACROS FOR FILE ACCESS
// ============================================================================
// Use these for clarity when accessing files

// Static files (UI, OTA, hub_config)
#define STATIC_FILE(path) (StaticFS.open(path, "r"))
#define STATIC_FILE_W(path) (StaticFS.open(path, "w"))
#define STATIC_EXISTS(path) (StaticFS.exists(path))
#define STATIC_MKDIR(path) (StaticFS.mkdir(path))
#define STATIC_REMOVE(path) (StaticFS.remove(path))

// User data files (JSON config, schedules)
#define USER_FILE(path) (UserFS.open(path, "r"))
#define USER_FILE_W(path) (UserFS.open(path, "w"))
#define USER_EXISTS(path) (UserFS.exists(path))
#define USER_MKDIR(path) (UserFS.mkdir(path))
#define USER_REMOVE(path) (UserFS.remove(path))

// ============================================================================
// INITIALIZATION FUNCTIONS
// ============================================================================

/**
 * @brief Initialize dual LittleFS filesystems
 * @param formatOnFail Format partition if mount fails
 * @return true if both filesystems mounted successfully
 */
bool setupDualFilesystem(bool formatOnFail = true);

/**
 * @brief Print filesystem info for debugging
 */
void printFilesystemInfo();

/**
 * @brief Check if dual filesystem is enabled (compile-time check)
 */
inline bool isDualFilesystemEnabled() {
#ifdef DUAL_LITTLEFS
    return true;
#else
    return false;
#endif
}

#endif // DUAL_FILESYSTEM_H
