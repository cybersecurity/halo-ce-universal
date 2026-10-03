#include <TargetConditionals.h>
#if TARGET_OS_SIMULATOR
#include "host_debug.h"
int halo_metalfx_supported(void) {return 0;}
int halo_metalfx_enabled(void) {return 0;}
void halo_metalfx_set_enabled(int enabled) {(void)enabled;}
unsigned int host_ios_render_height(void) {return 0;}
int host_ios_metalfx_present(unsigned int t,unsigned int w,unsigned int h,unsigned int ow,unsigned int oh) {(void)t;(void)w;(void)h;(void)ow;(void)oh;return 0;}
#else
/* Optional GLES -> IOSurface -> MetalFX presentation. Both APIs share the
   input image; explicit completion fences protect ownership across APIs. */
#import <UIKit/UIKit.h>
#import <OpenGLES/ES3/gl.h>
#import <OpenGLES/ES3/glext.h>
#import <OpenGLES/EAGL.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <MetalFX/MTLFXSpatialScaler.h>
#import <QuartzCore/CAMetalLayer.h>
#include "ios_host.h"
#include "host_debug.h"

static BOOL enabled;
static id<MTLDevice> device;
static id<MTLCommandQueue> queue;
static id<MTLCommandBuffer> pending;
static id<MTLFXSpatialScaler> scaler;
static CVOpenGLESTextureCacheRef glCache;
static CVMetalTextureCacheRef metalCache;
static CVPixelBufferRef pixels;
static CVOpenGLESTextureRef glImage;
static CVMetalTextureRef metalImage;
static id<MTLTexture> output;
static UIView *surface;
static CAMetalLayer *layer;
static GLuint readFBO,writeFBO;
static unsigned inputW,inputH,outputW,outputH;

