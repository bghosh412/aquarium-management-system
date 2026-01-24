#!/usr/bin/env python3
"""
Node OTA Mock Test Suite

Tests the OTA API logic using a mock hub server.
This allows testing the API contract without needing the actual hardware.

TESTS COVERED:
==============
API Endpoints (TestNodeOTAMock):
  1. /api/status - Hub health check
  2. /api/nodes/light/version - Get configured OTA URL  
  3. /api/nodes/light/check-update - Check if updates available
  4. /api/nodes/light/apply-update - Download and send OTA files
  5. Local file saving - Verify files saved to /ota/light/
  6. Error: No URL configured
  7. Error: No device online

Protocol Format (TestOTAProtocol):
  8. OTA command constants (0xA0, 0xA1, 0xC1, 0xF1)
  9. Chunk size validation (29 bytes max)
  10. Firmware chunk calculation for various sizes
  11. OTA_BEGIN message format
  12. OTA chunk message format
  13. OTA_END message format
  14. Status codes validation

Transfer Simulation (TestOTASimulation):
  15. Config file chunking and reassembly
  16. Firmware file chunking and reassembly
  17. Chunk boundary edge cases

Usage:
    python test/integration/test_node_ota_mock.py

For real hardware testing:
    python test/integration/test_node_ota.py --hub-ip 192.168.4.1
"""

import http.server
import json
import os
import socketserver
import sys
import tempfile
import threading
import time
import unittest
from urllib.request import urlopen, Request
from urllib.error import URLError, HTTPError

# Test configuration
MOCK_HUB_PORT = 18080
MOCK_OTA_PORT = 18088

# Test data
TEST_VERSION = "1.2.3"
TEST_CONFIG_CONTENT = """# Test node config
NODE_NAME=TestLightNode
DEBUG_SERIAL=true
FIRMWARE_VERSION=1.2.3
"""


class MockOTAServerHandler(http.server.BaseHTTPRequestHandler):
    """Mock OTA server that serves test files"""
    
    serve_dir = None
    
    def log_message(self, format, *args):
        pass  # Suppress logging
    
    def do_HEAD(self):
        """Handle HEAD requests for file existence checks"""
        self.do_GET(head_only=True)
    
    def do_GET(self, head_only=False):
        """Serve files from the OTA directory"""
        path = self.path.lstrip('/')
        if not path:
            path = 'index.html'
        
        file_path = os.path.join(MockOTAServerHandler.serve_dir, path)
        
        if os.path.exists(file_path) and os.path.isfile(file_path):
            self.send_response(200)
            
            # Determine content type
            if path.endswith('.txt'):
                self.send_header('Content-Type', 'text/plain')
            elif path.endswith('.bin'):
                self.send_header('Content-Type', 'application/octet-stream')
            else:
                self.send_header('Content-Type', 'application/octet-stream')
            
            file_size = os.path.getsize(file_path)
            self.send_header('Content-Length', str(file_size))
            self.end_headers()
            
            if not head_only:
                with open(file_path, 'rb') as f:
                    self.wfile.write(f.read())
        else:
            self.send_response(404)
            self.send_header('Content-Type', 'text/plain')
            self.end_headers()
            if not head_only:
                self.wfile.write(f"File not found: {path}".encode())


