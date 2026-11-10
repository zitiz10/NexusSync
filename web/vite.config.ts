import { defineConfig } from 'vitest/config';
import react from '@vitejs/plugin-react';

// Dev server on :5173 proxies the API and WebSocket to nexusd on :8080
// (CLAUDE.md "Commands"). Production: nexusd serves web/dist itself.
// NEXUS_API overrides the target, e.g. a demo server on another port.
const api = process.env.NEXUS_API ?? 'http://127.0.0.1:8080';

export default defineConfig({
  plugins: [react()],
  server: {
    port: Number(process.env.PORT ?? 5173),
    proxy: {
      '/api': api,
      '/ws': { target: api.replace(/^http/, 'ws'), ws: true },
    },
  },
  build: {
    outDir: 'dist',
    assetsDir: 'assets', // hashed files; nexusd serves /assets/ as immutable
    sourcemap: true,
  },
  test: {
    include: ['src/**/*.test.ts'],
    environment: 'node',
    globalSetup: ['test/globalSetup.ts'],
    testTimeout: 60_000,
  },
});
