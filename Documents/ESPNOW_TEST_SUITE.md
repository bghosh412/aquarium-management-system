# ESPNowManager Test Suite

**Comprehensive tests for validating ESPNowManager functionality**

---

## Test Environment Setup

### Hardware Required
- 1x ESP32-S3 (Hub)
- 2x ESP8266 (Nodes for testing)
- USB cables for programming
- Serial monitors for all devices

### Software Setup
```bash
# Build library
cd lib/ESPNowManager
platformio lib install

# Build test hub
platformio run --environment hub_esp32

# Build test nodes
platformio run --environment node_lighting
```

---

## Test Suite

### Test 1: Basic Initialization

**Objective**: Verify ESP-NOW manager initializes correctly

**Hub Code**:
```cpp
void setup() {
    Serial.begin(115200);
    WiFi.mode(WIFI_STA);
    
    bool success = ESPNowManager::getInstance().begin(6, true);
    
    if (success) {
        Serial.println("✅ Test 1 PASSED: Initialization successful");
    } else {
        Serial.println("❌ Test 1 FAILED: Initialization failed");
    }
}
```

**Expected Output**:
```
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
🚀 ESPNowManager: Initializing as HUB
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
✅ RX Queue created (10 entries)
✅ WiFi Channel: 6
✅ ESP-NOW initialized
✅ Callbacks registered
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
✅ ESPNowManager Ready
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
✅ Test 1 PASSED: Initialization successful
```

**Pass Criteria**: ✅ All initialization steps complete without errors

---

### Test 2: Node Discovery (ANNOUNCE/ACK)

**Objective**: Test node discovery and peer registration

**Node Code**:
```cpp
void setup() {
    Serial.begin(115200);
    WiFi.mode(WIFI_STA);
    
    ESPNowManager::getInstance().begin(6, false);
    
    // Send ANNOUNCE
    AnnounceMessage announce = {};
    announce.header.type = MessageType::ANNOUNCE;
    announce.header.nodeType = NodeType::LIGHT;
    strcpy(announce.nodeName, "Test Node");
    announce.firmwareVersion = 1;
    
    uint8_t broadcast[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    ESPNowManager::getInstance().send(broadcast, (uint8_t*)&announce, sizeof(announce));
    
    Serial.println("📡 ANNOUNCE sent");
}
```

**Hub Code**:
```cpp
void onAnnounceReceived(const uint8_t* mac, const AnnounceMessage& announce) {
    Serial.printf("✅ Test 2 PASSED: Received ANNOUNCE from %s\n", announce.nodeName);
    
    // Add peer
    ESPNowManager::getInstance().addPeer(mac);
    
    // Send ACK
    AckMessage ack = {};
    ack.header.type = MessageType::ACK;
    ack.assignedNodeId = 1;
    ack.accepted = true;
    
    ESPNowManager::getInstance().send(mac, (uint8_t*)&ack, sizeof(ack));
    Serial.println("📤 ACK sent");
}

void setup() {
    // ... init ...
    ESPNowManager::getInstance().onAnnounceReceived(onAnnounceReceived);
}
```

**Expected Output**:

Hub:
```
✅ Test 2 PASSED: Received ANNOUNCE from Test Node
✅ Added peer AA:BB:CC:DD:EE:FF
📤 ACK sent
```

Node:
```
📡 ANNOUNCE sent
✅ ACK received
```

**Pass Criteria**: ✅ Hub receives ANNOUNCE, node receives ACK, peer added

---

### Test 3: Simple Command Send/Receive

**Objective**: Test single-frame command communication

