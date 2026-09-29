/* Diagnostic-only load evidence, resolved from the exact loaded ELF symbols.
 * Source game.c verifies map_loaded/active prefix and options.map_name offsets.
 * Observes a successful native swap; it does not modify guest state. */
#include "host_game_evidence.h"
#include "guest_address.h"
#include "guest_call.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
static uint32_t globals_symbol, scenario_symbol;
static uint64_t next_report, frames;
void mac_game_evidence_configure(uint32_t globals_va,uint32_t scenario_va)
{ globals_symbol=globals_va; scenario_symbol=scenario_va; next_report=frames=0; }
static uint32_t ui_count_va,ui_targets_va,ui_x_va,ui_y_va;
static int32_t previous_count=-1;
static int16_t previous_x=-32768,previous_y=-32768;
void mac_ui_evidence_configure(uint32_t count,uint32_t targets,uint32_t x,uint32_t y)
{ ui_count_va=count;ui_targets_va=targets;ui_x_va=x;ui_y_va=y; }
static void ui_evidence_presented(void)
{
 if(!ui_count_va) return;
 int32_t count;int16_t x,y;unsigned char targets[96*16];
 if(mac_guest_read(ui_count_va,&count,4)||mac_guest_read(ui_x_va,&x,2)||mac_guest_read(ui_y_va,&y,2)||
    count<0||count>96||mac_guest_read(ui_targets_va,targets,(size_t)count*16)) return;
 if(count==previous_count&&x==previous_x&&y==previous_y) return;
 previous_count=count;previous_x=x;previous_y=y;
 fprintf(stderr,"[ui-pointer] click=%d,%d targets=%d\n",x,y,count);
 for(int32_t i=0;i<count;i++) {
  uint32_t widget;int16_t fields[6];memcpy(&widget,targets+i*16,4);memcpy(fields,targets+i*16+4,12);
  fprintf(stderr,"[ui-target] index=%d widget=%08x bounds=%d,%d..%d,%d kind=%d button=%d\n",
    i,widget,fields[1],fields[0],fields[3],fields[2],fields[4],fields[5]);
 }
}
void mac_game_evidence_presented(void)
{
 ui_evidence_presented();
 if(!globals_symbol || !scenario_symbol) return;
 uint64_t ticks_ms=SDL_GetTicks();
 ++frames;
 if(ticks_ms<next_report) return;
 next_report=ticks_ms+5000;
 uint32_t globals=0, scenario=0xffffffffu;
 unsigned char prefix[0x114];
 if(mac_guest_read(globals_symbol,&globals,4)!=0 || !globals ||
    mac_guest_read(globals,prefix,sizeof prefix)!=0 ||
    mac_guest_read(scenario_symbol,&scenario,4)!=0) return;
 char name[257]; memcpy(name,prefix+0x14,256);name[256]=0;
 for(unsigned i=0;name[i];i++) if((unsigned char)name[i]<32 || (unsigned char)name[i]>126) name[i]='?';
 fprintf(stderr,"[game-state] frame=%llu ticks_ms=%llu map_loaded=%u active=%u loading=%u scenario=%08x map=%s\n",
  (unsigned long long)frames,(unsigned long long)ticks_ms,prefix[0],prefix[1],prefix[3],scenario,name);
}
