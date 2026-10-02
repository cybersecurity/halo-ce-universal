/** Native worker framing and the existing web_net.c packet format. */
export const HEADER_BYTES = 24;
export const MAX_RECORD = 65536;
export const MAX_PACKET = HEADER_BYTES + 16000;
export const KIND = Object.freeze({ DATAGRAM: 1, OPEN: 2, DATA: 3, CLOSE: 4, REFUSE: 5 });

export function record(type, payload) {
  const bytes = Buffer.from(payload);
  if (bytes.length + 1 > MAX_RECORD) throw new Error('Record too large');
  const out = Buffer.allocUnsafe(bytes.length + 5);
  out.writeUInt32LE(bytes.length + 1, 0);
  out[4] = type;
  bytes.copy(out, 5);
  return out;
}

export class RecordDecoder {
  #pending = Buffer.alloc(0);
  push(chunk, receive) {
    this.#pending = Buffer.concat([this.#pending, chunk]);
    while (this.#pending.length >= 4) {
      const size = this.#pending.readUInt32LE(0);
      if (size < 1 || size > MAX_RECORD) throw new Error('Invalid worker record size');
      if (this.#pending.length < size + 4) break;
      const type = this.#pending[4];
      const payload = this.#pending.subarray(5, size + 4);
      this.#pending = this.#pending.subarray(size + 4);
      receive(type, payload);
    }
  }
  get pendingBytes() { return this.#pending.length; }
}

export function isVirtualAddress(address) {
  return Number.isInteger(address) && address > 0 && address <= 0xffffffff &&
    (address & 255) === 100 && ((address >>> 8) & 255) >= 64 && ((address >>> 8) & 255) < 128;
}

export function packetHeader(bytes) {
  if (bytes.length < HEADER_BYTES || bytes.length > MAX_PACKET || bytes.length % 4 !== 0)
    throw new Error('Invalid game packet size');
  const size = bytes.readUInt32LE(0), kind = bytes.readUInt32LE(4), length = bytes.readUInt32LE(20);
  if (size !== bytes.length || size !== ((HEADER_BYTES + length + 3) & ~3) || kind < 1 || kind > 5)
    throw new Error('Invalid game packet header');
  if ((kind === KIND.DATAGRAM && length > 1395) || (kind === KIND.DATA && (!length || length > 16000)) ||
      (![KIND.DATAGRAM, KIND.DATA].includes(kind) && length !== 0))
    throw new Error('Invalid game packet payload');
  return {
    kind, length, source: bytes.readUInt32LE(8), destination: bytes.readUInt32LE(12),
    // The C fields contain network byte order, despite the other fields being little-endian.
    sourcePort: bytes.readUInt16BE(16), destinationPort: bytes.readUInt16BE(18),
  };
}

/** Defense in depth: the worker independently authenticates the native peer. */
export class SessionPackets {
  constructor(address, maxStreams = 16) {
    if (!isVirtualAddress(address)) throw new Error('Invalid assigned address');
    this.address = address;
    this.peers = new Set();
    this.streams = new Map();
    this.closed = new Map();
    this.maxStreams = maxStreams;
  }
  peer(address, connected) {
    if (!isVirtualAddress(address) || address === this.address) throw new Error('Invalid peer address');
    if (connected) this.peers.add(address);
    else {
      this.peers.delete(address);
      for (const [key, stream] of this.streams) if (stream.peer === address) this.streams.delete(key);
    }
  }
  validate(bytes, outgoing) {
    const h = packetHeader(bytes);
    const local = outgoing ? h.source : h.destination;
    const remote = outgoing ? h.destination : h.source;
    const localPort = outgoing ? h.sourcePort : h.destinationPort;
    const remotePort = outgoing ? h.destinationPort : h.sourcePort;
    const broadcast = h.kind === KIND.DATAGRAM && h.destination === 0xffffffff;
    if ((local !== this.address && !(broadcast && !outgoing)) ||
        (!this.peers.has(remote) && !(broadcast && outgoing)))
      throw new Error('Game packet is outside this session');
    if (h.kind === KIND.DATAGRAM) {
      if (![5150, 5151].includes(h.sourcePort) || ![5150, 5151].includes(h.destinationPort))
        throw new Error('Unsupported game datagram port');
      return h;
    }
    if (!localPort || !remotePort) throw new Error('Missing game stream port');
    const key = `${remote}:${localPort}:${remotePort}`;
    const now = Date.now();
    for (const [tuple, expires] of this.closed) if (expires <= now) this.closed.delete(tuple);
    if (h.kind === KIND.OPEN) {
      // Native system link accepts game streams only on 5150.
      if (h.destinationPort !== 5150 || h.sourcePort < 1024)
        throw new Error('Unsupported game stream port');
      if (this.streams.has(key) || this.closed.has(key)) throw new Error('Game stream tuple reused');
      if (this.streams.size >= this.maxStreams)
        throw new Error('Too many game streams');
      this.streams.set(key, { peer: remote });
    } else {
      const stream = this.streams.get(key);
      // This protocol has full close, not TCP half close. The other side need
      // not acknowledge CLOSE; bytes already in flight belong to a dead tuple.
      if (!stream && this.closed.has(key)) return { ...h, discard: true };
      if (!stream) throw new Error('Unknown game stream');
      if (h.kind === KIND.REFUSE || h.kind === KIND.CLOSE) {
        if (this.closed.size >= 1024) throw new Error('Too many recently closed streams');
        this.streams.delete(key);
        this.closed.set(key, now + 30000);
      }
    }
    return h;
  }
}
