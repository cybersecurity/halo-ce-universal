#!/usr/bin/env python3
"""Run actual linked XInput + SDL guest adapter bytes with real virtual SDL pads.
Keyboard/mouse services are explicitly neutral; allocation uses a checked,
fixed fixture scratch arena. Cache mutex services are serialized fixture leaves;
source/native thread locking is tested separately. Gamepad payloads are real.
"""
import argparse, hashlib, json, re, shlex, subprocess, sys
from pathlib import Path
from macos_lift_elf import ROOT,parse_elf32
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--elf',type=Path,default=ROOT/'build/macos-aot/halo_guest.elf');p.add_argument('--output',type=Path,required=True);p.add_argument('--recomp-root',type=Path,required=True);p.add_argument('--reuse-lift',action='store_true');a=p.parse_args()
 symbols={s['name']:s['value'] for s in parse_elf32(a.elf)['symbols']};digest=hashlib.sha256(a.elf.read_bytes()).hexdigest()
 names=['XInputOpen','XInputGetState','XInputSetState','XGetDeviceChanges','controller_port','sdl_gamepads','sdl_gamepad_state','stick','merge_button','connected_gamepads','SDL_GetGamepads','SDL_OpenGamepad','SDL_GetGamepadFromID','SDL_GetGamepadAxis','SDL_GetGamepadButton','SDL_GetGamepadType','SDL_RumbleGamepad']
 if 'halo_input_gamepads_changed' in symbols:names.append('halo_input_gamepads_changed')
 cmd=[sys.executable,str(ROOT/'tools/macos_lift_elf.py'),'--elf',str(a.elf),'--output',str(a.output),'--expected-elf-sha256',digest,'--recomp-root',str(a.recomp_root),'--discover']
 for n in names:cmd+=['--address',hex(symbols[n])]
 if not a.reuse_lift:subprocess.run(cmd,check=True)
 report=json.loads((a.output/'lift_report.json').read_text());assert report['input_elf_sha256']==digest and not report['functions_failed']
 stubs=(a.output/'recomp_stubs_unresolved.c').read_text();services={
 'memset':'g_eax=MEM32(g_esp+4);memset((void *)XBOX_PTR(g_eax),MEM32(g_esp+8),MEM32(g_esp+12));g_esp+=4;',
 'memcpy':'g_eax=MEM32(g_esp+4);memcpy((void *)XBOX_PTR(g_eax),(void *)XBOX_PTR(MEM32(g_esp+8)),MEM32(g_esp+12));g_esp+=4;',
 'memcmp':'g_eax=memcmp((void *)XBOX_PTR(MEM32(g_esp+4)),(void *)XBOX_PTR(MEM32(g_esp+8)),MEM32(g_esp+12));g_esp+=4;',
 'malloc':'assert(MEM32(g_esp+4)<=256);g_eax=0x60008000;g_esp+=4;',
 'SDL_free':'assert(MEM32(g_esp+4)==0x60008000);g_esp+=4;',
 'pthread_mutex_lock':'g_eax=0;g_esp+=4;','__pthread_mutex_lock':'g_eax=0;g_esp+=4;',
 'pthread_mutex_unlock':'g_eax=0;g_esp+=4;','__pthread_mutex_unlock':'g_eax=0;g_esp+=4;',
 'platform_pump_events':'fixture_pump();g_esp+=4;',
 'platform_input_read':'memset((void *)XBOX_PTR(MEM32(g_esp+4)),0,128);g_esp+=4;',
 'mouse_poll':'g_esp+=4;','wheel_update':'g_esp+=4;','keyboard_gamepad':'g_esp+=4;','test_input_gamepad':'g_esp+=4;',
 'console_is_active':'g_eax=0;g_esp+=4;', 'SetEvent':'g_eax=1;g_esp+=8;', 'SetLastError':'g_esp+=8;'}
 stubs=stubs.replace('#include "recomp_funcs.h"','#include "recomp_funcs.h"\n#include <assert.h>\n#include <string.h>\nextern void fixture_pump(void);')
 for n,body in services.items():
  if n not in symbols:continue
  f=f'sub_{symbols[n]:08X}';pat=r'void '+f+r'\(void\) \{[^\n]*\}'
  if re.search(pat,stubs):stubs=re.sub(pat,'',stubs)+f'\nvoid {f}(void){{{body}}}\n'
 (a.output/'controlled_services.c').write_text(stubs)
 native=ROOT/'port/macos';flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','sdl3'],text=True))
 cmd=['clang','-O1','-UNDEBUG','-Wno-unused','-Wno-parentheses-equality',f'-I{a.output.resolve()}',f'-I{native}']
 for n in ['XInputOpen','XInputGetState','XInputSetState','XGetDeviceChanges']:
  cmd+=[f'-DTEST_{n}=sub_{symbols[n]:08X}']
 if 'halo_input_gamepads_changed' in symbols:cmd+=[f'-DTEST_GAMEPADS_CHANGED=sub_{symbols["halo_input_gamepads_changed"]:08X}']
 cmd+=[f'-DTEST_GAMEPAD_TYPE_VA={symbols["XDEVICE_TYPE_GAMEPAD_TABLE"]}']
 imports=json.loads((ROOT/'build/macos-aot/host-import-manifest.json').read_text())
 for item in imports['imports']:
  if item['name'].startswith('host_sdl_') and ('gamepad' in item['name']):assert symbols[item['name']]==item['va'];cmd+=[f'-DTEST_{item["name"]}={item["va"]}']
 cmd += [str(native/'tests/translated_gamepad_test.c'),str(a.output/'recomp_0000.c'),str(a.output/'controlled_services.c'),str(a.output/'recomp_import_aliases.c')]
 cmd += [str(native/n) for n in ('guest_image.c','guest_address.c','guest_allocator.c','guest_call.c','recomp_state.c','guest_x87_80.c','host_services.c','host_syscall.c','host_sdl.c')]
 exe=a.output/'translated-gamepad-test';subprocess.run(cmd+flags+['-framework','OpenGL','-o',str(exe)],check=True)
 print('actual ELF SHA256',digest,flush=True);subprocess.run([str(exe),str(a.elf)],check=True)
if __name__=='__main__':main()
