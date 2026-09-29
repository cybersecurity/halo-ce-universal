#!/usr/bin/env python3
"""Reproduce a silent archive/guest alias collision and verify the build guard."""
import subprocess
import sys
import tempfile
from pathlib import Path
from check_macos_symbol_namespaces import native_definitions, native_references


def main():
    checker = Path(__file__).with_name('check_macos_symbol_namespaces.py')
    # Explicit platform parser tests, including weak and undefined symbols.
    assert native_definitions('0000 T _helper\n U _missing\n0000 W _weak\n', True) == {'helper', 'weak'}
    assert native_definitions('0000 T helper\n U missing\n w weak_missing\n0000 W weak\n', False) == {'helper', 'weak'}
    assert native_references(' U _puts\n w _weak_missing\n0000 T _local\n', True) == {'puts', 'weak_missing'}
    with tempfile.TemporaryDirectory(prefix='halo-symbol-test-') as temporary:
        root = Path(temporary)
        lift = root / 'lift'
        lift.mkdir()
        (lift / 'recomp_import_aliases.c').write_text('void halo_copy(void) { }\n')
        (root / 'guest.c').write_text('void halo_copy(void) { }\nvoid adapter(void); int main(void) { adapter(); return 0; }\n')
        (root / 'adapter.c').write_text('void halo_copy(void); void adapter(void) { halo_copy(); }\n')
        (root / 'helper.c').write_text('void halo_copy(void) { }\n')
        for source in ('adapter', 'helper'):
            subprocess.run(['cc', '-c', str(root / f'{source}.c'), '-o', str(root / f'{source}.o')], check=True)
        archive = root / 'native.a'
        subprocess.run(['ar', 'rcs', str(archive), str(root / 'adapter.o'), str(root / 'helper.o')], check=True)
        # Normal ld succeeds, leaving the helper archive member unselected.
        subprocess.run(['cc', str(root / 'guest.c'), str(archive), '-o', str(root / 'silent-collision')], check=True)
        command = [sys.executable, str(checker), '--lift-dir', str(lift), '--archive', str(archive)]
        failure = subprocess.run(command, text=True, capture_output=True)
        assert failure.returncode == 1 and 'halo_copy' in failure.stderr, failure
        (root / 'helper.c').write_text('void mac_copy(void) { }\n')
        (root / 'adapter.c').write_text('void mac_copy(void); void adapter(void) { mac_copy(); }\n')
        for source in ('adapter', 'helper'):
            subprocess.run(['cc', '-c', str(root / f'{source}.c'), '-o', str(root / f'{source}.o')], check=True)
        subprocess.run(['ar', 'rcs', str(archive), str(root / 'adapter.o'), str(root / 'helper.o')], check=True)
        subprocess.run(['cc', str(root / 'guest.c'), str(archive), '-o', str(root / 'namespaced')], check=True)
        subprocess.run(command, check=True)
        # A native undefined reference can instead be shadowed by a guest
        # alias of a dylib API, even when no archive defines that name.
        (lift / 'recomp_import_aliases.c').write_text('void halo_copy(void) { }\nvoid puts(void) { }\n')
        (root / 'adapter.c').write_text('#include <stdio.h>\nvoid adapter(void) { puts("native"); }\n')
        subprocess.run(['cc', '-c', str(root / 'adapter.c'), '-o', str(root / 'adapter.o')], check=True)
        subprocess.run(['ar', 'rcs', str(archive), str(root / 'adapter.o'), str(root / 'helper.o')], check=True)
        failure = subprocess.run(command, text=True, capture_output=True)
        assert failure.returncode == 1 and 'puts' in failure.stderr, failure
    print('Silent archive shadowing regression PASS: original link succeeds but guard rejects; namespaced helper passes')


if __name__ == '__main__':
    main()
