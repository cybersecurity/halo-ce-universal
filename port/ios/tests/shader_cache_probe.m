#import <Foundation/Foundation.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#import "../host/host_shader_cache.m"
void host_logf(int priority,const char *format,...) {(void)priority;(void)format;}
int main(void) {@autoreleasepool {
    cacheDirectory=[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    assert([NSFileManager.defaultManager createDirectoryAtPath:cacheDirectory withIntermediateDirectories:YES attributes:nil error:nil]);
    memoryCache=[NSCache new];pendingLock=[NSLock new];writer=dispatch_queue_create("cache-probe",DISPATCH_QUEUE_SERIAL);
    const char key[]="key", bytes[]="compiled program";
    storeBlob(key,sizeof(key),bytes,sizeof(bytes));dispatch_sync(writer,^{});
    [memoryCache removeAllObjects];
    assert(getBlob(key,sizeof(key),NULL,0)==sizeof(bytes));
    char output[sizeof(bytes)];memset(output,0x5a,sizeof(output));
    assert(getBlob(key,sizeof(key),output,2)==sizeof(bytes));assert(output[0]==0x5a);
    assert(getBlob(key,sizeof(key),output,-1)==sizeof(bytes));assert(output[0]==0x5a);
    assert(getBlob(key,sizeof(key),output,sizeof(output))==sizeof(bytes));assert(!memcmp(output,bytes,sizeof(bytes)));
    assert(getBlob(NULL,0,output,sizeof(output))==0);
    assert(getBlob("missing",7,output,sizeof(output))==0);
    // Concurrent writers/readers must never return a partially written payload.
    dispatch_apply(32,dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^(size_t n){
        char payload[256];memset(payload,(int)n,sizeof(payload));
        storeBlob(&n,sizeof(n),payload,sizeof(payload));
        char readback[256];assert(getBlob(&n,sizeof(n),readback,sizeof(readback))==sizeof(payload));
        assert(!memcmp(readback,payload,sizeof(payload)));
    });
    dispatch_sync(writer,^{});
    // Sparse files exercise disk eviction without allocating 80 MiB of data.
    for(int i=0;i<5;i++) {
        NSString *path=[cacheDirectory stringByAppendingPathComponent:[NSString stringWithFormat:@"budget-%d.blob",i]];
        int fd=open(path.fileSystemRepresentation,O_CREAT|O_RDWR,0600);assert(fd>=0);assert(!ftruncate(fd,16*1024*1024));close(fd);
    }
    trimDisk();NSUInteger total=0;
    for(NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:cacheDirectory error:nil])
        total+=[[NSFileManager.defaultManager attributesOfItemAtPath:[cacheDirectory stringByAppendingPathComponent:name] error:nil] fileSize];
    assert(total<=maximumDisk);
    assert([NSFileManager.defaultManager removeItemAtPath:cacheDirectory error:nil]);
    puts("PASS: shader cache disk reload, buffer bounds, concurrent writes and 64 MiB eviction");
    return 0;
}}
