import { test, expect, type Page } from '@playwright/test';
import { readFileSync } from 'node:fs';

const reference = JSON.parse(readFileSync(new URL('../generated/package.json', import.meta.url), 'utf-8'));
const bytes = Buffer.from(reference.hex, 'hex');

async function attachMock(page: Page, mode = 'normal'): Promise<void> {
  await page.addInitScript(({ metadata, mode }) => {
    let phase = 0, received = 0, total = 0, session = '00000000', installed = mode === 'update';
    let currentVersion = installed ? '0.9.0' : metadata.version;
    let digest = installed ? '0'.repeat(64) : metadata.sha256;
    let controller: ReadableStreamDefaultController<Uint8Array>;
    const commands: string[] = [];
    Object.assign(window, { mockCommands: commands });
    const status = () => ({ format: 1, allowed: 1, busy: Number(phase !== 0), phase, session, received, total,
      count: Number(installed), orphans: 0, capacity: [1, 9240576, 24576], us: [0, 0, 0, 0, 0, 0, 0],
      error: '', apps: [], name: '', sha256: '', elf_size: 0 });
    const port = {
      readable: null as ReadableStream<Uint8Array> | null,
      writable: null as WritableStream<Uint8Array> | null,
      async open() {
        if (mode === 'busy') { throw new DOMException('Port busy', 'NetworkError'); }
        this.readable = new ReadableStream<Uint8Array>({ start(c) { controller = c; } });
        this.writable = new WritableStream<Uint8Array>({ async write(data) {
          const [_, protocol, id, operation] = /^!(RP1|RO1|RS1|RE1) ([0-9a-f]{8}) (.*)$/.exec(new TextDecoder().decode(data).trim())!;
          commands.push(operation);
          let response: object; let code = 0;
          if (protocol === 'RO1') { response = { version: '0.3.8', running: 'ota_0', boot: 'ota_0', ota_state: 'VALID', phase: 'IDLE' }; }
          else if (protocol === 'RS1') { response = { version: '0.3.8', running: 'ota_0', safe: 0, trial: 0, failures: 0, persistence: 1, stage: 4, latch: 0 }; }
          else if (protocol === 'RE1') { response = { loader: '1.3.5', state: 0, busy: 0, code: 0, data: 0 }; }
          else {
            const [command, arg1, arg2, arg3] = operation.split(' ');
            if (command === 'BEGIN') { phase = 1; session = arg1; total = Number(arg2); received = 0; }
            if (command === 'DATA') {
              if (mode === 'disconnect-data') { controller.close(); return; }
              // Let the DOM process cancel and progress while a real stream write is pending.
              await new Promise(resolve => setTimeout(resolve, 35));
              received += arg3.length / 2;
            }
            if (command === 'END') { phase = 4; }
            if (command === 'INSTALL') {
              if (mode === 'reject') { code = 259; }
              else { installed = true; currentVersion = metadata.version; digest = metadata.sha256; phase = 0; received = total = 0; }
              if (mode === 'disconnect-commit') { controller.close(); return; }
            }
            if (command === 'ABORT') { phase = received = total = 0; }
            response = status();
            if (command === 'LIST' || command === 'INFO') {
              if (command === 'INFO' && !installed) { code = 261; }
              response = { ...status(), apps: installed ? [[metadata.id, currentVersion, metadata.abi, 0]] : [],
                name: installed ? metadata.name : '', sha256: installed ? digest : '', elf_size: installed ? metadata.size - 224 : 0 };
            }
          }
          controller.enqueue(new TextEncoder().encode(`log\n!${protocol} ${id} ${code} ${JSON.stringify(response)}\n`));
        } });
      },
      async setSignals() {},
      async close() {},
    };
    Object.defineProperty(navigator, 'serial', { configurable: true, value: { async requestPort() {
      if (mode === 'chooser-cancel') { throw new DOMException('Canceled', 'NotFoundError'); }
      if (mode === 'permission') { throw new DOMException('Denied', 'NotAllowedError'); }
      return port;
    } } });
  }, { metadata: reference.metadata, mode });
}
async function choose(page: Page): Promise<void> {
  await page.getByLabel('Choose RPK1 package').setInputFiles({ name: 'manager-check.rpk', mimeType: 'application/octet-stream', buffer: bytes });
  await expect(page.locator('#package-name')).toHaveText(reference.metadata.name);
}
async function connect(page: Page): Promise<void> {
  await page.getByRole('button', { name: 'Connect device' }).click();
  await expect(page.locator('#connection')).toHaveText('Connected');
}
async function install(page: Page): Promise<void> {
  await choose(page);
  await page.getByRole('checkbox').check();
  await page.getByRole('button', { name: 'Install package', exact: true }).click();
}

