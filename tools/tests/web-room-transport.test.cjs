const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

function fixture(options = {}) {
  const intervals = [], connections = [];
  let now = 0;
  class RTC {
    constructor() { this.channels = []; connections.push(this); }
    createDataChannel() {
      const channel = { readyState: 'open', bufferedAmount: 0, sent: [],
        send(packet) { this.sent.push(packet.slice()); } };
      this.channels.push(channel);
      return channel;
    }
    close() { this.closed = true; }
    async setRemoteDescription(value) {
      this.remoteDescription = value;
      await options.remoteDescription?.(this, value);
    }
    async setLocalDescription(value) { this.localDescription = value; }
    async createOffer() { return { type: 'offer', sdp: 'test offer' }; }
    async createAnswer() { this.answers = (this.answers || 0) + 1; return { type: 'answer', sdp: 'test answer' }; }
    async addIceCandidate() {}
  }
  const context = { RTCPeerConnection: RTC, Uint8Array, Int32Array, DataView, Atomics,
    Date: class extends Date { static now() { return now; } },
    crypto: { getRandomValues(bytes) { bytes.fill(1); return bytes; } },
    localStorage: { getItem() { return null; }, setItem() {} },
    setInterval(fn) { intervals.push(fn); }, clearInterval() {}, clearTimeout() {},
  };
  // Expose only the boundary needed by this isolated VM; run the production
  // channel callbacks, ring writes and pump unchanged, without an MQTT server.
  let source = fs.readFileSync(require.resolve('../../port/web/site/net.js'), 'utf8');
  assert.ok(source.includes('return { attach, join, leave,'));
  source = source.replace('return { attach, join, leave,',
    'return { get fixtureState() { return state; }, peerFor, createConnection, dropPeer, handleSignal, pump, sweep, attach, join, leave,');
  vm.runInNewContext(source + '\nglobalThis.net = HaloNet; globalThis.Coordinator = HaloQuickCoordinator;', context);
  const net = context.net;
  const memory = { buffer: new SharedArrayBuffer(4096) };
  const offsets = { netInWrite: 0, netInRead: 4, netOutWrite: 8, netOutRead: 12,
    netLocalAddress: 16, netIn: 64, netOut: 320, netInBytes: 256, netOutBytes: 256,
    pingSequence: 576, pingHost: 580, pingEpoch: 584, pingUpdated: 588, pingCount: 592,
    pingPeers: 600, pingPeerCount: 128 };
  net.attach({ memory, base: 0, offsets });
  if (options.id) net.fixtureState.id = options.id;
  const peer = net.peerFor(options.peerId || '2222222222222222', options.peerAddress || 0x0201010a, 'Peer');
  const pc = net.createConnection(peer);
  peer.reliable.onopen();
  const words = new Int32Array(memory.buffer), bytes = new Uint8Array(memory.buffer);
  return { net, peer, pc, words, bytes, offsets, intervals, connections,
    now(time) { now = time; },
    tick(time) { now = time; net.sweep(); },
    receive(packet) { peer.reliable.onmessage({ data: packet.buffer }); },
    authority(epoch = 0, host = peer) {
      const state = net.fixtureState;
      state.address = options.address || 0x0101010a; words[4] = state.address;
      const coordinator = new context.Coordinator(state.id, state.address, now);
      coordinator.epoch = epoch;
      coordinator.result = { role: 'join', hostId: host.id, hostAddress: host.address };
      coordinator.presence = { role: 'join', hostId: host.id, phase: 'launched',
        gamePhase: 'playing', epoch, failover: true, migration: true, matchId: 500, checkpointTick: 90 };
      state.quick = { coordinator, resolved: true };
      host.quick = { role: 'host', hostId: host.id, phase: 'launched',
        gamePhase: 'playing', epoch, failover: true, migration: true, matchId: 500, checkpointTick: 90 };
      return coordinator;
    },
    consume() {
      const result = [];
      let read = words[1] >>> 0;
      const write = words[0] >>> 0;
      while (read !== write) result.push(bytes[64 + ((read++) & 255)]);
      Atomics.store(words, 1, read);
      return result;
    },
  };
}

