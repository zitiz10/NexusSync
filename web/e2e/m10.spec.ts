// M10: Compare versions against a real nexusd (DESIGN §8.5). The test
// uploads its own file and an edited copy, so it does not depend on M9's
// data; it runs after m09.spec.ts on the shared server (name order).
import { createHash } from 'node:crypto';
import { expect, test, type Page } from '@playwright/test';
import { joinCode, readState } from './server';

const DIR = 'screenshots/m10';
const SIZES = [
  { name: '1440', width: 1440, height: 900 },
  { name: '1366', width: 1366, height: 768 },
  { name: '390', width: 390, height: 844 },
] as const;

async function shotsAtSizes(page: Page, name: string) {
  for (const s of SIZES) {
    await page.setViewportSize({ width: s.width, height: s.height });
    await page.waitForTimeout(250);
    await page.screenshot({ path: `${DIR}/${name}-${s.name}.png`, animations: 'disabled' });
  }
  await page.setViewportSize({ width: 1440, height: 900 });
}
const shot = (page: Page, name: string) => page.screenshot({ path: `${DIR}/${name}.png`, animations: 'disabled' });

/** CSV-like rows; `edit` inserts one line in the middle (shifts everything after it). */
function csv(len: number, seed: number, edit = false): Buffer {
  let x = seed >>> 0;
  const r = () => ((x = (Math.imul(x, 1664525) + 1013904223) >>> 0), x);
  let s = '';
  while (s.length < len) s += `${r() % 100000},${r() % 977},lab-${r() % 50},${(r() % 10000) / 100}\n`;
  s = s.slice(0, len);
  if (edit) s = `${s.slice(0, len / 2)}corrected,value,here\n${s.slice(len / 2)}`;
  return Buffer.from(s);
}

async function signIn(page: Page, username: string, password: string) {
  await page.goto('/signin');
  await page.getByLabel('Username').fill(username);
  await page.getByLabel('Password', { exact: true }).fill(password);
  await page.getByRole('button', { name: 'Sign in' }).click();
  await expect(page.getByRole('navigation', { name: 'Main' })).toBeVisible();
}
const admin = (page: Page) => signIn(page, readState().admin.username, readState().admin.password);

const strips = (page: Page) => ({
  a: page.getByTestId('cmp-a-block'),
  b: page.getByTestId('cmp-b-block'),
});

test.describe.configure({ mode: 'serial' });
test.use({ viewport: { width: 1440, height: 900 } });