test('native browser capability is detected without a serial mock or permission prompt', async ({ page }) => {
  await page.goto('/');
  const capability = await page.evaluate(() => ({ secure: isSecureContext, serial: 'serial' in navigator, userAgent: navigator.userAgent }));
  expect(capability.secure).toBe(true);
  if (capability.serial) { await expect(page.locator('#connect')).toBeEnabled(); }
  else { await expect(page.locator('#connect')).toBeDisabled(); }
  await expect(page.locator('#connection')).toHaveText('Disconnected');
  console.log('NATIVE_BROWSER_CAPABILITY', JSON.stringify(capability));
});

test('unsupported browser has useful guidance, no connection prompt', async ({ page }) => {
  await page.addInitScript(() => { Object.defineProperty(navigator, 'serial', { configurable: true, value: undefined }); delete (navigator as unknown as { serial?: unknown }).serial; delete (Navigator.prototype as unknown as { serial?: unknown }).serial; });
  await page.goto('/');
  await expect(page.locator('#capability')).toContainText('unavailable');
  await expect(page.getByRole('button', { name: 'Connect device' })).toBeDisabled();
});
test('chooser cancellation and busy port produce specific errors', async ({ page }) => {
  await attachMock(page, 'chooser-cancel'); await page.goto('/');
  await page.getByRole('button', { name: 'Connect device' }).click();
  await expect(page.locator('#feedback')).toContainText('selection canceled');
});
test('port held by another process gives actionable message', async ({ page }) => {
  await attachMock(page, 'busy'); await page.goto('/');
  await page.getByRole('button', { name: 'Connect device' }).click();
  await expect(page.locator('#feedback')).toContainText('Close other monitors');
});
test('permission denial is distinguished from a device failure', async ({ page }) => {
  await attachMock(page, 'permission'); await page.goto('/');
  await page.getByRole('button', { name: 'Connect device' }).click();
  await expect(page.locator('#feedback')).toContainText('permission denied');
});
test('connect, read status, disconnect, reconnect; package list and update preview', async ({ page }) => {
  await attachMock(page, 'update'); await page.goto('/'); await connect(page);
  await expect(page.locator('#device-details')).toContainText('0.3.8');
  await expect(page.locator('#apps')).toContainText('org.roundie.managercheck');
  await choose(page); await expect(page.locator('#plan')).toContainText('Update');
  await page.getByRole('button', { name: 'Disconnect', exact: true }).click();
  await expect(page.locator('#connection')).toHaveText('Disconnected');
  await connect(page);
});
test('new install requires confirmation and verifies before success', async ({ page }) => {
  const errors: string[] = []; page.on('pageerror', error => errors.push(error.message));
  await attachMock(page); await page.goto('/'); await connect(page); await choose(page);
  await expect(page.locator('#plan')).toContainText('New installation');
  await expect(page.locator('#install')).toBeDisabled();
  await page.getByRole('checkbox').check(); await page.locator('#install').click();
  await expect(page.locator('#activity-title')).toHaveText('Installation verified');
  await expect(page.locator('#byte-progress')).toContainText('committed and verified');
  await expect(page.locator('#apps')).toContainText(reference.metadata.id);
  expect(errors).toEqual([]);
});
test('drag and drop safely inspects real RPK1 and keeps markup as text', async ({ page }) => {
  await attachMock(page); await page.goto('/');
  await page.locator('#dropzone').evaluate((zone, hex) => {
    const transfer = new DataTransfer();
    transfer.items.add(new File([new Uint8Array(hex.match(/../g)!.map((value: string) => parseInt(value, 16)))], '<img src=x onerror=alert(1)>.rpk'));
    zone.dispatchEvent(new DragEvent('drop', { bubbles: true, dataTransfer: transfer }));
  }, reference.hex);
  await expect(page.locator('#package-name')).toHaveText(reference.metadata.name);
  await expect(page.locator('#filename')).toHaveText('<img src=x onerror=alert(1)>.rpk');
  await expect(page.locator('#filename img')).toHaveCount(0);
});
test('progress is acknowledged bytes; cancellation aborts and controls recover', async ({ page }) => {
  await attachMock(page); await page.goto('/'); await connect(page); await install(page);
  await expect(page.locator('#cancel')).toBeVisible();
  await page.locator('#cancel').click();
  await expect(page.locator('#activity-title')).toHaveText('Transfer canceled');
  await expect(page.locator('#feedback')).toContainText('cleanup confirmed');
  await expect(page.locator('#refresh')).toBeEnabled();
});
for (const mode of ['disconnect-data', 'disconnect-commit']) {
  test(`${mode} never displays false success and reconnect reconciles`, async ({ page }) => {
    await attachMock(page, mode); await page.goto('/'); await connect(page); await install(page);
    await expect(page.locator('#activity-title')).toHaveText('Result needs reconciliation');
    await expect(page.locator('#feedback')).toContainText(mode === 'disconnect-data' ? 'cleanup is unverified' : 'outcome unknown');
    if (mode === 'disconnect-commit') {
      await connect(page);
      await expect(page.locator('#feedback')).toContainText('verified after reconnect');
    }
  });
}
test('invalid file reports validation failure without writes', async ({ page }) => {
  await attachMock(page); await page.goto('/');
  await page.getByLabel('Choose RPK1 package').setInputFiles({ name: 'bad.rpk', mimeType: 'application/octet-stream', buffer: Buffer.alloc(300) });
  await expect(page.locator('#feedback')).toContainText('Invalid or unsupported');
  await expect(page.locator('#preview')).toBeHidden();
});
test('dark/light theme, narrow layout and keyboard focus', async ({ page }) => {
  await attachMock(page); await page.setViewportSize({ width: 390, height: 844 }); await page.goto('/');
  await page.getByLabel('Switch color theme').click();
  await expect(page.locator('main')).toHaveCSS('display', 'grid');
  await expect(page.locator('body')).toHaveCSS('font-family', 'system-ui, sans-serif');
  await expect(page.locator('html')).toHaveAttribute('data-theme', /light|dark/);
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  await page.keyboard.press('Tab');
  await expect(page.getByRole('button', { name: 'Connect device' })).toBeFocused();
  await page.screenshot({ path: 'test-results/manager-mobile.png', fullPage: true });
});
test('desktop styles are loaded in light and dark mode', async ({ page }) => {
  await attachMock(page, 'update'); await page.setViewportSize({ width: 1280, height: 1000 });
  await page.emulateMedia({ colorScheme: 'light' }); await page.goto('/'); await connect(page); await choose(page);
  await expect(page.locator('main')).toHaveCSS('display', 'grid');
  await page.screenshot({ path: 'test-results/manager-desktop-light.png', fullPage: true });
  await page.getByLabel('Switch color theme').click();
  await expect(page.locator('html')).toHaveAttribute('data-theme', 'dark');
  await page.screenshot({ path: 'test-results/manager-desktop-dark.png', fullPage: true });
});
test('maximum-size hashing timing and browser heap sample (software only)', async ({ page }) => {
  await attachMock(page); await page.goto('/');
  const measurement = await page.evaluate(async hex => {
    const original = new Uint8Array(hex.match(/../g)!.map((value: string) => parseInt(value, 16)));
    const pkg = new Uint8Array(65760); pkg.set(original);
    const view = new DataView(pkg.buffer); view.setUint32(8, pkg.length, true); view.setUint32(16, 65536, true);
    pkg.set(new Uint8Array(await crypto.subtle.digest('SHA-256', pkg.subarray(224))), 24);
    const memory = () => (performance as unknown as { memory?: { usedJSHeapSize: number } }).memory?.usedJSHeapSize ?? null;
    // Dynamic import exercises the actual production inspector in a real browser.
    const moduleUrl = '/src/package.ts';
    const { inspectFile } = await import(/* @vite-ignore */ moduleUrl);
    const before = memory(); const started = performance.now();
    const result = await inspectFile(new File([pkg], 'maximum.rpk'));
    return { size: result.size, durationMs: performance.now() - started, hashMs: result.hashMs, before, after: memory(), userAgent: navigator.userAgent };
  }, reference.hex);
  expect(measurement.size).toBe(65760);
  console.log('BROWSER_HASH_MEASUREMENT', JSON.stringify(measurement));
});
