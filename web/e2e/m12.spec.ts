// M12: cleanup, compaction, and the integrity scan on the Engine screen,
// against a real nexusd (DESIGN §8.6, SPEC §10, D-048). Runs after m11
// (name order). It restarts the server with gc_grace_s = 0 and 1 MB
// containers, so a deleted file's blocks are cleanup work at once and
// compaction has sealed containers to rewrite.
//
// Real: the cleanup run and its numbers, the scan and its progress events,
// a byte flipped on disk and found, the files named, the result after a
// reload. Answered in the browser (D-037): the "running" screenshot and the
// 409 note, because a scan of a few MB finishes in milliseconds and two
// jobs never overlap long enough to click.
import { expect, test, type Page } from '@playwright/test';
import { openSync, readFileSync, readSync, closeSync, writeFileSync, writeSync } from 'node:fs';
import { join } from 'node:path';
import { killServer, readState, restartServer } from './server';

const DIR = 'screenshots/m12';
const SIZES = [
  { name: '1440', width: 1440, height: 900 },
  { name: '1366', width: 1366, height: 768 },
  { name: '390', width: 390, height: 844 },
] as const;
const CSRF = { 'X-NexusSync': '1' };

async function sectionShots(page: Page, name: string, heading: string) {
  for (const s of SIZES) {
    await page.setViewportSize({ width: s.width, height: s.height });
    await page.waitForTimeout(250);
    const sec = page.locator('section', { has: page.getByRole('heading', { name: heading, level: 2 }) });
    await sec.scrollIntoViewIfNeeded();
    await sec.screenshot({ path: `${DIR}/${name}-${s.name}.png`, animations: 'disabled' });
  }
  await page.setViewportSize({ width: 1440, height: 900 });
}

async function signIn(page: Page) {
  const { admin } = readState();
  await page.goto('/signin');
  await page.getByLabel('Username').fill(admin.username);
  await page.getByLabel('Password', { exact: true }).fill(admin.password);
  await page.getByRole('button', { name: 'Sign in' }).click();
  await expect(page.getByRole('navigation', { name: 'Main' })).toBeVisible();
}

/** Text-like content from a seed, so CDC cuts it like real lab data. */
function csv(seed: number, bytes: number): Buffer {
  let x = seed >>> 0 || 1;
  const next = () => {
    x ^= x << 13;
    x >>>= 0;
    x ^= x >>> 17;
    x ^= x << 5;
    x >>>= 0;
    return x;
  };
  const rows: string[] = ['id,group,score,note'];
  let n = rows[0]!.length + 1;
  while (n < bytes) {
    const r = `${next() % 100000},${next() % 977},${(next() % 100000) / 7},sample-${next().toString(36)}`;
    rows.push(r);
    n += r.length + 1;
  }
  return Buffer.from(rows.join('\n').slice(0, bytes));
}

async function uploadAndWait(page: Page, name: string, data: Buffer) {
  await page.setInputFiles('[data-testid=upload-input]', { name, mimeType: 'text/csv', buffer: data });
  await expect(page.getByTestId('tray-row').filter({ hasText: name })).toHaveAttribute('data-phase', 'done', {
    timeout: 30_000,
  });
}

async function fileId(page: Page, name: string): Promise<number> {
  const r = await page.request.get(`/api/files?q=${encodeURIComponent(name)}`);
  const items = ((await r.json()) as { items: { id: number; name: string }[] }).items;
  const f = items.find((i) => i.name === name);
  if (!f) throw new Error(`no file ${name}`);
  return f.id;
}

const engine = async (page: Page) => {
  await page.getByRole('link', { name: 'Engine', exact: true }).click();
  await expect(page.getByRole('heading', { name: 'Engine', level: 1 })).toBeVisible();
};
const kvValue = (page: Page, key: string) => page.locator('.kv__row', { has: page.getByText(key, { exact: true }) }).locator('dd');

test.describe.configure({ mode: 'serial' });
test.use({ viewport: { width: 1440, height: 900 } });

