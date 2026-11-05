// End-to-end tests and screenshots against a real nexusd (CLAUDE.md:
// "Run Playwright against a real nexusd, not mocks"). The global setup
// builds and starts the server on a free port with a fresh data dir.
// Uses the installed Google Chrome, so no browser download is needed.
import { defineConfig } from '@playwright/test';

export default defineConfig({
  testDir: 'e2e',
  globalSetup: './e2e/global-setup.ts',
  globalTeardown: './e2e/global-teardown.ts',
  workers: 1, // one shared server; some tests stop and restart it
  fullyParallel: false,
  timeout: 60_000,
  reporter: [['list']],
  use: {
    channel: 'chrome',
    headless: true,
    baseURL: process.env['NX_E2E_URL'],
    trace: 'retain-on-failure',
  },
});