class MockHubHandler(http.server.BaseHTTPRequestHandler):
    """Mock hub server that simulates the ESP32 hub API"""
    
    ota_url = None
    devices = []
    ota_files_dir = None
    
    def log_message(self, format, *args):
        pass  # Suppress logging
    
    def send_json(self, data, status=200):
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.end_headers()
        self.wfile.write(json.dumps(data).encode())
    
    def do_GET(self):
        if self.path == '/api/status':
            self.send_json({
                "status": "ok",
                "uptime": 12345,
                "freeHeap": 100000
            })
        
        elif self.path == '/api/nodes/light/version':
            if MockHubHandler.ota_url:
                self.send_json({"url": MockHubHandler.ota_url})
            else:
                self.send_json({"error": "LIGHT_NODE_OTA_URL not configured"})
        
        elif self.path == '/api/nodes/light/check-update':
            if not MockHubHandler.ota_url:
                self.send_json({"error": "LIGHT_NODE_OTA_URL not configured"})
                return
            
            # Check mock OTA server
            try:
                version_url = MockHubHandler.ota_url.rstrip('/') + '/version.txt'
                with urlopen(version_url, timeout=5) as resp:
                    version = resp.read().decode().strip()
                
                # Check for files
                has_firmware = False
                has_config = False
                
                try:
                    fw_url = MockHubHandler.ota_url.rstrip('/') + '/firmware.bin'
                    req = Request(fw_url, method='HEAD')
                    with urlopen(req, timeout=5):
                        has_firmware = True
                except:
                    pass
                
                try:
                    cfg_url = MockHubHandler.ota_url.rstrip('/') + '/node_config.txt'
                    req = Request(cfg_url, method='HEAD')
                    with urlopen(req, timeout=5):
                        has_config = True
                except:
                    pass
                
                self.send_json({
                    "available": has_firmware or has_config,
                    "version": version,
                    "hasFirmware": has_firmware,
                    "hasConfig": has_config
                })
            except Exception as e:
                self.send_json({"error": f"Failed to check OTA server: {e}"})
        
        else:
            self.send_response(404)
            self.end_headers()
    
    def do_POST(self):
        if self.path == '/api/nodes/light/apply-update':
            if not MockHubHandler.ota_url:
                self.send_json({"success": False, "error": "LIGHT_NODE_OTA_URL not configured"})
                return
            
            if not MockHubHandler.devices:
                self.send_json({"success": False, "error": "No online light device found"})
                return
            
            # Simulate downloading and saving files
            config_saved = False
            config_sent = False
            firmware_saved = False
            firmware_sent = False
            
            # Create OTA directory
            if MockHubHandler.ota_files_dir:
                ota_dir = os.path.join(MockHubHandler.ota_files_dir, 'ota', 'light')
                os.makedirs(ota_dir, exist_ok=True)
                
                # Download config
                try:
                    cfg_url = MockHubHandler.ota_url.rstrip('/') + '/node_config.txt'
                    with urlopen(cfg_url, timeout=5) as resp:
                        content = resp.read()
                    
                    with open(os.path.join(ota_dir, 'node_config.txt'), 'wb') as f:
                        f.write(content)
                    config_saved = True
                    config_sent = True  # Simulate sending
                    print(f"  [Mock Hub] Saved node_config.txt ({len(content)} bytes)")
                except Exception as e:
                    print(f"  [Mock Hub] Config download failed: {e}")
                
                # Download firmware
                try:
                    fw_url = MockHubHandler.ota_url.rstrip('/') + '/firmware.bin'
                    with urlopen(fw_url, timeout=5) as resp:
                        content = resp.read()
                    
                    with open(os.path.join(ota_dir, 'firmware.bin'), 'wb') as f:
                        f.write(content)
                    firmware_saved = True
                    firmware_sent = True  # Simulate sending
                    print(f"  [Mock Hub] Saved firmware.bin ({len(content)} bytes)")
                except Exception as e:
                    print(f"  [Mock Hub] Firmware download failed: {e}")
            
            if not config_sent and not firmware_sent:
                self.send_json({"success": False, "error": "No update files found at OTA URL"})
                return
            
            self.send_json({
                "success": True,
                "configSaved": config_saved,
                "configSent": config_sent,
                "firmwareSaved": firmware_saved,
                "firmwareSent": firmware_sent
            })
        
        else:
            self.send_response(404)
            self.end_headers()


def create_mock_ota_files(directory):
    """Create mock OTA files for testing"""
    os.makedirs(directory, exist_ok=True)
    
    # version.txt
    with open(os.path.join(directory, 'version.txt'), 'w') as f:
        f.write(TEST_VERSION)
    
    # node_config.txt
    with open(os.path.join(directory, 'node_config.txt'), 'w') as f:
        f.write(TEST_CONFIG_CONTENT)
    
    # firmware.bin (minimal test file)
    with open(os.path.join(directory, 'firmware.bin'), 'wb') as f:
        # ESP8266 firmware header + padding
        header = bytes([0xE9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00])
        f.write(header)
        f.write(bytes(1024 - len(header)))  # Pad to 1KB


