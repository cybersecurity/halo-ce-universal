#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"
#include "guest_image.h"
#include "guest_address.h"
#include "recomp_state.h"
#include "host_syscall.h"
#include "host_sdl.h"
#include <SDL3/SDL.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
static void unexpected(uint32_t va){fprintf(stderr,"unexpected guest call %08x\n",va);abort();}
void recomp_unsupported_instruction(uint32_t va){unexpected(va);}
void recomp_icall_fail_log(uint32_t va){unexpected(va);}
void recomp_icall_not_code_log(uint32_t va){unexpected(va);}
recomp_func_t recomp_lookup_manual(uint32_t va){(void)va;return NULL;}
recomp_func_t recomp_lookup(uint32_t va){(void)va;return NULL;}
recomp_func_t recomp_lookup_kernel(uint32_t va){(void)va;return NULL;}
void mac_host_apply_app_icon(void){}void mac_host_activate_app(void){}
void mac_game_evidence_presented(void){}
void mac_host_bink_presented(void){}void mac_host_memory_fingerprint_presented(void){}
void fixture_pump(void){SDL_UpdateJoysticks();while(mac_host_sdl_poll_event(0x60007000)){
#ifdef TEST_GAMEPADS_CHANGED
 SDL_Event *event=mac_guest_address_resolve(0x60007000,sizeof(SDL_Event));
 if(event->type==SDL_EVENT_GAMEPAD_ADDED || event->type==SDL_EVENT_GAMEPAD_REMOVED){uint32_t saved_stack=g_esp;PUSH32(g_esp,0);TEST_GAMEPADS_CHANGED();assert(g_esp==saved_stack);}
#endif
 } }
