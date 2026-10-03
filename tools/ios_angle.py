#!/usr/bin/env python3
"""Fetch the pinned, hash-verified ANGLE Metal frameworks used by the iOS host."""
import hashlib
import base64
from pathlib import Path
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
REVISION = 'c053bf85793bbb83016b1196d04e5df3594b9bcc'
VERSION = 'v2.1.28252'
SHA256 = '59e4b1f68956c92441cde4dca0e9eb1a835bbccd107cefdd1d3d3d60e27410be'

def prepare():
    directory = ROOT / 'build/third_party/angle'
    directory.mkdir(parents=True, exist_ok=True)
    archive = directory / 'package.zip'
    if not archive.exists():
        data = urllib.request.urlopen(f'https://github.com/EdgeFirstAI/angle-package/releases/download/{VERSION}/angle-xcframeworks-{VERSION}.zip', timeout=60).read()
        if hashlib.sha256(data).hexdigest() != SHA256:
            raise RuntimeError('ANGLE download checksum mismatch')
        archive.write_bytes(data)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise RuntimeError('Cached ANGLE archive checksum mismatch; remove package.zip and retry')
    with zipfile.ZipFile(archive) as package:
        for info in package.infolist():
            if info.is_dir() or any(p.startswith('._') for p in Path(info.filename).parts):
                continue
            if '/ios-arm64' not in info.filename and info.filename != 'dist/BUILD_INFO.txt':
                continue
            target = directory / info.filename
            if not target.resolve().is_relative_to(directory.resolve()):
                raise RuntimeError('Invalid ANGLE archive path')
            target.parent.mkdir(parents=True, exist_ok=True)
            data = package.read(info)
            if not target.exists() or target.read_bytes() != data:
                target.write_bytes(data)
                target.chmod((info.external_attr >> 16) & 0o777 or 0o644)
    stamp=directory/'source-revision'
    refresh=not stamp.exists() or stamp.read_text().strip()!=REVISION
    # Include upstream notices for the translator and bundled dependencies.
    notices = ROOT/'build/ios/licenses'
    notices.mkdir(parents=True, exist_ok=True)
    embedded = ['src/common/third_party/xxhash/LICENSE', 'src/third_party/ceval/LICENSE',
                'third_party/glslang/LICENSE', 'third_party/spirv-headers/LICENSE',
                'third_party/spirv-tools/LICENSE']
    for source in embedded:
        target=notices/('ANGLE-'+source.split('/')[-2]+'.txt')
        if refresh or not target.exists():
            target.write_bytes(urllib.request.urlopen(f'https://raw.githubusercontent.com/google/angle/{REVISION}/{source}', timeout=30).read())
    external = [
        ('SPIRV-Cross','external/github.com/KhronosGroup/SPIRV-Cross','b8fcf307f1f347089e3c46eb4451d27f32ebc8d3','LICENSE'),
        ('abseil','chromium/src/third_party/abseil-cpp','d8e483edd8b44da1845874ee84b42489589bb90f','LICENSE'),
        ('zlib','chromium/src/third_party/zlib','8b3aa8a1cd7585c0c4c67351481227b046a662a0','LICENSE'),
    ]
    for name, repository, revision, source in external:
        target=notices/f'ANGLE-{name}.txt'
        if refresh or not target.exists():
            data=urllib.request.urlopen(f'https://chromium.googlesource.com/{repository}/+/{revision}/{source}?format=TEXT', timeout=30).read()
            target.write_bytes(base64.b64decode(data))
    # Fetch public headers and the upstream license at the exact binary source revision.
    for source, target in [('include/EGL/eglext_angle.h', ROOT/'build/ios/gl_include/EGL/eglext_angle.h'), ('LICENSE', ROOT/'build/ios/licenses/ANGLE.txt')]:
        target.parent.mkdir(parents=True, exist_ok=True)
        if refresh or not target.exists():
            target.write_bytes(urllib.request.urlopen(f'https://raw.githubusercontent.com/google/angle/{REVISION}/{source}', timeout=30).read())

    stamp.write_text(REVISION+'\n')

if __name__ == '__main__':
    prepare()
