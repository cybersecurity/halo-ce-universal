#undef SDL_GetGamepadFromID
#undef SDL_OpenGamepad
#undef SDL_GetGamepadAxis
#undef SDL_GetGamepadButton
static SDL_Event event_buffer;
static int rumble_calls;
static int notify_gamepad_events=1;
static bool rumble(void *data,Uint16 low,Uint16 high){(void)data;assert(low==1234&&high==5678);rumble_calls++;return true;}
static SDL_JoystickID attach(bool sony,bool motors)
{
 SDL_VirtualJoystickDesc desc; SDL_INIT_INTERFACE(&desc);
 desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;desc.naxes=6;desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
 desc.vendor_id=sony?0x054c:0x045e;desc.product_id=sony?0x0ce6:0x028e;
 desc.name=sony?"Fixture DualSense":"Fixture Xbox 360";
 desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
 desc.Rumble=motors?rumble:NULL;
 SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc);assert(id);
 SDL_GUID guid=SDL_GetJoystickGUIDForID(id);char gs[64],mapping[1024];SDL_GUIDToString(guid,gs,sizeof(gs));
 snprintf(mapping,sizeof(mapping),"%s,%s,a:b0,b:b1,x:b2,y:b3,back:b4,start:b6,leftstick:b7,rightstick:b8,leftshoulder:b9,rightshoulder:b10,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,type:%s,",gs,desc.name,sony?"ps5":"xbox360");
 assert(SDL_AddGamepadMapping(mapping)>=0);return id;
}
static void pump(void){(void)notify_gamepad_events;SDL_UpdateJoysticks();while(mac_host_sdl_poll_event(0x1000)){
#ifdef HALO_GAMEPAD_CACHE_TEST
 if(notify_gamepad_events && (event_buffer.type==SDL_EVENT_GAMEPAD_ADDED || event_buffer.type==SDL_EVENT_GAMEPAD_REMOVED)) halo_input_gamepads_changed();
#endif
 } }
