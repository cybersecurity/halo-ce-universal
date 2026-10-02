'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

function runtime(batching, options = {}) {
  const events = [], messages = [], bitmaps = [];
  const gl = { flush() { events.push('submit'); } };
  let library;
  const context = vm.createContext({
    addToLibrary(value) { library = value; },
    SharedArrayBuffer,
    OffscreenCanvas: class {
      getContext() { return gl; }
      transferToImageBitmap() {
        events.push('bitmap');
        if (options.failTransfer) throw new Error('bitmap allocation failed');
        const size = options.emptyBitmap ? [0, 0] : (options.bitmapSizes?.[bitmaps.length] || [640, 480]);
        const bitmap = { width: size[0], height: size[1], closed: false,
          close() { this.closed = true; } };
        bitmaps.push(bitmap);
        return bitmap;
      }
    },
    GL: { registerContext() { return 1; }, makeContextCurrent() {} },
    HaloStreamBatch: { install(gl) {
      gl.flush = () => events.push('record-flush');
      return { flush() { events.push('replay'); } };
    } },
    ENVIRONMENT_IS_PTHREAD: true,
    postMessage(message, transfer) {
      events.push('post');
      if (options.failPost) throw new Error('post failed');
      if (message.handler === 'haloPresent') assert.equal(transfer[0], message.args[0], 'the bitmap is transferred');
      messages.push(message);
    },
  });
  vm.runInContext(fs.readFileSync(path.join(__dirname, '../../port/web/src/web_library.js'), 'utf8'), context);
  context.webHalo = library.$webHalo;
  library.web_js_gl_create(640, 480, batching, options.presentAck !== false);
  return { library, events, messages, bitmaps,
    pending: () => library.$webHalo.pendingFrames && Atomics.load(library.$webHalo.pendingFrames, 0) };
}

function page(options = {}) {
  const canvas = { width: 0, height: 0 };
  let presented = 0, consumed = 0;
  const globals = {
    canvas, document: { hidden: Boolean(options.hidden) },
    pixelFrames: false,
    context: { transferFromImageBitmap() {
      if (options.failConsume) throw new Error('bitmap renderer failed');
      consumed++;
    } },
    countPresent() { presented++; },
  };
  // Exercise the real launcher callback in isolation from downloads and
  // browser startup; this includes its bitmap ownership and ack handling.
  const source = fs.readFileSync(path.join(__dirname, '../../port/web/site/app.js'), 'utf8');
  const callback = source.slice(source.indexOf('haloPresent:') + 'haloPresent:'.length,
    source.indexOf('\n      haloMessage:')).trim().replace(/,$/, '');
  const present = vm.runInNewContext('(' + callback + ')', globals);
  return { canvas, present, presented: () => presented, consumed: () => consumed };
}

test('hidden batched frames submit GPU commands after replay without a bitmap', () => {
  const { library, events } = runtime(true);
  library.web_js_gl_flush();
  library.web_js_gl_flush();
  assert.deepEqual(events, ['replay', 'submit', 'replay', 'submit']);
});

test('hidden unbatched frames also submit GPU commands', () => {
  const { library, events } = runtime(false);
  library.web_js_gl_flush();
  assert.deepEqual(events, ['submit']);
});

test('visible frames replay before transferring and posting their bitmap', () => {
  const { library, events } = runtime(true);
  library.web_js_gl_present();
  assert.deepEqual(events, ['replay', 'bitmap', 'post']);
});

test('empty GPU surface frames close before posting and the next valid frame can recover', () => {
  for (const batching of [false, true]) {
    for (const presentAck of [false, true]) {
      const options = { emptyBitmap: true, presentAck };
      const worker = runtime(batching, options);
      worker.library.web_js_gl_present();
      assert.equal(worker.bitmaps.length, 1);
      assert.equal(worker.bitmaps[0].closed, true, 'the unusable bitmap is released');
      assert.equal(worker.messages.filter(message => message.handler === 'haloPresent').length, 0, 'the native serializer never receives an empty surface');
      assert.equal(worker.pending(), presentAck ? 0 : null, 'no queue slot is occupied');
      assert.deepEqual(worker.events, batching ? ['replay', 'bitmap', 'submit', 'post'] : ['bitmap', 'submit', 'post']);

      options.emptyBitmap = false;
      worker.library.web_js_gl_present();
      assert.equal(worker.messages.filter(message => message.handler === 'haloPresent').length, 1, 'an empty frame does not stop future presentation');
      assert.equal(worker.bitmaps[1].closed, false);
      assert.equal(worker.pending(), presentAck ? 1 : null);
      const consumer = page();
      consumer.present(...worker.messages.find(message => message.handler === 'haloPresent').args);
      assert.equal(worker.bitmaps[1].closed, true);
      assert.equal(worker.pending(), presentAck ? 0 : null);
      assert.equal(consumer.presented(), 1);
    }
  }
});

