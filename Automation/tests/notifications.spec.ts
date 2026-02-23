import { test, expect, APIRequestContext } from '@playwright/test';
import { config } from '../config/hub.config';
import { delayForESP32Recovery } from '../helpers/test-helpers';

/**
 * Notification Feature Tests
 *
 * Validates that hub API operations trigger ntfy.sh push notifications.
 *
 * Covers:
 *   1. Hub boot notification        (system.boot)
 *   2. Hub health baseline          (API sanity)
 *   3. Aquarium CRUD                (config.aquarium.add / edit / delete)
 *   4. Device delete                (config.device.delete)
 *   5. Session summary              (dump all recent ntfy messages)
 *
 * config.* and system.* routes use throttle=0, so notifications are sent
 * immediately.  We retry-poll ntfy.sh for up to 20 s to absorb any
 * HTTP propagation delay.
 *
 * ntfy.sh topic: ams-hub-bg
 */

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

const NTFY_TOPIC = 'ams-hub-bg';
const NTFY_POLL_URL = `https://ntfy.sh/${NTFY_TOPIC}/json?poll=1`;

/** Max time (ms) to retry-poll ntfy.sh for expected messages. */
const NTFY_POLL_TIMEOUT_MS = 30_000;

/** Poll ntfy.sh for messages since a given unix epoch (seconds). */
async function pollNtfy(
  request: APIRequestContext,
  sinceEpoch: number,
): Promise<any[]> {
  const url = `${NTFY_POLL_URL}&since=${sinceEpoch}`;
  const response = await request.get(url);
  const text = await response.text();
  return text
    .trim()
    .split('\n')
    .filter((line) => line.length > 0)
    .map((line) => JSON.parse(line))
    .filter((msg) => msg.event === 'message');
}

/** Current epoch in seconds. */
function nowEpoch(): number {
  return Math.floor(Date.now() / 1000);
}

/**
 * Retry-poll ntfy until at least `minCount` messages appear, or until
 * `timeoutMs` elapses.  Polls every 3 s.
 */
async function pollNtfyUntil(
  request: APIRequestContext,
  sinceEpoch: number,
  minCount: number,
  timeoutMs: number = NTFY_POLL_TIMEOUT_MS,
): Promise<any[]> {
  const deadline = Date.now() + timeoutMs;
  let msgs: any[] = [];
  while (Date.now() < deadline) {
    msgs = await pollNtfy(request, sinceEpoch);
    if (msgs.length >= minCount) return msgs;
    await new Promise((r) => setTimeout(r, 2_000));
  }
  return msgs;
}

/**
 * Retry-poll ntfy until at least one message matches the predicate,
 * or until `timeoutMs` elapses.  Returns all messages from the last poll.
 */
