/* ANGLE owns the opaque keys and validates compiled blobs. The application
   provides a bounded private disk cache, scoped to the GPU, OS and ANGLE build.
   Writes run off the rendering thread; eviction/corruption is just a miss. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CommonCrypto/CommonDigest.h>
#include <string.h>
#include "host_shader_cache.h"
#include "ios_host.h"

static NSString *cacheDirectory;
static NSCache<NSString *, NSData *> *memoryCache;
static dispatch_queue_t writer;
static NSLock *pendingLock;
static NSUInteger pendingBytes;
static const NSUInteger maximumBlob=16*1024*1024, maximumDisk=64*1024*1024;
static BOOL loggedHit,loggedStore;

static NSString *digest(NSData *data) {
    unsigned char bytes[CC_SHA256_DIGEST_LENGTH];CC_SHA256(data.bytes,(CC_LONG)data.length,bytes);
    NSMutableString *result=[NSMutableString new];
    for(unsigned i=0;i<sizeof(bytes);i++)[result appendFormat:@"%02x",bytes[i]];
    return result;
}
static NSString *keyName(const void *key,EGLsizeiANDROID size) {
    if(!key || size<=0 || size>65536)return nil;
    return digest([NSData dataWithBytes:key length:(NSUInteger)size]);
}
static void trimDisk(void) {
    NSFileManager *files=NSFileManager.defaultManager;
    NSArray *urls=[files contentsOfDirectoryAtURL:[NSURL fileURLWithPath:cacheDirectory]
        includingPropertiesForKeys:@[NSURLFileSizeKey,NSURLContentModificationDateKey] options:0 error:nil];
    NSMutableArray *entries=[NSMutableArray new];NSUInteger total=0;
    for(NSURL *url in urls) {
        if(![url.pathExtension isEqualToString:@"blob"])continue;
        NSDictionary *values=[url resourceValuesForKeys:@[NSURLFileSizeKey,NSURLContentModificationDateKey] error:nil];
        NSUInteger size=[values[NSURLFileSizeKey] unsignedIntegerValue];total+=size;
        [entries addObject:@{@"url":url,@"size":@(size),@"date":values[NSURLContentModificationDateKey]?:NSDate.distantPast}];
    }
    [entries sortUsingDescriptors:@[[NSSortDescriptor sortDescriptorWithKey:@"date" ascending:YES]]];
    for(NSDictionary *entry in entries) {
        if(total<=maximumDisk)break;
        [files removeItemAtURL:entry[@"url"] error:nil];total-=[entry[@"size"] unsignedIntegerValue];
    }
}
static void storeBlob(const void *key,EGLsizeiANDROID keySize,const void *value,EGLsizeiANDROID valueSize) {
    @autoreleasepool {
        NSString *name=keyName(key,keySize);
        if(!name || !value || valueSize<=0 || valueSize>maximumBlob)return;
        NSData *bytes=[NSData dataWithBytes:value length:(NSUInteger)valueSize];
        [memoryCache setObject:bytes forKey:name cost:bytes.length];
        [pendingLock lock];
        BOOL queued=pendingBytes+bytes.length<=maximumDisk;
        if(queued)pendingBytes+=bytes.length;
        [pendingLock unlock];
        if(!queued)return;
        dispatch_async(writer,^{@autoreleasepool {
            NSString *path=[cacheDirectory stringByAppendingPathComponent:[name stringByAppendingPathExtension:@"blob"]];
            BOOL ok=[bytes writeToFile:path options:NSDataWritingAtomic error:nil];
            if(ok && !loggedStore){loggedStore=YES;host_logf(HOST_LOG_INFO,"ANGLE shader cache: stored compiled blob");}
            trimDisk();
            [pendingLock lock];pendingBytes-=bytes.length;[pendingLock unlock];
        }});
    }
}
static EGLsizeiANDROID getBlob(const void *key,EGLsizeiANDROID keySize,void *value,EGLsizeiANDROID valueSize) {
    @autoreleasepool {
        NSString *name=keyName(key,keySize);if(!name)return 0;
        NSData *bytes=[memoryCache objectForKey:name];BOOL diskHit=NO;
        if(!bytes) {
            NSString *path=[cacheDirectory stringByAppendingPathComponent:[name stringByAppendingPathExtension:@"blob"]];
            unsigned long long size=[[NSFileManager.defaultManager attributesOfItemAtPath:path error:nil] fileSize];
            if(size==0 || size>maximumBlob)return 0;
            bytes=[NSData dataWithContentsOfFile:path options:0 error:nil];
            if(!bytes || bytes.length>maximumBlob)return 0;
            [memoryCache setObject:bytes forKey:name cost:bytes.length];diskHit=YES;
        }
        if(value && valueSize>=0 && (NSUInteger)valueSize>=bytes.length) {
            memcpy(value,bytes.bytes,bytes.length);

        }
        [pendingLock lock];BOOL log=diskHit && !loggedHit;if(diskHit)loggedHit=YES;[pendingLock unlock];
        if(log)host_logf(HOST_LOG_INFO,"ANGLE shader cache: loaded compiled blob from disk");
        return (EGLsizeiANDROID)bytes.length;
    }
}
void halo_shader_cache_install(EGLDisplay display,PFNEGLGETPROCADDRESSPROC get_proc) {
    PFNEGLQUERYSTRINGPROC query=(void *)get_proc("eglQueryString");
    const char *extensions=query?query(display,EGL_EXTENSIONS):NULL;
    PFNEGLSETBLOBCACHEFUNCSANDROIDPROC install=(void *)get_proc("eglSetBlobCacheFuncsANDROID");
    if(!extensions || !strstr(extensions,"EGL_ANDROID_blob_cache") || !install) {
        host_logf(HOST_LOG_INFO,"ANGLE shader cache: extension unavailable");return;
    }
    NSString *root=[NSSearchPathForDirectoriesInDomains(NSCachesDirectory,NSUserDomainMask,YES).firstObject
        stringByAppendingPathComponent:@"HaloShaderCache-v1"];
    NSString *angle=NSBundle.mainBundle.privateFrameworksPath;
    NSDictionary *version=[NSDictionary dictionaryWithContentsOfFile:[angle stringByAppendingPathComponent:@"libGLESv2.framework/Info.plist"]];
    NSString *identity=[NSString stringWithFormat:@"%@|%@|%@|%@",MTLCreateSystemDefaultDevice().name,
        NSProcessInfo.processInfo.operatingSystemVersionString,version[@"CFBundleVersion"],version[@"CFBundleShortVersionString"]];
    cacheDirectory=[root stringByAppendingPathComponent:digest([identity dataUsingEncoding:NSUTF8StringEncoding])];
    if(![NSFileManager.defaultManager createDirectoryAtPath:cacheDirectory withIntermediateDirectories:YES attributes:nil error:nil])return;
    memoryCache=[NSCache new];memoryCache.totalCostLimit=maximumDisk;
    pendingLock=[NSLock new];writer=dispatch_queue_create("org.haloce.shader-cache",DISPATCH_QUEUE_SERIAL);
    dispatch_async(writer,^{trimDisk();});
    install(display,storeBlob,getBlob);
    host_logf(HOST_LOG_INFO,"ANGLE shader cache: enabled (64 MiB disk budget)");
}
