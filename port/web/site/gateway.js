/* Native invite transport. A relay-assigned identity is fixed for one game
 * session. Reliable frames queue with bounds instead of disappearing on a
 * full shared ring. The relay URL comes from site configuration, never an invite. */
'use strict';
const HaloGateway = (() => {
  const MAX_QUEUE = 8 * 1024 * 1024;
  const MAX_FRAME = 65560;
  function idWords(identifier) {
    if (!/^[a-f0-9]{12}$/i.test(identifier || '')) throw new Error('Invalid relay identity.');
    const bytes = new Uint8Array(8);
    for (let i = 0; i < 6; i++) bytes[i] = parseInt(identifier.slice(i * 2, i * 2 + 2), 16);
    const view = new DataView(bytes.buffer);
    return [view.getInt32(0, true), view.getInt32(4, true)];
  }
  function validAddress(value) {
    // Native P2P virtual addresses use 100.64.0.0/10, in little-endian words.
    return Number.isInteger(value) && value > 0 && value <= 0xffffffff &&
      (value & 255) === 100 && ((value >>> 8) & 255) >= 64 && ((value >>> 8) & 255) <= 127;
  }
  function validFrame(packet) {
    if (packet.byteLength < 24 || packet.byteLength > MAX_FRAME || packet.byteLength % 4) return false;
    const v = new DataView(packet.buffer, packet.byteOffset, packet.byteLength);
    const length = v.getUint32(20, true), kind = v.getUint32(4, true);
    return v.getUint32(0, true) === packet.byteLength && kind >= 1 && kind <= 5 &&
      ((24 + length + 3) & ~3) === packet.byteLength &&
      ([1, 3].includes(kind) || length === 0);
  }
  async function connect(urlText, invite, { accessToken = '', onStatus = () => {} } = {}) {
    const url = new URL(urlText);
    if (url.protocol !== 'wss:' && !(url.protocol === 'ws:' &&
        ['localhost', '127.0.0.1', '[::1]'].includes(url.hostname))) {
      throw new Error('The game relay must use a secure WebSocket.');
    }
    if (url.username || url.password || url.search || url.hash) throw new Error('Invalid relay URL.');
    if (!/^[a-f0-9]{44}$/.test(invite)) throw new Error('Invalid desktop invite.');
    const socket = new WebSocket(url);
    socket.binaryType = 'arraybuffer';
    const peers = new Map(), queue = [];
    let queuedBytes = 0, queueHead = 0, identity = null, shared = null, ended = false, timer;
    let resolveReady, rejectReady;
    const ready = new Promise((resolve, reject) => { resolveReady = resolve; rejectReady = reject; });
    function report(state, message) { onStatus({ state, message, peers: peers.size }); }
    function fail(message) {
      if (ended) return;
      ended = true;
      clearTimeout(timer);
      peers.clear();
      queue.length = 0;
      queueHead = 0;
      queuedBytes = 0;
      sync();
      if (shared) Atomics.store(new Int32Array(shared.memory.buffer),
        (shared.base + shared.offsets.netLocalAddress) >>> 2, 0);
      rejectReady(new Error(message));
      report('error', message);
      try { socket.close(1000, 'Session ended'); } catch { /* already closed */ }
    }
    function sync() {
      if (!shared || !identity) return;
      const i32 = new Int32Array(shared.memory.buffer);
      const at = (key) => (shared.base + shared.offsets[key]) >>> 2;
      const id = idWords(identity.identifier);
      Atomics.store(i32, at('gatewayIdentifier'), id[0]);
      Atomics.store(i32, at('gatewayIdentifier') + 1, id[1]);
      Atomics.store(i32, at('netLocalAddress'), identity.address | 0);
      let index = 0;
      for (const peer of peers.values()) {
        const offset = at('gatewayPeers') + index++ * 3, words = idWords(peer.identifier);
        Atomics.store(i32, offset + 2, 0);
        Atomics.store(i32, offset, words[0]);
        Atomics.store(i32, offset + 1, words[1]);
        Atomics.store(i32, offset + 2, peer.address | 0);
      }
      for (; index < shared.offsets.gatewayPeerCount; index++) Atomics.store(i32, at('gatewayPeers') + index * 3 + 2, 0);
      Atomics.store(i32, at('gatewayEnabled'), 1);
    }
    const transport = {
      get address() { return identity.address; },
      get hostAddress() { return ended ? 0 : (peers.get(invite.slice(0, 12))?.address || 0) >>> 0; },
      get connected() { return !ended && peers.size > 0; },
      get closed() { return ended; },
      attach(value) { shared = value; sync(); },
      flush(receive) {
        if (ended) return;
        while (queueHead < queue.length && receive(queue[queueHead])) queuedBytes -= queue[queueHead++].byteLength;
        if (queueHead === queue.length) { queue.length = 0; queueHead = 0; }
        else if (queueHead > 1024) { queue.splice(0, queueHead); queueHead = 0; }
      },
      send(packet) {
        if (ended || socket.readyState !== WebSocket.OPEN) return false;
        if (socket.bufferedAmount > 1024 * 1024) return false;
        try { socket.send(packet); return true; } catch { fail('The relay connection closed. Reload to reconnect.'); return false; }
      },
      close() { fail('Disconnected. Reload to join again.'); },
    };
    timer = setTimeout(() => fail('The relay did not respond. Check the relay address and try again.'), 15000);
    socket.onopen = () => {
      socket.send(JSON.stringify({ type: 'join', invite, ...(accessToken ? { accessToken } : {}) }));
      report('connecting', 'Connecting to the desktop host…');
    };
    socket.onmessage = (event) => {
      if (ended) return;
      try {
        if (typeof event.data === 'string') {
          if (event.data.length > 4096) throw new Error('Invalid relay message.');
          const message = JSON.parse(event.data);
          if (message.type === 'ready') {
            if (identity || !validAddress(message.address)) throw new Error('Invalid relay handshake.');
            idWords(message.identifier);
            identity = message;
            clearTimeout(timer);
            resolveReady(transport);
            report('waiting', 'Relay connected. Waiting for the invited host…');
          } else if (message.type === 'peer') {
            if (!identity || !validAddress(message.address)) throw new Error('Invalid relay peer.');
            idWords(message.identifier);
            if (message.connected) peers.set(message.identifier, message);
            else peers.delete(message.identifier);
            if (peers.size > 32) throw new Error('Too many relay peers.');
            sync();
            report(peers.size ? 'connected' : 'waiting', peers.size ?
              'Host connected. Preparing to join the match…' :
              'Waiting for the host. An old invite stops working when its host closes the game.');
          } else if (message.type === 'error') {
            fail(message.message === 'Native host did not connect' ?
              'The invited host did not connect. Ask the host for a fresh invite and try again.' :
              'The relay could not join this invite. The host may be offline, or access may be denied.');
          }
        } else {
          const packet = new Uint8Array(event.data);
          if (!identity || !validFrame(packet)) throw new Error('Invalid relay game packet.');
          if (queuedBytes + packet.byteLength > MAX_QUEUE) throw new Error('The game cannot keep up with the relay. Reload to reconnect.');
          queue.push(packet);
          queuedBytes += packet.byteLength;
        }
      } catch (error) { fail(error.message || 'Invalid relay response.'); }
    };
    socket.onclose = () => fail('Relay disconnected. Reload to reconnect.');
    socket.onerror = () => fail('Cannot reach the game relay. Check its address and access key.');
    return ready;
  }
  return { connect, idWords, validAddress, validFrame };
})();
if (typeof module !== 'undefined') module.exports = HaloGateway;
