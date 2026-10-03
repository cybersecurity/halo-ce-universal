#!/usr/bin/env python3
"""Package a built iPhone/iPad or Apple TV app and write its SHA-256 checksum."""
import argparse
import hashlib
from pathlib import Path
import plistlib
import subprocess
import zipfile


def package(app, output, require_unsigned=False):
    app = app.resolve()
    with (app/'Info.plist').open('rb') as file:
        info = plistlib.load(file)
    if info.get('CFBundleSupportedPlatforms') not in (['iPhoneOS'], ['AppleTVOS']):
        raise ValueError('Only an iPhoneOS or AppleTVOS device app can be packaged as an IPA')
    if info.get('CFBundleExecutable') != 'HaloCE' or not (app/'HaloCE').is_file():
        raise ValueError('Not a complete HaloCE.app')
    files = sorted(p for p in app.rglob('*') if p.is_file())
    forbidden = {'.map', '.iso', '.xiso', '.p12', '.p8'}
    if any(p.suffix.lower() in forbidden or p.is_symlink() for p in files):
        raise ValueError('Refusing to package game data, private keys, or symlinks')
    if require_unsigned:
        if any(p.suffix == '.mobileprovision' or '_CodeSignature' in p.parts for p in files):
            raise ValueError('Public unsigned packages must not contain signing material')
        signature = subprocess.run(['codesign', '-d', str(app)], capture_output=True, text=True)
        if signature.returncode == 0:
            raise ValueError('Expected an unsigned app, but codesign found a signature')
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
        for path in files:
            archive.write(path, Path('Payload')/app.name/path.relative_to(app))
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    output.with_suffix(output.suffix+'.sha256').write_text(f'{digest}  {output.name}\n')
    print(f'IPA: {output}\nSHA-256: {digest}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('app', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--require-unsigned', action='store_true')
    args = parser.parse_args()
    package(args.app, args.output, args.require_unsigned)
