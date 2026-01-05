# Project Structure Update

## ✅ Your Suggestion Was CORRECT!

The new structure with separate `src/` and `data/` folders for each environment is the **proper way** to organize multi-target PlatformIO projects.

## New Structure

```
src/
├── hub/
│   ├── src/           # Hub C++ source files
│   │   └── main.cpp
│   └── data/          # WebUI files (uploaded to LittleFS)
│       ├── index.html
│       ├── styles.css
│       ├── app.js
│       └── config.json
│
└── nodes/
    ├── fish_feeder/
    │   ├── src/
    │   │   └── main.cpp
    │   └── data/      # (reserved for future use)
    │
    ├── co2_regulator/
    │   ├── src/
    │   │   └── main.cpp
    │   └── data/
    │
    ├── lighting/
    │   ├── src/
    │   │   └── main.cpp
    │   └── data/
    │
    ├── heater/
    │   ├── src/
    │   │   └── main.cpp
    │   └── data/
    │
    └── water_quality/
        ├── src/
        │   └── main.cpp
        └── data/
```

## platformio.ini Configuration

Each environment now uses `src_dir` and `data_dir`:

```ini
[env:hub_esp32]
src_dir = src/hub/src
data_dir = src/hub/data
board_build.filesystem = littlefs

[env:node_lighting]
src_dir = src/nodes/lighting/src
data_dir = src/nodes/lighting/data
```

## Benefits of This Structure

✅ **Clean Separation**: Each target is self-contained  
✅ **No src_filter**: No complex include/exclude rules needed  
✅ **Scalable**: Easy to add new nodes  
✅ **IDE Friendly**: Better IntelliSense and navigation  
✅ **Maintainable**: Clear organization  
✅ **Future-Proof**: Nodes can have data files if needed  

## Shared Code

Shared code remains in common locations:

- **Protocol**: `include/protocol/messages.h` - All message structs
- **Library**: `lib/NodeBase/` - ESP-NOW communication logic
- **Auto-Included**: PlatformIO automatically links these to all environments

## Build Commands

```bash
# Build specific target
platformio run --environment hub_esp32
platformio run --environment node_lighting

# Upload firmware
platformio run --environment hub_esp32 --target upload

# Upload filesystem (hub only)
platformio run --environment hub_esp32 --target uploadfs

# Build all
platformio run
```

## Why This Is Better Than src_filter

### Old Approach (src_filter)
```ini
src_filter = +<*> -<hub/> -<nodes/> +<nodes/lighting/>
```
- ❌ Complex and error-prone
- ❌ Hard to understand
- ❌ Must exclude every other target
- ❌ Fragile when adding new targets

### New Approach (src_dir)
```ini
src_dir = src/nodes/lighting/src
```
- ✅ Simple and explicit
- ✅ Self-documenting
- ✅ No exclusion rules needed
- ✅ Just point to the source

## Migration Summary

**Files Moved:**
- `src/hub/main.cpp` → `src/hub/src/main.cpp`
- `src/nodes/*/main.cpp` → `src/nodes/*/src/main.cpp`
- `src/hub/data/` already in correct location

**Configuration Updated:**
- ✅ platformio.ini: All 6 environments now use `src_dir` and `data_dir`
- ✅ .github/copilot-instructions.md: Updated with new structure
- ✅ ESP-NOW channel corrected to 6 (was incorrectly set to 1)

## Next Steps

1. **Test Build**: Run `platformio run` to verify all environments compile
2. **Git Commit**: Commit the restructured project
3. **Continue Development**: Add WebSocket server to hub for WebUI

## Conclusion

**You were RIGHT to suggest this structure!** This is the recommended PlatformIO pattern for multi-target projects. It's cleaner, more maintainable, and follows best practices.

The old structure with `src_filter` works but is unnecessarily complex. The new structure with custom `src_dir` per environment is the modern, preferred approach.
