#!/usr/bin/env python3
"""
Hub OTA Mock Test Suite

Tests the Hub OTA API logic using a mock OTA server.
This allows testing the API contract without actually updating the hub.

TESTS COVERED:
==============
API Endpoints (TestHubOTAMock):
  1. /api/settings/ota-urls - GET/POST OTA URLs and versions
  2. /api/hub/ota/check - Check for firmware and LittleFS updates
  3. Version comparison logic
  4. Error handling for missing URLs

Version Comparison (TestVersionComparison):
  5. Semantic version parsing (major.minor.patch)
  6. Version comparison (<, >, ==)
  7. Edge cases (v prefix, single digit versions)

Usage:
    python test/integration/test_hub_ota_mock.py

For testing against real hub:
    python test/integration/test_hub_ota_mock.py --hub-ip 192.168.1.53
"""

import http.server
import json
import os
import re
import socketserver
import sys
import tempfile
import threading
import time
import unittest
from urllib.request import urlopen, Request
from urllib.error import URLError, HTTPError

# Test configuration
MOCK_HUB_PORT = 18081
MOCK_OTA_PORT = 18089

# Test versions
TEST_FIRMWARE_VERSION = "1.0.1"
TEST_LITTLEFS_VERSION = "1.0.2"


class MockOTAServerHandler(http.server.BaseHTTPRequestHandler):
    """Mock OTA server that serves version.txt and binary files"""
    
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
    
    hub_firmware_url = None
    hub_littlefs_url = None
    hub_firmware_version = "1.0.0"
    hub_littlefs_version = "1.0.0"
    light_node_ota_url = None
    
    def log_message(self, format, *args):
        pass
    
    def send_json(self, data, status=200):
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.end_headers()
        self.wfile.write(json.dumps(data).encode())
    
    @staticmethod
    def parse_version(version_str):
        """Parse semantic version into (major, minor, patch)"""
        v = version_str.strip()
        if v.startswith('v') or v.startswith('V'):
            v = v[1:]
        
        parts = v.split('.')
        major = int(parts[0]) if len(parts) > 0 else 0
        minor = int(parts[1]) if len(parts) > 1 else 0
        patch = int(parts[2]) if len(parts) > 2 else 0
        return (major, minor, patch)
    
    @staticmethod
    def compare_versions(v1, v2):
        """Compare two version strings. Returns >0 if v1>v2, <0 if v1<v2, 0 if equal"""
        p1 = MockHubHandler.parse_version(v1)
        p2 = MockHubHandler.parse_version(v2)
        
        if p1[0] != p2[0]:
            return p1[0] - p2[0]
        if p1[1] != p2[1]:
            return p1[1] - p2[1]
        return p1[2] - p2[2]
    
    @staticmethod
    def fetch_remote_version(base_url):
        """Fetch version.txt from remote URL"""
        url = base_url.rstrip('/') + '/version.txt'
        try:
            with urlopen(url, timeout=5) as resp:
                return resp.read().decode().strip()
        except Exception as e:
            return None
    
    def do_GET(self):
        if self.path == '/api/status':
            self.send_json({
                "status": "ok",
                "uptime": 12345,
                "freeHeap": 100000
            })
        
        elif self.path == '/api/settings/ota-urls':
            self.send_json({
                "hubFirmwareUrl": MockHubHandler.hub_firmware_url or "",
                "hubLittlefsUrl": MockHubHandler.hub_littlefs_url or "",
                "hubFirmwareVersion": MockHubHandler.hub_firmware_version,
                "hubLittlefsVersion": MockHubHandler.hub_littlefs_version,
                "lightNodeOtaUrl": MockHubHandler.light_node_ota_url or ""
            })
        
        elif self.path == '/api/hub/ota/check':
            response = {}
            
            # Check firmware
            if MockHubHandler.hub_firmware_url:
                remote_version = self.fetch_remote_version(MockHubHandler.hub_firmware_url)
                if remote_version:
                    has_update = self.compare_versions(remote_version, MockHubHandler.hub_firmware_version) > 0
                    response["firmware"] = {
                        "currentVersion": MockHubHandler.hub_firmware_version,
                        "availableVersion": remote_version,
                        "hasUpdate": has_update,
                        "url": MockHubHandler.hub_firmware_url
                    }
                else:
                    response["firmware"] = {"error": "Failed to fetch firmware version"}
            else:
                response["firmware"] = {"error": "HUB_FIRMWARE_OTA_URL not configured"}
            
            # Check LittleFS
            if MockHubHandler.hub_littlefs_url:
                remote_version = self.fetch_remote_version(MockHubHandler.hub_littlefs_url)
                if remote_version:
                    has_update = self.compare_versions(remote_version, MockHubHandler.hub_littlefs_version) > 0
                    response["littlefs"] = {
                        "currentVersion": MockHubHandler.hub_littlefs_version,
                        "availableVersion": remote_version,
                        "hasUpdate": has_update,
                        "url": MockHubHandler.hub_littlefs_url
                    }
                else:
                    response["littlefs"] = {"error": "Failed to fetch LittleFS version"}
            else:
                response["littlefs"] = {"error": "HUB_LITTLEFS_OTA_URL not configured"}
            
            self.send_json(response)
        
        else:
            self.send_response(404)
            self.end_headers()
    
    def do_POST(self):
        content_length = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(content_length) if content_length > 0 else b''
        
        if self.path == '/api/settings/ota-urls':
            try:
                data = json.loads(body) if body else {}
                
                if 'hubFirmwareUrl' in data:
                    MockHubHandler.hub_firmware_url = data['hubFirmwareUrl']
                if 'hubLittlefsUrl' in data:
                    MockHubHandler.hub_littlefs_url = data['hubLittlefsUrl']
                if 'lightNodeOtaUrl' in data:
                    MockHubHandler.light_node_ota_url = data['lightNodeOtaUrl']
                
                self.send_json({"success": True})
            except Exception as e:
                self.send_json({"success": False, "error": str(e)}, 400)
        
        elif self.path == '/api/ota/firmware':
            if not MockHubHandler.hub_firmware_url:
                self.send_json({"success": False, "error": "HUB_FIRMWARE_OTA_URL not configured"}, 400)
                return
            
            remote_version = self.fetch_remote_version(MockHubHandler.hub_firmware_url)
            if not remote_version:
                self.send_json({"success": False, "error": "Failed to fetch remote version"}, 500)
                return
            
            if self.compare_versions(remote_version, MockHubHandler.hub_firmware_version) <= 0:
                self.send_json({
                    "success": False,
                    "error": f"No update available. Current: {MockHubHandler.hub_firmware_version}, Remote: {remote_version}"
                })
                return
            
            # Simulate successful update
            MockHubHandler.hub_firmware_version = remote_version
            self.send_json({
                "success": True,
                "message": "Firmware updated (mock), rebooting..."
            })
        
        elif self.path == '/api/ota/littlefs':
            if not MockHubHandler.hub_littlefs_url:
                self.send_json({"success": False, "error": "HUB_LITTLEFS_OTA_URL not configured"}, 400)
                return
            
            remote_version = self.fetch_remote_version(MockHubHandler.hub_littlefs_url)
            if not remote_version:
                self.send_json({"success": False, "error": "Failed to fetch remote version"}, 500)
                return
            
            if self.compare_versions(remote_version, MockHubHandler.hub_littlefs_version) <= 0:
                self.send_json({
                    "success": False,
                    "error": f"No update available. Current: {MockHubHandler.hub_littlefs_version}, Remote: {remote_version}"
                })
                return
            
            # Simulate successful update
            MockHubHandler.hub_littlefs_version = remote_version
            self.send_json({
                "success": True,
                "message": "LittleFS updated (mock), rebooting..."
            })
        
        else:
            self.send_response(404)
            self.end_headers()


