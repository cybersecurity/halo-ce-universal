const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const EXPECTED_MAPS = [
  'ui', 'a10', 'a30', 'a50', 'b30', 'b40', 'c10', 'c20', 'c40', 'd20', 'd40',
  'beavercreek', 'bloodgulch', 'boardingaction', 'carousel', 'chillout', 'damnation',
  'hangemhigh', 'longest', 'prisoner', 'putput', 'ratrace', 'sidewinder', 'wizard',
].map(name => name + '.map');

// Run the real launcher and its public Play/Module/visibility callbacks.
// As in native-invite.test.cjs, browser services are stubbed; only 64 KB of
// shared memory is needed to exercise the page's runtime handshake.
async function launch(query = '', viewport = {}, initiallyHidden = false) {
  const elements = new Map(), listeners = new Map(), timers = [], rafs = [];
  let now = 0, delivered = 0, attached, hiddenAtAttach;
  const bitmapContext = { transferFromImageBitmap() { delivered++; } };
  function element(id) {
    if (!elements.has(id)) elements.set(id, {
      value: '', hidden: true, disabled: false, dataset: {}, style: {}, children: [],
      appendChild(child) { this.children.push(child); },
      classList: { add() {}, remove() {} }, getContext: () => bitmapContext,
    });
    return elements.get(id);
  }
  const context = {
    console: { log() {} }, URL, URLSearchParams, SharedArrayBuffer,
    setTimeout() {}, clearTimeout() {},
    setInterval(callback, milliseconds) { timers.push({ callback, milliseconds }); },
    requestAnimationFrame(callback) { rafs.push(callback); },
    performance: { now: () => now },
    navigator: { userAgent: 'Test', platform: 'Test', storage: { getDirectory() {} } },
    location: new URL('http://localhost:8780/' + query),
    document: {
      hidden: initiallyHidden, getElementById: element, createElement: () => element(Symbol()),
      body: element('body'), documentElement: {},
      addEventListener(name, callback) {
        if (!listeners.has(name)) listeners.set(name, []);
        listeners.get(name).push(callback);
      },
    },
    localStorage: { getItem: () => null, setItem() {}, removeItem() {} },
    matchMedia: () => ({ matches: false }), addEventListener() {},
    screen: {}, history: { pushState() {} }, crossOriginIsolated: true,
    devicePixelRatio: 2, innerWidth: 1920, innerHeight: 1080, ...viewport,
    OffscreenCanvas: class { getContext() { return {}; } },
    WebAssembly: { Memory: class { buffer = new SharedArrayBuffer(65536); } },
    fetch: async () => { throw new Error('Offline'); },
    HALO_BROWSER_CONFIG: { relayUrl: '', defaultRoom: '' },
    HaloNet: { on() {}, addressText: () => '100.64.2.1',
      status: () => ({ room: null }), attach() {} },
    HaloInput: { attach(value) {
      attached = value;
      hiddenAtAttach = new Int32Array(value.memory.buffer)[(value.base + value.offsets.pageHidden) >> 2];
    }, setLookSensitivity() {}, pollGamepads() {} },
    HaloCache: { expected: EXPECTED_MAPS.slice(), mapsState: async () => ({ files: EXPECTED_MAPS.slice(), bytes: 1_856_530_432,
      dataRoot: '/data', saveRoot: '/data/save' }) },
  };
  context.window = context;
  vm.runInNewContext(fs.readFileSync(require.resolve('../../port/web/site/app.js'), 'utf8'), context);
  await new Promise(setImmediate);
  assert.equal(element('step-play').hidden, false, 'cached data reached Play');
  await element('play').onclick();
  const module = context.Module;
  assert.ok(module, 'Play created the runtime callbacks');
  const offsets = new Int32Array(module.wasmMemory.buffer, 128, 36);
  for (let index = 0; index < offsets.length; index++) offsets[index] = index * 4;
  module._web_shared_state = () => 1024;
  module._web_shared_offsets = () => 128;
  module.onRuntimeInitialized();
  return {
    context, timers, hiddenAtAttach,
    resolution(height) { element('opt-resolution').onchange({ target: { value: String(height) } }); },
    output: () => element('body').children.find(child => child.id === 'performance-stats'),
    samples() { return JSON.parse(this.output().dataset.samples); },
    tick(milliseconds = 1000) {
      now += milliseconds;
      for (const timer of timers) timer.callback();
    },
    raf(count = 1) { for (let frame = 0; frame < count; frame++) rafs.shift()(now); },
    present(count, width = 1280, height = 720) {
      for (let frame = 0; frame < count; frame++) module.haloPresent({ width, height });
    },
    visibility(hidden) {
      context.document.hidden = hidden;
      for (const callback of listeners.get('visibilitychange') || []) callback();
    },
    delivered: () => delivered,
    pageHidden: () => new Int32Array(attached.memory.buffer)[(attached.base + attached.offsets.pageHidden) >> 2],
    displaySize() {
      const words = new Int32Array(attached.memory.buffer);
      return ['displayWidth', 'displayHeight'].map(name => words[(attached.base + attached.offsets[name]) >> 2]);
    },
  };
}

