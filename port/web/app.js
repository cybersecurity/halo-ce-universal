// Import a Halo: Combat Evolved PAL disc image into the browser's origin
// private file system (OPFS), where the web build will mount it as d:\.

import {
	EXPECTED_BUILD,
	EXPECTED_CACHE_VERSION,
	fileBlob,
	listFiles,
	openXiso,
	readCacheHeader,
} from "./xiso.js";

const DATA_DIRECTORY = "halo-data";
// Only maps/ is imported: bink/ holds the cutscene movies, which the port does
// not play yet (port/linux/src/bink_null.c), Xdemos/ is the demo-disc
// launcher and default.xbe is the Xbox executable the port replaces.
// Together they are about 1.2 GB the game never reads.
const IMPORTED_PATH = /^maps\//i;
const COPY_CHUNK = 8 * 1024 * 1024;

const $ = (id) => document.getElementById(id);
const log = (text) => {
	$("log").textContent += text + "\n";
	console.log("[import] " + text);
};

function formatBytes(bytes) {
	const units = ["B", "KB", "MB", "GB"];
	let unit = 0;
	while (bytes >= 1024 && unit < units.length - 1) {
		bytes /= 1024;
		unit++;
	}
	return `${bytes.toFixed(unit ? 1 : 0)} ${units[unit]}`;
}

async function dataRoot(create) {
	const root = await navigator.storage.getDirectory();
	return root.getDirectoryHandle(DATA_DIRECTORY, { create });
}

async function directoryFor(root, path) {
	let directory = root;
	const parts = path.split("/");
	for (const part of parts.slice(0, -1)) {
		directory = await directory.getDirectoryHandle(part, { create: true });
	}
	return { directory, name: parts[parts.length - 1] };
}

// Checks every maps/*.map cache file header; returns the list of problems.
async function validate(image, files) {
	const maps = files.filter((file) => /^maps\/[^/]+\.map$/i.test(file.path));
	if (!maps.length) return { maps, problems: ["the disc has no maps/*.map cache files"] };
	const problems = [];
	for (const file of maps) {
		const header = await readCacheHeader(fileBlob(image, file));
		if (!header || !header.signatureOk) problems.push(`${file.path}: not a cache file`);
		else if (header.version !== EXPECTED_CACHE_VERSION) problems.push(`${file.path}: cache version ${header.version}, expected ${EXPECTED_CACHE_VERSION}`);
		else if (header.build !== EXPECTED_BUILD) problems.push(`${file.path}: build ${header.build || "(none)"}, expected ${EXPECTED_BUILD}`);
		else log(`ok  ${file.path}  (${header.name}, build ${header.build})`);
	}
	return { maps, problems };
}

async function copyFile(root, image, file, onBytes) {
	const { directory, name } = await directoryFor(root, file.path);
	const handle = await directory.getFileHandle(name, { create: true });
	const writable = await handle.createWritable();
	const source = fileBlob(image, file);
	for (let offset = 0; offset < source.size; offset += COPY_CHUNK) {
		const chunk = source.slice(offset, offset + COPY_CHUNK);
		await writable.write(chunk);
		onBytes(chunk.size);
	}
	await writable.close();
}

async function importIso(blob) {
	$("log").textContent = "";
	setBusy(true);
	try {
		log(`reading ${blob.name ?? "image"} (${formatBytes(blob.size)})`);
		const image = await openXiso(blob);
		log(`game partition at offset 0x${image.partition.toString(16)}`);
		const allFiles = await listFiles(image);
		const files = allFiles.filter((file) => IMPORTED_PATH.test(file.path));
		const total = files.reduce((sum, file) => sum + file.size, 0);
		log(`${allFiles.length} files on the disc; importing ${files.length} from maps/ (${formatBytes(total)})`);

		const { maps, problems } = await validate(image, files);
		if (problems.length) {
			problems.forEach((problem) => log("bad " + problem));
			throw new Error(`this is not the PAL ${EXPECTED_BUILD} disc the game needs`);
		}

		// a previous import is replaced, so it must not count against the quota
		await forget(false);
		if (navigator.storage.persist) await navigator.storage.persist();
		const estimate = await navigator.storage.estimate();
		if (estimate.quota && estimate.quota - estimate.usage < total) {
			throw new Error(`not enough browser storage: need ${formatBytes(total)}, have ${formatBytes(estimate.quota - estimate.usage)}; free up disk space and try again`);
		}

		const root = await dataRoot(true);
		let copied = 0;
		for (const file of files) {
			$("progress-label").textContent = file.path;
			await copyFile(root, image, file, (bytes) => {
				copied += bytes;
				$("progress").value = total ? copied / total : 1;
			});
		}
		const manifest = { build: EXPECTED_BUILD, files: files.length, bytes: total, maps: maps.map((map) => map.path), imported: new Date().toISOString() };
		const { directory, name } = await directoryFor(root, ".import.json");
		const writable = await (await directory.getFileHandle(name, { create: true })).createWritable();
		await writable.write(JSON.stringify(manifest));
		await writable.close();
		log(`imported ${files.length} files`);
		await showStored();
	} catch (error) {
		log("error: " + error.message);
		console.error(error);
	} finally {
		setBusy(false);
		$("progress-label").textContent = "";
	}
}

async function readManifest() {
	try {
		const root = await dataRoot(false);
		const file = await (await root.getFileHandle(".import.json")).getFile();
		return JSON.parse(await file.text());
	} catch {
		return null;
	}
}

async function showStored() {
	const manifest = await readManifest();
	$("stored").hidden = !manifest;
	$("pick").hidden = !!manifest;
	if (manifest) {
		$("stored-summary").textContent = `Build ${manifest.build}: ${manifest.files} files, ${formatBytes(manifest.bytes)}, imported ${new Date(manifest.imported).toLocaleString()}.`;
		$("stored-maps").textContent = manifest.maps.map((path) => path.replace(/^maps\//i, "")).join("  ");
	}
}

async function forget(refresh = true) {
	const root = await navigator.storage.getDirectory();
	try {
		await root.removeEntry(DATA_DIRECTORY, { recursive: true });
	} catch {
		// nothing stored yet
	}
	if (refresh) await showStored();
}

function setBusy(busy) {
	$("progress-row").hidden = !busy;
	$("iso").disabled = busy;
	$("progress").value = 0;
}

$("iso").addEventListener("change", (event) => {
	const file = event.target.files[0];
	if (file) importIso(file);
	event.target.value = "";
});
$("replace").addEventListener("click", () => {
	$("pick").hidden = false;
});
$("forget").addEventListener("click", () => forget());

// for testing from the console: halo.importIso(blob)
window.halo = { importIso, forget, readManifest };

if (!navigator.storage?.getDirectory) {
	log("error: this browser has no origin private file system; use a recent Chrome, Edge or Firefox");
	$("iso").disabled = true;
} else {
	showStored();
}
