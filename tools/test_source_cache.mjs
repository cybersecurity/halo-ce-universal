import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

const cacheSource = readFileSync(new URL('../port/web/site/cache.js', import.meta.url), 'utf8');
const workerSource = readFileSync(new URL('../port/web/site/xiso-worker.js', import.meta.url), 'utf8');
const map = new Uint8Array(2048);
map.set(new TextEncoder().encode('daeh'), 0);
map.set(new TextEncoder().encode('toof'), 2044);

function storage() {
  const files = new Map(), directories = new Set(['']);
  const absent = () => new DOMException('missing', 'NotFoundError');
  function put(path, data) {
    const parts = path.split('/');
    for (let i = 1; i < parts.length; i++) directories.add(parts.slice(0, i).join('/'));
    files.set(path, typeof data === 'string' ? new TextEncoder().encode(data) : data.slice());
  }
  function directory(path) {
    return {
      async getDirectoryHandle(name, { create = false } = {}) {
        const child = path ? `${path}/${name}` : name;
        if (!directories.has(child)) {
          if (!create) throw absent();
          directories.add(child);
        }
        return directory(child);
      },
      async getFileHandle(name, { create = false } = {}) {
        const child = path ? `${path}/${name}` : name;
        if (!files.has(child)) {
          if (!create) throw absent();
          files.set(child, new Uint8Array());
        }
        return {
          async getFile() { return new Blob([files.get(child)]); },
          async createSyncAccessHandle() {
            let data = files.get(child);
            return {
              async truncate(size) { data = data.slice(0, size); },
              async write(value, { at }) {
                const next = new Uint8Array(Math.max(data.length, at + value.length));
                next.set(data); next.set(value, at); data = next;
                return value.length;
              },
              async flush() { files.set(child, data); },
              async close() { files.set(child, data); },
            };
          },
        };
      },
      async removeEntry(name) {
        const child = path ? `${path}/${name}` : name;
        for (const key of files.keys()) if (key === child || key.startsWith(child + '/')) files.delete(key);
        for (const key of directories) if (key === child || key.startsWith(child + '/')) directories.delete(key);
      },
    };
  }
  return { files, put, getDirectory: async () => directory('') };
}

function launcher(data = storage(), locked = false) {
  let requests = 0;
  const context = {
    Uint8Array, Blob, DOMException, TextEncoder,
    navigator: { storage: data, locks: { request: async (_name, _options, use) => use(locked ? null : {}) } },
    fetch() { requests++; throw new Error('Game-data network request is forbidden'); },
  };
  vm.runInNewContext(cacheSource, context);
  return { cache: context.HaloCache, requests: () => requests, data };
}

const cold = launcher();
assert.equal(await cold.cache.mapsState(), null);
assert.equal(await cold.cache.mapsState({ required: ['ui.map', 'beavercreek.map'] }), null);
assert.equal(cold.requests(), 0);
assert.equal(typeof cold.cache.download, 'undefined');
assert.equal(typeof cold.cache.ensure, 'undefined');
console.log('PASS missing maps require user import and cause no network request');

const native = launcher();
const names = [...native.cache.expected];
for (const name of names) native.data.put(`maps/${name}`, map);
native.data.put('maps/.complete', JSON.stringify({ files: names, bytes: map.length * names.length }));
const nativeRoom = await native.cache.mapsState({ required: ['ui.map', 'beavercreek.map'] });
assert.equal(nativeRoom.dataRoot, '/data');
assert.equal(nativeRoom.requiredBytes, map.length * 2);
assert.equal(nativeRoom.files.length, names.length);
assert.equal((await native.cache.mapsState()).files.length, names.length);
assert.equal(native.requests(), 0);
console.log('PASS imported maps satisfy both room and full-menu launch without network');

const legacy = launcher();
for (const name of names) legacy.data.put(`halo/data/maps/${name}`, map);
assert.equal((await legacy.cache.mapsState()).dataRoot, '/data/halo/data');
assert.equal((await legacy.cache.mapsState({ required: ['ui.map', 'beavercreek.map'] })).saveRoot, '/data/halo/save');
console.log('PASS complete older browser cache remains compatible without a manifest');

const partial = launcher();
for (const name of ['ui.map', 'beavercreek.map']) partial.data.put(`halo/data/maps/${name}`, map);
assert.equal(await partial.cache.mapsState({ required: ['ui.map', 'beavercreek.map'] }), null);
partial.data.put('halo/data/maps.json', JSON.stringify({ 'ui.map': map.length, 'beavercreek.map': map.length }));
assert.equal((await partial.cache.mapsState({ required: ['ui.map', 'beavercreek.map'] })).files.length, 2);
assert.equal(await partial.cache.mapsState(), null);
partial.data.put('halo/data/maps/beavercreek.map', new Uint8Array(2048));
assert.equal(await partial.cache.mapsState({ required: ['ui.map', 'beavercreek.map'] }), null);
console.log('PASS marked room cache is reused, but unmarked or damaged partial data cannot launch');

const interrupted = launcher();
interrupted.data.put('maps/ui.map', map);
interrupted.data.put('maps/.complete', JSON.stringify({ files: ['ui.map'], bytes: 1 }));
assert.equal(await interrupted.cache.mapsState({ required: ['ui.map'] }), null);
for (const required of [[], ['ui.map', 'ui.map'], ['../ui.map'], ['missing.map'], 'ui.map']) {
  await assert.rejects(cold.cache.mapsState({ required }), { name: 'TypeError' });
}
const busy = launcher(storage(), true);
await assert.rejects(busy.cache.withLock(async () => {}), /another tab/);
console.log('PASS incomplete import markers and invalid cache requests remain blocked');

// Minimal synthetic XDVDFS image exercises the real local ISO extraction path.
const sector = 2048;
const iso = new Uint8Array(44 * sector);
const descriptor = 0x10000;
const magic = new TextEncoder().encode('MICROSOFT*XBOX*MEDIA');
iso.set(magic, descriptor);
iso.set(magic, descriptor + 0x7ec);
const view = new DataView(iso.buffer);
view.setUint32(descriptor + 20, 41, true);
view.setUint32(descriptor + 24, sector, true);
function entry(at, name, fileSector, size, attributes) {
  view.setUint32(at + 4, fileSector, true);
  view.setUint32(at + 8, size, true);
  iso[at + 12] = attributes;
  iso[at + 13] = name.length;
  iso.set(new TextEncoder().encode(name), at + 14);
}
entry(41 * sector, 'maps', 42, sector, 0x10);
entry(42 * sector, 'ui.map', 43, sector, 0);
iso.set(map, 43 * sector);
const imported = launcher();
const messages = [];
const worker = { Uint8Array, Blob, TextEncoder, navigator: { storage: imported.data },
  postMessage: message => messages.push(message) };
vm.runInNewContext(workerSource, worker);
await worker.onmessage({ data: { file: new Blob([iso]) } });
assert.equal(messages.at(-1).type, 'done');
assert.deepEqual([...imported.data.files.get('maps/ui.map')], [...map]);
assert.equal(JSON.parse(new TextDecoder().decode(imported.data.files.get('maps/.complete'))).bytes, sector);
assert.equal((await imported.cache.mapsState({ required: ['ui.map'] })).dataRoot, '/data');
console.log('PASS user-supplied ISO still imports maps and commits its completion marker');
