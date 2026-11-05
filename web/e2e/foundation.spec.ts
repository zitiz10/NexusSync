// M8: sign in, join, shell, status, system states, at the three sizes of
// CLAUDE.md, against a real nexusd. Screenshots go to screenshots/m8/
// (git-ignored) for checking against DESIGN.md.
import { expect, test, type Page } from '@playwright/test';
import { killServer, readState, restartServer } from './server';

const SIZES = [
  { name: '1440', width: 1440, height: 900 },
  { name: '1366', width: 1366, height: 768 },
  { name: '390', width: 390, height: 844 },
] as const;

const shot = (page: Page, name: string) =>
  page.screenshot({ path: `screenshots/m8/${name}.png`, fullPage: true, animations: 'disabled' });

/** After a viewport change: let layout settle; viewport-only (full-page
 * capture right after a resize can stitch stale tiles). */
async function shotAt(page: Page, width: number, height: number, name: string) {
  await page.setViewportSize({ width, height });
  await page.waitForTimeout(200);
  await page.screenshot({ path: `screenshots/m8/${name}.png`, animations: 'disabled' });
}

async function signIn(page: Page, who: 'admin' | 'student') {
  const s = readState();
  const u = who === 'admin' ? s.admin : s.student;
  await page.goto('/signin');
  await page.getByLabel('Username').fill(u.username);
  await page.getByLabel('Password', { exact: true }).fill(u.password);
  await page.getByRole('button', { name: 'Sign in' }).click();
  await expect(page.getByRole('navigation', { name: 'Main' })).toBeVisible();
}

async function waitLive(page: Page) {
  await expect(page.locator('.status--live')).toBeVisible({ timeout: 15_000 });
}

for (const size of SIZES) {
  test.describe(`at ${size.name}`, () => {
    test.use({ viewport: { width: size.width, height: size.height } });

    test('sign in and join screens', async ({ page }) => {
      await page.goto('/signin');
      await expect(page.getByRole('heading', { name: 'Sign in to the lab store' })).toBeVisible();
      await page.evaluate(() => document.fonts.ready);
      await shot(page, `signin-${size.name}`);
      await page.getByRole('button', { name: 'Sign in' }).click();
      await expect(page.getByText('Enter your username.')).toBeVisible();
      await shot(page, `signin-errors-${size.name}`);
      await page.getByRole('link', { name: /Create an account/ }).click();
      await expect(page.getByRole('heading', { name: 'Create your account' })).toBeVisible();
      await shot(page, `join-${size.name}`);
      await page.getByRole('button', { name: 'Create account' }).click();
      await expect(page.getByText('Enter your name as others should see it.')).toBeVisible();
      await shot(page, `join-errors-${size.name}`);
    });

    test('shell for the admin', async ({ page }) => {
      await signIn(page, 'admin');
      await waitLive(page);
      await expect(page.getByRole('link', { name: 'Engine' })).toBeVisible();
      await expect(page.getByRole('link', { name: 'Overview', exact: true })).toHaveAttribute('aria-current', 'page');
      await shot(page, `shell-admin-${size.name}`);
      await page.getByRole('button', { name: /account menu/ }).click();
      await expect(page.getByRole('menu', { name: 'Account' })).toBeVisible();
      await shot(page, `user-menu-${size.name}`);
    });
  });
}

