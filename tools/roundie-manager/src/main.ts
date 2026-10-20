import './style.css';
import { inspectFile, type InspectedPackage } from './package.ts';
import { ProtocolClient } from './protocol.ts';
import { DeploymentCoordinator, DeviceService, installationPlan, type DeploymentProgress } from './services.ts';
import { connectionError, serialCapability, WebSerialTransport, type SerialApi } from './transport.ts';

const root = document.querySelector<HTMLDivElement>('#app')!;
// Only static markup is inserted as HTML. All external metadata uses textContent.
root.innerHTML = `
  <header><a class="brand" href="./" aria-label="Roundie Manager home"><span class="logo" aria-hidden="true">r</span><span>roundie <strong>manager</strong></span></a><button id="theme" class="quiet" aria-label="Switch color theme">◐ <span>Theme</span></button></header>
  <main>
    <div class="intro"><p class="eyebrow">YOUR APPS. YOUR DEVICE.</p><h1>A little space for<br>something new.</h1><p>Manage the applications on your Roundie.<br>Connect by USB, choose a package, make it yours.</p></div>
    <section class="device panel" aria-labelledby="device-title"><div class="section-heading"><div><p class="eyebrow">01 / CONNECTION</p><h2 id="device-title">Your Roundie</h2></div><span class="badge" id="connection">Disconnected</span></div><p id="device-copy">Connect Roundie by USB to see its applications.</p><dl id="device-details"></dl><p id="admission"></p><div class="actions"><button id="connect" class="primary">Connect device <span aria-hidden="true">↗</span></button><button id="disconnect" hidden>Disconnect</button><button id="refresh" class="quiet" hidden>Refresh</button></div><p class="caption" id="capability"></p></section>
    <section class="library panel" aria-labelledby="library-title"><div class="section-heading"><div><p class="eyebrow">02 / ON YOUR DEVICE</p><h2 id="library-title">Applications <span id="app-count" class="count">—</span></h2></div></div><div id="apps"><p class="empty">Your installed applications will appear here after connecting.</p></div></section>
    <section class="installer panel" aria-labelledby="install-title"><div class="section-heading"><div><p class="eyebrow">03 / MAKE ROOM FOR MORE</p><h2 id="install-title">Install an application</h2></div><span class="file-tag">.rpk</span></div><div id="dropzone"><div class="upload-symbol" aria-hidden="true">↑</div><h3>Drop a package here</h3><p>RPK1 · up to 65,760 bytes · stays on your computer</p><label class="file-button" for="file">Choose package</label><input id="file" type="file" accept=".rpk" aria-label="Choose RPK1 package"></div><div id="preview" hidden><p id="filename" class="caption"></p><h3 id="package-name"></h3><dl id="package-details"></dl><p id="plan" class="notice"></p><p class="caption">Only install packages you trust. Native apps share the firmware address space. SHA-256 checks integrity; it is not a publisher signature. Roundie performs final package and ELF validation.</p><label class="confirmation"><input id="confirm" type="checkbox"> I trust this package and confirm the installation shown above.</label><div class="actions"><button id="install" class="primary" disabled>Install package</button><button id="clear" class="quiet">Clear</button></div></div></section>
    <section class="activity panel" aria-labelledby="activity-title"><p class="eyebrow">TRANSFER DETAILS</p><h2 id="activity-title">Ready when you are</h2><p id="feedback" role="status" aria-live="polite">Packages go directly from this browser to Roundie. No account, no cloud.</p><progress id="progress" max="100" value="0" aria-label="Acknowledged package bytes" hidden></progress><p id="byte-progress" class="caption"></p><button id="cancel" hidden>Cancel transfer</button><dl id="metrics"></dl></section>
  </main><footer><span>Round Embedded Platform / Web Serial</span><span>Local connection. Explicit installation. No automatic launch.</span></footer>`;

const el = <T extends HTMLElement = HTMLElement>(id: string): T => document.getElementById(id) as T;
const button = (id: string) => el<HTMLButtonElement>(id);
const text = (id: string, value: string) => { el(id).textContent = value; };
const capability = serialCapability(navigator, window.isSecureContext);
text('capability', capability ?? 'Choose the ESP32-S3 USB Serial/JTAG port. Close other serial monitors first.');
let device: DeviceService | undefined;
let selected: InspectedPackage | undefined;
let working = false;
let connecting = false;
let connectionGeneration = 0;
const deployment = new DeploymentCoordinator();
let lastProgress: DeploymentProgress | undefined;

