import { test as base } from '@playwright/test';
import { delayForESP32Recovery, ESP32_RECOVERY_DELAY_MS } from '../helpers/test-helpers';

/**
 * Extended test fixture that automatically adds delays between tests
 * to prevent ESP32 Hub from being overwhelmed by rapid requests
 */
export const test = base.extend({
  // Override page fixture to add cleanup delay
  page: async ({ page }, use) => {
    await use(page);
    // Delay after each test to let ESP32 recover
    await delayForESP32Recovery();
  },
  
  // Override request fixture to add cleanup delay  
  request: async ({ request }, use) => {
    await use(request);
    // Delay after API tests to let ESP32 recover
    await delayForESP32Recovery();
  },
});

// Re-export expect from playwright
export { expect } from '@playwright/test';

// Export delay constant for manual use if needed
export { delayForESP32Recovery, ESP32_RECOVERY_DELAY_MS };
