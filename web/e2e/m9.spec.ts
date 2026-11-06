// M9: Overview, Files, upload tray against a real nexusd (DESIGN §8.2–§8.4).
// Runs after foundation.spec.ts on the same server (one worker, files in
// name order), so the store is empty when this file starts.
//
// Every failure state is produced for real where the server can be made to
// produce it: a corrupted chunk body (the server re-hashes and rejects it),
// a session ended from another tab, a dropped connection. Only "disk full"
// is answered in the browser (D-037: states a real server cannot be held
// in on demand).
import { createHash } from 'node:crypto';
import { expect, test, type Browser, type Page } from '@playwright/test';
import { readState } from './server';

const SIZES = [
  { name: '1440', width: 1440, height: 900 },
  { name: '1366', width: 1366, height: 768 },
  { name: '390', width: 390, height: 844 },
] as const;

const DIR = 'screenshots/m9';
const shot = (page: Page, name: string, fullPage = true) =>
  page.screenshot({ path: `${DIR}/${name}.png`, fullPage, animations: 'disabled' });

async function shotsAtSizes(page: Page, name: string) {
  for (const s of SIZES) {
    await page.setViewportSize({ width: s.width, height: s.height });
    await page.waitForTimeout(250);
    await page.screenshot({ path: `${DIR}/${name}-${s.name}.png`, animations: 'disabled' });
  }
  await page.setViewportSize({ width: 1440, height: 900 });
}

/** CSV-like bytes (so content-defined chunking finds cut points). */
function csv(len: number, seed: number, edit = false): Buffer {
  let x = seed >>> 0;
  const r = () => ((x = (Math.imul(x, 1664525) + 1013904223) >>> 0), x);
  let s = '';
  while (s.length < len) s += `${r() % 100000},${r() % 977},lab-${r() % 50},${(r() % 10000) / 100}\n`;
  s = s.slice(0, len);
  if (edit) s = `${s.slice(0, len / 2)}edited,row,here,1\n${s.slice(len / 2)}`;
  return Buffer.from(s);
}
const sha = (b: Buffer) => createHash('sha256').update(b).digest('hex');

async function signIn(page: Page, who: 'admin' | 'student') {
  const s = readState();
  const u = who === 'admin' ? s.admin : s.student;
  await page.goto('/signin');
  await page.getByLabel('Username').fill(u.username);
  await page.getByLabel('Password', { exact: true }).fill(u.password);
  await page.getByRole('button', { name: 'Sign in' }).click();
  await expect(page.getByRole('navigation', { name: 'Main' })).toBeVisible();
  await expect(page.locator('.status--live')).toBeVisible({ timeout: 15_000 });
}

async function newSignedIn(browser: Browser, who: 'admin' | 'student', opts = {}) {
  const ctx = await browser.newContext({ viewport: { width: 1440, height: 900 }, ...opts });
  const page = await ctx.newPage();
  await signIn(page, who);
  return { ctx, page };
}

const upload = (page: Page, name: string, buffer: Buffer) =>
  page.setInputFiles('[data-testid=upload-input]', { name, mimeType: 'text/csv', buffer });

const trayRow = (page: Page, name: string) => page.getByTestId('tray-row').filter({ hasText: name });

async function waitPhase(page: Page, name: string, phase: string, timeout = 30_000) {
  await expect(trayRow(page, name).first()).toHaveAttribute('data-phase', phase, { timeout });
}

test.describe.configure({ mode: 'serial' });

