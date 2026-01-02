# Migration Notes

## Project Restructure - December 28, 2025

The project has been restructured according to the architecture specified in `context/copilot-instructions.md`.

### Old Structure (deprecated)
```
├── src/main.cpp
├── include/
├── lib/
├── test/
└── platformio.ini
```

### New Structure
```
├── include/
│   └── protocol/        # Shared message definitions
├── src/
│   ├── hub/            # Hub firmware source
│   └── node/           # Node firmware source  
├── platformio.ini      # Multi-environment build config
├── context/            # Documentation
└── README.md
```

This is a **standard PlatformIO structure** with multiple build environments in one project.

### What Changed

1. **Single Project**: One `platformio.ini` with multiple environments (hub_esp32, node_esp8266, node_esp32c3)
2. **Source Filters**: Each environment uses `src_filter` to compile only relevant code
3. **Shared Headers**: Protocol definitions in `include/protocol/`
4. **Standard Layout**: Follows PlatformIO conventions exactly

### Old Files

The following files from the original PlatformIO template are now deprecated:
- `/src/main.cpp` - ✅ Removed, replaced with src/hub/main.cpp and src/node/main.cpp
- `/include/README` - Can be removed
- `/lib/` - Not needed with new structure  
- `/test/` - Tests should be added later if needed

**The old hub/ and node/ subdirectories with separate platformio.ini files have been consolidated.**

### Building

```bash
# All commands run from project root
pio run -e hub_esp32          # Build hub
pio run -e node_esp8266       # Build node for ESP8266
pio run -e node_esp32c3       # Build node for ESP32-C3

# Upload
pio run -e hub_esp32 -t upload
```

### Next Steps

1. Test hub firmware on ESP32-WROOM
2. Configure and test node firmware on ESP8266
3. Verify discovery and heartbeat exchange
4. Begin implementing subsystem-specific control logic
