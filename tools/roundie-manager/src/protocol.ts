import type { SerialTransport } from './transport.ts';
import { DIGEST_PATTERN, MAX_PACKAGE, validId, validVersion } from './package.ts';
import { boundedCleanup, ConnectionDiagnostics, ConnectionFailure } from './diagnostics.ts';

export type Protocol = 'RP1' | 'RS1' | 'RO1' | 'RE1';
export type JsonRecord = Record<string, unknown>;
export type AppRecord = [id: string, version: string, abi: number, health: number];
export interface PackageStatus {
  format: number; allowed: number; busy: number; phase: number; session: string;
  received: number; total: number; count: number; orphans: number;
  capacity: number[]; us: number[]; error: string; apps: AppRecord[];
  name: string; sha256: string; elf_size: number;
}
export class ProtocolError extends Error { override name = 'ProtocolError'; }
export class DeviceError extends Error {
  readonly code: number;
  readonly status: JsonRecord;
  constructor(code: number, status: JsonRecord) {
    const names: Record<number, string> = { 257: 'Out of memory or storage', 258: 'Invalid argument', 259: 'Device busy or admission denied', 260: 'Invalid size', 261: 'Not found', 262: 'Unsupported operation', 263: 'Device timeout', [-1]: 'Device failure' };
    super(`${names[code] ?? 'Device rejected request'} (${code})${status.error ? `: ${String(status.error)}` : ''}`);
    this.code = code;
    this.status = status;
  }
}
export function requireCondition(ok: unknown, message = 'Malformed device response.'): asserts ok {
  if (!ok) { throw new ProtocolError(message); }
}
export function integer(value: unknown, max = 0xffffffff): value is number {
  return typeof value === 'number' && Number.isInteger(value) && value >= 0 && value <= max;
}
function vector(value: unknown, size: number): value is number[] {
  return Array.isArray(value) && value.length === size && value.every(v => integer(v));
}
export function decodePackage(s: JsonRecord): PackageStatus {
  requireCondition(s.format === 1, 'Unsupported RP1 format.');
  requireCondition(integer(s.allowed, 1) && integer(s.busy, 1) && integer(s.phase, 5));
  for (const key of ['received', 'total', 'count', 'orphans', 'elf_size']) { requireCondition(integer(s[key])); }
  requireCondition(Number(s.received) <= Number(s.total) && Number(s.total) <= MAX_PACKAGE && Number(s.count) <= 8);
  requireCondition(typeof s.session === 'string' && /^[0-9a-f]{8}$/.test(s.session));
  requireCondition(vector(s.us, 7) && vector(s.capacity, 3));
  requireCondition(s.capacity[0] <= 1 && s.capacity[2] <= s.capacity[1]);
  requireCondition(Array.isArray(s.apps) && s.apps.length <= 8);
  const seen = new Set<string>();
  for (const app of s.apps) {
    requireCondition(Array.isArray(app) && app.length === 4 && typeof app[0] === 'string' && validId(app[0]) &&
      typeof app[1] === 'string' && validVersion(app[1]) && integer(app[2], 65535) && integer(app[3], 2) && !seen.has(app[0]));
    seen.add(app[0]);
  }
  requireCondition(typeof s.name === 'string' && new TextEncoder().encode(s.name).length <= 63 &&
    typeof s.error === 'string' && s.error.length < 80 && typeof s.sha256 === 'string' && (!s.sha256 || DIGEST_PATTERN.test(s.sha256)));
  return s as unknown as PackageStatus;
}

export function encodeFrame(protocol: Protocol, id: string, operation: string): Uint8Array {
  requireCondition(/^[0-9a-f]{8}$/.test(id) && /^[\x21-\x7e]+(?: [\x21-\x7e]+)*$/.test(operation), 'Invalid command.');
  const line = `!${protocol} ${id} ${operation}`;
  requireCondition(line.length < 1100, 'Command exceeds firmware line bound.');
  // Production roundie_time.write_command: newline before and after the command.
  return new TextEncoder().encode(`\n${line}\n`);
}
export function randomId(): string {
  let id: string;
  do { id = crypto.getRandomValues(new Uint32Array(1))[0].toString(16).padStart(8, '0'); } while (id === '00000000');
  return id;
}

