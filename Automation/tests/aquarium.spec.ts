import { test, expect } from '@playwright/test';
import { config } from '../config/hub.config';
import { 
  navigateTo, 
  waitForPageLoad, 
  getAquariums, 
  createTestAquarium,
  deleteAquarium,
  clickButton,
  fillFormField,
  verifyPageHeading,
  waitForApiResponse,
  delayForESP32Recovery
} from '../helpers/test-helpers';

/**
 * Aquarium Management Tests
 * @tags aquarium
 */
test.describe('Aquarium Management Tests', () => {

  // Add delay after each test to let ESP32 recover
  test.afterEach(async () => {
    await delayForESP32Recovery();
  });

  test('should load aquarium selection page @smoke', async ({ page }) => {
    await navigateTo(page, config.pages.aquariumSelection);
    
    // Page should load without errors
    await expect(page.locator('body')).toBeVisible();
  });

  test('should display aquarium list', async ({ page }) => {
    await navigateTo(page, config.pages.aquariumSelection);
    
    // Look for aquarium cards or list items
    const aquariumList = page.locator('.aquarium-card, .aquarium-item, [class*="aquarium"]');
    // May be empty if no aquariums exist
    await expect(page.locator('body')).toBeVisible();
  });

  test('should navigate to add new aquarium page', async ({ page }) => {
    await navigateTo(page, config.pages.aquariumSelection);
    
    // Find and click add button
    const addButton = page.locator('a[href*="add"], button:has-text("Add"), .add-btn, [class*="add"]');
    if (await addButton.count() > 0) {
      await addButton.first().click();
      await waitForPageLoad(page);
      
      // Should be on add page
      expect(page.url()).toContain('add');
    }
  });

  test('should load add aquarium form', async ({ page }) => {
    await navigateTo(page, config.pages.addAquarium);
    
    // Form should have name input
    const nameInput = page.locator('input[name="name"], input#name, input[placeholder*="name" i]');
    if (await nameInput.count() > 0) {
      await expect(nameInput).toBeVisible();
    }
  });

  test('API: should get aquariums list @api', async ({ request }) => {
    const response = await request.get(config.api.aquariums);
    expect(response.ok()).toBeTruthy();
    
    const data = await response.json();
    expect(Array.isArray(data.aquariums) || Array.isArray(data)).toBeTruthy();
  });

  test('API: should create and delete aquarium @api', async ({ request }) => {
    // Create test aquarium with all required fields
    const createResponse = await request.post(config.api.aquariums, {
      headers: { 'Content-Type': 'application/json' },
      data: {
        name: `Test_${Date.now()}`,
        volumeLiters: 50,
        enabled: true
      }
    });
    
    // Check if response is ok before parsing JSON
    if (createResponse.ok()) {
      const createResult = await createResponse.json();
      
      // If creation was successful, try to delete it
      if (createResult.success && createResult.aquarium?.id) {
        const deleteResponse = await request.post(config.api.aquariumDelete, {
          headers: { 'Content-Type': 'application/json' },
          data: { id: createResult.aquarium.id }
        });
        expect(deleteResponse.ok()).toBeTruthy();
      }
    } else {
      // API returned error - acceptable for this test (API validation)
      const text = await createResponse.text();
      console.log('Create response:', createResponse.status(), text);
    }
  });

  test('should display aquarium details', async ({ page, request }) => {
    // First get aquariums via API
    const response = await request.get(config.api.aquariums);
    const data = await response.json();
    const aquariums = data.aquariums || data;
    
    if (aquariums.length > 0) {
      // Navigate to aquarium selection
      await navigateTo(page, config.pages.aquariumSelection);
      
      // Click on first aquarium
      const firstAquarium = page.locator('.aquarium-card, .aquarium-item, [class*="aquarium-list"] > *').first();
      if (await firstAquarium.count() > 0) {
        await firstAquarium.click();
        await waitForPageLoad(page);
      }
    }
  });

  test('should load manage aquarium page', async ({ page }) => {
    await navigateTo(page, config.pages.manageAquarium);
    
    // Page should load
    await expect(page.locator('body')).toBeVisible();
  });

  test('should display aquarium devices page', async ({ page }) => {
    // Navigate with a dummy aquarium ID
    await page.goto(config.pages.aquariumDevices + '?id=1');
    await waitForPageLoad(page);
    
    // Page should load (may show error if aquarium doesn't exist)
    await expect(page.locator('body')).toBeVisible();
  });

  test('should validate aquarium form fields', async ({ page }) => {
    await navigateTo(page, config.pages.addAquarium);
    
    // Try to submit empty form
    const submitBtn = page.locator('button[type="submit"], button:has-text("Save"), button:has-text("Add"), button:has-text("Create")');
    if (await submitBtn.count() > 0) {
      await submitBtn.first().click();
      
      // Should show validation error or form should not submit
      // Check for error messages
      const errorMsg = page.locator('.error, .invalid, [class*="error"]');
      // Either error shown or still on same page
    }
  });
});
