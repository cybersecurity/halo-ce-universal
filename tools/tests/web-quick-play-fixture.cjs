const { readFileSync } = require('node:fs');
const { webcrypto } = require('node:crypto');
const vm = require('node:vm');
const HOST_ADDRESS = 0x0201010a;

// Exercise the real public API and encrypted presence publication with a fake
// broker and deterministic clock. This does not open a network or run Halo.
async function network() {
  let now = 0, nextTimer = 0, roomKey;
  const timers = new Map(), sockets = [], messages = [], encryption = [], decryption = [], connections = [];
  class Socket {
    readyState = 1;
    constructor() { sockets.push(this); }
    send() {}
    close() { this.readyState = 3; }
  }
  const subtle = {};
  for (const method of ['importKey', 'digest'])
    subtle[method] = webcrypto.subtle[method].bind(webcrypto.subtle);
  subtle.deriveKey = async (...args) => (roomKey = await webcrypto.subtle.deriveKey(...args));
  subtle.decrypt = (...args) => {
    const pending = webcrypto.subtle.decrypt(...args); decryption.push(pending); return pending;
  };
  subtle.encrypt = (...args) => {
    messages.push(JSON.parse(new TextDecoder().decode(args[2])));
    const pending = webcrypto.subtle.encrypt(...args);
    encryption.push(pending);
    return pending;
  };
  const context = {
    crypto: { getRandomValues: webcrypto.getRandomValues.bind(webcrypto), subtle },
    Date: class extends Date { static now() { return now; } },
    TextEncoder, TextDecoder, Uint8Array, DOMException,
    localStorage: { getItem() { return null; }, setItem() {} }, WebSocket: Socket,
    RTCPeerConnection: class {
      constructor() { connections.push(this); this.channels = []; }
      createDataChannel(label) {
        const channel = { label, readyState: 'open', bufferedAmount: 0, sent: [], send(value) { this.sent.push(value); } };
        this.channels.push(channel); return channel;
      }
      async createOffer() { return { sdp: 'offer' }; }
      async setLocalDescription(value) { this.localDescription = value; }
      close() {}
    },
    setInterval: fn => { const id = ++nextTimer; timers.set(id, fn); return id; },
    clearInterval: id => timers.delete(id), setTimeout() {}, clearTimeout() {},
  };
  vm.runInNewContext(readFileSync(require.resolve('../../port/web/site/net.js'), 'utf8') +
    '\nglobalThis.net = HaloNet;', context);
  await context.net.join('TEST42', { brokers: ['wss://test.invalid'] });
  sockets[0].onmessage({ data: new Uint8Array([0x20, 2, 0, 0]).buffer });
  async function peerPresence({ id = 'ffffffffffffffff', address = HOST_ADDRESS, sequence = 1, quick }) {
    const iv = webcrypto.getRandomValues(new Uint8Array(12));
    const plain = new TextEncoder().encode(JSON.stringify({ type: 'hello', from: id,
      mid: webcrypto.randomUUID(), address, name: 'Host', quickSequence: sequence, quick }));
    const encrypted = new Uint8Array(await webcrypto.subtle.encrypt({ name: 'AES-GCM', iv }, roomKey, plain));
    const body = [0, 1, 120, ...iv, ...encrypted], length = [];
    let size = body.length;
    do { let byte = size % 128; size = Math.floor(size / 128); length.push(size ? byte | 128 : byte); } while (size);
    sockets[0].onmessage({ data: new Uint8Array([0x30, ...length, ...body]).buffer });
    await Promise.all(decryption); await new Promise(setImmediate);
    return connections.at(-1);
  }
  return { net: context.net, messages,
    peerPresence,
    disconnectBrokers() { for (const socket of sockets) { socket.close(); socket.onclose(); } },
    tick(time) { now = time; for (const fn of [...timers.values()]) fn(); },
    async close() { await context.net.leave(); await Promise.all(encryption); },
    latest() { return messages.filter(message => message.type === 'hello').at(-1); },
    checkpoint(tick = 90, epoch = 0, matchId = 500) {
      return context.net.quickPlayCheckpoint({ phase: 'checkpoint', epoch, tick, matchId });
    },
    async hostPresence(epoch = 0) {
      return peerPresence({ sequence: epoch + 1,
        quick: { role: 'host', hostId: 'ffffffffffffffff', phase: 'launched', gamePhase: 'playing', epoch,
          failover: true, migration: true, matchId: 500, checkpointTick: 90 } });
    },
  };
}

module.exports = { network };
