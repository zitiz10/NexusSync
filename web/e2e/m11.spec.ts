// M11: Engine and Users screens, and the system states they touch, against
// a real nexusd (DESIGN §8.6–§8.8). Runs after m10 (name order) and brings
// its own data. Only two things are answered in the browser (D-037):
// the LAN addresses (the e2e server listens on 127.0.0.1 only, so it has
// none to offer; the QR state needs one) and the "recovering" page (real
// recovery takes milliseconds).
import { expect, test, type Page } from '@playwright/test';
import { killServer, readState, restartServer } from './server';

const DIR = 'screenshots/m11';
const SIZES = [
  { name: '1440', width: 1440, height: 900 },
  { name: '1366', width: 1366, height: 768 },
  { name: '390', width: 390, height: 844 },
] as const;

async function shotsAtSizes(page: Page, name: string, fullPage = false) {
  for (const s of SIZES) {
    await page.setViewportSize({ width: s.width, height: s.height });
    await page.waitForTimeout(250);
    await page.screenshot({ path: `${DIR}/${name}-${s.name}.png`, fullPage, animations: 'disabled' });
  }
  await page.setViewportSize({ width: 1440, height: 900 });
}
const shot = (page: Page, name: string, fullPage = false) =>
  page.screenshot({ path: `${DIR}/${name}.png`, fullPage, animations: 'disabled' });

async function signIn(page: Page, username: string, password: string) {
  await page.goto('/signin');
  await page.getByLabel('Username').fill(username);
  await page.getByLabel('Password', { exact: true }).fill(password);
  await page.getByRole('button', { name: 'Sign in' }).click();
  await expect(page.getByRole('navigation', { name: 'Main' })).toBeVisible();
}
const admin = (page: Page) => signIn(page, readState().admin.username, readState().admin.password);
const student = (page: Page) => signIn(page, readState().student.username, readState().student.password);

const upload = (page: Page, name: string, text: string) =>
  page.setInputFiles('[data-testid=upload-input]', { name, mimeType: 'text/csv', buffer: Buffer.from(text) });

test.describe.configure({ mode: 'serial' });
test.use({ viewport: { width: 1440, height: 900 } });

