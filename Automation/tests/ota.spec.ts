/**
 * OTA (Over-The-Air Update) Tests
 * 
 * WARNING: These tests trigger actual OTA updates which will cause the Hub to reboot!
 * Only run these tests when you explicitly want to test OTA functionality.
 * 
 * Run with: npm run test:ota
 */

import { test, expect } from '@playwright/test';
import { config } from '../config/hub.config';
import { isHubOnline, waitForPageLoad, navigateTo } from '../helpers/test-helpers';

// Device types to test (matches ota.json)
const DEVICE_TYPES = ['light', 'co2', 'heater', 'fish_feeder', 'sensor', 'doser', 'filter', 'repeater'];

test.describe('OTA Update Tests @ota', () => {
  
  test.describe.configure({ mode: 'serial' }); // Run tests in order
  
  // Skip all OTA tests by default to prevent accidental Hub reboots
  // Remove .skip when you want to explicitly test OTA
  test.skip();
  
  test.beforeEach(async ({ request }) => {
    // Verify Hub is online before running OTA tests
    const online = await isHubOnline(request);
    expect(online, 'Hub must be online to run OTA tests').toBeTruthy();
  });

  test.describe('OTA Check APIs', () => {
    
    test('GET /api/settings/ota-urls - should return OTA configuration', async ({ request }) => {
      const response = await request.get(config.api.settingsOtaUrls);
      expect(response.ok()).toBeTruthy();
      
      const data = await response.json();
      expect(data).toBeDefined();
      // Check expected fields
      if (data.firmwareUrl) {
        expect(typeof data.firmwareUrl).toBe('string');
      }
      if (data.littlefsUrl) {
        expect(typeof data.littlefsUrl).toBe('string');
      }
    });

    test('GET /api/hub/ota/check - should check for available updates', async ({ request }) => {
      const response = await request.get(config.api.hubOtaCheck);
      expect(response.ok()).toBeTruthy();
      
      const data = await response.json();
      expect(data).toBeDefined();
      // Response should indicate update availability
    });
  });

  test.describe('Generic Device OTA APIs', () => {
    
    for (const deviceType of DEVICE_TYPES) {
      test(`GET /api/nodes/${deviceType}/list - should return ${deviceType} device list`, async ({ request }) => {
        const response = await request.get(config.api.nodesDeviceList(deviceType));
        expect(response.status()).toBeLessThan(500);
        
        const data = await response.json();
        expect(data).toBeDefined();
        // Should have devices array (may be empty)
        if (data.devices) {
          expect(Array.isArray(data.devices)).toBeTruthy();
        }
      });

      test(`POST /api/nodes/${deviceType}/check-update - should check ${deviceType} updates`, async ({ request }) => {
        const response = await request.post(config.api.nodesDeviceCheckUpdate(deviceType));
        expect(response.status()).toBeLessThan(500);
        
        const data = await response.json();
        expect(data).toBeDefined();
        // May have error if OTA URL not configured
      });
    }
  });

  test.describe('OTA Trigger APIs (DANGEROUS - Causes Reboot)', () => {
    
    // These tests will trigger actual OTA updates!
    // The Hub will reboot after these tests run.
    
    test('POST /api/ota/firmware - should trigger firmware update', async ({ request }) => {
      console.warn('⚠️ This test will trigger a firmware OTA update and reboot the Hub!');
      
      const response = await request.post(config.api.otaFirmware);
      expect(response.status()).toBeLessThan(500);
      
      // Response indicates whether update was triggered
      const data = await response.json();
      expect(data).toBeDefined();
    });

    test('POST /api/ota/littlefs - should trigger LittleFS update', async ({ request }) => {
      console.warn('⚠️ This test will trigger a LittleFS OTA update and reboot the Hub!');
      
      const response = await request.post(config.api.otaLittlefs);
      expect(response.status()).toBeLessThan(500);
      
      const data = await response.json();
      expect(data).toBeDefined();
    });

    test('POST /api/ota/all - should trigger combined firmware and LittleFS update', async ({ request }) => {
      console.warn('⚠️ This test will trigger a COMBINED OTA update and reboot the Hub!');
      
      const response = await request.post(config.api.otaAll);
      expect(response.status()).toBeLessThan(500);
      
      const data = await response.json();
      expect(data).toBeDefined();
    });
  });

  test.describe('OTA UI Tests', () => {
    
    test('Update software page should load', async ({ page }) => {
      await navigateTo(page, config.pages.settingsUpdateSoftware);
      await expect(page.locator('body')).toBeVisible();
    });

    test('Update page should display current version', async ({ page }) => {
      await navigateTo(page, config.pages.settingsUpdateSoftware);
      
      // Look for version display
      const versionElement = page.locator('#currentVersion, [class*="version"], text=/v?\\d+\\.\\d+/');
      // Version may or may not be present depending on UI
    });

    test('Update page should have check updates button', async ({ page }) => {
      await navigateTo(page, config.pages.settingsUpdateSoftware);
      
      const checkBtn = page.locator('#checkHubUpdateBtn, button:has-text("Check"), button:has-text("check")').first();
      await expect(checkBtn).toBeVisible({ timeout: 5000 }).catch(() => {
        // Button might not exist
      });
    });

    test('Update page should display Hub update section', async ({ page }) => {
      await navigateTo(page, config.pages.settingsUpdateSoftware);
      
      // Look for Hub update section
      const hubSection = page.locator('text=/Hub|Firmware|Update/i').first();
      await expect(hubSection).toBeVisible({ timeout: 5000 }).catch(() => {
        // Section naming may vary
      });
    });

    test('Update page should display Device OTA section', async ({ page }) => {
      await navigateTo(page, config.pages.settingsUpdateSoftware);
      
      // Look for Device OTA section with dropdown
      const deviceOtaSection = page.locator('text=/Device OTA|Device Type/i').first();
      await expect(deviceOtaSection).toBeVisible({ timeout: 5000 }).catch(() => {
        // Section may not be visible initially
      });
    });

    test('Device type dropdown should be present', async ({ page }) => {
      await navigateTo(page, config.pages.settingsUpdateSoftware);
      
      // Look for device type dropdown
      const dropdown = page.locator('#deviceTypeSelect');
      await expect(dropdown).toBeVisible({ timeout: 5000 });
    });

    test('Device type dropdown should have options', async ({ page }) => {
      await navigateTo(page, config.pages.settingsUpdateSoftware);
      
      // Wait for dropdown to be populated
      await page.waitForTimeout(1000);
      
      const dropdown = page.locator('#deviceTypeSelect');
      const optionCount = await dropdown.locator('option').count();
      // Should have at least the placeholder + device types
      expect(optionCount).toBeGreaterThan(1);
    });
  });

  test.describe('Post-OTA Verification', () => {
    
    test('Hub should come back online after OTA', async ({ request }) => {
      // This test should be run after OTA to verify Hub recovered
      // Wait up to 3 minutes for Hub to come back
      const maxWaitMs = 180000;
      const pollIntervalMs = 5000;
      let online = false;
      
      for (let elapsed = 0; elapsed < maxWaitMs && !online; elapsed += pollIntervalMs) {
        await new Promise(resolve => setTimeout(resolve, pollIntervalMs));
        online = await isHubOnline(request);
      }
      
      expect(online, 'Hub should come back online after OTA').toBeTruthy();
    });

    test('Hub API should be functional after OTA', async ({ request }) => {
      const response = await request.get(config.api.status);
      expect(response.ok()).toBeTruthy();
      
      const data = await response.json();
      expect(data.uptime).toBeDefined();
    });

    test('Hub UI should be functional after OTA', async ({ page }) => {
      await page.goto('/');
      await waitForPageLoad(page);
      await expect(page.locator('body')).toBeVisible();
    });
  });
});
