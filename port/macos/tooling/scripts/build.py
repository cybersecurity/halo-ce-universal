#!/usr/bin/env python3
"""Fetch pinned sources and build entirely inside an isolated working directory."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]

def run(*args, cwd=None, env=None):
    print('+', ' '.join(map(str,args)), flush=True)
    subprocess.run(list(map(str,args)), cwd=cwd, env=env, check=True, stdin=subprocess.DEVNULL)

def sha(path):
    digest=hashlib.sha256()
    with Path(path).open('rb') as source:
        for data in iter(lambda:source.read(1024*1024),b''):digest.update(data)
    return digest.hexdigest()

def checkout(spec, dest):
    if dest.exists():
        raise ValueError('Source directory already exists; choose a fresh --work-dir: '+str(dest))
    run('git','init','-q',dest)
    run('git','-C',dest,'fetch','--depth=1',spec['url'],spec['commit'])
    run('git','-C',dest,'checkout','--detach','FETCH_HEAD')
    got=subprocess.check_output(['git','-C',str(dest),'rev-parse','HEAD'],text=True).strip()
    if got != spec['commit']:raise ValueError('Fetched commit mismatch')

def snapshot_checkout(checkout_path, destination):
    """Copy indexed current bytes, retaining working/staged edits without mutating Git."""
    checkout_path=Path(checkout_path).expanduser().resolve()
    destination=Path(destination).resolve()
    if destination.exists() or destination.is_relative_to(checkout_path):
        raise ValueError('Checkout snapshot needs a fresh destination outside the source checkout')
    prefix=subprocess.check_output(['git','-C',str(checkout_path),'rev-parse','--show-toplevel'],text=True).strip()
    if Path(prefix).resolve()!=checkout_path:
        raise ValueError('--source-checkout must name the repository root')
    commit=subprocess.check_output(['git','-C',str(checkout_path),'rev-parse','HEAD'],text=True).strip()
    names=subprocess.check_output(['git','-C',str(checkout_path),'ls-files','--cached','-z']).decode().split('\0')
    ignored_parts={'.git','build','assets','__pycache__','venv','.work','node_modules'}
    excluded_suffixes={'.iso','.xiso','.map','.bik','.xmv','.xbe','.elf','.o','.a','.dylib','.so','.exe','.dll','.obj','.lib','.pyc'}
    inventory={}
    for name in sorted(set(names)-{''}):
        relative=Path(name)
        if relative.is_absolute() or '..' in relative.parts:
            raise ValueError('Unsafe indexed source path')
        if ignored_parts.intersection(relative.parts) or any(part.endswith('.app') for part in relative.parts) or relative.suffix.lower() in excluded_suffixes:
            continue
        if relative.parts[:2]==('tools','dependencies'):
            continue
        source=checkout_path/relative
        if source.is_symlink():
            raise ValueError('Source snapshot requires regular indexed files: '+name)
        if not source.exists():
            continue # A working-tree deletion must remain deleted in the snapshot.
        if not source.is_file():
            raise ValueError('Indexed source is not a regular file: '+name)
        inventory[name]=sha(source)
    required={'port/macos/CMakeLists.txt','tools/macos_aot_build.py','validation/sourceport-lp64/batch_compile_guest.py'}
    if not required.issubset(inventory):
        raise ValueError('Current checkout needs the integrated Mac target; stage newly added source files before using --source-checkout')
    destination.mkdir(parents=True)
    for name,expected in inventory.items():
        target=destination/name;target.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(checkout_path/name,target)
        if sha(target)!=expected:
            raise ValueError('Source changed while snapshotting: '+name)
    encoded=json.dumps(inventory,sort_keys=True,separators=(',',':')).encode()
    return {'mode':'current-checkout','commit':commit,'files_sha256':inventory,
            'inventory_sha256':hashlib.sha256(encoded).hexdigest(),
            'new_files':'Only indexed files are included; stage new build inputs first.'}


def prepare_halo_source(pins, destination, source_checkout=None):
    if source_checkout is not None:
        # Current-checkout mode never fetches/repatches/replaces Halo sources.
        return snapshot_checkout(source_checkout,destination)
    checkout(pins['halo'],destination)
    run('git','apply',ROOT/'patches/macos-source.patch',cwd=destination)
    shutil.copytree(ROOT/'overlay',destination,dirs_exist_ok=True,ignore=shutil.ignore_patterns('__pycache__','*.pyc'))
    return {'mode':'pinned-upstream','url':pins['halo']['url'],'commit':pins['halo']['commit']}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work-dir',required=True,type=Path)
    parser.add_argument('--assets',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--jobs',type=int,default=4)
    parser.add_argument('--prepare-only',action='store_true')
    parser.add_argument('--source-checkout',type=Path,help='Use current indexed/working source with the Mac target; skip Halo fetch, Mac patch and overlay replacement')
    args=parser.parse_args()
    work=args.work_dir.expanduser().resolve()
    if work.is_relative_to(ROOT):
        raise ValueError('Use a workspace outside the tooling repository so generated game code/data cannot enter its tree')
    assets=args.assets.expanduser().resolve();out=args.output.expanduser().resolve()
    if out.is_relative_to(ROOT):
        raise ValueError('Keep the built app outside the tooling repository')
    if args.source_checkout is not None:
        current=args.source_checkout.expanduser().resolve()
        if work.is_relative_to(current) or out.is_relative_to(current):
            raise ValueError('Keep build workspace and app output outside the source checkout')
    work.mkdir(parents=True,exist_ok=True)
    pins=json.loads((ROOT/'pins.json').read_text())
    tooling_names=['pins.json','patches/xboxrecomp-runtime.patch','scripts/build.py']
    if args.source_checkout is None:
        overlay_manifest=json.loads((ROOT/'overlay-manifest.json').read_text())
        actual_overlay={str(path.relative_to(ROOT)) for path in (ROOT/'overlay').rglob('*')
                        if path.is_file() and '__pycache__' not in path.parts and path.suffix != '.pyc'}
        if actual_overlay != set(overlay_manifest['files_sha256']):
            raise ValueError('Overlay file inventory differs from its reviewed manifest')
        for relative,expected in overlay_manifest['files_sha256'].items():
            if sha(ROOT/relative)!=expected:
                raise ValueError('Overlay manifest mismatch: '+relative)
        tooling_names+=['patches/macos-source.patch','overlay-manifest.json']
    tooling_inputs={name:sha(ROOT/name) for name in tooling_names}
    source=work/'source';runtime=work/'xboxrecomp'
    source_provenance=prepare_halo_source(pins,source,args.source_checkout)
    checkout(pins['xboxrecomp'],runtime)
    (work/'source-provenance.json').write_text(json.dumps(source_provenance,indent=2)+'\n')
    run('git','apply',ROOT/'patches/xboxrecomp-runtime.patch',cwd=runtime)
    deps=source/'build/deps';deps.mkdir(parents=True)
    archive=deps/'musl-1.2.5.tar.gz'
    urllib.request.urlretrieve(pins['musl']['url'],archive)
    if sha(archive)!=pins['musl']['sha256']:raise ValueError('musl download SHA256 mismatch')
    with tarfile.open(archive) as tar:
        # Avoid link/path escape regardless of upstream download authenticity.
        for member in tar.getmembers():
            p=Path(member.name)
            if p.is_absolute() or '..' in p.parts or member.issym() or member.islnk():
                raise ValueError('Unsafe musl archive member')
        tar.extractall(deps)
    musl=deps/'musl-1.2.5';include=deps/'musl-include';(include/'bits').mkdir(parents=True)
    with (include/'bits/alltypes.h').open('wb') as sink:
        subprocess.run(['sed','-f',str(musl/'tools/mkalltypes.sed'),str(musl/'arch/i386/bits/alltypes.h.in'),str(musl/'include/alltypes.h.in')],stdout=sink,check=True)
    # Platform sources use the same interposed syscall numbers as libc.
    import importlib.util
    spec=importlib.util.spec_from_file_location('guest_compile',source/'validation/sourceport-lp64/batch_compile_guest.py')
    helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
    helper.generate_guest_syscall_header(source/'port/android/guest/libc/arch/arm64_32/bits/syscall.h.in',include/'bits/syscall.h')
    internal=deps/'musl-internal';internal.mkdir();(internal/'version.h').write_text('#define VERSION "1.2.5"\n')
    aot=source/'build/macos-aot';aot.mkdir()
    python=sys.executable
    run(python,'tools/linux_msvc_semantics.py','--output',aot/'halo_msvc_semantics.h','--all-inlines','--tags','source','--inlines','source','--inlines','port/include/xdk',cwd=source)
    run(python,'tools/linux_msvc_semantics.py','--output',aot/'platform_msvc_semantics.h','--inlines','port/include/xdk',cwd=source)
    brew=subprocess.check_output(['brew','--prefix'],text=True).strip()
    llvm=Path(subprocess.check_output(['brew','--prefix','llvm'],text=True).strip())/'bin'
    lld=Path(subprocess.check_output(['brew','--prefix','lld'],text=True).strip())/'bin/ld.lld'
    env=os.environ.copy();env['HALO_BREW_INCLUDE']=brew+'/include'
    if args.prepare_only:
        print('Prepared isolated source/runtime/musl tree:',work);return
    for scope in ('game','platform','math','libc'):
        run(python,'validation/sourceport-lp64/batch_compile_guest.py','--scope',scope,'--jobs',args.jobs,'--cc','clang',cwd=source,env=env)
    link_args=['--link-cc',llvm/'clang','--lld',lld,'--llvm-nm',llvm/'llvm-nm']
    run(python,'tools/macos_aot_build.py','combine',*link_args,cwd=source)
    run(python,'tools/macos_aot_build.py','link',*link_args,cwd=source)
    elf=aot/'halo_guest.elf';elf_sha=sha(elf);lift=aot/'lifted-final'
    run(python,'tools/macos_lift_elf.py','--elf',elf,'--output',lift,'--recomp-root',runtime,'--expected-elf-sha256',elf_sha,cwd=source)
    report=json.loads((lift/'lift_report.json').read_text())
    if report.get('functions_failed') or report.get('unresolved_direct_targets'):
        raise ValueError('AOT translation contains failures/unresolved direct targets')
    native=source/'build/native'
    run('cmake','-S','port/macos','-B',native,'-DCMAKE_BUILD_TYPE=Release','-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0','-DBUILD_TESTING=ON','-DHALO_AOT_HOST_OPTIMIZATION=2','-DHALO_LIFT_DIR='+str(lift),cwd=source)
    run('cmake','--build',native,'--target','halo_macos_aot','halo_posix_host_imports_test','--parallel',args.jobs,cwd=source)
    run('ctest','--test-dir',native,'--output-on-failure','-R','^halo_posix_',cwd=source)
    run(python,'tools/test_macos_sdl_window.py',cwd=source)
    run(python,'tools/test_macos_hd_resolution.py',cwd=source)
    run(python,'tools/test_macos_display_cycle.py',cwd=source)
    run(python,'tools/test_macos_ui_condition.py',cwd=source)
    run(python,'tools/test_macos_network_startup.py',cwd=source)
    run(python,'tools/test_macos_multiplayer_harness.py',cwd=source)
    run(python,'tools/test_macos_symbol_namespaces.py',cwd=source)
    run(python,'tools/test_macos_gamepad.py',cwd=source)
    run(python,'tools/test_macos_translated_gamepad.py','--elf',elf,'--output',aot/'gamepad-fixture','--recomp-root',runtime,cwd=source)
    run(python,'tools/test_macos_translated_errno.py','--elf',elf,'--output',aot/'errno-fixture','--miniupnpc-lib',native/'libhalo_miniupnpc.a','--recomp-root',runtime,cwd=source)
    binary=native/'halo_macos_aot';stamp=aot/'local-build-provenance.json'
    versions={}
    for key,cmd in {'macos':['sw_vers','-productVersion'],'clang':['clang','--version'],'llvm':[str(llvm/'clang'),'--version'],'brew':['brew','list','--versions','sdl3','ffmpeg','llvm','lld','cmake'],'capstone':[python,'-c','import capstone; print(capstone.__version__)']}.items():
        versions[key]=subprocess.check_output(cmd,text=True).strip()
    stamp.write_text(json.dumps({'source':source_provenance,'tooling_inputs_sha256':tooling_inputs,'build_environment':versions,'binary_sha256':sha(binary),'elf_sha256':elf_sha,'pins':{name:pin for name,pin in pins.items() if name!='halo' or args.source_checkout is None},'candidate_only':True,'startup_acceptance_pending':True,'note':'Locally built; packaging checks do not assert gameplay acceptance.'},indent=2)+'\n')
    run(python,'tools/macos_package_app.py','--binary',binary,'--elf',elf,'--stamp',stamp,'--assets',assets,'--output',out,cwd=source)
    run(out/'Contents/MacOS/Halo Combat Evolved','--check')
    print('Built:',out,'\nLaunch with Finder. Game data is bundled; move the whole app freely. Application Support holds only writable user state.')

if __name__=='__main__':
    try:main()
    except (ValueError,OSError,subprocess.CalledProcessError) as error:
        raise SystemExit('Build failed: '+str(error))
