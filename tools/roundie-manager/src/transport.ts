import { boundedCleanup, ConnectionDiagnostics, ConnectionFailure, type ConnectionPhase } from './diagnostics.ts';

/** The only stream reader belongs to the protocol client. write() consumes all
 * bytes or rejects; the native Web Serial sink has no partial byte-count API. */
export interface SerialTransport {
  read(): Promise<Uint8Array | null>;
  write(bytes: Uint8Array): Promise<void>;
  close(): Promise<void>;
  onError?: (error: ConnectionFailure) => void;
}

export interface SerialPortLike {
  readable: ReadableStream<Uint8Array> | null;
  writable: WritableStream<Uint8Array> | null;
  open(options: { baudRate: number; dataBits?: number; stopBits?: number; parity?: string; flowControl?: string; bufferSize?: number }): Promise<void>;
  setSignals(signals: { dataTerminalReady: boolean; requestToSend: boolean }): Promise<void>;
  close(): Promise<void>;
}
export interface SerialApi {
  requestPort(): Promise<SerialPortLike>;
}

export function serialCapability(nav: object, secure: boolean): string | null {
  if (!secure) { return 'Open Manager on localhost or HTTPS to use Web Serial.'; }
  if (!('serial' in nav)) { return 'Web Serial is unavailable in this browser. Use a browser that exposes navigator.serial on Linux.'; }
  return null;
}

export class WebSerialTransport implements SerialTransport {
  private port: SerialPortLike;
  private reader: ReadableStreamDefaultReader<Uint8Array>;
  private writer: WritableStreamDefaultWriter<Uint8Array>;
  private closing?: Promise<void>;
  readonly diagnostics: ConnectionDiagnostics;
  onError?: (error: ConnectionFailure) => void;

  private constructor(port: SerialPortLike, reader: ReadableStreamDefaultReader<Uint8Array>,
    writer: WritableStreamDefaultWriter<Uint8Array>, diagnostics: ConnectionDiagnostics) {
    this.port = port;
    this.reader = reader;
    this.writer = writer;
    this.diagnostics = diagnostics;
    // closed can reject even while idle. Observe both without claiming a read failed.
    void reader.closed.catch(error => diagnostics.note('READER_CLOSED_ERROR', { error: String(error), closing: !!this.closing }));
    void writer.closed.catch(error => {
      if (this.closing) { diagnostics.note('WRITER_CLOSED_DURING_CLOSE', { error: String(error) }); }
      else {
        diagnostics.failure('WRITE_STREAM_FAILED', error, 'write');
        this.onError?.(new ConnectionFailure('write', 'Serial output stream failed.', error));
      }
    });
  }

  static async connect(api: SerialApi, diagnostics = new ConnectionDiagnostics()): Promise<WebSerialTransport> {
    let phase: ConnectionPhase = 'selection';
    let port: SerialPortLike | undefined;
    let opened = false;
    let reader: ReadableStreamDefaultReader<Uint8Array> | undefined;
    let writer: WritableStreamDefaultWriter<Uint8Array> | undefined;
    try {
      diagnostics.note('PORT_REQUESTED');
      // No await before requestPort(): preserve the click's user activation.
      port = await api.requestPort();
      phase = 'open'; diagnostics.state.phase = phase; diagnostics.note('PORT_OPEN_STARTED');
      await port.open({ baudRate: 115200, dataBits: 8, stopBits: 1, parity: 'none', flowControl: 'none', bufferSize: 4096 });
      opened = true; diagnostics.state.transport = 'open'; diagnostics.note('PORT_OPENED');
      phase = 'signals'; diagnostics.state.phase = phase; diagnostics.note('SIGNALS_STARTED');
      // Match the verified Python idle pair. Never toggle the lines separately.
      await port.setSignals({ dataTerminalReady: true, requestToSend: true });
      diagnostics.note('SIGNALS_COMPLETE');
      phase = 'streams'; diagnostics.state.phase = phase;
      if (!port.readable || !port.writable) { throw new Error('Serial streams unavailable.'); }
      reader = port.readable.getReader();
      diagnostics.state.readerOwned = true; diagnostics.note('READER_ACQUIRED');
      writer = port.writable.getWriter();
      diagnostics.state.writerOwned = true; diagnostics.note('WRITER_ACQUIRED');
      return new WebSerialTransport(port, reader, writer, diagnostics);
    } catch (cause) {
      diagnostics.failure('CONNECT_FAILED', cause, phase);
      const failure = new ConnectionFailure(phase, 'Serial connection setup failed.', cause);
      // Roll back partial acquisition, including getWriter() after getReader().
      reader?.releaseLock(); writer?.releaseLock();
      diagnostics.state.readerOwned = false; diagnostics.state.writerOwned = false;
      if (opened && port) {
        try { await boundedCleanup(port.close(), 'Port'); diagnostics.state.transport = 'closed'; }
        catch (error) { failure.cleanupError = error; diagnostics.failure('INITIALIZATION_CLEANUP_FAILED', error, phase); }
      }
      throw failure;
    }
  }

