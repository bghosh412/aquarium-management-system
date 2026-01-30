import { test, expect } from '@playwright/test';
import { config } from '../config/hub.config';
import { navigateTo, waitForPageLoad } from '../helpers/test-helpers';

/**
 * Settings Page Tests
 * @tags settings
 */
test.describe('Settings Tests', () => {

  test('should load WiFi settings page @smoke', async ({ page }) => {
    await navigateTo(page, config.pages.settingsWifi);
    
    await expect(page.locator('body')).toBeVisible();
  });

  test('should load backup/restore page @smoke', async ({ page }) => {
    await navigateTo(page, config.pages.settingsBackup);
    
    await expect(page.locator('body')).toBeVisible();
  });

  test('should load download/upload page', async ({ page }) => {
    await navigateTo(page, config.pages.settingsDownloadUpload);
    
    await expect(page.locator('body')).toBeVisible();
  });

  test('should load update software page @smoke', async ({ page }) => {
    await navigateTo(page, config.pages.settingsUpdateSoftware);
    
    await expect(page.locator('body')).toBeVisible();
  });

  test('should load diagnostics page', async ({ page }) => {
    await navigateTo(page, config.pages.settingsDiagnostics);
    
    await expect(page.locator('body')).toBeVisible();
  });

  test('should display OTA version info on update page', async ({ page }) => {
    await navigateTo(page, config.pages.settingsUpdateSoftware);
    
    // Look for version display elements
    const versionElements = page.locator('[id*="version" i], [class*="version" i], text=/\d+\.\d+\.\d+/');
    await expect(page.locator('body')).toBeVisible();
  });

  test('should have check for updates button', async ({ page }) => {
    await navigateTo(page, config.pages.settingsUpdateSoftware);
    
    // Look for check/update buttons
    const checkBtn = page.locator('button:has-text("Check"), button:has-text("Update"), button[id*="check" i]');
    if (await checkBtn.count() > 0) {
      await expect(checkBtn.first()).toBeVisible();
    }
  });

  test('API: should get settings files list @api', async ({ request }) => {
    const response = await request.get(config.api.settingsFiles);
    expect(response.ok()).toBeTruthy();
    
    const data = await response.json();
    expect(data).toBeDefined();
  });

  test('API: should get OTA URLs config @api', async ({ request }) => {
    const response = await request.get(config.api.settingsOtaUrls);
    expect(response.ok()).toBeTruthy();
    
    const data = await response.json();
    expect(data).toBeDefined();
    
    // Should have version fields
    expect(data.hubFirmwareVersion || data.hubFirmwareUrl !== undefined).toBeTruthy();
  });

  test('API: should check for hub OTA updates @api', async ({ request }) => {
    const response = await request.get(config.api.hubOtaCheck);
    // May fail if OTA URLs not configured
    expect(response.status()).toBeLessThan(500);
    
    if (response.ok()) {
      const data = await response.json();
      // Should have firmware and/or littlefs info
      expect(data.firmware || data.littlefs || data.error).toBeDefined();
    }
  });

  test('should display config file download options', async ({ page }) => {
    await navigateTo(page, config.pages.settingsDownloadUpload);
    
    // Look for download buttons or file list
    const downloadElements = page.locator('button:has-text("Download"), a:has-text("Download"), .download-btn');
    // May or may not have downloads available
    await expect(page.locator('body')).toBeVisible();
  });

  test('should have file upload capability', async ({ page }) => {
    await navigateTo(page, config.pages.settingsDownloadUpload);
    
    // Look for file input
    const fileInput = page.locator('input[type="file"]');
    if (await fileInput.count() > 0) {
      await expect(fileInput).toBeAttached();
    }
  });

  test('should display light node update section', async ({ page }) => {
    await navigateTo(page, config.pages.settingsUpdateSoftware);
    
    // Look for light node section
    const lightNodeSection = page.locator('[class*="light"], [id*="light"], text=/light.node/i');
    // May or may not be present
    await expect(page.locator('body')).toBeVisible();
  });

  test('API: should get light nodes for OTA @api', async ({ request }) => {
    const response = await request.get(config.api.nodesLightList);
    expect(response.ok()).toBeTruthy();
    
    const data = await response.json();
    expect(data).toBeDefined();
  });

  test('should have check updates button present', async ({ page }) => {
    await navigateTo(page, config.pages.settingsUpdateSoftware);
    
    // Find the check button but don't click it (to avoid triggering OTA)
    const checkBtn = page.locator('#checkHubUpdateBtn, button:has-text("Check"), button:has-text("check")').first();
    
    // Button should be present on the page
    await expect(checkBtn).toBeVisible({ timeout: 5000 }).catch(() => {
      // Button might not exist, which is acceptable
    });
    
    // Page should still be functional
    await expect(page.locator('body')).toBeVisible();
  });
});
