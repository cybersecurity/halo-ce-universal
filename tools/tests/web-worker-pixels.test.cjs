'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

function runtime(batching = false, options = {}) {
  const events = [], messages = [], reads = [], allocations = [];
  const originalFramebuffer = {}, originalPackBuffer = {};
  const constants = {
    READ_FRAMEBUFFER: 36008, READ_FRAMEBUFFER_BINDING: 36010,
    PIXEL_PACK_BUFFER: 35051, PIXEL_PACK_BUFFER_BINDING: 35053,
    PACK_ALIGNMENT: 3333, PACK_ROW_LENGTH: 3330, PACK_SKIP_PIXELS: 3332, PACK_SKIP_ROWS: 3331,
    RGBA: 6408, UNSIGNED_BYTE: 5121, READ_BUFFER: 3074, BACK: 1029,
  };
  const initial = new Map([
    [constants.READ_FRAMEBUFFER_BINDING, originalFramebuffer],
    [constants.PIXEL_PACK_BUFFER_BINDING, originalPackBuffer],
    [constants.PACK_ALIGNMENT, 8], [constants.PACK_ROW_LENGTH, 17],
    [constants.PACK_SKIP_PIXELS, 3], [constants.PACK_SKIP_ROWS, 2],
  ]);
  const state = new Map(initial);
  const readBuffers = new Map([[originalFramebuffer, 36064], [null, 0]]);
  const gl = {
    ...constants,
    flush() { events.push('submit'); },
    getParameter(parameter) {
      return parameter === this.READ_BUFFER ? readBuffers.get(state.get(this.READ_FRAMEBUFFER_BINDING)) : state.get(parameter);
    },
    bindFramebuffer(target, value) {
      assert.equal(target, this.READ_FRAMEBUFFER);
      state.set(this.READ_FRAMEBUFFER_BINDING, value);
    },
    bindBuffer(target, value) {
      assert.equal(target, this.PIXEL_PACK_BUFFER);
      state.set(this.PIXEL_PACK_BUFFER_BINDING, value);
    },
    pixelStorei(parameter, value) { state.set(parameter, value); },
    readBuffer(value) { readBuffers.set(state.get(this.READ_FRAMEBUFFER_BINDING), value); },
    readPixels(x, y, width, height, format, type, pixels) {
      events.push('read');
      reads.push({ width, height });
      assert.deepEqual([x, y, format, type], [0, 0, this.RGBA, this.UNSIGNED_BYTE]);
      assert.equal(Object.prototype.toString.call(pixels.buffer), '[object ArrayBuffer]');
      assert.equal(state.get(this.READ_FRAMEBUFFER_BINDING), null);
      assert.equal(state.get(this.PIXEL_PACK_BUFFER_BINDING), null);
      assert.equal(readBuffers.get(null), this.BACK);
      for (const parameter of [this.PACK_ROW_LENGTH, this.PACK_SKIP_PIXELS, this.PACK_SKIP_ROWS])
        assert.equal(state.get(parameter), 0);
      assert.equal(state.get(this.PACK_ALIGNMENT), 1);
      if (options.failRead) throw new Error('readback failed');
      for (let row = 0; row < height; row++) {
        for (let column = 0; column < width; column++)
          pixels.set([row + 1, column + 1, 9, 0], (row * width + column) * 4);
      }
    },
  };
  let library;
  const context = vm.createContext({
    addToLibrary(value) { library = value; }, SharedArrayBuffer,
    Uint8Array: class extends Uint8Array {
      constructor(...args) { super(...args); if (typeof args[0] === 'number') allocations.push(args[0]); }
    },
    OffscreenCanvas: class {
      constructor(width, height) { this.width = width; this.height = height; }
      getContext() { return gl; }
      transferToImageBitmap() { throw new Error('raw presentation reached ImageBitmap'); }
    },
    GL: { registerContext() { return 1; }, makeContextCurrent() {} },
    HaloStreamBatch: { install() {
      const queue = [];
      const flush = () => {
        events.push('replay');
        for (const command of queue.splice(0)) command();
      };
      // Model the production recorder's deferred mutations and synchronous
      // read barriers. Raw presentation must restore native state immediately.
      for (const name of ['bindFramebuffer', 'bindBuffer', 'pixelStorei', 'readBuffer', 'flush']) {
        const original = gl[name].bind(gl);
        gl[name] = (...args) => queue.push(() => original(...args));
      }
      for (const name of ['getParameter', 'readPixels']) {
        const original = gl[name].bind(gl);
        gl[name] = (...args) => { flush(); return original(...args); };
      }
      return { flush };
    } },
    ENVIRONMENT_IS_PTHREAD: true,
    postMessage(message, transfer) {
      events.push('post');
      if (options.failPost) throw new Error('post failed');
      assert.equal(message.handler, 'haloPresent');
      assert.equal(transfer.length, 1, 'only the owned pixel buffer is transferred');
      assert.equal(transfer[0], message.args[0].pixels);
      assert.equal(Object.prototype.toString.call(transfer[0]), '[object ArrayBuffer]');
      const delivered = structuredClone(message, { transfer });
      assert.equal(message.args[0].pixels.byteLength, 0, 'worker releases its payload after transfer');
      messages.push(delivered);
    },
  });
  vm.runInContext(fs.readFileSync(path.join(__dirname, '../../port/web/src/web_library.js'), 'utf8'), context);
  context.webHalo = library.$webHalo;
  library.web_js_gl_create(2, options.height || 3, batching, true, true);
  return { library, events, messages, reads, allocations,
    pending: () => Atomics.load(library.$webHalo.pendingFrames, 0),
    assertRestored() {
      assert.deepEqual(state, initial, 'native GL readback state is restored synchronously');
      assert.deepEqual(readBuffers, new Map([[originalFramebuffer, 36064], [null, 0]]),
        'each framebuffer retains its original read buffer selection');
    },
  };
}

