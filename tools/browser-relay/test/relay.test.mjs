import test from 'node:test';
import assert from 'node:assert/strict';
import { once } from 'node:events';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { WebSocket } from 'ws';
import { createRelay, validateConfig } from '../server.mjs';
import { RecordDecoder, SessionPackets, record } from '../protocol.mjs';

const ORIGIN = 'https://fqlx.github.io';
const INVITE = 'abcdef012345' + '01'.repeat(16);
const TOKEN = 'test-only-capability-'.repeat(3);
const MOCK = fileURLToPath(new URL('./mock-worker.mjs', import.meta.url));
const LOCAL = 0x02004064, PEER = 0x03004064;

function frame({ kind = 1, source = LOCAL, destination = PEER, sourcePort = 5150, destinationPort = 5150,
  data = Buffer.from('game') } = {}) {
  const bytes = Buffer.alloc((24 + data.length + 3) & ~3);
  bytes.writeUInt32LE(bytes.length, 0); bytes.writeUInt32LE(kind, 4);
  bytes.writeUInt32LE(source, 8); bytes.writeUInt32LE(destination, 12);
  bytes.writeUInt16BE(sourcePort, 16); bytes.writeUInt16BE(destinationPort, 18);
  bytes.writeUInt32LE(data.length, 20); data.copy(bytes, 24);
  return bytes;
}

async function fixture(t, options = {}) {
  const dir = await mkdtemp(join(tmpdir(), 'halo-relay-test-'));
  const pidFile = join(dir, 'worker.pid');
  const { mode = 'echo', ...overrides } = options;
  const relay = createRelay({ host: '127.0.0.1', port: 0, allowedOrigins: [ORIGIN], accessToken: TOKEN,
    worker: process.execPath, workerArgs: [MOCK, mode, pidFile], ...overrides });
  const address = await relay.listen();
  t.after(async () => { await relay.close(); await rm(dir, { recursive: true, force: true }); });
  const url = `ws://127.0.0.1:${address.port}/join`;
  return { relay, url, pidFile };
}

async function connect(url, invite = INVITE, accessToken = TOKEN) {
  const ws = new WebSocket(url, { origin: ORIGIN });
  const messages = [];
  ws.on('message', (bytes, binary) => messages.push(binary ? Buffer.from(bytes) : JSON.parse(bytes.toString())));
  await once(ws, 'open');
  ws.send(JSON.stringify({ type: 'join', invite, accessToken }));
  return { ws, messages };
}