function packet(kind, length = 24, fill = 0) {
  const bytes = new Uint8Array(length), view = new DataView(bytes.buffer);
  bytes.fill(fill, 24);
  view.setUint32(0, length, true);
  view.setUint32(4, kind, true);
  view.setUint32(12, 0x0201010a, true);
  return bytes;
}

function establishOutgoing(f, peer = f.peer) {
  const open = packet(2);
  f.bytes.set(open, 320); f.words[2] = open.length;
  f.net.pump();
  assert.equal(peer.reliable.sent.length, 1);
  peer.reliable.sent.length = 0;
  f.words[2] = f.words[3] = 0;
}

function streamPacket(f, kind, outgoing = false, length = 24) {
  const bytes = packet(kind, length), view = new DataView(bytes.buffer);
  view.setUint32(8, outgoing ? f.words[4] : f.peer.address, true);
  view.setUint32(12, outgoing ? f.peer.address : f.words[4], true);
  view.setUint16(16, outgoing ? 49152 : 5152, true);
  view.setUint16(18, outgoing ? 5152 : 49152, true);
  return bytes;
}

function packets(bytes) {
  const result = [];
  for (let offset = 0; offset < bytes.length;) {
    const packet = Uint8Array.from(bytes.slice(offset));
    const size = new DataView(packet.buffer).getUint32(0, true);
    assert.ok(size >= 24 && size <= packet.length);
    result.push(packet.slice(0, size));
    offset += size;
  }
  return result;
}

function migrationFrame(packet, epoch) {
  const wire = new Uint8Array(packet.length + 8), view = new DataView(wire.buffer);
  view.setUint32(0, 0x484d4731, true);
  view.setUint32(4, epoch, true);
  wire.set(packet, 8);
  return wire;
}

test('quick-play frames carry authority epochs without reducing the largest game packet', () => {
  const f = fixture(); f.authority(3);
  f.receive(migrationFrame(streamPacket(f, 2), 3)); f.consume();
  const largest = streamPacket(f, 3, false, 256);
  f.receive(migrationFrame(largest, 3));
  assert.deepEqual(f.consume(), Array.from(largest), 'the wrapper stays outside the engine receive ring');
  assert.equal(f.pc.closed, undefined);
  const outgoing = streamPacket(f, 1, true, 256);
  f.bytes.set(outgoing, 320); f.words[2] = outgoing.length;
  f.net.pump();
  assert.deepEqual(Array.from(f.peer.unreliable.sent.at(-1)), Array.from(migrationFrame(outgoing, 3)));
});

test('stale authority and unwrapped legacy frames never enter a running quick-play match', () => {
  for (const unreliable of [false, true]) {
    const f = fixture(); f.authority(3);
    const current = streamPacket(f, unreliable ? 1 : 2);
    const receive = data => (unreliable ? f.peer.unreliable : f.peer.reliable).onmessage({ data: data.buffer });
    receive(migrationFrame(current, 2));
    receive(current);
    assert.deepEqual(f.consume(), [], 'old authority cannot write into the game packet ring');
    receive(migrationFrame(current, 3));
    assert.deepEqual(f.consume(), Array.from(current));
  }
});

test('reliable frames queued under backpressure are fenced when authority changes', () => {
  const f = fixture(), coordinator = f.authority(0);
  f.words[0] = 256; // The engine has not drained any of the full input ring.
  const old = streamPacket(f, 2);
  f.receive(migrationFrame(old, 0));
  assert.ok(f.peer.receivedBytes > 0);
  coordinator.epoch = coordinator.presence.epoch = 1;
  f.peer.quick.epoch = 1;
  f.words[1] = f.words[0];
  f.net.pump();
  assert.deepEqual(f.consume(), [], 'a buffered old-epoch OPEN cannot recreate the previous host connection');
  assert.equal(f.peer.receivedBytes, 0);
  f.receive(migrationFrame(old, 1));
  assert.deepEqual(f.consume(), Array.from(old));
});

