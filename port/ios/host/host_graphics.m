/* ANGLE's Metal backend and Apple's native GLES driver have distinct symbol
   namespaces. No guest rendering call may accidentally cross between them. */
#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#define EGL_EGL_PROTOTYPES 0
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <OpenGLES/ES3/gl.h>
#include <OpenGLES/ES3/glext.h>
#include <dlfcn.h>
#include <string.h>
#include "host_graphics.h"
#include "ios_host.h"

static BOOL metal;
static void *eglLibrary,*glesLibrary;
static EGLDisplay display;
static EGLConfig config;
static EGLContext context;
static EGLSurface windowSurface,sharedSurface;
static GLuint sharedTexture;
static BOOL sharedBound;
static SDL_MetalView metalView;
static PFNEGLGETPROCADDRESSPROC eglGetProcAddress;static PFNEGLINITIALIZEPROC eglInitialize;static PFNEGLCHOOSECONFIGPROC eglChooseConfig;
static PFNEGLCREATECONTEXTPROC eglCreateContext;static PFNEGLCREATEWINDOWSURFACEPROC eglCreateWindowSurface;static PFNEGLMAKECURRENTPROC eglMakeCurrent;
static PFNEGLSWAPBUFFERSPROC eglSwapBuffers;static PFNEGLSWAPINTERVALPROC eglSwapInterval;static PFNEGLGETERRORPROC eglGetError;
static PFNEGLCREATEPBUFFERFROMCLIENTBUFFERPROC eglCreatePbufferFromClientBuffer;static PFNEGLBINDTEXIMAGEPROC eglBindTexImage;
static PFNEGLRELEASETEXIMAGEPROC eglReleaseTexImage;static PFNEGLDESTROYSURFACEPROC eglDestroySurface;