/** Incremental bounded line decoder, including unrelated asynchronous logs. */
export class LineDecoder {
  private bytes: number[] = [];
  private discard = false;
  feed(chunk: Uint8Array, line: (value: string) => void, oversize: (prefix: string) => void,
    addressed: (prefix: string) => boolean = () => true): void {
    for (const byte of chunk) {
      if (this.bytes.length >= 2048 && !this.discard) {
        oversize(new TextDecoder().decode(new Uint8Array(this.bytes.slice(0, 14))));
        this.discard = true;
      }
      if (!this.discard) { this.bytes.push(byte); }
      if (byte === 10) {
        if (!this.discard) {
          // Decode only protocol candidates strictly. Arbitrary logs may contain binary noise.
          // Like Python read_serial_reply: match ASCII address before strict decoding.
          if (this.bytes[0] === 33 && addressed(String.fromCharCode(...this.bytes.slice(0, 14)))) {
            let value: string;
            try { value = new TextDecoder('utf-8', { fatal: true }).decode(new Uint8Array(this.bytes)); }
            catch (cause) { throw new ProtocolError('Invalid protocol encoding.', { cause }); }
            line(value);
          }
        }
        this.bytes = [];
        this.discard = false;
      }
    }
  }
}

interface Pending {
  prefix: string; protocol: Protocol;
  resolve: (status: JsonRecord) => void; reject: (error: Error) => void;
  timer: ReturnType<typeof setTimeout>;
}
export class ProtocolClient {
  private transport: SerialTransport;
  private pending?: Pending;
  private decoder = new LineDecoder();
  private pump: Promise<void>;
  private closing?: Promise<void>;
  private closed?: Promise<void>;
  private dead = false;
  private nextId: () => string;
  private first?: Error;
  cleanupError?: Error;
  readonly diagnostics: ConnectionDiagnostics;
  onDisconnect?: (reason: string, error: Error) => void;

  constructor(transport: SerialTransport, nextId = randomId, diagnostics = new ConnectionDiagnostics()) {
    this.transport = transport;
    this.nextId = nextId;
    this.diagnostics = diagnostics;
    transport.onError = error => this.fail(error);
    diagnostics.state.protocol = 'receiving'; diagnostics.note('RECEIVE_PUMP_STARTED');
    this.pump = this.receive();
  }
  get connected(): boolean { return !this.dead; }
  get failure(): Error | undefined { return this.first; }

  async request(protocol: Protocol, operation: string, timeoutMs = 8000): Promise<JsonRecord> {
    if (this.dead) { throw new ProtocolError('Disconnected. Reconnect and refresh device state.'); }
    if (this.pending) { throw new ProtocolError('Another serial command is in progress.'); }
    const id = this.nextId();
    const wire = encodeFrame(protocol, id, operation);
    return new Promise<JsonRecord>((resolve, reject) => {
      this.pending = { prefix: `!${protocol} ${id} `, protocol, resolve, reject,
        timer: setTimeout(() => this.fail(new ConnectionFailure('timeout', 'Response timeout. Request may have reached the device; reconnect to reconcile.')), timeoutMs) };
      this.diagnostics.state.pendingId = id;
      this.diagnostics.note('REQUEST_SENT', { wireProtocol: protocol, command: operation.split(' ')[0] });
      // A write resolving is never an acknowledgement. Only addressed replies resolve requests.
      void this.transport.write(wire).catch(error => this.fail(error instanceof ConnectionFailure ? error :
        new ConnectionFailure('write', `Serial write failed: ${String(error)}`, error)));
    });
  }
  async package(operation: string): Promise<PackageStatus> {
    const raw = await this.request('RP1', operation);
    try { return decodePackage(raw); }
    catch (error) {
      const failure = new ConnectionFailure('protocol', 'Invalid RP1 response.', error);
      this.fail(failure); throw failure;
    }
  }