  async read(): Promise<Uint8Array | null> {
    if (this.closing) { return null; }
    this.diagnostics.note('READ_STARTED');
    try {
      const result = await this.reader.read();
      this.diagnostics.note('READ_COMPLETE', { done: result.done, bytes: result.value?.length ?? 0 });
      return result.done ? null : result.value;
    } catch (cause) {
      if (this.closing) { this.diagnostics.note('READ_REJECTED_DURING_CLOSE', { error: String(cause) }); }
      else { this.diagnostics.failure('READ_FAILED', cause, 'read'); }
      throw new ConnectionFailure('read', 'Unexpected serial read failure.', cause);
    }
  }

  async write(bytes: Uint8Array): Promise<void> {
    if (this.closing) { throw new ConnectionFailure('write', 'Serial connection closed.'); }
    this.diagnostics.note('WRITE_STARTED', { bytes: bytes.length });
    try { await this.writer.write(bytes); this.diagnostics.note('WRITE_COMPLETE'); }
    catch (cause) {
      if (this.closing) { this.diagnostics.note('WRITE_REJECTED_DURING_CLOSE', { error: String(cause) }); }
      else { this.diagnostics.failure('WRITE_FAILED', cause, 'write'); }
      throw new ConnectionFailure('write', 'Serial write failed.', cause);
    }
  }

  close(): Promise<void> {
    this.closing ??= Promise.resolve().then(() => this.teardown());
    return this.closing;
  }

  private async teardown(): Promise<void> {
    const d = this.diagnostics;
    d.state.transport = 'closing'; d.note('TRANSPORT_TEARDOWN_STARTED');
    const results = await Promise.allSettled([
      boundedCleanup(this.reader.cancel(), 'Reader'), boundedCleanup(this.writer.abort(), 'Writer')
    ]);
    const failures: unknown[] = [];
    for (const result of results) {
      if (result.status === 'rejected') {
        d.note('STREAM_CLEANUP_ERROR', { error: String(result.reason) });
        // An errored stream may reject cancellation with its original I/O error.
        if (result.reason instanceof ConnectionFailure && result.reason.phase === 'close') { failures.push(result.reason); }
      }
    }
    this.reader.releaseLock();
    this.writer.releaseLock();
    d.state.readerOwned = false; d.state.writerOwned = false; d.note('LOCKS_RELEASED');
    try { await boundedCleanup(this.port.close(), 'Port'); d.state.transport = 'closed'; d.note('PORT_CLOSED'); }
    catch (error) { failures.push(error); d.failure('PORT_CLOSE_FAILED', error, 'cleanup'); }
    if (failures.length) { throw new ConnectionFailure('close', 'Serial cleanup incomplete. Reload this page before reconnecting.', new AggregateError(failures)); }
  }
}

export function connectionError(error: unknown): string {
  if (error instanceof ConnectionFailure) {
    const cause = error.cause;
    switch (error.phase) {
      case 'selection':
        if (cause instanceof Error && cause.name === 'NotFoundError') { return 'Port selection canceled. No connection was opened.'; }
        if (cause instanceof Error && ['SecurityError', 'NotAllowedError'].includes(cause.name)) { return 'Serial permission denied. Check browser permission and secure context.'; }
        return 'Serial port selection failed. See connection details.';
      case 'open': return 'Cannot open serial port. Close other monitors and check Linux device permissions. See connection details.';
      case 'signals': return 'Port opened, but serial control-signal setup failed. See connection details.';
      case 'streams': return 'Port opened, but serial stream initialization failed. See connection details.';
      case 'read': case 'write': return 'Unexpected serial I/O failure after opening the port. Reconnect to check device state. See connection details.';
      case 'handshake': return 'Protocol handshake failed. See connection details.';
      case 'timeout': return 'Protocol response timed out. Reconnect to check device state. See connection details.';
      case 'protocol': return 'Protocol incompatibility or invalid response. See connection details.';
      case 'lost': return 'Connection lost. Reconnect to check device state.';
      case 'user': return 'Disconnected. You can reconnect when ready.';
      case 'close': return error.message;
    }
  }
  return error instanceof Error ? error.message : String(error);
}
