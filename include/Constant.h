#ifndef CONSTANT_H
#define CONSTANT_H


// ntfy.sh notification messages for AMS Hub
#define NTFY_MSG_BOOT "AMS Hub booted successfully."
#define NTFY_MSG_WIFI_FAIL "AMS Hub WiFi connection failed."
#define NTFY_MSG_HEARTBEAT_WARN "AMS Hub: Heartbeat warning."
#define NTFY_MSG_MEMORY_WARN "AMS Hub: Memory warning."
#define NTFY_MSG_NODE_TIMEOUT "AMS Hub: Node timeout detected."
#define NTFY_MSG_FAILSAFE_TRIGGERED "AMS Hub: Fail-safe triggered."
#define NTFY_MSG_CONFIG_LOADED "AMS Hub: Configuration loaded."
#define NTFY_MSG_CONFIG_ERROR "AMS Hub: Configuration error."
#define NTFY_MSG_WEBSOCKET_CLIENT "AMS Hub: WebSocket client event."
#define NTFY_MSG_EMERGENCY_SHUTDOWN "AMS Hub: Emergency shutdown triggered."

#define NTFY_MSG_WEBSERVER_UP "AMS HUB UI is up and running on %s"

// Scheduler notifications
#define NTFY_MSG_TASK_EXECUTED "Task executed: %s → %s [%s] — %s"
// args: aquariumName, deviceName, deviceType, actionDescription

// Device discovery notifications
#define NTFY_MSG_DEVICE_DISCOVERED "New device discovered: %s (%s) — awaiting provisioning"
// args: macAddress, deviceType
#define NTFY_MSG_DEVICE_PROVISIONED "Device provisioned: %s → %s (Tank %d)"
// args: deviceName, macAddress, tankId

// Device offline notifications
#define NTFY_MSG_DEVICE_OFFLINE "%s in %s is offline (no heartbeat for 6 min)"
// args: deviceName, aquariumName
#define NTFY_MSG_DEVICE_BACK_ONLINE "%s in %s is back online"
// args: deviceName, aquariumName

// Aquarium CRUD notifications
#define NTFY_MSG_AQUARIUM_CREATED "Aquarium created: %s (ID %d)"
#define NTFY_MSG_AQUARIUM_UPDATED "Aquarium updated: %s (ID %d)"
#define NTFY_MSG_AQUARIUM_DELETED "Aquarium deleted: ID %d"

// Device CRUD notifications
#define NTFY_MSG_DEVICE_DELETED "Device deleted: %s"

// CO2 delta control notifications
#define NTFY_MSG_CO2_DELTA_OFF "CO2 auto-OFF: pH drop %.2f in %s (%s)"
// args: delta, deviceName, aquariumName
#define NTFY_MSG_CO2_DELTA_ON "CO2 auto-ON: pH recovered to delta %.2f in %s (%s)"
// args: delta, deviceName, aquariumName

// OTA notifications
#define NTFY_MSG_HUB_OTA_STARTED "Hub OTA started: %s update %s → %s"
// args: type (Firmware/LittleFS), currentVersion, newVersion
#define NTFY_MSG_HUB_OTA_SUCCESS "Hub OTA success: %s updated to %s — rebooting"
// args: type, newVersion
#define NTFY_MSG_HUB_OTA_FAILED "Hub OTA failed: %s — %s"
// args: type, error
#define NTFY_MSG_NODE_OTA_STARTED "Node OTA started: %s (%d devices)"
// args: deviceType, targetCount
#define NTFY_MSG_NODE_OTA_COMPLETE "Node OTA complete: %d updated, %d failed"
// args: devicesUpdated, devicesFailed

#endif // CONSTANT_H