  private async receive(): Promise<void> {
    try {
      while (!this.dead) {
        const bytes = await this.transport.read();
        // Cancellation may resolve with buffered bytes or reject after fail() won.
        if (this.dead) { this.diagnostics.note('LATE_READ_IGNORED'); break; }
        if (bytes === null) { throw new ConnectionFailure('lost', 'USB disconnected. Incomplete operations require reconciliation.'); }
        this.decoder.feed(bytes, line => this.dispatch(line), prefix => {
          if (this.pending?.prefix === prefix) { throw new ProtocolError('Oversized protocol reply.'); }
        }, prefix => {
          const matches = this.pending?.prefix === prefix;
          if (!matches) { this.diagnostics.note('UNRELATED_REPLY_IGNORED'); }
          return matches;
        });
      }
    } catch (error) {
      if (this.dead) { this.diagnostics.note('SECONDARY_RECEIVE_ERROR', { error: String(error) }); }
      else { this.fail(error instanceof ConnectionFailure ? error : new ConnectionFailure(
        error instanceof ProtocolError ? 'protocol' : 'read', error instanceof Error ? error.message : String(error), error)); }
    } finally { this.diagnostics.note('RECEIVE_PUMP_STOPPED'); }
  }
  private dispatch(line: string): void {
    const p = this.pending;
    if (!p || !line.startsWith(p.prefix)) { return; } // Ignore logs and unrelated/late request IDs.
    const max = p.protocol === 'RP1' ? 1900 : p.protocol === 'RE1' ? 2048 : 1024;
    requireCondition(new TextEncoder().encode(line).length <= max, 'Oversized addressed response.');
    const match = /^(-?[0-9]{1,10}) (.+)\n$/.exec(line.slice(p.prefix.length));
    requireCondition(match, 'Malformed addressed response.');
    let status: unknown;
    try { status = JSON.parse(match[2]); } catch (cause) { throw new ProtocolError('Malformed response JSON.', { cause }); }
    requireCondition(status !== null && typeof status === 'object' && !Array.isArray(status));
    // Validate RP1 error payloads as strictly as successful responses.
    if (p.protocol === 'RP1') { decodePackage(status as JsonRecord); }
    clearTimeout(p.timer);
    this.diagnostics.note('RESPONSE_MATCHED', { wireProtocol: p.protocol, code: Number(match[1]) });
    this.diagnostics.state.pendingId = null;
    this.pending = undefined;
    if (Number(match[1]) !== 0) { p.reject(new DeviceError(Number(match[1]), status as JsonRecord)); }
    else { p.resolve(status as JsonRecord); }
  }
  private fail(error: Error): void {
    if (this.dead) { this.diagnostics.note('SECONDARY_FAILURE', { error: error.message }); return; }
    this.first = error;
    const intentional = error instanceof ConnectionFailure && error.phase === 'user';
    if (intentional) { this.diagnostics.note('USER_DISCONNECT'); }
    else { this.diagnostics.failure('PROTOCOL_FAILED', error, error instanceof ConnectionFailure ? error.phase : 'protocol'); }
    this.diagnostics.note('TEARDOWN_REQUESTED', { initiator: intentional ? 'user' : 'failure' });
    this.dead = true;
    this.transport.onError = undefined;
    this.diagnostics.state.protocol = 'dead';
    if (this.pending) { clearTimeout(this.pending.timer); this.pending.reject(error); this.pending = undefined; }
    this.diagnostics.state.pendingId = null;
    this.closing ??= Promise.resolve().then(() => boundedCleanup(this.transport.close(), 'Transport', 5000)).catch(cause => {
      this.cleanupError = cause instanceof Error ? cause : new Error(String(cause));
      this.diagnostics.failure('PROTOCOL_CLEANUP_FAILED', cause, 'cleanup');
    });
    if (!intentional) { this.onDisconnect?.(error.message, error); }
  }
  close(reason: Error = new ConnectionFailure('user', 'Disconnected by user.')): Promise<void> {
    this.fail(reason);
    this.closed ??= (async () => {
      await this.closing;
      try { await boundedCleanup(this.pump, 'Receive pump'); }
      catch (cause) {
        this.cleanupError ??= cause instanceof Error ? cause : new Error(String(cause));
        this.diagnostics.failure('PUMP_CLEANUP_FAILED', cause, 'cleanup');
      }
      if (this.cleanupError) { throw this.cleanupError; }
    })();
    return this.closed;
  }
}
