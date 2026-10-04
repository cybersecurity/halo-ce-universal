// Reader for the Xbox DVD filesystem (XDVDFS) of an original Xbox game disc
// image. Works on a File or Blob without loading the whole image: every read
// is a slice.
//
// A disc image holds the game partition either at the start (an "xiso", as
// written by extract-xiso) or behind the video partition (a full XGD1/2/3
// dump). The partition's volume descriptor sits 32 sectors in and begins and
// ends with "MICROSOFT*XBOX*MEDIA".

export const SECTOR_SIZE = 2048;

const MEDIA_MAGIC = "MICROSOFT*XBOX*MEDIA";
const VOLUME_DESCRIPTOR_SECTOR = 32;
const DIRECTORY_ATTRIBUTE = 0x10;
const NO_ENTRY = 0xffff;

// xiso, XGD1 (Halo: Combat Evolved's disc), XGD2, XGD3
const PARTITION_OFFSETS = [0, 0x18300000, 0x2080000, 0xfd90000];

async function readBytes(blob, offset, length) {
	return new Uint8Array(await blob.slice(offset, offset + length).arrayBuffer());
}

function ascii(bytes, start, length) {
	let text = "";
	for (let i = start; i < start + length; i++) text += String.fromCharCode(bytes[i]);
	return text;
}

export async function openXiso(blob) {
	for (const partition of PARTITION_OFFSETS) {
		const descriptorOffset = partition + VOLUME_DESCRIPTOR_SECTOR * SECTOR_SIZE;
		if (descriptorOffset + SECTOR_SIZE > blob.size) continue;
		const descriptor = await readBytes(blob, descriptorOffset, SECTOR_SIZE);
		if (ascii(descriptor, 0, 20) !== MEDIA_MAGIC) continue;
		if (ascii(descriptor, SECTOR_SIZE - 20, 20) !== MEDIA_MAGIC) continue;
		const view = new DataView(descriptor.buffer);
		return {
			blob,
			partition,
			rootSector: view.getUint32(20, true),
			rootSize: view.getUint32(24, true),
		};
	}
	throw new Error("not an Xbox disc image (no XDVDFS volume descriptor found)");
}

// A directory is a binary tree of entries packed into whole sectors; each
// entry names its left and right subtrees by offset in 4-byte units.
async function readDirectory(image, sector, size) {
	if (size === 0) return [];
	const bytes = await readBytes(image.blob, image.partition + sector * SECTOR_SIZE, size);
	const view = new DataView(bytes.buffer);
	const entries = [];
	const pending = [0];
	const visited = new Set();
	while (pending.length) {
		const offset = pending.pop();
		if (visited.has(offset) || offset + 14 > size) continue;
		visited.add(offset);
		const left = view.getUint16(offset, true);
		const right = view.getUint16(offset + 2, true);
		if (left === NO_ENTRY && right === NO_ENTRY) continue; // sector padding
		const nameLength = bytes[offset + 13];
		entries.push({
			name: ascii(bytes, offset + 14, nameLength),
			sector: view.getUint32(offset + 4, true),
			size: view.getUint32(offset + 8, true),
			isDirectory: (bytes[offset + 12] & DIRECTORY_ATTRIBUTE) !== 0,
		});
		if (left && left !== NO_ENTRY) pending.push(left * 4);
		if (right && right !== NO_ENTRY) pending.push(right * 4);
	}
	return entries.sort((a, b) => a.name.localeCompare(b.name));
}

// Every file on the disc as { path, sector, size }, paths joined with "/".
export async function listFiles(image) {
	const files = [];
	const walk = async (sector, size, prefix) => {
		for (const entry of await readDirectory(image, sector, size)) {
			const path = prefix + entry.name;
			if (entry.isDirectory) await walk(entry.sector, entry.size, path + "/");
			else files.push({ path, sector: entry.sector, size: entry.size });
		}
	};
	await walk(image.rootSector, image.rootSize, "");
	return files;
}

export function fileBlob(image, file) {
	const start = image.partition + file.sector * SECTOR_SIZE;
	return image.blob.slice(start, start + file.size);
}

// The uncompressed 0x800-byte header at the start of every cache file
// (source/cache/cache_files.c, struct cache_file_header).
export const EXPECTED_BUILD = "01.01.14.2342";
export const EXPECTED_CACHE_VERSION = 5;

export async function readCacheHeader(blob) {
	if (blob.size < 0x800) return null;
	const bytes = await readBytes(blob, 0, 0x800);
	const view = new DataView(bytes.buffer);
	const cString = (start) => {
		const end = bytes.indexOf(0, start);
		return ascii(bytes, start, (end < 0 || end > start + 0x20 ? start + 0x20 : end) - start);
	};
	return {
		signatureOk: ascii(bytes, 0, 4) === "daeh" && ascii(bytes, 0x7fc, 4) === "toof",
		version: view.getInt32(4, true),
		name: cString(0x20),
		build: cString(0x40),
	};
}
