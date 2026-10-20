import assert from 'node:assert/strict';
import { test } from 'node:test';
import { connectionError, WebSerialTransport, type SerialPortLike } from '../src/transport.ts';
import { ProtocolClient } from '../src/protocol.ts';
import { MockTransport } from './mock.ts';
import { boundedCleanup, ConnectionDiagnostics, ConnectionFailure } from '../src/diagnostics.ts';
import { runSerialProbe, type ProbeLevel } from '../src/serial-probe.ts';

test('an unexpected read NetworkError is never classified as an open failure', () => {
  assert.doesNotMatch(connectionError(new DOMException('The device has been lost.', 'NetworkError')), /Cannot open serial port/);
});

test('writer acquisition failure releases the already acquired reader', async () => {
  const readable = new ReadableStream<Uint8Array>();
  const writable = new WritableStream<Uint8Array>();
  const otherWriter = writable.getWriter();
  let closeCalls = 0;
  const port: SerialPortLike = { readable, writable, open: async () => {}, setSignals: async () => {},
    close: async () => { closeCalls++; assert.equal(readable.locked, false); } };
  await assert.rejects(WebSerialTransport.connect({ requestPort: async () => port }));
  assert.equal(readable.locked, false);
  assert.equal(closeCalls, 1);
  otherWriter.releaseLock();
});

test('unrelated stale response with invalid UTF-8 cannot kill the current RO1 handshake', async () => {
  const mock = new MockTransport(() => null);
  const client = new ProtocolClient(mock, () => '12345678');
  const reply = client.request('RO1', 'STATUS', 100);
  mock.push(new Uint8Array([...new TextEncoder().encode('\n!RO1 deadbeef 0 '), 255, 10,
    ...new TextEncoder().encode('!RO1 12345678 0 {"phase":"IDLE"}\n')]));
  try { assert.equal((await reply).phase, 'IDLE'); }
  finally { await client.close(); }
});

test('addressed invalid UTF-8 still fails closed and preserves the decoder cause', async () => {
  const mock = new MockTransport(() => null);
  const client = new ProtocolClient(mock, () => '12345678');
  const request = client.request('RO1', 'STATUS');
  mock.push(new Uint8Array([...new TextEncoder().encode('!RO1 12345678 0 '), 255, 10]));
  await assert.rejects(request, (error: unknown) => {
    assert.ok(error instanceof ConnectionFailure);
    assert.equal(error.phase, 'protocol');
    assert.ok(error.cause instanceof Error);
    assert.ok(error.cause.cause instanceof TypeError);
    return true;
  });
  assert.equal(client.connected, false);
  await client.close();
});

function fakePort() {
  let source!: ReadableStreamDefaultController<Uint8Array>;
  let sink!: WritableStreamDefaultController;
  const state = { closes: 0, writeError: undefined as Error | undefined, closeError: undefined as Error | undefined };
  const readable = new ReadableStream<Uint8Array>({ start(c) { source = c; } });
  const writable = new WritableStream<Uint8Array>({ start(c) { sink = c; }, write() { if (state.writeError) { throw state.writeError; } } });
  const port: SerialPortLike = { readable, writable, open: async () => {}, setSignals: async () => {}, close: async () => {
    assert.equal(readable.locked, false); assert.equal(writable.locked, false);
    state.closes++; if (state.closeError) { throw state.closeError; }
  } };
  return { port, source, sink, state, readable, writable };
}

test('native streams raw read, pending cancellation and concurrent close have one owner', async () => {
  const f = fakePort();
  const transport = await WebSerialTransport.connect({ requestPort: async () => f.port });
  f.source.enqueue(new Uint8Array([10, 255, 0]));
  assert.deepEqual(await transport.read(), new Uint8Array([10, 255, 0]));
  const pending = transport.read();
  const first = transport.close(); assert.equal(transport.close(), first);
  await first; assert.equal(await pending, null); assert.equal(f.state.closes, 1);
});

for (const operation of ['read', 'write'] as const) {
  test(`native ${operation} failure preserves original browser cause and releases locks`, async () => {
    const f = fakePort(); const d = new ConnectionDiagnostics(true);
    const transport = await WebSerialTransport.connect({ requestPort: async () => f.port }, d);
    const original = new DOMException('The device has been lost.', 'NetworkError');
    const client = new ProtocolClient(transport, () => '12345678', d);
    if (operation === 'write') { f.state.writeError = original; }
    const request = client.request('RO1', 'STATUS');
    if (operation === 'read') { f.source.error(original); }
    await assert.rejects(request, (error: unknown) => {
      assert.ok(error instanceof ConnectionFailure); assert.equal(error.phase, operation); assert.equal(error.cause, original); return true;
    });
    const first = client.failure;
    await client.close(); assert.equal(client.failure, first);
    assert.equal(d.snapshot().firstError?.operation, `${operation.toUpperCase()}_FAILED`);
    assert.equal(f.state.closes, 1);
  });
}