test('an epoch wrapper cannot authorize a packet claiming a different peer address', () => {
  const f = fixture(); f.authority(1);
  const forged = streamPacket(f, 2);
  new DataView(forged.buffer).setUint32(8, 0x0301010a, true);
  f.receive(migrationFrame(forged, 1));
  assert.deepEqual(f.consume(), [], 'the WebRTC sender and engine packet source must agree');
});

test('reliable OPEN, DATA and CLOSE survive a full receive ring in order', () => {
  const f = fixture();
  const packets = [packet(2), packet(3, 180, 1), packet(3, 180, 2), packet(4)];
  for (const p of packets) f.receive(p);
  assert.ok(f.peer.receivedBytes > 0, 'overflow remains queued instead of being dropped');
  const received = f.consume();
  f.net.pump();
  received.push(...f.consume());
  assert.deepEqual(received, packets.flatMap(p => Array.from(p)));
  assert.equal(f.peer.receivedBytes, 0);
  assert.equal(f.peer.received.length, 0);
});

test('outbound reliable bytes stay ordered in a bounded peer queue under WebRTC backpressure', () => {
  const f = fixture(), p = packet(3, 180, 7);
  establishOutgoing(f);
  f.bytes.set(p, 320); f.words[2] = p.length;
  f.peer.reliable.bufferedAmount = 1024 * 1024;
  f.net.pump();
  assert.equal(f.words[3], p.length);
  assert.equal(f.peer.outgoingBytes, p.length);
  assert.equal(f.peer.reliable.sent.length, 0);
  f.peer.reliable.bufferedAmount = 0;
  f.peer.reliable.onbufferedamountlow();
  assert.equal(f.words[3], p.length);
  assert.deepEqual(Array.from(f.peer.reliable.sent[0]), Array.from(p));
  assert.equal(f.peer.outgoingBytes, 0);
});

test('a stalled player cannot block the host from sending keepalives to healthy players', () => {
  const f = fixture(); establishOutgoing(f);
  const healthy = f.net.peerFor('3333333333333333', 0x0301010a, 'Healthy');
  f.net.createConnection(healthy); healthy.reliable.onopen();
  f.peer.reliable.bufferedAmount = 1024 * 1024;
  const slow = packet(3, 80, 7), fast = packet(2);
  new DataView(fast.buffer).setUint32(12, healthy.address, true);
  f.bytes.set(slow, 320); f.bytes.set(fast, 320 + slow.length);
  f.words[2] = slow.length + fast.length;
  f.net.pump();
  assert.equal(healthy.reliable.sent.length, 1, 'healthy traffic passes the stalled peer');
  assert.deepEqual(Array.from(healthy.reliable.sent[0]), Array.from(fast));
  assert.equal(f.peer.outgoingBytes, slow.length);
  assert.equal(f.words[3], f.words[2]);
});

test('queued outgoing traffic from an older authority never reaches a replacement host', () => {
  const f = fixture(), coordinator = f.authority(0);
  const open = streamPacket(f, 2, true);
  f.peer.reliable.bufferedAmount = 1024 * 1024;
  f.bytes.set(open, 320); f.words[2] = open.length; f.net.pump();
  coordinator.epoch = coordinator.presence.epoch = 1;
  f.peer.reliable.bufferedAmount = 0; f.net.pump();
  assert.equal(f.peer.reliable.sent.length, 0);
  assert.equal(f.peer.outgoingBytes, 0);
});

