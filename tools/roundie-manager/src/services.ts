import { compareVersion, hex, type InspectedPackage } from './package.ts';
import { DeviceError, integer, ProtocolClient, ProtocolError, randomId, requireCondition, type AppRecord, type JsonRecord, type PackageStatus } from './protocol.ts';

export interface InstalledApp { record: AppRecord; name: string; digest: string; elfSize: number }
export interface DeviceSnapshot {
  ota: JsonRecord; safe: JsonRecord; elf: JsonRecord; packages: PackageStatus;
  apps: InstalledApp[]; ready: boolean; reason: string; timings: Record<string, number>;
}

export class DeviceService {
  readonly client: ProtocolClient;
  snapshot?: DeviceSnapshot;
  constructor(client: ProtocolClient) { this.client = client; }

  async refresh(): Promise<DeviceSnapshot> {
    // Discard previous admission as soon as a fresh check begins.
    this.snapshot = undefined;
    const started = performance.now();
    const ota = await this.client.request('RO1', 'STATUS');
    const safe = await this.client.request('RS1', 'STATUS');
    const elf = await this.client.request('RE1', 'STATUS');
    const packages = await this.client.package('STATUS');
    // Multi-protocol REP fingerprint, not USB VID/PID or arbitrary serial text.
    // There is no authenticated identity or explicit REP product string in these protocols.
    requireCondition(typeof ota.version === 'string' && /^[\w.+-]{1,32}$/.test(ota.version) &&
      ['ota_0', 'ota_1'].includes(String(ota.running)) && ['ota_0', 'ota_1'].includes(String(ota.boot)) &&
      ['IDLE', 'RECEIVING', 'VERIFYING', 'READY', 'ACTIVATION_PENDING'].includes(String(ota.phase)) &&
      ['NEW', 'PENDING_VERIFY', 'VALID', 'INVALID', 'ABORTED', 'UNDEFINED', 'NO_METADATA'].includes(String(ota.ota_state)) &&
      integer(safe.safe, 1) && integer(safe.failures, 3) && integer(safe.trial, 1) &&
      integer(safe.persistence, 1) && integer(safe.stage, 4) && integer(safe.latch) &&
      safe.version === ota.version && safe.running === ota.running && elf.loader === '1.3.5' &&
      integer(elf.state, 11) && integer(elf.busy, 1) && integer(elf.code) && integer(elf.data),
      'Unexpected device or unsupported REP diagnostics. No package writes permitted.');
    const statusMs = performance.now() - started;
    let reason = '';
    if (safe.safe !== 0) { reason = 'Safe Mode is active.'; }
    else if (safe.stage !== 4 || safe.latch !== 0 || safe.trial !== 0 || safe.failures !== 0 || safe.persistence !== 1 || ota.ota_state !== 'VALID' || ota.running !== ota.boot) { reason = 'BootGuard / selected firmware is not stable.'; }
    else if (ota.phase !== 'IDLE') { reason = 'An OTA operation is active.'; }
    else if (elf.state !== 0 || elf.busy !== 0 || elf.code !== 0 || elf.data !== 0) { reason = 'An ELF is loaded or reserved. Unload it on Roundie before installing.'; }
    else if (!packages.allowed || packages.busy || packages.phase !== 0) { reason = 'PackageManager admission is closed or a transaction is active.'; }
    else if (!packages.capacity[0]) { reason = 'Storage capacity is unavailable.'; }
    const listStarted = performance.now();
    const apps: InstalledApp[] = [];
    // Safe Mode may reject LIST. Retain supported status diagnostics instead.
    if (packages.allowed) {
      const list = await this.client.package('LIST');
      requireCondition(list.apps.length === list.count, 'Incomplete registry response.');
      for (const record of list.apps) {
        const info = await this.client.package(`INFO ${record[0]}`);
        requireCondition(info.apps.length === 1 && info.apps[0].every((v, i) => v === record[i]), 'Registry changed while reading. Refresh before installation.');
        apps.push({ record, name: info.name, digest: info.sha256, elfSize: info.elf_size });
      }
    }
    this.snapshot = { ota, safe, elf, packages, apps, ready: !reason, reason,
      timings: { statusMs, listMs: performance.now() - listStarted, connectionMs: performance.now() - started } };
    return this.snapshot;
  }
}

