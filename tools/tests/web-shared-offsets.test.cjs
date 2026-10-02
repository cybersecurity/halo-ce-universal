const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

// Exercise the production ABI reader with both versions of the runtime's
// exported layout. Adjacent memory is deliberately plausible as new offsets.
function readLayout(current) {
  const source = fs.readFileSync(require.resolve('../../port/web/site/app.js'), 'utf8');
  const start = source.indexOf('  function readOffsets(module) {');
  const end = source.indexOf('  function updateDisplaySize()', start);
  assert.ok(start > 0 && end > start);
  const memory = { buffer: new ArrayBuffer(2048) }, words = new Int32Array(memory.buffer);
  const pingStart = 512 + 32 * 12;
  words[0] = current ? pingStart + 20 + 128 * 8 : pingStart;
  words[34] = 512; words[35] = 32;
  words.set([pingStart, pingStart + 4, pingStart + 8, pingStart + 12, pingStart + 16, pingStart + 20, 128], 36);
  const context = { state: { memory }, Int32Array, module: { _web_shared_offsets() { return 0; } } };
  vm.runInNewContext(source.slice(start, end) + '\nresult = readOffsets(module);', context);
  return context.result;
}

test('new launcher does not read ping offsets from adjacent memory in a cached older runtime', () => {
  const offsets = readLayout(false);
  assert.equal(offsets.gatewayPeerCount, 32);
  assert.equal(offsets.pingSequence, undefined);
  assert.equal(offsets.pingPeers, undefined);
});

test('current runtime publishes the appended ping table without shifting the original fields', () => {
  const offsets = readLayout(true);
  assert.equal(offsets.gatewayPeers, 512);
  assert.equal(offsets.pingSequence, 896);
  assert.equal(offsets.pingPeers, 916);
  assert.equal(offsets.pingPeerCount, 128);
});