test('handshake timeout initiates teardown; secondary read rejection cannot replace it', async () => {
  const d = new ConnectionDiagnostics(true);
  let rejectRead!: (error: Error) => void;
  const client = new ProtocolClient({ read: () => new Promise((_, reject) => { rejectRead = reject; }),
    write: async () => {}, close: async () => { rejectRead(new DOMException('The device has been lost.', 'NetworkError')); }
  }, () => '12345678', d);
  await assert.rejects(client.request('RO1', 'STATUS', 10), /timeout/);
  await client.close();
  assert.match(client.failure!.message, /timeout/);
  assert.equal(d.snapshot().firstError?.initiator, 'timeout');
  assert.ok(d.snapshot().entries.some(e => e.operation === 'SECONDARY_RECEIVE_ERROR'));
});

test('user disconnect during handshake rejects request once without device loss callback', async () => {
  const f = fakePort(); const d = new ConnectionDiagnostics(true);
  const transport = await WebSerialTransport.connect({ requestPort: async () => f.port }, d);
  const client = new ProtocolClient(transport, () => '12345678', d);
  let notices = 0; client.onDisconnect = () => { notices++; };
  const request = client.request('RO1', 'STATUS');
  const rejection = assert.rejects(request, /Disconnected by user/);
  await Promise.all([client.close(), client.close(), rejection]);
  assert.equal(notices, 0); assert.equal(d.snapshot().firstError, undefined);
});

test('an idle output stream failure invalidates the connection even without a write request', async () => {
  const f = fakePort(); const d = new ConnectionDiagnostics(true);
  const transport = await WebSerialTransport.connect({ requestPort: async () => f.port }, d);
  const client = new ProtocolClient(transport, () => '12345678', d);
  const original = new DOMException('Output lost', 'NetworkError');
  f.sink.error(original);
  await new Promise(resolve => setTimeout(resolve, 0));
  assert.equal(client.connected, false);
  assert.equal(client.failure?.cause, original);
  assert.equal(d.snapshot().firstError?.operation, 'WRITE_STREAM_FAILED');
  await client.close();
});

test('close failure is observable without overwriting the first protocol failure', async () => {
  const f = fakePort(); f.state.closeError = new Error('close failed');
  const d = new ConnectionDiagnostics(true);
  const transport = await WebSerialTransport.connect({ requestPort: async () => f.port }, d);
  const client = new ProtocolClient(transport, () => '12345678', d);
  const request = client.request('RO1', 'STATUS');
  f.source.enqueue(new TextEncoder().encode('!RO1 12345678 0 invalid\n'));
  await assert.rejects(request, /Malformed response JSON/);
  await assert.rejects(client.close(), /cleanup incomplete/);
  assert.equal(f.readable.locked, false); assert.equal(f.writable.locked, false);
  assert.equal(d.snapshot().firstError?.initiator, 'protocol');
});

for (const phase of ['selection', 'open', 'signals', 'streams'] as const) {
  test(`setup errors retain ${phase} and clean up only an opened port`, async () => {
    const f = fakePort(); const original = new DOMException('failure', 'NetworkError');
    if (phase === 'open') { f.port.open = async () => { throw original; }; }
    if (phase === 'signals') { f.port.setSignals = async () => { throw original; }; }
    if (phase === 'streams') { f.port.readable = null; }
    const d = new ConnectionDiagnostics(true);
    await assert.rejects(WebSerialTransport.connect({ requestPort: async () => {
      if (phase === 'selection') { throw original; } return f.port;
    } }, d), (error: unknown) => {
      assert.ok(error instanceof ConnectionFailure); assert.equal(error.phase, phase);
      if (phase !== 'streams') { assert.equal(error.cause, original); }
      assert.equal(connectionError(error).includes('Cannot open serial port'), phase === 'open'); return true;
    });
    assert.equal(f.state.closes, ['signals', 'streams'].includes(phase) ? 1 : 0);
    assert.equal(f.readable.locked, false); assert.equal(f.writable.locked, false);
  });
}

test('cleanup deadline reports uncertainty without an unhandled late rejection', async () => {
  let reject!: (error: Error) => void;
  const blocked = new Promise<void>((_, no) => { reject = no; });
  await assert.rejects(boundedCleanup(blocked, 'Test writer', 5), /cleanup did not finish/);
  reject(new Error('late sink rejection'));
  await new Promise(resolve => setTimeout(resolve, 0));
});

test('blocked native write cannot deadlock teardown; locks release and uncertainty is reported', async () => {
  const f = fakePort();
  let finish!: () => void;
  f.port.writable = new WritableStream<Uint8Array>({ write: () => new Promise<void>(resolve => { finish = resolve; }) });
  const transport = await WebSerialTransport.connect({ requestPort: async () => f.port });
  const writing = transport.write(new Uint8Array([1]));
  await new Promise(resolve => setTimeout(resolve, 0));
  await assert.rejects(transport.close(), /cleanup incomplete/);
  assert.equal(f.port.writable.locked, false); assert.equal(f.state.closes, 1);
  finish(); await writing;
});