test('send queue overflow disconnects only the stalled player and continues healthy traffic', () => {
  const f = fixture(); establishOutgoing(f);
  const healthy = f.net.peerFor('3333333333333333', 0x0301010a, 'Healthy');
  f.net.createConnection(healthy); healthy.reliable.onopen();
  f.peer.reliable.bufferedAmount = 1024 * 1024;
  const slow = packet(3, 256, 9);
  for (let i = 0; i <= 4096; i++) {
    f.words[2] = f.words[3] = 0; f.bytes.set(slow, 320); f.words[2] = slow.length;
    f.net.pump();
  }
  assert.equal(f.pc.closed, true);
  assert.equal(f.peer.outgoingBytes, 0);
  assert.equal(f.net.status().players, 1);
  const fast = packet(2); new DataView(fast.buffer).setUint32(12, healthy.address, true);
  f.words[2] = f.words[3] = 0; f.bytes.set(fast, 320); f.words[2] = fast.length;
  f.net.pump();
  assert.deepEqual(Array.from(healthy.reliable.sent[0]), Array.from(fast));
  assert.equal(f.words[3], fast.length);
});

test('incoming network events flush output even without timer callbacks', () => {
  const f = fixture(), p = packet(3, 80, 9);
  establishOutgoing(f);
  f.bytes.set(p, 320); f.words[2] = p.length;
  f.peer.unreliable.onmessage({ data: packet(1).buffer });
  assert.equal(f.words[3], p.length);
  assert.equal(f.peer.reliable.sent.length, 1);
});

test('failed send retains stream bytes and malformed input disconnects', () => {
  const f = fixture(), p = packet(3, 80, 9);
  establishOutgoing(f);
  f.bytes.set(p, 320); f.words[2] = p.length;
  f.peer.reliable.send = () => { throw new Error('buffer full'); };
  f.net.pump();
  assert.equal(f.words[3], p.length);
  assert.equal(f.peer.outgoingBytes, p.length);
  const malformed = packet(3, 40);
  new DataView(malformed.buffer).setUint32(0, 80, true);
  f.receive(malformed);
  assert.equal(f.pc.closed, true);
  assert.equal(f.net.status().players, 0);
});

test('removed RTC channels cannot reopen a peer or inject late packets', () => {
  const f = fixture(), reliable = f.peer.reliable, unreliable = f.peer.unreliable;
  f.words[0] = 256; // a full ring leaves a reliable packet in the peer queue
  f.receive(packet(3, 80));
  assert.equal(f.peer.receivedBytes, 80);
  f.net.dropPeer(f.peer);
  assert.equal(f.pc.closed, true);
  assert.equal(f.peer.pc, null);
  assert.equal(f.peer.receivedBytes, 0);
  assert.equal(f.peer.received.length, 0);
  reliable.onopen();
  reliable.onmessage({ data: packet(2).buffer });
  unreliable.onmessage({ data: packet(1).buffer });
  assert.equal(f.peer.open, false);
  assert.equal(f.net.status().players, 0);
  assert.equal(f.words[0], 256, 'stale channels do not write into the native receive ring');
  assert.equal(f.peer.receivedBytes, 0);
});

test('connection replacement waits for the new channel and ignores every old callback', () => {
  const f = fixture(), oldReliable = f.peer.reliable, oldUnreliable = f.peer.unreliable;
  const current = f.net.createConnection(f.peer);
  assert.equal(f.pc.closed, true);
  assert.equal(f.peer.open, false, 'the previous open channel does not make negotiation look complete');
  oldReliable.onopen();
  oldReliable.onmessage({ data: packet(2).buffer });
  oldUnreliable.onmessage({ data: packet(1).buffer });
  oldReliable.onclose();
  assert.equal(f.words[0], 0);
  assert.equal(f.peer.pc, current);
  assert.equal(f.net.status().players, 0);
  f.peer.reliable.onopen();
  assert.equal(f.net.status().players, 1);
  oldReliable.onclose();
  assert.equal(f.net.status().players, 1, 'an obsolete close cannot remove the new connection');
});