void mac_guest_import_dispatch_token(uint32_t va)
{
 uint32_t a=MEM32(g_esp+4),b=MEM32(g_esp+8),c=MEM32(g_esp+12),d=MEM32(g_esp+16);
 switch(va){
 case TEST_host_sdl_get_gamepads:g_eax=mac_host_sdl_get_gamepads(a,b);break;
 case TEST_host_sdl_open_gamepad:g_eax=mac_host_sdl_open_gamepad(a);break;
 case TEST_host_sdl_gamepad_from_id:g_eax=mac_host_sdl_gamepad_from_id(a);break;
 case TEST_host_sdl_gamepad_axis:g_eax=mac_host_sdl_gamepad_axis(a,b);break;
 case TEST_host_sdl_gamepad_button:g_eax=mac_host_sdl_gamepad_button(a,b);break;
 case TEST_host_sdl_gamepad_type:g_eax=mac_host_sdl_gamepad_type(a);break;
 case TEST_host_sdl_rumble_gamepad:g_eax=mac_host_sdl_rumble_gamepad(a,b,c,d);break;
 default:unexpected(va);
 }g_esp+=4;
}
static SDL_JoystickID attach(bool sony)
{
 SDL_VirtualJoystickDesc desc;SDL_INIT_INTERFACE(&desc);desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;desc.naxes=6;desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
 desc.vendor_id=sony?0x054c:0x045e;desc.product_id=sony?0x0ce6:0x028e;desc.name=sony?"Fixture DualSense":"Fixture Xbox 360";
 desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
 SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc);assert(id);char gs[64],map[1024];SDL_GUIDToString(SDL_GetJoystickGUIDForID(id),gs,sizeof(gs));
 snprintf(map,sizeof(map),"%s,%s,a:b0,b:b1,x:b2,y:b3,back:b4,start:b6,leftstick:b7,rightstick:b8,leftshoulder:b9,rightshoulder:b10,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,type:%s,",gs,desc.name,sony?"ps5":"xbox360");assert(SDL_AddGamepadMapping(map)>=0);return id;
}
static void neutral(SDL_Joystick *j){for(int i=0;i<6;i++)assert(SDL_SetJoystickVirtualAxis(j,i,i>=4?-32768:0));for(int i=0;i<SDL_GAMEPAD_BUTTON_COUNT;i++)assert(SDL_SetJoystickVirtualButton(j,i,false));fixture_pump();}
static uint32_t call(recomp_func_t f,const uint32_t *args,int count,int popped)
{
 mac_recomp_state_initialize_thread(0x6000f000,0x60002000);MEM32(g_esp)=0;for(int i=0;i<count;i++)MEM32(g_esp+4+i*4)=args[i];f();assert(g_esp==0x6000f000+4+(uint32_t)popped);return g_eax;
}
static uint32_t open_pad(unsigned port){uint32_t args[]={TEST_GAMEPAD_TYPE_VA,port,0,0};return call(TEST_XInputOpen,args,4,16);}
static void changes(uint32_t inserted,uint32_t removed)
{
 uint32_t args[]={TEST_GAMEPAD_TYPE_VA,0x60004000,0x60004004};
 uint32_t status=call(TEST_XGetDeviceChanges,args,3,12);
 assert(status==!!(inserted|removed)&&MEM32(0x60004000)==inserted&&MEM32(0x60004004)==removed);
}
static uint32_t get(uint32_t pad)
{
 memset((void *)XBOX_PTR(0x60001000),0xa5,64);uint32_t args[]={pad,0x60001004};uint32_t status=call(TEST_XInputGetState,args,2,8);
 /* Xbox state is packed2: four-byte packet + exactly18 gamepad bytes. */
 for(unsigned i=0;i<4;i++)assert(MEM8(0x60001000+i)==0xa5);
 for(unsigned i=26;i<64;i++)assert(MEM8(0x60001000+i)==0xa5);return status;
}
int main(int argc,char **argv)
{
 assert(argc==2);FILE *file=fopen(argv[1],"rb");assert(file);fseek(file,0,SEEK_END);long size=ftell(file);rewind(file);void *bytes=malloc(size);assert(fread(bytes,1,size,file)==(size_t)size);fclose(file);
 struct mac_guest_image image;char error[256];assert(mac_guest_image_load(bytes,size,&image,error,sizeof(error))==0);assert(mac_guest_address_commit(0x60000000,0x10000,PROT_READ|PROT_WRITE)==0);assert(mac_recomp_state_set_image(image.memory_offset,image.code_lo_va,image.code_hi_va)==0);
 mac_recomp_state_initialize_thread(0x6000f000,0x60002000);assert(SDL_Init(SDL_INIT_GAMEPAD));SDL_JoystickID x=attach(false),d=attach(true);SDL_Joystick *jx=SDL_OpenJoystick(x),*jd=SDL_OpenJoystick(d);neutral(jx);neutral(jd);uint32_t p0=open_pad(0),p1=open_pad(1);assert(p0&&p1);changes(3,0);changes(0,0);
 assert(get(p0)==0);for(unsigned i=0;i<18;i++)assert(MEM8(0x60001008+i)==0);
 for(int i=0;i<SDL_GAMEPAD_BUTTON_COUNT;i++)assert(SDL_SetJoystickVirtualButton(jx,i,true));for(int i=0;i<6;i++)assert(SDL_SetJoystickVirtualAxis(jx,i,32767));fixture_pump();assert(get(p0)==0);
 assert(MEM16(0x60001008)==0xff);for(unsigned i=0;i<8;i++)assert(MEM8(0x6000100a+i)==255);assert((int16_t)MEM16(0x60001012)==32767&&(int16_t)MEM16(0x60001014)==-32767);
 assert(SDL_DetachVirtualJoystick(x));fixture_pump();assert(get(p0)==0);for(unsigned i=0;i<18;i++)assert(MEM8(0x60001008+i)==0);changes(0,0);SDL_CloseJoystick(jx);
 assert(SDL_SetJoystickVirtualButton(jd,SDL_GAMEPAD_BUTTON_START,true));assert(SDL_SetJoystickVirtualButton(jd,SDL_GAMEPAD_BUTTON_DPAD_DOWN,true));fixture_pump();assert(get(p1)==0&&MEM16(0x60001008)==0x12);
 uint32_t feedback=0x60003000;memset((void *)XBOX_PTR(feedback),0,70);MEM16(feedback+66)=1234;MEM16(feedback+68)=5678;uint32_t args[]={p1,feedback};assert(call(TEST_XInputSetState,args,2,8)==50&&MEM32(feedback)==50);
 assert(SDL_DetachVirtualJoystick(d));fixture_pump();changes(0,2);assert(get(p1)==1167);for(unsigned i=0;i<22;i++)assert(MEM8(0x60001004+i)==0);SDL_CloseJoystick(jd);
 SDL_Quit();mac_guest_address_reset();free(bytes);puts("Actual translated XInputOpen/GetState/SetState + SDL guest/native ABI:22-byte state bounds, neutral/full controls, held removal, second-slot menu and unsupported rumble PASS");return 0;
}
