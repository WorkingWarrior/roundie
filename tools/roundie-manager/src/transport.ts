/** The only stream reader belongs to the protocol client. write() consumes all
 * bytes or rejects; the native Web Serial sink has no partial byte-count API. */
export interface SerialTransport {
  read(): Promise<Uint8Array | null>;
  write(bytes: Uint8Array): Promise<void>;
  close(): Promise<void>;
}

export interface SerialPortLike {
  readable: ReadableStream<Uint8Array> | null;
  writable: WritableStream<Uint8Array> | null;
  open(options: { baudRate: number; dataBits: number; stopBits: number; parity: string; flowControl: string; bufferSize: number }): Promise<void>;
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

  private constructor(port: SerialPortLike) {
    if (!port.readable || !port.writable) { throw new Error('Serial streams unavailable.'); }
    this.port = port;
    this.reader = port.readable.getReader();
    this.writer = port.writable.getWriter();
    // Observe stream failures even while no command is being sent.
    void this.writer.closed.catch(() => {});
  }

  static async connect(api: SerialApi): Promise<WebSerialTransport> {
    // Called directly by the Connect click; no permission prompt on page load.
    const port = await api.requestPort();
    await port.open({ baudRate: 115200, dataBits: 8, stopBits: 1, parity: 'none', flowControl: 'none', bufferSize: 4096 });
    try {
      // Match the verified Python idle pair. Never toggle the lines separately.
      await port.setSignals({ dataTerminalReady: true, requestToSend: true });
      return new WebSerialTransport(port);
    } catch (error) {
      await port.close().catch(() => {});
      throw error;
    }
  }

  async read(): Promise<Uint8Array | null> {
    const result = await this.reader.read();
    return result.done ? null : result.value;
  }

  async write(bytes: Uint8Array): Promise<void> {
    if (this.closing) { throw new Error('Serial connection closed.'); }
    await this.writer.write(bytes);
  }

  close(): Promise<void> {
    this.closing ??= this.teardown();
    return this.closing;
  }

  private async teardown(): Promise<void> {
    // Abort queued writes instead of waiting indefinitely for USB backpressure.
    await Promise.allSettled([this.reader.cancel(), this.writer.abort()]);
    this.reader.releaseLock();
    this.writer.releaseLock();
    await this.port.close();
  }
}

export function connectionError(error: unknown): string {
  if (error instanceof Error) {
    if (error.name === 'NotFoundError') { return 'Port selection canceled. No connection was opened.'; }
    if (error.name === 'SecurityError' || error.name === 'NotAllowedError') { return 'Serial permission denied. Check browser permission and secure context.'; }
    if (error.name === 'NetworkError' || error.name === 'InvalidStateError') { return 'Cannot open serial port. Close other monitors and check Linux device permissions.'; }
    return error.message;
  }
  return String(error);
}
