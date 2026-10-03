#import "game_store.h"
#import <CommonCrypto/CommonDigest.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static NSString *digest_file(NSString *path, FILE *copy, xiso_progress_proc progress,
                             void *context, const char *phase, NSString **failure) {
    FILE *input=fopen(path.fileSystemRepresentation,"rb");
    if(!input){*failure=@"Could not read the disc image.";return nil;}
    struct stat before,after;
    if(fstat(fileno(input),&before) || !S_ISREG(before.st_mode) || before.st_size<=0) {
        fclose(input);*failure=@"Choose a non-empty disc image file.";return nil;
    }
    CC_SHA256_CTX hash;CC_SHA256_Init(&hash);
    unsigned char *buffer=malloc(1024*1024);uint64_t done=0;BOOL ok=buffer!=NULL;
    while(ok) {
        if(progress && !progress(context,phase,done,before.st_size)){*failure=@"Import cancelled.";ok=NO;break;}
        size_t count=fread(buffer,1,1024*1024,input);
        if(!count){if(ferror(input))ok=NO;break;}
        if(copy && fwrite(buffer,1,count,copy)!=count){ok=NO;break;}
        CC_SHA256_Update(&hash,buffer,(CC_LONG)count);done+=count;
    }
    if(fstat(fileno(input),&after) || before.st_size!=after.st_size || done!=(uint64_t)before.st_size ||
       before.st_mtimespec.tv_sec!=after.st_mtimespec.tv_sec || before.st_mtimespec.tv_nsec!=after.st_mtimespec.tv_nsec)ok=NO;
    free(buffer);fclose(input);
    if(!ok){if(!*failure)*failure=@"Could not copy the complete image. Check available storage and try again.";return nil;}
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256_Final(digest,&hash);
    NSMutableString *result=[NSMutableString new];for(int i=0;i<CC_SHA256_DIGEST_LENGTH;i++)[result appendFormat:@"%02x",digest[i]];
    return result;
}

NSString *halo_game_store_current(NSString *root) {
    NSDictionary *pointer=[NSDictionary dictionaryWithContentsOfFile:[root stringByAppendingPathComponent:@"game-store.plist"]];
    NSString *name=pointer[@"generation"];
    /* Only recover directories explicitly marked as our unpublished imports.
       Never delete an old committed generation or an unrecognized folder. */
    NSFileManager *files=NSFileManager.defaultManager;
    for(NSString *candidate in [files contentsOfDirectoryAtPath:root error:nil]) {
        if([candidate isEqual:name] || ![candidate hasPrefix:@"game-"] ||
           ![[NSUUID alloc]initWithUUIDString:[candidate substringFromIndex:5]])continue;
        NSString *pending=[root stringByAppendingPathComponent:candidate];
        NSString *marker=[NSString stringWithContentsOfFile:[pending stringByAppendingPathComponent:@"import.pending"] encoding:NSUTF8StringEncoding error:nil];
        if([marker isEqualToString:@"Halo private image transaction v1"])[files removeItemAtPath:pending error:nil];
    }
    if(![name isKindOfClass:NSString.class] || ![name hasPrefix:@"game-"] || ![[NSUUID alloc]initWithUUIDString:[name substringFromIndex:5]])return nil;
    NSString *directory=[root stringByAppendingPathComponent:name];
    NSDictionary *manifest=[NSDictionary dictionaryWithContentsOfFile:[directory stringByAppendingPathComponent:@"integrity.plist"]];
    NSDictionary *attrs=[NSFileManager.defaultManager attributesOfItemAtPath:[directory stringByAppendingPathComponent:@"disc.iso"] error:nil];
    if(![manifest[@"sha256"] isKindOfClass:NSString.class] ||
       ![manifest[@"size"] isKindOfClass:NSNumber.class] ||
       [manifest[@"version"] intValue]!=1 || [manifest[@"sha256"] length]!=64 ||
       ![attrs[NSFileType] isEqual:NSFileTypeRegular] || ![attrs[NSFileSize] isEqual:manifest[@"size"]])return nil;
    if(!xiso_maps_ready([[directory stringByAppendingPathComponent:@"maps"] fileSystemRepresentation],NULL,0))return nil;
    return directory;
}

BOOL halo_game_store_import(NSURL *image, NSString *root, xiso_progress_proc progress,
                           void *context, NSString **failure) {
    NSString *reason=nil;NSFileManager *files=NSFileManager.defaultManager;
    NSString *name=[@"game-" stringByAppendingString:NSUUID.UUID.UUIDString];
    NSString *directory=[root stringByAppendingPathComponent:name];
    if(![files createDirectoryAtPath:directory withIntermediateDirectories:NO attributes:nil error:nil]) {
        if(failure)*failure=@"Could not create private game storage.";return NO;
    }
    if(![@"Halo private image transaction v1" writeToFile:[directory stringByAppendingPathComponent:@"import.pending"] atomically:YES encoding:NSUTF8StringEncoding error:nil]){[files removeItemAtPath:directory error:nil];if(failure)*failure=@"Could not initialize the import transaction.";return NO;}
    [[NSURL fileURLWithPath:directory]setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
    NSString *copyPath=[directory stringByAppendingPathComponent:@"disc.iso"];
    FILE *output=fopen(copyPath.fileSystemRepresentation,"wbx");
    NSString *sourceHash=output?digest_file(image.path,output,progress,context,"Copying disc image",&reason):nil;
    if(output){if(fflush(output) || fsync(fileno(output)))sourceHash=nil;if(fclose(output))sourceHash=nil;}
    NSString *copyHash=sourceHash?digest_file(copyPath,NULL,progress,context,"Verifying private copy",&reason):nil;
    BOOL ok=copyHash && [copyHash isEqual:sourceHash];
    if(ok){char error[1024]={0};ok=xiso_extract_maps(copyPath.fileSystemRepresentation,directory.fileSystemRepresentation,progress,context,error,sizeof(error));if(!ok)reason=[NSString stringWithUTF8String:error];}
    if(ok){
        NSDictionary *attrs=[files attributesOfItemAtPath:copyPath error:nil];
        NSDictionary *manifest=@{@"version":@1,@"sha256":copyHash,@"size":attrs[NSFileSize],@"originalName":image.lastPathComponent};
        ok=[manifest writeToFile:[directory stringByAppendingPathComponent:@"integrity.plist"] atomically:YES];
        /* Publish only after both the retained image and validated maps exist.
           A crash leaves either the old generation or the complete new one. */
        if(ok)ok=[@{@"generation":name} writeToFile:[root stringByAppendingPathComponent:@"game-store.plist"] atomically:YES];
    }
    if(ok)[files removeItemAtPath:[directory stringByAppendingPathComponent:@"import.pending"] error:nil];
    if(!ok){[files removeItemAtPath:directory error:nil];if(failure)*failure=reason?:@"Could not verify or save the private disc image. Check available storage.";}
    return ok;
}
