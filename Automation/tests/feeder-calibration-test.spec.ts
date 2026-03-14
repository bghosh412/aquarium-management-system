import { test, expect } from '@playwright/test';

/**
 * Feeder Calibration Test Button Debug Test
 * Navigates to feeder-calibration page and clicks the Test button,
 * then captures the API response to diagnose hub restart issue.
 */
test('feeder calibration test button should not cause hub restart', async ({ page, request }) => {
    const FEEDER_MAC = '00:70:07:17:02:8C';
    const BASE_URL = process.env.HUB_URL || 'http://192.168.1.53';

    // Capture all console messages
    const consoleLogs: string[] = [];
    page.on('console', msg => {
        consoleLogs.push(`[${msg.type()}] ${msg.text()}`);
    });

    // Capture all network requests/responses
    const networkEvents: string[] = [];
    page.on('request', req => {
        if (req.url().includes('/api/')) {
            networkEvents.push(`REQ: ${req.method()} ${req.url()}`);
        }
    });
    page.on('response', resp => {
        if (resp.url().includes('/api/')) {
            networkEvents.push(`RSP: ${resp.status()} ${resp.url()}`);
        }
    });

    // Navigate to feeder calibration page
    console.log(`Navigating to feeder calibration for MAC: ${FEEDER_MAC}`);
    await page.goto(`${BASE_URL}/device/calibration/feeder-calibration.html?mac=${FEEDER_MAC}`, {
        waitUntil: 'networkidle',
        timeout: 15000,
    });

    // Wait for device info to load (dutyCycleValue should show a number, not '—')
    await expect(page.locator('#dutyCycleValue')).not.toHaveText('—', { timeout: 10000 });
    console.log('Duty cycle loaded:', await page.locator('#dutyCycleValue').textContent());
    console.log('Pulse duration loaded:', await page.locator('#pulseDurationValue').textContent());

    // Intercept the test API call to capture request/response details
    let testApiRequest: any = null;
    let testApiResponse: any = null;

    page.on('request', req => {
        if (req.url().includes('/api/feeder-calibration/test')) {
            testApiRequest = {
                url: req.url(),
                method: req.method(),
                postData: req.postData(),
            };
            console.log('=== TEST API REQUEST ===');
            console.log('URL:', req.url());
            console.log('Method:', req.method());
            console.log('Body:', req.postData());
        }
    });

    page.on('response', async resp => {
        if (resp.url().includes('/api/feeder-calibration/test')) {
            try {
                const body = await resp.text();
                testApiResponse = {
                    status: resp.status(),
                    body: body,
                };
                console.log('=== TEST API RESPONSE ===');
                console.log('Status:', resp.status());
                console.log('Body:', body);
            } catch (e) {
                console.log('=== TEST API RESPONSE ERROR (hub may have restarted) ===');
                console.log('Error:', e);
            }
        }
    });

    // Click the Test button
    console.log('Clicking Test button...');
    const testBtn = page.locator('#testBtn');
    await expect(testBtn).toBeVisible();
    await testBtn.click();

    // Wait for the test to complete (button returns to "🔄 Test")
    // or for an error message to appear
    try {
        await Promise.race([
            // Wait for button to re-enable
            expect(testBtn).toHaveText('🔄 Test', { timeout: 15000 }),
            // Wait for success message
            expect(page.locator('#calMessage.msg-success')).toBeVisible({ timeout: 15000 }),
            // Wait for error message
            expect(page.locator('#calMessage.msg-error')).toBeVisible({ timeout: 15000 }),
        ]);
    } catch (e) {
        console.log('Timeout waiting for test to complete — hub may have restarted');
    }

    // Give a moment for the response to arrive
    await page.waitForTimeout(3000);

    // Check if hub is still up
    console.log('\n=== Checking hub availability after test ===');
    let hubUp = false;
    try {
        const healthResp = await request.get(`${BASE_URL}/api/devices`, { timeout: 5000 });
        hubUp = healthResp.ok();
        console.log('Hub status after test:', hubUp ? 'UP ✓' : `DOWN (status ${healthResp.status()})`);
    } catch (err) {
        console.log('Hub status after test: DOWN / unreachable ✗', err);
    }

    // Print all console logs
    console.log('\n=== Page Console Logs ===');
    consoleLogs.forEach(l => console.log(l));

    // Print network events
    console.log('\n=== Network Events ===');
    networkEvents.forEach(e => console.log(e));

    // Check result message
    const msgEl = page.locator('#calMessage');
    const msgClass = await msgEl.getAttribute('class');
    const msgText = await msgEl.textContent();
    console.log('\n=== Calibration Message ===');
    console.log('Class:', msgClass);
    console.log('Text:', msgText);

    // The hub should remain up after the test
    expect(hubUp, 'Hub should remain up after clicking Test button').toBe(true);

    // The test should succeed
    expect(msgClass, 'Expected success message after test').toContain('msg-success');
});