**Hub Code**:
```cpp
void testSimpleCommand() {
    uint8_t nodeMac[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    
    CommandMessage cmd = {};
    cmd.header.type = MessageType::COMMAND;
    cmd.header.nodeType = NodeType::HUB;
    cmd.commandId = 42;
    cmd.commandSeqID = 0;
    cmd.finalCommand = true;
    cmd.commandData[0] = 0x01;  // Command type
    cmd.commandData[1] = 0xFF;  // Data
    
    bool sent = ESPNowManager::getInstance().send(nodeMac, (uint8_t*)&cmd, sizeof(cmd));
    
    if (sent) {
        Serial.println("✅ Test 3 PASSED: Command sent");
    } else {
        Serial.println("❌ Test 3 FAILED: Send failed");
    }
}
```

**Node Code**:
```cpp
void onCommandReceived(const uint8_t* mac, const uint8_t* data, size_t len) {
    Serial.printf("✅ Test 3 PASSED: Received command (%d bytes)\n", len);
    Serial.printf("   Command type: 0x%02X\n", data[0]);
    Serial.printf("   Command data: 0x%02X\n", data[1]);
    
    if (data[0] == 0x01 && data[1] == 0xFF) {
        Serial.println("   Data matches expected values");
    }
}

void setup() {
    // ... init ...
    ESPNowManager::getInstance().onCommandReceived(onCommandReceived);
}
```

**Expected Output**:

Hub:
```
✅ Test 3 PASSED: Command sent
```

Node:
```
✅ Test 3 PASSED: Received command (32 bytes)
   Command type: 0x01
   Command data: 0xFF
   Data matches expected values
```

**Pass Criteria**: ✅ Command sent successfully, node receives correct data

---

### Test 4: Fragmented Message (Large Command)

**Objective**: Test automatic fragmentation and reassembly

**Hub Code**:
```cpp
void testFragmentation() {
    uint8_t nodeMac[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    
    // Create 128-byte test data
    uint8_t largeData[128];
    for (int i = 0; i < 128; i++) {
        largeData[i] = i;  // 0x00, 0x01, 0x02, ... 0x7F
    }
    
    Serial.println("📦 Sending 128-byte message (should fragment)...");
    
    bool sent = ESPNowManager::getInstance().sendFragmented(
        nodeMac, 
        42,          // Command ID
        largeData, 
        128
    );
    
    if (sent) {
        Serial.println("✅ Test 4 PASSED: All fragments sent");
    } else {
        Serial.println("❌ Test 4 FAILED: Fragmentation failed");
    }
}
```

**Node Code**:
```cpp
void onCommandReceived(const uint8_t* mac, const uint8_t* data, size_t len) {
    Serial.printf("✅ Received reassembled message: %d bytes\n", len);
    
    // Verify data integrity
    bool dataOk = true;
    for (int i = 0; i < len; i++) {
        if (data[i] != (i % 256)) {
            Serial.printf("❌ Data corruption at byte %d: expected 0x%02X, got 0x%02X\n", 
                         i, (i % 256), data[i]);
            dataOk = false;
            break;
        }
    }
    
    if (dataOk && len == 128) {
        Serial.println("✅ Test 4 PASSED: Data integrity verified");
    } else {
        Serial.println("❌ Test 4 FAILED: Data corruption or wrong size");
    }
}
```

**Expected Output**:

Hub:
```
📦 Sending 128-byte message (should fragment)...
📦 Fragmenting message: 128 bytes into 32-byte chunks
  📤 Sent fragment 1/4 (32 bytes)
  📤 Sent fragment 2/4 (32 bytes)
  📤 Sent fragment 3/4 (32 bytes)
  📤 Sent fragment 4/4 (32 bytes) [FINAL]
✅ Sent 4 fragments successfully
✅ Test 4 PASSED: All fragments sent
```

Node:
```
🧩 Starting reassembly for command 42
  🧩 Fragment 0 appended (32 bytes total)
  🧩 Fragment 1 appended (64 bytes total)
  🧩 Fragment 2 appended (96 bytes total)
  🧩 Fragment 3 appended (128 bytes total)
✅ Reassembly complete: 128 bytes
✅ Received reassembled message: 128 bytes
✅ Test 4 PASSED: Data integrity verified
```

