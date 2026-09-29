#!/usr/bin/env python3
"""Read user-owned Xbox XDVDFS images; copy only Halo cache/movie data."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct

SECTOR = 2048
MAGIC = b'MICROSOFT*XBOX*MEDIA'
BASES = (0, 0x0FD90000, 0x02080000, 0x18300000)
REQUIRED_MAPS = set('a10 a30 a50 b30 b40 beavercreek bloodgulch boardingaction c10 c20 c40 carousel chillout d20 d40 damnation hangemhigh longest prisoner putput ratrace sidewinder ui wizard'.split())
REQUIRED_MOVIES = set('attract1 attract2 attract3 credits intro'.split())
BUILDS = {b'01.10.12.2276': 'NTSC retail', b'01.01.14.2342': 'PAL retail'}

class ImageError(ValueError):
    pass

class Image:
    def __init__(self, path):
        self.file = open(path, 'rb')
        self.size = os.fstat(self.file.fileno()).st_size
    def __del__(self):
        if hasattr(self, "file"):
            self.file.close()
    def read(self, offset, length):
        if offset < 0 or length < 0 or offset + length > self.size:
            raise ImageError('Truncated image or invalid file extent')
        self.file.seek(offset)
        data = self.file.read(length)
        if len(data) != length:
            raise ImageError('Truncated image')
        return data

def inventory(image):
    base = None
    for candidate in BASES:
        offset = candidate + 32 * SECTOR
        if offset + SECTOR <= image.size:
            descriptor = image.read(offset, SECTOR)
            if descriptor[:20] == MAGIC and descriptor[-20:] == MAGIC:
                base = candidate
                break
    if base is None:
        if image.size > 17 * SECTOR and image.read(16 * SECTOR + 1, 5) == b'CD001':
            raise ImageError('PC/Mac ISO9660 installer detected. This port requires retail Xbox Halo cache version 5; PC/Custom Edition/MCC assets are incompatible.')
        raise ImageError('No Xbox XDVDFS game partition found. Supply a decrypted Xbox XISO or Redump disc image, not an archive, XBE, installer, or encrypted dump.')
    sector, size = struct.unpack_from('<II', descriptor, 20)
    files = {}
    seen_dirs = set()
    def directory(start, count, prefix, depth):
        if depth > 32 or count > 16 * 1024 * 1024 or count < 14:
            raise ImageError('Invalid directory size/depth')
        key = (start, count)
        if key in seen_dirs:
            raise ImageError('Repeated/cyclic directory extent')
        seen_dirs.add(key)
        table = image.read(base + start * SECTOR, count)
        visited = set()
        pending = [0]
        while pending:
            offset = pending.pop()
            if offset in visited or offset + 14 > len(table):
                raise ImageError('Cyclic directory tree or invalid entry offset')
            visited.add(offset)
            left, right, data_sector, data_size, attrs, nlen = struct.unpack_from('<HHIIBB', table, offset)
            if not nlen or offset + 14 + nlen > len(table):
                raise ImageError('Invalid directory filename length')
            raw = table[offset+14:offset+14+nlen]
            try:
                name = raw.decode('ascii')
            except UnicodeDecodeError:
                raise ImageError('Non-ASCII Xbox filename')
            if name in ('.', '..') or any(c in name for c in '/\\\x00:') or any(ord(c) < 32 or ord(c) == 127 for c in name):
                raise ImageError('Unsafe Xbox filename')
            path = '/'.join((*prefix, name)).lower()
            if path in files:
                raise ImageError('Duplicate/case-colliding Xbox path')
            if base + data_sector * SECTOR + data_size > image.size:
                raise ImageError('File extent exceeds image')
            files[path] = (base + data_sector * SECTOR, data_size, bool(attrs & 0x10))
            if attrs & 0x10:
                directory(data_sector, data_size, (*prefix, name), depth+1)
            if left:
                pending.append(left * 4)
            if right:
                pending.append(right * 4)
    directory(sector, size, (), 0)
    return base, files

def selected_assets(image, files):
    selected = {}
    builds = set()
    for name, (offset, size, isdir) in files.items():
        parts = name.split('/')
        if isdir or len(parts) != 2:
            continue
        kind, leaf = parts
        if kind in ('maps', 'maps_de', 'maps_fr', 'maps_es', 'maps_it') and leaf.endswith('.map'):
            if size < 2048:
                raise ImageError('Halo map header is truncated: ' + name)
            header = image.read(offset, 2048)
            # Xbox uses reversed four-character constants on little-endian hosts.
            if header[:4] != b'daeh' or struct.unpack_from('<I', header, 4)[0] != 5:
                raise ImageError('Unsupported cache format in ' + name + '; Xbox cache version 5 required')
            build = header[64:96].split(b'\x00', 1)[0]
            if build not in BUILDS:
                raise ImageError('Unsupported Halo map build: ' + repr(build))
            builds.add(build.decode())
        elif kind == 'bink' and leaf.endswith('.bik'):
            if size < 8 or not image.read(offset, 3) == b'BIK':
                raise ImageError('Invalid Bink movie: ' + name)
        else:
            continue
        selected[name] = (offset, size)
    if 'maps/ui.map' not in selected or 'maps/a10.map' not in selected or not any(n.startswith('bink/') for n in selected):
        raise ImageError('Image lacks Halo retail maps/ui.map, maps/a10.map, or Bink movies')
    # Recognized retail cache builds are normalized by the engine per map.
    return selected, sorted(builds)

def require_complete(selected):
    required = {'maps/'+n+'.map' for n in REQUIRED_MAPS} | {'bink/'+n+'.bik' for n in REQUIRED_MOVIES}
    missing = required - set(selected)
    if missing:
        raise ImageError('Incomplete Halo retail data; missing: '+', '.join(sorted(missing)))

def inspect_iso(path):
    image = Image(path)
    base, files = inventory(image)
    selected, builds = selected_assets(image, files)
    require_complete(selected)
    return {'image_bytes': image.size, 'partition_offset': base, 'map_builds': builds, 'asset_count': len(selected)}


def extract(path, output):
    image = Image(path)
    base, files = inventory(image)
    selected, builds = selected_assets(image, files)
    require_complete(selected)
    output = Path(output)
    if output.exists():
        raise ImageError('Extraction destination already exists; use a fresh directory')
    output.mkdir(parents=True)
    manifest = {'image_bytes': image.size, 'partition_offset': base, 'map_builds': builds, 'assets': []}
    try:
        for name, (offset, length) in sorted(selected.items()):
            dest = output / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            digest = hashlib.sha256()
            with dest.open('xb') as sink:
                for pos in range(0, length, 1024*1024):
                    data = image.read(offset+pos, min(1024*1024, length-pos))
                    digest.update(data)
                    sink.write(data)
            manifest['assets'].append({'path': name, 'bytes': length, 'sha256': digest.hexdigest()})
        digest = hashlib.sha256()
        with open(path, 'rb') as source:
            for data in iter(lambda: source.read(8*1024*1024), b''):
                digest.update(data)
        manifest['image_sha256'] = digest.hexdigest()
        (output / 'extraction-provenance.json').write_text(json.dumps(manifest, indent=2)+'\n')
    except BaseException:
        import shutil
        shutil.rmtree(output)
        raise
    return manifest

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('iso', type=Path)
    parser.add_argument('output', type=Path, nargs='?')
    parser.add_argument('--inspect', action='store_true', help='Validate directory/cache headers without copying or hashing the full image')
    args = parser.parse_args()
    try:
        if args.inspect:
            result = inspect_iso(args.iso)
        elif args.output is not None:
            result = extract(args.iso, args.output)
        else:
            parser.error('output directory required unless --inspect')
        print(json.dumps(result, indent=2))
    except (ImageError, OSError) as error:
        parser.exit(1, 'Asset extraction failed: '+str(error)+'\n')
