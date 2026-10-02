/*
NET.JS

Online play: the players of a room form one network, over which the game's
own system link (its discovery broadcasts and its connections) works as on
a LAN.

- Every copy of the game has an address on that network, 10.x.y.z, kept in
  this browser. The game's sockets (port/web/src/web_net.c) put what they
  send to other addresses in a ring in the shared memory, and take what
  arrives from a second ring.
- The page carries those packets over WebRTC: a data channel to each other
  player, reliable and ordered for the game's connections, unreliable for
  its datagrams. Broadcasts go to every player.
- Players find each other through public MQTT brokers (over secure
  WebSockets), in a topic derived from the room's code. Everything sent there
  is encrypted with a key derived from the code, so only those who have it
  can read the room's messages or join it.

A room is a code the players share (or a link with ?room=CODE). NAT that
WebRTC cannot cross without a relay (some mobile networks) needs a TURN
server, which the settings can name.
*/

'use strict';

// Election is deliberately limited to visible, connected quick-play players.
// A room is not a consensus service: disconnected network partitions can each
// form a game. Once a host is reserved, later participants join that host.
class HaloQuickCoordinator {
  static DISCOVERY_MS = 6000;
  static SETTLE_MS = 2000;
  static RESERVE_MS = 1500;
  static TIMEOUT_MS = 35000;
  static HOST_GRACE_MS = 10000;
  static HEARTBEAT_TIMEOUT_MS = 25000;
  constructor(id, address, now) {
    this.id = id;
    this.address = address;
    this.started = now;
    this.brokerSince = null;
    this.selectedSince = now;
    this.selected = null;
    this.reservedSince = null;
    this.result = null;
    this.epoch = 0;
    this.recovering = false;
    this.hasMatch = false;
    this.missingSince = null;
    this.presence = { role: 'candidate', hostId: null, phase: 'electing', gamePhase: '', epoch: 0, failover: true,
      migration: true, matchId: null, checkpointTick: -1 };
  }
  recover(now, epoch = this.epoch + 1) {
    if (this.result) this.previousAuthority = { ...this.result, epoch: this.epoch };
    this.freshJoin = !this.result && this.presence.matchId === null;
    this.epoch = epoch;
    this.recovering = true;
    this.started = now;
    this.brokerSince = null;
    this.selectedSince = now;
    this.selected = null;
    this.reservedSince = null;
    this.result = null;
    this.missingSince = null;
    this.presence = { ...this.presence, role: 'candidate', hostId: null, phase: 'electing', epoch, failover: true };
  }
  select(id, now) {
    if (id !== this.selected) {
      this.selected = id; this.selectedSince = now;
      if (this.presence.role === 'join') this.reservedSince = now;
    }
    this.presence.hostId = id;
  }
  hostHealthy(host, now, epoch = this.epoch) {
    return host?.open && host.quick?.role === 'host' && host.quick.gamePhase !== 'migration-failed' &&
      (host.quick.epoch || 0) === epoch && (!host.quick.failover ||
        ((!Number.isFinite(host.lastPacketAt) || now - host.lastPacketAt < HaloQuickCoordinator.HEARTBEAT_TIMEOUT_MS) &&
         (host.quick.gamePhase !== 'playing' || host.quick.checkpointTick < 0 ||
          !Number.isFinite(host.lastProgressAt) || now - host.lastProgressAt < HaloQuickCoordinator.HEARTBEAT_TIMEOUT_MS)));
  }
  tick(now, peers, brokerReady) {
    const sameMatch = peers.filter(peer => peer.open && peer.quick &&
      (this.presence.matchId === null || peer.quick.matchId === this.presence.matchId));
    const currentHost = peers.find(peer => peer.id === this.result?.hostId);
    const healthyAuthority = this.result?.role === 'host' ? this.presence.gamePhase === 'playing' :
      this.result?.role === 'join' && this.hostHealthy(currentHost, now);
    // A client's suspicion is not a transfer of authority. Keep the live host
    // and healthy clients playing; a verified, launched replacement still fences
    // the old authority after an actual partition or host failure.
    const newest = Math.max(this.epoch, ...sameMatch.filter(peer => !healthyAuthority ||
      (peer.quick.role === 'host' && peer.quick.phase === 'launched' && peer.quick.gamePhase === 'playing' &&
        peer.quick.migration && peer.quick.checkpointTick >= 0)).map(peer => peer.quick.epoch || 0));
    if (newest > this.epoch) this.recover(now, newest);
    const previousHost = peers.find(peer => peer.id === this.previousAuthority?.hostId);
    if (this.recovering && this.presence.role === 'candidate' && this.previousAuthority?.role === 'join' &&
        this.hostHealthy(previousHost, now, this.previousAuthority.epoch) &&
        !sameMatch.some(peer => peer.quick.role === 'host' && peer.quick.phase === 'launched' &&
          peer.quick.gamePhase === 'playing' && (peer.quick.epoch || 0) > this.previousAuthority.epoch)) {
      // The suspected host returned before a replacement committed. Its native
      // authority never changed: retract the proposal and reattach only this client.
      this.epoch = this.previousAuthority.epoch;
      this.result = { role: 'join', hostId: previousHost.id, hostAddress: previousHost.address };
      this.recovering = false;
      this.missingSince = null;
      this.presence = { ...this.presence, role: 'join', hostId: previousHost.id, phase: 'launched', epoch: this.epoch };
      return { result: this.result, reconnect: true };
    }
    if (this.result?.role === 'join') {
      const host = peers.find(peer => peer.id === this.result.hostId);
      const healthy = this.hostHealthy(host, now);
      if (healthy) this.missingSince = null;
      else {
        if (this.missingSince === null) this.missingSince = now;
        if (now - this.missingSince < HaloQuickCoordinator.HOST_GRACE_MS)
          return { state: 'reconnecting', message: 'The host connection was lost. Waiting briefly for it to return…' };
        this.recover(now);
      }
    }
    if (this.result) return { result: this.result };
    const preservingMatch = this.recovering && this.hasMatch && this.presence.matchId !== null;
    if (!preservingMatch && now - this.started >= HaloQuickCoordinator.TIMEOUT_MS)
      return { error: 'The room could not agree on a reachable host. Check the connection, then try again.' };
    if (brokerReady && this.brokerSince === null) this.brokerSince = now;
    const active = peers.filter(peer => peer.quick && (peer.quick.epoch || 0) === this.epoch && Number.isInteger(peer.address) &&
      peer.address > 0 && peer.address <= 0xffffffff &&
      (this.presence.matchId === null || peer.quick.matchId === this.presence.matchId));
    const hosts = active.filter(peer => peer.quick.role === 'host' && peer.quick.gamePhase !== 'migration-failed' && (!this.recovering ||
      (peer.quick.migration && peer.quick.checkpointTick >= 0 && (this.freshJoin || peer.quick.matchId === this.presence.matchId))));
    if (this.presence.role === 'host') hosts.push({ id: this.id, address: this.address, open: true, quick: this.presence });
    hosts.sort((a, b) => Number(b.quick.phase === 'launched') - Number(a.quick.phase === 'launched') || a.id.localeCompare(b.id));
    if (hosts.length) {
      const host = hosts[0];
      if (this.freshJoin) this.presence.matchId = host.quick.matchId;
      this.select(host.id, now);
      if (!host.open) return { state: 'connecting', message: this.recovering ?
        'Connecting to the replacement host…' : 'Connecting to the room’s host…' };
      if (host.id !== this.id && this.presence.role !== 'join') {
        this.presence = { ...this.presence, role: 'join', hostId: host.id, phase: 'reserved', epoch: this.epoch, failover: true };
        this.reservedSince = now;
      }
      if (now - this.reservedSince < HaloQuickCoordinator.RESERVE_MS)
        return { state: 'coordinating', message: 'Confirming the multiplayer host…' };
      this.result = { role: host.id === this.id ? 'host' : 'join', hostId: host.id, hostAddress: host.address };
      return { result: this.result };
    }
    if (this.presence.role === 'join') {
      if (preservingMatch) {
        // A transport can vanish during the replacement reservation. Keep the
        // loaded match and its epoch while that host or another survivor returns.
        this.recover(now, this.epoch);
        return { state: 'recovering', message: 'The replacement host connection was lost. Preserving the match…' };
      }
      return { error: 'The selected host left before the game started. Try again.' };
    }
    if (active.some(peer => peer.quick.role === 'join'))
      return { state: 'connecting', message: 'Waiting for the room’s existing host…' };
    const candidates = active.filter(peer => peer.quick.role === 'candidate');
    const eligible = candidate => candidate.quick.gamePhase !== 'migration-failed' && (!this.recovering ||
      (candidate.quick.migration && candidate.quick.checkpointTick >= 0 &&
        candidate.quick.matchId !== null && candidate.quick.matchId === this.presence.matchId));
    // Connected checkpoint owners can discover and acknowledge the same election
    // over RTC. An isolated survivor or a new room still needs broker discovery.
    const directRecovery = preservingMatch && candidates.some(peer => peer.open && eligible(peer));
    if (!brokerReady && !directRecovery) return { state: 'connecting', message: this.recovering ?
      'Reconnecting room signaling while preserving the match…' : 'Connecting to the multiplayer room…' };
    if (this.brokerSince === null) this.brokerSince = now;
    const choices = [...candidates, { id: this.id, quick: this.presence }].filter(eligible);
    choices.sort((a, b) => (this.recovering ? b.quick.checkpointTick - a.quick.checkpointTick : 0) || a.id.localeCompare(b.id));
    const selected = choices[0]?.id;
    if (!selected) return { state: 'recovering', message: 'Waiting for a player with a complete match checkpoint…' };
    this.select(selected, now);
    if (candidates.some(peer => !peer.open))
      return { state: 'connecting', message: 'Connecting the players before choosing a host…' };
    const settled = now - this.brokerSince >= HaloQuickCoordinator.DISCOVERY_MS &&
      now - this.selectedSince >= HaloQuickCoordinator.SETTLE_MS;
    // Each connected candidate must have observed and acknowledged the same
    // winner before it can reserve the host role. Idle room members do not vote.
    const acknowledged = candidates.filter(eligible).every(peer => peer.quick.hostId === selected);
    if (selected === this.id && settled && acknowledged) {
      this.presence = { ...this.presence, role: 'host', hostId: this.id, phase: 'reserved', epoch: this.epoch, failover: true };
      this.reservedSince = now;
      return { state: 'coordinating', message: 'Preparing to host the room…' };
    }
    return { state: 'coordinating', message: 'Choosing a multiplayer host…' };
  }
  launched(phase = 'loading') {
    if (this.result) {
      this.presence.phase = 'launched'; this.presence.gamePhase = phase;
      if (phase === 'playing') this.hasMatch = true;
    }
  }
  checkpoint(epoch, tick, matchId) {
    if (epoch !== this.epoch || !Number.isInteger(tick) || tick < 0 || tick > 0x7fffffff ||
        !Number.isInteger(matchId) || matchId < 0 || matchId > 0xffffffff) return false;
    if (this.presence.matchId !== null && this.presence.matchId !== matchId) return false;
    if (tick < this.presence.checkpointTick) return false;
    this.presence.matchId = matchId;
    this.presence.checkpointTick = tick;
    this.hasMatch = true;
    return true;
  }
}