test.describe('M11', () => {
  test('Engine: every section with live data, checkpoint, event log follows uploads', async ({ page }) => {
    await admin(page);
    await page.getByRole('link', { name: 'Engine', exact: true }).click();
    await expect(page.getByRole('heading', { name: 'Engine', level: 1 })).toBeVisible();
    for (const h of ['Server', 'Write-ahead log', 'Block index', 'Containers', 'Integrity', 'Recovery history', 'Event log'])
      await expect(page.getByRole('heading', { name: h, level: 2 })).toBeVisible();
    await expect(page.getByText('Content-defined, 2.0 KB to 16 KB, about 4.0 KB on average')).toBeVisible();
    await expect(page.getByText(/The server listens on this computer only/)).toBeVisible();
    await expect(page.getByTestId('join-code').locator('.joincode__value')).toHaveText(/^[2-9A-HJ-NP-Z]{8}$/);
    await expect(page.getByTestId('recoveries')).toContainText('Clean start');
    await expect(page.getByTestId('event-log')).toContainText('engine.recovery');
    // Cleanup and scan arrive in M12: shown, not usable, and said so.
    await expect(page.getByRole('button', { name: 'Run cleanup' })).toBeDisabled();
    await expect(page.getByRole('button', { name: 'Scan now' })).toBeDisabled();

    // Live: an upload shows up in the event log; the WAL count moves.
    await upload(page, 'engine_probe.csv', 'week,score\n1,90\n2,85\n3,77\n');
    await expect(page.getByTestId('event-log')).toContainText('upload.committed', { timeout: 15_000 });
    await expect(page.getByTestId('event-log')).toContainText('name=engine_probe.csv');

    // The buffer-pool sparkline fills in from 2 s samples.
    await expect(page.locator('svg.spark path')).toHaveAttribute('d', /M.*L/, { timeout: 15_000 });

    await page.getByRole('button', { name: 'Checkpoint now' }).click();
    await expect(page.getByText(/^Checkpoint (written: \d+ pages? in [\d,]+ ms|done\. Nothing was waiting to be written)\.$/)).toBeVisible();
    await expect(page.getByTestId('event-log')).toContainText('engine.checkpoint', { timeout: 10_000 });
    await page.getByRole('button', { name: 'Close the upload tray' }).click().catch(() => undefined);
    await page.waitForTimeout(500);
    await shotsAtSizes(page, 'engine', true);
  });

  test('Engine: QR codes for the LAN addresses (answered in the browser, D-037)', async ({ page }) => {
    await admin(page);
    await page.route('**/api/admin/addresses', (r) =>
      r.fulfill({
        contentType: 'application/json',
        body: JSON.stringify({ port: 8080, listen: '0.0.0.0', items: [{ address: '192.168.1.20', url: 'http://192.168.1.20:8080' }] }),
      }),
    );
    await page.goto('/engine');
    const qr = page.getByRole('img', { name: 'QR code for http://192.168.1.20:8080' });
    await expect(qr).toBeVisible();
    // A real QR symbol: version 1–3 for this URL, square, with a quiet zone.
    const box = (await qr.getAttribute('viewBox'))!.split(' ').map(Number);
    expect(box[2]).toBe(box[3]);
    expect([25, 29, 33]).toContain(box[2]);
    await expect(page.getByText('http://192.168.1.20:8080')).toBeVisible();
    await qr.scrollIntoViewIfNeeded();
    await shot(page, 'engine-qr-1440');
  });

  test('Engine: a hard kill shows up in the recovery history', async ({ page }) => {
    await admin(page);
    await upload(page, 'before_kill.csv', 'a,b\n1,2\n3,4\n5,6\n');
    await expect(page.getByTestId('tray-row').first()).toHaveAttribute('data-phase', 'done', { timeout: 15_000 });
    killServer(readState()); /* committed work is in the log, not yet checkpointed */
    await restartServer();
    await page.goto('/engine');
    const firstRow = page.getByTestId('recoveries').locator('tbody tr').first();
    await expect(firstRow).toContainText('Recovered');
    await expect(firstRow.locator('td').nth(1)).not.toHaveText('0');
    await expect(page.getByTestId('event-log')).toContainText('engine.recovery');
    await page.getByRole('heading', { name: 'Recovery history' }).scrollIntoViewIfNeeded();
    await shot(page, 'engine-recovered-1440');
  });

  test('Users: roles, disable signs the person out everywhere, enable, rotate the join code', async ({ page, browser }) => {
    await admin(page);
    await page.getByRole('link', { name: 'Users', exact: true }).click();
    const rows = page.getByTestId('user-row');
    const me = rows.filter({ hasText: '(you)' });
    await expect(me).toHaveCount(1);
    await expect(me.getByRole('button')).toHaveCount(0); /* no actions on yourself */
    const riya = rows.filter({ hasText: 'Riya Kapoor' });
    await expect(riya).toContainText('Student');
    await expect(riya).toContainText('Active');
    await shotsAtSizes(page, 'users', true);

    await riya.getByRole('button', { name: 'Make admin' }).click();
    await expect(riya).toContainText('Admin');
    await expect(page.getByText('Riya Kapoor is now an admin.')).toBeVisible();
    await expect(riya.getByRole('button', { name: 'Make student' })).toBeFocused();
    await page.getByRole('button', { name: 'Undo' }).click(); /* role changes can be undone */
    await expect(riya.getByRole('button', { name: 'Make admin' })).toBeVisible();
    await expect(riya).not.toContainText('Admin');

    // Riya is signed in elsewhere; disabling her ends that session.
    const ctx = await browser.newContext({ viewport: { width: 1440, height: 900 } });
    const rp = await ctx.newPage();
    await student(rp);
    await riya.getByRole('button', { name: 'Disable' }).click();
    await expect(page.getByText(/Disable Riya Kapoor\? They are signed out everywhere/)).toBeVisible();
    await expect(riya.getByRole('button', { name: 'Disable' })).toBeFocused();
    await shot(page, 'users-disable-confirm-1440');
    await page.keyboard.press('Enter');
    await expect(riya).toContainText('Disabled');
    await expect(riya.getByRole('button', { name: 'Enable' })).toBeFocused(); /* focus follows the action */
    await rp.reload();
    await expect(rp.getByRole('heading', { name: 'Sign in to the lab store' })).toBeVisible();
    await rp.getByLabel('Username').fill(readState().student.username);
    await rp.getByLabel('Password', { exact: true }).fill(readState().student.password);
    await rp.getByRole('button', { name: 'Sign in' }).click();
    await expect(rp.getByText(/disabled/i)).toBeVisible();
    await riya.getByRole('button', { name: 'Enable' }).click();
    await expect(riya).toContainText('Active');
    await rp.getByLabel('Password', { exact: true }).fill(readState().student.password); /* cleared after the refusal */
    await rp.getByRole('button', { name: 'Sign in' }).click();
    await expect(rp.getByRole('navigation', { name: 'Main' })).toBeVisible();
    await ctx.close();

    // Rotate the join code: warned inline, then a new code.
    const code = page.getByTestId('join-code').locator('.joincode__value');
    const old = await code.textContent();
    await page.getByTestId('join-code').getByRole('button', { name: 'Rotate' }).click();
    await expect(page.getByText('Anyone with the old code can no longer join.', { exact: false })).toBeVisible();
    await shot(page, 'users-rotate-confirm-1440');
    await page.keyboard.press('Escape'); /* Escape cancels, focus back on Rotate */
    await expect(page.getByTestId('join-code').getByRole('button', { name: 'Rotate' })).toBeFocused();
    await page.getByTestId('join-code').getByRole('button', { name: 'Rotate' }).click();
    await page.getByRole('button', { name: 'Rotate the code' }).click();
    await expect(page.getByTestId('join-code').getByRole('button', { name: 'Copy' })).toBeFocused();
    await expect(code).not.toHaveText(old ?? '');
    await expect(code).toHaveText(/^[2-9A-HJ-NP-Z]{8}$/);
  });

  test('offline: data visuals dim, text keeps full contrast (D-039 item 1)', async ({ page }) => {
    await admin(page);
    await expect(page.locator('.status--live')).toBeVisible({ timeout: 15_000 });
    await expect(page.getByTestId('block-field')).toBeVisible();
    killServer(readState());
    await expect(page.locator('main.main--dimmed')).toBeVisible({ timeout: 20_000 });
    expect(Number(await page.getByTestId('block-field').evaluate((el) => getComputedStyle(el).opacity))).toBeCloseTo(0.6, 2);
    expect(await page.locator('main').evaluate((el) => getComputedStyle(el).opacity)).toBe('1');
    expect(await page.locator('#hero-title').evaluate((el) => getComputedStyle(el).opacity)).toBe('1');
    await shotsAtSizes(page, 'offline-overview');
    await restartServer();
    /* The app may already have reconnected on its own backoff timer. */
    await page.getByRole('button', { name: 'Retry now' }).click({ timeout: 3000 }).catch(() => undefined);
    await expect(page.locator('.status--live')).toBeVisible({ timeout: 15_000 });
    await expect(page.locator('main.main--dimmed')).toHaveCount(0);
  });

  test('recovering: live count of records redone (D-039 item 2)', async ({ page }) => {
    await page.route('**/api/health', (r) =>
      r.fulfill({
        contentType: 'application/json',
        body: JSON.stringify({
          status: 'recovering',
          version: '0.1.0',
          progress: { phase: 'replaying-log', elapsedMs: 3200, recordsReplayed: 1204 },
        }),
      }),
    );
    await page.goto('/');
    await expect(page.getByText('Replaying the log, 1,204 records replayed, 3 s so far.')).toBeVisible();
    await shotsAtSizes(page, 'recovering-count');
    await page.unroute('**/api/health');
  });

  test('a student cannot reach Engine or Users, nor the admin API', async ({ page }) => {
    await student(page);
    await page.goto('/users');
    await expect(page).toHaveURL(/\/$/);
    expect((await page.request.get('/api/admin/users')).status()).toBe(403);
    expect((await page.request.get('/api/admin/events')).status()).toBe(403);
  });
});
