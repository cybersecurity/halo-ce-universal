const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

test('room forms accept typing without feeding keyboard events to the active game', () => {
  const listeners = new Map();
  const memory = { buffer: new SharedArrayBuffer(4096) };
  const context = {
    window: { addEventListener: (type, callback) => listeners.set(type, callback) },
    document: { addEventListener() {} }, console, setTimeout,
    matchMedia: () => ({ matches: false }),
  };
  vm.runInNewContext(fs.readFileSync(require.resolve('../../port/web/site/input.js'), 'utf8') +
    '\nthis.input = HaloInput;', context);
  context.input.attach({ memory, base: 0,
    offsets: { eventWrite: 0, eventRead: 4, events: 64, eventCapacity: 64, eventSize: 32 },
    canvas: { addEventListener() {} }, touch: false });
  const words = new Int32Array(memory.buffer);
  let prevented = 0;
  const key = { code: 'KeyW', key: 'w', preventDefault() { prevented++; } };
  listeners.get('keydown')(key);
  assert.equal(words[0], 1);
  assert.equal(prevented, 1);
  context.input.setUIActive(true);
  assert.equal(words[0], 2, 'opening the room panel releases game focus');
  listeners.get('keydown')(key);
  listeners.get('keyup')(key);
  assert.equal(words[0], 2);
  assert.equal(prevented, 1, 'typing and browser navigation remain available');
  context.input.setUIActive(false);
  assert.equal(words[0], 3);
  listeners.get('keydown')({ ...key, target: { closest: () => ({}) } });
  assert.equal(words[0], 3, 'editable elements never send keys to Halo');
  listeners.get('keydown')(key);
  assert.equal(words[0], 4, 'keyboard controls resume after closing the room panel');
});