test('a reloaded peer replaces the old id at its address and routes output only to the new channel', async () => {
  const f = fixture(), oldUnreliable = f.peer.unreliable;
  const current = f.net.peerFor('3333333333333333', f.peer.address, 'Reloaded');
  f.net.createConnection(current);
  current.reliable.onopen();
  assert.equal(f.pc.closed, true);
  assert.equal(f.peer.pc, null);
  assert.equal(f.net.status().players, 1);
  assert.deepEqual(Array.from(f.net.status().names), ['Reloaded']);
  establishOutgoing(f, current);
  const outgoing = packet(3, 80, 9);
  f.bytes.set(outgoing, 320); f.words[2] = outgoing.length;
  f.net.pump();
  assert.equal(current.reliable.sent.length, 1);
  assert.equal(f.pc.channels[0].sent.length, 0);
  oldUnreliable.onmessage({ data: packet(1).buffer });
  assert.equal(f.words[0], 0);

  // Delayed signals from any broker must not resurrect the old tab and steal
  // the stable virtual IP from the replacement that is already connected.
  await f.net.handleSignal({ type: 'hello', from: f.peer.id, address: f.peer.address });
  await f.net.handleSignal({ type: 'offer', from: f.peer.id, address: f.peer.address, sdp: 'old offer' });
  assert.equal(f.connections.length, 2);
  assert.equal(f.net.status().players, 1);
  assert.equal(current.pc.closed, undefined);
});

test('superseded asynchronous offers cannot answer on an obsolete connection', async () => {
  let resumeFirst;
  const f = fixture({ remoteDescription(pc, value) {
    if (value.sdp === 'first') return new Promise(resolve => { resumeFirst = resolve; });
  } });
  const first = f.net.handleSignal({ type: 'offer', from: f.peer.id, address: f.peer.address, sdp: 'first' });
  const old = f.peer.pc;
  await f.net.handleSignal({ type: 'offer', from: f.peer.id, address: f.peer.address, sdp: 'second' });
  const current = f.peer.pc;
  assert.equal(old.closed, true);
  assert.equal(current.answers, 1);
  resumeFirst(); await first;
  assert.equal(old.answers, undefined, 'late negotiation completion stops before generating or publishing an answer');
  assert.equal(f.peer.pc, current);
});

test('leaving the room clears retired identities so they can join a fresh room', async () => {
  const f = fixture();
  f.net.peerFor('3333333333333333', f.peer.address, 'Reloaded');
  assert.equal(f.net.peerFor(f.peer.id, f.peer.address), null);
  await f.net.leave();
  assert.ok(f.net.peerFor(f.peer.id, f.peer.address));
});

test('RTC drop delivers native EOF for incoming and outgoing stream tuples', () => {
  for (const outgoing of [false, true]) {
    const f = fixture(), open = streamPacket(f, 2, outgoing);
    if (outgoing) {
      f.bytes.set(open, 320); f.words[2] = open.length;
      f.net.pump();
      assert.equal(f.peer.reliable.sent.length, 1);
    } else {
      f.receive(open);
      assert.equal(f.consume().length, 24);
    }
    f.net.dropPeer(f.peer);
    const expected = streamPacket(f, 4);
    assert.deepEqual(f.consume(), Array.from(expected), 'EOF uses the remote-to-local tuple');
    f.net.dropPeer(f.peer);
    assert.equal(f.consume().length, 0, 'duplicate callbacks do not inject another close');
  }
});

test('a full ring retains RTC EOF before the replacement OPEN and DATA', () => {
  const f = fixture();
  f.receive(streamPacket(f, 2)); f.consume();
  f.words[0] = f.words[1] + 256;
  const oldClose = f.peer.reliable.onclose;
  const current = f.net.peerFor('3333333333333333', f.peer.address, 'Reloaded');
  f.net.createConnection(current);
  current.reliable.onopen();
  current.reliable.onmessage({ data: streamPacket(f, 2).buffer });
  current.reliable.onmessage({ data: streamPacket(f, 3, false, 80).buffer });
  current.unreliable.onmessage({ data: packet(1).buffer });
  assert.equal(current.receivedBytes, 104, 'replacement bytes stay queued behind EOF');
  assert.equal(f.words[0] - f.words[1], 256, 'unreliable input cannot displace pending EOF');
  f.consume();
  f.net.pump();
  assert.deepEqual(packets(f.consume()).map(p => new DataView(p.buffer).getUint32(4, true)), [4, 2, 3]);
  assert.equal(current.receivedBytes, 0);
  oldClose();
  f.net.pump();
  assert.equal(f.consume().length, 0, 'the obsolete close cannot close the new generation');
  assert.equal(f.net.status().players, 1);
});