int halo_graphics_prefer_metal(void) {
    NSString *value=[NSUserDefaults.standardUserDefaults stringForKey:@"HaloRenderer"];
    return ![value isEqualToString:@"opengl"];
}
void halo_graphics_set_preference(int value) {
    [NSUserDefaults.standardUserDefaults setObject:value?@"metal":@"opengl" forKey:@"HaloRenderer"];
}
void halo_graphics_initialize(void) {
    metal=halo_graphics_prefer_metal();
    NSString *test=NSProcessInfo.processInfo.environment[@"HALO_IOS_TEST_RENDERER"];
    if([test isEqualToString:@"metal"] || [test isEqualToString:@"opengl"])metal=[test isEqualToString:@"metal"];
    host_logf(HOST_LOG_INFO,"Renderer: %s",metal?"Metal (ANGLE)":"OpenGL ES (Apple)");
}
int halo_graphics_metal(void) {return metal;}
void *halo_graphics_proc(const char *name) {
    void *result=metal?(void *)eglGetProcAddress(name):(void *)SDL_GL_GetProcAddress(name);
    if(!result)host_fatal("Graphics entry point unavailable: %s",name);
    return result;
}
int halo_graphics_has_extension(const char *name) {
    if(!metal)return SDL_GL_ExtensionSupported(name);
    const GLubyte *(*getStringi)(GLenum,GLuint)=halo_graphics_proc("glGetStringi");
    void (*getInteger)(GLenum,GLint *)=halo_graphics_proc("glGetIntegerv");
    GLint count=0;getInteger(GL_NUM_EXTENSIONS,&count);
    for(GLint i=0;i<count;i++)if(!strcmp((const char *)getStringi(GL_EXTENSIONS,i),name))return 1;
    return 0;
}
void *halo_graphics_create(SDL_Window *window) {
    NSString *frameworks=NSBundle.mainBundle.privateFrameworksPath;
    glesLibrary=dlopen([[frameworks stringByAppendingPathComponent:@"libGLESv2.framework/libGLESv2"] fileSystemRepresentation],RTLD_NOW|RTLD_LOCAL);
    eglLibrary=dlopen([[frameworks stringByAppendingPathComponent:@"libEGL.framework/libEGL"] fileSystemRepresentation],RTLD_NOW|RTLD_LOCAL);
    if(!glesLibrary || !eglLibrary)host_fatal("Could not load bundled ANGLE: %s",dlerror());
#define LOAD(name) name=dlsym(eglLibrary,#name);if(!name)host_fatal("Missing ANGLE symbol: %s",#name)
    LOAD(eglGetProcAddress);LOAD(eglInitialize);LOAD(eglChooseConfig);LOAD(eglCreateContext);
    LOAD(eglCreateWindowSurface);LOAD(eglMakeCurrent);LOAD(eglSwapBuffers);LOAD(eglSwapInterval);LOAD(eglGetError);
    LOAD(eglCreatePbufferFromClientBuffer);LOAD(eglBindTexImage);LOAD(eglReleaseTexImage);LOAD(eglDestroySurface);
#undef LOAD
    PFNEGLGETPLATFORMDISPLAYEXTPROC getDisplay=(void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLint attributes[]={EGL_PLATFORM_ANGLE_TYPE_ANGLE,EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,EGL_NONE};
    display=getDisplay?getDisplay(EGL_PLATFORM_ANGLE_ANGLE,EGL_DEFAULT_DISPLAY,attributes):EGL_NO_DISPLAY;
    if(!display || !eglInitialize(display,NULL,NULL))host_fatal("Could not initialize ANGLE Metal: EGL %x",eglGetError());
    EGLint configAttributes[]={EGL_SURFACE_TYPE,EGL_WINDOW_BIT|EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_DEPTH_SIZE,0,EGL_STENCIL_SIZE,0,EGL_NONE};
    EGLint count=0;
    if(!eglChooseConfig(display,configAttributes,&config,1,&count) || !count)host_fatal("ANGLE Metal has no ES3 configuration");
    EGLint contextAttributes[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};
    context=eglCreateContext(display,config,EGL_NO_CONTEXT,contextAttributes);
    metalView=SDL_Metal_CreateView(window);
    CAMetalLayer *layer=(__bridge CAMetalLayer *)SDL_Metal_GetLayer(metalView);
    windowSurface=eglCreateWindowSurface(display,config,(__bridge EGLNativeWindowType)layer,NULL);
    if(!context || !windowSurface || !halo_graphics_make_current())host_fatal("Could not create ANGLE Metal context: EGL %x",eglGetError());
    const GLubyte *(*getString)(GLenum)=halo_graphics_proc("glGetString");
    host_logf(HOST_LOG_INFO,"GPU: %s; %s",getString(GL_RENDERER),getString(GL_VERSION));
    return context;
}
int halo_graphics_make_current(void) {return eglMakeCurrent(display,windowSurface,windowSurface,context);}
int halo_graphics_swap(void) {return eglSwapBuffers(display,windowSurface);}
int halo_graphics_swap_interval(int interval) {return eglSwapInterval(display,interval);}

unsigned int halo_graphics_shared_create(void *surface,unsigned int w,unsigned int h) {
    halo_graphics_shared_destroy();
    EGLint attrs[]={EGL_WIDTH,(EGLint)w,EGL_HEIGHT,(EGLint)h,EGL_IOSURFACE_PLANE_ANGLE,0,EGL_TEXTURE_TARGET,EGL_TEXTURE_2D,EGL_TEXTURE_FORMAT,EGL_TEXTURE_RGBA,EGL_TEXTURE_TYPE_ANGLE,GL_UNSIGNED_BYTE,EGL_TEXTURE_INTERNAL_FORMAT_ANGLE,GL_BGRA_EXT,EGL_NONE};
    sharedSurface=eglCreatePbufferFromClientBuffer(display,EGL_IOSURFACE_ANGLE,surface,config,attrs);
    if(!sharedSurface)return 0;
    void (*genTextures)(GLsizei,GLuint *)=halo_graphics_proc("glGenTextures");genTextures(1,&sharedTexture);
    return halo_graphics_shared_begin()?sharedTexture:0;
}
int halo_graphics_shared_begin(void) {
    if(sharedBound)return 1;
    if(!sharedSurface)return 0;
    void (*bindTexture)(GLenum,GLuint)=halo_graphics_proc("glBindTexture");bindTexture(GL_TEXTURE_2D,sharedTexture);
    sharedBound=eglBindTexImage(display,sharedSurface,EGL_BACK_BUFFER);return sharedBound;
}
int halo_graphics_shared_end(void) {
    if(!sharedBound)return 1;
    EGLBoolean released=eglReleaseTexImage(display,sharedSurface,EGL_BACK_BUFFER);
    /* Complete both the rendering and any release-time work before another
       Metal command queue reads the IOSurface. */
    void (*finish)(void)=halo_graphics_proc("glFinish");finish();
    if(released)sharedBound=NO;
    return released;
}
void halo_graphics_shared_destroy(void) {
    halo_graphics_shared_end();
    if(sharedTexture){void (*deleteTextures)(GLsizei,const GLuint *)=halo_graphics_proc("glDeleteTextures");deleteTextures(1,&sharedTexture);sharedTexture=0;}
    if(sharedSurface){eglDestroySurface(display,sharedSurface);sharedSurface=EGL_NO_SURFACE;}
    sharedBound=NO;
}