def create_mock_ota_files(directory):
    """Create mock OTA files for testing"""
    # Create firmware directory
    firmware_dir = os.path.join(directory, 'hub', 'firmware')
    os.makedirs(firmware_dir, exist_ok=True)
    
    with open(os.path.join(firmware_dir, 'version.txt'), 'w') as f:
        f.write(TEST_FIRMWARE_VERSION)
    
    # Create a minimal mock firmware binary
    with open(os.path.join(firmware_dir, 'firmware.bin'), 'wb') as f:
        f.write(bytes(1024))  # 1KB dummy file
    
    # Create LittleFS directory
    littlefs_dir = os.path.join(directory, 'hub', 'littlefs')
    os.makedirs(littlefs_dir, exist_ok=True)
    
    with open(os.path.join(littlefs_dir, 'version.txt'), 'w') as f:
        f.write(TEST_LITTLEFS_VERSION)
    
    # Create a minimal mock LittleFS binary
    with open(os.path.join(littlefs_dir, 'littlefs.bin'), 'wb') as f:
        f.write(bytes(2048))  # 2KB dummy file


class TestVersionComparison(unittest.TestCase):
    """Test version comparison logic"""
    
    def test_parse_version_full(self):
        """Test parsing full semantic version"""
        result = MockHubHandler.parse_version("1.2.3")
        self.assertEqual(result, (1, 2, 3))
    
    def test_parse_version_two_parts(self):
        """Test parsing two-part version"""
        result = MockHubHandler.parse_version("1.2")
        self.assertEqual(result, (1, 2, 0))
    
    def test_parse_version_single(self):
        """Test parsing single digit version"""
        result = MockHubHandler.parse_version("5")
        self.assertEqual(result, (5, 0, 0))
    
    def test_parse_version_with_v_prefix(self):
        """Test parsing version with 'v' prefix"""
        result = MockHubHandler.parse_version("v1.2.3")
        self.assertEqual(result, (1, 2, 3))
    
    def test_compare_greater(self):
        """Test version1 > version2"""
        self.assertGreater(MockHubHandler.compare_versions("1.0.1", "1.0.0"), 0)
        self.assertGreater(MockHubHandler.compare_versions("1.1.0", "1.0.9"), 0)
        self.assertGreater(MockHubHandler.compare_versions("2.0.0", "1.9.9"), 0)
    
    def test_compare_less(self):
        """Test version1 < version2"""
        self.assertLess(MockHubHandler.compare_versions("1.0.0", "1.0.1"), 0)
        self.assertLess(MockHubHandler.compare_versions("1.0.9", "1.1.0"), 0)
        self.assertLess(MockHubHandler.compare_versions("1.9.9", "2.0.0"), 0)
    
    def test_compare_equal(self):
        """Test version1 == version2"""
        self.assertEqual(MockHubHandler.compare_versions("1.0.0", "1.0.0"), 0)
        self.assertEqual(MockHubHandler.compare_versions("v1.0.0", "1.0.0"), 0)


