import { defineConfig } from '@playwright/test';

export default defineConfig({
  testDir: './tests/browser',
  fullyParallel: false,
  workers: 1,
  use: { baseURL: 'http://127.0.0.1:5173', browserName: 'chromium', headless: true,
    launchOptions: { args: ['--enable-precise-memory-info'] } },
  webServer: { command: process.env.MANAGER_PREVIEW ? 'npm run preview -- --port 5173 --strictPort' : 'npm run dev -- --port 5173 --strictPort', url: 'http://127.0.0.1:5173', reuseExistingServer: false },
});