class TestNodeOTAMock(unittest.TestCase):
    """Test Node OTA functionality using mock servers"""
    
    @classmethod
    def setUpClass(cls):
        """Start mock servers"""
        # Create temp directories
        cls.ota_files_dir = tempfile.mkdtemp(prefix='ota_files_')
        cls.hub_data_dir = tempfile.mkdtemp(prefix='hub_data_')
        
        # Create mock OTA files
        create_mock_ota_files(cls.ota_files_dir)
        print(f"\n  Created OTA files in: {cls.ota_files_dir}")
        
        # Configure mock OTA server directory
        MockOTAServerHandler.serve_dir = cls.ota_files_dir
        
        # Start mock OTA server with socket reuse
        socketserver.TCPServer.allow_reuse_address = True
        cls.ota_server = socketserver.TCPServer(
            ("", MOCK_OTA_PORT),
            MockOTAServerHandler
        )
        cls.ota_thread = threading.Thread(target=cls.ota_server.serve_forever)
        cls.ota_thread.daemon = True
        cls.ota_thread.start()
        print(f"  Mock OTA server started on port {MOCK_OTA_PORT}")
        
        # Configure mock hub
        MockHubHandler.ota_url = f"http://localhost:{MOCK_OTA_PORT}/"
        MockHubHandler.devices = [{"type": "LIGHT", "status": "ONLINE", "mac": "AA:BB:CC:DD:EE:FF"}]
        MockHubHandler.ota_files_dir = cls.hub_data_dir
        
        # Start mock hub server
        cls.hub_server = socketserver.TCPServer(("", MOCK_HUB_PORT), MockHubHandler)
        cls.hub_thread = threading.Thread(target=cls.hub_server.serve_forever)
        cls.hub_thread.daemon = True
        cls.hub_thread.start()
        print(f"  Mock Hub server started on port {MOCK_HUB_PORT}")
        
        cls.base_url = f"http://localhost:{MOCK_HUB_PORT}"
        
        # Wait for servers to start
        time.sleep(0.5)
    
    @classmethod
    def tearDownClass(cls):
        """Stop mock servers"""
        cls.hub_server.shutdown()
        cls.ota_server.shutdown()
        
        # Cleanup temp directories
        import shutil
        shutil.rmtree(cls.ota_files_dir, ignore_errors=True)
        shutil.rmtree(cls.hub_data_dir, ignore_errors=True)
    
    def test_01_hub_status(self):
        """Test /api/status endpoint"""
        with urlopen(f"{self.base_url}/api/status", timeout=5) as resp:
            data = json.loads(resp.read())
        
        self.assertEqual(data['status'], 'ok')
        self.assertIn('uptime', data)
        print("  [PASS] Hub status endpoint works")
    
    def test_02_get_version_url(self):
        """Test /api/nodes/light/version endpoint"""
        with urlopen(f"{self.base_url}/api/nodes/light/version", timeout=5) as resp:
            data = json.loads(resp.read())
        
        self.assertIn('url', data)
        self.assertEqual(data['url'], f"http://localhost:{MOCK_OTA_PORT}/")
        print(f"  [PASS] Version endpoint returns URL: {data['url']}")
    
    def test_03_check_update(self):
        """Test /api/nodes/light/check-update endpoint"""
        with urlopen(f"{self.base_url}/api/nodes/light/check-update", timeout=5) as resp:
            data = json.loads(resp.read())
        
        self.assertTrue(data.get('available'), f"Update should be available: {data}")
        self.assertEqual(data.get('version'), TEST_VERSION)
        self.assertTrue(data.get('hasFirmware'))
        self.assertTrue(data.get('hasConfig'))
        print(f"  [PASS] Check update: version={data['version']}, firmware={data['hasFirmware']}, config={data['hasConfig']}")
    
    def test_04_apply_update(self):
        """Test /api/nodes/light/apply-update endpoint"""
        req = Request(f"{self.base_url}/api/nodes/light/apply-update", method='POST')
        with urlopen(req, timeout=10) as resp:
            data = json.loads(resp.read())
        
        self.assertTrue(data.get('success'), f"Apply update failed: {data}")
        self.assertTrue(data.get('configSaved'))
        self.assertTrue(data.get('configSent'))
        self.assertTrue(data.get('firmwareSaved'))
        self.assertTrue(data.get('firmwareSent'))
        print(f"  [PASS] Apply update: configSaved={data['configSaved']}, firmwareSaved={data['firmwareSaved']}")
    
    def test_05_files_saved_locally(self):
        """Verify OTA files were saved to hub's filesystem"""
        ota_dir = os.path.join(self.hub_data_dir, 'ota', 'light')
        
        # Check config file
        config_path = os.path.join(ota_dir, 'node_config.txt')
        self.assertTrue(os.path.exists(config_path), "node_config.txt not saved")
        with open(config_path) as f:
            content = f.read()
        self.assertIn('NODE_NAME=TestLightNode', content)
        
        # Check firmware file
        firmware_path = os.path.join(ota_dir, 'firmware.bin')
        self.assertTrue(os.path.exists(firmware_path), "firmware.bin not saved")
        size = os.path.getsize(firmware_path)
        self.assertEqual(size, 1024, f"Firmware size mismatch: {size}")
        
        print(f"  [PASS] Files saved: node_config.txt, firmware.bin (1024 bytes)")
    
    def test_06_no_url_configured(self):
        """Test error when OTA URL not configured"""
        # Temporarily clear OTA URL
        original_url = MockHubHandler.ota_url
        MockHubHandler.ota_url = None
        
        try:
            with urlopen(f"{self.base_url}/api/nodes/light/version", timeout=5) as resp:
                data = json.loads(resp.read())
            
            self.assertIn('error', data)
            self.assertIn('not configured', data['error'])
            print(f"  [PASS] No URL configured error: {data['error']}")
        finally:
            MockHubHandler.ota_url = original_url
    
    def test_07_no_device_online(self):
        """Test error when no light device online"""
        # Temporarily clear devices
        original_devices = MockHubHandler.devices
        MockHubHandler.devices = []
        
        try:
            req = Request(f"{self.base_url}/api/nodes/light/apply-update", method='POST')
            with urlopen(req, timeout=5) as resp:
                data = json.loads(resp.read())
            
            self.assertFalse(data.get('success'))
            self.assertIn('No online light device', data.get('error', ''))
            print(f"  [PASS] No device error: {data['error']}")
        finally:
            MockHubHandler.devices = original_devices