test('renegotiating an RTC connection closes its old native streams once', () => {
  const f = fixture();
  f.receive(streamPacket(f, 2)); f.consume();
  const oldClose = f.peer.reliable.onclose;
  f.net.createConnection(f.peer);
  f.net.pump();
  assert.deepEqual(f.consume(), Array.from(streamPacket(f, 4)));
  oldClose();
  assert.equal(f.consume().length, 0);
  assert.ok(f.peer.pc && !f.peer.pc.closed);
});

test('a delivered native CLOSE removes tracking while a queued CLOSE survives RTC drop', () => {
  for (const blocked of [false, true]) {
    const f = fixture();
    f.receive(streamPacket(f, 2)); f.consume();
    if (blocked) f.words[0] = f.words[1] + 256;
    f.receive(streamPacket(f, 4));
    if (!blocked) assert.deepEqual(f.consume(), Array.from(streamPacket(f, 4)));
    f.net.dropPeer(f.peer);
    if (blocked) { f.consume(); f.net.pump(); }
    assert.deepEqual(f.consume(), blocked ? Array.from(streamPacket(f, 4)) : [],
      'a queued wire close is replaced with retained EOF when its channel queue is discarded');
  }
});

test('a fresh RTC generation discards old reliable DATA until its stream OPEN', () => {
  const f = fixture();
  establishOutgoing(f);
  const current = f.net.peerFor('3333333333333333', f.peer.address, 'Reloaded');
  f.net.createConnection(current); current.reliable.onopen();
  assert.equal(f.consume().length, 24, 'the old native endpoint receives EOF');
  const stale = packet(3, 80, 1);
  f.bytes.set(stale, 320); f.words[2] = stale.length;
  f.net.pump();
  assert.equal(f.words[3], stale.length, 'stale bytes are consumed instead of holding up the ring');
  assert.equal(current.reliable.sent.length, 0);
  f.words[2] = f.words[3] = 0;
  const open = packet(2), fresh = packet(3, 80, 2);
  f.bytes.set(open, 320); f.bytes.set(fresh, 320 + open.length);
  f.words[2] = open.length + fresh.length;
  f.net.pump();
  assert.deepEqual(current.reliable.sent.map(p => new DataView(p.buffer).getUint32(4, true)), [2, 3]);
  assert.deepEqual(Array.from(current.reliable.sent[1]), Array.from(fresh));
});

test('room heartbeats acknowledge liveness without entering the game packet ring', () => {
  const f = fixture(); f.peer.quick = { failover: true };
  f.tick(3000);
  assert.equal(f.peer.unreliable.sent.at(-1), 'halo-room-ping-v1');
  f.peer.unreliable.onmessage({ data: 'halo-room-ping-v1' });
  assert.equal(f.peer.unreliable.sent.at(-1), 'halo-room-pong-v1');
  assert.equal(f.peer.lastPacketAt, 3000); assert.equal(f.words[0], 0);
  f.tick(27000); assert.equal(f.net.status().players, 1);
  f.tick(28001); assert.equal(f.net.status().players, 0);
});

test('heartbeat expiry retires native streams and obsolete pongs cannot revive the peer', () => {
  const f = fixture(), oldUnreliable = f.peer.unreliable;
  f.peer.quick = { failover: true };
  f.receive(streamPacket(f, 2)); f.consume();
  f.tick(25001);
  assert.equal(f.net.status().players, 0);
  assert.deepEqual(f.consume(), Array.from(streamPacket(f, 4)), 'failover timeout also releases the native endpoint');
  oldUnreliable.onmessage({ data: 'halo-room-pong-v1' });
  assert.equal(f.peer.lastPacketAt, 0, 'obsolete liveness callbacks stop before updating peer state');
  assert.equal(f.consume().length, 0);
});