function page(options = {}) {
  const canvas = { width: 0, height: 0 }, consumed = [], legacy = [];
  let presented = 0;
  const source = fs.readFileSync(path.join(__dirname, '../../port/web/site/app.js'), 'utf8');
  const callback = source.slice(source.indexOf('haloPresent:') + 'haloPresent:'.length,
    source.indexOf('\n      haloMessage:')).trim().replace(/,$/, '');
  const present = vm.runInNewContext('(' + callback + ')', {
    canvas, pixelFrames: true, document: { hidden: Boolean(options.hidden) },
    context: {
      putImageData(value, x, y) {
        if (options.failConsume) throw new Error('canvas presentation failed');
        assert.deepEqual([x, y], [0, 0]);
        consumed.push({ width: value.width, height: value.height, data: Array.from(value.data) });
      },
      drawImage(bitmap, x, y) { assert.deepEqual([x, y], [0, 0]); legacy.push(bitmap); },
      transferFromImageBitmap() { throw new Error('raw launcher reached bitmaprenderer'); },
    },
    ImageData: class { constructor(data, width, height) { Object.assign(this, { data, width, height }); } },
    countPresent() { presented++; },
  });
  return { canvas, present, consumed, legacy, presented: () => presented };
}

test('RGBA presentation transfers owned bytes and flips odd and even row counts once', () => {
  for (const batching of [false, true]) {
    for (const height of [1, 2, 3, 4]) {
      const worker = runtime(batching, { height }), consumer = page();
      worker.library.web_js_gl_present();
      assert.equal(worker.pending(), 1);
      worker.assertRestored();
      consumer.present(...worker.messages[0].args);
      assert.equal(worker.pending(), 0);
      assert.equal(consumer.presented(), 1);
      assert.deepEqual(consumer.canvas, { width: 2, height });
      const expected = [];
      for (let row = height; row > 0; row--)
        for (let column = 1; column <= 2; column++) expected.push(row, column, 9, 255);
      assert.deepEqual(consumer.consumed[0].data, expected);
    }
  }
});

