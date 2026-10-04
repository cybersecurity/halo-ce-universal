#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import "../apple/game_store.h"
#include "ios_host.h"
#include "host_debug.h"
#include <SDL3/SDL.h>
void host_ios_touch_initialize(void) {}
static NSWindow *gameWindow;
void *host_macos_game_window(void) {return (__bridge void *)gameWindow;}
void host_ios_touch_attach(SDL_Window *window) {
 gameWindow=(__bridge NSWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(window),SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,NULL);
 [gameWindow makeKeyAndOrderFront:nil];[NSApp activateIgnoringOtherApps:YES];halo_debug_install_menu();
}
void host_ios_touch_reset(void) {}
void host_ios_touch_focus(void) {}
int host_ios_gamepads(uint32_t *out,int capacity){int count=0;SDL_JoystickID *ids=SDL_GetGamepads(&count);int n=MIN(count,capacity);for(int i=0;i<n;i++)out[i]=ids[i];SDL_free(ids);return n;}
int host_ios_gamepad_type(SDL_Gamepad *pad){return SDL_GetGamepadType(pad);}
int host_ios_gamepad_axis(SDL_Gamepad *pad,int axis){return SDL_GetGamepadAxis(pad,(SDL_GamepadAxis)axis);}
int host_ios_gamepad_button(SDL_Gamepad *pad,int button){return SDL_GetGamepadButton(pad,(SDL_GamepadButton)button);}
@interface HaloImportActions : NSObject
@property(atomic) BOOL cancelled;
@end
@implementation HaloImportActions
-(void)cancel:(id)sender{(void)sender;self.cancelled=YES;}
@end
static int progress(void *context,const char *file,uint64_t done,uint64_t total){(void)file;(void)done;(void)total;return !((__bridge HaloImportActions*)context).cancelled;}
void host_ios_prepare_assets(const char *path){
 NSString *root=[NSString stringWithUTF8String:path];
 while(!halo_game_store_current(root)) {
  NSAlert *intro=[NSAlert new];intro.messageText=@"Choose your game image";intro.informativeText=@"Choose your own original Xbox Halo: Combat Evolved ISO or XISO. A verified copy and extracted maps will be stored privately on this Mac. No game image is included.";[intro addButtonWithTitle:@"Choose Halo ISO"];[intro addButtonWithTitle:@"Quit"];intro.icon=[NSImage imageNamed:NSImageNameApplicationIcon];intro.buttons.firstObject.bezelColor=NSColor.systemBlueColor;intro.buttons.firstObject.contentTintColor=NSColor.whiteColor;
  [NSApp activateIgnoringOtherApps:YES];if([intro runModal]!=NSAlertFirstButtonReturn)exit(0);
  NSOpenPanel *picker=[NSOpenPanel openPanel];picker.canChooseDirectories=NO;picker.allowsMultipleSelection=NO;
  if([picker runModal]!=NSModalResponseOK)continue;
  NSURL *url=picker.URL;BOOL scoped=[url startAccessingSecurityScopedResource];
  NSWindow *window=[[NSWindow alloc]initWithContentRect:NSMakeRect(0,0,500,150) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];window.title=@"Importing game";
  NSTextField *label=[NSTextField wrappingLabelWithString:@"Copying and verifying your image, then validating the game maps. This may take several minutes."];label.frame=NSMakeRect(20,65,460,65);[window.contentView addSubview:label];
  HaloImportActions *actions=[HaloImportActions new];NSButton *cancel=[NSButton buttonWithTitle:@"Cancel" target:actions action:@selector(cancel:)];cancel.frame=NSMakeRect(380,20,100,32);[window.contentView addSubview:cancel];[window center];[window makeKeyAndOrderFront:nil];
  __block BOOL finished=NO,ok=NO;__block NSString *failure=nil;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{
   NSFileCoordinator *coordinator=[[NSFileCoordinator alloc]initWithFilePresenter:nil];NSError *error=nil;
   [coordinator coordinateReadingItemAtURL:url options:0 error:&error byAccessor:^(NSURL *readable){ok=halo_game_store_import(readable,root,progress,(__bridge void*)actions,&failure);}];
   if(error)failure=error.localizedDescription;
   dispatch_async(dispatch_get_main_queue(),^{finished=YES;});
  });
  while(!finished){NSEvent *event=[NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate dateWithTimeIntervalSinceNow:.02] inMode:NSDefaultRunLoopMode dequeue:YES];if(event)[NSApp sendEvent:event];[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];}
  [window orderOut:nil];if(scoped)[url stopAccessingSecurityScopedResource];
  if(!ok){NSAlert *alert=[NSAlert new];alert.messageText=@"Import did not finish";alert.informativeText=failure?:@"Try choosing the image again.";[alert runModal];}
 }
}

/* AppKit enters NSEventTrackingRunLoopMode inside SDL_PollEvent for native
   menus/window tracking. A timer services sound_idle there, completing packets
   and advancing streaming state so callbacks can refill the music stream.
   It is armed only around the event pump, on the guest's arena stack and
   thread, never during game sound
   updates or on the SDL mixing worker. No rendering or input is reentered. */
static uint32_t audioService;
static NSTimer *audioServiceTimer;
static BOOL menuTestScheduled;
void host_macos_audio_event_begin(uint32_t callback) {
    audioService=callback;
    if(!menuTestScheduled && getenv("HALO_MAC_TEST_MENU_AUDIO")) {
        menuTestScheduled=YES;
        NSTimer *test=[NSTimer timerWithTimeInterval:25 repeats:NO block:^(NSTimer *timer){
            (void)timer;
            NSMenu *menu=[[NSMenu alloc]initWithTitle:@"Audio regression test"];
            [menu addItemWithTitle:@"Keep this menu open for eight seconds" action:NULL keyEquivalent:@""];
            [gameWindow makeKeyAndOrderFront:nil];[NSApp activateIgnoringOtherApps:YES];
            CFAbsoluteTime started=CFAbsoluteTimeGetCurrent();
            host_logf(HOST_LOG_INFO,"Menu audio test: begin eight seconds of native menu tracking");
            NSTimer *close=[NSTimer timerWithTimeInterval:8 repeats:NO block:^(NSTimer *t){(void)t;[menu cancelTracking];}];
            [NSRunLoop.mainRunLoop addTimer:close forMode:NSEventTrackingRunLoopMode];
            [menu popUpMenuPositioningItem:nil atLocation:NSMakePoint(30,30) inView:gameWindow.contentView];
            host_logf(HOST_LOG_INFO,"Menu audio test: native menu closed after %.2f seconds",CFAbsoluteTimeGetCurrent()-started);
            halo_debug_present();
        }];
        [NSRunLoop.mainRunLoop addTimer:test forMode:NSDefaultRunLoopMode];
    }
    if(!audioServiceTimer) {
        audioServiceTimer=[NSTimer timerWithTimeInterval:0.01 repeats:YES block:^(NSTimer *timer){
            (void)timer;
            if(audioService)host_call_guest(audioService,0,0,0,0);
        }];
        [NSRunLoop.mainRunLoop addTimer:audioServiceTimer forMode:NSEventTrackingRunLoopMode];
    }
}
void host_macos_audio_event_end(void) {audioService=0;}
