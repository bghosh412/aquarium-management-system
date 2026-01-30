# AMS Hub UI Automation Tests

Playwright-based UI automation testing suite for the Aquarium Management System Hub.

## Prerequisites

- Node.js 18+ installed
- Hub running and accessible at `http://192.168.1.53` (or custom URL)

## Setup

```bash
cd Automation

# Install dependencies
npm install

# Install Playwright browsers
npx playwright install chromium
```

## Configuration

The hub URL defaults to `http://192.168.1.53`. Override with environment variable:

```bash
export HUB_URL=http://your-hub-ip
```

Or in Windows:
```cmd
set HUB_URL=http://your-hub-ip
```

## Running Tests

### All Tests
```bash
npm test
```

### Smoke Tests (Quick Validation)
```bash
npm run test:smoke
```

### API Tests Only
```bash
npm run test:api
```

### Specific Test Suite
```bash
npm run test:dashboard
npm run test:aquarium
npm run test:devices
npm run test:settings
```

### Headed Mode (Watch Browser)
```bash
npm run test:headed
```

### Interactive UI Mode
```bash
npm run test:ui
```

### Debug Mode
```bash
npm run test:debug
```

## Test Reports

After tests run, view the HTML report:

```bash
npm run report
```

Reports are saved in `playwright-report/` directory.

## Test Structure

```
Automation/
├── config/
│   └── hub.config.ts      # Configuration and API endpoints
├── helpers/
│   └── test-helpers.ts    # Reusable test utilities
├── tests/
│   ├── smoke.spec.ts      # Quick smoke tests
│   ├── dashboard.spec.ts  # Dashboard/home page tests
│   ├── aquarium.spec.ts   # Aquarium management tests
│   ├── devices.spec.ts    # Device management tests
│   ├── settings.spec.ts   # Settings page tests
│   ├── api.spec.ts        # API endpoint tests
│   └── navigation.spec.ts # Navigation tests
├── playwright.config.ts   # Playwright configuration
└── package.json
```

## Test Tags

Tests are tagged for selective execution:

- `@smoke` - Quick validation tests
- `@api` - API-only tests (no UI)
- `@dashboard` - Dashboard tests
- `@aquarium` - Aquarium management
- `@devices` - Device management
- `@settings` - Settings pages
- `@navigation` - Navigation tests

Run tagged tests:
```bash
npx playwright test --grep @smoke
npx playwright test --grep @api
```

## Writing New Tests

1. Create a new `.spec.ts` file in `tests/`
2. Import helpers from `../helpers/test-helpers`
3. Import config from `../config/hub.config`
4. Use `test.describe()` for grouping
5. Tag important tests with `@smoke`, `@api`, etc.

Example:
```typescript
import { test, expect } from '@playwright/test';
import { config } from '../config/hub.config';
import { navigateTo } from '../helpers/test-helpers';

test.describe('My Feature Tests', () => {
  test('should do something @smoke', async ({ page }) => {
    await navigateTo(page, config.pages.dashboard);
    await expect(page.locator('.my-element')).toBeVisible();
  });
});
```

## CI/CD Integration

The test suite is configured for CI:
- Single worker (sequential execution for embedded device)
- Retries on failure
- HTML report generation
- Screenshots on failure

Example GitHub Actions:
```yaml
- name: Run Playwright Tests
  run: |
    cd Automation
    npm ci
    npx playwright install chromium
    npm test
  env:
    HUB_URL: http://192.168.1.53
```

## Troubleshooting

### Hub not reachable
```bash
# Verify hub is online
curl http://192.168.1.53/api/status
```

### Tests timing out
- Increase `timeout` in `playwright.config.ts`
- Check network connectivity to hub

### Browser not installing
```bash
npx playwright install --with-deps chromium
```

## Notes for MicroCore Migration

When migrating to MicroCore framework:
1. Update API endpoints in `config/hub.config.ts`
2. Update page paths if changed
3. Run smoke tests to verify basic functionality
4. Run full test suite for comprehensive validation