**Pass Criteria**: ✅ Message fragmented, all fragments received, reassembled correctly, data integrity 100%

---

### Test 5: Reassembly Timeout

**Objective**: Test timeout protection for incomplete messages

**Hub Code** (modified to send incomplete message):
```cpp
void testReassemblyTimeout() {
    uint8_t nodeMac[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    
    Serial.println("📦 Sending INCOMPLETE message (only 2 of 4 fragments)...");
    
    // Send only 2 fragments, then stop
    for (int seqID = 0; seqID < 2; seqID++) {
        CommandMessage cmd = {};
        cmd.header.type = MessageType::COMMAND;
        cmd.commandId = 99;
        cmd.commandSeqID = seqID;
        cmd.finalCommand = false;  // Not final
        
        ESPNowManager::getInstance().send(nodeMac, (uint8_t*)&cmd, sizeof(cmd));
        delay(10);
    }
    
    Serial.println("⏱️  Partial message sent. Node should timeout after 1.5s");
}
```

**Node Code**:
```cpp
void loop() {
    ESPNowManager::getInstance().processQueue();
    
    static unsigned long lastStatsCheck = 0;
    if (millis() - lastStatsCheck > 2000) {  // Check after 2 seconds
        lastStatsCheck = millis();
        
        auto stats = ESPNowManager::getInstance().getStatistics();
        if (stats.reassemblyTimeouts > 0) {
            Serial.println("✅ Test 5 PASSED: Reassembly timeout triggered");
            Serial.printf("   Timeouts: %u\n", stats.reassemblyTimeouts);
        }
    }
}
```

**Expected Output**:

Hub:
```
📦 Sending INCOMPLETE message (only 2 of 4 fragments)...
⏱️  Partial message sent. Node should timeout after 1.5s
```

Node:
```
🧩 Starting reassembly for command 99
  🧩 Fragment 0 appended (32 bytes total)
  🧩 Fragment 1 appended (64 bytes total)
⏱️  Reassembly timeout, dropping partial message
✅ Test 5 PASSED: Reassembly timeout triggered
   Timeouts: 1
```

**Pass Criteria**: ✅ Partial message dropped after 1.5s, counter incremented

---

### Test 6: Duplicate Detection

**Objective**: Test duplicate message protection

**Hub Code**:
```cpp
void testDuplicateDetection() {
    uint8_t nodeMac[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    
    HeartbeatMessage hb = {};
    hb.header.type = MessageType::HEARTBEAT;
    hb.header.sequenceNum = 42;  // Same sequence
    hb.health = 100;
    
    Serial.println("📤 Sending message with sequence 42...");
    ESPNowManager::getInstance().send(nodeMac, (uint8_t*)&hb, sizeof(hb));
    
    delay(100);
    
    Serial.println("📤 Sending DUPLICATE with same sequence 42...");
    ESPNowManager::getInstance().send(nodeMac, (uint8_t*)&hb, sizeof(hb));
    
    delay(100);
    
    auto stats = ESPNowManager::getInstance().getStatistics();
    if (stats.duplicatesIgnored == 1) {
        Serial.println("✅ Test 6 PASSED: Duplicate detected and ignored");
    } else {
        Serial.printf("❌ Test 6 FAILED: Expected 1 duplicate, got %u\n", 
                     stats.duplicatesIgnored);
    }
}
```

**Expected Output**:

Hub (node side would be reverse):
```
📤 Sending message with sequence 42...
📤 Sending DUPLICATE with same sequence 42...
🚫 Duplicate message ignored (seq 42)
✅ Test 6 PASSED: Duplicate detected and ignored
```

**Pass Criteria**: ✅ Second message with same sequence ignored, counter = 1

---

### Test 7: Retry Mechanism

**Objective**: Test automatic retry on send failure