export interface InstallPlan { label: string; allowed: boolean; reason: string }
export function installationPlan(pkg: InspectedPackage, snapshot?: DeviceSnapshot): InstallPlan {
  if (!snapshot) { return { label: 'Connect to check installation', allowed: false, reason: 'No verified device.' }; }
  const existing = snapshot.apps.find(app => app.record[0] === pkg.id);
  let label = 'New installation';
  let reason = snapshot.reason;
  if (existing) {
    const comparison = compareVersion(pkg.version, existing.record[1]);
    label = comparison === 0 ? 'Same-version replacement' : comparison < 0 ? 'Downgrade' : 'Update';
    if (comparison <= 0 || existing.record[3] === 1) { reason = 'PackageManager requires a healthy older installed version. Replacement is forbidden.'; }
  } else if (snapshot.packages.count + snapshot.packages.orphans >= 8) { reason = 'Registry has reached its eight-package limit (including orphans).'; }
  const [, total, used] = snapshot.packages.capacity;
  if (total - used < 256 * 1024 + pkg.size + 16384) { reason = 'Insufficient free storage including the device emergency reserve.'; }
  return { label, allowed: snapshot.ready && !reason, reason };
}

export type DeploymentPhase = 'checking' | 'preparing' | 'uploading' | 'validating' | 'committing' | 'verifying' | 'completed' | 'canceled' | 'failed' | 'unknown';
export interface DeploymentProgress {
  phase: DeploymentPhase; acknowledged: number; total: number; message: string;
  timings: Record<string, number>;
}
export interface UncertainInstall { id: string; version: string; digest: string; abi: number; elfSize: number; commitSent: boolean }
export interface Reconciliation { message: string; outcome: 'verified' | 'not-installed' | 'pending' }

export class DeploymentCoordinator {
  private active = false;
  private canceled = false;
  uncertain?: UncertainInstall;
  onProgress?: (progress: DeploymentProgress) => void;
  get busy(): boolean { return this.active; }
  cancel(): void { this.canceled = true; }