function details(id: string, pairs: [string, string][]): void {
  const fragment = document.createDocumentFragment();
  for (const [label, value] of pairs) {
    const dt = document.createElement('dt');
    const dd = document.createElement('dd');
    dt.textContent = label;
    dd.textContent = value;
    fragment.append(dt, dd);
  }
  el(id).replaceChildren(fragment);
}
function feedback(message: string, error = false): void {
  text('feedback', message);
  el('feedback').classList.toggle('error', error);
}
function render(): void {
  const snapshot = device?.snapshot;
  const connected = !!device?.client.connected;
  text('connection', connected && snapshot ? 'Connected' : connecting ? 'Connecting' : 'Disconnected');
  el('connection').classList.toggle('connected', connected && !!snapshot);
  button('connect').hidden = connected;
  button('connect').disabled = !!capability || working || connecting;
  button('disconnect').hidden = !connected && !connecting;
  button('refresh').hidden = !connected;
  button('refresh').disabled = working || connecting;
  button('clear').disabled = working;
  el<HTMLInputElement>('file').disabled = working;
  el<HTMLInputElement>('confirm').disabled = working;
  const plan = selected ? installationPlan(selected, snapshot) : undefined;
  button('install').disabled = working || !connected || !plan?.allowed || !el<HTMLInputElement>('confirm').checked || !!deployment.uncertain;
  text('plan', plan ? `${plan.label}. ${plan.reason || 'Device is ready. The device rechecks admission before writing.'}` : '');
  text('admission', snapshot ? snapshot.reason || 'Ready for application installation' : '');
  text('device-copy', snapshot ? `Round Embedded Platform · RP1 format ${snapshot.packages.format}` : 'Connect Roundie by USB to see its applications.');
  details('device-details', snapshot ? [
    ['Firmware', String(snapshot.ota.version)], ['Running slot', String(snapshot.ota.running)],
    ['BootGuard', snapshot.safe.safe === 0 && snapshot.safe.trial === 0 && snapshot.safe.failures === 0 ? 'Normal / stable checks' : 'Recovery or trial'],
    ['OTA / image', `${String(snapshot.ota.phase)} / ${String(snapshot.ota.ota_state)}`],
    ['ELF runtime', snapshot.elf.state === 0 ? 'Unloaded' : `State ${String(snapshot.elf.state)}`],
    ['PackageManager', `Phase ${snapshot.packages.phase} · ${snapshot.packages.busy ? 'busy' : 'idle'}`],
    ['Free storage', snapshot.packages.capacity[0] ? `${(snapshot.packages.capacity[1] - snapshot.packages.capacity[2]).toLocaleString()} B` : 'Unavailable']
  ] : []);
  text('app-count', snapshot ? String(snapshot.apps.length) : '—');
  const apps = el('apps');
  apps.replaceChildren();
  if (!snapshot || snapshot.apps.length === 0) {
    const empty = document.createElement('p'); empty.className = 'empty';
    empty.textContent = snapshot ? (snapshot.packages.allowed ? 'No installed applications.' : 'Registry unavailable in the current device state.') : 'Your installed applications will appear here after connecting.';
    apps.append(empty);
  } else {
    for (const app of snapshot.apps) {
      const article = document.createElement('article'); article.className = 'app-row';
      const title = document.createElement('h3'); title.textContent = app.name || app.record[0];
      const id = document.createElement('p'); id.className = 'caption'; id.textContent = app.record[0];
      const meta = document.createElement('p'); meta.textContent = `v${app.record[1]} · ABI ${app.record[2]} · ${['Healthy', 'Corrupt', 'Incompatible'][app.record[3]]}`;
      const digest = document.createElement('details'); const summary = document.createElement('summary'); summary.textContent = 'Package SHA-256';
      const code = document.createElement('code'); code.textContent = app.digest || 'Not available'; digest.append(summary, code);
      article.append(title, id, meta, digest); apps.append(article);
    }
  }
}

async function reconcile(): Promise<void> {
  if (!device || !deployment.uncertain) { return; }
  const result = await deployment.reconcile(device);
  if (result.outcome === 'verified' && lastProgress) { progress({ ...lastProgress, phase: 'completed', message: result.message }); }
  else {
    if (result.outcome === 'not-installed') { text('activity-title', 'Interrupted operation reconciled'); }
    feedback(result.message, result.outcome === 'pending');
  }
}
async function refresh(showTimings = true): Promise<void> {
  if (!device || working) { return; }
  working = true; render();
  try {
    const snapshot = await device.refresh();
    await reconcile();
    if (showTimings) { details('metrics', Object.entries(snapshot.timings).map(([key, value]) => [key, `${value.toFixed(1)} ms`])); }
  } catch (error) { feedback(connectionError(error), true); }
  finally { working = false; render(); }
}

