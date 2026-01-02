# Copilot Instructions - Quick Reference

**Navigation map for modular instructions**

---

## 📁 File Structure

```
.github/
├── copilot-instructions.md          # THIS FILE (index/overview)
├── hub-instructions.md              # Hub development (30KB)
├── node-instructions.md             # Node base patterns (23KB)
├── protocol-instructions.md         # ESP-NOW protocol (17KB)
├── build-instructions.md            # PlatformIO builds (15KB)
└── nodes/                           # Node-specific files (~1-2KB each)
    ├── fish-feeder-instructions.md
    ├── co2-regulator-instructions.md
    ├── lighting-instructions.md
    ├── heater-instructions.md
    ├── water-quality-instructions.md
    └── repeater-instructions.md
```

---

## 🎯 What to Read Based on Your Task

| Task | Read These Files |
|------|-----------------|
| **Hub firmware** | `hub-instructions.md` + `protocol-instructions.md` |
| **New node type** | `node-instructions.md` + `protocol-instructions.md` |
| **Modify lighting** | `nodes/lighting-instructions.md` + `node-instructions.md` |
| **Modify CO₂** | `nodes/co2-regulator-instructions.md` + `node-instructions.md` |
| **Modify feeder** | `nodes/fish-feeder-instructions.md` + `node-instructions.md` |
| **Modify heater** | `nodes/heater-instructions.md` + `node-instructions.md` |
| **Modify sensors** | `nodes/water-quality-instructions.md` + `node-instructions.md` |
| **Modify repeater** | `nodes/repeater-instructions.md` + `node-instructions.md` |
| **Protocol changes** | `protocol-instructions.md` + all affected files |
| **Build issues** | `build-instructions.md` |
| **Quick overview** | `copilot-instructions.md` (main index) |

---

## 📊 File Sizes (Approximate)

- **Main index**: 3.6KB (this file)
- **Hub**: 30KB (ESP32-S3, WebUI, FreeRTOS)
- **Node base**: 23KB (patterns, NodeBase library)
- **Protocol**: 17KB (ESP-NOW, messages, discovery)
- **Build**: 15KB (PlatformIO, environments, troubleshooting)
- **Each node**: 1-2KB (hardware-specific details)

**Total**: ~100KB (vs 17KB old monolithic file)

**Benefits**:
- ✅ Load only what you need
- ✅ Faster Copilot context loading
- ✅ Easier to maintain
- ✅ Clearer organization

---

## 🚀 Common Workflows

### Adding a New Node Type

1. Read: `node-instructions.md` (base patterns)
2. Read: `protocol-instructions.md` (communication)
3. Copy template from `node-instructions.md`
4. Create: `nodes/new-node-instructions.md`
5. Build: `build-instructions.md` (add environment)

### Modifying Hub WebUI

1. Read: `hub-instructions.md` (WebUI section)
2. Edit: `src/hub/data/UI/` files
3. Upload: `platformio run -e hub_esp32 -t uploadfs`

### Debugging Communication

1. Read: `protocol-instructions.md` (troubleshooting)
2. Check: Serial monitor on both hub and node
3. Verify: ESP-NOW channel matches (Channel 6)

---

## 💡 Tips

- **Start small**: Read only the files you need
- **Follow links**: Each file references related files
- **Check dates**: "Last Updated" at bottom of each file
- **Search**: Use grep/find for specific topics

```bash
# Find all references to "fail-safe"
grep -r "fail-safe" .github/

# Find command format examples
grep -r "commandData" .github/nodes/
```

---

**Last Updated**: December 29, 2025  
**Version**: 2.0 (Modular)