class TestHubOTAMock(unittest.TestCase):
    """Test Hub OTA functionality using mock servers"""
    
    @classmethod
    def setUpClass(cls):
        """Start mock servers"""
        # Create temp directory
        cls.ota_files_dir = tempfile.mkdtemp(prefix='hub_ota_files_')
        
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
        
        # Reset mock hub state
        MockHubHandler.hub_firmware_url = f"http://localhost:{MOCK_OTA_PORT}/hub/firmware"
        MockHubHandler.hub_littlefs_url = f"http://localhost:{MOCK_OTA_PORT}/hub/littlefs"
        MockHubHandler.hub_firmware_version = "1.0.0"
        MockHubHandler.hub_littlefs_version = "1.0.0"
        MockHubHandler.light_node_ota_url = None
        
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
        
        # Cleanup temp directory
        import shutil
        shutil.rmtree(cls.ota_files_dir, ignore_errors=True)
    
    def test_01_hub_status(self):
        """Test /api/status endpoint"""
        with urlopen(f"{self.base_url}/api/status", timeout=5) as resp:
            data = json.loads(resp.read())
        
        self.assertEqual(data['status'], 'ok')
        print("  [PASS] Hub status endpoint works")
    
    def test_02_get_ota_urls(self):
        """Test GET /api/settings/ota-urls"""
        with urlopen(f"{self.base_url}/api/settings/ota-urls", timeout=5) as resp:
            data = json.loads(resp.read())
        
        self.assertIn('hubFirmwareUrl', data)
        self.assertIn('hubLittlefsUrl', data)
        self.assertIn('hubFirmwareVersion', data)
        self.assertIn('hubLittlefsVersion', data)
        print(f"  [PASS] OTA URLs: firmware={data['hubFirmwareUrl'][:40]}...")
    
    def test_03_check_updates_available(self):
        """Test /api/hub/ota/check detects available updates"""
        with urlopen(f"{self.base_url}/api/hub/ota/check", timeout=5) as resp:
            data = json.loads(resp.read())
        
        # Firmware check
        self.assertIn('firmware', data)
        self.assertNotIn('error', data['firmware'])
        self.assertEqual(data['firmware']['currentVersion'], '1.0.0')
        self.assertEqual(data['firmware']['availableVersion'], TEST_FIRMWARE_VERSION)
        self.assertTrue(data['firmware']['hasUpdate'])
        
        # LittleFS check
        self.assertIn('littlefs', data)
        self.assertNotIn('error', data['littlefs'])
        self.assertEqual(data['littlefs']['currentVersion'], '1.0.0')
        self.assertEqual(data['littlefs']['availableVersion'], TEST_LITTLEFS_VERSION)
        self.assertTrue(data['littlefs']['hasUpdate'])
        
        print(f"  [PASS] Update check: FW {data['firmware']['currentVersion']} -> {data['firmware']['availableVersion']}")
        print(f"         LittleFS {data['littlefs']['currentVersion']} -> {data['littlefs']['availableVersion']}")
    
    def test_04_apply_firmware_update(self):
        """Test POST /api/ota/firmware applies update"""
        req = Request(f"{self.base_url}/api/ota/firmware", method='POST')
        with urlopen(req, timeout=10) as resp:
            data = json.loads(resp.read())
        
        self.assertTrue(data['success'])
        self.assertIn('message', data)
        
        # Verify version was updated
        self.assertEqual(MockHubHandler.hub_firmware_version, TEST_FIRMWARE_VERSION)
        print(f"  [PASS] Firmware updated to {MockHubHandler.hub_firmware_version}")
    
    def test_05_apply_littlefs_update(self):
        """Test POST /api/ota/littlefs applies update"""
        req = Request(f"{self.base_url}/api/ota/littlefs", method='POST')
        with urlopen(req, timeout=10) as resp:
            data = json.loads(resp.read())
        
        self.assertTrue(data['success'])
        self.assertIn('message', data)
        
        # Verify version was updated
        self.assertEqual(MockHubHandler.hub_littlefs_version, TEST_LITTLEFS_VERSION)
        print(f"  [PASS] LittleFS updated to {MockHubHandler.hub_littlefs_version}")
    
    def test_06_no_update_when_current(self):
        """Test update is rejected when already current"""
        # Now both versions are at latest
        req = Request(f"{self.base_url}/api/ota/firmware", method='POST')
        with urlopen(req, timeout=10) as resp:
            data = json.loads(resp.read())
        
        self.assertFalse(data['success'])
        self.assertIn('No update available', data['error'])
        print(f"  [PASS] Correctly rejected update when current: {data['error']}")
    
    def test_07_check_no_update_available(self):
        """Test /api/hub/ota/check shows no update when current"""
        with urlopen(f"{self.base_url}/api/hub/ota/check", timeout=5) as resp:
            data = json.loads(resp.read())
        
        self.assertFalse(data['firmware']['hasUpdate'])
        self.assertFalse(data['littlefs']['hasUpdate'])
        print("  [PASS] Correctly shows no update available")
    
    def test_08_no_url_configured_error(self):
        """Test error when URL not configured"""
        # Clear URLs
        original_firmware_url = MockHubHandler.hub_firmware_url
        MockHubHandler.hub_firmware_url = None
        
        try:
            with urlopen(f"{self.base_url}/api/hub/ota/check", timeout=5) as resp:
                data = json.loads(resp.read())
            
            self.assertIn('error', data['firmware'])
            self.assertIn('not configured', data['firmware']['error'])
            print(f"  [PASS] Error when URL not configured: {data['firmware']['error']}")
        finally:
            MockHubHandler.hub_firmware_url = original_firmware_url
    
    def test_09_set_ota_urls(self):
        """Test POST /api/settings/ota-urls"""
        new_url = "http://example.com/ota"
        data = json.dumps({"hubFirmwareUrl": new_url}).encode()
        
        req = Request(
            f"{self.base_url}/api/settings/ota-urls",
            data=data,
            headers={'Content-Type': 'application/json'},
            method='POST'
        )
        
        with urlopen(req, timeout=5) as resp:
            result = json.loads(resp.read())
        
        self.assertTrue(result['success'])
        self.assertEqual(MockHubHandler.hub_firmware_url, new_url)
        print(f"  [PASS] OTA URL updated to: {new_url}")
        
        # Restore original URL
        MockHubHandler.hub_firmware_url = f"http://localhost:{MOCK_OTA_PORT}/hub/firmware"


