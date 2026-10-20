import assert from 'node:assert/strict';
import { test } from 'node:test';
import { readFileSync } from 'node:fs';
import { decodePackage, DeviceError, encodeFrame, LineDecoder, ProtocolClient, type Protocol } from '../src/protocol.ts';
import { inspectFile, inspectMetadata, MAX_PACKAGE, sha256 } from '../src/package.ts';
import { DeploymentCoordinator, DeviceService, installationPlan } from '../src/services.ts';
import { serialCapability, WebSerialTransport, type SerialPortLike } from '../src/transport.ts';
import { fixturePackage, MockTransport, PackageDevice, packageStatus } from './mock.ts';

const vectors = JSON.parse(readFileSync(new URL('./generated/vectors.json', import.meta.url), 'utf-8'));
for (const vector of vectors.requests) {
  test(`production Python bytes: ${vector.protocol} ${vector.operation.slice(0, 24)}`, () => {
    assert.equal(Buffer.from(encodeFrame(vector.protocol as Protocol, vector.id, vector.operation)).toString('hex'), vector.wire);
  });
}
test('production Python-validated success and rejection responses', async () => {
  for (const [index, wire] of vectors.responses.entries()) {
    const mock = new MockTransport(() => null);
    const client = new ProtocolClient(mock, () => '12345678');
    const response = client.package('STATUS');
    mock.push(new Uint8Array(Buffer.from(wire, 'hex')));
    if (index === 0) { assert.equal((await response).format, 1); }
    else { await assert.rejects(response, DeviceError); }
    await client.close();
  }
});
test('fragmented and coalesced frames preserve binary-sensitive bytes and logs', () => {
  const decoder = new LineDecoder();
  const output: string[] = [];
  const data = new TextEncoder().encode('log\n!RP1 12345678 0 {"name":"Próba"}\n!RP1 87654321 0 {}\n');
  for (const byte of data) { decoder.feed(new Uint8Array([byte]), line => output.push(line), () => assert.fail('unexpected overflow')); }
  assert.deepEqual(output, ['!RP1 12345678 0 {"name":"Próba"}\n', '!RP1 87654321 0 {}\n']);
});
test('oversized unrelated logging is discarded with bounded storage', async () => {
  const mock = new MockTransport(() => null);
  const client = new ProtocolClient(mock, () => '12345678');
  const request = client.package('STATUS');
  mock.push(new TextEncoder().encode('x'.repeat(50000) + '\n!RP1 deadbeef 0 {}\n!RP1 12345678 0 ' + JSON.stringify(packageStatus()) + '\n'));
  assert.equal((await request).format, 1);
  await client.close();
});
for (const fragmentSize of [1, 7, 4096]) {
  test(`sole reader reassembles ${fragmentSize}-byte fragments`, async () => {
    const mock = new MockTransport(); mock.fragmentSize = fragmentSize;
    const client = new ProtocolClient(mock);
    assert.equal((await client.package('STATUS')).phase, 0);
    await client.close(); assert.equal(mock.closed, true);
  });
}
for (const bad of ['not json', '{}', '[]', '{"format":2}', 'null']) {
  test(`malformed addressed response fails closed: ${bad}`, async () => {
    const mock = new MockTransport(() => null);
    const client = new ProtocolClient(mock, () => '12345678');
    const request = client.package('STATUS');
    mock.push(new TextEncoder().encode(`!RP1 12345678 0 ${bad}\n`));
    await assert.rejects(request); assert.equal(client.connected, false);
    await client.close();
  });
}
for (const [key, value] of Object.entries({ phase: 6, total: MAX_PACKAGE + 1, session: 'bad', count: 9, us: [1],
  allowed: true, capacity: [1, 0, 1], apps: [['../path', '1.0.0', 1, 0]], sha256: 'bad' })) {
  test(`RP1 strict schema rejects ${key}`, () => assert.throws(() => decodePackage(packageStatus({ [key]: value }))));
}
test('request timeout cancels waits and poisons session without retries', async () => {
  const mock = new MockTransport(() => null);
  const client = new ProtocolClient(mock);
  await assert.rejects(client.request('RP1', 'STATUS', 10), /timeout/);
  assert.equal(mock.writes.length, 1);
  assert.equal(client.connected, false);
  await assert.rejects(client.package('STATUS'), /Disconnected/);
  await client.close();
});
test('concurrent command refusal and unexpected disconnect', async () => {
  const mock = new MockTransport(() => null);
  const client = new ProtocolClient(mock);
  const pending = client.package('STATUS');
  await assert.rejects(client.package('LIST'), /in progress/);
  await mock.close(); await assert.rejects(pending, /USB disconnected/);
  await client.close();
});
test('partial sink writes reject without emitting a second command', async () => {
  const mock = new MockTransport();
  mock.write = async bytes => { mock.writes.push(new TextDecoder().decode(bytes.subarray(0, 5))); throw new Error('short serial write'); };
  const client = new ProtocolClient(mock);
  await assert.rejects(client.package('STATUS'), /short serial write/);
  assert.equal(mock.writes.length, 1); await client.close();
});
test('oversized addressed frame closes session', async () => {
  const mock = new MockTransport(() => null);
  const client = new ProtocolClient(mock, () => '12345678');
  const request = client.package('STATUS');
  mock.push(new TextEncoder().encode('!RP1 12345678 0 ' + 'x'.repeat(2100) + '\n'));
  await assert.rejects(request, /Oversized/); await client.close();
});
test('incorrect response ID cannot complete a request', async () => {
  const mock = new MockTransport(() => null);
  const client = new ProtocolClient(mock, () => '12345678');
  const request = client.request('RP1', 'STATUS', 15);
  mock.push(new TextEncoder().encode('!RP1 87654321 0 ' + JSON.stringify(packageStatus()) + '\n'));
  await assert.rejects(request, /timeout/); await client.close();
});
test('unknown device error codes remain explicit rejections', async () => {
  const mock = new MockTransport(() => packageStatus({ responseCode: 123456, error: 'future-reason' }));
  const client = new ProtocolClient(mock);
  await assert.rejects(client.package('STATUS'), /123456.*future-reason/);
  assert.equal(client.connected, true); await client.close();
});
test('late old reply cannot consume the next command response', async () => {
  const mock = new MockTransport(() => null);
  const ids = ['12345678', '87654321']; const client = new ProtocolClient(mock, () => ids.shift()!);
  const line = (id: string) => `!RP1 ${id} 0 ${JSON.stringify(packageStatus())}\n`;
  const first = client.package('STATUS'); mock.push(new TextEncoder().encode(line('12345678'))); await first;
  const second = client.package('LIST'); mock.push(new TextEncoder().encode(line('12345678') + line('87654321')));
  assert.equal((await second).phase, 0); await client.close();
});
test('command injection and overlong frames are rejected before write', () => {
  assert.throws(() => encodeFrame('RP1', '12345678', 'STATUS\nINSTALL bad'));
  assert.throws(() => encodeFrame('RP1', '12345678', 'A'.repeat(1100)));
});

