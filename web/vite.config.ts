import { defineConfig } from 'vitest/config';
import react from '@vitejs/plugin-react';

// Dev server on :5173 proxies the API and WebSocket to nexusd on :8080
// (CLAUDE.md "Commands"). Production: nexusd serves web/dist itself.
export default defineConfig({
  plugins: [react()],
  server: {
    port: 5173,
    proxy: {
      '/api': 'http://127.0.0.1:8080',
      '/ws': { target: 'ws://127.0.0.1:8080', ws: true },
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
  },
});
