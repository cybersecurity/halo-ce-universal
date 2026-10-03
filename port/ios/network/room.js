/* Adapt the unmodified PR #12 HaloNet to copied native packet batches.
   Only this JS process accesses these arrays: SharedArrayBuffer is unnecessary. */
'use strict';
const HaloNative = (() => {
  const OUT = 1024 * 1024, IN = 2 * OUT, LIMIT = 256 * 1024;
  const PING_PEERS = 128, META = 40 + PING_PEERS * 8;
  const offsets = { netLocalAddress: 0, netOutWrite: 4, netOutRead: 8,
    netInWrite: 12, netInRead: 16, pingSequence: 20, pingHost: 24,
    pingEpoch: 28, pingUpdated: 32, pingCount: 36, pingPeers: 40, pingPeerCount: PING_PEERS,
    netOut: META, netOutBytes: OUT,
    netIn: META + OUT, netInBytes: IN };
  const buffer = new ArrayBuffer(META + OUT + IN);
  const bytes = new Uint8Array(buffer), words = new Int32Array(buffer);
  const commands = [];
  let pending = null, inputId = 0, selected = null, error = null, status = 'Ready';
  function command(...values) {
    if (commands.length >= 64) throw new Error('Native room control queue full');
    while (values.length < 4) values.push(0);
    commands.push(values.map(value => value >>> 0));
  }
  function encode(array) {
    let text = '';
    for (let i = 0; i < array.length; i += 8192)
      text += String.fromCharCode(...array.subarray(i, i + 8192));
    return btoa(text);
  }
  function decode(text) {
    if (typeof text !== 'string' || text.length > Math.ceil(LIMIT / 3) * 4)
      throw new Error('Invalid native packet batch');
    return Uint8Array.from(atob(text), c => c.charCodeAt(0));
  }
  function frames(data) {
    const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
    for (let i = 0; i < data.length;) {
      if (data.length - i < 24) throw new Error('Truncated packet header');
      const size = view.getUint32(i, true), kind = view.getUint32(i + 4, true);
      if (size < 24 || size % 4 || size > data.length - i ||
          view.getUint32(i + 20, true) > size - 24 || kind < 1 || kind > 5)
        throw new Error('Invalid packet framing');
      i += size;
    }
  }
  function append(data) {
    frames(data);
    const write = words[1] >>> 0, read = words[2] >>> 0;
    if (((write - read) >>> 0) > OUT || data.length > OUT - ((write - read) >>> 0)) return 0;
    const start = write & (OUT - 1), first = Math.min(data.length, OUT - start);
    bytes.set(data.subarray(0, first), offsets.netOut + start);
    bytes.set(data.subarray(first), offsets.netOut);
    words[1] = (write + data.length) | 0;
    return data.length;
  }
  function incoming(ack) {
    if (pending && ack === pending.id) {
      words[4] = pending.end | 0;
      pending = null;
    }
    if (pending) return pending;
    let read = words[4] >>> 0;
    const write = words[3] >>> 0, result = [];
    while (((write - read) >>> 0) >= 24) {
      const head = new Uint8Array(24);
      for (let i = 0; i < 24; i++) head[i] = bytes[offsets.netIn + ((read + i) & (IN - 1))];
      const size = new DataView(head.buffer).getUint32(0, true);
      if (size < 24 || size % 4 || size > ((write - read) >>> 0)) throw new Error('Corrupt receive ring');
      if (result.length + size > LIMIT) break;
      for (let i = 0; i < size; i++) result.push(bytes[offsets.netIn + ((read + i) & (IN - 1))]);
      read = (read + size) >>> 0;
    }
    if (!result.length) return {id: 0, data: ''};
    pending = {id: ++inputId, data: encode(Uint8Array.from(result)), end: read};
    return pending;
  }
  function report(value) {
    if (value.phase === 'checkpoint') return HaloNet.quickPlayCheckpoint(value);
    status = value.message || value.phase;
    if (value.phase === 'disconnected' && HaloNet.quickPlayLost()) return;
    HaloNet.quickPlayPhase(value.phase);
  }
  HaloNet.attach({memory: {buffer}, base: 0, offsets});
  return {
    async join(options) {
      try {
        await HaloNet.join(options.room, options);
        selected = await HaloNet.quickPlay({
          onStatus(value) {
            status = value.message || value.phase || 'Connecting';
            if (typeof value.hold === 'boolean') command(2, value.hold ? 1 : 0);
          },
          onReconnect(value) { command(4, value.hostAddress, value.epoch); },
          onFailover(value) { command(3, value.role === 'host' ? 1 : 2, value.hostAddress, value.epoch); }
        });
        command(1, HaloNet.address, selected.epoch);
        HaloNet.quickPlayStarted();
      } catch (failure) { error = String(failure); }
    },
    exchange(value) {
      const sent = append(decode(value.out || ''));
      for (const item of value.reports || []) report(item);
      const input = incoming(value.ack);
      return {sent, input, commands: commands.splice(0), selected, error, status,
        address: HaloNet.address, pings: {host: words[6] >>> 0, epoch: words[7] >>> 0, updated: words[8] >>> 0,
          rows: Array.from({length: Math.max(0, Math.min(PING_PEERS, words[9]))},
            (_, i) => [words[10 + i * 2] >>> 0, words[11 + i * 2]])}};
    },
    async leave() { command(5); await HaloNet.leave(); selected = null; },
    // Used by the protocol fixture, not exported across the native bridge.
    memory: {buffer}, offsets
  };
})();