test.describe('M9', () => {
  test.use({ viewport: { width: 1440, height: 900 } });

  test('empty states: Overview and Files on a fresh store', async ({ page }) => {
    await signIn(page, 'admin');
    await expect(page.getByRole('heading', { name: 'Nothing stored yet.' })).toBeVisible();
    await expect(page.getByText('Uploads will appear here.')).toBeVisible();
    await expect(page.locator('.tile.tile--l0')).toHaveCount(384);
    await shotsAtSizes(page, 'overview-empty');
    await page.getByRole('link', { name: 'Files', exact: true }).click();
    await expect(page.getByTestId('files-empty')).toHaveText('No files yet. Drop a file anywhere on this page to upload it.');
    await shotsAtSizes(page, 'files-empty');
  });

  test('loading: content-shaped skeletons, no spinner', async ({ page }) => {
    await signIn(page, 'admin');
    await page.route('**/api/stats', async (r) => {
      await new Promise((ok) => setTimeout(ok, 1500));
      await r.continue().catch(() => undefined); /* the page may have aborted it */
    });
    await page.reload();
    await expect(page.locator('.hero[aria-busy="true"] .skeleton--display')).toBeVisible();
    await expect(page.locator('.spinner')).toHaveCount(0);
    await shot(page, 'overview-loading-1440');
    await expect(page.locator('.hero[aria-busy="true"]')).toHaveCount(0, { timeout: 10_000 });
  });

  test('upload: phases to Done, then an identical copy and an edited version', async ({ page }) => {
    await signIn(page, 'admin');
    const train = csv(3_000_000, 1);
    // Slow the chunk requests a little so the Sending phase can be seen.
    await page.route('**/api/uploads/*/chunks', async (r) => {
      await new Promise((ok) => setTimeout(ok, 700));
      await r.continue().catch(() => undefined); /* the page may have aborted it */
    });
    await upload(page, 'CV_dataset_train.csv', train);
    await expect(page.getByRole('region', { name: 'Uploads', exact: true })).toBeVisible();
    await waitPhase(page, 'CV_dataset_train.csv', 'sending');
    await expect(trayRow(page, 'CV_dataset_train.csv')).toContainText(/All new\. Sending 6\d\d blocks, 2\.9\sMB/);
    await expect(page.getByTestId('recent-active')).toBeVisible(); /* in-progress row on top */
    await shot(page, 'upload-sending-1440', false);
    await waitPhase(page, 'CV_dataset_train.csv', 'done');
    await expect(trayRow(page, 'CV_dataset_train.csv')).toContainText('Stored. 2.9 MB sent, all of it new.');
    await page.unroute('**/api/uploads/*/chunks');

    await upload(page, 'copy_of_train.csv', train);
    await waitPhase(page, 'copy_of_train.csv', 'done');
    await expect(trayRow(page, 'copy_of_train.csv')).toContainText('Stored. Nothing sent: all 2.9 MB was already on the server.');

    await upload(page, 'CV_dataset_train.csv', csv(3_000_000, 1, true));
    await waitPhase(page, 'CV_dataset_train.csv', 'done');
    await expect(trayRow(page, 'CV_dataset_train.csv').first()).toContainText(/Stored\. \d+(\.\d)?\sKB sent instead of 2\.9\sMB\./);
    await upload(page, 'DBMS_notes.csv', csv(1_200_000, 5));
    await waitPhase(page, 'DBMS_notes.csv', 'done');
    await shotsAtSizes(page, 'tray-done');

    // The headline tells the truth: 3 × 3,000,000 + 18 + 1,200,000 bytes
    // uploaded (9.7 MB in the 1024-based units of formatBytes); one copy of
    // the train data plus the edit plus the notes on disk (about 4 MB).
    await expect(page.locator('#hero-title')).toHaveText(/9\.7\sMB uploaded\.\s*4(\.\d)?\sMB on disk\./, { timeout: 10_000 });
    await expect(page.getByTestId('recent-row')).toHaveCount(3); /* chain heads: v1 of the train file is folded into v2 */
  });

  test('Overview: populated, linked hover, live pulse on another screen', async ({ page, browser }) => {
    await signIn(page, 'admin');
    await expect(page.getByTestId('recent-row').first()).toBeVisible();
    await page.mouse.move(0, 0); /* no hovered row in the shots */
    await page.waitForTimeout(2600); /* entrance done */
    await shotsAtSizes(page, 'overview');

    // Hover DBMS_notes.csv: its tiles keep their shade with an outline,
    // the rest dim to 22%, and the caption names the file.
    const row = page.getByTestId('recent-row').filter({ hasText: 'DBMS_notes.csv' });
    await row.hover();
    await expect(page.getByTestId('field-caption')).toHaveText(/^Tiles holding blocks of DBMS_notes\.csv\. All new data when it arrived\.$/);
    const hit = await page.locator('.tile--hit').count();
    expect(hit).toBeGreaterThan(40); /* ~1.1 of ~4.1 MB: about a quarter of 384 */
    expect(hit).toBeLessThan(140);
    await page.waitForTimeout(450);
    const dim = await page.locator('.tile:not(.tile--hit)').first().evaluate((el) => getComputedStyle(el).opacity);
    expect(Number(dim)).toBeCloseTo(0.22, 2);
    await shot(page, 'overview-hover-1440', false);
    // Keyboard reaches the same highlight.
    await page.mouse.move(0, 0);
    await page.getByRole('link', { name: 'CV_dataset_train.csv' }).first().focus();
    await expect(page.getByTestId('field-caption')).toHaveText(/^Tiles holding blocks of CV_dataset_train\.csv\./);
    await page.keyboard.press('Tab');

    // A student watching the Overview sees tiles pulse when the admin's
    // new blocks land (anonymous newTiles), and the numbers update.
    const st = await newSignedIn(browser, 'student');
    await expect(st.page.locator('#hero-title')).toContainText('uploaded.');
    await st.page.evaluate(() => {
      (window as unknown as { __pulses: number }).__pulses = 0;
      new MutationObserver((m) => {
        for (const x of m)
          if ((x.target as Element).classList?.contains('tile--pulse')) (window as unknown as { __pulses: number }).__pulses++;
      }).observe(document.querySelector('[data-testid=block-field]')!, { subtree: true, attributes: true, attributeFilter: ['class'] });
    });
    await upload(page, 'lab3_results.csv', csv(600_000, 9));
    await waitPhase(page, 'lab3_results.csv', 'done');
    await expect.poll(() => st.page.evaluate(() => (window as unknown as { __pulses: number }).__pulses)).toBeGreaterThan(0);
    await expect(st.page.locator('#hero-title')).toHaveText(/10\sMB uploaded\./, { timeout: 10_000 }); /* + 600,000 bytes */
    await expect(st.page.getByText('lab3_results.csv')).toHaveCount(0); /* not shared: the student never sees the name */
    await shotsAtSizes(st.page, 'overview-student');
    await st.ctx.close();
  });

  test('Files: list, search, scope, drawer, SHA-256, download, share', async ({ page, browser }) => {
    await signIn(page, 'admin');
    await page.getByRole('link', { name: 'Files', exact: true }).click();
    await expect(page.getByTestId('file-row')).toHaveCount(4); /* heads of chains: v2 of the train file */
    await shotsAtSizes(page, 'files');
    await page.getByLabel('Search files').fill('zzz');
    await expect(page.getByTestId('files-empty')).toHaveText('No files match "zzz".');
    await shot(page, 'files-no-results-1440', false);
    await page.getByLabel('Search files').fill('train');
    await expect(page.getByTestId('file-row')).toHaveCount(2);
    await page.getByLabel('Search files').fill('');
    await page.getByRole('button', { name: 'Shared with lab' }).click();
    await expect(page.getByTestId('files-empty')).toHaveText('Nothing is shared with the lab yet.');
    await page.getByRole('button', { name: 'All', exact: true }).click();

    // Drawer for the edited train file (version 2).
    await page.getByTestId('file-row').filter({ hasText: 'CV_dataset_train.csv' }).locator('.row__date').click();
    const drawer = page.getByRole('dialog');
    await expect(drawer.getByRole('heading', { name: 'CV_dataset_train.csv' })).toBeVisible();
    await expect(drawer).toContainText('Version 2, uploaded by you');
    await expect(drawer.getByTestId('sha256')).toHaveText(sha(csv(3_000_000, 1, true)));
    await expect(drawer).toContainText('Content-defined, about 4 KB blocks');
    await expect(drawer.getByRole('checkbox', { name: /Version 1/ })).toBeVisible();
    await shotsAtSizes(page, 'files-drawer');
    // The download is byte-identical (the demo's verification step).
    const id = new URL(page.url()).searchParams.get('open');
    const body = await (await page.request.get(`/api/files/${id}/content`)).body();
    expect(sha(body)).toBe(sha(csv(3_000_000, 1, true)));
    // Pick both versions: "Compare these two" opens Compare.
    await drawer.getByRole('checkbox', { name: /Version 1/ }).check();
    await drawer.getByRole('checkbox', { name: /Version 2/ }).check();
    await expect(drawer.getByRole('button', { name: 'Compare these two' })).toBeVisible();
    await page.keyboard.press('Escape');
    await expect(page.getByRole('dialog')).toHaveCount(0);

    // Share DBMS_notes.csv with the lab; the student then sees it.
    const notes = page.getByTestId('file-row').filter({ hasText: 'DBMS_notes.csv' });
    await notes.hover();
    await notes.getByRole('button', { name: /More actions/ }).click();
    await expect(page.getByRole('menu')).toBeVisible();
    await shot(page, 'files-row-menu-1440', false);
    await page.getByRole('menuitemcheckbox', { name: 'Share with lab' }).click();
    await expect(page.getByText('DBMS_notes.csv is now shared with the lab.')).toBeVisible();
    const st = await newSignedIn(browser, 'student');
    await st.page.getByRole('link', { name: 'Files', exact: true }).click();
    await expect(st.page.getByTestId('file-row')).toHaveCount(1);
    await expect(st.page.getByTestId('file-row')).toContainText('Anita Rao');
    await expect(st.page.getByTestId('file-row').getByRole('button', { name: /More actions/ })).toHaveCount(0); /* read only */
    await shotsAtSizes(st.page, 'files-student');
    await st.ctx.close();
  });

  test('delete with undo, then for real after 6 s', async ({ page }) => {
    await signIn(page, 'admin');
    await page.goto('/files');
    const row = () => page.getByTestId('file-row').filter({ hasText: 'copy_of_train.csv' });
    // The whole path by keyboard: open, focus moves into the drawer; Delete
    // asks inline with focus on the confirming button; Enter deletes and
    // focus lands on Undo; Enter undoes.
    await row().locator('.row__open').focus();
    await page.keyboard.press('Enter');
    await expect(page.getByRole('dialog')).toBeFocused();
    await page.getByRole('dialog').getByRole('button', { name: 'Delete' }).click();
    await expect(page.getByText('Delete copy_of_train.csv? Blocks shared with other files are kept.')).toBeVisible();
    await expect(page.getByRole('dialog').getByRole('button', { name: 'Delete' })).toBeFocused();
    await shot(page, 'files-delete-confirm-1440', false);
    await page.keyboard.press('Enter');
    await expect(row()).toHaveCount(0);
    await expect(page.getByText('Deleted copy_of_train.csv.')).toBeVisible();
    await expect(page.getByRole('button', { name: 'Undo' })).toBeFocused();
    await shot(page, 'files-delete-toast-1440', false);
    await page.keyboard.press('Enter');
    await expect(row()).toHaveCount(1);
    await expect(page.locator('#main')).toBeFocused(); /* not left on a removed button */
    // Nothing reached the server: the file is still there.
    const list = await (await page.request.get('/api/files?q=copy_of')).json();
    expect(list.items).toHaveLength(1);
    const id = list.items[0].id as number;

    await row().locator('.row__open').click();
    await page.getByRole('dialog').getByRole('button', { name: 'Delete' }).click();
    await page.getByRole('dialog').getByRole('button', { name: 'Delete' }).click();
    await page.mouse.move(700, 100); /* the timer pauses while hovered */
    await expect(page.getByText('Deleted copy_of_train.csv.')).toHaveCount(0, { timeout: 9000 });
    await expect.poll(async () => (await page.request.get(`/api/files/${id}`)).status()).toBe(404);
  });

  test('failures: lost connection with resume, rejected block, session ended, disk full, cancel', async ({ page, browser }) => {
    await signIn(page, 'admin');
    const big = csv(6_000_000, 21);

    // 1. The connection drops during Sending; Retry resumes and sends only
    //    what the server does not have yet.
    let calls = 0;
    await page.route('**/api/uploads/*/chunks', async (r) => {
      calls++;
      if (calls === 2) return r.abort('connectionreset');
      await r.continue().catch(() => undefined); /* the page may have aborted it */
    });
    await upload(page, 'big_dataset.csv', big);
    await waitPhase(page, 'big_dataset.csv', 'failed');
    await expect(trayRow(page, 'big_dataset.csv')).toContainText('Connection lost.');
    await shotsAtSizes(page, 'tray-connection-lost');
    await page.unroute('**/api/uploads/*/chunks');
    await trayRow(page, 'big_dataset.csv').getByRole('button', { name: 'Retry' }).click();
    await waitPhase(page, 'big_dataset.csv', 'done');
    // The retry sent only what was missing, and the file still reports
    // every byte that crossed the network, in both sessions (D-041).
    await expect(trayRow(page, 'big_dataset.csv')).toContainText('Stored. 5.7 MB sent, all of it new.');
    const big1 = (await (await page.request.get('/api/files?q=big_dataset')).json()).items[0];
    expect(big1.bytesSent).toBe(big.length);

    // 2. A block whose bytes do not match its fingerprint: corrupt one byte
    //    of the chunk body on the way. The real server re-hashes and rejects.
    await page.route('**/api/uploads/*/chunks', async (r) => {
      const b = r.request().postDataBuffer();
      if (!b) return r.continue();
      const c = Buffer.from(b);
      c[c.length - 1] = c[c.length - 1]! ^ 0xff;
      await r.continue({ postData: c });
    });
    await upload(page, 'tampered.csv', csv(5000, 33));
    await waitPhase(page, 'tampered.csv', 'failed');
    await expect(trayRow(page, 'tampered.csv')).toContainText('The server rejected 1 block that did not match its fingerprint.');
    await expect(trayRow(page, 'tampered.csv').getByRole('button', { name: 'Retry' })).toBeVisible();
    await shot(page, 'tray-hash-mismatch-1440', false);
    await page.unroute('**/api/uploads/*/chunks');

    // 3. Disk full: the one state answered in the browser (D-037).
    await page.route('**/api/uploads/*/commit', (r) =>
      r.fulfill({ status: 507, contentType: 'application/json', body: '{"error":{"code":"disk_full","message":"No space."}}' }),
    );
    await upload(page, 'too_big.csv', csv(20_000, 34));
    await waitPhase(page, 'too_big.csv', 'failed');
    await expect(trayRow(page, 'too_big.csv')).toContainText('The server is out of space. Ask the lab admin.');
    await expect(trayRow(page, 'too_big.csv').getByRole('button', { name: 'Retry' })).toHaveCount(0);
    await page.unroute('**/api/uploads/*/commit');

    // 4. Cancel during Sending: the session is dropped, no file appears.
    await page.route('**/api/uploads/*/chunks', async (r) => {
      await new Promise((ok) => setTimeout(ok, 3000));
      await r.continue().catch(() => undefined);
    });
    await upload(page, 'cancel_me.csv', csv(2_000_000, 35));
    await waitPhase(page, 'cancel_me.csv', 'sending');
    await trayRow(page, 'cancel_me.csv').getByRole('button', { name: 'Cancel' }).click();
    await waitPhase(page, 'cancel_me.csv', 'cancelled');
    await page.unroute('**/api/uploads/*/chunks');
    const list = await (await page.request.get('/api/files?q=cancel_me')).json();
    expect(list.items).toHaveLength(0);

    // 5. Session ended (signed out in another tab of the same browser)
    //    while an upload waits: sign in again, the file is still there.
    await page.route('**/api/uploads/*/chunks', async (r) => {
      await new Promise((ok) => setTimeout(ok, 1500));
      await r.continue().catch(() => undefined); /* the page may have aborted it */
    });
    await upload(page, 'after_signout.csv', csv(300_000, 36));
    await waitPhase(page, 'after_signout.csv', 'sending');
    const other = await page.context().newPage();
    await other.goto('/');
    await other.getByRole('button', { name: /account menu/ }).click();
    await other.getByRole('menuitem', { name: 'Sign out' }).click();
    await expect(other.getByRole('heading', { name: 'Sign in to the lab store' })).toBeVisible();
    await other.close();
    await expect(page.getByText('Your session ended. Sign in again.')).toBeVisible({ timeout: 15_000 });
    await page.unroute('**/api/uploads/*/chunks');
    const s = readState();
    await page.getByLabel('Username').fill(s.admin.username);
    await page.getByLabel('Password', { exact: true }).fill(s.admin.password);
    await page.getByRole('button', { name: 'Sign in' }).click();
    await expect(trayRow(page, 'after_signout.csv')).toContainText('Your session ended before the upload finished.');
    await trayRow(page, 'after_signout.csv').getByRole('button', { name: 'Retry' }).click();
    await waitPhase(page, 'after_signout.csv', 'done');
    void browser;
  });

  test('drag and drop anywhere opens the tray', async ({ page }) => {
    await signIn(page, 'admin');
    await page.goto('/files');
    const dt = await page.evaluateHandle(() => {
      const d = new DataTransfer();
      d.items.add(new File(['week,score\n1,90\n2,85\n'], 'dropped.csv', { type: 'text/csv' }));
      return d;
    });
    await page.dispatchEvent('main', 'dragenter', { dataTransfer: dt });
    await expect(page.getByText('Drop to upload')).toBeVisible();
    await shot(page, 'drop-overlay-1440', false);
    await page.dispatchEvent('main', 'drop', { dataTransfer: dt });
    await expect(page.getByText('Drop to upload')).toHaveCount(0);
    await waitPhase(page, 'dropped.csv', 'done');
  });

  test('dark mode and reduced motion', async ({ browser }) => {
    const dark = await newSignedIn(browser, 'admin', { colorScheme: 'dark' });
    await dark.page.waitForTimeout(2600);
    await shot(dark.page, 'overview-dark-1440', false);
    await dark.page.goto('/files');
    await dark.page.getByTestId('file-row').first().locator('.row__open').click();
    await expect(dark.page.getByRole('dialog')).toBeVisible();
    await shot(dark.page, 'files-drawer-dark-1440', false);
    await dark.ctx.close();

    const calm = await newSignedIn(browser, 'admin', { reducedMotion: 'reduce' });
    const anim = await calm.page.locator('.tile').first().evaluate((el) => getComputedStyle(el).animationName);
    expect(anim).toBe('none');
    await calm.ctx.close();
  });
});