button('connect').addEventListener('click', async () => {
  if (working || connecting || capability) { return; }
  connecting = true;
  const generation = ++connectionGeneration;
  render();
  let client: ProtocolClient | undefined;
  try {
    const transport = await WebSerialTransport.connect((navigator as unknown as { serial: SerialApi }).serial);
    client = new ProtocolClient(transport);
    if (generation !== connectionGeneration) { await client.close(); return; }
    device = new DeviceService(client);
    client.onDisconnect = reason => {
      if (device?.client !== client) { return; }
      if (device) { device.snapshot = undefined; }
      feedback(reason, true); render();
    };
    await device.refresh();
    if (deployment.uncertain) { await reconcile(); }
    else { feedback('Roundie connected. Device status and installed applications checked.'); }
  } catch (error) {
    await client?.close();
    feedback(connectionError(error), true);
    if (device?.client === client) { device = undefined; }
  } finally { connecting = false; render(); }
});
button('disconnect').addEventListener('click', async () => {
  connectionGeneration++;
  deployment.cancel();
  await device?.client.close();
  if (!deployment.busy) { feedback('Disconnected. You can reconnect when ready.'); }
  render();
});
button('refresh').addEventListener('click', () => { void refresh(); });

async function choose(file?: File): Promise<void> {
  if (!file || working) { return; }
  working = true; selected = undefined;
  el('preview').hidden = true;
  el<HTMLInputElement>('confirm').checked = false;
  text('activity-title', 'Reading package');
  feedback('Inspecting RPK1 metadata and calculating SHA-256 locally…'); render();
  try {
    selected = await inspectFile(file);
    text('filename', selected.filename);
    text('package-name', selected.name);
    details('package-details', [['Package ID', selected.id], ['Version', selected.version], ['Compatibility', `ABI ${selected.abi} / API ${selected.api} / ${selected.target}`], ['Size', `${selected.size.toLocaleString()} B`], ['Package SHA-256', selected.digest]]);
    el('preview').hidden = false;
    text('activity-title', 'Review your package');
    feedback('Metadata and integrity checks passed. Review the package and confirm before installation.');
  } catch (error) { text('activity-title', 'Package could not be opened'); feedback(connectionError(error), true); }
  finally { working = false; render(); }
}
el<HTMLInputElement>('file').addEventListener('change', event => { void choose((event.target as HTMLInputElement).files?.[0]); });
el('dropzone').addEventListener('dragover', event => { event.preventDefault(); if (!working) { el('dropzone').classList.add('dragging'); } });
el('dropzone').addEventListener('dragleave', () => el('dropzone').classList.remove('dragging'));
el('dropzone').addEventListener('drop', event => {
  event.preventDefault(); el('dropzone').classList.remove('dragging');
  const files = event.dataTransfer?.files;
  if (files?.length !== 1) { feedback('Choose one package at a time.', true); return; }
  void choose(files[0]);
});
// Prevent dropping a file elsewhere from navigating away from an active transfer.
window.addEventListener('dragover', event => event.preventDefault());
window.addEventListener('drop', event => event.preventDefault());
el('confirm').addEventListener('change', render);
button('clear').addEventListener('click', () => {
  if (working) { return; }
  selected = undefined; el('preview').hidden = true; el<HTMLInputElement>('file').value = ''; render();
});

function progress(p: DeploymentProgress): void {
  lastProgress = p;
  text('activity-title', { checking: 'Checking device', preparing: 'Preparing transaction', uploading: 'Uploading', validating: 'Validating package', committing: 'Committing', verifying: 'Verifying installation', completed: 'Installation verified', canceled: 'Transfer canceled', failed: 'Installation failed', unknown: 'Result needs reconciliation' }[p.phase]);
  feedback(p.message, p.phase === 'failed' || p.phase === 'unknown');
  el<HTMLProgressElement>('progress').hidden = false;
  el<HTMLProgressElement>('progress').value = p.total ? p.acknowledged / p.total * 100 : 0;
  text('byte-progress', `${p.acknowledged.toLocaleString()} / ${p.total.toLocaleString()} B acknowledged${p.phase === 'completed' ? ' · committed and verified' : ' · transfer only; completion requires INFO verification'}`);
  button('cancel').hidden = !['checking', 'preparing', 'uploading', 'validating'].includes(p.phase);
  details('metrics', Object.entries(p.timings).map(([key, value]) => [key, `${value.toFixed(1)} ${key === 'bytesPerSecond' ? 'B/s' : 'ms'}`]));
}
deployment.onProgress = progress;
button('cancel').addEventListener('click', () => { deployment.cancel(); button('cancel').disabled = true; });
button('install').addEventListener('click', async () => {
  if (working || !device || !selected || button('install').disabled) { return; }
  working = true; button('cancel').disabled = false; render();
  try { await deployment.install(device, selected); }
  catch (error) { feedback(connectionError(error), true); }
  finally {
    working = false; el<HTMLInputElement>('confirm').checked = false; button('cancel').hidden = true;
    if (device.client.connected) { await refresh(false); }
    render();
  }
});
button('theme').addEventListener('click', () => {
  const dark = document.documentElement.dataset.theme === 'dark' || (!document.documentElement.dataset.theme && matchMedia('(prefers-color-scheme: dark)').matches);
  document.documentElement.dataset.theme = dark ? 'light' : 'dark';
});
window.addEventListener('beforeunload', event => {
  if (working || deployment.uncertain) { event.preventDefault(); }
});
render();
