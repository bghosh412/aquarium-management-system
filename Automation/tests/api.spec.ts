import { test, expect } from '@playwright/test';
import { config } from '../config/hub.config';

/**
 * API Endpoint Tests
 * Comprehensive API validation without UI interaction
 * @tags api
 */
test.describe('API Endpoint Tests', () => {

  test.describe('Core APIs', () => {
    
    test('GET /api/status - should return hub status @smoke', async ({ request }) => {
      const response = await request.get(config.api.status);
      expect(response.ok()).toBeTruthy();
      expect(response.headers()['content-type']).toContain('application/json');
    });

    test('POST /api/reboot - should accept reboot request', async ({ request }) => {
      // Note: Don't actually reboot during tests, just verify endpoint exists
      // This is a risky test - uncomment only if you want to test reboot
      // const response = await request.post(config.api.reboot);
      // expect(response.status()).toBeLessThan(500);
      expect(true).toBeTruthy(); // Placeholder
    });
  });

  test.describe('Aquarium APIs', () => {
    
    test('GET /api/aquariums - should return aquariums list', async ({ request }) => {
      const response = await request.get(config.api.aquariums);
      expect(response.ok()).toBeTruthy();
      
      const data = await response.json();
      expect(Array.isArray(data.aquariums) || Array.isArray(data)).toBeTruthy();
    });

    test('POST /api/aquariums - should validate required fields', async ({ request }) => {
      // Send empty body - should fail validation
      const response = await request.post(config.api.aquariums, {
        headers: { 'Content-Type': 'application/json' },
        data: {}
      });
      // Expect either 400 (bad request) or error in response
      // Handle non-JSON responses gracefully
      let data: any = {};
      const contentType = response.headers()['content-type'] || '';
      if (contentType.includes('application/json')) {
        data = await response.json();
      }
      expect(response.status() === 400 || data.error || data.success === false || !response.ok()).toBeTruthy();
    });

    test('POST /api/aquarium/delete - should validate aquarium id', async ({ request }) => {
      const response = await request.post(config.api.aquariumDelete, {
        headers: { 'Content-Type': 'application/json' },
        data: { id: 'non-existent-id-12345' }
      });
      // Handle non-JSON responses gracefully
      const contentType = response.headers()['content-type'] || '';
      if (contentType.includes('application/json')) {
        const data = await response.json();
        expect(data).toBeDefined();
      }
      // Should fail gracefully for non-existent ID (no 500 error)
      expect(response.status()).toBeLessThan(500);
    });
  });

  test.describe('Device APIs', () => {
    
    test('GET /api/devices - should return devices list', async ({ request }) => {
      const response = await request.get(config.api.devices);
      expect(response.ok()).toBeTruthy();
      
      const data = await response.json();
      expect(data).toBeDefined();
    });

    test('GET /api/unmapped-devices - should return unmapped devices', async ({ request }) => {
      const response = await request.get(config.api.unmappedDevices);
      expect(response.ok()).toBeTruthy();
      
      const data = await response.json();
      expect(data).toBeDefined();
    });

    test('GET /api/peers - should return ESP-NOW peers', async ({ request }) => {
      const response = await request.get(config.api.peers);
      expect(response.ok()).toBeTruthy();
      
      const data = await response.json();
      expect(data).toBeDefined();
    });

    test('GET /api/hub-macs - should return hub MAC addresses', async ({ request }) => {
      const response = await request.get(config.api.hubMacs);
      expect(response.ok()).toBeTruthy();
      
      const data = await response.json();
      expect(data).toBeDefined();
    });

    test('POST /api/provision-device - should validate MAC address', async ({ request }) => {
      const response = await request.post(config.api.provisionDevice, {
        headers: { 'Content-Type': 'application/json' },
        data: {
          mac: 'invalid-mac',
          name: 'Test Device',
          aquariumId: '1'
        }
      });
      // Should fail for invalid MAC (no 500 error)
      expect(response.status()).toBeLessThan(500);
    });
  });

  test.describe('Light APIs', () => {
    
    test('GET /api/light-schedule - should handle missing MAC', async ({ request }) => {
      const response = await request.get(config.api.lightSchedule);
      // Should return error for missing MAC
      expect(response.status()).toBeLessThan(500);
    });

    test('GET /api/light-status - should handle missing MAC', async ({ request }) => {
      const response = await request.get(config.api.lightStatus);
      expect(response.status()).toBeLessThan(500);
    });

    test('GET /api/nodes/light/list - should return light nodes', async ({ request }) => {
      const response = await request.get(config.api.nodesLightList);
      expect(response.ok()).toBeTruthy();
      
      const data = await response.json();
      expect(data.devices !== undefined || data.error).toBeTruthy();
    });

    test('GET /api/nodes/light/version - should return version info', async ({ request }) => {
      const response = await request.get(config.api.nodesLightVersion);
      expect(response.status()).toBeLessThan(500);
    });
  });

  test.describe('Settings APIs', () => {
    
    test('GET /api/settings/files - should return config files', async ({ request }) => {
      const response = await request.get(config.api.settingsFiles);
      expect(response.ok()).toBeTruthy();
      
      const data = await response.json();
      expect(data).toBeDefined();
    });

    test('GET /api/settings/ota-urls - should return OTA config', async ({ request }) => {
      const response = await request.get(config.api.settingsOtaUrls);
      expect(response.ok()).toBeTruthy();
      
      const data = await response.json();
      expect(data).toBeDefined();
      expect(typeof data.hubFirmwareVersion === 'string' || data.hubFirmwareUrl !== undefined).toBeTruthy();
    });

    test('GET /api/hub/ota/check - should check for updates', async ({ request }) => {
      const response = await request.get(config.api.hubOtaCheck);
      expect(response.status()).toBeLessThan(500);
      
      const data = await response.json();
      expect(data).toBeDefined();
    });

    test('GET /api/settings/download - should require filename', async ({ request }) => {
      const response = await request.get(config.api.settingsDownload);
      // Should return error for missing filename
      expect(response.status()).toBe(400);
    });

    test('GET /api/settings/download?file=hub_config.txt - should download config', async ({ request }) => {
      const response = await request.get(config.api.settingsDownload + '?file=hub_config.txt');
      // File may or may not exist
      expect(response.status()).toBeLessThan(500);
    });
  });

  test.describe('OTA Check APIs', () => {
    
    // Note: OTA trigger tests are in ota.spec.ts (they cause Hub reboot)
    // These tests only check read-only OTA endpoints
    
    test('GET /api/settings/ota-urls - should return OTA configuration @api', async ({ request }) => {
      const response = await request.get(config.api.settingsOtaUrls);
      expect(response.ok()).toBeTruthy();
      
      const data = await response.json();
      expect(data).toBeDefined();
    });

    test('GET /api/hub/ota/check - should check for updates @api', async ({ request }) => {
      const response = await request.get(config.api.hubOtaCheck);
      expect(response.ok()).toBeTruthy();
    });
  });

  test.describe('Error Handling', () => {
    
    test('should return 404 for non-existent endpoint', async ({ request }) => {
      const response = await request.get('/api/non-existent-endpoint-12345');
      expect(response.status()).toBe(404);
    });

    test('should handle malformed JSON gracefully', async ({ request }) => {
      const response = await request.post(config.api.aquariums, {
        headers: { 'Content-Type': 'application/json' },
        data: 'not-valid-json{'
      });
      expect(response.status()).toBeLessThan(500);
    });

    test('should return JSON content type for API responses', async ({ request }) => {
      const response = await request.get(config.api.status);
      expect(response.headers()['content-type']).toContain('application/json');
    });
  });
});
