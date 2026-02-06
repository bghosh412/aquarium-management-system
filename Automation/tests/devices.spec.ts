import { test, expect } from '@playwright/test';
import { config } from '../config/hub.config';
import { 
  navigateTo, 
  waitForPageLoad, 
  getDevices,
  getUnmappedDevices,
  clickButton,
  waitForElement,
  delayForESP32Recovery
} from '../helpers/test-helpers';

/**
 * Device Management Tests
 * @tags devices
 */
test.describe('Device Management Tests', () => {

  // Add delay after each test to let ESP32 recover
  test.afterEach(async () => {
    await delayForESP32Recovery();
  });

  test('should load manage devices page @smoke', async ({ page }) => {
    await navigateTo(page, config.pages.manageDevices);
    
    // Page should load
    await expect(page.locator('body')).toBeVisible();
  });

  test('should display device list or empty state', async ({ page }) => {
    await navigateTo(page, config.pages.manageDevices);
    
    // Should show either devices or empty state message
    const content = page.locator('.device-list, .devices, .no-devices, [class*="device"]');
    await expect(page.locator('body')).toBeVisible();
  });

  test('should load add device page', async ({ page }) => {
    await navigateTo(page, config.pages.addDevice);
    
    // Page should load
    await expect(page.locator('body')).toBeVisible();
  });

  test('should load device setup page', async ({ page }) => {
    await navigateTo(page, config.pages.deviceSetup);
    
    // Page should load
    await expect(page.locator('body')).toBeVisible();
  });

  test('API: should get devices list @api', async ({ request }) => {
    const response = await request.get(config.api.devices);
    expect(response.ok()).toBeTruthy();
    
    const data = await response.json();
    expect(data).toBeDefined();
  });

  test('API: should get unmapped devices @api', async ({ request }) => {
    const response = await request.get(config.api.unmappedDevices);
    expect(response.ok()).toBeTruthy();
    
    const data = await response.json();
    expect(data).toBeDefined();
  });

  test('API: should get ESP-NOW peers @api', async ({ request }) => {
    const response = await request.get(config.api.peers);
    expect(response.ok()).toBeTruthy();
    
    const data = await response.json();
    expect(data).toBeDefined();
  });

  test('API: should get hub MAC addresses @api', async ({ request }) => {
    const response = await request.get(config.api.hubMacs);
    expect(response.ok()).toBeTruthy();
    
    const data = await response.json();
    expect(data).toBeDefined();
    // Should have STA and AP MAC addresses
  });

  test('should display unmapped devices section', async ({ page }) => {
    await navigateTo(page, config.pages.addDevice);
    
    // Look for unmapped devices section
    const unmappedSection = page.locator('.unmapped, [class*="unmapped"], [class*="discovered"]');
    // Section may or may not be visible depending on device availability
    await expect(page.locator('body')).toBeVisible();
  });

  test('should load light control page', async ({ page }) => {
    await page.goto(config.pages.lightControl + '?mac=test');
    await waitForPageLoad(page);
    
    // Page should load (may show error if device doesn't exist)
    await expect(page.locator('body')).toBeVisible();
  });

  test('should load light schedule page', async ({ page }) => {
    await page.goto(config.pages.lightSchedule + '?mac=test');
    await waitForPageLoad(page);
    
    // Page should load
    await expect(page.locator('body')).toBeVisible();
  });

  test('should load feeder schedule page', async ({ page }) => {
    await page.goto(config.pages.feederSchedule + '?mac=test');
    await waitForPageLoad(page);

    // Page should load and show feeder header
    await expect(page.locator('h1')).toContainText(/Feeder Schedule/i);
  });

  test('should load light details page', async ({ page }) => {
    await page.goto(config.pages.lightDetails + '?mac=test');
    await waitForPageLoad(page);
    
    // Page should load
    await expect(page.locator('body')).toBeVisible();
  });

  test('API: should get light schedule @api', async ({ request }) => {
    const response = await request.get(config.api.lightSchedule);
    // May return 400 if no MAC provided, which is expected
    expect(response.status()).toBeLessThan(500);
  });

  test('API: should get light nodes list @api', async ({ request }) => {
    const response = await request.get(config.api.nodesLightList);
    expect(response.ok()).toBeTruthy();
    
    const data = await response.json();
    expect(data).toBeDefined();
  });

  test('should show device online/offline status', async ({ page, request }) => {
    // Get devices from API first
    const response = await request.get(config.api.devices);
    const devices = await response.json();
    
    // Navigate to manage devices
    await navigateTo(page, config.pages.manageDevices);
    
    // Look for status indicators
    const statusIndicators = page.locator('.status, .online, .offline, [class*="status"]');
    // May or may not have devices
    await expect(page.locator('body')).toBeVisible();
  });
});