test('old pump cannot dispatch queued bytes after shutdown or affect a new client', async () => {
  let resolveOld!: (bytes: Uint8Array | null) => void;
  const old = new ProtocolClient({ read: () => new Promise(resolve => { resolveOld = resolve; }), write: async () => {},
    close: async () => { resolveOld(new TextEncoder().encode('!RO1 11111111 0 invalid\n')); }
  }, () => '11111111');
  const oldRequest = old.request('RO1', 'STATUS');
  const rejected = assert.rejects(oldRequest, /Disconnected by user/);
  const closing = old.close();
  const mock = new MockTransport(() => ({ phase: 'IDLE' }));
  const fresh = new ProtocolClient(mock, () => '22222222');
  assert.equal((await fresh.request('RO1', 'STATUS')).phase, 'IDLE');
  await Promise.all([closing, rejected]); assert.equal(fresh.connected, true); await fresh.close();
});

for (const fragmentSize of [1, 7, 4096]) {
  test(`RO1 leading LF, stale session, unrelated protocol and current reply in ${fragmentSize}-byte reads`, async () => {
    const mock = new MockTransport(() => null); mock.fragmentSize = fragmentSize;
    const client = new ProtocolClient(mock, () => '12345678');
    const request = client.request('RO1', 'STATUS', 100);
    setTimeout(() => mock.push(new TextEncoder().encode('\nnoise\n!RO1 deadbeef 0 {}\n!RS1 12345678 0 {}\n!RO1 12345678 0 {"phase":"IDLE"}\n')), 5);
    assert.equal((await request).phase, 'IDLE'); await client.close();
  });
}

test('diagnostics remain bounded and retain the first error outside the ring', () => {
  const d = new ConnectionDiagnostics(true); d.failure('FIRST', new Error('original'), 'test');
  for (let i = 0; i < 300; i++) { d.note('READ'); }
  d.failure('SECOND', new Error('cleanup'), 'cleanup');
  const snapshot = d.snapshot(); assert.equal(snapshot.entries.length, 256);
  assert.equal(snapshot.firstError?.errorMessage, 'original');
  assert.ok(snapshot.entries.every((e, i, entries) => i === 0 || e.sequence > entries[i - 1].sequence));
  const silent = new ConnectionDiagnostics(); silent.failure('ERROR', new Error('x'), 'test');
  assert.equal(silent.snapshot().entries.length, 0);
});

for (const missing of ['readable', 'writable'] as const) {
  test(`null ${missing} is stream initialization failure with no retained lock`, async () => {
    const f = fakePort(); f.port[missing] = null;
    await assert.rejects(WebSerialTransport.connect({ requestPort: async () => f.port }), (error: unknown) => {
      assert.ok(error instanceof ConnectionFailure); assert.equal(error.phase, 'streams'); return true;
    });
    assert.equal(f.readable.locked, false); assert.equal(f.writable.locked, false);
    assert.equal(f.state.closes, 1);
  });
}

test('raw passive probe reads bytes without a command or control-signal setup', async () => {
  const f = fakePort();
  f.port.setSignals = async () => assert.fail('Raw probes must not set signals');
  f.source.enqueue(new Uint8Array(255));
  const result = await runSerialProbe({ requestPort: async () => f.port }, 'A', new ConnectionDiagnostics(true));
  assert.equal(result.bytes, 255); assert.equal(result.idle, false);
  assert.equal(f.state.closes, 1); assert.equal(f.readable.locked, false);
});

for (const level of ['B', 'C', 'D'] as ProbeLevel[]) {
  test(`differential probe ${level} matches only its own fragmented RO1 STATUS reply`, async () => {
    const f = fakePort(); let writes = 0;
    f.port.writable = new WritableStream<Uint8Array>({ write(bytes) {
      writes++;
      const request = new TextDecoder().decode(bytes);
      const match = /^\n!RO1 ([0-9a-f]{8}) STATUS\n$/.exec(request)!;
      assert.ok(match);
      const reply = new TextEncoder().encode(`\n!RO1 00000000 0 {}\n!RO1 ${match[1]} 0 {"version":"test","running":"ota_0","phase":"IDLE"}\n`);
      f.source.enqueue(reply.subarray(0, 40)); f.source.enqueue(reply.subarray(40));
    } });
    if (level === 'B') { f.port.setSignals = async () => assert.fail('Raw probe must not set signals'); }
    const result = await runSerialProbe({ requestPort: async () => f.port }, level, new ConnectionDiagnostics(true));
    assert.equal(result.firmware, 'test'); assert.equal(writes, 1);
    assert.equal(f.state.closes, 1); assert.equal(f.port.writable.locked, false);
  });
}