test.describe('M12', () => {
  test.beforeAll(async () => {
    const s = readState();
    const conf = readFileSync(s.conf, 'utf8');
    if (!conf.includes('gc_grace_s')) {
      writeFileSync(s.conf, `${conf}gc_grace_s = 0\ncontainer_size = 1048576\n`);
      killServer(s);
      await restartServer();
    }
  });

  test('cleanup: a deleted file comes back as free space, with the result in words', async ({ page }) => {
    await signIn(page);
    // About 4.4 MB in 1 MB containers, then delete three of the four files.
    for (let i = 0; i < 4; i++) await uploadAndWait(page, `m12-week${i + 1}.csv`, csv(100 + i, 1_100_000));
    for (let i = 0; i < 3; i++) {
      const id = await fileId(page, `m12-week${i + 1}.csv`);
      const r = await page.request.delete(`/api/files/${id}`, { headers: CSRF });
      expect(r.status()).toBe(204);
    }
    await page.getByRole('button', { name: 'Close the upload tray' }).click().catch(() => undefined);
    await engine(page);
    await expect(kvValue(page, 'Waiting for cleanup')).not.toHaveText('0 bytes', { timeout: 10_000 });
    await expect(kvValue(page, 'Last cleanup')).toHaveText('None since the server started');
    await expect(kvValue(page, 'Next nightly cleanup')).toHaveText(/^[A-Z][a-z]+day \d{1,2} [A-Z][a-z]+ at 02:00$/);
    const before = await page.request.get('/api/stats').then((r) => r.json() as Promise<{ containers: { bytes: number } }>);
    await sectionShots(page, 'containers-before', 'Containers');

    await page.getByTestId('run-cleanup').click();
    const status = page.getByTestId('gc-status');
    await expect(status).toHaveText(/^Cleanup reclaimed [\d.]+ (KB|MB): [\d,]+ unused blocks freed, \d+ containers? compacted(, [\d,]+ blocks? moved)?\.$/, {
      timeout: 20_000,
    });
    await expect(page.getByTestId('run-cleanup')).toHaveText('Run cleanup');
    await expect(kvValue(page, 'Last cleanup')).toHaveText(/^(just now|\d+ s ago)$/, { timeout: 10_000 });
    await expect(kvValue(page, 'Waiting for cleanup')).toHaveText('0 bytes');
    await expect(page.getByTestId('event-log')).toContainText('engine.gc');
    await expect(page.getByTestId('event-log')).toContainText('phase=done trigger=admin blocksFreed=');
    await expect(page.getByTestId('event-log')).not.toContainText('phase=compacting');
    const after = await page.request.get('/api/stats').then((r) => r.json() as Promise<{ containers: { bytes: number } }>);
    // The three deleted files were 3.3 MB of unique data; earlier specs' data stays.
    expect(before.containers.bytes - after.containers.bytes).toBeGreaterThan(3_000_000);
    // The file that stayed still downloads byte for byte.
    const keep = await fileId(page, 'm12-week4.csv');
    const body = await page.request.get(`/api/files/${keep}/content`).then((r) => r.body());
    expect(body.equals(csv(103, 1_100_000))).toBe(true);
    await sectionShots(page, 'containers-after', 'Containers');

    // A second run has nothing to do, and says so.
    await page.getByTestId('run-cleanup').click();
    await expect(status).toHaveText('Nothing to clean up. Every stored block is in use.', { timeout: 20_000 });
  });

  test('scan: clean, then one flipped byte on disk is found and both files are named', async ({ page }) => {
    await signIn(page);
    const base = csv(7, 200_000);
    await uploadAndWait(page, 'm12-lab.csv', base);
    await uploadAndWait(page, 'm12-lab-copy.csv', Buffer.concat([base, Buffer.from('\n9,9,9,appended\n')]));
    await page.getByRole('button', { name: 'Close the upload tray' }).click().catch(() => undefined);
    await engine(page);

    // Progress events arrive over the socket at most once per 1%, up to 100%.
    const percents: number[] = [];
    page.on('websocket', (ws) =>
      ws.on('framereceived', (f) => {
        const text = typeof f.payload === 'string' ? f.payload : f.payload.toString('utf8');
        if (!text.includes('"type":"engine.scan"')) return;
        const ev = JSON.parse(text) as { data: { phase: string; percent?: number } };
        if (ev.data.phase === 'scanning' && typeof ev.data.percent === 'number') percents.push(ev.data.percent);
      }),
    );
    await page.reload();
    await expect(page.getByRole('heading', { name: 'Integrity', level: 2 })).toBeVisible();
    await expect(page.locator('.status--live')).toBeVisible({ timeout: 15_000 });

    await page.getByTestId('scan-now').click();
    const status = page.getByTestId('scan-status');
    await expect(status).toHaveText(/^Scan finished in under a second: [\d,]+ blocks verified, no damage found\.$/, {
      timeout: 20_000,
    });
    expect(percents.length).toBeGreaterThan(1);
    expect(percents.at(-1)).toBe(100);
    expect(percents.every((p, i) => i === 0 || p >= percents[i - 1]!)).toBe(true);
    await expect(kvValue(page, 'Failures')).toHaveText('0');
    await expect(kvValue(page, 'Failures').locator('.state-dot--ok')).toBeVisible();
    await sectionShots(page, 'integrity-clean', 'Integrity');

    // Flip one byte inside the first block, which both files share.
    const a = await fileId(page, 'm12-lab.csv');
    const cmp = (await page.request
      .get(`/api/compare?a=${a}&b=${a}&mode=cdc&from=0&count=1`)
      .then((r) => r.json())) as { window: { a: { stored: { containerId: number; offset: number } | null }[] } };
    const loc = cmp.window.a[0]!.stored!;
    const path = join(readState().dir, 'data', 'containers', `c${String(loc.containerId).padStart(8, '0')}.dat`);
    const flip = () => {
      const fd = openSync(path, 'r+');
      const b = Buffer.alloc(1);
      readSync(fd, b, 0, 1, loc.offset + 40 + 21);
      b[0] = b[0]! ^ 0x20;
      writeSync(fd, b, 0, 1, loc.offset + 40 + 21);
      closeSync(fd);
    };
    flip();
    await page.getByTestId('scan-now').click();
    await expect(status).toHaveText(/^Scan finished in under a second: 1 damaged block found, [\d,]+ verified\. /, {
      timeout: 20_000,
    });
    const list = page.getByTestId('scan-failures');
    await expect(list).toContainText(`in container ${loc.containerId}: its bytes changed on disk (checksum mismatch).`);
    await expect(list).toContainText(/Used by m12-lab(-copy)?\.csv and m12-lab(-copy)?\.csv\./);
    await expect(kvValue(page, 'Failures')).toHaveText('1');
    await expect(kvValue(page, 'Failures').locator('.state-dot--danger')).toBeVisible();
    await expect(page.getByTestId('event-log')).toContainText('engine.corruption');
    await sectionShots(page, 'integrity-damaged', 'Integrity');

    // The result is still there after a reload (from the server's event log).
    await page.reload();
    await expect(page.getByTestId('scan-failures')).toContainText('m12-lab-copy.csv');

    // Put the byte back: the next scan is clean again.
    flip();
    await page.getByTestId('scan-now').click();
    await expect(status).toHaveText(/no damage found\.$/, { timeout: 20_000 });
    await expect(page.getByTestId('scan-failures')).toHaveCount(0);
  });

  test('scan in progress and a refused second job (answered in the browser, D-037)', async ({ page }) => {
    await signIn(page);
    await engine(page);
    await expect(page.getByTestId('scan-now')).toBeEnabled();
    // A long scan, as a big store would show it: 42%, both actions waiting.
    await page.route('**/api/stats', async (r) => {
      const res = await r.fetch();
      const j = (await res.json()) as Record<string, Record<string, unknown>>;
      j['integrity'] = { ...j['integrity'], running: true, percent: 42 };
      await r.fulfill({ response: res, json: j });
    });
    await expect(page.getByRole('progressbar', { name: 'Integrity scan progress' })).toHaveAttribute('aria-valuenow', '42', {
      timeout: 10_000,
    });
    await expect(page.getByTestId('scan-now')).toHaveText('Scan now'); /* the label stays (DESIGN §6) */
    await expect(page.getByTestId('scan-now')).toHaveAttribute('aria-busy', 'true');
    await expect(page.getByTestId('scan-status')).toHaveText('Scan started.');
    await expect(page.getByTestId('run-cleanup')).toHaveAttribute('aria-disabled', 'true');
    await expect(page.getByTestId('gc-status')).toHaveText('Available when the scan is done.');
    await sectionShots(page, 'integrity-running', 'Integrity');
    await page.unroute('**/api/stats');
    await page.reload();

    await page.route('**/api/admin/gc', (r) =>
      r.fulfill({
        status: 409,
        json: { error: { code: 'maintenance_running', message: 'Cleanup or an integrity scan is already running.' } },
      }),
    );
    await page.getByTestId('run-cleanup').click();
    await expect(page.getByTestId('gc-status')).toHaveText('Cleanup or a scan is already running. Try again when it is done.');
  });

  test('dark mode and the keyboard path through both actions', async ({ browser }) => {
    const ctx = await browser.newContext({ colorScheme: 'dark', viewport: { width: 1440, height: 900 } });
    const page = await ctx.newPage();
    await signIn(page);
    await engine(page);
    await page.getByTestId('run-cleanup').focus();
    await expect(page.getByTestId('run-cleanup')).toBeFocused();
    await page.keyboard.press('Enter');
    await expect(page.getByTestId('gc-status')).toHaveText(/^(Nothing to clean up|Cleanup (freed|reclaimed))/, { timeout: 20_000 });
    await page.getByTestId('scan-now').focus();
    await page.keyboard.press('Enter');
    await expect(page.getByTestId('scan-now')).toBeFocused(); /* focus survives the loading state */
    await expect(page.getByTestId('scan-status')).toHaveText(/no damage found\.$/, { timeout: 20_000 });
    await sectionShots(page, 'dark-containers', 'Containers');
    await sectionShots(page, 'dark-integrity', 'Integrity');
    await ctx.close();
  });
});