async function until(predicate, timeout = 3000) {
  const end = Date.now() + timeout;
  while (!predicate()) {
    if (Date.now() > end) throw new Error('Timed out waiting for test condition');
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
}

test('public binding fails closed without capability; only exact HTTPS origins are accepted', () => {
  assert.throws(() => validateConfig({ host: '0.0.0.0' }), /ACCESS_TOKEN/);
  assert.throws(() => validateConfig({ allowedOrigins: ['https://example.com/path'] }), /Origins/);
  assert.throws(() => validateConfig({ allowedOrigins: ['http://example.com'] }), /Origins/);
  assert.throws(() => validateConfig({ allowedOrigins: ['*'] }));
  assert.equal(validateConfig({ host: '0.0.0.0', accessToken: TOKEN, allowedOrigins: [ORIGIN] }).accessToken, TOKEN);
});

test('record decoder handles split/coalesced records and rejects size bombs', () => {
  const decoder = new RecordDecoder(), received = [];
  const bytes = Buffer.concat([record(1, INVITE), record(2, 'payload')]);
  for (const byte of bytes) decoder.push(Buffer.from([byte]), (type, payload) => received.push([type, payload.toString()]));
  assert.deepEqual(received, [[1, INVITE], [2, 'payload']]);
  assert.equal(decoder.pendingBytes, 0);
  assert.throws(() => decoder.push(Buffer.from([255, 255, 255, 255]), () => {}), /record size/);
});

test('packet ACL rejects arbitrary addresses/ports, bad lengths and untracked streams', () => {
  const packets = new SessionPackets(LOCAL, 1);
  packets.peer(PEER, true);
  packets.validate(frame(), true);
  packets.validate(frame({ destination: 0xffffffff }), true);
  assert.throws(() => packets.validate(frame({ destination: 0x0100007f }), true), /outside/);
  assert.throws(() => packets.validate(frame({ source: PEER }), true), /outside/);
  assert.throws(() => packets.validate(frame({ destinationPort: 53 }), true), /port/);
  assert.throws(() => packets.validate(frame({ data: Buffer.alloc(1396) }), true), /payload/);
  assert.throws(() => packets.validate(frame({ kind: 3 }), true), /Unknown/);
  const damaged = frame(); damaged.writeUInt32LE(99, 20);
  assert.throws(() => packets.validate(damaged, true), /header/);
  packets.validate(frame({ kind: 2, sourcePort: 6000, data: Buffer.alloc(0) }), true);
  packets.validate(frame({ kind: 3, sourcePort: 6000 }), true);
  assert.throws(() => packets.validate(frame({ kind: 2, sourcePort: 6001, data: Buffer.alloc(0) }), true), /Too many/);
  packets.validate(frame({ kind: 4, sourcePort: 6000, data: Buffer.alloc(0) }), true);
  assert.equal(packets.validate(frame({ kind: 3, sourcePort: 6000 }), true).discard, true);
  assert.throws(() => packets.validate(frame({ kind: 2, sourcePort: 6000, data: Buffer.alloc(0) }), true), /reused/);
  packets.validate(frame({ kind: 4, source: PEER, destination: LOCAL, destinationPort: 6000, data: Buffer.alloc(0) }), false);
  assert.equal(packets.streams.size, 0);
});

test('one-sided CLOSE releases stream slots across repeated reconnects', () => {
  const packets = new SessionPackets(LOCAL);
  packets.peer(PEER, true);
  for (let n = 0; n < 100; n++) {
    const port = 6000 + n;
    packets.validate(frame({ kind: 2, sourcePort: port, data: Buffer.alloc(0) }), true);
    const closing = frame({ kind: 4, source: PEER, destination: LOCAL,
      destinationPort: port, data: Buffer.alloc(0) });
    packets.validate(closing, false);
    assert.equal(packets.streams.size, 0);
    assert.equal(packets.validate(closing, false).discard, true);
    assert.equal(packets.validate(frame({ kind: 3, sourcePort: port }), true).discard, true);
  }
  assert.equal(packets.closed.size, 100);
});

test('health is read-only and WebSocket upgrade rejects wrong origin/path', async (t) => {
  const { url, relay, pidFile } = await fixture(t);
  const health = await fetch(url.replace('ws:', 'http:').replace('/join', '/health'));
  assert.equal(health.status, 200);
  assert.deepEqual(await health.json(), { service: 'halo-browser-relay', protocol: 1, status: 'ok' });
  for (const [target, origin, expected] of [[url, 'https://attacker.example', 403], [url + '?token=secret', ORIGIN, 404]]) {
    const client = new WebSocket(target, { origin });
    const response = await new Promise((resolve) => client.on('unexpected-response', (_, result) => {
      result.resume(); resolve(result.statusCode); client.terminate();
    }).on('error', () => {}));
    assert.equal(response, expected);
  }
  assert.equal(relay.sessions, 0);
  await assert.rejects(readFile(pidFile), { code: 'ENOENT' });
});

test('invalid invite or capability closes before spawning native worker', async (t) => {
  const { url, pidFile } = await fixture(t);
  for (const [invite, token] of [['not-an-invite', TOKEN], [INVITE, 'wrong']]) {
    const { ws } = await connect(url, invite, token);
    const [code] = await once(ws, 'close');
    assert.equal(code, 1008);
  }
  await assert.rejects(readFile(pidFile), { code: 'ENOENT' });
});

test('mock worker handshake, framed datagram roundtrip and disconnect cleanup', async (t) => {
  const { url, relay, pidFile } = await fixture(t);
  const { ws, messages } = await connect(url);
  await until(() => messages.some((m) => m.type === 'peer'));
  assert.deepEqual(messages[0], { type: 'ready', protocol: 1, identifier: '112233445566', address: LOCAL });
  assert.equal(messages[1].identifier, INVITE.slice(0, 12));
  ws.send(frame());
  await until(() => messages.some(Buffer.isBuffer));
  const reply = messages.find(Buffer.isBuffer);
  assert.equal(reply.readUInt32LE(8), PEER);
  assert.equal(reply.subarray(24).toString(), 'game');
  const pid = Number(await readFile(pidFile, 'utf8'));
  const closed = once(ws, 'close'); ws.close(); await closed;
  await until(() => relay.sessions === 0);
  assert.throws(() => process.kill(pid, 0), { code: 'ESRCH' });
});

test('worker crash and malformed output close client with bounded errors', async (t) => {
  for (const mode of ['exit', 'malformed']) {
    await t.test(mode, async (t) => {
      const { url } = await fixture(t, { mode });
      const { ws, messages } = await connect(url);
      const closed = once(ws, 'close');
      if (mode === 'malformed') {
        await until(() => messages.some((m) => m.type === 'peer'));
        ws.send(frame());
      }
      assert.equal((await closed)[0], 1011);
      assert.ok(messages.some((m) => m.type === 'error'));
      assert.ok(!JSON.stringify(messages).includes(INVITE));
    });
  }
});

test('session cap rejects additional sockets; invalid frame cannot reach worker', async (t) => {
  const { url } = await fixture(t, { maxSessions: 1 });
  const { ws, messages } = await connect(url);
  await until(() => messages.some((m) => m.type === 'peer'));
  const second = new WebSocket(url, { origin: ORIGIN });
  const status = await new Promise((resolve) => second.on('unexpected-response', (_, result) => {
    result.resume(); resolve(result.statusCode); second.terminate();
  }).on('error', () => {}));
  assert.equal(status, 503);
  const closed = once(ws, 'close');
  ws.send(frame({ destination: 0x08080808, destinationPort: 53 }));
  assert.equal((await closed)[0], 1008);
  assert.equal(messages.filter(Buffer.isBuffer).length, 0);
});

test('silent worker startup and incoming packet flood are bounded', async (t) => {
  await t.test('startup timeout', async (t) => {
    const { url, relay } = await fixture(t, { mode: 'silent', joinTimeoutMs: 30 });
    const { ws } = await connect(url);
    assert.equal((await once(ws, 'close'))[0], 1008);
    await until(() => relay.sessions === 0);
  });
  await t.test('rate limit', async (t) => {
    const { url } = await fixture(t, { packetsPerSecond: 2 });
    const { ws, messages } = await connect(url);
    await until(() => messages.some((m) => m.type === 'peer'));
    const closed = once(ws, 'close');
    for (let i = 0; i < 10; i++) ws.send(frame());
    assert.equal((await closed)[0], 1008);
  });
});

test('stalled worker input is paused and the session closes instead of silently losing stream bytes', async (t) => {
  const { url, relay } = await fixture(t, { mode: 'slow', joinTimeoutMs: 100,
    backpressureTimeoutMs: 60, packetsPerSecond: 100000, bytesPerSecond: 100000000 });
  const { ws, messages } = await connect(url);
  await until(() => messages.some((m) => m.type === 'peer'));
  const closed = once(ws, 'close');
  ws.send(frame({ kind: 2, sourcePort: 6000, data: Buffer.alloc(0) }));
  const payload = frame({ kind: 3, sourcePort: 6000, data: Buffer.alloc(16000) });
  for (let index = 0; index < 200; index++) ws.send(payload);
  assert.equal((await closed)[0], 1013);
  await until(() => relay.sessions === 0);
});