static void neutral(SDL_Joystick *j){for(int i=0;i<6;i++)assert(SDL_SetJoystickVirtualAxis(j,i,i>=4?-32768:0));for(int i=0;i<SDL_GAMEPAD_BUTTON_COUNT;i++)assert(SDL_SetJoystickVirtualButton(j,i,false));pump();}
static XINPUT_GAMEPAD state(SDL_Gamepad *p){XINPUT_GAMEPAD s={0};sdl_gamepad_state(p,&s);return s;}
#ifdef HALO_GAMEPAD_CACHE_TEST
static void *cache_reader(void *unused)
{
 (void)unused;SDL_Gamepad *pads[4];
 for(unsigned i=0;i<100;i++)assert(sdl_gamepads(pads)==2&&pads[0]&&pads[1]);
 return NULL;
}
#endif
int main(void)
{
 _Static_assert(sizeof(XINPUT_GAMEPAD)==18,"Xbox guest gamepad ABI");
 assert(SDL_Init(SDL_INIT_GAMEPAD));assert(mac_guest_address_register(0x1000,&event_buffer,sizeof(event_buffer))==0);
 SDL_JoystickID xbox=attach(false,true),sony=attach(true,false);SDL_Joystick *x=SDL_OpenJoystick(xbox),*d=SDL_OpenJoystick(sony);assert(x&&d);neutral(x);neutral(d);
 SDL_Gamepad *pads[4];assert(sdl_gamepads(pads)==2);assert(pads[0]&&pads[1]);uint32_t xt=(uint32_t)(uintptr_t)pads[0],dt=(uint32_t)(uintptr_t)pads[1];
 assert(mac_host_sdl_gamepad_type(xt)==SDL_GAMEPAD_TYPE_XBOX360);assert(mac_host_sdl_gamepad_type(dt)==SDL_GAMEPAD_TYPE_PS5);
 assert(mac_host_sdl_open_gamepad(xbox)==xt);assert(connected_gamepads()==3);
#ifdef HALO_GAMEPAD_CACHE_TEST
 unsigned before=enumeration_calls;for(int i=0;i<100;i++)assert(sdl_gamepads(pads)==2);assert(enumeration_calls==before);
 pthread_t readers[4];for(unsigned i=0;i<4;i++)assert(!pthread_create(&readers[i],NULL,cache_reader,NULL));
 for(unsigned i=0;i<4;i++)assert(!pthread_join(readers[i],NULL));assert(enumeration_calls==before);
 puts("Controller cache:100 unchanged +400 concurrent polls enumerate zero times PASS");
#endif
 XINPUT_GAMEPAD n=state(pads[0]);assert(!memcmp(&n,&(XINPUT_GAMEPAD){0},sizeof(n)));
 for(int button=0;button<SDL_GAMEPAD_BUTTON_COUNT;button++)assert(SDL_SetJoystickVirtualButton(x,button,true));
 for(int axis=0;axis<6;axis++)assert(SDL_SetJoystickVirtualAxis(x,axis,32767));pump();
 XINPUT_GAMEPAD full=state(pads[0]);assert(full.wButtons==0xff);for(int i=0;i<8;i++)assert(full.bAnalogButtons[i]==255);
 assert(full.sThumbLX==32767&&full.sThumbRX==32767&&full.sThumbLY==-32767&&full.sThumbRY==-32767);
 assert(SDL_SetJoystickVirtualAxis(x,SDL_GAMEPAD_AXIS_LEFTY,-32768));assert(SDL_SetJoystickVirtualAxis(x,SDL_GAMEPAD_AXIS_RIGHTY,-32768));pump();full=state(pads[0]);assert(full.sThumbLY==32767&&full.sThumbRY==32767);
 assert(mac_host_sdl_rumble_gamepad(xt,1234,5678,100));assert(rumble_calls==1);assert(!mac_host_sdl_rumble_gamepad(dt,1234,5678,100));
 assert(fix_dead_zone(0,9000)==0&&fix_dead_zone(9000,9000)==0&&fix_dead_zone(-9000,9000)==0);assert(fix_dead_zone(9010,9000)>0&&fix_dead_zone(-9010,9000)<0);assert(fix_dead_zone(32767,9000)==32767&&fix_dead_zone(-32768,9000)==-32768);
 /* Remove while every button/stick/trigger is held. Slot one must not move. */
 assert(SDL_DetachVirtualJoystick(xbox));pump();assert(sdl_gamepads(pads)==2&&!pads[0]&&(uint32_t)(uintptr_t)pads[1]==dt);assert(connected_gamepads()==3);
 assert(!mac_host_sdl_gamepad_button(xt,SDL_GAMEPAD_BUTTON_SOUTH));assert(!mac_host_sdl_gamepad_axis(xt,SDL_GAMEPAD_AXIS_LEFTX));assert(!mac_host_sdl_rumble_gamepad(xt,1234,5678,100));SDL_CloseJoystick(x);
 SDL_JoystickID replacement=attach(false,true);SDL_Joystick *r=SDL_OpenJoystick(replacement);neutral(r);assert(sdl_gamepads(pads)==2&&pads[0]&&(uint32_t)(uintptr_t)pads[0]!=xt&&(uint32_t)(uintptr_t)pads[1]==dt);
 assert(SDL_SetJoystickVirtualButton(d,SDL_GAMEPAD_BUTTON_START,true));assert(SDL_SetJoystickVirtualButton(d,SDL_GAMEPAD_BUTTON_DPAD_DOWN,true));pump();XINPUT_GAMEPAD menu=state(pads[1]);assert(menu.wButtons==(XINPUT_GAMEPAD_START|XINPUT_GAMEPAD_DPAD_DOWN));
 notify_gamepad_events=0; /* missing notification must still recover stale tokens */
 assert(SDL_DetachVirtualJoystick(sony));assert(SDL_DetachVirtualJoystick(replacement));pump();SDL_CloseJoystick(d);SDL_CloseJoystick(r);assert(sdl_gamepads(pads)==0&&connected_gamepads()==1);SDL_Quit();mac_guest_address_reset();
 puts("Virtual Xbox/DualSense native-token/source-XInput ABI mapping, neutral/full inputs, menu, held disconnect, stable reconnect slots, truthful rumble: PASS");return 0;
}
