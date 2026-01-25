# Hub OTA Test Files

Example directory structure for Hub OTA updates.

## Directory Structure

```
hub/
├── firmware/
│   ├── version.txt      # Contains semantic version (e.g., "1.0.1")
│   └── firmware.bin     # ESP32-S3 firmware binary
│
└── littlefs/
    ├── version.txt      # Contains semantic version (e.g., "1.0.1")
    └── littlefs.bin     # LittleFS filesystem image
```

## version.txt Format

The `version.txt` file should contain a semantic version number:

```
1.0.1
```

Supported formats:
- `1` (major only)
- `1.0` (major.minor)
- `1.0.0` (major.minor.patch)
- `v1.0.0` (optional 'v' prefix)

## How It Works

1. Hub fetches `<OTA_URL>/version.txt`
2. Compares remote version with installed version in `hub_config.txt`
3. If remote version is newer, enables the Update button
4. On update, downloads `<OTA_URL>/firmware.bin` or `<OTA_URL>/littlefs.bin`
5. After successful update, updates version in `hub_config.txt`
6. Hub reboots with new firmware/filesystem

## Configuration

Set these in `hub_config.txt`:

```
# OTA URLs (base URL, trailing slash optional)
HUB_FIRMWARE_OTA_URL=http://192.168.1.100:8088/hub/firmware
HUB_LITTLEFS_OTA_URL=http://192.168.1.100:8088/hub/littlefs

# Current installed versions (updated automatically after OTA)
HUB_FIRMWARE_VERSION=1.0.0
HUB_LITTLEFS_VERSION=1.0.0
```

## Testing with Python HTTP Server

```bash
# Start mock OTA server
cd test/ota
python3 -m http.server 8088

# Set Hub OTA URLs (replace with your IP)
curl -X POST -H "Content-Type: application/json" \
  -d '{"hubFirmwareUrl":"http://192.168.1.100:8088/hub/firmware","hubLittlefsUrl":"http://192.168.1.100:8088/hub/littlefs"}' \
  http://ams.local/api/settings/ota-urls

# Check for updates
curl http://ams.local/api/hub/ota/check

# Trigger firmware update
curl -X POST http://ams.local/api/ota/firmware
```

## API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/settings/ota-urls` | GET | Get current OTA URLs and versions |
| `/api/settings/ota-urls` | POST | Set OTA URLs (runtime only) |
| `/api/hub/ota/check` | GET | Check for available updates |
| `/api/ota/firmware` | POST | Trigger firmware update |
| `/api/ota/littlefs` | POST | Trigger LittleFS update |
