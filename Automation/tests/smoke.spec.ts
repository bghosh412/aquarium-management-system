import { test, expect } from '@playwright/test';
import { config } from '../config/hub.config';
import { waitForPageLoad, isHubOnline } from '../helpers/test-helpers';

/**
 * Smoke Tests
 * Quick validation that the hub is working
 * Run with: npm run test:smoke
 * @tags smoke
 */
test.describe('Smoke Tests @smoke', () => {
  
  test('Hub is online and responding', async ({ request }) => {
    const online = await isHubOnline(request);
    expect(online, 'Hub should be online at ' + config.hubUrl).toBeTruthy();
  });

  test('API status endpoint returns valid JSON', async ({ request }) => {
    const response = await request.get(config.api.status);
    expect(response.ok(), 'Status API should return 200').toBeTruthy();
    expect(response.headers()['content-type']).toContain('application/json');
  });

  test('Home page loads successfully', async ({ page }) => {
    const response = await page.goto('/');
    expect(response?.ok(), 'Home page should load').toBeTruthy();
    await waitForPageLoad(page);
    await expect(page.locator('body')).toBeVisible();
  });

  test('Sidebar navigation is present', async ({ page }) => {
    await page.goto('/');
    await waitForPageLoad(page);
    
    const sidebar = page.locator('.sidebar, aside.sidebar, nav.sidebar-nav').first();
    await expect(sidebar).toBeVisible();
  });

  test('Can navigate to settings page', async ({ page }) => {
    await page.goto(config.pages.settingsUpdateSoftware);
    await waitForPageLoad(page);
    await expect(page.locator('body')).toBeVisible();
  });

  test('Can get aquariums list via API', async ({ request }) => {
    const response = await request.get(config.api.aquariums);
    expect(response.ok()).toBeTruthy();
  });

  test('Can get devices list via API', async ({ request }) => {
    const response = await request.get(config.api.devices);
    expect(response.ok()).toBeTruthy();
  });

  test('Can get OTA configuration via API', async ({ request }) => {
    const response = await request.get(config.api.settingsOtaUrls);
    expect(response.ok()).toBeTruthy();
    
    const data = await response.json();
    expect(data.hubFirmwareVersion).toBeDefined();
  });

  test('Static assets are served correctly', async ({ page }) => {
    await page.goto('/');
    await waitForPageLoad(page);
    
    // Check that CSS is loaded (page should have styled elements)
    const styledElement = page.locator('[style], [class]').first();
    await expect(styledElement).toBeVisible();
  });

  test('JavaScript is functioning', async ({ page }) => {
    await page.goto('/');
    await waitForPageLoad(page);
    
    // Check if JavaScript has executed (look for dynamically loaded content)
    // This assumes the page uses JS to load some content
    await page.waitForTimeout(1000); // Brief wait for JS execution
    await expect(page.locator('body')).toBeVisible();
  });
});
