#!/usr/bin/env python3
"""Package the source-built browser runtime for the game site and its mirror."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[1]


def relay_url(value):
    if not value:
        return value
    parsed = urlsplit(value)
    if (parsed.scheme != 'wss' and not (parsed.scheme == 'ws' and parsed.hostname in ('localhost', '127.0.0.1', '::1'))
            or not parsed.hostname or parsed.username or parsed.password or parsed.query or parsed.fragment
            or parsed.path != '/join'):
        raise argparse.ArgumentTypeError('Use wss://relay-host/join (ws://loopback/join for local testing).')
    return value


def room_code(value):
    value = value.strip().upper()
    if value and not re.fullmatch(r'[A-Z0-9]{4,16}', value):
        raise argparse.ArgumentTypeError('Use 4–16 letters or digits, or an empty value to disable the default room.')
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--relay', type=relay_url, default='', help='Native multiplayer WSS endpoint; blank leaves desktop joining unavailable')
    parser.add_argument('--default-room', type=room_code, default='FQLX01', help='Shared browser room joined on a first visit; empty disables automatic room entry')
    args = parser.parse_args()
    native_version = re.search(r'^#define HALO_PORT_NETWORK_VERSION (\d+)$',
                              (ROOT / 'port/linux/include/halo_port_limits.h').read_text(), re.MULTILINE)
    if not native_version:
        parser.error('Missing native network version in halo_port_limits.h.')
    source = ROOT / 'build/web/site'
    output = ROOT / 'dist/browser-multiplayer'
    names = [path.relative_to(ROOT / 'port/web/site') for path in (ROOT / 'port/web/site').rglob('*') if path.is_file()]
    names += [Path('halo.js'), Path('halo.wasm')]
    for name in names:
        if not (source / name).is_file():
            parser.error(f'Missing {name}; run python3 configure.py --release and ninja web with Emscripten 6.0.10 first.')
    expected = set(names) | {Path('version.json'), Path('deployment.json'), Path('.nojekyll')}
    if output.exists():
        unexpected = {path.relative_to(output) for path in output.rglob('*') if path.is_file()} - expected
        if unexpected:
            parser.error(f'Refusing to overwrite unexpected files in {output}: {sorted(map(str, unexpected))}')
    output.mkdir(parents=True, exist_ok=True)
    for name in names:
        (output / name).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source / name, output / name)
    (output / 'site-config.js').write_text('window.HALO_BROWSER_CONFIG = Object.freeze(' + json.dumps({'relayUrl': args.relay, 'defaultRoom': args.default_room}) + ');\n')
    # Connection configuration participates in the offline cache version.
    subprocess.run(['python3', str(ROOT / 'port/web/stamp_version.py'), str(output / 'version.json'),
                    *map(str, sorted(path for path in output.rglob('*') if path.is_file() and path.name not in {'version.json', 'deployment.json', '.nojekyll'}))], check=True)
    (output / '.nojekyll').touch()
    receipt = {
        'source_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
        'source_dirty': bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT, text=True).strip()),
        'runtime': 'source-built Emscripten 6.0.10',
        'native_network_version': int(native_version.group(1)),
        'relay_configured': bool(args.relay),
        'default_browser_room': args.default_room,
        'game_assets_included': False,
        'files': {str(path.relative_to(output)): {'bytes': path.stat().st_size,
                  'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
                  for path in sorted(output.rglob('*')) if path.is_file() and path.name != 'deployment.json'},
    }
    (output / 'deployment.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(output)


if __name__ == '__main__':
    main()