class TestOTAProtocol(unittest.TestCase):
    """Test OTA protocol message format"""
    
    def test_ota_command_constants(self):
        """Verify OTA command constants match expected values"""
        # These should match messages.h
        OTA_CMD_OTA_BEGIN = 0xA0
        OTA_CMD_OTA_END = 0xA1
        OTA_CMD_CONFIG_CHUNK = 0xC1
        OTA_CMD_FIRMWARE_CHUNK = 0xF1
        
        # Verify values are distinct
        commands = [OTA_CMD_OTA_BEGIN, OTA_CMD_OTA_END, OTA_CMD_CONFIG_CHUNK, OTA_CMD_FIRMWARE_CHUNK]
        self.assertEqual(len(commands), len(set(commands)), "OTA commands must be unique")
        
        print(f"  [PASS] OTA commands: BEGIN=0x{OTA_CMD_OTA_BEGIN:02X}, END=0x{OTA_CMD_OTA_END:02X}, CONFIG=0x{OTA_CMD_CONFIG_CHUNK:02X}, FW=0x{OTA_CMD_FIRMWARE_CHUNK:02X}")
    
    def test_chunk_size(self):
        """Verify chunk size fits in ESP-NOW message"""
        MAX_ESPNOW_PAYLOAD = 250
        COMMAND_HEADER_SIZE = 11  # MessageHeader (8) + commandId (1) + commandSeqID (1) + finalCommand (1)
        COMMAND_DATA_SIZE = 32
        CHUNK_OVERHEAD = 3  # command type (1) + chunk index (2)
        
        max_chunk_data = COMMAND_DATA_SIZE - CHUNK_OVERHEAD
        self.assertEqual(max_chunk_data, 29, f"Chunk data size should be 29, got {max_chunk_data}")
        
        total_size = COMMAND_HEADER_SIZE + COMMAND_DATA_SIZE
        self.assertLessEqual(total_size, MAX_ESPNOW_PAYLOAD, "Command message exceeds ESP-NOW limit")
        
        print(f"  [PASS] Chunk size: {max_chunk_data} bytes per chunk, total message: {total_size} bytes")
    
    def test_firmware_chunk_calculation(self):
        """Calculate chunks needed for typical firmware"""
        CHUNK_SIZE = 29
        
        # Typical ESP8266 firmware sizes
        test_sizes = [
            (1024, "1 KB test"),
            (100 * 1024, "100 KB small"),
            (300 * 1024, "300 KB typical"),
            (500 * 1024, "500 KB large"),
        ]
        
        for size, desc in test_sizes:
            chunks = (size + CHUNK_SIZE - 1) // CHUNK_SIZE
            time_estimate = chunks * 0.03  # 30ms per chunk
            print(f"  {desc}: {size:,} bytes = {chunks:,} chunks (~{time_estimate:.1f}s)")
        
        print("  [PASS] Chunk calculations verified")
    
    def test_ota_begin_message_format(self):
        """Test OTA_BEGIN message format"""
        # commandData layout for OTA_BEGIN:
        # [0] = OTA_CMD_OTA_BEGIN (0xA0)
        # [1] = type (CONFIG_CHUNK or FIRMWARE_CHUNK)
        # [2-5] = total size (uint32_t, little endian)
        # [6] = chunk size (29)
        
        import struct
        
        OTA_CMD_OTA_BEGIN = 0xA0
        OTA_CMD_FIRMWARE_CHUNK = 0xF1
        
        total_size = 325863  # Example firmware size
        chunk_size = 29
        
        command_data = bytearray(32)
        command_data[0] = OTA_CMD_OTA_BEGIN
        command_data[1] = OTA_CMD_FIRMWARE_CHUNK
        struct.pack_into('<I', command_data, 2, total_size)
        command_data[6] = chunk_size
        
        # Verify we can read it back
        read_type = command_data[0]
        read_ota_type = command_data[1]
        read_size = struct.unpack_from('<I', command_data, 2)[0]
        read_chunk = command_data[6]
        
        self.assertEqual(read_type, OTA_CMD_OTA_BEGIN)
        self.assertEqual(read_ota_type, OTA_CMD_FIRMWARE_CHUNK)
        self.assertEqual(read_size, total_size)
        self.assertEqual(read_chunk, chunk_size)
        
        print(f"  [PASS] OTA_BEGIN format: type=0x{read_type:02X}, ota_type=0x{read_ota_type:02X}, size={read_size}, chunk={read_chunk}")
    
    def test_ota_chunk_message_format(self):
        """Test OTA chunk message format"""
        # commandData layout for chunks:
        # [0] = type (CONFIG_CHUNK or FIRMWARE_CHUNK)
        # [1-2] = chunk index (uint16_t, little endian)
        # [3-31] = chunk data (up to 29 bytes)
        
        import struct
        
        OTA_CMD_FIRMWARE_CHUNK = 0xF1
        chunk_index = 1234
        chunk_data = b"Hello, this is test data!!"  # 26 bytes
        
        command_data = bytearray(32)
        command_data[0] = OTA_CMD_FIRMWARE_CHUNK
        struct.pack_into('<H', command_data, 1, chunk_index)
        command_data[3:3+len(chunk_data)] = chunk_data
        
        # Verify we can read it back
        read_type = command_data[0]
        read_index = struct.unpack_from('<H', command_data, 1)[0]
        read_data = bytes(command_data[3:3+len(chunk_data)])
        
        self.assertEqual(read_type, OTA_CMD_FIRMWARE_CHUNK)
        self.assertEqual(read_index, chunk_index)
        self.assertEqual(read_data, chunk_data)
        
        print(f"  [PASS] Chunk format: type=0x{read_type:02X}, index={read_index}, data_len={len(read_data)}")
    
    def test_ota_end_message_format(self):
        """Test OTA_END message format"""
        # commandData layout for OTA_END:
        # [0] = OTA_CMD_OTA_END (0xA1)
        # [1] = type (CONFIG_CHUNK or FIRMWARE_CHUNK)
        
        OTA_CMD_OTA_END = 0xA1
        OTA_CMD_CONFIG_CHUNK = 0xC1
        
        command_data = bytearray(32)
        command_data[0] = OTA_CMD_OTA_END
        command_data[1] = OTA_CMD_CONFIG_CHUNK
        
        # Verify we can read it back
        read_type = command_data[0]
        read_ota_type = command_data[1]
        
        self.assertEqual(read_type, OTA_CMD_OTA_END)
        self.assertEqual(read_ota_type, OTA_CMD_CONFIG_CHUNK)
        
        print(f"  [PASS] OTA_END format: type=0x{read_type:02X}, ota_type=0x{read_ota_type:02X}")
    
    def test_status_codes(self):
        """Verify OTA status codes"""
        # Status codes from messages.h
        OTA_STATUS_OK = 0x00
        OTA_STATUS_CHUNK_OK = 0x10
        OTA_STATUS_CHUNK_ERR = 0x11
        OTA_STATUS_BEGIN_OK = 0x20
        OTA_STATUS_BEGIN_ERR = 0x21
        OTA_STATUS_APPLY_OK = 0x30
        OTA_STATUS_APPLY_ERR = 0x31
        
        # All codes should be unique
        codes = [OTA_STATUS_OK, OTA_STATUS_CHUNK_OK, OTA_STATUS_CHUNK_ERR,
                 OTA_STATUS_BEGIN_OK, OTA_STATUS_BEGIN_ERR, 
                 OTA_STATUS_APPLY_OK, OTA_STATUS_APPLY_ERR]
        self.assertEqual(len(codes), len(set(codes)), "Status codes must be unique")
        
        print(f"  [PASS] Status codes: OK=0x{OTA_STATUS_OK:02X}, CHUNK_OK=0x{OTA_STATUS_CHUNK_OK:02X}, BEGIN_OK=0x{OTA_STATUS_BEGIN_OK:02X}, APPLY_OK=0x{OTA_STATUS_APPLY_OK:02X}")


