// M14: the end-to-end flow of SPEC §15 against a real nexusd, and the final
// screenshot pass over every screen and state at the three DESIGN §13 sizes.
// Runs last (name order) on the store the earlier specs filled.
//
// Real: joining, every upload, the "nothing sent" copy, the download and its
// SHA-256, compare, delete with undo and the real delete, the offline banner
// (the server is killed), the session-ended page (the cookie is dropped).
// Answered in the browser (D-037): the loading skeletons (requests held for a
// moment) and the list error (one request answered 500), because a healthy
// local server is too fast or too healthy to show them.
import { createHash } from 'node:crypto';
import { expect, test, type Browser, type Page } from '@playwright/test';
import { joinCode, killServer, readState, restartServer } from './server';

const DIR = 'screenshots/m14';
const SIZES = [
  { name: '1440', width: 1440, height: 900 },
  { name: '1366', width: 1366, height: 768 },
  { name: '390', width: 390, height: 844 },
] as const;

async function shots(page: Page, name: string, fullPage = true) {
  for (const s of SIZES) {
    await page.setViewportSize({ width: s.width, height: s.height });
    await page.waitForTimeout(300);
    await page.screenshot({ path: `${DIR}/${name}-${s.name}.png`, fullPage, animations: 'disabled' });
  }
  await page.setViewportSize({ width: 1440, height: 900 });
}
const shot = (page: Page, name: string, fullPage = false) =>
  page.screenshot({ path: `${DIR}/${name}.png`, fullPage, animations: 'disabled' });

function csv(len: number, seed: number, edit = false): Buffer {
  let x = seed >>> 0;
  const r = () => ((x = (Math.imul(x, 1664525) + 1013904223) >>> 0), x);
  let s = 'student_id,course,week,score\n';
  while (s.length < len) s += `${20240000 + (r() % 4000)},CS${100 + (r() % 300)},${1 + (r() % 14)},${(r() % 10000) / 100}\n`;
  s = s.slice(0, len);
  if (edit) s = `${s.slice(0, len / 3)}20249999,CS301,9,88.50\n${s.slice(len / 3)}`;
  return Buffer.from(s);
}
const sha = (b: Buffer) => createHash('sha256').update(b).digest('hex');

async function signIn(page: Page, username: string, password: string) {
  await page.goto('/signin');
  await page.getByLabel('Username').fill(username);
  await page.getByLabel('Password', { exact: true }).fill(password);
  await page.getByRole('button', { name: 'Sign in' }).click();
  await expect(page.getByRole('navigation', { name: 'Main' })).toBeVisible();
  await expect(page.locator('.status--live')).toBeVisible({ timeout: 15_000 });
}
const admin = (page: Page) => signIn(page, readState().admin.username, readState().admin.password);

const upload = (page: Page, name: string, buffer: Buffer) =>
  page.setInputFiles('[data-testid=upload-input]', { name, mimeType: 'text/csv', buffer });
const trayRow = (page: Page, name: string) => page.getByTestId('tray-row').filter({ hasText: name }).first();
async function done(page: Page, name: string) {
  await expect(trayRow(page, name)).toHaveAttribute('data-phase', 'done', { timeout: 30_000 });
}
const closeTray = (page: Page) => page.getByRole('button', { name: 'Close the upload tray' }).click().catch(() => undefined);

test.describe.configure({ mode: 'serial' });
test.use({ viewport: { width: 1440, height: 900 } });

const NEHA = { username: 'neha', displayName: 'Neha Kapoor', password: 'neha-password-12' };