const pingPrefix = 'halo-host-rtt-v1:';
function pingMessages(peer, type) {
  return peer.unreliable.sent.filter(value => typeof value === 'string' && value.startsWith(pingPrefix))
    .map(value => JSON.parse(value.slice(pingPrefix.length))).filter(value => value.type === type);
}
function receivePing(f, peer, message) { peer.unreliable.onmessage({ data: pingPrefix + JSON.stringify(message) }); }
function hostAuthority(f, epoch = 3) {
  const coordinator = f.authority(epoch), state = f.net.fixtureState;
  coordinator.result = { role: 'host', hostId: state.id, hostAddress: state.address };
  coordinator.presence.role = 'host'; coordinator.presence.hostId = state.id;
  f.peer.quick = { ...coordinator.presence, role: 'join' };
  return coordinator;
}
function sharedPings(f) {
  const start = f.offsets.pingPeers / 4;
  return Array.from({ length: f.words[f.offsets.pingCount / 4] }, (_, i) =>
    [f.words[start + i * 2] >>> 0, f.words[start + i * 2 + 1]]);
}

test('host measures correlated RTT and every client receives the host measurements', () => {
  const host = fixture(), coordinator = hostAuthority(host);
  const client = fixture({ id: host.peer.id, address: host.peer.address,
    peerId: host.net.fixtureState.id, peerAddress: host.net.fixtureState.address });
  client.authority(3);
  host.tick(1000);
  const probe = pingMessages(host.peer, 'probe').at(-1);
  client.now(1010); receivePing(client, client.peer, probe);
  const pong = pingMessages(client.peer, 'pong').at(-1);
  assert.equal(pong.sequence, probe.sequence);
  host.now(1045); receivePing(host, host.peer, pong);
  host.tick(2000);
  const table = pingMessages(host.peer, 'table').at(-1);
  assert.deepEqual(table.rows.map(row => row.slice(0, 2)), [[host.net.fixtureState.address, 0], [host.peer.address, 45]]);
  client.now(1055); receivePing(client, client.peer, table);
  assert.deepEqual(sharedPings(client), table.rows.map(row => row.slice(0, 2)));
  assert.deepEqual(sharedPings(host), table.rows.map(row => row.slice(0, 2)));
  assert.equal(host.words[host.offsets.pingSequence / 4] & 1, 0);
  assert.equal(client.words[client.offsets.pingHost / 4] >>> 0, coordinator.result.hostAddress);
  assert.deepEqual(client.consume(), [], 'RTT controls stay outside the native game packet ring');
});

test('ping samples expire and migration clears them before the replacement host probes', () => {
  const f = fixture(), coordinator = hostAuthority(f);
  f.tick(1000);
  const probe = pingMessages(f.peer, 'probe').at(-1);
  f.now(1060); receivePing(f, f.peer, { ...probe, type: 'pong' });
  assert.equal(sharedPings(f)[1][1], 60);
  f.tick(11060); assert.equal(sharedPings(f)[1][1], -1);
  coordinator.recover(12000); f.net.pump();
  assert.deepEqual(sharedPings(f), []);
  assert.equal(f.words[f.offsets.pingHost / 4], 0);
  f.now(12010); receivePing(f, f.peer, { ...probe, type: 'pong' });
  assert.deepEqual(sharedPings(f), [], 'a delayed old-host probe cannot repopulate migration state');
  f.authority(4); f.tick(12020);
  assert.deepEqual(sharedPings(f), [], 'a new client waits for the replacement host table');
  hostAuthority(f, 5); f.tick(13000);
  assert.deepEqual(sharedPings(f), [[f.net.fixtureState.address, 0], [f.peer.address, -1]]);
});