const HaloNet = (() => {
  const BROKERS = [
    'wss://broker.emqx.io:8084/mqtt',
    'wss://broker.hivemq.com:8884/mqtt',
    'wss://test.mosquitto.org:8081/mqtt',
  ];
  const STUN = [{ urls: ['stun:stun.l.google.com:19302', 'stun:stun.cloudflare.com:3478'] }];
  const HELLO_INTERVAL = 3000;
  const PEER_TIMEOUT = 20000;
  const PACKET_HEADER = 24;
  const MIGRATION_MAGIC = 0x484d4731;
  const MIGRATION_HEADER = 8;
  const MAX_RELIABLE_QUEUE = 8 * 1024 * 1024;
  const MAX_CHANNEL_BUFFER = 1024 * 1024;
  const PING_CONTROL = 'halo-host-rtt-v1:';
  const QUICK_CONTROL = 'halo-room-quick-v1:';
  const MAX_QUICK_CONTROL = 1024;
  const PING_STALE_MS = 10000;
  const PING_PEERS = 128;
  const KIND = { DATAGRAM: 1, OPEN: 2, DATA: 3, CLOSE: 4, REFUSE: 5 };

  const state = {
    shared: null,        // { memory, base, offsets }
    room: null,
    key: null,
    topic: null,
    id: randomId(),
    address: 0,          // network byte order
    brokers: [],
    peers: new Map(),    // id -> peer
    byAddress: new Map(),// address -> peer
    replacedPeers: new Set(), // obsolete peer ids from reloads at an existing address
    pendingCloses: new Map(), // native EOF packets must precede a replacement's OPEN
    seen: new Set(),     // message ids already handled (several brokers)
    listeners: new Set(),
    iceServers: STUN,
    pumpTimer: null,
    helloTimer: null,
    transport: null,     // native invite gateway, when selected
    quick: null,
    quickSequence: 0,
    roomGeneration: 0,
    pings: { signature: '', authority: null, rows: [], updated: 0, sequence: 0, received: 0 },
    pingProbe: 0,
  };

  function randomId() {
    const bytes = crypto.getRandomValues(new Uint8Array(8));
    return Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');
  }

  // ---------- this machine's address

  function addressText(address) {
    return [address & 255, (address >>> 8) & 255, (address >>> 16) & 255, address >>> 24].join('.');
  }

  function localAddress() {
    let text = null;
    try { text = localStorage.getItem('halo-web-net-address'); } catch { /* none */ }
    const parts = text ? text.split('.').map(Number) : null;
    if (!parts || parts.length !== 4 || parts[0] !== 10) {
      const random = crypto.getRandomValues(new Uint8Array(3));
      const b = 1 + (random[0] % 254), c = random[1], d = 1 + (random[2] % 254);
      text = `10.${b}.${c}.${d}`;
      try { localStorage.setItem('halo-web-net-address', text); } catch { /* not kept */ }
    }
    const p = text.split('.').map(Number);
    return (p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24)) >>> 0;
  }

  // ---------- events for the page

  function emit(type, detail) {
    for (const listener of state.listeners) listener(type, detail);
  }

  function status() {
    const connected = [...state.peers.values()].filter((peer) => peer.open).length;
    return {
      room: state.room,
      address: addressText(state.address),
      brokers: state.brokers.filter((broker) => broker.ready).length,
      players: connected,
      names: [...state.peers.values()].filter((peer) => peer.open).map((peer) => peer.name),
      hostPings: state.pings.rows.map(([address, ms]) => ({ address: addressText(address),
        ms: Date.now() - state.pings.updated < PING_STALE_MS && ms >= 0 ? ms : null })),
    };
  }

  // ---------- encryption (AES-GCM, a key from the room's code)

  async function deriveRoom(code) {
    const encoder = new TextEncoder();
    const material = await crypto.subtle.importKey('raw', encoder.encode(code), 'PBKDF2', false, ['deriveKey']);
    const key = await crypto.subtle.deriveKey(
      { name: 'PBKDF2', salt: encoder.encode('halo-web-room-v1'), iterations: 50000, hash: 'SHA-256' },
      material, { name: 'AES-GCM', length: 256 }, false, ['encrypt', 'decrypt']);
    const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', encoder.encode('halo-web-topic-v1:' + code)));
    const topic = 'halo-web/v1/' + Array.from(digest.slice(0, 12), (b) => b.toString(16).padStart(2, '0')).join('');
    return { key, topic };
  }

  async function seal(message) {
    const iv = crypto.getRandomValues(new Uint8Array(12));
    const plain = new TextEncoder().encode(JSON.stringify(message));
    const cipher = new Uint8Array(await crypto.subtle.encrypt({ name: 'AES-GCM', iv }, state.key, plain));
    const result = new Uint8Array(12 + cipher.length);
    result.set(iv);
    result.set(cipher, 12);
    return result;
  }

  async function open(bytes) {
    try {
      const plain = await crypto.subtle.decrypt({ name: 'AES-GCM', iv: bytes.slice(0, 12) }, state.key, bytes.slice(12));
      return JSON.parse(new TextDecoder().decode(plain));
    } catch {
      return null; // another room's, or damaged
    }
  }

  // ---------- MQTT 3.1.1 over WebSockets (QoS 0 only)

  function encodeLength(length) {
    const bytes = [];
    do {
      let byte = length % 128;
      length = Math.floor(length / 128);
      if (length > 0) byte |= 128;
      bytes.push(byte);
    } while (length > 0);
    return bytes;
  }

  function mqttString(text) {
    const bytes = new TextEncoder().encode(text);
    return [bytes.length >> 8, bytes.length & 255, ...bytes];
  }

  function mqttPacket(type, body) {
    return new Uint8Array([type, ...encodeLength(body.length), ...body]);
  }

  function connectBroker(url) {
    const broker = { url, socket: null, ready: false, buffer: new Uint8Array(0), ping: null, retry: null };
    const start = () => {
      broker.buffer = new Uint8Array(0);
      let socket;
      try {
        socket = new WebSocket(url, 'mqtt');
      } catch {
        return;
      }
      broker.socket = socket;
      socket.binaryType = 'arraybuffer';
      socket.onopen = () => {
        const body = [...mqttString('MQTT'), 4, 0x02, 0, 60, ...mqttString('halo-' + state.id.slice(0, 12) + '-' +
          Math.floor(Math.random() * 1e6))];
        socket.send(mqttPacket(0x10, body));
      };
      socket.onmessage = (event) => {
        const incoming = new Uint8Array(event.data);
        const joined = new Uint8Array(broker.buffer.length + incoming.length);
        joined.set(broker.buffer);
        joined.set(incoming, broker.buffer.length);
        broker.buffer = joined;
        parseMqtt(broker);
      };
      socket.onclose = () => {
        broker.ready = false;
        clearInterval(broker.ping);
        emit('status', status());
        if (state.room && state.brokers.includes(broker)) broker.retry = setTimeout(start, 5000);
      };
      socket.onerror = () => {};
    };
    start();
    return broker;
  }

  function parseMqtt(broker) {
    for (;;) {
      const bytes = broker.buffer;
      if (bytes.length < 2) return;
      let length = 0, multiplier = 1, index = 1, byte;
      do {
        if (index >= bytes.length) return;
        byte = bytes[index++];
        length += (byte & 127) * multiplier;
        multiplier *= 128;
      } while (byte & 128);
      if (bytes.length < index + length) return;
      const type = bytes[0] & 0xF0;
      const body = bytes.slice(index, index + length);
      broker.buffer = bytes.slice(index + length);
      if (type === 0x20) {
        if (body.length !== 2 || body[1] !== 0) { broker.socket.close(); return; }
        // CONNACK: subscribe to the room
        const body2 = [0, 1, ...mqttString(state.topic), 0];
        broker.socket.send(mqttPacket(0x82, body2));
        broker.ready = true;
        broker.ping = setInterval(() => {
          if (broker.socket.readyState === 1) broker.socket.send(new Uint8Array([0xC0, 0]));
        }, 30000);
        emit('status', status());
        hello();
      } else if (type === 0x30) {
        const topicLength = (body[0] << 8) | body[1];
        let offset = 2 + topicLength;
        if (bytes[0] & 0x06) offset += 2; // (a packet id, QoS above 0)
        receiveSignal(body.slice(offset));
      }
    }
  }

  async function publish(message) {
    if (!state.room) return;
    const generation = state.roomGeneration;
    message.from = state.id;
    message.mid = randomId();
    const payload = await seal(message);
    if (generation !== state.roomGeneration || !state.room) return;
    const body = [...mqttString(state.topic), ...payload];
    const packet = mqttPacket(0x30, body);
    for (const broker of state.brokers) {
      if (broker.ready && broker.socket.readyState === 1) broker.socket.send(packet);
    }
  }

  async function receiveSignal(bytes) {
    const generation = state.roomGeneration;
    const message = await open(bytes);
    if (generation !== state.roomGeneration || !state.room) return;
    if (!message || message.from === state.id || state.seen.has(message.mid)) return;
    state.seen.add(message.mid);
    if (state.seen.size > 5000) state.seen = new Set([...state.seen].slice(-2000));
    if (message.to && message.to !== state.id) return;
    handleSignal(message);
  }

  // ---------- peers

  function hello() {
    // Once peers are connected, authority announcements must not depend on
    // their public broker subscriptions staying available through a handover.
    for (const peer of state.peers.values()) sendQuickPresence(peer);
    publish({ type: 'hello', address: state.address, name: playerName(),
      quick: state.quick ? { ...state.quick.coordinator.presence } : null, quickSequence: state.quickSequence });
  }

  function sendQuickPresence(peer) {
    if (!state.quick && state.quickSequence === 0) return;
    const channel = peer.unreliable;
    if (!peer.open || channel?.readyState !== 'open') return;
    const message = QUICK_CONTROL + JSON.stringify({ sequence: state.quickSequence,
      quick: state.quick ? { ...state.quick.coordinator.presence } : null });
    if (message.length > MAX_QUICK_CONTROL || channel.bufferedAmount + message.length > MAX_CHANNEL_BUFFER) return;
    try { channel.send(message); } catch { /* closing; the next beacon retries */ }
  }

  function updateQuickPresence(peer, value, sequence, source) {
    if (!Number.isSafeInteger(sequence) || sequence < 0 || sequence <= peer.quickSequence) return;
    const presence = value === null ? null : quickPresence(value, peer.id);
    if (value !== null && !presence) return;
    const previous = peer.quick;
    peer.quickSequence = sequence;
    peer.quick = presence;
    if (presence && (!previous || presence.epoch !== previous.epoch || presence.matchId !== previous.matchId ||
        presence.checkpointTick > previous.checkpointTick)) peer.lastProgressAt = Date.now();
    if (presence && (!previous || previous.epoch !== presence.epoch || previous.role !== presence.role ||
        previous.hostId !== presence.hostId || previous.matchId !== presence.matchId))
      emit('diagnostic', { event: 'authority-presence', source, address: addressText(peer.address),
        role: presence.role, epoch: presence.epoch, matchId: presence.matchId, sequence });
  }

  function receiveQuickPresence(peer, text) {
    if (text.length > MAX_QUICK_CONTROL) return;
    try {
      const message = JSON.parse(text.slice(QUICK_CONTROL.length));
      if (!message || typeof message !== 'object' || !Object.hasOwn(message, 'quick')) return;
      // The current RTC connection supplies the identity. A payload cannot
      // announce another player's host claim, and MQTT shares this sequence gate.
      updateQuickPresence(peer, message.quick, message.sequence, 'rtc');
    } catch { /* malformed control packet */ }
  }

  function playerName() {
    try { return localStorage.getItem('halo-web-player-name') || 'Player'; } catch { return 'Player'; }
  }

  function peerFor(id, address, name) {
    if (state.replacedPeers.has(id)) return null;
    let peer = state.peers.get(id);
    if (!peer) {
      peer = { id, address, name: name || 'Player', pc: null, reliable: null, unreliable: null, open: false,
        lastSeen: Date.now(), pendingCandidates: [], quick: null, quickSequence: -1 };
      state.peers.set(id, peer);
    }
    if (address) {
      if (peer.address !== (address >>> 0)) {
        peer.pingPending = peer.hostPing = null;
        invalidatePeerPing(peer);
      }
      if (state.byAddress.get(peer.address) === peer) state.byAddress.delete(peer.address);
      peer.address = address >>> 0;
      const previous = state.byAddress.get(peer.address);
      if (previous && previous !== peer) {
        // A reload keeps its virtual LAN address but gets a new peer id. Retire
        // the old channels before they can mix packets with the new runtime.
        state.replacedPeers.add(previous.id);
        dropPeer(previous);
      }
      state.byAddress.set(peer.address, peer);
    }
    if (name) peer.name = name;
    peer.lastSeen = Date.now();
    return peer;
  }

  function connectionCurrent(peer, pc) {
    return state.peers.get(peer.id) === peer && peer.pc === pc;
  }

  function createConnection(peer) {
    const previous = peer.pc;
    peer.pc = null;
    peer.open = false;
    peer.pingPending = peer.hostPing = null;
    invalidatePeerPing(peer);
    retireStreams(peer);
    try { previous?.close(); } catch { /* closed */ }
    const pc = new RTCPeerConnection({ iceServers: state.iceServers });
    peer.pc = pc;
    peer.connectingSince = Date.now();
    peer.reliable = pc.createDataChannel('reliable', { negotiated: true, id: 0, ordered: true });
    peer.unreliable = pc.createDataChannel('unreliable', { negotiated: true, id: 1, ordered: false, maxRetransmits: 0 });
    peer.received = [];
    peer.receivedHead = 0;
    peer.receivedBytes = 0;
    peer.outgoing = [];
    peer.outgoingHead = peer.outgoingBytes = 0;
    peer.nativeStreams = new Map();
    for (const channel of [peer.reliable, peer.unreliable]) {
      channel.binaryType = 'arraybuffer';
    }
    peer.reliable.onmessage = (event) => {
      if (!connectionCurrent(peer, pc) || !state.shared) return;
      peer.lastPacketAt = Date.now();
      const packet = receivePacket(peer, new Uint8Array(event.data));
      if (!packet) return;
      if (packet.length < PACKET_HEADER || packet.length > state.shared.offsets.netInBytes ||
          new DataView(packet.buffer).getUint32(0, true) !== packet.length ||
          peer.receivedBytes + packet.length > MAX_RELIABLE_QUEUE) {
        // Disconnect on overflow instead of silently corrupting the byte stream.
        dropPeer(peer, 'invalid packet or receive queue overflow');
        return;
      }
      peer.received.push(packet);
      peer.receivedBytes += packet.length;
      pump();
    };
    peer.unreliable.onmessage = (event) => {
      if (!connectionCurrent(peer, pc)) return;
      peer.lastPacketAt = Date.now();
      // Heartbeats use the room's established channel, independently of MQTT.
      // Older launchers ignore these strings and still send normal game packets.
      if (typeof event.data === 'string') {
        if (event.data === 'halo-room-ping-v1' && peer.unreliable.readyState === 'open')
          peer.unreliable.send('halo-room-pong-v1');
        else if (event.data.startsWith(PING_CONTROL)) receivePing(peer, event.data);
        else if (event.data.startsWith(QUICK_CONTROL)) receiveQuickPresence(peer, event.data);
        return;
      }
      // Network events also drain output while background timers are throttled.
      pump();
      const packet = receivePacket(peer, new Uint8Array(event.data));
      if (packet && !state.pendingCloses.size) incoming(packet);
    };
    peer.reliable.bufferedAmountLowThreshold = MAX_CHANNEL_BUFFER / 2;
    peer.reliable.onbufferedamountlow = () => { if (connectionCurrent(peer, pc)) pump(); };
    peer.reliable.onopen = () => {
      if (!connectionCurrent(peer, pc)) return;
      peer.open = true;
      peer.lastPacketAt = Date.now();
      sendQuickPresence(peer);
      emit('joined', { name: peer.name, address: addressText(peer.address) });
      emit('status', status());
    };
    peer.unreliable.onopen = () => { if (connectionCurrent(peer, pc)) sendQuickPresence(peer); };
    peer.reliable.onclose = () => { if (connectionCurrent(peer, pc)) dropPeer(peer, 'reliable channel closed'); };
    pc.onicecandidate = (event) => {
      if (connectionCurrent(peer, pc) && event.candidate)
        publish({ type: 'candidate', to: peer.id, candidate: event.candidate.toJSON() });
    };
    pc.onconnectionstatechange = () => {
      if (!connectionCurrent(peer, pc)) return;
      if (pc.connectionState === 'failed' || pc.connectionState === 'closed') dropPeer(peer, 'RTC ' + pc.connectionState);
    };
    return pc;
  }

  function dropPeer(peer, reason = 'peer left') {
    if (state.peers.get(peer.id) !== peer) return;
    state.peers.delete(peer.id);
    if (state.byAddress.get(peer.address) === peer) state.byAddress.delete(peer.address);
    const pc = peer.pc, wasOpen = peer.open;
    peer.pc = null;
    peer.open = false;
    peer.pingPending = peer.hostPing = null;
    invalidatePeerPing(peer);
    peer.received = [];
    peer.receivedHead = peer.receivedBytes = 0;
    peer.outgoing = [];
    peer.outgoingHead = peer.outgoingBytes = 0;
    emit('diagnostic', { event: 'peer-disconnected', address: addressText(peer.address), reason,
      epoch: state.quick?.coordinator.epoch, matchId: state.quick?.coordinator.presence.matchId });
    retireStreams(peer);
    try { pc?.close(); } catch { /* closed */ }
    if (wasOpen) emit('left', { name: peer.name });
    emit('status', status());
    pump();
  }

  async function handleSignal(message) {
    if (state.replacedPeers.has(message.from)) return;
    const generation = state.roomGeneration;
    if (message.type === 'hello') {
      const known = state.peers.get(message.from);
      const peer = peerFor(message.from, message.address, message.name);
      updateQuickPresence(peer, message.quick, message.quickSequence, 'broker');
      if (!known) hello(); // (so that it knows this machine without waiting)
      // the smaller id makes the offer
      if (!peer.pc && state.id < message.from) {
        const pc = createConnection(peer);
        const offer = await pc.createOffer();
        if (!connectionCurrent(peer, pc) || generation !== state.roomGeneration) return;
        await pc.setLocalDescription(offer);
        if (!connectionCurrent(peer, pc) || generation !== state.roomGeneration) return;
        publish({ type: 'offer', to: peer.id, sdp: pc.localDescription.sdp, address: state.address, name: playerName() });
      }
    } else if (message.type === 'offer') {
      const peer = peerFor(message.from, message.address, message.name);
      const pc = createConnection(peer);
      await pc.setRemoteDescription({ type: 'offer', sdp: message.sdp });
      if (!connectionCurrent(peer, pc) || generation !== state.roomGeneration) return;
      for (const candidate of peer.pendingCandidates.splice(0)) {
        await pc.addIceCandidate(candidate).catch(() => {});
        if (!connectionCurrent(peer, pc) || generation !== state.roomGeneration) return;
      }
      const answer = await pc.createAnswer();
      if (!connectionCurrent(peer, pc) || generation !== state.roomGeneration) return;
      await pc.setLocalDescription(answer);
      if (!connectionCurrent(peer, pc) || generation !== state.roomGeneration) return;
      publish({ type: 'answer', to: peer.id, sdp: pc.localDescription.sdp });
    } else if (message.type === 'answer') {
      const peer = state.peers.get(message.from);
      if (peer && peer.pc && peer.pc.signalingState === 'have-local-offer') {
        const pc = peer.pc;
        await pc.setRemoteDescription({ type: 'answer', sdp: message.sdp });
        if (!connectionCurrent(peer, pc) || generation !== state.roomGeneration) return;
        for (const candidate of peer.pendingCandidates.splice(0)) {
          await pc.addIceCandidate(candidate).catch(() => {});
          if (!connectionCurrent(peer, pc) || generation !== state.roomGeneration) return;
        }
      }
    } else if (message.type === 'candidate') {
      const peer = state.peers.get(message.from);
      if (!peer) return;
      if (peer.pc && peer.pc.remoteDescription) await peer.pc.addIceCandidate(message.candidate).catch(() => {});
      else peer.pendingCandidates.push(message.candidate);
    } else if (message.type === 'bye') {
      const peer = state.peers.get(message.from);
      if (peer) dropPeer(peer);
    }
  }

  function sweep() {
    const now = Date.now();
    for (const peer of [...state.peers.values()]) {
      if (peer.open && peer.quick?.failover && now - peer.lastPacketAt > HaloQuickCoordinator.HEARTBEAT_TIMEOUT_MS) {
        dropPeer(peer, 'gameplay heartbeat timeout');
      } else if (!peer.open && now - peer.lastSeen > PEER_TIMEOUT) {
        dropPeer(peer);
      } else if (!peer.open && peer.pc && now - peer.connectingSince > 15000) {
        // a connection that never opened (a lost offer): start again at the
        // next hello
        try { peer.pc.close(); } catch { /* closed */ }
        peer.pc = null;
      }
      if (peer.open && peer.unreliable?.readyState === 'open' && peer.unreliable.bufferedAmount < MAX_CHANNEL_BUFFER)
        peer.unreliable.send('halo-room-ping-v1');
    }
    sweepPings(now);
  }

  // Measure over the gameplay channel, rather than MQTT or ICE candidate
  // statistics. Only the selected host probes, then publishes its RTT table.
  function pingAuthority() {
    const coordinator = state.quick?.coordinator;
    const result = coordinator?.result, presence = coordinator?.presence;
    if (!result || presence?.gamePhase !== 'playing' || presence.matchId === null ||
        coordinator.recovering || coordinator.missingSince != null || state.quick.held) return null;
    if (!Number.isInteger(presence.matchId) || !Number.isInteger(coordinator.epoch)) return null;
    return { hostId: result.hostId, host: result.hostAddress >>> 0,
      epoch: coordinator.epoch, match: presence.matchId, local: result.role === 'host' };
  }

  function publishPingMemory() {
    if (!state.shared || !Number.isInteger(state.shared.offsets.pingSequence)) return;
    const i32 = words(), pings = state.pings, authority = pings.authority;
    Atomics.add(i32, field('pingSequence'), 1);
    Atomics.store(i32, field('pingHost'), authority?.host || 0);
    Atomics.store(i32, field('pingEpoch'), authority?.epoch || 0);
    Atomics.store(i32, field('pingUpdated'), pings.updated | 0);
    const rows = pings.rows.slice(0, Math.min(PING_PEERS, state.shared.offsets.pingPeerCount));
    Atomics.store(i32, field('pingCount'), rows.length);
    rows.forEach(([address, ms], index) => {
      Atomics.store(i32, field('pingPeers') + index * 2, address | 0);
      Atomics.store(i32, field('pingPeers') + index * 2 + 1, ms);
    });
    Atomics.add(i32, field('pingSequence'), 1);
  }

  function syncPingAuthority() {
    const authority = pingAuthority();
    const signature = authority ? `${state.roomGeneration}:${authority.hostId}:${authority.host}:${authority.epoch}:${authority.match}` : '';
    if (signature !== state.pings.signature) {
      state.pings = { signature, authority, rows: [], updated: 0, sequence: 0, received: 0 };
      for (const peer of state.peers.values()) peer.pingPending = peer.hostPing = null;
      publishPingMemory();
    }
    return authority;
  }

  function invalidatePeerPing(peer) {
    if (state.pings.authority?.hostId === peer.id) {
      state.pings.rows = []; state.pings.updated = 0;
      // A reopened connection must not revive an old table sequence.
    } else state.pings.rows = state.pings.rows.filter(row => row[0] !== peer.address);
    publishPingMemory();
  }

  function sendPing(peer, message) {
    if (peer.open && peer.unreliable?.readyState === 'open' && peer.unreliable.bufferedAmount < MAX_CHANNEL_BUFFER)
      peer.unreliable.send(PING_CONTROL + JSON.stringify(message));
  }

  function hostPingTable(now, authority, broadcast = true) {
    state.pings.rows = [[state.address, 0, state.id], ...[...state.peers.values()]
      .filter(peer => peer.open && peer.quick?.epoch === authority.epoch && peer.quick?.matchId === authority.match)
      .slice(0, PING_PEERS - 1).map(peer => [peer.address,
        peer.hostPing && now - peer.hostPing.at >= 0 && now - peer.hostPing.at < PING_STALE_MS ? peer.hostPing.ms : -1, peer.id])];
    state.pings.updated = now;
    publishPingMemory();
    if (!broadcast) return;
    const table = { ...authority, local: undefined, type: 'table', sequence: ++state.pings.sequence, rows: state.pings.rows };
    for (const peer of state.peers.values()) sendPing(peer, table);
  }

  function sweepPings(now) {
    const authority = syncPingAuthority();
    if (!authority?.local) return;
    for (const peer of state.peers.values()) {
      if (!peer.open || peer.quick?.epoch !== authority.epoch || peer.quick?.matchId !== authority.match) continue;
      if (peer.pingPending && now - peer.pingPending.at < PING_STALE_MS && now >= peer.pingPending.at) continue;
      peer.pingPending = { sequence: ++state.pingProbe, at: now };
      sendPing(peer, { ...authority, local: undefined, type: 'probe', sequence: peer.pingPending.sequence });
    }
    hostPingTable(now, authority);
  }

  function receivePing(peer, wire) {
    const authority = syncPingAuthority();
    if (!authority || wire.length > 8192 || !peer.open) return;
    let message;
    try { message = JSON.parse(wire.slice(PING_CONTROL.length)); } catch { return; }
    if (!message || message.hostId !== authority.hostId || message.host !== authority.host ||
        message.epoch !== authority.epoch || message.match !== authority.match ||
        !Number.isSafeInteger(message.sequence) || message.sequence <= 0) return;
    const now = Date.now();
    if (message.type === 'probe' && !authority.local && peer.id === authority.hostId && peer.address === authority.host) {
      sendPing(peer, { ...message, type: 'pong' });
    } else if (message.type === 'pong' && authority.local && peer.pingPending?.sequence === message.sequence) {
      const ms = now - peer.pingPending.at;
      peer.pingPending = null;
      if (ms < 0 || ms >= PING_STALE_MS) return;
      peer.hostPing = { ms: Math.round(ms), at: now };
      hostPingTable(now, authority, false);
    } else if (message.type === 'table' && !authority.local && peer.id === authority.hostId && peer.address === authority.host) {
      if (message.sequence <= state.pings.received || !Array.isArray(message.rows) ||
          message.rows.length < 1 || message.rows.length > PING_PEERS) return;
      const addresses = new Set();
      for (const row of message.rows) {
        if (!Array.isArray(row) || row.length !== 3 || !/^[a-f0-9]{16}$/.test(row[2] || '') ||
            !Number.isInteger(row[0]) || row[0] <= 0 || row[0] > 0xffffffff ||
            !Number.isInteger(row[1]) || row[1] < -1 || row[1] >= PING_STALE_MS || addresses.has(row[0]) ||
            (row[0] === authority.host && (row[1] !== 0 || row[2] !== authority.hostId))) return;
        addresses.add(row[0]);
      }
      if (!addresses.has(authority.host)) return;
      state.pings.received = message.sequence;
      // Address reuse after a reload must not attach the departed peer's
      // RTT to its replacement, even if an older table arrives late.
      state.pings.rows = message.rows.filter(([address, , id]) =>
        address === state.address ? id === state.id : state.byAddress.get(address)?.id === id);
      state.pings.updated = now;
      publishPingMemory();
    }
  }

  // ---------- the rings (port/web/src/web_shared.h)

  function words() {
    return new Int32Array(state.shared.memory.buffer);
  }

  function field(name) {
    return (state.shared.base + state.shared.offsets[name]) >> 2;
  }

  function incoming(packet) {
    if (!state.shared || packet.length < PACKET_HEADER) return false;
    const i32 = words();
    const bytes = new Uint8Array(state.shared.memory.buffer);
    const capacity = state.shared.offsets.netInBytes;
    const ring = state.shared.base + state.shared.offsets.netIn;
    const write = Atomics.load(i32, field('netInWrite')) >>> 0;
    const read = Atomics.load(i32, field('netInRead')) >>> 0;
    const size = new DataView(packet.buffer, packet.byteOffset, packet.byteLength).getUint32(0, true);
    if (size !== packet.length || capacity - ((write - read) >>> 0) < size) return false;
    const start = write & (capacity - 1);
    const first = Math.min(capacity - start, size);
    bytes.set(packet.subarray(0, first), ring + start);
    if (first < size) bytes.set(packet.subarray(first), ring);
    Atomics.store(i32, field('netInWrite'), (write + size) | 0);
    return true;
  }

  function refuse(header) {
    // nobody at that address: the connection is refused
    const packet = new Uint8Array(PACKET_HEADER);
    const view = new DataView(packet.buffer);
    view.setUint32(0, PACKET_HEADER, true);
    view.setUint32(4, KIND.REFUSE, true);
    view.setUint32(8, header.destination, true);
    view.setUint32(12, header.source, true);
    view.setUint16(16, header.destinationPort, true);
    view.setUint16(18, header.sourcePort, true);
    view.setUint32(20, 0, true);
    incoming(packet);
  }

  function isBroadcast(address) {
    return address === 0xFFFFFFFF || (address >>> 24) === 255;
  }

  function trackStream(peer, packet, outgoing = false) {
    const view = new DataView(packet.buffer, packet.byteOffset, packet.byteLength);
    const kind = view.getUint32(4, true);
    if (kind !== KIND.OPEN && kind !== KIND.CLOSE && kind !== KIND.REFUSE) return;
    // Normalize to the incoming direction: remote address/port, local address/port.
    const source = view.getUint32(outgoing ? 12 : 8, true);
    const destination = view.getUint32(outgoing ? 8 : 12, true);
    const sourcePort = view.getUint16(outgoing ? 18 : 16, true);
    const destinationPort = view.getUint16(outgoing ? 16 : 18, true);
    const key = `${source}:${sourcePort}:${destination}:${destinationPort}`;
    if (kind !== KIND.OPEN) { peer.nativeStreams.delete(key); return; }
    const close = new Uint8Array(PACKET_HEADER), header = new DataView(close.buffer);
    header.setUint32(0, PACKET_HEADER, true);
    header.setUint32(4, KIND.CLOSE, true);
    header.setUint32(8, source, true);
    header.setUint32(12, destination, true);
    header.setUint16(16, sourcePort, true);
    header.setUint16(18, destinationPort, true);
    peer.nativeStreams.set(key, close);
  }

  function retireStreams(peer) {
    for (const [key, close] of peer.nativeStreams || []) state.pendingCloses.set(key, close);
    peer.nativeStreams?.clear();
  }

  function send(channel, packet) {
    if (state.quick) {
      const frame = new Uint8Array(packet.length + MIGRATION_HEADER), view = new DataView(frame.buffer);
      view.setUint32(0, MIGRATION_MAGIC, true);
      view.setUint32(4, state.quick.coordinator.epoch, true);
      frame.set(packet, MIGRATION_HEADER);
      packet = frame;
    }
    if (channel && channel.readyState === 'open') {
      if (channel.bufferedAmount + packet.length > MAX_CHANNEL_BUFFER) return false;
      try { channel.send(packet); return true; } catch { /* closing */ }
    }
    return false;
  }

  function receivePacket(peer, frame) {
    const epoch = state.quick?.coordinator.epoch;
    let packet = frame;
    if (frame.length >= MIGRATION_HEADER && new DataView(frame.buffer, frame.byteOffset, frame.byteLength).getUint32(0, true) === MIGRATION_MAGIC) {
      const generation = new DataView(frame.buffer, frame.byteOffset, frame.byteLength).getUint32(4, true);
      if (generation !== epoch) return null;
      packet = frame.slice(MIGRATION_HEADER);
      packet.haloEpoch = generation;
    } else if (epoch !== undefined) return null;
    if (epoch !== undefined) {
      if (packet.length < PACKET_HEADER) return null;
      const view = new DataView(packet.buffer, packet.byteOffset, packet.byteLength);
      if (view.getUint32(8, true) !== peer.address) return null;
      const destination = view.getUint32(12, true);
      if (destination !== state.address && !isBroadcast(destination)) return null;
    }
    return packet;
  }

  function flushPeerOutput(peer) {
    while (peer.outgoingHead < peer.outgoing.length) {
      const queued = peer.outgoing[peer.outgoingHead];
      if (queued.epoch === state.quick?.coordinator.epoch && !send(peer.reliable, queued.packet)) break;
      peer.outgoingHead++;
      peer.outgoingBytes -= queued.packet.length;
    }
    if (peer.outgoingHead === peer.outgoing.length) {
      peer.outgoing.length = 0;
      peer.outgoingHead = 0;
    } else if (peer.outgoingHead > 1024) {
      peer.outgoing.splice(0, peer.outgoingHead);
      peer.outgoingHead = 0;
    }
  }

  function queuePeerOutput(peer, packet) {
    flushPeerOutput(peer);
    if (!peer.outgoingBytes && send(peer.reliable, packet)) return true;
    // Keep stream order within this peer, while other players continue to
    // receive gameplay and keepalives from the shared native output ring.
    if (peer.outgoingBytes + packet.length > MAX_CHANNEL_BUFFER) {
      dropPeer(peer, 'send queue overflow');
      return false;
    }
    peer.outgoing.push({ packet, epoch: state.quick?.coordinator.epoch });
    peer.outgoingBytes += packet.length;
    return true;
  }

  function pump() {
    if (state.pumping) return;
    state.pumping = true;
    try { pumpOnce(); } finally { state.pumping = false; }
  }

  function pumpOnce() {
    syncPingAuthority();
    if (!state.shared) return;
    if (state.transport) state.transport.flush(incoming);
    for (const peer of state.peers.values()) if (peer.open) flushPeerOutput(peer);
    for (const [key, close] of state.pendingCloses) {
      if (!incoming(close)) break;
      state.pendingCloses.delete(key);
    }
    // A full native ring retains EOF until it fits. Do not admit a new OPEN
    // first: the same tuple may already belong to that replacement by then.
    for (const peer of state.pendingCloses.size ? [] : state.peers.values()) {
      if (!peer.received) continue;
      while (peer.receivedHead < peer.received.length) {
        const packet = peer.received[peer.receivedHead];
        if (packet.haloEpoch !== state.quick?.coordinator.epoch) {
          peer.receivedHead++;
          peer.receivedBytes -= packet.length;
          continue;
        }
        if (!incoming(packet)) break;
        peer.receivedHead++;
        trackStream(peer, packet);
        peer.receivedBytes -= packet.length;
      }
      if (peer.receivedHead === peer.received.length) {
        peer.received.length = 0;
        peer.receivedHead = 0;
      } else if (peer.receivedHead > 1024) {
        peer.received.splice(0, peer.receivedHead);
        peer.receivedHead = 0;
      }
    }
    const i32 = words();
    const memory = new Uint8Array(state.shared.memory.buffer);
    const capacity = state.shared.offsets.netOutBytes;
    const ring = state.shared.base + state.shared.offsets.netOut;
    let read = Atomics.load(i32, field('netOutRead')) >>> 0;
    const write = Atomics.load(i32, field('netOutWrite')) >>> 0;
    while (read !== write) {
      const start = read & (capacity - 1);
      const headerBytes = new Uint8Array(PACKET_HEADER);
      for (let index = 0; index < PACKET_HEADER; index++) headerBytes[index] = memory[ring + ((start + index) & (capacity - 1))];
      const view = new DataView(headerBytes.buffer);
      const size = view.getUint32(0, true);
      if (size < PACKET_HEADER || size > capacity) {
        read = write;
        break;
      }
      const packet = new Uint8Array(size);
      const first = Math.min(capacity - start, size);
      packet.set(memory.subarray(ring + start, ring + start + first));
      if (first < size) packet.set(memory.subarray(ring, ring + size - first), first);
      const header = {
        kind: view.getUint32(4, true),
        source: view.getUint32(8, true),
        destination: view.getUint32(12, true),
        sourcePort: view.getUint16(16, true),
        destinationPort: view.getUint16(18, true),
      };
      if (state.transport) {
        // Leave reliable bytes in the ring until the socket can accept them.
        if (!state.transport.send(packet)) break;
      } else if (header.kind === KIND.DATAGRAM) {
        if (isBroadcast(header.destination)) {
          for (const peer of state.peers.values()) if (peer.open) send(peer.unreliable, packet);
        } else {
          const peer = state.byAddress.get(header.destination);
          if (peer && peer.open) send(peer.unreliable, packet);
        }
      } else {
        const peer = state.byAddress.get(header.destination);
        // WebRTC backpressure must not consume bytes the channel did not take.
        if (peer && peer.open) {
          const tuple = `${header.destination}:${header.destinationPort}:${header.source}:${header.sourcePort}`;
          // Native output for a departed endpoint can arrive after the fresh
          // RTC channel opens. That generation has not opened this stream yet.
          if (header.kind !== KIND.DATA || peer.nativeStreams.has(tuple)) {
            if (queuePeerOutput(peer, packet)) trackStream(peer, packet, true);
          }
        }
        else if (header.kind === KIND.OPEN) refuse(header);
      }
      read = (read + size) >>> 0;
    }
    Atomics.store(i32, field('netOutRead'), read | 0);
  }

  // ---------- the page's interface

  function attach({ memory, base, offsets }) {
    state.shared = { memory, base, offsets };
    const i32 = words();
    Atomics.store(i32, field('netLocalAddress'), state.address | 0);
    publishPingMemory();
    if (state.transport) state.transport.attach(state.shared);
    // (the game's sockets look at the incoming ring every few milliseconds;
    // this looks at the outgoing one as often)
    state.pumpTimer = setInterval(pump, 4);
  }

  async function join(code, options = {}) {
    if (state.transport) throw new Error('Reload to switch from a desktop invite to a browser room.');
    code = String(code || '').trim().toUpperCase().replace(/[^A-Z0-9]/g, '');
    if (code.length < 4) throw new Error('A room code has at least 4 letters or digits.');
    await leave();
    const { key, topic } = await deriveRoom(code);
    state.room = code;
    state.key = key;
    state.topic = topic;
    if (options.turn && options.turn.urls) state.iceServers = [...STUN, options.turn];
    const brokers = options.brokers && options.brokers.length ? options.brokers : BROKERS;
    state.brokers = brokers.map(connectBroker);
    state.helloTimer = setInterval(() => { hello(); sweep(); }, HELLO_INTERVAL);
    emit('status', status());
    return code;
  }

  async function leave() {
    cancelQuickPlay();
    state.roomGeneration++;
    state.replacedPeers.clear();
    if (!state.room) return;
    await publish({ type: 'bye' }).catch(() => {});
    clearInterval(state.helloTimer);
    for (const peer of [...state.peers.values()]) dropPeer(peer);
    for (const broker of state.brokers) {
      clearTimeout(broker.retry);
      clearInterval(broker.ping);
      try { broker.socket && broker.socket.close(); } catch { /* closed */ }
    }
    state.brokers = [];
    state.room = null;
    emit('status', status());
  }

  function newRoomCode() {
    const alphabet = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
    const bytes = crypto.getRandomValues(new Uint8Array(6));
    return Array.from(bytes, (b) => alphabet[b % alphabet.length]).join('');
  }

  function on(listener) {
    state.listeners.add(listener);
  }

  state.address = localAddress();

  async function useTransport(transport) {
    if (state.shared) throw new Error('Reload before joining another desktop invite.');
    await leave();
    if (state.transport) state.transport.close();
    state.transport = transport;
    state.address = transport.address;
  }

  function quickPresence(value, id) {
    if (!value || !/^[a-f0-9]{16}$/.test(id || '') || !['candidate', 'host', 'join'].includes(value.role)) return null;
    if (value.hostId !== null && !/^[a-f0-9]{16}$/.test(value.hostId || '')) return null;
    if (!['electing', 'reserved', 'launched'].includes(value.phase)) return null;
    if ((value.role === 'candidate') !== (value.phase === 'electing')) return null;
    if (value.role === 'host' && value.hostId !== id) return null;
    if (value.role === 'join' && (!value.hostId || value.hostId === id)) return null;
    return { role: value.role, hostId: value.hostId, phase: value.phase,
      epoch: Number.isSafeInteger(value.epoch) && value.epoch >= 0 && value.epoch <= 0x7fffffff ? value.epoch : 0,
      failover: value.failover === true,
      migration: value.migration === true,
      matchId: Number.isInteger(value.matchId) && value.matchId >= 0 && value.matchId <= 0xffffffff ? value.matchId : null,
      checkpointTick: Number.isInteger(value.checkpointTick) && value.checkpointTick >= 0 && value.checkpointTick <= 0x7fffffff ? value.checkpointTick : -1,
      gamePhase: ['hosting', 'searching', 'joining', 'waiting', 'loading', 'playing', 'migration-failed'].includes(value.gamePhase) ? value.gamePhase : '' };
  }

  function publishQuick() { state.quickSequence++; hello(); }

  function cancelQuickPlay() {
    const attempt = state.quick;
    if (!attempt) return;
    state.quick = null;
    syncPingAuthority();
    clearInterval(attempt.timer);
    attempt.signal?.removeEventListener('abort', attempt.abort);
    if (!attempt.resolved) attempt.reject(new DOMException('Quick play cancelled.', 'AbortError'));
    publishQuick();
  }

  async function quickPlay({ signal, onStatus = () => {}, onFailover = () => {}, onReconnect = () => {} } = {}) {
    if (signal?.aborted) throw new DOMException('Quick play cancelled.', 'AbortError');
    if (state.transport) {
      if (!state.transport.connected || !state.transport.hostAddress) throw new Error('The invited host is not connected.');
      return { role: 'join', room: null, hostId: null, hostAddress: state.transport.hostAddress };
    }
    if (!state.room) throw new Error('Join a browser room before starting multiplayer.');
    if (state.quick) return state.quick.promise;
    const coordinator = new HaloQuickCoordinator(state.id, state.address, Date.now());
    const attempt = { coordinator, room: state.room, signal, timer: null, resolved: false,
      previous: JSON.stringify(coordinator.presence), lastStatus: '', abort: cancelQuickPlay, lastResult: null,
      held: false, migrationPending: false, wireEpoch: 0, onReconnect };
    attempt.promise = new Promise((resolve, reject) => { attempt.resolve = resolve; attempt.reject = reject; });
    state.quick = attempt;
    signal?.addEventListener('abort', attempt.abort, { once: true });
    const tick = () => {
      if (state.quick !== attempt) return;
      const result = coordinator.tick(Date.now(), [...state.peers.values()], state.brokers.some(broker => broker.ready));
      if (coordinator.epoch !== attempt.wireEpoch) {
        attempt.wireEpoch = coordinator.epoch;
        for (const peer of state.peers.values()) peer.nativeStreams?.clear();
        if (state.shared) {
          const i32 = words();
          Atomics.store(i32, field('netOutRead'), Atomics.load(i32, field('netOutWrite')));
        }
        if (attempt.resolved) {
          attempt.held = true;
          onStatus({ state: 'recovering', message: 'Choosing a replacement host and preserving the match…', hold: true });
        }
      }
      const presence = JSON.stringify(coordinator.presence);
      if (presence !== attempt.previous) { attempt.previous = presence; publishQuick(); }
      if (result.error) {
        if (attempt.resolved) onStatus({ state: 'error', message: result.error });
        attempt.resolved = true; // report this failure, rather than an AbortError
        attempt.reject(new Error(result.error));
        cancelQuickPlay();
      } else if (result.result) {
        const selection = { ...result.result, room: attempt.room, epoch: coordinator.epoch };
        const signature = JSON.stringify(result.result) + ':' + coordinator.epoch;
        if (result.reconnect) {
          attempt.migrationPending = false;
          reconnectQuickClient(attempt);
        }
        if (signature !== attempt.lastResult) {
          attempt.lastResult = signature;
          if (!attempt.resolved) { attempt.resolved = true; attempt.resolve(selection); }
          else { attempt.held = true; attempt.migrationPending = true; onFailover(selection); }
        } else if (attempt.held && !attempt.migrationPending && !coordinator.recovering && coordinator.missingSince === null &&
            coordinator.presence.gamePhase === 'playing') {
          attempt.held = false;
          onStatus({ state: 'playing', message: 'Multiplayer is ready.', hold: false });
        }
        coordinator.recovering = false;
      } else if (result.message !== attempt.lastStatus) {
        attempt.lastStatus = result.message;
        const hold = attempt.resolved && (coordinator.recovering || result.state === 'reconnecting');
        if (hold) attempt.held = true;
        if (result.state === 'reconnecting' && coordinator.hasMatch) reconnectQuickClient(attempt);
        onStatus({ state: coordinator.recovering ? 'recovering' : result.state,
          message: result.message,
          ...(hold ? { hold: true } : {}) });
      }
    };
    publishQuick();
    attempt.timer = setInterval(tick, 100);
    tick();
    return attempt.promise;
  }

  function quickPlayPhase(phase) {
    if (phase === 'menu' || phase === 'error') {
      const coordinator = state.quick?.coordinator;
      if (coordinator && (coordinator.recovering || coordinator.missingSince !== null))
        return true;
      cancelQuickPlay(); return false;
    }
    if (!state.quick || !state.quick.resolved) return;
    if (phase === 'migration-failed') {
      const attempt = state.quick, coordinator = attempt.coordinator;
      attempt.held = true;
      attempt.migrationPending = true;
      coordinator.presence.gamePhase = phase;
      // A checkpoint from the preceding authority cannot make a failed
      // reattachment into a healthy host. A later verified transfer and native
      // playing receipt can restore eligibility without resetting the match.
      coordinator.presence.checkpointTick = -1;
      if (coordinator.presence.role === 'host') coordinator.recover(Date.now(), coordinator.epoch);
      publishQuick();
      return false;
    }
    if (!['hosting', 'searching', 'joining', 'waiting', 'loading', 'playing'].includes(phase)) return;
    if (phase === 'playing') state.quick.migrationPending = false;
    state.quick.coordinator.launched(phase);
    publishQuick();
  }

  function quickPlayCheckpoint({ epoch, tick, matchId }) {
    if (state.quick?.coordinator.checkpoint(epoch, tick, matchId)) publishQuick();
  }

  function reconnectQuickClient(attempt) {
    const coordinator = attempt.coordinator;
    if (attempt.migrationPending || coordinator.result?.role !== 'join') return;
    attempt.held = true;
    attempt.migrationPending = true;
    attempt.onReconnect({ ...coordinator.result, room: attempt.room, epoch: coordinator.epoch });
  }

  function quickPlayLost() {
    const attempt = state.quick;
    if (!attempt?.resolved) return false;
    const coordinator = attempt.coordinator;
    // Native EOF can arrive after signalling has begun the election, or after
    // its replacement selection has already been sent to the engine. Consume
    // those old-session receipts without cancelling the preserved match or
    // advancing its authority a second time.
    // Native playing completes the handshake before the page's next tick
    // clears held. A new failure in that interval needs another repair; the
    // presentation hold alone does not mean native recovery is still pending.
    if (coordinator.hasMatch && (coordinator.recovering || attempt.migrationPending)) return true;
    if (coordinator.result?.role === 'host') {
      if (!coordinator.hasMatch) return false;
      // A broken host's local native connection cannot serve this authority.
      // Keep its world, withdraw its candidacy, and follow a healthy survivor
      // rather than cancelling quick play or immediately choosing it again.
      coordinator.presence.gamePhase = 'migration-failed';
      coordinator.presence.checkpointTick = -1;
    } else if (coordinator.result?.role !== 'join') return false;
    else {
      reconnectQuickClient(attempt);
      return true;
    }
    attempt.held = true;
    coordinator.recover(Date.now());
    publishQuick();
    return true;
  }

  return { attach, join, leave, newRoomCode, on, status, addressText, useTransport,
    quickPlay, cancelQuickPlay, quickPlayLost, quickPlayPhase, quickPlayCheckpoint, quickPlayStarted: () => quickPlayPhase('loading'),
    get address() { return state.address; } };
})();
if (typeof module !== 'undefined') module.exports = { HaloQuickCoordinator };
