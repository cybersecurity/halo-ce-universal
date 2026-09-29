#!/usr/bin/env python3
"""Actual guest FileExists/GetFileAttributes + native stat errno regression.
Path/ref leaves provide an exact fixture path; emutls leaf provides bounded
per-guest-thread Win32 LastError storage. Musl errno accessor stays translated.
"""
import argparse,hashlib,re,subprocess,sys
from pathlib import Path
from macos_lift_elf import ROOT,parse_elf32

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--elf',type=Path,default=ROOT/'build/macos-aot/halo_guest.elf');p.add_argument('--output',type=Path,required=True);p.add_argument('--miniupnpc-lib',type=Path,required=True);p.add_argument('--recomp-root',type=Path,required=True);a=p.parse_args()
 elf=parse_elf32(a.elf); symbols={s['name']:s['value'] for s in elf['symbols']}
 # Static __get_tp appears once per musl unit; include every real symbol.
 tp_addresses=[s['value'] for s in elf['symbols'] if s['name']=='__get_tp'];digest=hashlib.sha256(a.elf.read_bytes()).hexdigest()
 names=['__guest_get_tp','__errno_location','__get_tp','GetFileAttributesA','platform_set_last_error_from_errno','GetLastError','file_exists']
 cmd=[sys.executable,str(ROOT/'tools/macos_lift_elf.py'),'--elf',str(a.elf),'--output',str(a.output),'--expected-elf-sha256',digest,'--recomp-root',str(a.recomp_root),'--discover']
 for n in names:cmd+=['--address',hex(symbols[n])]
 for address in tp_addresses:cmd+=['--address',hex(address)]
 subprocess.run(cmd,check=True,stdout=subprocess.DEVNULL)
 services={'memset':'memset((void *)XBOX_PTR(MEM32(g_esp+4)),MEM32(g_esp+8),MEM32(g_esp+12));g_eax=MEM32(g_esp+4);g_esp+=4;',
 'platform_translate_path':'strcpy((char *)XBOX_PTR(MEM32(g_esp+8)),(char *)XBOX_PTR(MEM32(g_esp+4)));g_eax=MEM32(g_esp+8);g_esp+=4;',
 'file_reference_get_const_info':'g_eax=MEM32(g_esp+4);g_esp+=4;',
 'file_reference_get_info':'g_eax=MEM32(g_esp+4);g_esp+=4;',
 'file_location_get_full_path':'strcpy((char *)XBOX_PTR(MEM32(g_esp+12)),(char *)XBOX_PTR(MEM32(g_esp+8)));g_esp+=4;',
 '__emutls_get_address':'g_eax=mac_host_get_guest_tp()+0x100;g_esp+=4;',
 'file_error':'fixture_file_errors++;g_esp+=4;',
 'fill_attribute_data':'MEM32(MEM32(g_esp+8))=128;g_esp+=4;'}
 stubs=(a.output/'recomp_stubs_unresolved.c').read_text().replace('#include "recomp_funcs.h"','#include "recomp_funcs.h"\n#include "host_services.h"\n#include <string.h>\nextern int fixture_file_errors;')
 for n,body in services.items():
  if n not in symbols:continue
  f=f'sub_{symbols[n]:08X}';pat=r'void '+f+r'\(void\) \{[^\n]*\}'
  if re.search(pat,stubs):stubs=re.sub(pat,'',stubs)+f'\nvoid {f}(void){{{body}}}\n'
 (a.output/'controlled_services.c').write_text(stubs)
 native=ROOT/'port/macos';command=['clang','-O1','-UNDEBUG','-Wno-unused','-Wno-parentheses-equality',f'-I{a.output.resolve()}',f'-I{native}',f'-I{ROOT}/port/third_party/miniupnpc/include']
 for n in ('__errno_location','__get_tp','GetFileAttributesA','GetLastError','file_exists'):command+=[f'-DTEST_{n}=sub_{symbols[n]:08X}']
 command += [f'-DVA___get_tp={symbols["__get_tp"]}']
 for n in ('host_get_tp','posix_stat'):command+=[f'-DTOKEN_{n}={symbols[n]}']
 command+=[str(native/'tests/translated_errno_test.c'),str(a.output/'recomp_0000.c'),str(a.output/'controlled_services.c'),str(a.output/'recomp_import_aliases.c')]
 command+=[str(native/n) for n in ('guest_image.c','guest_address.c','guest_allocator.c','guest_call.c','recomp_state.c','guest_x87_80.c','host_services.c','host_syscall.c','guest_errno.c','posix_backend_files.c','posix_backend_net.c','posix_backend_upnp.c')]
 # Exercise the published native adapter and its real translated TLS accessor.
 command+=[str(native/'posix_host_imports.c'),str(a.miniupnpc_lib)]
 exe=a.output/'errno-test';subprocess.run(command+['-o',str(exe)],check=True)
 print('actual ELF SHA256',digest,flush=True);subprocess.run([str(exe),str(a.elf)],check=True)
if __name__=='__main__':main()
