import { defineConfig, devices } from '@playwright/test';

/**
 * ESP32 Recovery Delay (ms) - Hub needs time between requests
 * This is applied via slowMo to prevent memory issues from rapid calls
 */
const ESP32_SLOW_MO = 500;

/**
 * Playwright configuration for AMS Hub UI Testing
 * @see https://playwright.dev/docs/test-configuration
 */
export default defineConfig({
  testDir: './tests',
  
  /* Run tests in files in parallel */
  fullyParallel: false,  // Sequential for embedded device testing
  
  /* Fail the build on CI if you accidentally left test.only in the source code */
  forbidOnly: !!process.env.CI,
  
  /* Retry on CI only - no retries for embedded testing to avoid overwhelming device */
  retries: 0,
  
  /* Single worker for embedded device */
  workers: 1,
  
  /* Reporter to use */
  reporter: [
    ['html', { open: 'never' }],
    ['list']
  ],
  
  /* Global setup to add delays between tests */
  globalSetup: undefined,
  
  /* Shared settings for all the projects below */
  use: {
    /* Base URL for Hub - can be overridden with HUB_URL env var */
    baseURL: process.env.HUB_URL || 'http://192.168.1.53',
    
    /* Collect trace when retrying the failed test */
    trace: 'on-first-retry',
    
    /* Screenshot on failure */
    screenshot: 'only-on-failure',
    
    /* Timeout for actions - longer for ESP32 */
    actionTimeout: 15000,
    
    /* Navigation timeout - longer for embedded device */
    navigationTimeout: 30000,
    
    /* Slow down browser actions for ESP32 recovery */
    launchOptions: {
      slowMo: ESP32_SLOW_MO,
    },
  },

  /* Configure projects for major browsers */
  projects: [
    {
      name: 'chromium',
      use: { ...devices['Desktop Chrome'] },
    },
    // Uncomment for additional browsers
    // {
    //   name: 'firefox',
    //   use: { ...devices['Desktop Firefox'] },
    // },
    // {
    //   name: 'webkit',
    //   use: { ...devices['Desktop Safari'] },
    // },
  ],

  /* Global timeout */
  timeout: 60000,
  
  /* Expect timeout */
  expect: {
    timeout: 10000,
  },
});