test.describe('behaviour', () => {
  test.use({ viewport: { width: 1440, height: 900 } });

  test('a student sees no admin pages', async ({ page }) => {
    await signIn(page, 'student');
    await expect(page.getByRole('link', { name: 'Engine' })).toHaveCount(0);
    await expect(page.getByRole('link', { name: 'Users' })).toHaveCount(0);
    await page.goto('/engine');
    await expect(page).toHaveURL(/\/$/); /* sent back to Overview */
    await page.getByRole('link', { name: 'Files' }).click();
    await expect(page.getByRole('heading', { name: 'Files' })).toBeVisible();
    await expect(page).toHaveTitle('Files · NexusSync');
  });

  test('wrong password, then right; the server message is shown', async ({ page }) => {
    const s = readState();
    await page.goto('/signin');
    await page.getByLabel('Username').fill(s.student.username);
    await page.getByLabel('Password', { exact: true }).fill('not-the-password');
    await page.getByRole('button', { name: 'Sign in' }).click();
    await expect(page.getByRole('alert')).toHaveText('Wrong username or password.');
    await expect(page.getByLabel('Password', { exact: true })).toBeFocused();
    await page.getByLabel('Password', { exact: true }).fill(s.student.password);
    await page.getByRole('button', { name: 'Sign in' }).click();
    await expect(page.getByRole('button', { name: /Riya Kapoor/ })).toBeVisible();
  });

  test('join: server errors land under the right field, then success', async ({ page }) => {
    await page.goto('/join');
    await page.getByLabel('Display name').fill('Kabir Singh');
    await page.getByLabel('Username').fill('riya'); /* taken */
    await page.getByLabel('Password', { exact: true }).fill('kabir-password-1');
    await page.getByLabel('Join code').fill('ABCD2345'); /* wrong */
    await page.getByRole('button', { name: 'Create account' }).click();
    await expect(page.getByText('That join code is not valid. Ask the lab admin for the current one.')).toBeVisible();
    await expect(page.getByLabel('Join code')).toBeFocused();
    const { joinCode } = await import('./server');
    await page.getByLabel('Join code').fill(joinCode(readState().dir).toLowerCase());
    await page.getByRole('button', { name: 'Create account' }).click();
    await expect(page.getByText('That username is taken. Choose another.')).toBeVisible();
    await page.getByLabel('Username').fill('kabir');
    await page.getByRole('button', { name: 'Show password' }).click();
    await expect(page.getByLabel('Password', { exact: true })).toHaveAttribute('type', 'text');
    await page.getByRole('button', { name: 'Create account' }).click();
    await expect(page.getByRole('button', { name: /Kabir Singh/ })).toBeVisible();
  });

  test('keyboard: skip link, menu with arrows and Escape', async ({ page }) => {
    await signIn(page, 'admin');
    await page.keyboard.press('Tab');
    await expect(page.getByRole('link', { name: 'Skip to content' })).toBeFocused();
    await shot(page, 'focus-skip-link-1440');
    const menuButton = page.getByRole('button', { name: /account menu/ });
    await menuButton.focus();
    await page.keyboard.press('ArrowDown');
    await expect(page.getByRole('menuitemradio', { name: 'System' })).toBeFocused();
    await page.keyboard.press('ArrowDown');
    await expect(page.getByRole('menuitem', { name: 'Sign out' })).toBeFocused();
    await page.keyboard.press('Escape');
    await expect(page.getByRole('menu')).toHaveCount(0);
    await expect(menuButton).toBeFocused();
    await shot(page, 'focus-menu-button-1440');
  });

  test('theme choice applies and survives a reload', async ({ page }) => {
    await signIn(page, 'admin');
    await page.getByRole('button', { name: /account menu/ }).click();
    await page.getByRole('menuitemradio', { name: 'Dark' }).click();
    await expect(page.locator('html')).toHaveAttribute('data-theme', 'dark');
    await page.keyboard.press('Escape');
    await waitLive(page);
    await shot(page, 'shell-dark-1440');
    await page.reload();
    await expect(page.locator('html')).toHaveAttribute('data-theme', 'dark');
    await page.getByRole('button', { name: /account menu/ }).click();
    await page.getByRole('menuitemradio', { name: 'System' }).click();
    await expect(page.locator('html')).not.toHaveAttribute('data-theme', /.+/);
  });

  test('dark system preference on the sign-in screen', async ({ browser }) => {
    const ctx = await browser.newContext({ colorScheme: 'dark', viewport: { width: 1440, height: 900 } });
    const page = await ctx.newPage();
    await page.goto(`${readState().url}/signin`);
    await page.evaluate(() => document.fonts.ready);
    await shot(page, 'signin-dark-1440');
    const bg = await page.evaluate(() => getComputedStyle(document.body).backgroundColor);
    expect(bg).toBe('rgb(16, 24, 28)'); /* --bg dark */
    await ctx.close();
  });

  test('loading: skeleton shaped like the shell, no spinner', async ({ page }) => {
    await signIn(page, 'admin');
    await page.route('**/api/me', async (r) => {
      await new Promise((ok) => setTimeout(ok, 2500));
      await r.continue();
    });
    await page.reload();
    await expect(page.locator('[aria-busy="true"]')).toBeVisible();
    await shot(page, 'loading-1440');
    await expect(page.getByRole('navigation', { name: 'Main' })).toBeVisible();
  });

  // The two states a real server cannot be held in on demand (recovery
  // takes milliseconds): /api/health is intercepted in the browser only.
  test('recovering, then recovery complete', async ({ page }) => {
    await page.route('**/api/health', (r) =>
      r.fulfill({
        status: 200,
        contentType: 'application/json',
        body: JSON.stringify({ status: 'recovering', version: '0.1.0', progress: { phase: 'replaying-log', elapsedMs: 3200 } }),
      }),
    );
    await page.goto('/signin');
    await expect(page.getByRole('heading', { name: 'The server is recovering after an unexpected stop' })).toBeVisible();
    await expect(page.getByText('Replaying the log, 3 s so far.')).toBeVisible();
    await shot(page, 'recovering-1440');
    await shotAt(page, 390, 844, 'recovering-390');
    await page.setViewportSize({ width: 1440, height: 900 });
    await page.unroute('**/api/health');
    await expect(page.getByRole('heading', { name: 'Recovery complete' })).toBeVisible();
    await shot(page, 'recovered-1440');
    await expect(page.getByRole('heading', { name: 'Sign in to the lab store' })).toBeVisible({ timeout: 10_000 });
  });

  test('unreachable before sign-in', async ({ page }) => {
    await page.route('**/api/health', (r) => r.abort('connectionrefused'));
    await page.goto('/signin');
    await expect(page.getByRole('heading', { name: 'Cannot reach the lab server' })).toBeVisible();
    await shot(page, 'unreachable-1440');
  });

  test('session ended elsewhere: back to sign-in with a note', async ({ page }) => {
    await signIn(page, 'student');
    await waitLive(page);
    await page.evaluate(() => fetch('/api/auth/logout', { method: 'POST', headers: { 'X-NexusSync': '1' } }));
    await expect(page.getByText('Your session ended. Sign in again.')).toBeVisible({ timeout: 15_000 });
    await expect(page).toHaveURL(/\/signin$/);
    await shot(page, 'session-ended-1440');
  });

  test('sign out from the menu', async ({ page }) => {
    await signIn(page, 'student');
    await page.getByRole('button', { name: /account menu/ }).click();
    await page.getByRole('menuitem', { name: 'Sign out' }).click();
    await expect(page.getByRole('heading', { name: 'Sign in to the lab store' })).toBeVisible();
    await page.goto('/files');
    await expect(page).toHaveURL(/\/signin$/);
  });

  // Last: stops the real server with SIGKILL and starts it again.
  test('offline banner, then reconnect when the server is back', async ({ page }) => {
    await signIn(page, 'admin');
    await waitLive(page);
    killServer(readState());
    await expect(page.locator('.status--reconnecting, .status--offline')).toBeVisible({ timeout: 10_000 });
    await expect(page.getByText('Lost connection to the lab server.')).toBeVisible({ timeout: 15_000 });
    await expect(page.locator('main.main--dimmed')).toBeVisible();
    await shot(page, 'offline-1440');
    await shotAt(page, 390, 844, 'offline-390');
    await page.setViewportSize({ width: 1440, height: 900 });
    // Signing out needs the server (the cookie is HttpOnly): refuse, stay signed in.
    await page.getByRole('button', { name: /account menu/ }).click();
    await page.getByRole('menuitem', { name: 'Sign out' }).click();
    await expect(page.getByText('Signing out needs the server.')).toBeVisible();
    await shot(page, 'offline-signout-refused-1440');
    await page.keyboard.press('Escape');
    await restartServer();
    await page.getByRole('button', { name: 'Retry now' }).click();
    await waitLive(page);
    await expect(page.getByText('Lost connection to the lab server.')).toHaveCount(0);
    await expect(page.getByRole('button', { name: /Anita Rao/ })).toBeVisible(); /* still signed in (D-030) */
  });
});
