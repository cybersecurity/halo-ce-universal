#!/usr/bin/env python3
"""Compile actual network-test frontend gate/timer/host-entry statements."""
from pathlib import Path
import subprocess,tempfile
from test_macos_ui_condition import function_body
ROOT=Path(__file__).resolve().parents[1]
def main():
 text=(ROOT/'port/linux/game/network_test.c').read_text()
 gate=function_body(text,'network_test_frontend_ready')
 update=function_body(text,'network_test_update')
 start=update.index('if (!network_test_frontend_ready(main_menu_loaded))')
 timer=update[start:update.index('switch (network_test.mode)',start)]
 host=update[update.index('if (!network_test.set_up)',start):update.index('else if (!network_test.started)',start)]
 unit=r'''
#include <assert.h>
#include <stdio.h>
typedef unsigned char boolean;
#define TRUE 1
#define FALSE 0
static struct { float menu_seconds; boolean set_up; char *map_name; } network_test;
static boolean movie_active,texture_pool_borrowed;
static unsigned host_entries;
static boolean bink_playback_active(void){return movie_active;}
static void main_set_multiplayer_map_name(const char *s){assert(s);}
static void player_ui_fast_setup_network_server(void){assert(!texture_pool_borrowed);host_entries++;}
static void platform_log(const char *s,...){assert(s);}
'''
 unit+='\nstatic boolean '+gate+'\nstatic void update_frontend(boolean main_menu_loaded,float seconds){\n'+timer+host+'}\n'
 unit+=r'''
int main(void){
 network_test.map_name="bloodgulch";movie_active=texture_pool_borrowed=TRUE;
 /* A loaded menu beneath the intro must not consume its safe-start timer. */
 for(unsigned i=0;i<60;i++)update_frontend(TRUE,1.0f/3.0f);
 assert(host_entries==0&&!network_test.set_up&&network_test.menu_seconds==0);
 movie_active=texture_pool_borrowed=FALSE;
 update_frontend(FALSE,5);assert(host_entries==0&&network_test.menu_seconds==0);
 update_frontend(TRUE,1);assert(host_entries==0);update_frontend(TRUE,1);
 assert(host_entries==1&&network_test.set_up);update_frontend(TRUE,1);assert(host_entries==1);
 puts("Actual network-test frontend:loaded menu during intro preserves texture ownership/timer; normal movie disposal then2s starts host once PASS");
}
'''
 with tempfile.TemporaryDirectory(prefix='halo-network-startup-') as tmp:
  c=Path(tmp)/'test.c';exe=Path(tmp)/'test';c.write_text(unit)
  subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-UNDEBUG',str(c),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
if __name__=='__main__':main()
