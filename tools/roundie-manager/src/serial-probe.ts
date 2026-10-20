import { boundedCleanup, ConnectionDiagnostics, ConnectionFailure } from './diagnostics.ts';
import { encodeFrame, LineDecoder, ProtocolClient, randomId, requireCondition, type JsonRecord } from './protocol.ts';
import { WebSerialTransport, type SerialApi, type SerialPortLike, type SerialTransport } from './transport.ts';

export type ProbeLevel = 'A' | 'B' | 'C' | 'D';
export interface ProbeResult { level: ProbeLevel; bytes?: number; idle?: boolean; firmware?: string; slot?: string; phase?: string; elapsedMs: number }

/** B/C keep the single-command reader independent of ProtocolClient scheduling. */
async function rawStatus(transport: SerialTransport, d: ConnectionDiagnostics): Promise<JsonRecord> {
  const id = randomId();
  const prefix = `!RO1 ${id} `;
  d.state.pendingId = id;
  const decoder = new LineDecoder();
  const receiving = (async () => {
    let status: JsonRecord | undefined;
    while (!status) {
      const bytes = await transport.read();
      if (bytes === null) { throw new ConnectionFailure('lost', 'Stream ended before the RO1 reply.'); }
      decoder.feed(bytes, line => {
        requireCondition(new TextEncoder().encode(line).length <= 1024, 'Oversized RO1 response.');
        const match = /^0 (.+)\n$/.exec(line.slice(prefix.length));
        requireCondition(match, 'RO1 rejected or malformed.');
        const value: unknown = JSON.parse(match[1]);
        requireCondition(value !== null && typeof value === 'object' && !Array.isArray(value));
        status = value as JsonRecord;
      }, start => { requireCondition(start !== prefix, 'Oversized RO1 response.'); }, start => start === prefix);
    }
    d.note('RAW_RESPONSE_MATCHED');
    return status;
  })();
  d.note('RAW_REQUEST_SENT');
  const operation = Promise.all([transport.write(encodeFrame('RO1', id, 'STATUS')), receiving]);
  let timer: ReturnType<typeof setTimeout> | undefined;
  try {
    return await Promise.race([operation.then(([, status]) => status), new Promise<never>((_, reject) => {
      timer = setTimeout(() => reject(new ConnectionFailure('handshake', 'RO1 response timeout.')), 8000);
    })]);
  } finally { clearTimeout(timer); }
}

/** Read-only differential probes. No reset, flushing, retries or package operations. */
export async function runSerialProbe(api: SerialApi, level: ProbeLevel, d: ConnectionDiagnostics): Promise<ProbeResult> {
  const started = performance.now();
  let port: SerialPortLike | undefined;
  let reader: ReadableStreamDefaultReader<Uint8Array> | undefined;
  let writer: WritableStreamDefaultWriter<Uint8Array> | undefined;
  let transport: SerialTransport | undefined;
  let client: ProtocolClient | undefined;
  let opened = false;
  let failed = false;
  let result: Omit<ProbeResult, 'elapsedMs'> = { level };
  d.state.phase = `probe-${level}`;
  try {
    if (level === 'A' || level === 'B') {
      d.note('RAW_PORT_REQUESTED');
      port = await api.requestPort();
      d.note('RAW_OPEN_STARTED');
      // Exactly the successful manual open. Raw levels do not call setSignals().
      await port.open({ baudRate: 115200 });
      opened = true;
      d.state.transport = 'open'; d.note('RAW_PORT_OPENED');
      if (!port.readable || !port.writable) { throw new ConnectionFailure('streams', 'Raw serial streams unavailable.'); }
      reader = port.readable.getReader();
      d.state.readerOwned = true;
      d.note('RAW_READER_ACQUIRED');
      if (level === 'A') {
        // Passive input need not exist. This is a sampling window, not a handshake timeout.
        let timer: ReturnType<typeof setTimeout> | undefined;
        try {
          d.note('RAW_READ_STARTED');
          const sample = await Promise.race([reader.read(), new Promise<null>(resolve => { timer = setTimeout(() => resolve(null), 1500); })]);
          result = { level, bytes: sample?.value?.length ?? 0, idle: sample === null };
          d.note('RAW_PASSIVE_READ', { bytes: result.bytes!, idle: result.idle! });
        } finally { clearTimeout(timer); }
      } else {
        writer = port.writable.getWriter();
        d.state.writerOwned = true;
        d.note('RAW_WRITER_ACQUIRED');
        const rawReader = reader;
        const rawWriter = writer;
        transport = { read: async () => {
          d.note('RAW_READ_STARTED');
          const value = await rawReader.read();
          d.note('RAW_READ_COMPLETE', { bytes: value.value?.length ?? 0, done: value.done });
          return value.done ? null : value.value;
        }, write: async bytes => { d.note('RAW_WRITE_STARTED'); await rawWriter.write(bytes); d.note('RAW_WRITE_COMPLETE'); },
        close: async () => {} };
      }
    } else {
      transport = await WebSerialTransport.connect(api, d);
      if (level === 'D') { client = new ProtocolClient(transport, undefined, d); }
    }
    if (level !== 'A') {
      const status = client ? await client.request('RO1', 'STATUS') : await rawStatus(transport!, d);
      requireCondition(typeof status.version === 'string' && ['ota_0', 'ota_1'].includes(String(status.running)) && typeof status.phase === 'string', 'Unexpected RO1 identity.');
      result = { level, firmware: status.version, slot: String(status.running), phase: status.phase };
    }
    d.note('PROBE_COMPLETE');
  } catch (error) {
    failed = true;
    d.failure('PROBE_FAILED', error, `probe-${level}`);
    throw error;
  } finally {
    try {
      d.note('PROBE_CLEANUP_STARTED');
      if (port) {
        // Raw path intentionally uses native streams, without Manager teardown.
        if (reader) {
          try { await boundedCleanup(reader.cancel(), 'Raw reader'); }
          catch (error) { d.note('RAW_CANCEL_ERROR', { error: String(error) }); }
          reader.releaseLock();
        }
        writer?.releaseLock();
        d.state.readerOwned = false; d.state.writerOwned = false;
        if (opened) {
          await boundedCleanup(port.close(), 'Raw port');
          d.state.transport = 'closed'; d.note('RAW_PORT_CLOSED');
        } else { d.note('RAW_PORT_NOT_OPENED'); }
      } else if (client) { await client.close(); }
      else if (transport) { await transport.close(); }
    } catch (error) {
      d.failure('PROBE_CLEANUP_FAILED', error, 'cleanup');
      if (!failed) { throw error; }
    }
  }
  return { ...result, elapsedMs: performance.now() - started };
}