test.describe('M14', () => {
  test('SPEC §15: join, upload, identical copy sends nothing, download, compare, delete with undo', async ({ page }) => {
    // Join with the lab's code.
    await page.goto('/join');
    await page.getByLabel('Display name').fill(NEHA.displayName);
    await page.getByLabel('Username').fill(NEHA.username);
    await page.getByLabel('Password', { exact: true }).fill(NEHA.password);
    await page.getByLabel('Join code').fill(joinCode(readState().dir));
    await page.getByRole('button', { name: 'Create account' }).click();
    await expect(page.getByRole('button', { name: /Neha Kapoor/ })).toBeVisible();
    await expect(page.locator('.status--live')).toBeVisible({ timeout: 15_000 });

    // Upload: unique data to this test, so every block is new.
    const v1 = csv(1_500_000, 1414);
    await upload(page, 'marks_week9.csv', v1);
    await done(page, 'marks_week9.csv');
    await expect(trayRow(page, 'marks_week9.csv')).toContainText(/^.*Stored\. 1\.4\sMB sent, all of it new\./);

    // An identical copy: only fingerprints cross the network.
    await upload(page, 'marks_week9_copy.csv', v1);
    await done(page, 'marks_week9_copy.csv');
    await expect(trayRow(page, 'marks_week9_copy.csv')).toContainText(
      'Stored. Nothing sent: all 1.4 MB was already on the server.',
    );
    await shot(page, 'flow-1-identical-copy-1440');

    // An edited version: a little is sent, and it becomes version 2.
    const v2 = csv(1_500_000, 1414, true);
    await upload(page, 'marks_week9.csv', v2);
    await expect(page.getByTestId('tray-row').filter({ hasText: 'marks_week9.csv' }).first()).toContainText(
      /Stored\. \d+(\.\d)?\sKB sent instead of 1\.4\sMB\./,
      { timeout: 30_000 },
    );
    await closeTray(page);

    // Download from the drawer: the bytes hash to the SHA-256 it shows.
    await page.getByRole('link', { name: 'Files', exact: true }).click();
    const row = page.getByTestId('file-row').filter({ hasText: 'marks_week9.csv' });
    await row.locator('.row__open').click();
    const drawer = page.getByRole('dialog');
    await expect(drawer).toContainText('Version 2, uploaded by you');
    await expect(drawer.getByTestId('sha256')).toHaveText(sha(v2));
    const dl = page.waitForEvent('download');
    await drawer.getByRole('link', { name: 'Download' }).click();
    const file = await dl;
    const stream = await file.createReadStream();
    const chunks: Buffer[] = [];
    for await (const c of stream) chunks.push(c as Buffer);
    expect(sha(Buffer.concat(chunks))).toBe(sha(v2));
    await shot(page, 'flow-2-drawer-1440');

    // Compare the two versions: fixed blocks first, then content-defined.
    await drawer.getByRole('checkbox', { name: /Version 1/ }).check();
    await drawer.getByRole('checkbox', { name: /Version 2/ }).check();
    await drawer.getByRole('button', { name: 'Compare these two' }).click();
    await expect(page.getByRole('heading', { name: 'Compare two versions', level: 1 })).toBeVisible();
    await expect(page.locator('#pick-version-1 option:checked')).toHaveText(/marks_week9\.csv v1/);
    await expect(page.getByTestId('cmp-summary')).toContainText(/of \d+ blocks match version 1/);
    await page.getByRole('button', { name: 'Content-defined', exact: true }).click();
    await expect(page.getByTestId('cmp-summary')).toContainText(/already stored|match version 1/);
    await page.waitForTimeout(3700);
    await shot(page, 'flow-3-compare-cdc-1440');

    // Delete with undo: Undo sends nothing; then the real delete after 6 s.
    await page.getByRole('link', { name: 'Files', exact: true }).click();
    const copy = page.getByTestId('file-row').filter({ hasText: 'marks_week9_copy.csv' });
    await copy.locator('.row__open').click();
    await page.getByRole('dialog').getByRole('button', { name: 'Delete' }).click();
    await page.getByRole('dialog').getByRole('button', { name: 'Delete' }).click();
    await expect(copy).toHaveCount(0);
    await page.getByRole('button', { name: 'Undo' }).click();
    await expect(copy).toHaveCount(1);
    const id = ((await (await page.request.get('/api/files?q=marks_week9_copy')).json()).items[0].id) as number;
    expect((await page.request.get(`/api/files/${id}`)).status()).toBe(200);
    await copy.locator('.row__open').click();
    await page.getByRole('dialog').getByRole('button', { name: 'Delete' }).click();
    await page.getByRole('dialog').getByRole('button', { name: 'Delete' }).click();
    await page.mouse.move(700, 100);
    await expect.poll(async () => (await page.request.get(`/api/files/${id}`)).status(), { timeout: 12_000 }).toBe(404);
    // The other file, which shares every block with the deleted copy, still downloads.
    const left = ((await (await page.request.get('/api/files?q=marks_week9.csv')).json()).items[0].id) as number;
    const body = await (await page.request.get(`/api/files/${left}/content`)).body();
    expect(sha(body)).toBe(sha(v2));
  });

  test('every screen at three sizes, light', async ({ page }) => {
    await page.goto('/signin');
    await shots(page, 'signin');
    await page.goto('/join');
    await shots(page, 'join');
    await admin(page);
    await page.waitForTimeout(1500); /* the block field has settled */
    await shots(page, 'overview');
    await page.getByRole('link', { name: 'Files', exact: true }).click();
    await expect(page.getByTestId('file-row').first()).toBeVisible();
    await shots(page, 'files');
    await page.getByTestId('file-row').first().locator('.row__open').click();
    await expect(page.getByRole('dialog')).toBeVisible();
    await shots(page, 'files-drawer', false);
    await page.keyboard.press('Escape');
    await expect(page.getByRole('dialog')).toHaveCount(0);
    await page.getByRole('link', { name: 'Compare', exact: true }).click();
    await expect(page.getByTestId('cmp-summary')).toBeVisible();
    await page.waitForTimeout(3700);
    await shots(page, 'compare-fixed');
    await page.getByRole('button', { name: 'Content-defined', exact: true }).click();
    await page.waitForTimeout(3700);
    await shots(page, 'compare-cdc');
    await page.getByRole('link', { name: 'Engine', exact: true }).click();
    await expect(page.getByRole('heading', { name: 'Engine', level: 1 })).toBeVisible();
    await page.waitForTimeout(1000);
    await shots(page, 'engine');
    await page.getByRole('link', { name: 'Users', exact: true }).click();
    await expect(page.getByRole('heading', { name: 'Users', level: 1 })).toBeVisible();
    await shots(page, 'users');
  });

  test('states: empty, loading, error, offline, session ended', async ({ page, browser }) => {
    // Empty: a student with no files yet.
    await signIn(page, NEHA.username, NEHA.password);
    const fresh = { username: 'omar', displayName: 'Omar Sheikh', password: 'omar-password-12' };
    const r = await page.request.post('/api/auth/join', {
      headers: { 'X-NexusSync': '1' },
      data: { ...fresh, joinCode: joinCode(readState().dir) },
    });
    expect([201, 409]).toContain(r.status());
    const ctx = await browser.newContext({ viewport: { width: 1440, height: 900 } });
    const empty = await ctx.newPage();
    await signIn(empty, fresh.username, fresh.password);
    await empty.getByRole('link', { name: 'Files', exact: true }).click();
    await shots(empty, 'empty-files');
    await empty.getByRole('link', { name: 'Overview', exact: true }).click();
    await shots(empty, 'empty-overview');

    // Loading: hold the data requests for a moment.
    await empty.route('**/api/{stats,files,blockmap}*', async (rt) => {
      await new Promise((ok) => setTimeout(ok, 4000));
      await rt.continue().catch(() => undefined);
    });
    await empty.reload();
    await expect(empty.locator('.skeleton').first()).toBeVisible();
    await shots(empty, 'loading-overview');
    await empty.unroute('**/api/{stats,files,blockmap}*');

    // Error in a list: one answer is a 500; the line offers Retry.
    await empty.route('**/api/files?*', (rt) => rt.fulfill({ status: 500, json: { error: { code: 'internal', message: 'x' } } }));
    await empty.getByRole('link', { name: 'Files', exact: true }).click();
    await expect(empty.getByRole('alert')).toContainText('Retry');
    await shots(empty, 'error-files');
    await empty.unroute('**/api/files?*');
    await ctx.close();

    // Offline: the server really goes away, the banner appears, it comes back.
    await page.goto('/');
    await expect(page.locator('.status--live')).toBeVisible({ timeout: 15_000 });
    killServer(readState());
    await expect(page.getByText(/^Lost connection to the lab server\./)).toBeVisible({ timeout: 20_000 });
    await shots(page, 'offline-overview');
    await restartServer();
    await expect(page.locator('.status--live')).toBeVisible({ timeout: 30_000 });

    // Session ended: the cookie is gone, the next request gets 401.
    await page.context().clearCookies();
    await page.getByRole('link', { name: 'Files', exact: true }).click();
    await expect(page.getByText('Your session ended. Sign in again.')).toBeVisible({ timeout: 15_000 });
    await shots(page, 'session-ended');
  });

  test('dark mode once per screen, and reduced motion', async ({ browser }) => {
    const dark = await darkPage(browser);
    for (const [link, name] of [
      ['Overview', 'overview'],
      ['Files', 'files'],
      ['Compare', 'compare'],
      ['Engine', 'engine'],
      ['Users', 'users'],
    ] as const) {
      await dark.getByRole('link', { name: link, exact: true }).click();
      await dark.waitForTimeout(name === 'compare' ? 3700 : 1200);
      await dark.screenshot({ path: `${DIR}/dark-${name}-1440.png`, fullPage: true, animations: 'disabled' });
    }
    await dark.context().close();

    // Reduced motion: no entrance animation on the block field or compare strips.
    const ctx = await browser.newContext({ viewport: { width: 1440, height: 900 }, reducedMotion: 'reduce' });
    const page = await ctx.newPage();
    await admin(page);
    const tile = page.locator('.tile').first();
    await expect(tile).toBeVisible();
    expect(await tile.evaluate((el) => getComputedStyle(el).animationName)).toBe('none');
    await page.getByRole('link', { name: 'Compare', exact: true }).click();
    const blk = page.locator('.blk').first();
    await expect(blk).toBeVisible();
    expect(await blk.evaluate((el) => getComputedStyle(el).animationName)).toBe('none');
    await ctx.close();
  });
});

