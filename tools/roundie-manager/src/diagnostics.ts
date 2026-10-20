export type ConnectionPhase = 'selection' | 'open' | 'signals' | 'streams' | 'read' | 'write' | 'handshake' | 'timeout' | 'protocol' | 'lost' | 'close' | 'user';

export class ConnectionFailure extends Error {
  readonly phase: ConnectionPhase;
  cleanupError?: unknown;
  constructor(phase: ConnectionPhase, message: string, cause?: unknown) {
    super(message, { cause });
    this.name = 'ConnectionFailure';
    this.phase = phase;
  }
}

export function errorDetails(error: unknown): string {
  if (!(error instanceof Error)) { return String(error); }
  const own = `${error.name}: ${error.message}`;
  const members = error instanceof AggregateError ? `\n${error.errors.map(errorDetails).join('\n')}` : '';
  return own + members + (error.cause === undefined ? '' : `\nCaused by: ${errorDetails(error.cause)}`);
}

let generation = 0;
type Fields = Record<string, string | number | boolean | null>;
export interface DiagnosticEntry extends Fields { sequence: number; elapsedMs: number; session: number; operation: string }

/** Opt-in, local-only metadata. No payloads, package bytes or USB serial numbers. */
export class ConnectionDiagnostics {
  readonly session = ++generation;
  private started = performance.now();
  private sequence = 0;
  private entries: DiagnosticEntry[] = [];
  private first?: DiagnosticEntry;
  readonly state: Fields = { phase: 'selection', transport: 'unopened', protocol: 'idle', pendingId: null,
    readerOwned: false, writerOwned: false };
  readonly enabled: boolean;
  constructor(enabled = false) { this.enabled = enabled; }
  note(operation: string, fields: Fields = {}): void {
    if (!this.enabled) { return; }
    const entry = { ...this.state, ...fields, sequence: ++this.sequence,
      elapsedMs: Math.round((performance.now() - this.started) * 10) / 10, session: this.session, operation };
    this.entries.push(entry);
    if (this.entries.length > 256) { this.entries.shift(); }
  }
  failure(operation: string, error: unknown, initiator: string): void {
    this.note(operation, { errorName: error instanceof Error ? error.name : typeof error,
      errorMessage: error instanceof Error ? error.message : String(error), initiator, primary: !this.first });
    if (!this.first && this.enabled) { this.first = this.entries.at(-1); }
  }
  snapshot(): { firstError?: DiagnosticEntry; entries: DiagnosticEntry[] } {
    return { firstError: this.first && { ...this.first }, entries: this.entries.map(entry => ({ ...entry })) };
  }
}

/** A cleanup deadline reports uncertainty; it never retries or declares a port closed. */
export async function boundedCleanup<T>(operation: Promise<T>, label: string, timeoutMs = 2000): Promise<T> {
  let timer: ReturnType<typeof setTimeout> | undefined;
  try {
    return await Promise.race([operation, new Promise<never>((_, reject) => {
      timer = setTimeout(() => reject(new ConnectionFailure('close', `${label} cleanup did not finish. Reload this page before reconnecting.`)), timeoutMs);
    })]);
  } finally { clearTimeout(timer); }
}