test('runtime initializes current visibility before attaching game input, including an already hidden tab', async () => {
  for (const hidden of [true, false]) {
    const page = await launch('', {}, hidden);
    assert.equal(page.hiddenAtAttach, Number(hidden), 'initial visibility is published before runtime consumers attach');
    assert.equal(page.pageHidden(), Number(hidden), 'startup does not need a visibilitychange event');
    page.visibility(!hidden);
    assert.equal(page.pageHidden(), Number(!hidden), 'later visibility changes still reach the engine');
  }
});

test('FPS diagnostics are opt-in and count bitmap delivery rather than animation callbacks', async () => {
  for (const query of ['', '?fps=0']) {
    const page = await launch(query);
    page.raf(120);
    page.present(30);
    assert.equal(page.output(), undefined);
    assert.equal(page.timers.length, 0);
    assert.equal(page.delivered(), 30);
  }
  const page = await launch('?fps=1');
  assert.deepEqual(page.timers.map(timer => timer.milliseconds), [1000]);
  page.raf(120);
  page.tick();
  assert.deepEqual(page.samples(), [], 'animation callbacks alone are not game frames');
  page.present(30);
  page.raf(120);
  page.tick();
  assert.deepEqual(page.samples(), [{ fps: 30, frames: 30, ms: 2000, windowMs: 1000, width: 1280, height: 720 }]);
  page.raf(120);
  page.tick();
  assert.equal(page.samples().at(-1).fps, 0, 'a stalled game reports zero despite continuing RAF');
});

test('batch stream opt-out reaches the worker runtime arguments only when requested', async () => {
  const argument = '--HALO_WEB_BATCH_STREAMS=0';
  for (const query of ['', '?batch_streams=1', '?fps=1']) {
    const page = await launch(query);
    assert.equal(page.context.Module.arguments.includes(argument), false, query || 'default');
  }
  const disabled = await launch('?fps=1&batch_streams=0');
  assert.equal(disabled.context.Module.arguments.filter(value => value === argument).length, 1);
});

test('reference geometry path is opt-in for controlled comparisons', async () => {
  for (const query of ['', '?geometry_cache=0', '?geometry_cache=1']) {
    const page = await launch(query);
    assert.equal(page.context.Module.arguments.includes('--HALO_WEB_GEOMETRY_CACHE=1'), query.endsWith('=1'));
  }
});

test('Chrome on Mac negotiates pixel frames; other desktops default to 720p', async () => {
  for (const [userAgent, enabled] of [
    ['Mozilla/5.0 (Macintosh) Chrome/153.0.0.0 Safari/537.36', true],
    ['Mozilla/5.0 (Macintosh) Chrome/153.0.0.0 Edg/153.0.0.0', true],
    ['Mozilla/5.0 (Macintosh) Version/26.4 Safari/605.1.15', false],
    ['Mozilla/5.0 (Windows NT 10.0) Chrome/153.0.0.0', false],
  ]) {
    const page = await launch('', { navigator: { userAgent, platform: 'Test', storage: { getDirectory() {} } } });
    assert.equal(page.context.Module.arguments.includes('--HALO_WEB_PIXEL_FRAMES=1'), enabled);
    assert.equal(page.displaySize()[1], enabled ? 480 : 720);
  }
  const forced = await launch('?frame_transport=rgba');
  assert.equal(forced.context.Module.arguments.includes('--HALO_WEB_PIXEL_FRAMES=1'), true);
  const bitmap = await launch('?frame_transport=bitmap', {
    navigator: { userAgent: 'Macintosh Chrome/153.0.0.0', platform: 'Test', storage: { getDirectory() {} } },
  });
  assert.equal(bitmap.context.Module.arguments.includes('--HALO_WEB_PIXEL_FRAMES=1'), false);
});