test('a blocked page retains at most two bitmaps while rendering keeps submitting', () => {
  for (const batching of [false, true]) {
    const worker = runtime(batching);
    for (let frame = 0; frame < 12; frame++) worker.library.web_js_gl_present();
    assert.equal(worker.pending(), 2);
    assert.equal(worker.bitmaps.length, 2);
    assert.equal(worker.messages.length, 2);
    assert.equal(worker.events.filter(event => event === 'submit').length, 10);
    if (batching) assert.equal(worker.events.filter(event => event === 'replay').length, 12);

    const consumer = page();
    consumer.present(...worker.messages.shift().args);
    assert.equal(worker.pending(), 1);
    assert.equal(worker.bitmaps[0].closed, true);
    assert.equal(consumer.presented(), 1);
    assert.deepEqual(consumer.canvas, { width: 640, height: 480 });
    worker.library.web_js_gl_present();
    assert.equal(worker.pending(), 2, 'the acknowledged slot can be reused');
    assert.equal(worker.bitmaps.length, 3);
  }
});

test('a page hidden after the send closes and acknowledges queued bitmaps', () => {
  const worker = runtime(true), consumer = page({ hidden: true });
  worker.library.web_js_gl_present();
  consumer.present(...worker.messages[0].args);
  assert.equal(worker.pending(), 0);
  assert.equal(worker.bitmaps[0].closed, true);
  assert.equal(consumer.consumed(), 0);
  assert.equal(consumer.presented(), 0);
  assert.deepEqual(consumer.canvas, { width: 0, height: 0 });
});

test('bitmap renderer failures still release both the bitmap and queue slot', () => {
  const worker = runtime(true), consumer = page({ failConsume: true });
  worker.library.web_js_gl_present();
  assert.throws(() => consumer.present(...worker.messages[0].args), /bitmap renderer failed/);
  assert.equal(worker.pending(), 0);
  assert.equal(worker.bitmaps[0].closed, true);
  assert.equal(consumer.presented(), 0);
  worker.library.web_js_gl_present();
  assert.equal(worker.pending(), 1);
});

test('bitmap transfer failures do not leave occupied queue slots', () => {
  for (const failure of ['failTransfer', 'failPost']) {
    const worker = runtime(true, { [failure]: true });
    assert.throws(() => worker.library.web_js_gl_present(), /failed/);
    assert.equal(worker.pending(), 0);
    if (failure === 'failPost') assert.equal(worker.bitmaps[0].closed, true);
  }
});

test('empty browser bitmaps are closed before serialization and later frames recover', () => {
  for (const presentAck of [false, true]) {
    const worker = runtime(true, { presentAck, bitmapSizes: [[0, 480], [640, 0], [640, 480]] });
    worker.library.web_js_gl_present();
    worker.library.web_js_gl_present();
    assert.equal(worker.pending(), presentAck ? 0 : null, 'discarded bitmaps never occupy a queue slot');
    assert.equal(worker.bitmaps[0].closed, true);
    assert.equal(worker.bitmaps[1].closed, true);
    assert.equal(worker.messages.filter(message => message.handler === 'haloPresent').length, 0);
    assert.equal(worker.messages.filter(message => message.handler === 'haloMessage').length, 1,
      'the diagnostic is logged once without transferring an empty bitmap');
    worker.library.web_js_gl_present();
    assert.equal(worker.pending(), presentAck ? 1 : null);
    assert.equal(worker.messages.filter(message => message.handler === 'haloPresent').length, 1);
    assert.equal(worker.bitmaps[2].closed, false);
  }
});

test('a new launcher using an old runtime keeps accepting one-argument bitmaps', () => {
  const consumer = page();
  for (let frame = 0; frame < 12; frame++) {
    const bitmap = { width: 800, height: 480, closed: false, close() { this.closed = true; } };
    consumer.present(bitmap);
    assert.equal(bitmap.closed, true);
  }
  assert.equal(consumer.presented(), 12);
});

test('an old launcher using a new runtime keeps receiving frames without acknowledgements', () => {
  for (const batching of [false, true]) {
    const worker = runtime(batching, { presentAck: false });
    for (let frame = 0; frame < 12; frame++) worker.library.web_js_gl_present();
    assert.equal(worker.pending(), null, 'unsupported acknowledgements must not allocate a counter');
    assert.equal(worker.messages.length, 12, 'an older page cannot release queue slots');
    assert.equal(worker.bitmaps.length, 12);
    for (const message of worker.messages) assert.equal(message.args.length, 1);
    assert.equal(worker.events.filter(event => event === 'submit').length, 0);
  }
});
