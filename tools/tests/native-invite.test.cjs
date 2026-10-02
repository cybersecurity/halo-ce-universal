const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const { network } = require('./web-quick-play-fixture.cjs');
const Invite = require('../../port/web/site/native-invite.js');
const Gateway = require('../../port/web/site/gateway.js');
const TOKEN = '0123456789ab' + 'c'.repeat(32);
const ADDRESS = 0x01024064;
const EXPECTED_MAPS = [
  'ui', 'a10', 'a30', 'a50', 'b30', 'b40', 'c10', 'c20', 'c40', 'd20', 'd40',
  'beavercreek', 'bloodgulch', 'boardingaction', 'carousel', 'chillout', 'damnation',
  'hangemhigh', 'longest', 'prisoner', 'putput', 'ratrace', 'sidewinder', 'wizard',
].map(name => name + '.map');
const QUICK_MAPS = ['ui.map', 'beavercreek.map'];
function fullMaps(overrides = {}) {
  return { files: EXPECTED_MAPS.slice(), bytes: 1_856_530_432,
    dataRoot: '/data', saveRoot: '/data/save', ...overrides };
}
function quickMaps(overrides = {}) {
  return { files: QUICK_MAPS.slice(), bytes: 36_268_032, requiredBytes: 36_268_032,
    dataRoot: '/data', saveRoot: '/data/save', ...overrides };
}
function packet(kind = 3, length = 4) {
  const bytes = new Uint8Array((24 + length + 3) & ~3), v = new DataView(bytes.buffer);
  v.setUint32(0, bytes.length, true); v.setUint32(4, kind, true); v.setUint32(20, length, true);
  return bytes;
}
class FakeSocket {
  static OPEN = 1;
  static latest;
  readyState = 1; bufferedAmount = 0; sent = [];
  constructor(url) { this.url = url; FakeSocket.latest = this; }
  send(value) { this.sent.push(value); }
  close() { this.readyState = 3; }
  json(value) { this.onmessage({ data: JSON.stringify(value) }); }
}
global.WebSocket = FakeSocket;

test('native and HTTPS invites preserve the exact secret in a fragment', () => {
  assert.equal(Invite.parse('halo://join/' + TOKEN.toUpperCase()), TOKEN);
  const url = Invite.link('https://fqlx.github.io/halo-ce-universal/?room=OLD', TOKEN);
  assert.equal(url, 'https://fqlx.github.io/halo-ce-universal/#join=' + TOKEN);
  assert.equal(new URL(url).search, '');
  assert.equal(Invite.parse(url), TOKEN);
});
test('reject partial, extra path, misleading scheme, and insecure public links', () => {
  for (const value of [TOKEN.slice(1), TOKEN + 'f', 'halo://join/' + TOKEN + '/extra',
    'javascript:' + TOKEN, 'http://example.com/#join=' + TOKEN]) assert.equal(Invite.parse(value), null);
  assert.throws(() => Invite.link('http://example.com/', TOKEN));
  assert.equal(Invite.parse('http://localhost:8779/#join=' + TOKEN), TOKEN);
});
test('packet validator rejects length, kind, and control-payload mismatches', () => {
  assert.ok(Gateway.validFrame(packet()));
  assert.ok(Gateway.validFrame(packet(2, 0)));
  assert.equal(Gateway.validFrame(packet(2, 4)), false);
  assert.equal(Gateway.validFrame(packet(9, 0)), false);
  const invalid = packet(); new DataView(invalid.buffer).setUint32(0, 24, true);
  assert.equal(Gateway.validFrame(invalid), false);
});
test('virtual addresses and identifier words match the native little-endian ABI', () => {
  assert.ok(Gateway.validAddress(ADDRESS));
  assert.equal(Gateway.validAddress(0x0100007f), false);
  assert.equal(Gateway.validAddress(0x01008064), false);
  assert.deepEqual(Gateway.idWords('010203040506'), [0x04030201, 0x0605]);
});
test('relay handshake publishes identity and peer map, queues reliable bytes until accepted', async () => {
  const statuses = [];
  const promise = Gateway.connect('ws://localhost:8781/join', TOKEN, { onStatus: v => statuses.push(v) });
  const socket = FakeSocket.latest;
  socket.onopen();
  assert.deepEqual(JSON.parse(socket.sent[0]), { type: 'join', invite: TOKEN });
  assert.equal(socket.url.search, '');
  socket.json({ type: 'ready', identifier: '010203040506', address: ADDRESS });
  const gateway = await promise;
  assert.equal(gateway.connected, false);
  assert.equal(gateway.hostAddress, 0);
  socket.json({ type: 'peer', identifier: TOKEN.slice(0, 12), address: ADDRESS + 0x1000000, connected: true });
  assert.equal(gateway.connected, true);
  assert.equal(gateway.hostAddress, ADDRESS + 0x1000000);
  const memory = { buffer: new SharedArrayBuffer(512) };
  gateway.attach({ memory, base: 0, offsets: { gatewayEnabled: 0, gatewayIdentifier: 4,
    netLocalAddress: 12, gatewayPeers: 16, gatewayPeerCount: 32 } });
  const words = new Int32Array(memory.buffer);
  assert.equal(words[0], 1); assert.equal(words[1], 0x04030201); assert.equal(words[3], ADDRESS);
  assert.equal(words[6], ADDRESS + 0x1000000);
  const frame = packet(); socket.onmessage({ data: frame.buffer });
  gateway.flush(() => false);
  let count = 0; gateway.flush(value => { count++; assert.deepEqual(value, frame); return true; });
  gateway.flush(() => { throw new Error('duplicate'); });
  assert.equal(count, 1);
  socket.bufferedAmount = 2 * 1024 * 1024;
  assert.equal(gateway.send(frame), false);
  socket.bufferedAmount = 0;
  assert.equal(gateway.send(frame), true);
  socket.json({ type: 'peer', identifier: TOKEN.slice(0, 12), address: ADDRESS + 0x1000000, connected: false });
  assert.equal(words[6], 0); assert.equal(gateway.connected, false);
  assert.equal(gateway.hostAddress, 0);
  gateway.close();
  assert.equal(gateway.hostAddress, 0);
  assert.equal(words[3], 0);
  socket.json({ type: 'peer', identifier: TOKEN.slice(0, 12), address: ADDRESS + 0x1000000, connected: true });
  assert.equal(gateway.connected, false);
  assert.equal(words[6], 0);
  assert.ok(statuses.some(value => value.state === 'connected'));
});
test('invalid identity and insecure relay fail closed', async () => {
  await assert.rejects(Gateway.connect('ws://example.com/join', TOKEN));
  const promise = Gateway.connect('ws://localhost:8781/join', TOKEN);
  FakeSocket.latest.json({ type: 'ready', identifier: 'bad', address: ADDRESS });
  await assert.rejects(promise, /identity/);
  assert.equal(FakeSocket.latest.readyState, 3);
});