async function pollNtfyUntilMatch(
  request: APIRequestContext,
  sinceEpoch: number,
  predicate: (msg: any) => boolean,
  timeoutMs: number = NTFY_POLL_TIMEOUT_MS,
): Promise<any[]> {
  const deadline = Date.now() + timeoutMs;
  let msgs: any[] = [];
  while (Date.now() < deadline) {
    msgs = await pollNtfy(request, sinceEpoch);
    if (msgs.some(predicate)) return msgs;
    await new Promise((r) => setTimeout(r, 2_000));
  }
  return msgs;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

test.describe('Notification Feature Tests', () => {
  // Allow enough room for API calls + ntfy retry-polling
  test.describe.configure({ timeout: 90_000 });

  test.afterEach(async () => {
    await delayForESP32Recovery(1000);
  });

  // =======================================================================
  // 1. Hub boot notification (baseline sanity)
  // =======================================================================
  test('Hub boot notification exists in ntfy', async ({ request }) => {
    // First check hub uptime so we know how far back to search
    const statusResp = await request.get(config.api.status);
    const statusData = await statusResp.json();
    const uptimeSec = statusData.uptime || 0;

    // Look back slightly beyond the uptime (add 5-min buffer), capped at 12 h
    const lookbackSec = Math.min(uptimeSec + 300, 43200);
    const since = nowEpoch() - lookbackSec;

    console.log(`  ℹ️  Hub uptime: ${uptimeSec}s → searching ntfy since ${lookbackSec}s ago`);

    const msgs = await pollNtfy(request, since);

    console.log(`  📨 ntfy messages (last ${Math.round(lookbackSec / 60)} min): ${msgs.length}`);
    msgs
      .filter((m) => m.tags?.includes('system.boot'))
      .forEach((m) => console.log(`     [${m.tags?.join(',')}] ${m.message}`));

    const bootMsg = msgs.some(
      (m) =>
        m.tags?.includes('system.boot') ||
        m.message?.includes('HUB UI is up and running'),
    );
    expect(bootMsg, 'Expected a system.boot notification in ntfy').toBeTruthy();
  });

  // =======================================================================
  // 2. Hub status health check
  // =======================================================================
  test('Hub status API responsive and healthy', async ({ request }) => {
    const resp = await request.get(config.api.status);
    expect(resp.ok()).toBeTruthy();

    const data = await resp.json();
    expect(data.ntp_synced).toBeTruthy();
    expect(data.uptime).toBeGreaterThan(0);

    const heapKB = (data.memory.heapFree / 1024).toFixed(0);
    const psramKB = (data.memory.psramFree / 1024).toFixed(0);
    console.log(`  ✅ Uptime: ${data.uptime}s | NTP: ${data.ntp_synced}`);
    console.log(`  💾 Heap: ${heapKB} KB free | PSRAM: ${psramKB} KB free`);
  });

  // =======================================================================
  // 3. Aquarium create → update → delete  ⇒  3 ntfy notifications
  // =======================================================================
  test('Aquarium CRUD triggers ntfy notifications', async ({ request }) => {
    // Subtract 5 s to absorb clock skew between test host and ntfy.sh
    const since = nowEpoch() - 5;

    // --- CREATE ---------------------------------------------------------
    const createResp = await request.post(config.api.aquariums, {
      headers: { 'Content-Type': 'application/json' },
      data: {
        name: 'Notif Test Tank',
        volumeLiters: 50,
        tankType: 'Planted',
        location: 'Test Room',
      },
    });
    expect(createResp.ok(), 'Create aquarium should succeed').toBeTruthy();
    const createData = await createResp.json();
    expect(createData.success).toBeTruthy();
    const aqId = createData.id ?? createData.aquariumId;
    console.log(`  ✅ Created aquarium ID: ${aqId}`);

    await delayForESP32Recovery(2000);

    // --- UPDATE ---------------------------------------------------------
    const updateResp = await request.post(
      `${config.api.aquariumUpdate}?id=${aqId}`,
      {
        headers: { 'Content-Type': 'application/json' },
        data: {
          name: 'Notif Test Tank v2',
          volumeLiters: 75,
          tankType: 'Planted',
          location: 'Updated Room',
        },
      },
    );
    expect(updateResp.ok(), 'Update aquarium should succeed').toBeTruthy();
    const updateText = await updateResp.text();
    expect(updateText.toLowerCase()).toContain('updated');
    console.log(`  ✅ Updated aquarium ID: ${aqId} → "${updateText.trim()}"`);

    await delayForESP32Recovery(2000);

    // --- DELETE ----------------------------------------------------------
    const deleteResp = await request.post(
      `${config.api.aquariumDelete}?id=${aqId}`,
    );
    expect(deleteResp.ok(), 'Delete aquarium should succeed').toBeTruthy();
    const deleteBody = await deleteResp.text();
    console.log(`  ✅ Deleted aquarium ID: ${aqId} → ${deleteBody.substring(0, 60)}`);

    // Brief pause so the hub worker task can POST the last notification
    await delayForESP32Recovery(3000);

    // --- VERIFY NOTIFICATIONS -------------------------------------------
    console.log(`  ⏳ Polling ntfy (up to ${NTFY_POLL_TIMEOUT_MS / 1000}s) for 3 notifications…`);
    const msgs = await pollNtfyUntil(request, since, 3);

    console.log(`  📨 ntfy messages since CRUD: ${msgs.length}`);
    msgs.forEach((m) =>
      console.log(`     [${m.tags?.join(',')}] ${m.message}`),
    );

    const hasCreate = msgs.some(
      (m) =>
        m.message?.includes('Notif Test Tank') &&
        m.message?.toLowerCase().includes('created'),
    );
    const hasUpdate = msgs.some(
      (m) =>
        m.message?.includes('Notif Test Tank v2') &&
        m.message?.toLowerCase().includes('updated'),
    );
    const hasDelete = msgs.some(
      (m) =>
        m.message?.toLowerCase().includes('deleted') &&
        m.tags?.some((t: string) => t.includes('config.aquarium')),
    );

    console.log(`  ${hasCreate ? '✅' : '❌'} Create notification`);
    console.log(`  ${hasUpdate ? '✅' : '❌'} Update notification`);
    console.log(`  ${hasDelete ? '✅' : '❌'} Delete notification`);

    expect(hasCreate, 'Aquarium CREATE notification missing').toBeTruthy();
    expect(hasUpdate, 'Aquarium UPDATE notification missing').toBeTruthy();
    expect(hasDelete, 'Aquarium DELETE notification missing').toBeTruthy();
  });

  // =======================================================================
  // 4. Device delete notification (fake MAC – API always succeeds)
  // =======================================================================
  test('Device delete triggers ntfy notification', async ({ request }) => {
    const since = nowEpoch() - 5;

    const delResp = await request.post(config.api.deleteDevice, {
      headers: { 'Content-Type': 'application/json' },
      data: { mac: 'AA:BB:CC:DD:EE:FF' },
    });
    expect(delResp.ok(), 'Delete-device API should return 200').toBeTruthy();
    const delData = await delResp.json();
    console.log(`  📝 Delete device response: ${JSON.stringify(delData)}`);

    console.log(`  ⏳ Polling ntfy for device-delete notification…`);
    const isDeviceDelete = (m: any) =>
      m.tags?.includes('config.device.delete') &&
      m.message?.includes('AA:BB:CC:DD:EE:FF');

    const msgs = await pollNtfyUntilMatch(request, since, isDeviceDelete);

    console.log(`  📨 ntfy messages: ${msgs.length}`);
    msgs.forEach((m) =>
      console.log(`     → [${m.tags?.join(',')}] ${m.message}`),
    );

    const found = msgs.some(
      (m) =>
        m.tags?.includes('config.device.delete') &&
        m.message?.includes('AA:BB:CC:DD:EE:FF'),
    );
    console.log(`  ${found ? '✅' : '❌'} Device delete notification`);
    expect(found, 'Device DELETE notification missing').toBeTruthy();
  });

  // =======================================================================
  // 5. Summary – dump all ntfy messages from session
  // =======================================================================
  test('Summary: all ntfy messages from this session', async ({ request }) => {
    const since = nowEpoch() - 900; // last 15 min
    const msgs = await pollNtfy(request, since);

    console.log(
      `\n  ══════════════════════════════════════════════════════`,
    );
    console.log(`  📋 Total ntfy messages (last 15 min): ${msgs.length}`);
    console.log(
      `  ══════════════════════════════════════════════════════`,
    );
    msgs.forEach((m, i) => {
      const time = new Date(m.time * 1000).toLocaleTimeString();
      const tags = m.tags?.join(',') || 'none';
      console.log(`  [${i + 1}] ${time} [${tags}] ${m.message}`);
    });
    console.log(
      `  ══════════════════════════════════════════════════════\n`,
    );

    // We expect at minimum: boot + 3 CRUD + device-delete = 5
    expect(msgs.length).toBeGreaterThanOrEqual(4);
  });
});
