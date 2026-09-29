#!/usr/bin/env python3
"""Actual SDL virtual Xbox/DualSense → native tokens → source XInput mapping.
No game is launched; virtual devices exercise mappings without physical hardware.
"""
from pathlib import Path
import argparse, re, subprocess, tempfile, shlex
from test_macos_ui_condition import function_body
ROOT=Path(__file__).resolve().parents[1]
def main():
 p=argparse.ArgumentParser();p.parse_args()
 src=ROOT/'port/linux/src/xinput_sdl.c'
 host=ROOT/'port/macos/host_sdl.c'
 text=src.read_text(); defs='\n'.join(x for x in (ROOT/'port/include/xdk/xdk_xbox.h').read_text().splitlines() if x.startswith('#define XINPUT_GAMEPAD_') or x.startswith('#define XDEVICE_PORT'))
 pdb=(ROOT/'port/include/xdk/xdk_pdb.h').read_text(); struct=re.search(r'struct _XINPUT_GAMEPAD \{.*?\};',pdb,re.S).group()
 unit='''#include <SDL3/SDL.h>
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host_sdl.h"
#include "guest_address.h"
#define HALO_MACOS 1
#define PORT_COUNT 4
#define TRUE 1
#define FALSE 0
#define SHORT_MAX 32767
#define SHORT_MIN (-32768)
typedef int BOOL; typedef uint8_t BYTE; typedef uint16_t WORD; typedef int16_t SHORT; typedef uint32_t DWORD;
void mac_host_log(int p,const char *s){(void)p;(void)s;}
void mac_host_logf(int p,const char *s,...){(void)p;(void)s;}
void mac_host_apply_app_icon(void){} void mac_host_activate_app(void){}
void mac_game_evidence_presented(void){}
void mac_host_bink_presented(void){} void mac_host_memory_fingerprint_presented(void){}
'''+defs+'\n'+struct+'\ntypedef struct _XINPUT_GAMEPAD XINPUT_GAMEPAD;\n'
 unit+='''static unsigned enumeration_calls;
static SDL_JoystickID *fixture_gamepads(int *n){enumeration_calls++;return SDL_GetGamepads(n);}
#define SDL_GetGamepads fixture_gamepads
static SDL_Gamepad *guest_from(SDL_JoystickID id){return (SDL_Gamepad *)(uintptr_t)mac_host_sdl_gamepad_from_id(id);}
static SDL_Gamepad *guest_open(SDL_JoystickID id){return (SDL_Gamepad *)(uintptr_t)mac_host_sdl_open_gamepad(id);}
static Sint16 guest_axis(SDL_Gamepad *p,SDL_GamepadAxis a){return mac_host_sdl_gamepad_axis((uint32_t)(uintptr_t)p,a);}
static bool guest_button(SDL_Gamepad *p,SDL_GamepadButton b){return mac_host_sdl_gamepad_button((uint32_t)(uintptr_t)p,b);}
#define SDL_GetGamepadFromID guest_from
#define SDL_OpenGamepad guest_open
#define SDL_GetGamepadAxis guest_axis
#define SDL_GetGamepadButton guest_button
'''
 if 'gamepad_cache_lock' in text:
  start=text.index('#ifdef HALO_MACOS\n/* Four fixed player slots.');end=text.index('static int sdl_gamepads',start);unit+='\n#define HALO_GAMEPAD_CACHE_TEST 1\n'+text[start:end]
 for name, ret in [('sdl_gamepads','static int '),('stick','static SHORT '),('merge_button','static void '),('sdl_gamepad_state','static void '),('connected_gamepads','static DWORD ')]:unit+='\n'+ret+function_body(text,name)+'\n'
 unit+='\nshort '+function_body((ROOT/'source/input/input_xbox.c').read_text(),'fix_dead_zone')+'\n'
 unit+=(ROOT/'port/macos/tests/gamepad_virtual_test.inc').read_text()
 flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','sdl3'],text=True))
 with tempfile.TemporaryDirectory(prefix='halo-gamepad-') as tmp:
  c=Path(tmp)/'test.c';exe=Path(tmp)/'test';c.write_text(unit)
  subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-UNDEBUG','-I'+str(ROOT/'port/macos'),str(c),str(host),str(ROOT/'port/macos/guest_address.c'),str(ROOT/'port/macos/guest_call.c'),'-framework','OpenGL',*flags,'-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True)
if __name__=='__main__':main()
