#ifndef PROTOCOL_MESSAGES_H
#define PROTOCOL_MESSAGES_H

#include <stdint.h>

// ESP-NOW configuration
#define MAX_TANK_ID 255
#define MAX_NODE_NAME_LEN 16

// Message types for ESP-NOW communication
enum class MessageType : uint8_t {
    ANNOUNCE = 0x01,    // Node announces itself to hub (discovery)
    ACK = 0x02,         // Hub acknowledges node
    CONFIG = 0x03,      // Hub sends configuration to node (provisioning)
    COMMAND = 0x04,     // Hub sends command to node
    STATUS = 0x05,      // Node sends status to hub
    HEARTBEAT = 0x06,   // Periodic alive signal
    UNMAP = 0x07        // Hub unmaps a device (reset to discovery mode)
};

// Node types in the system
enum class NodeType : uint8_t {
    UNKNOWN = 0x00,
    HUB = 0x01,
    LIGHT = 0x02,
    CO2 = 0x03,
    DOSER = 0x04,
    SENSOR = 0x05,
    HEATER = 0x06,
    FILTER = 0x07,
    FISH_FEEDER = 0x08,
    REPEATER = 0x09
};

// Base message header (included in all messages)
struct MessageHeader {
    MessageType type;
    uint8_t tankId;
    NodeType nodeType;
    uint32_t timestamp;  // millis() when sent
    uint8_t sequenceNum; // For tracking message order
} __attribute__((packed));

// ANNOUNCE message - sent by nodes on boot (discovery phase)
// Node does NOT know tankId or name yet - hub will provision via CONFIG
struct AnnounceMessage {
    MessageHeader header;          // tankId = 0 (unmapped), nodeType = device type
    uint8_t firmwareVersion;
    uint8_t capabilities;          // Bitfield for node capabilities
    uint8_t reserved[16];          // Reserved for future use
} __attribute__((packed));

// ACK message - hub response to ANNOUNCE
struct AckMessage {
    MessageHeader header;
    uint8_t assignedNodeId;  // Hub-assigned unique ID
    bool accepted;           // Whether node is accepted into network
    uint8_t returnMac[6];    // Hub AP MAC that node should use for replies
    uint32_t unixTimestamp;  // Current Unix timestamp from hub
    uint8_t reserved[2];     // Reserved for future use
} __attribute__((packed));

// CONFIG message - hub sends configuration to node (provisioning)
struct ConfigMessage {
    MessageHeader header;          // tankId = assigned tank
    char deviceName[MAX_NODE_NAME_LEN];  // Friendly name from hub
    uint8_t configData[32];        // Device-specific config
} __attribute__((packed));

// COMMAND message - hub to node (control commands)
struct CommandMessage {
    MessageHeader header;          // tankId = device's assigned tank
    uint8_t commandId;
    uint8_t commandSeqID;          // sequenceID for commands, incase larger command needs to be sent to nodes, then this increases by 1 for each sub messages
    bool finalCommand;             // True if final message sequence, else False if there are subsequent messages to be followed
    uint8_t returnMac[6];          // Hub AP MAC for node to reply to
    uint8_t commandData[32];       // Generic command payload
} __attribute__((packed));

// STATUS message - node to hub
struct StatusMessage {
    MessageHeader header;
    uint8_t commandId; // CommandID to be used by Hub for ack
    uint8_t statusCode;
    uint8_t statusData[32];  // Generic status payload
} __attribute__((packed));

// HEARTBEAT message - bidirectional
struct HeartbeatMessage {
    MessageHeader header;
    uint8_t health;  // 0-100 health indicator
    uint16_t uptimeMinutes;
    uint32_t nodeUnixTime;  // Node's current Unix timestamp
} __attribute__((packed));

// UNMAP message - hub to node (reset device to discovery mode)
struct UnmapMessage {
    MessageHeader header;
    uint8_t reason;  // Reason code for unmapping
    uint8_t reserved[8];  // Reserved for future use
} __attribute__((packed));

// ============================================================================
// OTA COMMAND TYPES (Common to all nodes)
// ============================================================================
// These commands are used for over-the-air updates
// F01 = Firmware OTA chunk
// N01 = Node config file chunk

// OTA command identifiers (first byte of commandData)
#define OTA_CMD_FIRMWARE_CHUNK   0xF1  // F01: Firmware binary chunk
#define OTA_CMD_CONFIG_CHUNK     0xC1  // C01: Config file chunk (node_config.txt)
#define OTA_CMD_OTA_BEGIN        0xA0  // Begin OTA transfer
#define OTA_CMD_OTA_END          0xA1  // End OTA transfer (apply)

// OTA begin payload structure (commandData for OTA_CMD_OTA_BEGIN)
// commandData[0] = OTA_CMD_OTA_BEGIN
// commandData[1] = OTA type (0xF1=firmware, 0xC1=config)
// commandData[2-5] = Total size (uint32_t, little-endian)
// commandData[6] = Chunk size (max chunk payload size)

// OTA chunk payload structure (commandData for OTA_CMD_FIRMWARE_CHUNK/OTA_CMD_CONFIG_CHUNK)
// commandData[0] = Command type (0xF1 or 0xC1)
// commandData[1-2] = Chunk index (uint16_t, little-endian)
// commandData[3-31] = Data (up to 29 bytes per chunk)

// OTA end payload structure (commandData for OTA_CMD_OTA_END)
// commandData[0] = OTA_CMD_OTA_END
// commandData[1] = OTA type (0xF1=firmware, 0xC1=config)
// commandData[2-5] = CRC32 checksum (optional, 0 to skip)

// OTA status codes (in StatusMessage.statusCode)
#define OTA_STATUS_OK            0x00  // Success
#define OTA_STATUS_ERROR         0x01  // General error
#define OTA_STATUS_CHUNK_OK      0x10  // Chunk received OK
#define OTA_STATUS_CHUNK_ERR     0x11  // Chunk receive error
#define OTA_STATUS_BEGIN_OK      0x20  // OTA begin OK
#define OTA_STATUS_BEGIN_ERR     0x21  // OTA begin error
#define OTA_STATUS_NEED_BEGIN    0x22  // Node needs BEGIN re-send (missed initial BEGIN)
#define OTA_STATUS_READY_END     0x25  // Node received all chunks, ready for END
#define OTA_STATUS_APPLY_OK      0x30  // OTA applied, rebooting
#define OTA_STATUS_APPLY_ERR     0x31  // OTA apply failed

// Maximum message size check (ESP-NOW limit is 250 bytes)
static_assert(sizeof(AnnounceMessage) <= 250, "AnnounceMessage too large for ESP-NOW");
static_assert(sizeof(AckMessage) <= 250, "AckMessage too large for ESP-NOW");
static_assert(sizeof(ConfigMessage) <= 250, "ConfigMessage too large for ESP-NOW");
static_assert(sizeof(CommandMessage) <= 250, "CommandMessage too large for ESP-NOW");
static_assert(sizeof(StatusMessage) <= 250, "StatusMessage too large for ESP-NOW");
static_assert(sizeof(HeartbeatMessage) <= 250, "HeartbeatMessage too large for ESP-NOW");
static_assert(sizeof(UnmapMessage) <= 250, "UnmapMessage too large for ESP-NOW");

#endif // PROTOCOL_MESSAGES_H
