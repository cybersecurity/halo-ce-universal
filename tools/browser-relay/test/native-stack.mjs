/** Real transport fixture: WebSocket -> worker -> encrypted native UDP/KCP.
 * This uses a local MQTT broker and native echo host, not a Halo match.
 * Run with NATIVE_WORKER=<HALO_RELAY_TEST binary> npm run test:native.
 */
import assert from 'node:assert/strict';
import { once } from 'node:events';
import { spawn } from 'node:child_process';
import { randomBytes } from 'node:crypto';
import { WebSocket } from 'ws';
import { createRelay } from '../server.mjs';
import { RecordDecoder } from '../protocol.mjs';
import { broker } from './mqtt-fixture.mjs';

const executable = process.env.NATIVE_WORKER;
if (!executable) throw new Error('Set NATIVE_WORKER to the native test binary (never the production binary).');
const loader = process.env.NATIVE_LOADER;
const command = loader || executable;
const args = loader ? ['--library-path', process.env.NATIVE_LIBRARY_PATH, executable] : [];

class Inbox {
  items = [];
  waiting = [];
  error = null;
  push(value) { const next = this.waiting.shift(); if (next) next.resolve(value); else this.items.push(value); }
  fail(error) {
    this.error = error;
    for (const waiter of this.waiting.splice(0)) waiter.reject(error);
  }
  take(context = 'native event') {
    if (this.error) return Promise.reject(this.error);
    if (this.items.length) return Promise.resolve(this.items.shift());
    return new Promise((resolve, reject) => {
      const waiter = {
        resolve: (value) => { clearTimeout(timer); resolve(value); },
        reject: (error) => { clearTimeout(timer); reject(error); },
      };
      const timer = setTimeout(() => {
        this.waiting.splice(this.waiting.indexOf(waiter), 1);
        reject(new Error(`Local native fixture timed out waiting for ${context}`));
      }, 20000);
      this.waiting.push(waiter);
    });
  }
}


function packet(kind, source, destination, sourcePort, destinationPort, payload = Buffer.alloc(0)) {
  const bytes = Buffer.alloc((24 + payload.length + 3) & ~3);
  bytes.writeUInt32LE(bytes.length, 0); bytes.writeUInt32LE(kind, 4);
  bytes.writeUInt32LE(source, 8); bytes.writeUInt32LE(destination, 12);
  bytes.writeUInt16BE(sourcePort, 16); bytes.writeUInt16BE(destinationPort, 18);
  bytes.writeUInt32LE(payload.length, 20); payload.copy(bytes, 24);
  return bytes;
}

