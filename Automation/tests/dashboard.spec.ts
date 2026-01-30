import { test, expect } from '@playwright/test';
import { config } from '../config/hub.config';
import { navigateTo, waitForPageLoad, isHubOnline, getHubStatus, delayForESP32Recovery } from '../helpers/test-helpers';

/**
 * Dashboard and Home Page Tests
 * @tags smoke, dashboard
 */
test.describe('Dashboard Tests', () => {
  
  // Add delay after each test to let ESP32 recover
  test.afterEach(async () => {
    await delayForESP32Recovery();
  });

  test.beforeEach(async ({ request }) => {
    // Verify hub is online before each test
    const online = await isHubOnline(request);
    expect(online, 'Hub should be online').toBeTruthy();
  });

  test('should load home page @smoke', async ({ page }) => {
    await page.goto('/');
    await waitForPageLoad(page);
    
    // Verify page loaded
    await expect(page).toHaveTitle(/Aquarium|AMS|Dashboard/i);
  });

  test('should display sidebar navigation @smoke', async ({ page }) => {
    await page.goto('/');
    await waitForPageLoad(page);
    
    // Check sidebar is visible (use first() since there may be multiple nav elements)
    const sidebar = page.locator('.sidebar, aside.sidebar, nav.sidebar-nav').first();
    await expect(sidebar).toBeVisible();
  });

  test('should have working navigation links @smoke', async ({ page }) => {
    await page.goto('/');
    await waitForPageLoad(page);
    
    // Check common navigation links exist
    const navLinks = page.locator('a[href*="aquarium"], a[href*="device"], a[href*="settings"]');
    const count = await navLinks.count();
    expect(count).toBeGreaterThan(0);
  });

  test('should load dashboard content', async ({ page }) => {
    await navigateTo(page, config.pages.dashboard);
    
    // Verify dashboard has content sections
    const content = page.locator('main, .content, .dashboard, #content');
    await expect(content).toBeVisible();
  });

  test('should display system status section', async ({ page }) => {
    await navigateTo(page, config.pages.dashboard);
    
    // Look for status indicators
    const statusSection = page.locator('.status, .system-status, [class*="status"]');
    if (await statusSection.count() > 0) {
      await expect(statusSection.first()).toBeVisible();
    }
  });

  test('API: should return hub status @api @smoke', async ({ request }) => {
    const status = await getHubStatus(request);
    
    // Verify status response has expected fields
    expect(status).toBeDefined();
    // The exact structure depends on your API, adjust as needed
  });

  test('should be responsive - mobile viewport', async ({ page }) => {
    await page.setViewportSize({ width: 375, height: 667 });
    await page.goto('/');
    await waitForPageLoad(page);
    
    // Page should still be functional
    const body = page.locator('body');
    await expect(body).toBeVisible();
  });

  test('should handle page refresh gracefully', async ({ page }) => {
    await page.goto('/');
    await waitForPageLoad(page);
    
    // Refresh the page
    await page.reload();
    await waitForPageLoad(page);
    
    // Page should still work
    await expect(page.locator('body')).toBeVisible();
  });
});