// Exercise the real launcher's public button handlers and gateway events;
// stub browser capabilities without starting a game or reserving 2.1 GB.
async function launcher(useTransport = async () => {}, options = {}) {
  const elements = new Map();
  const storage = options.storage || new Map(), joins = [], listeners = [];
  const quickCalls = [], phases = [], checkpoints = [], windowEvents = new Map(), uiActive = [];
  const inspections = [], imports = [];
  let room = null;
  function element(id) {
    if (!elements.has(id)) elements.set(id, {
      value: '', disabled: false, hidden: true, dataset: {}, style: {}, children: [],
      appendChild(child) { this.children.push(child); },
      classList: { add() {}, remove() {} }, getContext() { return {}; },
    });
    return elements.get(id);
  }
  const context = {
    console: { log() {} }, URL, URLSearchParams, SharedArrayBuffer, AbortController, DOMException, TextDecoder,
    setTimeout: options.setTimeout || setTimeout, clearTimeout: options.clearTimeout || clearTimeout,
    setInterval() {}, clearInterval() {}, performance: { now: () => 0 },
    navigator: { userAgent: 'Test', platform: 'Test', storage: options.gameStorage || { getDirectory() {} },
      ...(options.gameLocks ? { locks: options.gameLocks } : {}),
      ...(options.share ? { share: options.share } : {}),
      ...(options.serviceWorker ? { serviceWorker: options.serviceWorker } : {}) },
    location: new URL(options.url || 'http://localhost:8780/'),
    document: { getElementById: element, createElement: () => element(Symbol()),
      body: element('body'), documentElement: {}, addEventListener() {} },
    localStorage: { getItem: key => storage.get(key) ?? null,
      setItem: (key, value) => storage.set(key, value), removeItem: key => storage.delete(key) },
    matchMedia: () => ({ matches: false }),
    addEventListener(type, listener) {
      if (!windowEvents.has(type)) windowEvents.set(type, []);
      windowEvents.get(type).push(listener);
    },
    screen: {}, history: { pushState() {}, replaceState(_state, _title, url) { context.location.href = String(url); } },
    crossOriginIsolated: true,
    OffscreenCanvas: class { getContext() { return {}; } },
    WebAssembly: { Memory: options.Memory || class {} }, fetch: options.fetch || (async () => { throw new Error('Offline'); }),
    Worker: class {
      postMessage(message) {
        imports.push(message.file);
        options.onImport?.(message.file);
        queueMicrotask(() => this.onmessage({ data: { type: 'done', files: 24, bytes: 1_856_530_432 } }));
      }
      terminate() {}
    },
    HALO_BROWSER_CONFIG: { relayUrl: options.relayUrl ?? 'ws://localhost:8781/join',
      ...('defaultRoom' in options ? { defaultRoom: options.defaultRoom } : {}) },
    HaloInvite: Invite, HaloGateway: Gateway,
    HaloInput: { setUIActive: active => uiActive.push(active) },
    HaloNet: {
      on(listener) { listeners.push(listener); }, addressText: address => [address & 255,
        (address >>> 8) & 255, (address >>> 16) & 255, address >>> 24].join('.'),
      status: () => ({ room, players: 0, brokers: 1, names: [] }), useTransport,
      quickPlay(request) {
        quickCalls.push(request);
        if (options.quickPlay) return options.quickPlay({ ...request, room });
        return new Promise((_resolve, reject) => request.signal.addEventListener('abort',
          () => reject(new DOMException('Aborted', 'AbortError')), { once: true }));
      },
      cancelQuickPlay() { options.net?.cancelQuickPlay(); },
      quickPlayStarted() { options.net?.quickPlayStarted(); }, newRoomCode: () => 'PRIVATE7',
      quickPlayLost: () => options.net ? options.net.quickPlayLost() : options.hostLost ?? true,
      quickPlayCheckpoint(value) { checkpoints.push(value); options.net?.quickPlayCheckpoint(value); },
      quickPlayPhase(phase) {
        phases.push(phase);
        return options.net ? options.net.quickPlayPhase(phase) : options.recovering ?? false;
      },
      async join(code) {
        joins.push(code);
        await options.beforeJoin?.();
        room = code.trim().toUpperCase();
        listeners.forEach(listener => listener('status', this.status()));
        return room;
      },
      async leave() {
        room = null;
        listeners.forEach(listener => listener('status', this.status()));
      },
    },
    HaloCache: { expected: EXPECTED_MAPS.slice(),
      mapsState: async request => {
        inspections.push(Array.from(request.required));
        return options.mapsState ? options.mapsState(request) : fullMaps();
      },
      withLock: task => task() },
  };
  context.window = context;
  vm.runInNewContext(fs.readFileSync(require.resolve('../../port/web/site/app.js'), 'utf8'), context);
  await new Promise(setImmediate);
  if (!options.mapsState) assert.equal(element('step-play').hidden, false, 'launcher reached the cached-data ready state');
  return { element, context, storage, joins, quickCalls, phases, checkpoints, windowEvents, uiActive, inspections, imports,
    emitNetwork: (type, detail) => listeners.forEach(listener => listener(type, detail)) };
}