test('only fresh current-host tables are accepted; invalid rows never replace a good table', () => {
  const f = fixture(); f.authority(3); f.tick(1000);
  f.net.peerFor('3333333333333333', 0x0301010a, 'Other');
  const table = { type: 'table', hostId: f.peer.id, host: f.peer.address, epoch: 3,
    match: 500, sequence: 1, rows: [[f.peer.address, 0, f.peer.id], [f.net.fixtureState.address, 83, f.net.fixtureState.id], [0x0301010a, 160, '3333333333333333']] };
  receivePing(f, f.peer, table);
  assert.deepEqual(sharedPings(f), table.rows.map(row => row.slice(0, 2)), 'another player shows its RTT to the host');
  const updated = f.words[f.offsets.pingUpdated / 4];
  f.now(2000);
  for (const invalid of [table, { ...table, epoch: 2 }, { ...table, match: 501 },
    { ...table, hostId: '3333333333333333' }, { ...table, host: 0x0401010a },
    { ...table, sequence: 2, rows: [[f.peer.address, 5, f.peer.id]] },
    { ...table, sequence: 2, rows: [[f.peer.address, 0, f.peer.id], [f.peer.address, 99, f.peer.id]] },
    { ...table, sequence: 2, rows: [[f.peer.address, 0, f.peer.id], [1, NaN, f.net.fixtureState.id]] },
    { ...table, sequence: 2, rows: [[f.peer.address, 0, f.peer.id], [1, 10000, f.net.fixtureState.id]] },
    { ...table, sequence: 2, rows: Array.from({ length: 129 }, (_, i) => [i + 1, 0, f.peer.id]) }]) {
    receivePing(f, f.peer, invalid);
    assert.deepEqual(sharedPings(f), table.rows.map(row => row.slice(0, 2)));
    assert.equal(f.words[f.offsets.pingUpdated / 4], updated, 'replay cannot renew a stale table');
  }
  const other = f.net.peerFor('3333333333333333', 0x0301010a, 'Other');
  f.net.createConnection(other); other.reliable.onopen();
  receivePing(f, other, { ...table, sequence: 2 });
  assert.equal(f.words[f.offsets.pingUpdated / 4], updated, 'a non-host cannot impersonate the table publisher');
  f.now(11000);
  assert.ok(f.net.status().hostPings.every(row => row.ms === null));
});

test('reopened and replaced peer connections cannot reuse previous latency', () => {
  const f = fixture(); hostAuthority(f); f.tick(1000);
  const oldChannel = f.peer.unreliable, probe = pingMessages(f.peer, 'probe').at(-1);
  f.now(1080); receivePing(f, f.peer, { ...probe, type: 'pong' });
  assert.equal(sharedPings(f)[1][1], 80);
  f.net.createConnection(f.peer); f.peer.reliable.onopen(); f.tick(2000);
  assert.equal(sharedPings(f)[1][1], -1);
  oldChannel.onmessage({ data: pingPrefix + JSON.stringify({ ...probe, type: 'pong' }) });
  assert.equal(sharedPings(f)[1][1], -1);
  const replacement = f.net.peerFor('3333333333333333', f.peer.address, 'Reload');
  f.net.createConnection(replacement); replacement.reliable.onopen();
  assert.ok(sharedPings(f).every(row => row[0] !== replacement.address), 'old address sample was retired');
});


test('delayed host tables cannot attribute a departed peer RTT to a reused address', () => {
  const f = fixture(); f.authority(3); f.tick(1000);
  const address = 0x0301010a, oldId = '3333333333333333', newId = '4444444444444444';
  const old = f.net.peerFor(oldId, address, 'Old');
  const table = { type: 'table', hostId: f.peer.id, host: f.peer.address, epoch: 3,
    match: 500, sequence: 1, rows: [[f.peer.address, 0, f.peer.id], [address, 99, oldId]] };
  receivePing(f, f.peer, table); assert.equal(sharedPings(f)[1][1], 99);
  f.net.peerFor(newId, address, 'Reload');
  receivePing(f, f.peer, { ...table, sequence: 2 });
  assert.deepEqual(sharedPings(f), [[f.peer.address, 0]]);
  receivePing(f, f.peer, { ...table, sequence: 3, rows: [[f.peer.address, 0, f.peer.id], [address, 33, newId]] });
  assert.deepEqual(sharedPings(f), [[f.peer.address, 0], [address, 33]]);
});