test('hidden frames and elapsed hidden time are excluded after a visibility reset', async () => {
  const page = await launch('?fps=1');
  page.present(30);
  page.tick();
  page.visibility(true);
  page.present(80);
  page.tick(60000);
  assert.equal(page.samples().length, 1, 'hidden timer does not produce a visible sample');
  page.visibility(false);
  page.present(60, 1920, 1080);
  page.tick();
  assert.deepEqual(page.samples().at(-1), {
    fps: 60, frames: 90, ms: 62000, windowMs: 1000, width: 1920, height: 1080,
  });
});

test('a delayed visible timer retains the real elapsed window and bounded recent history', async () => {
  const page = await launch('?fps=1');
  page.present(30);
  page.tick(5000);
  assert.deepEqual(page.samples()[0], {
    fps: 6, frames: 30, ms: 5000, windowMs: 5000, width: 1280, height: 720,
  });
  for (let index = 0; index < 125; index++) {
    page.present(1);
    page.tick();
  }
  const samples = page.samples();
  assert.equal(samples.length, 120);
  assert.equal(samples[0].frames, 36, 'old samples are removed');
  assert.equal(samples.at(-1).frames, 155);
  assert.equal(samples.at(-1).fps, 1);
});

test('presentation height override is bounded and preserves aspect above the 720p desktop default', async () => {
  for (const [query, expected] of [
    ['', [1280, 720]], ['?render_height=480', [852, 480]],
    ['?render_height=720', [1280, 720]], ['?render_height=1440', [2560, 1440]],
    ['?render_height=9999', [2560, 1440]], ['?render_height=1', [852, 480]],
    ['?render_height=0', [1280, 720]], ['?render_height=invalid', [1280, 720]],
    ['?render_height=Infinity', [1280, 720]],
  ]) {
    const page = await launch(query);
    assert.deepEqual(page.displaySize(), expected, query || 'default');
  }
  const portrait = await launch('?render_height=720', { innerWidth: 1080, innerHeight: 1920 });
  assert.deepEqual(portrait.displaySize(), [1280, 720]);
  const small = await launch('?render_height=720', { devicePixelRatio: 1, innerWidth: 640, innerHeight: 360 });
  assert.deepEqual(small.displaySize(), [640, 360], 'a maximum does not upscale small viewports');
});

test('resolution settings update display size, reject invalid values, and retain diagnostic overrides', async () => {
  const page = await launch();
  for (const [height, expected] of [[480, [852, 480]], [1080, [1920, 1080]], [1440, [2560, 1440]], [720, [1280, 720]]]) {
    page.resolution(height);
    assert.deepEqual(page.displaySize(), expected);
  }
  page.resolution(9999);
  assert.deepEqual(page.displaySize(), [1280, 720]);
  const override = await launch('?render_height=480');
  override.resolution(1440);
  assert.deepEqual(override.displaySize(), [852, 480]);
});

test('iPhone and desktop-mode iPad avoid Retina-sized bitmap copies and retain overrides', async () => {
  for (const navigator of [
    { userAgent: 'Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X)', platform: 'iPhone' },
    { userAgent: 'Mozilla/5.0 (Macintosh; Intel Mac OS X)', platform: 'MacIntel', maxTouchPoints: 5 },
  ]) {
    navigator.storage = { getDirectory() {} };
    const viewport = { navigator, innerWidth: 402, innerHeight: 874, devicePixelRatio: 3 };
    const page = await launch('', viewport);
    assert.deepEqual(page.displaySize(), [1044, 480]);
    const override = await launch('?render_height=1440', viewport);
    assert.deepEqual(override.displaySize(), [1748, 804]);
    const landscape = await launch('', { ...viewport, innerWidth: 874, innerHeight: 402 });
    assert.deepEqual(landscape.displaySize(), page.displaySize());
  }
});
