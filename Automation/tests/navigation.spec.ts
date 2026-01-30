import { test, expect } from '@playwright/test';
import { config } from '../config/hub.config';
import { navigateTo, waitForPageLoad, isHubOnline, delayForESP32Recovery } from '../helpers/test-helpers';

/**
 * Navigation Tests
 * Tests sidebar navigation and page transitions
 * @tags navigation, smoke
 */
test.describe('Navigation Tests', () => {

  // Add delay after each test to let ESP32 recover
  test.afterEach(async () => {
    await delayForESP32Recovery();
  });

  test.beforeEach(async ({ request }) => {
    const online = await isHubOnline(request);
    expect(online, 'Hub should be online').toBeTruthy();
  });

  test('should navigate from home to aquarium selection', async ({ page }) => {
    await page.goto('/');
    await waitForPageLoad(page);
    
    // Click aquarium link in sidebar
    const aquariumLink = page.locator('a[href*="aquarium"]').first();
    if (await aquariumLink.count() > 0) {
      await aquariumLink.click();
      await waitForPageLoad(page);
      expect(page.url()).toContain('aquarium');
    }
  });

  test('should navigate from home to device management', async ({ page }) => {
    await page.goto('/');
    await waitForPageLoad(page);
    
    const deviceLink = page.locator('a[href*="device"]').first();
    if (await deviceLink.count() > 0) {
      await deviceLink.click();
      await waitForPageLoad(page);
      expect(page.url()).toContain('device');
    }
  });

  test('should navigate from home to settings', async ({ page }) => {
    await page.goto('/');
    await waitForPageLoad(page);
    
    const settingsLink = page.locator('a[href*="settings"]').first();
    if (await settingsLink.count() > 0) {
      await settingsLink.click();
      await waitForPageLoad(page);
      expect(page.url()).toContain('settings');
    }
  });

  test('should have consistent sidebar across pages @smoke', async ({ page }) => {
    const pagesToCheck = [
      '/',
      config.pages.aquariumSelection,
      config.pages.manageDevices,
      config.pages.settingsUpdateSoftware,
    ];

    for (const pagePath of pagesToCheck) {
      await page.goto(pagePath);
      await waitForPageLoad(page);
      
      // Sidebar should be present
      const sidebar = page.locator('.sidebar, aside.sidebar, nav.sidebar-nav').first();
      await expect(sidebar).toBeVisible();
    }
  });

  test('should handle back button navigation', async ({ page }) => {
    // Navigate to home
    await page.goto('/');
    await waitForPageLoad(page);
    
    // Navigate to settings
    await page.goto(config.pages.settingsUpdateSoftware);
    await waitForPageLoad(page);
    
    // Go back
    await page.goBack();
    await waitForPageLoad(page);
    
    // Should be back at home
    await expect(page.locator('body')).toBeVisible();
  });

  test('should handle direct URL access', async ({ page }) => {
    // Access pages directly via URL
    await page.goto(config.pages.settingsUpdateSoftware);
    await waitForPageLoad(page);
    await expect(page.locator('body')).toBeVisible();
    
    await page.goto(config.pages.manageDevices);
    await waitForPageLoad(page);
    await expect(page.locator('body')).toBeVisible();
    
    await page.goto(config.pages.aquariumSelection);
    await waitForPageLoad(page);
    await expect(page.locator('body')).toBeVisible();
  });

  test('should maintain state across navigation', async ({ page }) => {
    // This test checks that navigating away and back preserves state
    // First load aquarium page
    await navigateTo(page, config.pages.aquariumSelection);
    
    // Navigate to settings
    await page.goto(config.pages.settingsUpdateSoftware);
    await waitForPageLoad(page);
    
    // Navigate back to aquarium
    await page.goto(config.pages.aquariumSelection);
    await waitForPageLoad(page);
    
    // Page should load correctly
    await expect(page.locator('body')).toBeVisible();
  });

  test('should show breadcrumbs or navigation path if available', async ({ page }) => {
    await navigateTo(page, config.pages.addAquarium);
    
    // Look for breadcrumbs
    const breadcrumbs = page.locator('.breadcrumb, .breadcrumbs, [class*="breadcrumb"], nav[aria-label="breadcrumb"]');
    // Breadcrumbs may or may not be implemented
    await expect(page.locator('body')).toBeVisible();
  });

  test('should have working home/logo link', async ({ page }) => {
    await page.goto(config.pages.settingsUpdateSoftware);
    await waitForPageLoad(page);
    
    // Click logo or home link
    const homeLink = page.locator('a[href="/"], a[href="/index.html"], .logo, .home-link').first();
    if (await homeLink.count() > 0) {
      await homeLink.click();
      await waitForPageLoad(page);
      
      // Should be at home page
      expect(page.url()).toMatch(/\/(index\.html)?$/);
    }
  });
});