  async install(device: DeviceService, pkg: InspectedPackage): Promise<DeploymentProgress> {
    if (this.active || this.uncertain) { throw new Error('Resolve the existing operation before starting another installation.'); }
    this.active = true;
    this.canceled = false;
    const session = randomId();
    let begun = false;
    let commitSent = false;
    let acknowledged = 0;
    const timings: Record<string, number> = { hashMs: pkg.hashMs };
    const emit = (phase: DeploymentPhase, message: string): DeploymentProgress => {
      const p = { phase, message, acknowledged, total: pkg.size, timings: { ...timings } };
      this.onProgress?.(p);
      return p;
    };
    const cancelCheck = () => { if (this.canceled) { throw new Error('Installation canceled.'); } };
    try {
      emit('checking', 'Checking device and replacement policy');
      const snapshot = await device.refresh();
      const plan = installationPlan(pkg, snapshot);
      requireCondition(plan.allowed, plan.reason);
      cancelCheck();
      emit('preparing', 'Preparing transaction');
      cancelCheck();
      begun = true; // BEGIN may be accepted even if its reply is lost.
      let begin: PackageStatus;
      try { begin = await device.client.package(`BEGIN ${session} ${pkg.size} ${pkg.digest}`); }
      catch (error) { if (error instanceof DeviceError) { begun = false; } throw error; }
      requireCondition(begin.phase === 1 && begin.session === session && begin.total === pkg.size && begin.received === 0, 'Inconsistent BEGIN acknowledgement.');
      const transferStarted = performance.now();
      for (let offset = 0; offset < pkg.size;) {
        cancelCheck();
        emit('uploading', 'Uploading package');
        cancelCheck();
        const chunk = pkg.bytes.subarray(offset, offset + 512);
        const response = await device.client.package(`DATA ${session} ${offset} ${hex(chunk)}`);
        requireCondition(response.phase === 1 && response.session === session && response.total === pkg.size && response.received === offset + chunk.length, 'Inconsistent DATA acknowledgement. No chunk will be replayed.');
        offset += chunk.length;
        acknowledged = offset;
        emit('uploading', 'Uploading package');
      }
      timings.transferMs = performance.now() - transferStarted;
      timings.bytesPerSecond = pkg.size / Math.max(timings.transferMs / 1000, 0.001);
      cancelCheck();
      emit('validating', 'Validating package on Roundie (END)');
      const ready = await device.client.package(`END ${session}`);
      requireCondition(ready.phase === 4 && ready.session === session && ready.received === pkg.size, 'Package did not become READY.');
      cancelCheck();
      emit('committing', 'Committing installation (INSTALL). Cancellation cannot reverse a commit.');
      this.uncertain = { id: pkg.id, version: pkg.version, digest: pkg.digest, abi: pkg.abi, elfSize: pkg.elfSize, commitSent: true };
      commitSent = true;
      const commitStarted = performance.now();
      await device.client.package(`INSTALL ${pkg.digest}`);
      timings.commitMs = performance.now() - commitStarted;
      begun = false;
      emit('verifying', 'Verifying installed identity, version and full digest (INFO)');
      const verifyStarted = performance.now();
      const verified = await this.verify(device, this.uncertain);
      requireCondition(verified, 'Installed package does not match the intended identity/version/digest.');
      this.uncertain = undefined;
      timings.verifyMs = performance.now() - verifyStarted;
      return emit('completed', `${plan.label === 'Update' ? 'Update' : 'Installation'} verified. Application was not started.`);
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      // A lost INSTALL reply requires INFO; never replay INSTALL, never ABORT it.
      if (commitSent) {
        if (device.client.connected && this.uncertain) {
          try {
            if (await this.verify(device, this.uncertain)) {
              this.uncertain = undefined;
              return emit('completed', 'Installation independently verified after uncertain commit.');
            }
            // Even INSTALL can report a post-commit scan failure. Only a fresh
            // INFO plus IDLE state may resolve a definite rejection as failure.
            if (error instanceof DeviceError) {
              const status = await device.client.package('STATUS');
              if (status.phase === 0 && !status.busy) {
                this.uncertain = undefined;
                return emit('failed', `${message} INFO confirms the expected replacement is not installed.`);
              }
            }
          } catch { /* Preserve uncertainty for a new handshake. */ }
        }
        return emit('unknown', `${message} Commit outcome unknown. Reconnect and reconcile; do not retry installation.`);
      }
      this.uncertain = undefined;
      let cleanup = '';
      if (begun) {
        try {
          if (!device.client.connected) { throw new ProtocolError('Disconnected'); }
          const aborted = await device.client.package(`ABORT ${session}`);
          requireCondition(aborted.phase === 0 && !aborted.busy, 'ABORT did not confirm IDLE.');
          cleanup = ' Staging cleanup confirmed.';
        } catch {
          this.uncertain = { id: pkg.id, version: pkg.version, digest: pkg.digest, abi: pkg.abi, elfSize: pkg.elfSize, commitSent: false };
          return emit('unknown', `${message} Staging cleanup is unverified. Reconnect and check PackageManager; no automatic resume.`);
        }
      }
      return emit(this.canceled ? 'canceled' : 'failed', message + cleanup);
    } finally { this.active = false; }
  }

  private async verify(device: DeviceService, expected: UncertainInstall): Promise<boolean> {
    const info = await device.client.package(`INFO ${expected.id}`);
    const app = info.apps[0];
    return info.apps.length === 1 && app[0] === expected.id && app[1] === expected.version && app[2] === expected.abi &&
      app[3] === 0 && info.sha256 === expected.digest && info.elf_size === expected.elfSize;
  }

  async reconcile(device: DeviceService): Promise<Reconciliation> {
    const expected = this.uncertain;
    if (!expected) { return { message: '', outcome: 'pending' }; }
    const snapshot = await device.refresh();
    const app = snapshot.apps.find(a => a.record[0] === expected.id);
    if (expected.commitSent && app && await this.verify(device, expected)) {
      this.uncertain = undefined;
      return { message: 'Installation verified after reconnect by INFO. No write was replayed.', outcome: 'verified' };
    }
    if (!snapshot.packages.busy && snapshot.packages.phase === 0) {
      this.uncertain = undefined;
      return { message: expected.commitSent ? 'Expected package is not committed. Current registry is authoritative; the prior version, if present, is listed below.' : 'Upload was interrupted before commit. PackageManager is IDLE; current installed applications are listed below.', outcome: 'not-installed' };
    }
    return { message: 'PackageManager still has an incomplete transaction. Wait for the device timeout, then Refresh to reconcile. No upload is resumed.', outcome: 'pending' };
  }
}