test('phone: Compare says tap, and the tapped block details come into view', async ({ browser }) => {
  const ctx = await browser.newContext({ viewport: { width: 390, height: 844 }, isMobile: true, hasTouch: true });
  const page = await ctx.newPage();
  await admin(page);
  await page.getByRole('link', { name: 'Compare', exact: true }).click();
  await expect(page.getByText('Tap a version 2 block to find its match')).toBeVisible();
  await expect(page.getByText('Select a block', { exact: true })).toHaveCount(0);
  await page.waitForTimeout(3700);
  await page.locator('.blk').last().tap();
  await expect(page.getByTestId('cmp-details')).toBeInViewport({ ratio: 0.3 });
  await page.screenshot({ path: `${DIR}/compare-tapped-390.png`, animations: 'disabled' });
  // Files as admin below 1200 px: the owner column is hidden, so each row
  // names its owner in the meta line (two students' dataset.csv look alike).
  await page.getByRole('link', { name: 'Files', exact: true }).click();
  await expect(page.getByTestId('file-row').first().locator('.row__meta-owner')).toBeVisible();
  await expect(page.getByTestId('file-row').first().locator('.row__meta-owner')).toHaveText(/^ · \S/);
  await ctx.close();
});

async function darkPage(browser: Browser): Promise<Page> {
  const ctx = await browser.newContext({ viewport: { width: 1440, height: 900 }, colorScheme: 'dark' });
  const page = await ctx.newPage();
  await admin(page);
  return page;
}

test('compact widths: the header stays on one line (D-052)', async ({ browser }) => {
  const ctx = await browser.newContext({ viewport: { width: 1440, height: 900 } });
  const page = await ctx.newPage();
  await admin(page);
  for (const width of [768, 900, 1023, 1024, 1199, 1366]) {
    await page.setViewportSize({ width, height: 800 });
    await page.waitForTimeout(150);
    const tops = await page.locator('.nav a').evaluateAll((as) => as.map((a) => Math.round(a.getBoundingClientRect().top)));
    expect(new Set(tops).size, `nav on one line at ${width} px`).toBe(1);
    const name = page.locator('.user__name');
    if (width >= 1024) {
      expect(Math.round((await name.boundingBox())!.height), `display name on one line at ${width} px`).toBeLessThan(30);
    } else {
      await expect(name).toBeHidden();
    }
  }
  await ctx.close();
});
