#!/usr/bin/env python3
"""Compile the native SDL token/window regression against real SDL3.
The fixture opens a hidden window; it does not launch the game. Logging and
application activation leaves are isolated, while SDL and guest spans are real.
"""
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    flags = shlex.split(subprocess.check_output(
        ['pkg-config', '--cflags', '--libs', 'sdl3'], text=True))
    with tempfile.TemporaryDirectory(prefix='halo-sdl-window-') as temporary:
        support = Path(temporary) / 'support.c'
        executable = Path(temporary) / 'test'
        support.write_text('''void mac_host_log(int p,const char*s){(void)p;(void)s;}
void mac_host_logf(int p,const char*s,...){(void)p;(void)s;}
void mac_host_apply_app_icon(void){}
void mac_host_activate_app(void){}
void mac_game_evidence_presented(void){}
''')
        native = ROOT / 'port/macos'
        subprocess.run([
            'clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-UNDEBUG',
            '-I' + str(native), str(native/'tests/host_sdl_test.c'),
            str(native/'host_sdl.c'), str(native/'guest_address.c'),
            str(native/'guest_call.c'), str(support), '-Wl,-dead_strip',
            '-framework', 'OpenGL', *flags, '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
