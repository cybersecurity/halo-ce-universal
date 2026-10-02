/*
XISO-WORKER.JS

Copies the maps folder out of an Xbox disc image of Halo: Combat Evolved
(an "xiso", .iso or .xiso) into the site's Origin Private File System, as
port/linux/src/xiso.c does for the desktop ports. It runs in a Worker so
that it can use synchronous OPFS access handles, including in browsers
that do not support OPFS writable streams.

The file system is XDVDFS: 2048-byte sectors; a volume descriptor at 0x10000
that starts and ends with "MICROSOFT*XBOX*MEDIA" and gives the root
directory's sector and size; directories whose entries form a binary tree
(left and right subtree offsets in 4-byte units, the start sector, the size,
attributes, the name's length and the name). Images of a whole disc put the
game partition further in, at one of the offsets below (extract-xiso's).

The page gets messages { type: 'progress', file, done, total },
{ type: 'done', files, bytes } or { type: 'error', message }.
*/

'use strict';

const SECTOR_SIZE = 2048;
const VOLUME_DESCRIPTOR_OFFSET = 0x10000;
const PARTITION_OFFSETS = [0, 0x0FD90000, 0x02080000, 0x18300000];
const MAGIC = 'MICROSOFT*XBOX*MEDIA';
const ENTRY_HEADER_SIZE = 14;
const ATTRIBUTE_DIRECTORY = 0x10;
const COPY_CHUNK = 4 * 1024 * 1024;
// written last: the page starts the game only when it is there
const COMPLETE_MARKER = '.complete';

async function readAt(file, offset, size) {
  const end = Math.min(offset + size, file.size);
  if (offset >= end) return new Uint8Array(0);
  return new Uint8Array(await file.slice(offset, end).arrayBuffer());
}

function text(bytes, start, length) {
  let result = '';
  for (let i = 0; i < length; i++) result += String.fromCharCode(bytes[start + i]);
  return result;
}

function u32(bytes, at) {
  return (bytes[at] | bytes[at + 1] << 8 | bytes[at + 2] << 16 | bytes[at + 3] << 24) >>> 0;
}

async function findVolume(file) {
  for (const partition of PARTITION_OFFSETS) {
    const descriptor = await readAt(file, partition + VOLUME_DESCRIPTOR_OFFSET, SECTOR_SIZE);
    if (descriptor.length < SECTOR_SIZE) continue;
    if (text(descriptor, 0, 20) === MAGIC && text(descriptor, 0x7EC, 20) === MAGIC) {
      return { partition, rootSector: u32(descriptor, 20), rootSize: u32(descriptor, 24) };
    }
  }
  return null;
}

// the entries of a directory table, files or directories as asked
function walkDirectory(table, wantDirectories) {
  const entries = [];
  let visited = 0;
  const walk = (offset, depth) => {
    offset *= 4;
    if (depth > 64 || ++visited > 4096 || offset + ENTRY_HEADER_SIZE > table.length) return;
    const left = table[offset] | table[offset + 1] << 8;
    const right = table[offset + 2] | table[offset + 3] << 8;
    if (left === 0xFFFF) return;
    const nameLength = table[offset + 13];
    if (left) walk(left, depth + 1);
    if (offset + ENTRY_HEADER_SIZE + nameLength <= table.length && nameLength > 0 &&
        !(table[offset + 12] & ATTRIBUTE_DIRECTORY) === !wantDirectories) {
      const name = text(table, offset + ENTRY_HEADER_SIZE, nameLength);
      if (name !== '.' && name !== '..' && !name.includes('/') && !name.includes('\\')) {
        entries.push({ name, sector: u32(table, offset + 4), size: u32(table, offset + 8) });
      }
    }
    if (right) walk(right, depth + 1);
  };
  walk(0, 0);
  return entries;
}

async function readDirectory(file, volume, sector, size) {
  if (!size || size > 1024 * 1024) return null;
  const table = await readAt(file, volume.partition + sector * SECTOR_SIZE, size);
  return table.length === size ? table : null;
}

async function writeFile(directory, name, source, onChunk) {
  const handle = await directory.getFileHandle(name, { create: true });
  const access = await handle.createSyncAccessHandle();
  try {
    await access.truncate(0);
    let position = 0;
    while (position < source.size) {
      const count = Math.min(COPY_CHUNK, source.size - position);
      const bytes = await readAt(source.file, source.offset + position, count);
      if (bytes.length !== count) throw new Error(`Could not read ${name} from the disc image (is it complete?).`);
      let written = 0;
      while (written < count) {
        const result = await access.write(bytes.subarray(written), { at: position + written });
        if (!result) throw new Error(`Could not write ${name} (is the device full?).`);
        written += result;
      }
      position += count;
      onChunk(count);
    }
    await access.flush();
  } finally {
    await access.close();
  }
}

async function extract(file) {
  const volume = await findVolume(file);
  if (!volume) throw new Error('This file is not an Xbox disc image (no XDVDFS volume was found).');

  const root = await readDirectory(file, volume, volume.rootSector, volume.rootSize);
  if (!root) throw new Error("The disc image's file system is damaged.");
  const maps = walkDirectory(root, true).find((entry) => entry.name.toLowerCase() === 'maps');
  if (!maps) throw new Error('The disc image has no maps folder: it is not a Halo disc.');
  const table = await readDirectory(file, volume, maps.sector, maps.size);
  if (!table) throw new Error("The disc image's maps folder is damaged.");
  const files = walkDirectory(table, false);
  if (!files.some((entry) => entry.name.toLowerCase() === 'ui.map')) {
    throw new Error("The disc image's maps folder has no ui.map: it is not a Halo disc.");
  }

  const total = files.reduce((sum, entry) => sum + entry.size, 0);
  const storage = await navigator.storage.getDirectory();
  // a new copy replaces whatever an earlier, perhaps interrupted, one left
  await storage.removeEntry('maps', { recursive: true }).catch(() => {});
  const directory = await storage.getDirectoryHandle('maps', { create: true });
  let done = 0;
  let lastReport = 0;
  for (const entry of files) {
    await writeFile(directory, entry.name, {
      file,
      offset: volume.partition + entry.sector * SECTOR_SIZE,
      size: entry.size,
    }, (count) => {
      done += count;
      const now = Date.now();
      if (now - lastReport > 100 || done === total) {
        lastReport = now;
        postMessage({ type: 'progress', file: entry.name, done, total });
      }
    });
  }
  const marker = await directory.getFileHandle(COMPLETE_MARKER, { create: true });
  const access = await marker.createSyncAccessHandle();
  await access.truncate(0);
  await access.write(new TextEncoder().encode(JSON.stringify({ files: files.map((f) => f.name), bytes: total })), { at: 0 });
  await access.flush();
  await access.close();
  return { files: files.length, bytes: total };
}

onmessage = async (event) => {
  try {
    const result = await extract(event.data.file);
    postMessage({ type: 'done', ...result });
  } catch (error) {
    postMessage({ type: 'error', name: error.name, message: error && error.message ? error.message : String(error) });
  }
};