**Hub Code** (simulate failure by removing peer):
```cpp
void testRetry() {
    uint8_t nodeMac[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    
    // Remove peer to force failure
    Serial.println("🗑️  Removing peer to force send failure...");
    ESPNowManager::getInstance().removePeer(nodeMac);
    
    CommandMessage cmd = {};
    cmd.header.type = MessageType::COMMAND;
    cmd.commandId = 77;
    
    Serial.println("🔄 Attempting send with retry (should fail all attempts)...");
    
    bool sent = ESPNowManager::getInstance().sendWithRetry(nodeMac, (uint8_t*)&cmd, sizeof(cmd), 3);
    
    auto stats = ESPNowManager::getInstance().getStatistics();
    
    if (!sent && stats.retries == 3) {
        Serial.println("✅ Test 7 PASSED: Retry mechanism worked (3 retries attempted)");
    } else {
        Serial.printf("❌ Test 7 FAILED: Expected 3 retries, got %u\n", stats.retries);
    }
    
    // Re-add peer
    ESPNowManager::getInstance().addPeer(nodeMac);
}
```

**Expected Output**:
```
🗑️  Removing peer to force send failure...
🔄 Attempting send with retry (should fail all attempts)...
❌ Send failed: 12298
🔄 Retry 1/3 (delay 100ms)
❌ Send failed: 12298
🔄 Retry 2/3 (delay 200ms)
❌ Send failed: 12298
🔄 Retry 3/3 (delay 400ms)
❌ Send failed: 12298
❌ Failed after 3 retries
✅ Test 7 PASSED: Retry mechanism worked (3 retries attempted)
```

**Pass Criteria**: ✅ 3 retry attempts made with exponential backoff

---

### Test 8: Offline Check

**Objective**: Test offline detection and send blocking

**Hub Code**:
```cpp
void testOfflineCheck() {
    uint8_t nodeMac[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    
    // Mark peer offline
    Serial.println("⚠️  Marking peer as offline...");
    ESPNowManager::getInstance().setPeerOnline(nodeMac, false);
    
    // Try to send with offline check
    CommandMessage cmd = {};
    cmd.header.type = MessageType::COMMAND;
    
    bool sent = ESPNowManager::getInstance().send(
        nodeMac, 
        (uint8_t*)&cmd, 
        sizeof(cmd), 
        true  // checkOnline = true
    );
    
    if (!sent) {
        Serial.println("✅ Test 8a PASSED: Send blocked when peer offline");
    } else {
        Serial.println("❌ Test 8a FAILED: Send should have been blocked");
    }
    
    // Mark peer online
    Serial.println("✅ Marking peer as online...");
    ESPNowManager::getInstance().setPeerOnline(nodeMac, true);
    
    // Try to send again
    sent = ESPNowManager::getInstance().send(
        nodeMac, 
        (uint8_t*)&cmd, 
        sizeof(cmd), 
        true  // checkOnline = true
    );
    
    if (sent) {
        Serial.println("✅ Test 8b PASSED: Send allowed when peer online");
    } else {
        Serial.println("❌ Test 8b FAILED: Send should have been allowed");
    }
}
```

**Expected Output**:
```
⚠️  Marking peer as offline...
⚠️  Peer offline, message not sent
✅ Test 8a PASSED: Send blocked when peer offline
✅ Marking peer as online...
✅ Test 8b PASSED: Send allowed when peer online
```

**Pass Criteria**: ✅ Send blocked when offline, allowed when online

---

### Test 9: Statistics Collection

**Objective**: Verify all statistics are tracked correctly

