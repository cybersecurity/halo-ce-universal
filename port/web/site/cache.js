/* Recognize maps imported from the player's disc image and existing browser caches. */
'use strict';

globalThis.HaloCache = (() => {
  const EXPECTED = [
    'ui', 'a10', 'a30', 'a50', 'b30', 'b40', 'c10', 'c20', 'c40', 'd20', 'd40',
    'beavercreek', 'bloodgulch', 'boardingaction', 'carousel', 'chillout', 'damnation',
    'hangemhigh', 'longest', 'prisoner', 'putput', 'ratrace', 'sidewinder', 'wizard',
  ].map(name => name + '.map');
  const DATA_PATH = ['halo', 'data'];

  async function directory(path, create = false) {
    let folder = await navigator.storage.getDirectory();
    for (const name of path) folder = await folder.getDirectoryHandle(name, { create });
    return folder;
  }

  async function readJSON(path, name) {
    try {
      return JSON.parse(await (await (await (await directory(path)).getFileHandle(name)).getFile()).text());
    } catch (error) {
      if (error.name === 'NotFoundError' || error instanceof SyntaxError) return null;
      throw error;
    }
  }

  async function headerValid(file) {
    if (file.size < 2048) return false;
    const data = new Uint8Array(await file.slice(0, 2048).arrayBuffer());
    const text = offset => String.fromCharCode(...data.subarray(offset, offset + 4));
    return text(0) === 'daeh' && text(2044) === 'toof';
  }

  async function inspect(path, manifest = null) {
    const maps = new Map();
    let folder;
    try { folder = await directory(path); }
    catch (error) { if (error.name === 'NotFoundError') return maps; throw error; }
    for (const name of EXPECTED) {
      try {
        const file = await (await folder.getFileHandle(name)).getFile();
        if ((!manifest || manifest[name] === file.size) && await headerValid(file)) maps.set(name, file.size);
      } catch (error) {
        if (error.name !== 'NotFoundError') throw error;
      }
    }
    return maps;
  }

  function validateRequired(required) {
    if (!Array.isArray(required) || !required.length || new Set(required).size !== required.length ||
        required.some(name => !EXPECTED.includes(name))) {
      throw new TypeError('Required maps must be a nonempty list of unique supported .map names');
    }
    return required.slice();
  }

  function mapState(maps, legacy, required) {
    if (!required.every(name => maps.has(name))) return null;
    return {
      files: [...maps.keys()], bytes: [...maps.values()].reduce((sum, size) => sum + size, 0),
      requiredBytes: required.reduce((sum, name) => sum + maps.get(name), 0),
      dataRoot: legacy ? '/data/halo/data' : '/data',
      saveRoot: legacy ? '/data/halo/save' : '/data/save', cached: true,
    };
  }

  async function nativeMaps() {
    const marker = await readJSON(['maps'], '.complete');
    // Disc extraction commits this marker only after the entire copy finishes.
    // Unmarked maps may be leftovers from an interrupted import.
    if (!marker || !Array.isArray(marker.files) || !marker.files.length ||
        new Set(marker.files).size !== marker.files.length ||
        marker.files.some(name => !EXPECTED.includes(name)) ||
        !Number.isSafeInteger(marker.bytes) || marker.bytes < 2048) return new Map();
    const folder = await directory(['maps']);
    let bytes = 0;
    for (const name of marker.files) {
      try { bytes += (await (await folder.getFileHandle(name)).getFile()).size; }
      catch (error) { if (error.name === 'NotFoundError') return new Map(); throw error; }
    }
    if (bytes !== marker.bytes) return new Map();
    const maps = await inspect(['maps']);
    return new Map([...maps].filter(([name]) => marker.files.includes(name)));
  }

  async function mapsState({ required = EXPECTED } = {}) {
    required = validateRequired(required);
    // Older production imports live below halo/data. Its oldest imports
    // predate maps.json and were checked by map header.
    const manifest = await readJSON(DATA_PATH, 'maps.json');
    const legacyMaps = await inspect([...DATA_PATH, 'maps'], manifest);
    // Before maps.json existed, the production importer copied all 24 maps.
    // Keep that complete-cache compatibility without treating interrupted,
    // unmarked imports as a committed room subset.
    const legacy = manifest || legacyMaps.size === EXPECTED.length ? mapState(legacyMaps, true, required) : null;
    if (legacy) return legacy;
    return mapState(await nativeMaps(), false, required);
  }

  async function withLock(task, signal) {
    signal?.throwIfAborted();
    if (!navigator.locks) return task();
    return navigator.locks.request('halo-game', { ifAvailable: true }, lock => {
      if (!lock) throw new Error('Halo is open in another tab. Close that tab, then retry.');
      return task();
    });
  }

  return { mapsState, withLock, directory, expected: EXPECTED.slice() };
})();