class TestRealHub(unittest.TestCase):
    """Test against real hub hardware (optional)"""
    
    hub_ip = None
    
    @classmethod
    def setUpClass(cls):
        if cls.hub_ip is None:
            raise unittest.SkipTest("Real hub tests require --hub-ip argument")
    
    def test_01_hub_status(self):
        """Test real hub status"""
        url = f"http://{self.hub_ip}/api/status"
        with urlopen(url, timeout=10) as resp:
            data = json.loads(resp.read())
        
        self.assertEqual(data['status'], 'ok')
        print(f"  [PASS] Real hub status: uptime={data.get('uptime', 'N/A')}s")
    
    def test_02_get_ota_urls(self):
        """Test real hub OTA URLs"""
        url = f"http://{self.hub_ip}/api/settings/ota-urls"
        with urlopen(url, timeout=10) as resp:
            data = json.loads(resp.read())
        
        print(f"  [PASS] Hub firmware version: {data.get('hubFirmwareVersion', 'N/A')}")
        print(f"         Hub LittleFS version: {data.get('hubLittlefsVersion', 'N/A')}")
    
    def test_03_check_updates(self):
        """Test real hub update check"""
        url = f"http://{self.hub_ip}/api/hub/ota/check"
        with urlopen(url, timeout=15) as resp:
            data = json.loads(resp.read())
        
        if 'error' not in data.get('firmware', {}):
            print(f"  [PASS] Firmware: {data['firmware']['currentVersion']} -> {data['firmware']['availableVersion']}")
            print(f"         Has update: {data['firmware']['hasUpdate']}")
        else:
            print(f"  [INFO] Firmware: {data['firmware']['error']}")
        
        if 'error' not in data.get('littlefs', {}):
            print(f"  [PASS] LittleFS: {data['littlefs']['currentVersion']} -> {data['littlefs']['availableVersion']}")
            print(f"         Has update: {data['littlefs']['hasUpdate']}")
        else:
            print(f"  [INFO] LittleFS: {data['littlefs']['error']}")


def main():
    """Run tests"""
    # Parse command line arguments
    hub_ip = None
    for i, arg in enumerate(sys.argv):
        if arg == '--hub-ip' and i + 1 < len(sys.argv):
            hub_ip = sys.argv[i + 1]
            sys.argv.remove('--hub-ip')
            sys.argv.remove(hub_ip)
            break
    
    if hub_ip:
        TestRealHub.hub_ip = hub_ip
        print(f"\n{'='*60}")
        print(f"Testing against REAL hub at: {hub_ip}")
        print(f"{'='*60}\n")
    
    # Run tests
    unittest.main(verbosity=2)


if __name__ == "__main__":
    main()