test('pixel backpressure caps payloads at two and skips further allocations and readbacks', () => {
  for (const batching of [false, true]) {
    const worker = runtime(batching);
    worker.library.web_js_gl_present(); worker.library.web_js_gl_present();
    const allocations = worker.allocations.length;
    for (let frame = 0; frame < 12; frame++) worker.library.web_js_gl_present();
    assert.equal(worker.pending(), 2);
    assert.equal(worker.messages.length, 2);
    assert.equal(worker.reads.length, 2);
    assert.equal(worker.allocations.length, allocations, 'a blocked page does not allocate discarded frames');
    assert.equal(worker.events.filter(event => event === 'submit').length, 12);
    worker.assertRestored();
    page().present(...worker.messages[0].args);
    worker.library.web_js_gl_present();
    assert.equal(worker.pending(), 2);
    assert.equal(worker.reads.length, 3);
  }
});

test('a pixel frame arriving after the page hides is acknowledged without drawing', () => {
  const worker = runtime(true), consumer = page({ hidden: true });
  worker.library.web_js_gl_present();
  consumer.present(...worker.messages[0].args);
  assert.equal(worker.pending(), 0);
  assert.equal(consumer.presented(), 0);
  assert.equal(consumer.consumed.length, 0);
  assert.deepEqual(consumer.canvas, { width: 0, height: 0 });
});

test('hidden worker frames submit without allocating a pixel payload', () => {
  const worker = runtime(true);
  worker.library.web_js_gl_flush(); worker.library.web_js_gl_flush();
  assert.equal(worker.pending(), 0);
  assert.equal(worker.reads.length, 0);
  assert.equal(worker.allocations.length, 0);
  assert.deepEqual(worker.events, ['replay', 'submit', 'replay', 'submit']);
});

test('failed readbacks and failed posts restore GL state and leave no occupied slots', () => {
  for (const batching of [false, true]) {
    for (const failure of ['failRead', 'failPost']) {
      const worker = runtime(batching, { [failure]: true });
      assert.throws(() => worker.library.web_js_gl_present(), /failed/);
      worker.assertRestored();
      assert.equal(worker.pending(), 0);
      assert.equal(worker.messages.length, 0);
    }
  }
});

test('page drawing errors acknowledge the pixel slot so the worker can continue', () => {
  const worker = runtime(), consumer = page({ failConsume: true });
  worker.library.web_js_gl_present();
  assert.throws(() => consumer.present(...worker.messages[0].args), /canvas presentation failed/);
  assert.equal(worker.pending(), 0);
  assert.equal(consumer.presented(), 0);
  worker.library.web_js_gl_present();
  assert.equal(worker.pending(), 1);
});

test('resizing produces an exact pixel payload and the launcher adopts its dimensions', () => {
  const worker = runtime(), consumer = page();
  worker.library.web_js_gl_resize(3, 2);
  worker.library.web_js_gl_present();
  assert.equal(worker.messages[0].args[0].pixels.byteLength, 3 * 2 * 4);
  assert.deepEqual(worker.reads, [{ width: 3, height: 2 }]);
  consumer.present(...worker.messages[0].args);
  assert.deepEqual(consumer.canvas, { width: 3, height: 2 });
  assert.equal(worker.pending(), 0);
});

test('a pixel launcher consumes and releases legacy ImageBitmaps from an older runtime', () => {
  const consumer = page();
  const bitmap = { width: 4, height: 2, closed: false, close() { this.closed = true; } };
  const pending = new Int32Array(new SharedArrayBuffer(4));
  pending[0] = 1;
  consumer.present(bitmap, pending.buffer);
  assert.equal(consumer.legacy[0], bitmap);
  assert.equal(bitmap.closed, true);
  assert.equal(pending[0], 0);
  assert.equal(consumer.presented(), 1);
  assert.deepEqual(consumer.canvas, { width: 4, height: 2 });
});