test('RPK1 matches existing builder and C inspector metadata / SHA-256', async () => {
  const pkg = await fixturePackage();
  const reference = JSON.parse(readFileSync(new URL('./generated/package.json', import.meta.url), 'utf-8')).metadata;
  assert.equal(pkg.id, reference.id); assert.equal(pkg.version, reference.version);
  assert.equal(pkg.name, reference.name); assert.equal(pkg.abi, reference.abi);
  assert.equal(pkg.digest, reference.sha256); assert.equal(pkg.elfDigest, reference.elf_sha256);
  assert.equal(await sha256(new TextEncoder().encode('abc')), 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
  const file = new File([Uint8Array.from(pkg.bytes)], 'check.rpk');
  assert.equal((await inspectFile(file)).digest, pkg.digest);
});
for (const [label, offset, value] of [
  ['magic', 0, 0], ['revision', 4, 2], ['header size', 7, 255], ['total', 8, 0],
  ['manifest size', 12, 0], ['ELF size', 16, 0], ['reserved header', 20, 1],
  ['identifier grammar', 64, 47], ['malformed UTF8', 128, 255], ['control', 128, 1],
  ['ABI', 198, 4], ['target', 200, 88], ['API', 216, 2], ['padding', 223, 1]
] as const) {
  test(`package rejects ${label}`, async () => {
    const pkg = await fixturePackage(); const bytes = pkg.bytes.slice(); bytes[offset] = value;
    assert.throws(() => inspectMetadata(bytes));
  });
}
test('truncated / oversized packages rejected before reading file contents', async () => {
  assert.throws(() => inspectMetadata(new Uint8Array(223)));
  assert.throws(() => inspectMetadata(new Uint8Array(MAX_PACKAGE + 1)));
  await assert.rejects(inspectFile({ name: 'huge.rpk', size: MAX_PACKAGE + 1, arrayBuffer: () => assert.fail('must not allocate') } as unknown as File));
});
test('mismatched embedded digest rejected', async () => {
  const pkg = await fixturePackage(); pkg.bytes[224] ^= 1;
  await assert.rejects(inspectFile(new File([Uint8Array.from(pkg.bytes)], 'bad.rpk')), /digest mismatch/);
});

async function setup() {
  const pkg = await fixturePackage(); const model = new PackageDevice(pkg);
  const mock = new MockTransport(model.handle); const client = new ProtocolClient(mock);
  const device = new DeviceService(client); const deployment = new DeploymentCoordinator();
  return { pkg, model, mock, client, device, deployment };
}
test('complete multi-chunk upload, acknowledged progress, END + INSTALL + independent INFO', async () => {
  const { pkg, mock, client, device, deployment } = await setup();
  const progress: number[] = []; const phases: string[] = [];
  deployment.onProgress = p => { progress.push(p.acknowledged); phases.push(p.phase); };
  const result = await deployment.install(device, pkg);
  assert.equal(result.phase, 'completed'); assert.equal(result.acknowledged, pkg.size);
  assert.ok(mock.writes.filter(l => l.includes(' DATA ')).length > 1);
  const install = mock.writes.findIndex(l => l.includes(' INSTALL '));
  assert.ok(mock.writes.slice(install + 1).some(l => l.endsWith(`INFO ${pkg.id}`)));
  assert.ok(progress.every((v, i) => i === 0 || v >= progress[i - 1]));
  assert.ok(phases.includes('committing') && phases.includes('verifying'));
  assert.equal(mock.writes.some(l => l.includes(' LAUNCH ')), false);
  await client.close();
});
for (const command of ['BEGIN', 'DATA', 'END', 'INSTALL']) {
  test(`device rejection at ${command} never produces false success`, async () => {
    const { pkg, model, mock, client, device, deployment } = await setup();
    model.previous = true; model.reject = command;
    const result = await deployment.install(device, pkg);
    assert.notEqual(result.phase, 'completed'); assert.equal(model.installed, false);
    if (command === 'BEGIN') { assert.equal(mock.writes.some(l => l.includes(' DATA ')), false); }
    else if (command !== 'INSTALL') { assert.ok(mock.writes.some(l => l.includes(' ABORT '))); }
    assert.equal(model.previous, true); await client.close();
  });
}
test('cancel before BEGIN never creates a transaction', async () => {
  const { pkg, mock, client, device, deployment } = await setup();
  deployment.onProgress = p => { if (p.phase === 'checking') { deployment.cancel(); } };
  assert.equal((await deployment.install(device, pkg)).phase, 'canceled');
  assert.equal(mock.writes.some(l => l.includes(' BEGIN ')), false); await client.close();
});
test('cancel during DATA stops scheduling and confirms ABORT', async () => {
  const { pkg, mock, client, device, deployment } = await setup();
  deployment.onProgress = p => { if (p.acknowledged >= 512) { deployment.cancel(); } };
  const result = await deployment.install(device, pkg);
  assert.equal(result.phase, 'canceled'); assert.match(result.message, /cleanup confirmed/);
  assert.equal(mock.writes.filter(l => l.includes(' DATA ')).length, 1);
  assert.equal(mock.writes.some(l => l.includes(' INSTALL ')), false); await client.close();
});
test('concurrent upload refused while original completes', async () => {
  const { pkg, client, device, deployment } = await setup();
  const first = deployment.install(device, pkg);
  await assert.rejects(deployment.install(device, pkg), /existing operation/);
  assert.equal((await first).phase, 'completed'); await client.close();
});
test('loaded ELF blocks BEGIN, including suspended state', async () => {
  const { pkg, model, mock, client, device, deployment } = await setup(); model.loaded = true;
  const result = await deployment.install(device, pkg);
  assert.equal(result.phase, 'failed'); assert.match(result.message, /ELF/);
  assert.equal(mock.writes.some(l => l.includes(' BEGIN ')), false); await client.close();
});
test('Safe Mode permits supported diagnostics and blocks package writes', async () => {
  const { pkg, model, mock, client, device, deployment } = await setup(); model.safe = true;
  assert.equal((await deployment.install(device, pkg)).phase, 'failed');
  assert.equal(mock.writes.some(l => l.includes(' BEGIN ')), false); await client.close();
});
test('storage reserve and same-version policy prevent BEGIN', async () => {
  const { pkg, model, client, device } = await setup();
  model.installed = true;
  assert.equal(installationPlan(pkg, await device.refresh()).label, 'Same-version replacement');
  assert.equal(installationPlan(pkg, device.snapshot).allowed, false);
  model.installed = false; model.storage = 270000;
  assert.equal(installationPlan(pkg, await device.refresh()).allowed, false); await client.close();
});
test('unexpected serial device cannot admit writes', async () => {
  const { pkg, model, mock, client, device, deployment } = await setup();
  model.onCommand = protocol => protocol === 'RO1' ? { version: 'other' } : undefined;
  assert.equal((await deployment.install(device, pkg)).phase, 'failed');
  assert.equal(mock.writes.some(l => l.includes(' BEGIN ')), false); await client.close();
});
for (const stage of ['DATA', 'INSTALL']) {
  test(`disconnect during ${stage} preserves uncertainty and reconciles with a fresh handshake`, async () => {
    const { pkg, model, mock, client, device, deployment } = await setup();
    model.onCommand = (_protocol, operation, transport) => {
      if (operation.startsWith(stage + ' ')) {
        if (stage === 'INSTALL') { model.installed = true; model.phase = 0; model.total = 0; model.received = 0; }
        void transport.close(); return null;
      }
      return undefined;
    };
    const result = await deployment.install(device, pkg);
    assert.equal(result.phase, 'unknown'); assert.ok(deployment.uncertain);
    assert.equal(mock.writes.filter(l => l.includes(` ${stage} `)).length, 1);
    await client.close(); model.onCommand = undefined;
    // Simulate the real device's 10-second incomplete-upload expiration, not host cleanup.
    if (stage === 'DATA') { model.phase = 0; model.total = 0; model.received = 0; }
    const reconnect = new ProtocolClient(new MockTransport(model.handle));
    const resultAfterReconnect = await deployment.reconcile(new DeviceService(reconnect));
    assert.match(resultAfterReconnect.message, stage === 'INSTALL' ? /verified after reconnect/ : /interrupted before commit/);
    assert.equal(deployment.uncertain, undefined); await reconnect.close();
  });
}
test('cancel during commit does not reverse or abort it', async () => {
  const { pkg, mock, client, device, deployment } = await setup();
  deployment.onProgress = p => { if (p.phase === 'committing') { deployment.cancel(); } };
  assert.equal((await deployment.install(device, pkg)).phase, 'completed');
  assert.equal(mock.writes.some(l => l.includes(' ABORT ')), false); await client.close();
});
test('mismatched INFO never reports commit success', async () => {
  const { pkg, model, client, device, deployment } = await setup();
  model.onCommand = (_protocol, operation) => operation.startsWith('INFO ') && model.installed ? model.status({ apps: [[pkg.id, '9.0.0', pkg.abi, 0]], sha256: 'f'.repeat(64), elf_size: pkg.elfSize }) : undefined;
  assert.equal((await deployment.install(device, pkg)).phase, 'unknown'); await client.close();
});
test('bad DATA acknowledgement aborts instead of duplicating a chunk', async () => {
  const { pkg, model, mock, client, device, deployment } = await setup();
  model.onCommand = (_protocol, operation) => operation.startsWith('DATA ') ? model.status({ received: 1 }) : undefined;
  assert.equal((await deployment.install(device, pkg)).phase, 'failed');
  assert.equal(mock.writes.filter(l => l.includes(' DATA ')).length, 1); await client.close();
});
test('ABORT rejection leaves explicit cleanup uncertainty', async () => {
  const { pkg, model, client, device, deployment } = await setup(); model.reject = 'ABORT';
  deployment.onProgress = p => { if (p.acknowledged >= 512) { deployment.cancel(); } };
  assert.equal((await deployment.install(device, pkg)).phase, 'unknown');
  assert.equal(deployment.uncertain?.commitSent, false); await client.close();
});
test('INSTALL error after active selection is reconciled by INFO rather than blindly failed', async () => {
  const { pkg, model, mock, client, device, deployment } = await setup();
  model.onCommand = (_protocol, operation) => {
    if (operation.startsWith('INSTALL ')) {
      model.installed = true; model.phase = 0; model.total = model.received = 0;
      return model.status({ responseCode: -1, error: 'registry-read' });
    }
    return undefined;
  };
  assert.equal((await deployment.install(device, pkg)).phase, 'completed');
  assert.equal(mock.writes.filter(l => l.includes(' INSTALL ')).length, 1); await client.close();
});
test('reconciliation while device remains busy blocks a second upload', async () => {
  const { pkg, model, client, device, deployment } = await setup();
  model.phase = 1;
  deployment.uncertain = { id: pkg.id, version: pkg.version, digest: pkg.digest, abi: pkg.abi, elfSize: pkg.elfSize, commitSent: false };
  assert.equal((await deployment.reconcile(device)).outcome, 'pending');
  await assert.rejects(deployment.install(device, pkg), /existing operation/); await client.close();
});

test('Web Serial capability checks secure context and API presence', () => {
  assert.match(serialCapability({}, true)!, /unavailable/);
  assert.match(serialCapability({ serial: {} }, false)!, /localhost/);
  assert.equal(serialCapability({ serial: {} }, true), null);
});
test('WebSerialTransport uses native stream ownership and releases both locks on close', async () => {
  let signals: unknown; let options: unknown; let closed = false;
  const written: Uint8Array[] = [];
  const readable = new ReadableStream<Uint8Array>();
  const writable = new WritableStream<Uint8Array>({ write: bytes => { written.push(bytes); } });
  const port: SerialPortLike = { readable, writable, open: async value => { options = value; },
    setSignals: async value => { signals = value; }, close: async () => { assert.equal(readable.locked, false); assert.equal(writable.locked, false); closed = true; } };
  const transport = await WebSerialTransport.connect({ requestPort: async () => port });
  assert.equal(readable.locked, true); assert.equal(writable.locked, true);
  await transport.write(new Uint8Array([0, 10, 255]));
  assert.deepEqual(written[0], new Uint8Array([0, 10, 255]));
  assert.equal((options as { baudRate: number }).baudRate, 115200);
  assert.deepEqual(signals, { dataTerminalReady: true, requestToSend: true });
  const read = transport.read(); await transport.close(); assert.equal(await read, null);
  assert.equal(closed, true); await transport.close();
});
