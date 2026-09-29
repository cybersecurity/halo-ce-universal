#!/usr/bin/env python3
"""Reject native archive definitions that can be shadowed by generated guest code.

A static archive member need not be linked when a generated guest alias already
satisfies its symbol. Calls from a native adapter can then recursively re-enter
that adapter. Check archives before linking, including members not selected by
ld; executable-only nm cannot detect these omitted native implementations.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path


def guest_definitions(directory):
    result = set()
    for path in directory.glob('recomp_*.c'):
        text = path.read_text()
        result.update(re.findall(r'^\s*void\s+(\w+)\s*\(void\)\s*\{', text, re.M))
    if not result:
        raise ValueError(f'no generated guest function definitions in {directory}')
    return result


def native_definitions(output, mach_o):
    result = set()
    for line in output.splitlines():
        # Both Darwin and GNU nm use: [address] <type> <symbol>. Ignore
        # undefined symbols (U and weak undefined w/v), archive headers/local
        # symbols, but include weak definitions W/V that can also be shadowed.
        match = re.search(r'(?:^|\s)([A-Z])\s+(\S+)\s*$', line)
        if not match or match[1] == 'U':
            continue
        symbol = match[2]
        if mach_o and symbol.startswith('_'):
            symbol = symbol[1:]
        result.add(symbol)
    return result


def native_references(output, mach_o):
    """External calls must not bind to a generated guest ABI entry point."""
    result = set()
    for line in output.splitlines():
        match = re.search(r'(?:^|\s)[Uwv]\s+(\S+)\s*$', line)
        if match:
            symbol = match[1]
            if mach_o and symbol.startswith('_'):
                symbol = symbol[1:]
            result.add(symbol)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lift-dir', type=Path, required=True)
    parser.add_argument('--archive', type=Path, action='append', required=True)
    parser.add_argument('--nm', default='nm')
    args = parser.parse_args()
    guest = guest_definitions(args.lift_dir)
    total = set()
    references = set()
    collisions = []
    for archive in args.archive:
        output = subprocess.check_output([args.nm, '-g', str(archive)], text=True)
        native = native_definitions(output, sys.platform == 'darwin')
        external = native_references(output, sys.platform == 'darwin')
        total.update(native)
        references.update(external)
        collisions.extend((symbol, archive) for symbol in sorted((native | external) & guest))
    if collisions:
        for symbol, archive in collisions:
            print(f'ERROR: guest/native symbol collision: {symbol} in {archive}', file=sys.stderr)
        print('Rename the native implementation (mac_*); keep guest ABI aliases unchanged.', file=sys.stderr)
        return 1
    print(f'Guest/native symbol namespace PASS: {len(guest)} guest definitions, '
          f'{len(total)} archive definitions, {len(references)} external references, '
          f'{len(args.archive)} archives')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