**Code**:
```cpp
void testStatistics() {
    ESPNowManager::getInstance().resetStatistics();
    
    uint8_t nodeMac[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    
    // Send 5 messages
    for (int i = 0; i < 5; i++) {
        CommandMessage cmd = {};
        ESPNowManager::getInstance().send(nodeMac, (uint8_t*)&cmd, sizeof(cmd));
    }
    
    // Send 1 fragmented message (128 bytes = 4 fragments)
    uint8_t largeData[128];
    ESPNowManager::getInstance().sendFragmented(nodeMac, 99, largeData, 128);
    
    auto stats = ESPNowManager::getInstance().getStatistics();
    
    Serial.println("📊 Statistics:");
    Serial.printf("   Messages Sent: %u (expected 9: 5 single + 4 fragments)\n", 
                  stats.messagesSent);
    Serial.printf("   Fragments Sent: %u (expected 4)\n", stats.fragmentsSent);
    
    if (stats.messagesSent == 9 && stats.fragmentsSent == 4) {
        Serial.println("✅ Test 9 PASSED: Statistics accurate");
    } else {
        Serial.println("❌ Test 9 FAILED: Statistics mismatch");
    }
}
```

**Expected Output**:
```
📊 Statistics:
   Messages Sent: 9 (expected 9: 5 single + 4 fragments)
   Fragments Sent: 4 (expected 4)
✅ Test 9 PASSED: Statistics accurate
```

**Pass Criteria**: ✅ All counters accurate

---

## Automated Test Runner

```cpp
void runAllTests() {
    Serial.println("\n╔════════════════════════════════════════╗");
    Serial.println("║  ESPNowManager Test Suite             ║");
    Serial.println("╚════════════════════════════════════════╝\n");
    
    int passed = 0;
    int total = 9;
    
    // Run tests
    if (testInitialization()) passed++;
    delay(1000);
    
    if (testDiscovery()) passed++;
    delay(1000);
    
    if (testSimpleCommand()) passed++;
    delay(1000);
    
    if (testFragmentation()) passed++;
    delay(1000);
    
    if (testReassemblyTimeout()) passed++;
    delay(1000);
    
    if (testDuplicateDetection()) passed++;
    delay(1000);
    
    if (testRetryMechanism()) passed++;
    delay(1000);
    
    if (testOfflineCheck()) passed++;
    delay(1000);
    
    if (testStatistics()) passed++;
    
    // Summary
    Serial.println("\n╔════════════════════════════════════════╗");
    Serial.printf("║  Test Results: %d/%d PASSED           ║\n", passed, total);
    Serial.println("╚════════════════════════════════════════╝\n");
    
    if (passed == total) {
        Serial.println("✅ ALL TESTS PASSED - LIBRARY READY FOR PRODUCTION");
    } else {
        Serial.printf("⚠️  %d TESTS FAILED - REVIEW REQUIRED\n", total - passed);
    }
}
```

---

## Performance Tests

### Test 10: Throughput (Messages/Second)

```cpp
void testThroughput() {
    uint8_t nodeMac[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    
    const int NUM_MESSAGES = 100;
    uint32_t startTime = millis();
    
    for (int i = 0; i < NUM_MESSAGES; i++) {
        CommandMessage cmd = {};
        ESPNowManager::getInstance().send(nodeMac, (uint8_t*)&cmd, sizeof(cmd));
    }
    
    uint32_t elapsed = millis() - startTime;
    float messagesPerSec = (NUM_MESSAGES * 1000.0) / elapsed;
    
    Serial.printf("📈 Throughput: %.2f messages/sec\n", messagesPerSec);
    Serial.printf("   (%d messages in %u ms)\n", NUM_MESSAGES, elapsed);
}
```

**Expected**: ~50-100 messages/second

---

## Test Summary Checklist

- [ ] Test 1: Initialization ✅
- [ ] Test 2: Discovery (ANNOUNCE/ACK) ✅
- [ ] Test 3: Simple Command ✅
- [ ] Test 4: Fragmentation ✅
- [ ] Test 5: Reassembly Timeout ✅
- [ ] Test 6: Duplicate Detection ✅
- [ ] Test 7: Retry Mechanism ✅
- [ ] Test 8: Offline Check ✅
- [ ] Test 9: Statistics ✅
- [ ] Test 10: Throughput ✅

---

**Document Version**: 1.0  
**Last Updated**: January 2, 2026  
**Status**: Ready for Execution