test('Safari storage rejection gives recovery guidance before memory allocation and still offers updates', async () => {
  let allocations = 0, inspections = 0;
  const messages = [];
  const { element, context } = await launcher(undefined, {
    defaultRoom: '',
    gameStorage: { async getDirectory() {
      throw new DOMException('The operation failed for an unknown transient reason (e.g. out of memory).', 'UnknownError');
    } },
    Memory: class { constructor() { allocations++; } },
    mapsState: async () => { inspections++; return null; },
    fetch: async url => ({ json: async () => ({ version: url.includes('latest') ? 'new' : 'old' }) }),
    serviceWorker: { register: async () => {}, addEventListener() {},
      controller: { postMessage: message => messages.push(message) } },
  });
  assert.equal(allocations, 0, 'unavailable storage must not leave a 2.1 GB memory reservation');
  assert.equal(inspections, 0, 'stop before attempting to read the blocked map cache');
  assert.equal(context.Module, undefined);
  const checks = element('checks').children;
  assert.ok(checks.some(check => check.className === 'check bad' && /regular tab.*Private Browsing/.test(check.textContent)));
  assert.ok(!checks.some(check => /page could not start|Memory \(/.test(check.textContent)));
  assert.ok(!checks.some(check => check.className === 'check' && /storage/i.test(check.textContent)));
  assert.equal(element('update-notice').hidden, false, 'repairs remain reachable after a failed check');
  element('update-button').onclick();
  assert.deepEqual(messages, ['update']);
});

test('missing storage API fails cleanly without inspecting data or allocating memory', async () => {
  const { element } = await launcher(undefined, {
    defaultRoom: '', gameStorage: {},
    Memory: class { constructor() { assert.fail('unexpected memory allocation'); } },
    mapsState: async () => { assert.fail('unexpected cache inspection'); },
  });
  assert.ok(element('checks').children.some(check => check.className === 'check bad' && /storage is unavailable/.test(check.textContent)));
});

test('idle launcher allocates no game memory; Play allocates after storage and the game lock', async () => {
  const order = [];
  const { element } = await launcher(undefined, {
    defaultRoom: '',
    gameStorage: { async getDirectory() { order.push('storage'); return {}; } },
    Memory: class { constructor() { order.push('memory'); } },
    gameLocks: { request: async (_name, _options, callback) => { order.push('lock'); return callback({}); } },
    mapsState: async () => {
      order.push('maps');
      return fullMaps();
    },
  });
  assert.deepEqual(order, ['storage', 'maps']);
  assert.equal(element('step-play').hidden, false);
  assert.ok(element('checks').children.some(check => check.className === 'check' && /Game file storage/.test(check.textContent)));
  await element('play').onclick();
  assert.deepEqual(order, ['storage', 'maps', 'lock', 'memory']);
});

test('a second game tab never reserves memory, and allocation failure releases the lock for retry', async () => {
  let allocations = 0;
  const blocked = await launcher(undefined, {
    defaultRoom: '', gameLocks: { request: async (_name, _options, callback) => callback(null) },
    Memory: class { constructor() { allocations++; } },
  });
  await blocked.element('play').onclick();
  assert.equal(allocations, 0);
  assert.equal(blocked.context.Module, undefined);

  let locks = 0, released = 0;
  const retry = await launcher(undefined, {
    defaultRoom: '', gameLocks: { request: async (_name, _options, callback) => {
      locks++;
      await callback({});
      released++;
    } },
    Memory: class { constructor() { if (++allocations === 1) throw new RangeError('out of memory'); } },
  });
  await retry.element('play').onclick();
  await new Promise(setImmediate);
  assert.equal(retry.context.Module, undefined);
  assert.equal(released, 1, 'failed allocation must not retain the game lock');
  await retry.element('play').onclick();
  assert.ok(retry.context.Module, 'Play can recover from transient allocation failure');
  assert.equal(allocations, 2);
  assert.equal(locks, 2);
});

for (const autoLaunch of [false, true]) {
  test(`update notice survives room notifications and remains retryable ${autoLaunch ? 'after auto-launch' : 'in the launcher'}`, async () => {
    const messages = [], timers = new Map();
    let receiveWorkerMessage, nextTimer = 0;
    const { element, context, emitNetwork } = await launcher(undefined, {
      url: autoLaunch ? 'http://localhost:8780/' : 'http://localhost:8780/?menu=1',
      quickPlay: async ({ room }) => ({ role: 'host', room, hostAddress: ADDRESS }),
      setTimeout: callback => { timers.set(++nextTimer, callback); return nextTimer; },
      clearTimeout: id => timers.delete(id),
      fetch: async url => ({ json: async () => ({ version: url.includes('latest') ? 'new' : 'old' }) }),
      serviceWorker: {
        register: async () => {},
        controller: { postMessage: message => messages.push(message) },
        addEventListener: (_type, listener) => { receiveWorkerMessage = listener; },
      },
    });
    assert.equal(!!context.Module, autoLaunch);
    assert.equal(element('update-notice').hidden, false, 'new build has a persistent update notice');
    const runToastTimer = () => {
      for (const callback of timers.values()) callback();
      timers.clear();
    };
    emitNetwork('joined', { name: 'Player' });
    assert.equal(element('toast').textContent, 'Player joined the room.');
    runToastTimer();
    assert.equal(element('toast').hidden, true);
    assert.equal(element('update-notice').hidden, false, 'toast expiry cannot hide the update');
    element('update-button').onclick();
    assert.deepEqual(messages, ['update'], 'update action remains reachable after status messages');
    assert.equal(element('update-button').disabled, true);
    assert.match(element('update-message').textContent, /restart/);
    receiveWorkerMessage({ data: 'update-failed' });
    const failure = element('update-message').textContent;
    assert.match(failure, /could not be downloaded/);
    assert.equal(element('update-button').disabled, false);
    assert.equal(element('update-button').textContent, 'Retry update');
    emitNetwork('left', { name: 'Player' });
    runToastTimer();
    assert.equal(element('update-notice').hidden, false);
    assert.equal(element('update-message').textContent, failure, 'room status cannot overwrite the persistent update status');
    element('update-button').onclick();
    assert.deepEqual(messages, ['update', 'update']);
  });
}

test('an offline host after relay ready allows a fresh invite; in-game disconnect still requires reload', async () => {
  const { element, context } = await launcher();
  element('invite-input').value = TOKEN;
  const first = element('invite-connect').onclick();
  const failed = FakeSocket.latest;
  failed.json({ type: 'ready', identifier: '010203040506', address: ADDRESS });
  await first;
  assert.equal(element('invite-input').disabled, true);
  failed.json({ type: 'error', message: 'Native host did not connect' });
  assert.equal(element('invite-connect').disabled, false);
  assert.equal(element('invite-input').disabled, false);
  assert.equal(context.Module, undefined, 'offline host does not launch the engine');
  assert.equal(element('fatal').hidden, true);

  const fresh = 'abcdefabcdef' + 'd'.repeat(32);
  element('invite-input').value = fresh;
  const second = element('invite-connect').onclick();
  const live = FakeSocket.latest;
  assert.notEqual(live, failed);
  live.onopen();
  assert.equal(JSON.parse(live.sent[0]).invite, fresh);
  live.json({ type: 'ready', identifier: '112233445566', address: ADDRESS });
  await second;
  live.json({ type: 'peer', identifier: fresh.slice(0, 12), address: ADDRESS + 0x1000000, connected: true });
  assert.ok(context.Module, 'native host availability automatically launches the game');
  assert.ok(context.Module.arguments.includes('--HALO_QUICK_PLAY=join'));
  assert.ok(context.Module.arguments.includes('--HALO_QUICK_PLAY_TARGET=100.64.2.2'));
  live.json({ type: 'error', message: 'Native session ended' });
  assert.equal(element('fatal').hidden, false);
  assert.match(element('fatal-text').textContent, /Reload/);
  assert.equal(element('invite-connect').disabled, true);
  assert.equal(element('invite-input').disabled, true);
  assert.equal(element('play').disabled, true);
  await element('invite-connect').onclick();
  assert.equal(FakeSocket.latest, live, 'running game cannot change identity');
});

test('failure while installing relay transport cannot relock the pre-game invite form', async () => {
  let finishInstall;
  const { element, context } = await launcher(() => new Promise(resolve => { finishInstall = resolve; }));
  element('invite-input').value = TOKEN;
  const attempt = element('invite-connect').onclick();
  const socket = FakeSocket.latest;
  socket.json({ type: 'ready', identifier: '010203040506', address: ADDRESS });
  await new Promise(setImmediate);
  socket.json({ type: 'error', message: 'Native session ended' });
  finishInstall();
  await attempt;
  assert.equal(element('invite-connect').disabled, false);
  assert.equal(element('invite-input').disabled, false);
  assert.equal(context.Module, undefined);
});

test('normal startup joins the public default room and waits for a ready multiplayer role', async () => {
  const { joins, element, context, storage } = await launcher();
  assert.deepEqual(joins, ['FQLX01']);
  assert.equal(element('online-default-code').textContent, 'FQLX01');
  assert.equal(element('online-code').textContent, 'FQLX01');
  assert.equal(element('online-default').disabled, true);
  assert.equal(context.Module, undefined);
  assert.equal((await launcher(undefined, { url: 'http://localhost:8780/?menu=1' })).quickCalls.length, 0);
  assert.equal(context.location.search, '');
  assert.equal(storage.has('halo-web-room'), false, 'automatic fallback is not remembered as a private choice');
  const configured = await launcher(undefined, { defaultRoom: 'CUSTOM01' });
  assert.deepEqual(configured.joins, ['CUSTOM01']);
  assert.equal(configured.element('online-default-code').textContent, 'CUSTOM01');
  const disabled = await launcher(undefined, { defaultRoom: '' });
  assert.deepEqual(disabled.joins, []);
  assert.equal(disabled.element('online-default-room').hidden, true);
});

test('explicit room wins over remembered choice and leave flag; private choice wins over default', async () => {
  const storage = new Map([['halo-web-room', 'PRIVATE7']]);
  const remembered = await launcher(undefined, { storage });
  assert.deepEqual(remembered.joins, ['PRIVATE7']);
  assert.equal(remembered.element('online-default').disabled, false);
  storage.set('halo-web-room-left', '1');
  const linked = await launcher(undefined, { storage, url: 'http://localhost:8780/?room=FRIENDS9' });
  assert.deepEqual(linked.joins, ['FRIENDS9']);
  assert.equal(storage.get('halo-web-room'), 'FRIENDS9');
  assert.equal(storage.has('halo-web-room-left'), false);
});

test('native invite suppresses explicit, remembered, and default browser rooms', async () => {
  const storage = new Map([['halo-web-room', 'PRIVATE7']]);
  const native = await launcher(undefined, { storage, relayUrl: '',
    url: 'http://localhost:8780/?room=FRIENDS9#join=' + TOKEN });
  assert.deepEqual(native.joins, []);
  assert.equal(native.element('browser-rooms').hidden, true);
  assert.equal(storage.get('halo-web-room'), 'PRIVATE7');
  assert.equal(native.context.Module, undefined);
});

test('Leave survives reload from a room link and Join default room restores automatic joining', async () => {
  const first = await launcher(undefined, { url: 'http://localhost:8780/?room=PRIVATE7' });
  await first.element('online-leave').onclick();
  assert.equal(first.context.location.search, '');
  assert.equal(first.storage.get('halo-web-room-left'), '1');
  assert.equal(first.element('online-room').hidden, true);
  const reloaded = await launcher(undefined, { storage: first.storage, url: first.context.location.href });
  assert.deepEqual(reloaded.joins, []);
  assert.equal(reloaded.element('online-default').disabled, false);
  await reloaded.element('online-default').onclick();
  assert.deepEqual(reloaded.joins, ['FQLX01']);
  assert.equal(first.storage.has('halo-web-room-left'), false);
  assert.equal(first.storage.get('halo-web-room'), 'FQLX01');
  assert.equal(reloaded.context.location.search, '?room=FQLX01');
  const returned = await launcher(undefined, { storage: first.storage });
  assert.deepEqual(returned.joins, ['FQLX01']);
});

test('leaving while automatic room setup is pending remains outside the room', async () => {
  let finishJoin;
  const pending = await launcher(undefined, { beforeJoin: () => new Promise(resolve => { finishJoin = resolve; }) });
  const leaving = pending.element('online-leave').onclick();
  finishJoin();
  await leaving;
  assert.equal(pending.element('online-room').hidden, true);
  assert.equal(pending.storage.get('halo-web-room-left'), '1');
  assert.equal(pending.storage.has('halo-web-room'), false);
});

test('stable host selection loads multiplayer without an enable prompt', async () => {
  const launched = await launcher(undefined, { quickPlay: async ({ room }) => ({ role: 'host', room }) });
  assert.ok(launched.context.Module.arguments.includes('--HALO_QUICK_PLAY=host'));
  assert.equal(launched.context.Module.arguments.some(value => value.includes('NETWORK_TEST')), false);
  assert.equal(launched.element('quick-panel').hidden, false);
  assert.equal(launched.element('play').hidden, true);
  assert.equal(launched.element('interaction-prompt').hidden, true);
  let fullscreenRequests = 0;
  launched.context.document.documentElement.requestFullscreen = () => {
    fullscreenRequests++;
    return Promise.resolve();
  };
  launched.windowEvents.get('pointerdown').forEach(listener => listener({ target: launched.element('screen') }));
  assert.equal(fullscreenRequests, 1, 'a normal game gesture unlocks controls without a separate prompt');
  assert.equal(launched.element('interaction-prompt').hidden, true);
  launched.context.Module.haloMessage(6, JSON.stringify({ phase: 'playing', message: 'Playing Beaver Creek.' }));
  assert.equal(launched.element('quick-panel').hidden, true);
  assert.deepEqual(launched.phases, ['playing']);
});

test('new launcher markup keeps cached interaction handlers inert and compatible', () => {
  const html = fs.readFileSync(require.resolve('../../port/web/site/index.html'), 'utf8');
  // A hard reload can fetch the new document before a previously installed
  // service worker serves its cached script. Old startup binds both buttons.
  const ids = new Set(Array.from(html.matchAll(/\bid="([^"]+)"/g), match => match[1]));
  for (const id of ['interaction-enable', 'interaction-menu', 'room-toggle', 'room-close']) {
    assert.ok(ids.has(id), `cached script can bind ${id}`);
  }
  assert.match(html, /id="interaction-prompt"[^>]*aria-hidden="true"[^>]*style="display:none!important"/);
  assert.doesNotMatch(html, /Enable sound &amp; controls|Click or tap to enable sound/);
});

test('existing host selection joins its exact address without a menu click', async () => {
  const joined = await launcher(undefined, { url: 'http://localhost:8780/?room=FRIENDS9',
    quickPlay: async ({ room }) => ({ role: 'join', room, hostAddress: 0x0403020a }) });
  assert.ok(joined.context.Module.arguments.includes('--HALO_QUICK_PLAY=join'));
  assert.ok(joined.context.Module.arguments.includes('--HALO_QUICK_PLAY_TARGET=10.2.3.4'));
});

test('host failover migrates the existing engine with its authority epoch and holds the match', async () => {
  const page = await launcher(undefined, { url: 'http://localhost:8780/?room=FRIENDS9',
    quickPlay: async ({ room }) => ({ role: 'join', room, hostAddress: 0x0403020a }) });
  const module = page.context.Module, migrations = [], holds = [];
  module._web_quick_play_restart = () => assert.fail('host departure must never restart or reset the match');
  module._web_quick_play_migrate = (...args) => migrations.push(args);
  module._web_quick_play_hold = value => holds.push(value);
  const request = page.quickCalls[0];
  request.onStatus({ state: 'reconnecting', message: 'Waiting for the host connection.', hold: true });
  assert.deepEqual(holds, [1]);
  request.onStatus({ state: 'playing', message: 'Connection restored.', hold: false });
  assert.deepEqual(holds, [1, 0]);
  request.onStatus({ state: 'recovering', message: 'Choosing a replacement host.', hold: true });
  request.onFailover({ role: 'host', room: 'FRIENDS9', hostAddress: ADDRESS, epoch: 1 });
  assert.deepEqual(migrations, [[1, ADDRESS, 1]]);
  request.onFailover({ role: 'join', room: 'FRIENDS9', hostAddress: 0x0302010a, epoch: 2 });
  assert.deepEqual(migrations[1], [2, 0x0302010a, 2]);
  assert.equal(page.context.Module, module, 'the running WebAssembly module stays alive');
  assert.equal(page.context.location.search, '?room=FRIENDS9');
  assert.match(page.element('quick-game-status').textContent, /replacement host/);
  assert.doesNotMatch(page.element('quick-game-status').textContent, /restart|reset/i);
  page.element('main-menu').onclick();
  const holdCount = holds.length;
  request.onFailover({ role: 'host', room: 'FRIENDS9', hostAddress: ADDRESS, epoch: 3 });
  request.onStatus({ state: 'recovering', message: 'Late recovery.', hold: true });
  assert.equal(migrations.length, 2, 'Main menu cancels automatic recovery');
  assert.equal(holds.length, holdCount, 'late recovery cannot pause a canceled match');
});

test('one player reattaches to the current authority without restarting or migrating the room', async () => {
  const page = await launcher(undefined, { url: 'http://localhost:8780/?room=FRIENDS9',
    quickPlay: async ({ room }) => ({ role: 'join', room, hostAddress: 0x0403020a }) });
  const module = page.context.Module, repairs = [];
  module._web_quick_play_reconnect = (...args) => repairs.push(args);
  module._web_quick_play_migrate = () => assert.fail('a player repair cannot change authority');
  module._web_quick_play_restart = () => assert.fail('a player repair cannot reset the match');
  const request = page.quickCalls[0];
  request.onReconnect({ role: 'join', room: 'FRIENDS9', hostAddress: 0x0403020a, epoch: 0 });
  assert.deepEqual(repairs, [[0x0403020a, 0]]);
  assert.equal(page.context.Module, module);
  assert.match(page.element('quick-game-status').textContent, /current host/);
  page.element('main-menu').onclick();
  request.onReconnect({ role: 'join', room: 'FRIENDS9', hostAddress: 0x0403020a, epoch: 0 });
  assert.equal(repairs.length, 1, 'cancellation fences late client repairs');
});

test('engine checkpoint receipts update recovery eligibility without changing the playing screen', async () => {
  const page = await launcher(undefined, { quickPlay: async ({ room }) => ({ role: 'host', room, epoch: 0 }) });
  const module = page.context.Module;
  module.haloMessage(6, JSON.stringify({ phase: 'playing', message: 'Playing Beaver Creek.' }));
  module.haloMessage(6, JSON.stringify({ phase: 'checkpoint', epoch: 0, tick: 125, matchId: 500 }));
  assert.deepEqual(page.checkpoints.map(value => ({ ...value })), [
    { phase: 'checkpoint', epoch: 0, tick: 125, matchId: 500 },
  ]);
  assert.deepEqual(page.phases, ['playing']);
  assert.equal(page.element('quick-panel').hidden, true);
  assert.equal(page.element('quick-game-status').textContent, 'Playing Beaver Creek.');
});

test('a late entrant initializes its engine with the recovered host authority epoch', async () => {
  const page = await launcher(undefined, {
    quickPlay: async ({ room }) => ({ role: 'join', room, hostAddress: ADDRESS, epoch: 4 }),
  });
  const epochs = [], module = page.context.Module;
  module._web_quick_play_set_epoch = epoch => epochs.push(epoch);
  // Stop at the shared-state boundary; this fixture intentionally does not
  // allocate the production WebAssembly memory or start graphics/audio.
  const boundary = new Error('shared-state test boundary');
  module._web_shared_state = () => { throw boundary; };
  assert.throws(() => module.onRuntimeInitialized(), error => error === boundary);
  assert.deepEqual(epochs, [4], 'epoch zero would reject every migrated host packet and checkpoint');
});

test('a lost host report preserves room recovery instead of returning to manual play', async () => {
  const page = await launcher(undefined, { recovering: true,
    quickPlay: async ({ room }) => ({ role: 'join', room, hostAddress: ADDRESS }) });
  page.context.Module.haloMessage(6, JSON.stringify({ phase: 'disconnected', message: 'Host lost.' }));
  assert.match(page.element('quick-game-status').textContent, /preserving the match/);
  page.context.Module.haloMessage(6, JSON.stringify({ phase: 'menu', message: 'Old session closed.' }));
  assert.match(page.element('quick-game-status').textContent, /replacement host/);
  assert.equal(page.quickCalls[0].signal.aborted, false);
});

async function loadedRoomLauncher() {
  const fixture = await network();
  const host = await fixture.hostPresence(); host.channels[0].onopen();
  const page = await launcher(undefined, { url: 'http://localhost:8780/?room=TEST42',
    mapsState: async () => quickMaps(), net: fixture.net,
    quickPlay: request => fixture.net.quickPlay(request) });
  fixture.tick(1500); await new Promise(setImmediate);
  const module = page.context.Module, migrations = [];
  assert.ok(module, 'the initial elected match launched');
  module._web_quick_play_restart = () => assert.fail('disconnect must preserve the existing match');
  module._web_quick_play_migrate = (...args) => migrations.push(args);
  module._web_quick_play_hold = () => {};
  module.haloMessage(6, JSON.stringify({ phase: 'playing', message: 'Playing Beaver Creek.' }));
  module.haloMessage(6, JSON.stringify({ phase: 'checkpoint', epoch: 0, tick: 125, matchId: 500 }));
  return { fixture, host, page, module, migrations };
}

test('the actual launcher consumes native disconnect after signalling has cleared its election result', async () => {
  const { fixture, host, page, module, migrations } = await loadedRoomLauncher();
  try {
    const location = page.context.location.href;
    host.connectionState = 'failed'; host.onconnectionstatechange();
    fixture.tick(2000); fixture.tick(12000);
    assert.equal(fixture.latest().quick.role, 'candidate');
    module.haloMessage(6, JSON.stringify({ phase: 'disconnected', message: 'Host lost.' }));
    assert.equal(page.quickCalls[0].signal.aborted, false, 'native EOF cannot cancel an active preserved election');
    assert.equal(page.element('fatal').hidden, true, 'bootstrap maps cannot turn this receipt into a fatal reload');
    assert.equal(fixture.latest().quick.epoch, 1);
    fixture.tick(18000); fixture.tick(19500);
    assert.deepEqual(migrations, [[1, fixture.net.address, 1]], 'the same running engine receives the elected host');
    assert.equal(page.context.Module, module); assert.equal(page.context.location.href, location);
    assert.equal(fixture.latest().quick.matchId, 500);
  } finally { await fixture.close(); }
});

for (const role of ['host', 'join']) {
  test(`the actual launcher consumes a queued disconnect after replacement ${role} selection`, async () => {
    const { fixture, host, page, module, migrations } = await loadedRoomLauncher();
    try {
      const location = page.context.location.href;
      if (role === 'host') {
        host.connectionState = 'failed'; host.onconnectionstatechange();
        fixture.tick(2000); fixture.tick(12000); fixture.tick(18000); fixture.tick(19500);
      } else {
        await fixture.hostPresence(1); fixture.tick(2000); fixture.tick(3500);
      }
      assert.equal(migrations.length, 1); assert.equal(migrations[0][0], role === 'host' ? 1 : 2);
      module.haloMessage(6, JSON.stringify({ phase: 'disconnected', message: 'Old host disconnected.' }));
      module.haloMessage(6, JSON.stringify({ phase: 'disconnected', message: 'Queued old session closed.' }));
      fixture.tick(20000);
      assert.equal(page.quickCalls[0].signal.aborted, false);
      assert.equal(page.element('fatal').hidden, true);
      assert.equal(fixture.latest().quick.epoch, 1, 'queued old-session status cannot elect another authority');
      assert.equal(migrations.length, 1);
      module.haloMessage(6, JSON.stringify({ phase: 'playing', message: 'Preserved match resumed.' }));
      module.haloMessage(6, JSON.stringify({ phase: 'checkpoint', epoch: 1, tick: 150, matchId: 500 }));
      assert.equal(page.element('quick-panel').hidden, true);
      assert.equal(fixture.latest().quick.matchId, 500);
      assert.equal(fixture.latest().quick.checkpointTick, 150);
      assert.equal(page.context.Module, module); assert.equal(page.context.location.href, location);
    } finally { await fixture.close(); }
  });
}

test('the launcher retries a third-player disconnect immediately after replacement resume', async () => {
  const { fixture, page, module, migrations } = await loadedRoomLauncher();
  const repairs = [];
  module._web_quick_play_reconnect = (...args) => repairs.push(args);
  try {
    const location = page.context.location.href;
    await fixture.hostPresence(1); fixture.tick(2000); fixture.tick(3500);
    assert.deepEqual(migrations, [[2, 0x0201010a, 1]]);
    module.haloMessage(6, JSON.stringify({ phase: 'playing', message: 'Preserved match resumed.' }));
    // Deliver a fresh connection error before the page releases its old hold.
    module.haloMessage(6, JSON.stringify({ phase: 'disconnected', message: 'New connection failed.' }));
    assert.deepEqual(repairs, [[0x0201010a, 1]], 'the running engine must receive a same-host repair');
    assert.equal(page.quickCalls[0].signal.aborted, false);
    assert.equal(page.element('fatal').hidden, true);
    assert.equal(page.context.Module, module); assert.equal(page.context.location.href, location);
    assert.equal(fixture.latest().quick.matchId, 500);
    assert.equal(fixture.latest().quick.epoch, 1);
    assert.equal(migrations.length, 1, 'repair must preserve the existing replacement authority');
  } finally { await fixture.close(); }
});

test('a promoted host connection failure preserves the launcher and reattaches to another host', async () => {
  const { fixture, host, page, module, migrations } = await loadedRoomLauncher();
  try {
    const location = page.context.location.href;
    host.connectionState = 'failed'; host.onconnectionstatechange();
    fixture.tick(2000); fixture.tick(12000); fixture.tick(18000); fixture.tick(19500);
    module.haloMessage(6, JSON.stringify({ phase: 'playing', message: 'Preserved match resumed.' }));
    module.haloMessage(6, JSON.stringify({ phase: 'checkpoint', epoch: 1, tick: 150, matchId: 500 }));
    fixture.tick(20000);
    module.haloMessage(6, JSON.stringify({ phase: 'disconnected', message: 'Replacement host connection failed.' }));
    assert.equal(page.quickCalls[0].signal.aborted, false);
    assert.equal(page.element('fatal').hidden, true);
    assert.equal(page.context.location.href, location);
    const replacement = await fixture.hostPresence(2); replacement.channels[0].onopen();
    fixture.tick(21000); fixture.tick(22500);
    assert.deepEqual(migrations, [[1, fixture.net.address, 1], [2, 0x0201010a, 2]]);
    assert.equal(page.context.Module, module);
    assert.equal(fixture.latest().quick.matchId, 500);
  } finally { await fixture.close(); }
});

test('switching an active match restarts in the selected room and preserves cached game data', async () => {
  const page = await launcher(undefined, { url: 'http://localhost:8780/?room=FQLX01&batch_streams=0',
    quickPlay: async ({ room }) => ({ role: 'host', room }) });
  const firstModule = page.context.Module;
  page.element('online-input').value = ' friends-9 ';
  await page.element('online-enter').onclick();
  assert.equal(page.context.location.search, '?room=FRIENDS9&batch_streams=0');
  assert.equal(page.storage.get('halo-web-room'), 'FRIENDS9');
  assert.deepEqual(page.joins, ['FQLX01'], 'do not attach the running engine to a different room');
  assert.equal(page.context.Module, firstModule, 'the new document starts its own runtime');
  const reloaded = await launcher(undefined, { url: page.context.location.href, storage: page.storage,
    quickPlay: async ({ room }) => ({ role: 'join', room, hostAddress: 0x0403020a }) });
  assert.deepEqual(reloaded.joins, ['FRIENDS9']);
  assert.ok(reloaded.context.Module.arguments.includes('--HALO_QUICK_PLAY_TARGET=10.2.3.4'));
});

test('New room and Join default room restart an active game into the requested room', async () => {
  const page = await launcher(undefined, { url: 'http://localhost:8780/?room=FRIENDS9',
    quickPlay: async ({ room }) => ({ role: 'host', room }) });
  assert.equal(page.element('online-join').hidden, false, 'room controls stay visible');
  await page.element('online-create').onclick();
  assert.equal(page.context.location.search, '?room=PRIVATE7');
  assert.equal(page.storage.get('halo-web-room'), 'PRIVATE7');
  const next = await launcher(undefined, { url: page.context.location.href, storage: page.storage,
    quickPlay: async ({ room }) => ({ role: 'host', room }) });
  await next.element('online-default').onclick();
  assert.equal(next.context.location.search, '?room=FQLX01');
  assert.deepEqual(next.joins, ['PRIVATE7']);
});

test('room controls can be opened during a game without sending typing to Halo', async () => {
  const page = await launcher(undefined, {
    quickPlay: async ({ room }) => ({ role: 'host', room }) });
  assert.equal(page.element('room-toggle').hidden, false);
  page.element('room-toggle').onclick();
  assert.equal(page.element('room-close').hidden, false);
  assert.equal(page.element('room-toggle').hidden, true);
  assert.deepEqual(page.uiActive, [true]);
  page.element('room-close').onclick();
  assert.equal(page.element('room-close').hidden, true);
  assert.equal(page.element('room-toggle').hidden, false);
  assert.deepEqual(page.uiActive, [true, false]);
});

test('Leave closes an active session and remains out after restarting the launcher', async () => {
  const page = await launcher(undefined, { url: 'http://localhost:8780/?room=FRIENDS9',
    quickPlay: async ({ room }) => ({ role: 'host', room }) });
  await page.element('online-leave').onclick();
  assert.equal(page.context.location.search, '?menu=1');
  assert.equal(page.storage.get('halo-web-room-left'), '1');
  assert.equal(page.storage.has('halo-web-room'), false);
  const next = await launcher(undefined, { url: page.context.location.href, storage: page.storage });
  assert.deepEqual(next.joins, []);
  assert.equal(next.context.Module, undefined);
});

test('choosing a room from the main-menu launcher enables automatic multiplayer', async () => {
  const page = await launcher(undefined, { url: 'http://localhost:8780/?menu=1&room=FQLX01',
    quickPlay: async ({ room }) => ({ role: 'host', room }) });
  assert.equal(page.context.Module, undefined);
  page.element('online-input').value = 'FQLX01';
  await page.element('online-enter').onclick();
  await new Promise(setImmediate);
  assert.equal(page.context.location.search, '?room=FQLX01');
  assert.ok(page.context.Module.arguments.includes('--HALO_QUICK_PLAY=host'));
});

test('invalid room selection does not cancel the current match or overwrite its link', async () => {
  const page = await launcher(undefined, { url: 'http://localhost:8780/?room=FRIENDS9',
    quickPlay: async ({ room }) => ({ role: 'host', room }) });
  for (const value of ['abc', 'a'.repeat(17)]) {
    page.element('online-input').value = value;
    await page.element('online-enter').onclick();
    assert.equal(page.context.location.search, '?room=FRIENDS9');
    assert.deepEqual(page.joins, ['FRIENDS9']);
  }
});

test('Share link always names the joined room and waits for room switching to finish', async () => {
  const shared = [];
  let finishJoin;
  const page = await launcher(undefined, { url: 'http://localhost:8780/?menu=1&room=FQLX01&fps=1',
    share: async value => shared.push(value) });
  await page.element('online-share').onclick();
  assert.equal(shared[0].url, 'http://localhost:8780/?room=FQLX01');
  const delayed = await launcher(undefined, { url: 'http://localhost:8780/?menu=1&room=FRIENDS9',
    share: async value => shared.push(value),
    beforeJoin: () => new Promise(resolve => { finishJoin = resolve; }) });
  assert.equal(delayed.element('online-share').disabled, true);
  await delayed.element('online-share').onclick();
  assert.equal(shared.length, 1);
  finishJoin();
  await new Promise(setImmediate);
  await delayed.element('online-share').onclick();
  assert.equal(shared[1].url, 'http://localhost:8780/?room=FRIENDS9');
});

test('connection-panel Show log opens the same current runtime and saved diagnostic log as Settings', async () => {
  const page = await launcher(undefined, { quickPlay: async ({ room }) => ({ role: 'join', room, hostAddress: ADDRESS }) });
  let diskLog = 'native search attempt one';
  page.context.navigator.storage.getDirectory = async () => ({
    async getFileHandle(name) {
      assert.equal(name, 'debug.txt');
      return { getFile: async () => ({ text: async () => diskLog }) };
    },
  });
  const module = page.context.Module;
  module.print('runtime: searching for the elected host');
  module.haloMessage(6, JSON.stringify({ phase: 'searching', message: 'Finding game…' }));
  assert.equal(page.element('quick-panel').hidden, false);
  assert.equal(page.element('log-view').hidden, true, 'connecting does not open the log by itself');
  assert.equal(page.element('quick-log').onclick, page.element('show-log').onclick);
  await page.element('quick-log').onclick();
  assert.equal(page.element('log-view').hidden, false);
  assert.match(page.element('log-text').textContent, /runtime: searching for the elected host/);
  assert.match(page.element('log-text').textContent, /--- debug.txt ---\nnative search attempt one/);
  page.element('log-close').onclick();
  assert.equal(page.element('log-view').hidden, true);
  diskLog += '\nnative search timed out';
  module.printErr('runtime: no game response');
  module.haloMessage(6, JSON.stringify({ phase: 'error', message: 'No game response.' }));
  assert.equal(page.element('quick-panel').hidden, false);
  assert.equal(page.element('log-view').hidden, true, 'an error leaves the viewer closed until requested');
  await page.element('quick-log').onclick();
  assert.match(page.element('log-text').textContent, /runtime: no game response/);
  assert.match(page.element('log-text').textContent, /native search timed out/);
});

test('timeout spam cannot hide the original disconnect and reattachment events in Show log', async () => {
  const page = await launcher(undefined, { url: 'http://localhost:8780/?menu=1' });
  const text = '11:00:00 connection lost\n11:00:01 machine #2 adopted the live match, epoch #1\n' +
    '11:01:00 timeout in network_connection_idle\n'.repeat(20000);
  const file = new Blob([text]);
  page.context.navigator.storage.getDirectory = async () => ({
    getFileHandle: async () => ({ getFile: async () => file }),
  });
  await page.element('show-log').onclick();
  const shown = page.element('log-text').textContent;
  assert.match(shown, /11:00:00 connection lost/);
  assert.match(shown, /machine #2 adopted the live match, epoch #1/);
  assert.match(shown, /Connection idle timeouts: 20000/);
  assert.ok(shown.length < 200000, 'reading the full file does not create an unbounded viewer');
});

for (const dataRoot of ['/data', '/data/halo/data']) {
  test(`Show log reads the active ${dataRoot} game directory without confusing the OPFS mount`, async () => {
    const page = await launcher(undefined, { url: 'http://localhost:8780/?menu=1',
      mapsState: async () => fullMaps({ dataRoot }) });
    const reads = [];
    const directory = path => ({
      async getDirectoryHandle(name, options) {
        assert.equal(options?.create, undefined, 'log lookup never creates directories');
        return directory(path + name + '/');
      },
      async getFileHandle(name) {
        reads.push(path + name);
        return { getFile: async () => ({ text: async () => 'log from ' + path + name }) };
      },
    });
    page.context.navigator.storage.getDirectory = async () => directory('');
    await page.element('show-log').onclick();
    const expected = dataRoot === '/data' ? 'debug.txt' : 'halo/data/debug.txt';
    assert.deepEqual(reads, [expected]);
    assert.ok(page.element('log-text').textContent.includes('log from ' + expected));
  });
}

test('an unavailable legacy log does not display an unrelated stale root log', async () => {
  const page = await launcher(undefined, { url: 'http://localhost:8780/?menu=1',
    mapsState: async () => fullMaps({ dataRoot: '/data/halo/data' }) });
  let rootReads = 0;
  page.context.navigator.storage.getDirectory = async () => ({
    getDirectoryHandle: async () => { throw new Error('Log directory unavailable'); },
    async getFileHandle() {
      rootReads++;
      return { getFile: async () => ({ text: async () => 'stale root log' }) };
    },
  });
  await page.element('show-log').onclick();
  assert.equal(rootReads, 0);
  assert.equal(page.element('log-text').textContent.includes('stale root log'), false);
  assert.equal(page.element('log-view').hidden, false, 'page diagnostics remain readable');
});

test('browser room waits for a user disc import, then launches automatically', async () => {
  let imported = false;
  const page = await launcher(undefined, {
    mapsState: async ({ required }) => imported && required.every(name => EXPECTED_MAPS.includes(name)) ? fullMaps() : null,
    onImport: () => { imported = true; },
    quickPlay: async ({ room }) => ({ role: 'host', room }),
  });
  assert.deepEqual(page.inspections[0], QUICK_MAPS);
  assert.equal(page.element('step-data').hidden, false);
  assert.equal(page.quickCalls.length, 0);
  assert.equal(page.context.Module, undefined);
  await page.element('iso-file').onchange({ target: { files: [{ name: 'my-disc.iso' }], value: '' } });
  await new Promise(setImmediate);
  assert.equal(page.imports.length, 1);
  assert.equal(page.quickCalls.length, 1);
  assert.ok(page.context.Module.arguments.includes('--HALO_QUICK_PLAY=host'));
});

test('map requirements follow browser room, manual menu, and native invite scope without downloading', async () => {
  for (const [url, relayUrl, expected] of [
    ['http://localhost:8780/?room=FQLX01', undefined, QUICK_MAPS],
    ['http://localhost:8780/?room=FQLX01&menu=1', undefined, EXPECTED_MAPS],
    ['http://localhost:8780/?room=FQLX01#join=' + TOKEN, '', EXPECTED_MAPS],
  ]) {
    const page = await launcher(undefined, { url, relayUrl, mapsState: async () => null });
    assert.deepEqual(page.inspections[0], expected, url);
    assert.equal(page.element('step-data').hidden, false);
    assert.equal(page.imports.length, 0);
    assert.equal(page.context.Module, undefined);
    assert.equal(page.quickCalls.length, 0);
  }
});

test('a cached bootstrap pair opens a room without importing unrelated maps', async () => {
  const page = await launcher(undefined, {
    url: 'http://localhost:8780/?room=FQLX01', mapsState: async () => quickMaps(),
    quickPlay: async ({ room }) => ({ role: 'host', room }),
  });
  assert.deepEqual(page.inspections[0], QUICK_MAPS);
  assert.equal(page.imports.length, 0);
  assert.ok(page.context.Module.arguments.includes('--HALO_QUICK_PLAY=host'));
  assert.match(page.element('data-summary').textContent, /2 maps/);
});

test('a native invite waits for full user-imported maps before opening the relay', async () => {
  let imported = false;
  const previousSocket = FakeSocket.latest;
  const page = await launcher(undefined, {
    url: 'http://localhost:8780/?room=FQLX01',
    mapsState: async () => imported ? fullMaps() : quickMaps(),
    onImport: () => { imported = true; },
  });
  page.element('invite-input').value = TOKEN;
  page.element('relay-access').value = 'test-private-access';
  await page.element('invite-connect').onclick();
  assert.equal(page.quickCalls[0].signal.aborted, true);
  assert.equal(page.element('step-data').hidden, false);
  assert.match(page.element('invite-status').textContent, /Import your disc image/);
  assert.equal(FakeSocket.latest, previousSocket);
  assert.equal(page.context.Module, undefined);
  await page.element('iso-file').onchange({ target: { files: [{ name: 'my-disc.iso' }], value: '' } });
  assert.equal(page.element('relay-access').value, 'test-private-access');
  assert.equal(page.context.location.href.includes('test-private-access'), false);
  const socket = FakeSocket.latest;
  assert.notEqual(socket, previousSocket);
  socket.onopen();
  assert.equal(JSON.parse(socket.sent[0]).invite, TOKEN);
  assert.equal(JSON.parse(socket.sent[0]).accessToken, 'test-private-access');
  socket.json({ type: 'ready', identifier: '010203040506', address: ADDRESS });
  await new Promise(setImmediate);
  socket.json({ type: 'peer', identifier: TOKEN.slice(0, 12), address: ADDRESS + 0x1000000, connected: true });
  assert.ok(page.context.Module.arguments.includes('--HALO_QUICK_PLAY=join'));
});

test('Main menu from a bootstrap cache reloads and waits for the remaining maps from a disc', async () => {
  let choose;
  const page = await launcher(undefined, {
    url: 'http://localhost:8780/?room=FQLX01&fps=1', mapsState: async () => quickMaps(),
    quickPlay: ({ room }) => new Promise(resolve => { choose = () => resolve({ role: 'host', room }); }),
  });
  page.element('main-menu').onclick();
  assert.equal(page.context.location.search, '?room=FQLX01&fps=1&menu=1');
  assert.equal(page.quickCalls[0].signal.aborted, true);
  choose();
  await new Promise(setImmediate);
  assert.equal(page.context.Module, undefined);

  let imported = false;
  const menu = await launcher(undefined, {
    url: page.context.location.href, mapsState: async () => imported ? fullMaps() : null,
    onImport: () => { imported = true; },
  });
  assert.deepEqual(menu.inspections[0], EXPECTED_MAPS);
  assert.equal(menu.element('step-data').hidden, false);
  assert.equal(menu.element('play').disabled, true);
  await menu.element('iso-file').onchange({ target: { files: [{ name: 'my-disc.iso' }], value: '' } });
  assert.equal(menu.context.Module, undefined, 'manual mode waits for Play');
  assert.equal(menu.element('play').disabled, false);
  await menu.element('play').onclick();
  assert.ok(menu.context.Module);
  assert.equal(menu.context.Module.arguments.some(value => value.startsWith('--HALO_QUICK_PLAY')), false);
});

test('a bootstrap-only match ending reloads to the full menu without using the native cancel export', async () => {
  const page = await launcher(undefined, {
    url: 'http://localhost:8780/?room=FQLX01', mapsState: async () => quickMaps(),
    quickPlay: async ({ room }) => ({ role: 'host', room }),
  });
  let cancellations = 0;
  page.context.Module._web_quick_play_cancel = () => cancellations++;
  page.context.Module.haloMessage(6, JSON.stringify({ phase: 'menu', message: 'The match ended.' }));
  assert.equal(page.context.location.searchParams.get('menu'), '1');
  assert.equal(page.quickCalls[0].signal.aborted, true);
  assert.equal(cancellations, 0);
});

test('a bootstrap-only match error prompts full map import for the main menu', async () => {
  const page = await launcher(undefined, {
    url: 'http://localhost:8780/?room=FQLX01', mapsState: async () => quickMaps(),
    quickPlay: async ({ room }) => ({ role: 'host', room }),
  });
  page.context.Module.haloMessage(6, JSON.stringify({ phase: 'error', message: 'Match failed.' }));
  assert.equal(page.element('fatal').hidden, false);
  assert.match(page.element('fatal-text').textContent, /Main menu.*import your disc image/);
  page.element('fatal-menu').onclick();
  assert.equal(page.context.location.searchParams.get('menu'), '1');
});

test('choosing a room with a cached bootstrap pair rechecks the room map scope', async () => {
  const page = await launcher(undefined, {
    url: 'http://localhost:8780/?menu=1&fps=1',
    mapsState: async ({ required }) => required.every(name => QUICK_MAPS.includes(name)) ? quickMaps() : null,
    quickPlay: async ({ room }) => ({ role: 'host', room }),
  });
  assert.equal(page.element('step-data').hidden, false);
  page.element('online-input').value = 'FRIENDS9';
  await page.element('online-enter').onclick();
  assert.equal(page.context.location.search, '?fps=1&room=FRIENDS9');
  assert.deepEqual(page.inspections.at(-1), QUICK_MAPS);
  assert.ok(page.context.Module.arguments.includes('--HALO_QUICK_PLAY=host'));
});

test('Main menu supersedes a delayed game lock without starting an outgoing runtime or retaining the lock', async () => {
  for (const maps of [quickMaps(), fullMaps()]) {
    let grantLock, released = 0;
    const page = await launcher(undefined, {
      url: 'http://localhost:8780/?room=FQLX01', mapsState: async () => maps,
      quickPlay: async ({ room }) => ({ role: 'host', room }),
      gameLocks: { request: (_name, _options, callback) => new Promise(resolve => {
        grantLock = () => Promise.resolve(callback({})).then(() => { released++; resolve(); });
      }) },
    });
    assert.equal(page.context.Module, undefined, 'runtime waits for the OPFS game lock');
    page.element('main-menu').onclick();
    assert.equal(page.context.location.searchParams.get('menu'), '1');
    assert.equal(page.quickCalls[0].signal.aborted, true);
    grantLock();
    await new Promise(setImmediate);
    assert.equal(page.context.Module, undefined, 'lock arrival cannot launch the outgoing document');
    assert.equal(released, 1, 'the new document can acquire the game lock');
  }
});

test('Main menu cancels pending election and ignores its stale host result', async () => {
  let choose;
  const waiting = await launcher(undefined, { quickPlay: ({ room }) =>
    new Promise(resolve => { choose = () => resolve({ role: 'host', room }); }) });
  waiting.element('main-menu').onclick();
  assert.equal(waiting.quickCalls[0].signal.aborted, true);
  assert.ok(waiting.context.Module);
  assert.equal(waiting.context.Module.arguments.some(value => value.startsWith('--HALO_QUICK_PLAY')), false);
  choose();
  await new Promise(setImmediate);
  assert.equal(waiting.context.Module.arguments.some(value => value.startsWith('--HALO_QUICK_PLAY')), false);
});

test('Main menu cancels a running quick session through the native export', async () => {
  const active = await launcher(undefined, { quickPlay: async ({ room }) => ({ role: 'host', room }) });
  let canceled = 0;
  active.context.Module._web_quick_play_cancel = () => canceled++;
  active.element('quick-menu').onclick();
  assert.equal(canceled, 1);
  assert.equal(active.quickCalls[0].signal.aborted, true);
  assert.equal(active.element('quick-panel').hidden, true);
});

test('room failure requires an explicit retry and does not repeatedly launch', async () => {
  let attempts = 0;
  const failed = await launcher(undefined, { quickPlay: async ({ room }) => {
    if (++attempts === 1) throw new Error('No host can connect.');
    return { role: 'host', room };
  } });
  assert.equal(attempts, 1);
  assert.equal(failed.context.Module, undefined);
  assert.equal(failed.element('quick-retry').hidden, false);
  assert.match(failed.element('quick-status').textContent, /No host can connect/);
  await new Promise(setImmediate);
  assert.equal(attempts, 1);
  await failed.element('quick-retry').onclick();
  assert.equal(attempts, 2);
  assert.ok(failed.context.Module.arguments.includes('--HALO_QUICK_PLAY=host'));
  failed.context.Module.haloMessage(6, JSON.stringify({ phase: 'error', message: 'Match failed.' }));
  assert.equal(failed.element('quick-panel').dataset.state, 'error');
  assert.equal(failed.quickCalls[1].signal.aborted, true);
  assert.equal(attempts, 2);
});

test('Main menu supersedes a pending native handshake without installing it or reopening errors', async () => {
  let installs = 0;
  const manual = await launcher(async () => { installs++; });
  manual.element('invite-input').value = TOKEN;
  const pending = manual.element('invite-connect').onclick();
  const socket = FakeSocket.latest;
  manual.element('main-menu').onclick();
  assert.ok(manual.context.Module);
  assert.equal(manual.context.Module.arguments.some(value => value.startsWith('--HALO_QUICK_PLAY')), false);
  socket.json({ type: 'ready', identifier: '010203040506', address: ADDRESS });
  await pending;
  assert.equal(installs, 0);
  assert.equal(socket.readyState, 3);
  assert.equal(manual.element('quick-panel').hidden, true);
  assert.equal(manual.element('quick-status').textContent, 'Main menu selected.');
});

test('Main menu during native transport installation reloads manual mode before engine launch', async () => {
  let finishInstall;
  const manual = await launcher(() => new Promise(resolve => { finishInstall = resolve; }));
  manual.element('invite-input').value = TOKEN;
  const pending = manual.element('invite-connect').onclick();
  const socket = FakeSocket.latest;
  socket.json({ type: 'ready', identifier: '010203040506', address: ADDRESS });
  await new Promise(setImmediate);
  manual.element('main-menu').onclick();
  assert.equal(manual.context.location.searchParams.get('menu'), '1');
  assert.equal(manual.context.Module, undefined, 'old page cannot launch with a partially installed transport');
  finishInstall();
  await pending;
  assert.equal(socket.readyState, 3);
  assert.equal(manual.element('quick-panel').hidden, true);
  assert.equal(manual.element('quick-status').textContent, 'Main menu selected.');
});