test.describe('M10 Compare', () => {
  test('empty state for someone with fewer than two files', async ({ page, request }) => {
    const s = readState();
    const who = { username: 'meera', displayName: 'Meera Iyer', password: 'student-password-9' };
    const r = await request.post('/api/auth/join', {
      headers: { 'X-NexusSync': '1' },
      data: { ...who, joinCode: joinCode(s.dir) },
    });
    expect([201, 409]).toContain(r.status());
    await signIn(page, who.username, who.password);
    await page.getByRole('link', { name: 'Compare', exact: true }).click();
    await expect(page.getByTestId('compare-empty')).toHaveText('Upload a file and then an edited copy to compare them here.');
    await shotsAtSizes(page, 'compare-empty');
  });

  test('defaults: the newest file with a previous version, in fixed 4 KB mode', async ({ page }) => {
    await admin(page);
    for (const [buf, label] of [
      [csv(1_000_000, 77), 'v1'],
      [csv(1_000_000, 77, true), 'v2'],
    ] as const) {
      await page.setInputFiles('[data-testid=upload-input]', { name: 'lab_report.csv', mimeType: 'text/csv', buffer: buf });
      await expect(page.getByTestId('tray-row').first()).toHaveAttribute('data-phase', 'done', { timeout: 30_000 });
      void label;
    }
    await page.getByRole('link', { name: 'Compare', exact: true }).click();
    await expect(page.getByRole('button', { name: 'Fixed 4 KB blocks' })).toHaveAttribute('aria-pressed', 'true');
    await expect(page.locator('#pick-version-1')).toHaveValue(/\d+/);
    await expect(page.locator('#pick-version-1 option:checked')).toHaveText(/lab_report\.csv v1/);
    await expect(page.locator('#pick-version-2 option:checked')).toHaveText(/lab_report\.csv v2/);
    // Fixed blocks: everything after the inserted line has moved.
    const summary = page.getByTestId('cmp-summary');
    await expect(summary).toContainText(/^122 of 245 blocks match version 1\./);
    await expect(summary).toContainText('Every block is exactly 4 KB.');
    await expect(page.getByRole('list', { name: 'Block colours' })).toContainText('Matches version 1');
    await expect(page.getByRole('list', { name: 'Block colours' })).not.toContainText('stored');
    await expect(strips(page).b).toHaveCount(32);
    // The window starts 3 blocks before the first difference (block 123).
    await expect(page.getByText('Showing blocks 120–151 of 245')).toBeVisible();
    await page.waitForTimeout(3700); /* entrance finished */
    await shotsAtSizes(page, 'compare-fixed');
  });

  test('content-defined: almost everything is already stored; hover finds partners', async ({ page }) => {
    await admin(page);
    await page.goto('/compare');
    await expect(page.getByTestId('cmp-summary')).toContainText('match version 1');
    await page.getByRole('button', { name: 'Content-defined' }).click();
    await expect(page).toHaveURL(/mode=cdc/);
    const summary = page.getByTestId('cmp-summary');
    await expect(summary).toContainText(/(\d{3}) of (\d{3}) blocks were already stored in version 1\./);
    const text = (await summary.textContent()) ?? '';
    const [, reused, total] = /(\d+) of (\d+) blocks were/.exec(text) ?? [];
    expect(Number(total) - Number(reused)).toBeLessThanOrEqual(3);
    await expect(summary).toContainText(/New in version 2: \d+(\.\d)?\sKB of 977\sKB\./);
    await expect(summary).toContainText('Boundaries are chosen by a rolling hash');
    // The entrance replays on a mode change (and only then).
    await expect(page.locator('.strips--enter')).toHaveCount(1);
    await expect(page.locator('.strips--enter')).toHaveCount(0, { timeout: 5000 });
    await shotsAtSizes(page, 'compare-cdc');

    // Hover a reused version 2 block: its partner in version 1 lights up.
    const reusedBlock = page.locator('[data-testid=cmp-b-block].blk--reused').first();
    await reusedBlock.hover();
    await expect(page.getByTestId('cmp-v1-label')).toHaveText(/^Matches version 1 block \d+/);
    await expect(page.locator('.blk--v2.blk--hover')).toHaveCount(1);
    await expect(page.locator('.blk--v1.blk--partner')).toHaveCount(1);
    await expect(page.locator('.blk--v1.blk--dim')).toHaveCount(31);
    await shot(page, 'compare-hover-reused-1440');
    // A new block has no partner: all of version 1 dims.
    await page.locator('[data-testid=cmp-b-block].blk--new').first().hover();
    await expect(page.getByTestId('cmp-v1-label')).toHaveText('No matching block');
    await expect(page.locator('.blk--v1.blk--partner')).toHaveCount(0);
    await expect(page.locator('.blk--v1.blk--dim')).toHaveCount(32);
    await shot(page, 'compare-hover-new-1440');

    // Click selects: the details panel shows where the block is stored.
    await reusedBlock.click();
    const details = page.getByTestId('cmp-details');
    await expect(details).toContainText(/Block \d+ of \d+/);
    await expect(details).toContainText(/Already stored: same as version 1 block \d+/);
    await expect(details).toContainText(/Container \d+, offset [\d,]+/);
    await expect(details).toContainText('2 times across stored files');
    const full = await details.locator('.mono').getAttribute('title');
    expect(full).toMatch(/^[0-9a-f]{64}$/);
    await expect(details.locator('.mono')).toHaveText(`${full!.slice(0, 16)}…${full!.slice(-8)}`);
    await shotsAtSizes(page, 'compare-details');
  });

  test('fixed mode details say the blocks are calculated, not stored', async ({ page }) => {
    await admin(page);
    await page.goto('/compare');
    const first = page.getByTestId('cmp-b-block').first();
    await first.click();
    await expect(page.getByTestId('cmp-details')).toContainText(
      'Not stored. NexusSync stores content-defined blocks; this fixed-size view is calculated for comparison.',
    );
    // The fingerprint is the real SHA-256 of that 4 KB slice of version 2.
    const index = Number(await first.getAttribute('data-index'));
    const v2 = csv(1_000_000, 77, true);
    const want = createHash('sha256').update(v2.subarray(index * 4096, index * 4096 + 4096)).digest('hex');
    await expect(page.getByTestId('cmp-details').locator('.mono')).toHaveAttribute('title', want);
  });

  test('minimap and keyboard move the window; arrows walk the blocks', async ({ page }) => {
    await admin(page);
    await page.goto('/compare?mode=cdc');
    const map = page.getByTestId('minimap');
    await expect(map).toBeVisible();
    const box = (await map.boundingBox())!;
    await page.mouse.click(box.x + box.width - 2, box.y + box.height / 2);
    await expect(page.getByText('Showing blocks 180–211 of 211')).toBeVisible(); /* clicked the end: last full window */
    await map.focus();
    await page.keyboard.press('Home');
    await expect(page.getByText(/^Showing blocks 1–32 of/)).toBeVisible();
    await page.keyboard.press('End');
    await expect(map).toHaveAttribute('aria-valuetext', /to (\d+) of \1$/);
    // No entrance on a window move.
    await expect(page.locator('.strips--enter')).toHaveCount(0);
    // Keyboard: Tab into version 2, arrows select blocks.
    await page.getByTestId('cmp-b-block').first().focus();
    await page.keyboard.press('ArrowRight');
    await expect(page.getByTestId('cmp-details')).toContainText(/Block \d+ of/);
    await expect(page.getByText(/Block \d+ of version 2 selected/)).toBeVisible();
    await page.keyboard.press('Escape'); /* clears the selection */
    await expect(page.getByText('Select a block', { exact: true })).toBeVisible();
    // After the window moves, each strip still has exactly one Tab stop.
    await page.getByRole('button', { name: /^Version 2, block/ }).first().click();
    await map.focus();
    await page.keyboard.press('Home');
    await expect(page.getByTestId('strip-b').locator('button[tabindex="0"]')).toHaveCount(1);
    await expect(page.getByTestId('strip-a').locator('button[tabindex="0"]')).toHaveCount(1);
  });

  test('opened from the Files drawer with two versions; unknown ids explain themselves', async ({ page }) => {
    await admin(page);
    await page.goto('/files?q=lab_report');
    await page.getByTestId('file-row').first().locator('.row__open').click();
    const drawer = page.getByRole('dialog');
    await drawer.getByRole('checkbox', { name: /Version 1/ }).check();
    await drawer.getByRole('checkbox', { name: /Version 2/ }).check();
    await drawer.getByRole('button', { name: 'Compare these two' }).click();
    await expect(page).toHaveURL(/\/compare\?a=\d+&b=\d+/);
    await expect(page.locator('#pick-version-2 option:checked')).toHaveText(/lab_report\.csv v2/);
    await expect(page.getByTestId('cmp-summary')).toContainText('match version 1');

    await page.goto('/compare?a=99998&b=99999');
    await expect(page.getByText('One of these files no longer exists, or is not shared with you.')).toBeVisible();
    await expect(page.getByRole('button', { name: 'Retry' })).toBeVisible();
    await expect(page.locator('.cmp-panel')).toHaveCount(0); /* no skeleton stuck under the error */
    await shot(page, 'compare-error-1440');
  });

  test('dark mode and reduced motion', async ({ browser }) => {
    const ctx = await browser.newContext({ viewport: { width: 1440, height: 900 }, colorScheme: 'dark', reducedMotion: 'reduce' });
    const page = await ctx.newPage();
    await admin(page);
    await page.goto('/compare?mode=cdc');
    await expect(page.getByTestId('cmp-b-block').first()).toBeVisible();
    expect(await page.locator('.strips--enter').count()).toBe(0); /* reduced motion: no entrance */
    await page.locator('[data-testid=cmp-b-block].blk--reused').first().hover();
    await shot(page, 'compare-dark-1440');
    await ctx.close();
  });
});