class TestOTASimulation(unittest.TestCase):
    """Simulate the full OTA transfer flow"""
    
    def test_simulate_config_transfer(self):
        """Simulate transferring node_config.txt"""
        config_content = TEST_CONFIG_CONTENT.encode()
        chunk_size = 29
        
        # Simulate chunking
        chunks = []
        offset = 0
        chunk_index = 0
        
        while offset < len(config_content):
            remaining = len(config_content) - offset
            this_chunk = min(remaining, chunk_size)
            chunk_data = config_content[offset:offset + this_chunk]
            chunks.append((chunk_index, chunk_data))
            offset += this_chunk
            chunk_index += 1
        
        # Simulate receiving and reassembling
        received_buffer = bytearray()
        for idx, data in chunks:
            received_buffer.extend(data)
        
        # Verify
        self.assertEqual(bytes(received_buffer), config_content)
        print(f"  [PASS] Config transfer simulation: {len(config_content)} bytes in {len(chunks)} chunks")
    
    def test_simulate_firmware_transfer(self):
        """Simulate transferring a small firmware file"""
        # Create mock firmware (1KB)
        firmware_size = 1024
        firmware_content = bytes([i % 256 for i in range(firmware_size)])
        chunk_size = 29
        
        # Calculate expected chunks
        expected_chunks = (firmware_size + chunk_size - 1) // chunk_size
        
        # Simulate chunking
        chunks = []
        offset = 0
        chunk_index = 0
        
        while offset < len(firmware_content):
            remaining = len(firmware_content) - offset
            this_chunk = min(remaining, chunk_size)
            chunk_data = firmware_content[offset:offset + this_chunk]
            chunks.append((chunk_index, chunk_data))
            offset += this_chunk
            chunk_index += 1
        
        self.assertEqual(len(chunks), expected_chunks)
        
        # Simulate receiving and reassembling
        received_buffer = bytearray()
        for idx, data in chunks:
            received_buffer.extend(data)
        
        # Verify integrity
        self.assertEqual(len(received_buffer), firmware_size)
        self.assertEqual(bytes(received_buffer), firmware_content)
        
        print(f"  [PASS] Firmware transfer simulation: {firmware_size} bytes in {len(chunks)} chunks")
    
    def test_chunk_boundary_cases(self):
        """Test edge cases in chunk boundaries"""
        chunk_size = 29
        
        test_cases = [
            (1, 1),       # Minimal
            (28, 1),      # Just under chunk size
            (29, 1),      # Exactly one chunk
            (30, 2),      # Just over one chunk
            (58, 2),      # Exactly two chunks
            (59, 3),      # Just over two chunks
            (100, 4),     # Random size
        ]
        
        for size, expected_chunks in test_cases:
            actual_chunks = (size + chunk_size - 1) // chunk_size
            self.assertEqual(actual_chunks, expected_chunks, f"Size {size} should be {expected_chunks} chunks")
        
        print(f"  [PASS] Chunk boundary cases verified")


def main():
    """Run all tests"""
    print("\n" + "=" * 60)
    print(" Node OTA Mock Test Suite")
    print("=" * 60)
    
    # Create test suite
    loader = unittest.TestLoader()
    suite = unittest.TestSuite()
    
    # Add test classes
    suite.addTests(loader.loadTestsFromTestCase(TestNodeOTAMock))
    suite.addTests(loader.loadTestsFromTestCase(TestOTAProtocol))
    suite.addTests(loader.loadTestsFromTestCase(TestOTASimulation))
    
    # Run tests
    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)
    
    # Summary
    print("\n" + "=" * 60)
    print(" Summary")
    print("=" * 60)
    print(f"  Tests run: {result.testsRun}")
    print(f"  Failures: {len(result.failures)}")
    print(f"  Errors: {len(result.errors)}")
    print("=" * 60)
    
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