int halo_metalfx_supported(void) {
    if(!device)device=MTLCreateSystemDefaultDevice();
    return device && [MTLFXSpatialScalerDescriptor supportsDevice:device];
}
int halo_metalfx_enabled(void) {return enabled;}
void halo_metalfx_set_enabled(int value) {
    enabled=value && halo_metalfx_supported();
    if(!enabled)surface.hidden=YES;
    host_logf(HOST_LOG_INFO,"MetalFX spatial upscaling %s",enabled?"enabled":"disabled");
}
unsigned int host_ios_render_height(void) {return enabled?720:0;}
static void releaseImages(void) {
    [pending waitUntilCompleted];pending=nil;
    if(glImage){CFRelease(glImage);glImage=NULL;}
    if(metalImage){CFRelease(metalImage);metalImage=NULL;}
    if(pixels){CFRelease(pixels);pixels=NULL;}
    if(glCache)CVOpenGLESTextureCacheFlush(glCache,0);
    if(metalCache)CVMetalTextureCacheFlush(metalCache,0);
    scaler=nil;output=nil;inputW=inputH=outputW=outputH=0;
}
static BOOL configure(unsigned w,unsigned h,unsigned ow,unsigned oh) {
    releaseImages();
    if(!queue)queue=[device newCommandQueue];
    if(!queue)return NO;
    if(!glCache && CVOpenGLESTextureCacheCreate(NULL,NULL,EAGLContext.currentContext,NULL,&glCache)!=kCVReturnSuccess)return NO;
    if(!metalCache && CVMetalTextureCacheCreate(NULL,NULL,device,NULL,&metalCache)!=kCVReturnSuccess)return NO;
    NSDictionary *attributes=@{(id)kCVPixelBufferIOSurfacePropertiesKey:@{},(id)kCVPixelBufferMetalCompatibilityKey:@YES,(id)kCVPixelBufferOpenGLESCompatibilityKey:@YES};
    if(CVPixelBufferCreate(NULL,w,h,kCVPixelFormatType_32BGRA,(__bridge CFDictionaryRef)attributes,&pixels)!=kCVReturnSuccess)return NO;
    if(CVOpenGLESTextureCacheCreateTextureFromImage(NULL,glCache,pixels,NULL,GL_TEXTURE_2D,GL_RGBA,w,h,GL_BGRA_EXT,GL_UNSIGNED_BYTE,0,&glImage)!=kCVReturnSuccess)return NO;
    MTLFXSpatialScalerDescriptor *desc=[MTLFXSpatialScalerDescriptor new];
    desc.inputWidth=w;desc.inputHeight=h;desc.outputWidth=ow;desc.outputHeight=oh;
    desc.colorTextureFormat=desc.outputTextureFormat=MTLPixelFormatBGRA8Unorm;
    desc.colorProcessingMode=MTLFXSpatialScalerColorProcessingModePerceptual;
    scaler=[desc newSpatialScalerWithDevice:device];if(!scaler)return NO;
    NSDictionary *textureAttributes=@{(id)kCVMetalTextureUsage:@(scaler.colorTextureUsage)};
    if(CVMetalTextureCacheCreateTextureFromImage(NULL,metalCache,pixels,(__bridge CFDictionaryRef)textureAttributes,MTLPixelFormatBGRA8Unorm,w,h,0,&metalImage)!=kCVReturnSuccess)return NO;
    MTLTextureDescriptor *texture=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:ow height:oh mipmapped:NO];
    texture.storageMode=MTLStorageModePrivate;texture.usage=scaler.outputTextureUsage;
    output=[device newTextureWithDescriptor:texture];if(!output)return NO;
    scaler.colorTexture=CVMetalTextureGetTexture(metalImage);scaler.outputTexture=output;
    scaler.inputContentWidth=w;scaler.inputContentHeight=h;
    if(!readFBO)glGenFramebuffers(1,&readFBO);
    if(!writeFBO)glGenFramebuffers(1,&writeFBO);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,writeFBO);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,CVOpenGLESTextureGetName(glImage),0);
    if(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)return NO;
    inputW=w;inputH=h;outputW=ow;outputH=oh;
    host_logf(HOST_LOG_INFO,"MetalFX configured %ux%u -> %ux%u",w,h,ow,oh);
    return YES;
}
int host_ios_metalfx_present(unsigned int texture,unsigned int w,unsigned int h,unsigned int ow,unsigned int oh) {
    if(!enabled || !w || !h || !ow || !oh)return 0;
    @autoreleasepool {
        GLint oldRead,oldWrite,oldTexture;glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&oldRead);glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&oldWrite);glGetIntegerv(GL_TEXTURE_BINDING_2D,&oldTexture);
        BOOL scissor=glIsEnabled(GL_SCISSOR_TEST),success=YES;
        [pending waitUntilCompleted];
        if(pending.status==MTLCommandBufferStatusError){host_logf(HOST_LOG_ERROR,"MetalFX: %s",pending.error.localizedDescription.UTF8String);success=NO;}
        pending=nil;
        if(success && (w!=inputW || h!=inputH || ow!=outputW || oh!=outputH))success=configure(w,h,ow,oh);
        if(success && !surface) {
            UIWindow *window=nil;
            for(UIScene *scene in UIApplication.sharedApplication.connectedScenes)if([scene isKindOfClass:UIWindowScene.class])for(UIWindow *candidate in ((UIWindowScene*)scene).windows)if(candidate.isKeyWindow)window=candidate;
            UIView *root=window.rootViewController.view;
            if(root){surface=[[UIView alloc]initWithFrame:root.bounds];surface.userInteractionEnabled=NO;surface.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;layer=[CAMetalLayer layer];layer.device=device;layer.pixelFormat=MTLPixelFormatBGRA8Unorm;layer.framebufferOnly=NO;[surface.layer addSublayer:layer];[root insertSubview:surface atIndex:0];}
            else success=NO;
        }
        if(success) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER,readFBO);glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER,writeFBO);glDisable(GL_SCISSOR_TEST);
            /* Guest row zero already denotes the top, as Metal expects. */
            glBlitFramebuffer(0,0,w,h,0,0,w,h,GL_COLOR_BUFFER_BIT,GL_NEAREST);
            glFinish();
            layer.frame=surface.bounds;layer.contentsScale=surface.window.screen.scale;
            CGSize size=CGSizeMake(round(surface.bounds.size.width*layer.contentsScale),round(surface.bounds.size.height*layer.contentsScale));
            if(!CGSizeEqualToSize(layer.drawableSize,size))layer.drawableSize=size;
            id<CAMetalDrawable> drawable=[layer nextDrawable];
            if(drawable && ow<=drawable.texture.width && oh<=drawable.texture.height) {
                id<MTLCommandBuffer> commands=[queue commandBuffer];
                [scaler encodeToCommandBuffer:commands];
                MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture=drawable.texture;pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;pass.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,1);
                [[commands renderCommandEncoderWithDescriptor:pass]endEncoding];
                id<MTLBlitCommandEncoder> blit=[commands blitCommandEncoder];
                [blit copyFromTexture:output sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(ow,oh,1) toTexture:drawable.texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake((drawable.texture.width-ow)/2,(drawable.texture.height-oh)/2,0)];
                [blit endEncoding];[commands presentDrawable:drawable];[commands commit];pending=commands;surface.hidden=NO;
            } else {surface.hidden=YES;success=NO;}
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER,oldRead);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,oldWrite);glBindTexture(GL_TEXTURE_2D,oldTexture);
        if(scissor)glEnable(GL_SCISSOR_TEST);else glDisable(GL_SCISSOR_TEST);
        if(!success){halo_metalfx_set_enabled(0);releaseImages();}
        return success;
    }
}

#endif
