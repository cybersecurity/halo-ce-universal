#!/usr/bin/env python3
"""Build the iPhone/iPad app from source on an Apple Silicon Mac."""
import argparse
import os
import platform
from pathlib import Path
import re
import shutil
import subprocess
import sys
import urllib.request
import ios_angle

ROOT = Path(__file__).resolve().parents[1]
GL_REVISION = '1cdd228e34966dd6b95bd203e9f84faba0f371a1'
EGL_REVISION = 'db3425b8246136faccb5e2782b5694960bd6edf1'
HEADERS = {
    'EGL/egl.h': ('EGL-Registry', EGL_REVISION, 'api/EGL/egl.h'),
    'EGL/eglext.h': ('EGL-Registry', EGL_REVISION, 'api/EGL/eglext.h'),
    'EGL/eglplatform.h': ('EGL-Registry', EGL_REVISION, 'api/EGL/eglplatform.h'),
    'GLES3/gl32.h': ('OpenGL-Registry', GL_REVISION, 'api/GLES3/gl32.h'),
    'GLES3/gl3platform.h': ('OpenGL-Registry', GL_REVISION, 'api/GLES3/gl3platform.h'),
    'GLES2/gl2platform.h': ('OpenGL-Registry', GL_REVISION, 'api/GLES2/gl2platform.h'),
    'GLES2/gl2ext.h': ('OpenGL-Registry', GL_REVISION, 'api/GLES2/gl2ext.h'),
    'KHR/khrplatform.h': ('EGL-Registry', EGL_REVISION, 'api/KHR/khrplatform.h'),
}

def run(*args):
    print('+', ' '.join(str(a) for a in args), flush=True)
    subprocess.run([str(a) for a in args], cwd=ROOT, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--simulator', action='store_true', help='build for an ARM64 simulator')
    mode.add_argument('--unsigned', action='store_true', help='build a device app for signing later')
    parser.add_argument('--team', help='Apple development team ID for device signing')
    parser.add_argument('--bundle-id', help='bundle identifier covered by your signing profile (default org.haloce.ios)')
    parser.add_argument('--ipa', type=Path, help='also package the device app at this path')
    parser.add_argument('--llvm', default='/opt/homebrew/opt/llvm')
    parser.add_argument('--lld', default='/opt/homebrew/opt/lld/bin/ld.lld')
    parser.add_argument('--jobs', type=int, default=min(12,os.cpu_count() or 4))
    args = parser.parse_args()
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        parser.error('an Apple Silicon Mac with full Xcode is required')
    if not args.simulator and not args.unsigned and not args.team:
        parser.error('use --team YOUR_TEAM_ID to sign, or --unsigned to sign later')
    if args.team and (args.unsigned or args.simulator):
        parser.error('--team is only used for signed device builds')
    if args.ipa and args.simulator:
        parser.error('--ipa requires a device build')
    args.bundle_id = args.bundle_id or 'org.haloce.ios'
    if not re.fullmatch(r'[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)+', args.bundle_id):
        parser.error('--bundle-id must be a reverse-DNS identifier (e.g. com.example.halo)')
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    ios_angle.prepare()
    include=ROOT/'build/ios/gl_include'
    for name,(registry,revision,source) in HEADERS.items():
        target=include/name;target.parent.mkdir(parents=True,exist_ok=True)
        url=f'https://raw.githubusercontent.com/KhronosGroup/{registry}/{revision}/{source}'
        data=urllib.request.urlopen(url,timeout=30).read()
        if not target.exists() or target.read_bytes()!=data:target.write_bytes(data)
    run(sys.executable,'configure.py','--ios','--pgo=off','--lto=off','--ios-llvm',args.llvm,'--ios-lld',args.lld)
    run('ninja','ios_guest','-j',args.jobs)
    run(sys.executable,'tools/ios_bridges.py')
    run(sys.executable,'tools/ios_embed_guest.py','build/ios/halo_guest.elf','build/ios/embedded')
    notices = ROOT/'build/ios/licenses'
    notices.mkdir(parents=True, exist_ok=True)
    for source, name in (
        ('LICENSE.md', 'Project-CC0.txt'),
        ('port/ios/THIRD_PARTY.md', 'NOTICE.txt'),
        ('build/third_party/SDL3/LICENSE.txt', 'SDL.txt'),
        ('build/third_party/musl-1.2.5/COPYRIGHT', 'musl.txt'),
        ('port/third_party/kcp/LICENSE', 'kcp.txt'),
        ('port/third_party/extract-xiso/LICENSE.TXT', 'extract-xiso.txt'),
        ('port/third_party/tomlc17/LICENSE', 'tomlc17.txt'),
    ):
        shutil.copyfile(ROOT/source, notices/name)
    build=ROOT/'build'/'ios'/('app-simulator' if args.simulator else
                'app-unsigned' if args.unsigned else 'app-device')
    sdk='iphonesimulator' if args.simulator else 'iphoneos'
    command=['cmake','-S','port/ios','-B',build,'-G','Xcode','-DCMAKE_SYSTEM_NAME=iOS',
             f'-DCMAKE_OSX_SYSROOT={sdk}','-DCMAKE_OSX_ARCHITECTURES=arm64','-DCMAKE_OSX_DEPLOYMENT_TARGET=16.0',
             f'-DHALO_BUNDLE_IDENTIFIER={args.bundle_id}', f'-DHALO_DEVELOPMENT_TEAM={args.team or ""}']
    run(*command)
    command=['cmake','--build',build,'--config','Release','--target','HaloCE','--','-quiet']
    if args.simulator or args.unsigned:command.append('CODE_SIGNING_ALLOWED=NO')
    else:command.append('-allowProvisioningUpdates')
    run(*command)
    app = build/f'Release-{sdk}/HaloCE.app'
    print(f'App: {app}')
    if args.unsigned:
        # Public unsigned IPAs must not retain the prebuilt vendor's signatures.
        # Operate only on the embedded build copies, never the verified archive.
        for framework in (app/'Frameworks').glob('*.framework'):
            run('codesign','--remove-signature',framework)
            shutil.rmtree(framework/'_CodeSignature', ignore_errors=True)
    if args.ipa:
        command = [sys.executable, 'tools/ios_package.py', app, args.ipa.resolve()]
        if args.unsigned: command.append('--require-unsigned')
        run(*command)

if __name__=='__main__':main()
