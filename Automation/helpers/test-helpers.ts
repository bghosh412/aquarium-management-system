import { test, expect, Page, APIRequestContext } from '@playwright/test';
import { config } from '../config/hub.config';

/**
 * Helper functions for AMS Hub UI testing
 */

/**
 * Wait for page to fully load (including AJAX calls)
 */
export async function waitForPageLoad(page: Page, timeout = config.timeouts.medium): Promise<void> {
  await page.waitForLoadState('networkidle', { timeout });
}

/**
 * Navigate to a page and wait for load
 */
export async function navigateTo(page: Page, path: string): Promise<void> {
  await page.goto(path);
  await waitForPageLoad(page);
}

/**
 * Click sidebar navigation link
 */
export async function clickSidebarLink(page: Page, linkText: string): Promise<void> {
  const sidebar = page.locator('#sidebar, .sidebar, nav');
  await sidebar.getByRole('link', { name: linkText }).click();
  await waitForPageLoad(page);
}

/**
 * Wait for toast/notification message
 */
export async function waitForToast(page: Page, expectedText?: string): Promise<void> {
  const toast = page.locator('.toast, .notification, .alert, .message');
  await expect(toast).toBeVisible({ timeout: config.timeouts.short });
  if (expectedText) {
    await expect(toast).toContainText(expectedText);
  }
}

/**
 * Check if hub is online by calling status API
 */
export async function isHubOnline(request: APIRequestContext): Promise<boolean> {
  try {
    const response = await request.get(config.api.status, { timeout: config.timeouts.short });
    return response.ok();
  } catch {
    return false;
  }
}

/**
 * Get hub status
 */
export async function getHubStatus(request: APIRequestContext): Promise<any> {
  const response = await request.get(config.api.status);
  expect(response.ok()).toBeTruthy();
  return await response.json();
}

/**
 * Get all aquariums
 */
export async function getAquariums(request: APIRequestContext): Promise<any> {
  const response = await request.get(config.api.aquariums);
  expect(response.ok()).toBeTruthy();
  return await response.json();
}

/**
 * Get all devices
 */
export async function getDevices(request: APIRequestContext): Promise<any> {
  const response = await request.get(config.api.devices);
  expect(response.ok()).toBeTruthy();
  return await response.json();
}

/**
 * Get unmapped devices
 */
export async function getUnmappedDevices(request: APIRequestContext): Promise<any> {
  const response = await request.get(config.api.unmappedDevices);
  expect(response.ok()).toBeTruthy();
  return await response.json();
}

/**
 * Create a test aquarium via API
 */
export async function createTestAquarium(request: APIRequestContext, name: string): Promise<any> {
  const response = await request.post(config.api.aquariums, {
    data: { name, volume: 100 }
  });
  return await response.json();
}

/**
 * Delete an aquarium via API
 */
export async function deleteAquarium(request: APIRequestContext, id: string): Promise<any> {
  const response = await request.post(config.api.aquariumDelete, {
    data: { id }
  });
  return await response.json();
}

/**
 * Wait for element with retry
 */
export async function waitForElement(page: Page, selector: string, timeout = config.timeouts.medium): Promise<void> {
  await page.waitForSelector(selector, { state: 'visible', timeout });
}

/**
 * Fill form field with label
 */
export async function fillFormField(page: Page, label: string, value: string): Promise<void> {
  const field = page.getByLabel(label);
  await field.fill(value);
}

/**
 * Select dropdown option
 */
export async function selectOption(page: Page, selector: string, value: string): Promise<void> {
  await page.selectOption(selector, value);
}

/**
 * Click button by text
 */
export async function clickButton(page: Page, text: string): Promise<void> {
  await page.getByRole('button', { name: text }).click();
}

/**
 * Verify page title or heading
 */
export async function verifyPageHeading(page: Page, expectedText: string): Promise<void> {
  const heading = page.locator('h1, h2, .page-title, .header-title').first();
  await expect(heading).toContainText(expectedText);
}

/**
 * Take screenshot with timestamp
 */
export async function takeScreenshot(page: Page, name: string): Promise<void> {
  const timestamp = new Date().toISOString().replace(/[:.]/g, '-');
  await page.screenshot({ path: `screenshots/${name}-${timestamp}.png`, fullPage: true });
}

/**
 * Wait for API response
 */
export async function waitForApiResponse(page: Page, urlPattern: string | RegExp): Promise<any> {
  const response = await page.waitForResponse(urlPattern, { timeout: config.timeouts.apiResponse });
  return await response.json();
}

/**
 * Check element count
 */
export async function getElementCount(page: Page, selector: string): Promise<number> {
  return await page.locator(selector).count();
}

/**
 * Verify table row exists
 */
export async function verifyTableRowExists(page: Page, tableSelector: string, rowText: string): Promise<void> {
  const table = page.locator(tableSelector);
  await expect(table.locator('tr', { hasText: rowText })).toBeVisible();
}

/**
 * Get table row count
 */
export async function getTableRowCount(page: Page, tableSelector: string): Promise<number> {
  return await page.locator(`${tableSelector} tbody tr`).count();
}
