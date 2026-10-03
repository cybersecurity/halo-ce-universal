#include "host_graphics.h"
#include "host_debug.h"
/* Start the statically compiled guest on the iOS UI thread and persist its log. */
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include "ios_host.h"
#include "../network/room_bridge.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static char data_root[1024],save_root[1024];
static FILE *log_file;
void host_logf(int priority,const char *format,...) {
    (void)priority;va_list ap;va_start(ap,format);
    char text[4096];vsnprintf(text,sizeof(text),format,ap);va_end(ap);
    fprintf(stderr,"halo-ios: %s\n",text);
    if(log_file){flockfile(log_file);fprintf(log_file,"%s\n",text);fflush(log_file);funlockfile(log_file);}
}
void host_log(int priority,const char *text) { host_logf(priority,"%s",text); }
void host_fatal(const char *format,...) {
    va_list ap;va_start(ap,format);char text[1024];vsnprintf(text,sizeof(text),format,ap);va_end(ap);
    host_logf(HOST_LOG_ERROR,"FATAL: %s",text);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"Halo",text,NULL);exit(1);
}
void host_abort(const char *reason) { host_logf(HOST_LOG_ERROR,"guest abort: %s",reason);abort(); }
void host_exit(int code) {host_logf(HOST_LOG_INFO,"game exit %d",code);exit(code);}
int host_errno(void) {return host_linux_errno(errno);}
void host_debug_thread_started(void) {}
void host_debug_thread_exited(void) {}
void host_debug_start_sampler(const char *setting) {(void)setting;}

static uint32_t copy_string(const char *text) {
    char *p=host_low_map(strlen(text)+1,PROT_READ|PROT_WRITE);
    if(!p)host_fatal("out of guest memory");strcpy(p,text);return guest_pointer(p);
}
int main(int argc,char **argv) {
    (void)argc;(void)argv;
    @autoreleasepool {
#if TARGET_OS_TV
        /* tvOS apps may only write to Caches (purgeable). */
        NSString *documents=[NSSearchPathForDirectoriesInDomains(NSCachesDirectory,NSUserDomainMask,YES).firstObject stringByAppendingPathComponent:@"Halo"];
        [NSFileManager.defaultManager createDirectoryAtPath:documents withIntermediateDirectories:YES attributes:nil error:nil];
#else
        NSString *documents=NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject;
#endif
        snprintf(data_root,sizeof(data_root),"%s",documents.fileSystemRepresentation);
        snprintf(save_root,sizeof(save_root),"%s/save",data_root);mkdir(save_root,0755);
        chdir(data_root);
        log_file=fopen("ios-runtime.log","w");setvbuf(stderr,NULL,_IONBF,0);
        host_logf(HOST_LOG_INFO,"Halo iOS native guest starting");
        UIApplication.sharedApplication.idleTimerDisabled=YES;
        host_ios_prepare_assets(data_root);
        if(host_load_image(NULL,0))host_fatal("Could not map the signed game image. See ios-runtime.log in Files.");
        host_install_signal_handlers();
        SDL_SetHint(SDL_HINT_ORIENTATIONS,"LandscapeLeft LandscapeRight");
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"0");
        /* Keep the Siri Remote out of the player ports; Halo needs a real controller. */
        SDL_SetHint(SDL_HINT_TV_REMOTE_AS_JOYSTICK,"0");
        if(!SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO|SDL_INIT_GAMEPAD))host_fatal("SDL initialization: %s",SDL_GetError());
        host_ios_touch_initialize();
        halo_graphics_initialize();
        halo_debug_initialize();
        host_ios_room_prepare();
        const SDL_DisplayMode *mode=SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
        int width=640,pixel_width=640,pixel_height=480;
        if(mode && mode->w && mode->h){
            int longer=mode->w>mode->h?mode->w:mode->h,shorter=mode->w>mode->h?mode->h:mode->w;
            float density=mode->pixel_density>0?mode->pixel_density:1;
            width=(480*longer/shorter)&~1;
            pixel_width=(int)(longer*density+0.5f);pixel_height=(int)(shorter*density+0.5f);
        }
        char env_data[1200],env_save[1200],env_width[64],env_pixel_width[64],env_pixel_height[64];
        snprintf(env_data,sizeof(env_data),"HALO_DATA_ROOT=%s",data_root);
        snprintf(env_save,sizeof(env_save),"HALO_SAVE_ROOT=%s",save_root);
        snprintf(env_width,sizeof(env_width),"HALO_DISPLAY_WIDTH=%d",width);
        snprintf(env_pixel_width,sizeof(env_pixel_width),"HALO_DISPLAY_PIXEL_WIDTH=%d",pixel_width);
        snprintf(env_pixel_height,sizeof(env_pixel_height),"HALO_DISPLAY_PIXEL_HEIGHT=%d",pixel_height);
        const char *env[16];size_t env_count=0;
        env[env_count++]=env_data;env[env_count++]=env_save;env[env_count++]=env_width;
        env[env_count++]=env_pixel_width;env[env_count++]=env_pixel_height;
#if TARGET_OS_TV
        char env_render[64];
        /* The build chooses the render height (tools/ios_build.py --render-height). */
        int render_height=[[NSBundle.mainBundle objectForInfoDictionaryKey:@"HaloRenderHeight"] intValue];
        if(render_height>0){snprintf(env_render,sizeof(env_render),"HALO_RENDER_HEIGHT=%d",render_height);env[env_count++]=env_render;}
#endif
        env[env_count++]="HALO_NET_ONLINE=0";
        env[env_count++]="TZ=UTC0";env[env_count++]=NULL;
        uint32_t *environment=host_low_map(sizeof(uint32_t)*env_count,PROT_READ|PROT_WRITE);
        for(size_t i=0;i<env_count-1;i++)environment[i]=copy_string(env[i]);environment[env_count-1]=0;
        uint32_t *arguments=host_low_map(8,PROT_READ|PROT_WRITE);arguments[0]=copy_string("halo");arguments[1]=0;
        struct halo_guest_boot *boot=host_low_map(sizeof(*boot),PROT_READ|PROT_WRITE);
        *boot=(struct halo_guest_boot){1,guest_pointer(arguments),guest_pointer(environment),0x4000};
        size_t stack_size=16*1024*1024;
        void *stack=host_low_map(stack_size+0x4000,PROT_READ|PROT_WRITE);
        if(!stack)host_fatal("could not allocate game stack");mprotect(stack,0x4000,PROT_NONE);
        host_logf(HOST_LOG_INFO,"entering guest at %08x, data %s",host_image.header->start,data_root);
        host_guest_on_stack((uintptr_t)host_pointer(host_image.header->start),guest_pointer(boot),host_arena,(char *)stack+stack_size+0x4000);
    }
    return 0;
}