const fixture = broker();
fixture.server.listen(0, '127.0.0.1');
await once(fixture.server, 'listening');
const workerEnv = { HALO_RELAY_TEST_BROKER: `127.0.0.1:${fixture.server.address().port}` };
const native = spawn(command, [...args, '--echo-host'], { env: { PATH: '/usr/bin:/bin', ...workerEnv }, stdio: ['pipe', 'pipe', 'pipe'] });
let relay, browser;
const events = new Inbox(), decoder = new RecordDecoder();
let nativeDiagnostics = '';
native.stderr.on('data', (data) => { nativeDiagnostics = (nativeDiagnostics + data).slice(-4096); });
native.stdout.on('data', (bytes) => decoder.push(bytes, (type, data) => {
  // The fixture host also reports local stream CLOSE frames as clients leave.
  // Those are not its control events and do not need a browser consumer.
  assert.ok(type === 2 || type === 3);
  if (type === 3) events.push(JSON.parse(data.toString()));
}));
native.on('error', (error) => events.fail(error));
native.on('exit', (code, signal) => events.fail(new Error(`Native fixture exited (${signal || code})`)));
try {
  assert.equal((await events.take()).type, 'ready');
  const invitation = await events.take();
  assert.equal(invitation.type, 'test_invite');
  const invite = invitation.invite.replace('halo://join/', '');
  const accessToken = randomBytes(32).toString('hex');
  relay = createRelay({ host: '127.0.0.1', port: 0, accessToken,
    allowedOrigins: ['http://localhost:8780'], worker: command, workerArgs: args, workerEnv });
  const endpoint = await relay.listen();
  browser = new WebSocket(`ws://127.0.0.1:${endpoint.port}/join`, { origin: 'http://localhost:8780' });
  const inbox = new Inbox();
  browser.on('message', (bytes, binary) => inbox.push(binary ? Buffer.from(bytes) : JSON.parse(bytes.toString())));
  browser.on('error', (error) => inbox.fail(error));
  browser.on('close', (code) => inbox.fail(new Error(`Fixture WebSocket closed (${code})`)));
  await once(browser, 'open');
  browser.send(JSON.stringify({ type: 'join', invite, accessToken }));
  const ready = await inbox.take(), peer = await inbox.take();
  assert.equal(ready.type, 'ready'); assert.equal(peer.type, 'peer'); assert.equal(peer.connected, true);
  assert.equal(peer.identifier, invite.slice(0, 12));
  const source = ready.address, destination = peer.address;
  for (const address of [destination, 0xffffffff]) {
    const data = Buffer.from(Array.from({ length: 256 }, (_, i) => i));
    browser.send(packet(1, source, address, 5151, 5150, data));
    const response = await inbox.take();
    assert.ok(Buffer.isBuffer(response));
    assert.equal(response.readUInt32LE(4), 1);
    assert.equal(response.readUInt32LE(8), destination); assert.equal(response.readUInt32LE(12), source);
    assert.equal(response.readUInt16BE(16), 5150); assert.equal(response.readUInt16BE(18), 5151);
    assert.deepEqual(response.subarray(24, 24 + data.length), data);
  }
  console.log('PASS: WebSocket frontend -> native encrypted UDP unicast and discovery broadcast');
  for (let connection = 0; connection < 20; connection++) {
    const port = 49157 + connection;
    // The native fixture echoes its first stream only. Later streams are sinks
    // that report bytes/checksum on close, exercising DATA+CLOSE drain without
    // waiting for a response before closing the browser side.
    const expected = Buffer.from(Array.from({ length: connection === 0 ? 25600 : connection === 1 ? 65536 : 64 },
      (_, i) => (i + connection) & 255));
    browser.send(packet(2, source, destination, port, 5150));
    for (let offset = 0; offset < expected.length; offset += 8000)
      browser.send(packet(3, source, destination, port, 5150, expected.subarray(offset, offset + 8000)));
    if (connection === 0) {
      let received = Buffer.alloc(0);
      while (received.length < expected.length) {
        const response = await inbox.take(`stream 1 echo (${received.length}/${expected.length} bytes)`);
        assert.ok(Buffer.isBuffer(response)); assert.equal(response.readUInt32LE(4), 3);
        received = Buffer.concat([received, response.subarray(24, 24 + response.readUInt32LE(20))]);
      }
      assert.deepEqual(received, expected);
    }
    browser.send(packet(4, source, destination, port, 5150));
    const closed = await events.take(`native stream ${connection + 1} close receipt`);
    let checksum = 0;
    for (const byte of expected) checksum = (Math.imul(checksum, 16777619) ^ byte) >>> 0;
    assert.deepEqual(closed, { type: 'test_stream_closed', bytes: expected.length, checksum });
  }
  console.log('PASS: WebSocket/native KCP echo, immediate CLOSE drains 65,536 bytes, and 20 connect/close cycles');
  const close = once(browser, 'close');
  browser.send(packet(1, source, 0x0100007f, 5151, 5150, Buffer.from('blocked')));
  assert.equal((await close)[0], 1008);
  const end = Date.now() + 3000;
  while (relay.sessions && Date.now() < end) await new Promise((resolve) => setTimeout(resolve, 10));
  assert.equal(relay.sessions, 0);
  console.log('PASS: frontend rejects non-peer destination and terminates its native worker');
  console.log('Transport fixture only: no Halo game or public native host was tested.');
} finally {
  browser?.terminate();
  if (relay) await relay.close();
  if (native.exitCode === null && native.signalCode === null && native.pid) {
    const exited = once(native, 'exit');
    native.kill('SIGTERM'); await exited;
  }
  fixture.close();
  assert.equal(nativeDiagnostics, '');
}
