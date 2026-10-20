import type { SerialTransport } from '../src/transport.ts';
import type { JsonRecord } from '../src/protocol.ts';
import { inspectMetadata, sha256, type InspectedPackage } from '../src/package.ts';
import { readFileSync } from 'node:fs';

export function packageStatus(extra: JsonRecord = {}): JsonRecord {
  return { format: 1, allowed: 1, busy: 0, phase: 0, session: '00000000', received: 0, total: 0,
    count: 0, orphans: 0, capacity: [1, 9240576, 24576], us: [0, 0, 0, 0, 0, 0, 0],
    error: '', apps: [], name: '', sha256: '', elf_size: 0, ...extra };
}
export async function fixturePackage(): Promise<InspectedPackage> {
  const data = JSON.parse(readFileSync(new URL('./generated/package.json', import.meta.url), 'utf-8'));
  const bytes = new Uint8Array(Buffer.from(data.hex, 'hex'));
  return { ...inspectMetadata(bytes), bytes, digest: await sha256(bytes), filename: 'reference.rpk', hashMs: 0 };
}
export type Handler = (protocol: string, operation: string, mock: MockTransport) => JsonRecord | null | undefined;
export class MockTransport implements SerialTransport {
  writes: string[] = [];
  closed = false;
  handler: Handler;
  fragmentSize = 1024;
  private queue: (Uint8Array | null)[] = [];
  private waiter?: (value: Uint8Array | null) => void;
  constructor(handler: Handler = () => packageStatus()) { this.handler = handler; }
  read(): Promise<Uint8Array | null> {
    if (this.queue.length) { return Promise.resolve(this.queue.shift()!); }
    if (this.closed) { return Promise.resolve(null); }
    return new Promise(resolve => { this.waiter = resolve; });
  }
  push(bytes: Uint8Array): void {
    for (let start = 0; start < bytes.length; start += this.fragmentSize) {
      const chunk = bytes.subarray(start, start + this.fragmentSize);
      if (this.waiter) { const waiter = this.waiter; this.waiter = undefined; waiter(chunk); }
      else { this.queue.push(chunk); }
    }
  }
  async write(bytes: Uint8Array): Promise<void> {
    if (this.closed) { throw new Error('Disconnected'); }
    const line = new TextDecoder().decode(bytes).trim();
    this.writes.push(line);
    const match = /^!(RP1|RS1|RO1|RE1) ([0-9a-f]{8}) (.*)$/.exec(line)!;
    const result = this.handler(match[1], match[3], this);
    if (result) {
      const { responseCode = 0, ...status } = result;
      this.push(new TextEncoder().encode(`diagnostic log\n!${match[1]} ${match[2]} ${String(responseCode)} ${JSON.stringify(status)}\n`));
    }
  }
  async close(): Promise<void> {
    this.closed = true;
    this.queue = [];
    this.waiter?.(null);
    this.waiter = undefined;
  }
}

export class PackageDevice {
  pkg: InspectedPackage;
  installed = false;
  previous = false;
  received = 0;
  total = 0;
  session = '00000000';
  phase = 0;
  loaded = false;
  safe = false;
  storage = 9240576;
  reject = '';
  onCommand?: Handler;
  constructor(pkg: InspectedPackage) { this.pkg = pkg; }
  status(extra: JsonRecord = {}): JsonRecord {
    return packageStatus({ session: this.session, received: this.received, total: this.total,
      busy: Number(this.phase !== 0), phase: this.phase,
      capacity: [1, this.storage, 24576], count: Number(this.installed || this.previous),
      ...extra });
  }
  handle: Handler = (protocol, operation, mock) => {
    const intercepted = this.onCommand?.(protocol, operation, mock);
    if (intercepted !== undefined) { return intercepted; }
    if (protocol === 'RO1') { return { version: '0.3.8', running: 'ota_0', boot: 'ota_0', ota_state: 'VALID', phase: 'IDLE' }; }
    if (protocol === 'RS1') { return { version: '0.3.8', running: 'ota_0', safe: Number(this.safe), trial: 0, failures: 0, persistence: 1, stage: 4, latch: 0 }; }
    if (protocol === 'RE1') { return { loader: '1.3.5', state: this.loaded ? 9 : 0, busy: Number(this.loaded), code: this.loaded ? 100 : 0, data: 0 }; }
    const [command, arg1, arg2, arg3] = operation.split(' ');
    if (command === this.reject) { return this.status({ responseCode: 259, error: 'package-admission' }); }
    if (command === 'LIST' || command === 'INFO') {
      const apps = this.installed || this.previous ? [[this.pkg.id, this.installed ? this.pkg.version : '0.9.0', this.pkg.abi, 0]] : [];
      if (command === 'INFO' && !apps.length) { return this.status({ responseCode: 261 }); }
      return this.status({ apps, name: this.pkg.name, sha256: this.installed ? this.pkg.digest : this.previous ? '0'.repeat(64) : '', elf_size: apps.length ? this.pkg.elfSize : 0 });
    }
    if (command === 'BEGIN') { this.session = arg1; this.total = Number(arg2); this.received = 0; this.phase = 1; }
    if (command === 'DATA') {
      if (Number(arg2) !== this.received) { return this.status({ responseCode: 260 }); }
      this.received += arg3.length / 2;
    }
    if (command === 'END') { this.phase = 4; }
    if (command === 'INSTALL') { this.installed = true; this.phase = 0; this.total = 0; this.received = 0; }
    if (command === 'ABORT') { this.phase = 0; this.total = 0; this.received = 0; }
    return this.status();
  };
}
