#!/usr/bin/env python3
"""Run native ILP32, memory-tracking and SDL audio handoff regressions."""
import shlex
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/ios/probe'
BUILD.mkdir(parents=True, exist_ok=True)

def run(*args):
    subprocess.run([str(a) for a in args], cwd=ROOT, check=True)

clang = '/opt/homebrew/opt/llvm/bin/clang'
lld = '/opt/homebrew/opt/lld/bin/ld.lld'
run(clang, '--target=arm64_32-apple-watchos', '-mcpu=cortex-a53', '-ffixed-x15', '-ffixed-x27',
    '-mllvm', '-aarch64-enable-compress-jump-tables=false', '-fno-stack-protector', '-ffreestanding',
    '-O2', '-S', 'port/ios/tests/guest_probe.c', '-o', BUILD/'probe.darwin.s')
run('python3', 'tools/ios_asm_convert.py', BUILD/'probe.darwin.s', BUILD/'probe.s')
run(clang, '--target=aarch64-none-elf', '-c', BUILD/'probe.s', '-o', BUILD/'probe.o')
run(lld, '-m', 'aarch64linux', '-static', '-nostdlib', '-T', 'port/ios/guest.ld', BUILD/'probe.o', '-o', BUILD/'probe.elf')
run('python3', 'tools/ios_embed_guest.py', BUILD/'probe.elf', BUILD/'embed')
run('xcrun','clang','-O2',f'-I{BUILD}/embed','port/ios/tests/probe_runner.c',
    'port/ios/host/guest_call.S',BUILD/'embed/guest_image.S','-o',BUILD/'runner')
run(BUILD/'runner')
run('xcrun','clang','-O2','-Iport/ios/host','-Iport/ios/host','-Iport/runtime/include',
    '-Iport/runtime/guest/runtime','port/ios/tests/memory_probe.c','port/ios/host/host_memory.c','-o',BUILD/'memory-probe')
run(BUILD/'memory-probe')
sdl_flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'sdl3'], text=True))
run('xcrun', 'clang', '-O2', '-DHALO_IOS=1', '-Iport/ios/host', '-Iport/ios/host',
    '-Iport/runtime/include', 'port/ios/tests/audio_probe.c', '-Wl,-dead_strip',
    *sdl_flags, '-o', BUILD/'audio-probe')
run(BUILD/'audio-probe')

run('xcrun', 'clang', '-O2', '-fsanitize=address,undefined', '-Iport/linux/src',
    'port/ios/tests/display_probe.c', '-o', BUILD/'display-probe')
run(BUILD/'display-probe')

# Parse untrusted XISO metadata and exercise extraction/cancellation under sanitizers.
run('python3', 'tools/ios_xiso_test.py')

# Bounded native/WebKit packet transport with real virtual sockets.
run('xcrun', 'clang', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
    '-fsanitize=address,undefined', '-Iport/linux/src', '-Iport/web/src',
    'port/ios/network/room_bridge.c', 'port/web/src/web_net.c',
    'port/ios/tests/room_bridge_probe.c', '-o', BUILD/'room-bridge')
run(BUILD/'room-bridge')

# Private compiled-shader cache: bounds, cross-launch reload and concurrency.
run('xcrun', 'clang', '-fobjc-arc', '-Wno-incompatible-pointer-types',
    '-Ibuild/ios/gl_include', '-Iport/ios/host', '-Iport/runtime/include',
    '-framework', 'Foundation', '-framework', 'Metal',
    'port/ios/tests/shader_cache_probe.m', '-o', BUILD/'shader-cache-probe')
run(BUILD/'shader-cache-probe')

# Actual Darwin TCP/UDP via the native/game ABI, alongside the WebRTC transport.
run('xcrun','clang','-O1','-g','-DHALO_IOS=1','-fsanitize=address,undefined',
    '-Iport/linux/src','-Iport/ios/network','-Iport/web/src',
    'port/ios/tests/native_net_probe.c','port/ios/host/posix_net.c',
    'port/ios/network/host_net.c','port/ios/network/host_web_net.c',
    'port/ios/network/room_bridge.c','-o',BUILD/'native-net-probe')
run(BUILD/'native-net-probe')

run('xcrun','clang','-O1','-g','-DHALO_IOS_BROWSER=1','-fsanitize=address,undefined',
    '-Itools','-Isource','tools/test_quick_play.c','-o',BUILD/'quick-play-probe')
run(BUILD/'quick-play-probe')
